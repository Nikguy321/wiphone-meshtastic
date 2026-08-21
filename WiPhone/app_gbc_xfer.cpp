/*
 * app_gbc_xfer.cpp — the file-transfer web server (see app_gbc_xfer.h).
 *
 * Robustness notes (learned on hardware):
 * - WebServer::handleClient() processes an entire POST synchronously in the
 *   main loop, so a big upload keeps the loop busy for many seconds. The idle
 *   task starves and the task watchdog reboots the phone. The upload/download
 *   handlers therefore delay(1) periodically to feed it.
 * - The upload page sends files ONE PER REQUEST, sequentially. One giant
 *   multipart POST with several files maximized the blocking window (reboots)
 *   and gave no per-file feedback (browser looked frozen).
 * - The server does NOT filter by extension: `accept` is only a hint to the
 *   browser's file picker. That is deliberate and has already earned its keep —
 *   it is how a 5 MB EPUB reached a phone whose firmware only knew about ROMs.
 */

#include "app_gbc_xfer.h"
#include "Arduino.h"
#include "WiFi.h"
#include "WebServer.h"
#include "ESPmDNS.h"
#include "HTTPClient.h"
#include "WiFiClientSecure.h"
#include "SD.h"
#include "GUI.h"

static WebServer*   s_server = NULL;
static File         s_uploadFile;
static volatile int s_filesAdded = 0;   // uploads + downloads this session (for status)
static bool         s_on = false;
static bool         s_usingAP = false;  // true if we had to bring up our own hotspot

/* ── Keeping the screen alive while the server is up ────────────────────────────────
 *
 * The phone dims at 20 s and sleeps at 30 s, and dragging a 5 MB track in from a
 * computer takes longer than that. Worse, the screen going dark mid-upload looks
 * exactly like a crash, so people reach for the keypad — and Back stops the server.
 *
 * Held HERE rather than in each app's transfer screen, because there are three
 * uploaders and this module is the one place that knows when a server is actually
 * running. Restored by xferStop(), which every app already calls from its destructor,
 * so an app dying with the screen up cannot leave the phone with a ten-minute timeout.
 *
 * ⚠ The timeouts are raised, not disabled. An upload screen left open on a forgotten
 * phone should still eventually sleep. */
extern GUI gui;

static bool     s_heldAwake = false;
static uint32_t s_savedDimMs = 0;
static uint32_t s_savedSleepMs = 0;

static void xferHoldAwake(bool hold) {
  if (hold == s_heldAwake) {
    return;
  }
  ControlState& cs = gui.state;
  if (hold) {
    s_savedDimMs = cs.dimAfterMs;
    s_savedSleepMs = cs.sleepAfterMs;
    if (cs.dimAfterMs < 300000) {
      cs.dimAfterMs = 300000;      // 5 minutes
    }
    if (cs.sleepAfterMs < 600000) {
      cs.sleepAfterMs = 600000;    // 10 minutes
    }
  } else {
    cs.dimAfterMs = s_savedDimMs;
    cs.sleepAfterMs = s_savedSleepMs;
  }
  // Queued events carry the OLD deadline; re-arm them against the new one.
  cs.unscheduleEvent(SCREEN_DIM_EVENT);
  cs.unscheduleEvent(SCREEN_SLEEP_EVENT);
  uint32_t now = millis();
  if (cs.doDimming()) {
    cs.scheduleEvent(SCREEN_DIM_EVENT, now + cs.dimAfterMs);
  }
  if (cs.doSleeping()) {
    cs.scheduleEvent(SCREEN_SLEEP_EVENT, now + cs.sleepAfterMs);
  }
  s_heldAwake = hold;
}
static char         s_addr[40] = {0};   // shown address (IP of STA or AP)
/* Breaker state is per-SERVER-SESSION: xferStart() clears it, so a fresh Start
 * (or `up on`) always begins live — the first cut kept it in a function-local
 * static, and one trip outlived every restart. */
static bool         s_breakerPaused = false;
static uint32_t     s_breakerFlipMs = 0;
static uint32_t     s_breakerWaitMs = 90000;   // timed-resume fallback; doubles on quick re-trip
static uint32_t     s_lastResumeMs  = 0;

static void xferServerUp();     // create + register + begin (also the breaker's resume)
static void xferServerDown();   // stop + delete + MDNS.end (also the breaker's pause)

static const XferConfig ROM_CFG = {
  "/roms", "Add Game Boy ROMs", ".gb,.gbc", "ROMs", "download.gbc", "WiPhone-ROMs"
};
/* The books uploader, startable from the serial console (`up on books`) — the
 * bench needs to feed the reader without hands on the phone. Mirrors
 * app_books.cpp's BOOKS_XFER_CFG (same dir, filter, AP name). */
static const XferConfig SERIAL_BOOKS_CFG = {
  "/books", "Add books", ".epub,.txt", "books", "download.epub", "WiPhone-Books"
};
const XferConfig* xferBooksConfig() { return &SERIAL_BOOKS_CFG; }
static const XferConfig* s_cfg = &ROM_CFG;

void gbcXferHandleClient() {
  if (!s_on) {
    return;
  }
  if (!s_server && !s_breakerPaused) {
    return;                 // not started at all
  }
  /* ── LOW-HEAP CIRCUIT BREAKER ──────────────────────────────────────────────
   * Measured 2026-08-20: back-to-back uploads ground the largest internal block
   * from 13 KB to 3.3 KB, at which point lwIP could not allocate for NEW
   * connections — first the listener died (instant connection-refused), then
   * the whole stack went mute (not even ping answered). The browser reads that
   * as "the page locked up", and 3 KB is the altitude where this firmware's
   * historic operator-new abort lives. The server now pauses ITSELF while
   * memory is tight instead of dragging the network stack (and then the whole
   * phone) down with it: the listener closes, nothing else is lost, and it
   * comes back on its own when heap recovers. Hysteresis so it cannot flap. */
  /* Trip threshold from the 2026-08-20 measurements: sequential uploads ran
   * HAPPILY at 7-8 KB largest; the listener only died below ~5 KB. Trip at 6 KB.
   *
   * ⚠ THE RESUME MUST NOT DEPEND ON A HEAP NUMBER ALONE. Two earlier cuts of
   * this breaker both died of the same catch-22 in the field, the SAME DAY they
   * were written: a paused-but-allocated server pins the largest block below
   * whatever resume threshold was chosen, so one trip = paused forever = "the
   * upload page works once per boot" (Nick, from the kitchen). So the pause now
   * FREES the server outright (delete + MDNS.end — that is what lets the heap
   * actually recover), and resume fires on heap recovery OR a timer, whichever
   * comes first — a stranded pause is structurally impossible. A resume that
   * re-trips within a minute doubles the wait, up to 10 min, so a genuinely
   * starved phone breathes instead of flapping. */
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t now = millis();
  if (!s_breakerPaused) {
    /* Mid-upload, backpressure (handleUpload) is the governor — pausing here
     * would abort the very transfer that is about to release the pressure, so
     * the trip defers to it unless memory is truly catastrophic. Between
     * requests the 6 KB line stands. */
    const size_t tripAt = s_uploadFile ? 3072 : 6144;
    if (largest < tripAt && (uint32_t)(now - s_breakerFlipMs) > 3000) {
      s_breakerPaused = true;
      s_breakerFlipMs = now;
      if (s_lastResumeMs && (uint32_t)(now - s_lastResumeMs) < 60000) {
        s_breakerWaitMs = s_breakerWaitMs < 300000 ? s_breakerWaitMs * 2 : 600000;
      }
      xferServerDown();
      log_e("XFER: PAUSED (largest %u < 6K) - server freed; back on recovery or %lus",
            (unsigned)largest, (unsigned long)(s_breakerWaitMs / 1000));
    } else {
      if (s_lastResumeMs && (uint32_t)(now - s_lastResumeMs) > 300000) {
        s_breakerWaitMs = 90000;        // 5 min of stable service earns the short fuse back
        s_lastResumeMs = 0;
      }
      s_server->handleClient();
    }
    return;
  }
  if ((largest >= 8192 || (uint32_t)(now - s_breakerFlipMs) >= s_breakerWaitMs) &&
      (uint32_t)(now - s_breakerFlipMs) > 3000) {
    s_breakerPaused = false;
    s_breakerFlipMs = now;
    s_lastResumeMs = now;
    xferServerUp();
    log_e("XFER: RESUMED (largest %u)", (unsigned)largest);
  }
}

bool        gbcXferOn()      { return s_on; }
bool        xferUsingAP()    { return s_usingAP; }
const char* xferAddr()       { return s_addr; }
const char* xferApName()     { return s_cfg->apName; }
int         xferFilesAdded() { return s_filesAdded; }

void gbcXferStart() {
  xferStart(&ROM_CFG);
}

/* The page served to the computer's browser: drag-and-drop / file-pick upload plus a
 * "download from URL" box. Kept small and dependency-free. Sent in pieces with the config's
 * words spliced in — a printf template would have to escape every '%' in the CSS, which is
 * exactly the kind of edit that breaks a page nobody re-reads. */
/* The page served to a browser. PHONE FIRST: this device is mostly fed from a phone, and
 * the first version was built around drag-and-drop, which does not exist on a touch
 * screen. What was left there was a bare <input type=file> in the middle of a dashed box,
 * easy to miss and awkward to hit.
 *
 * ⚠ NO `accept` ATTRIBUTE, deliberately. Android's file picker takes an extension list
 * like ".mp3,.wav" and commonly greys out EVERY file rather than filtering to those —
 * which looks exactly like a page that does not respond to taps. The server has never
 * filtered by extension anyway (that is what musicIsPlayable and the parsers are for), so
 * claiming to is worse than useless here.
 *
 * Sent in pieces with the config's words spliced in — a printf template would have to
 * escape every '%' in the CSS, which is the kind of edit that breaks a page nobody
 * re-reads. */
static const char PAGE_1[] =
  "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>WiPhone</title><style>"
  "body{font-family:sans-serif;max-width:520px;margin:16px auto;padding:0 16px;color:#222}"
  "h1{font-size:20px}h2{margin-top:24px;font-size:16px}"
  "#drop{border:2px dashed #888;border-radius:10px;padding:18px;text-align:center;color:#666}"
  "#drop.over{border-color:#2a7;background:#eafaf1}"
  "input[type=text]{width:100%;padding:12px;box-sizing:border-box;font-size:16px}"
  ".btn{display:block;width:100%;padding:16px;margin-top:10px;border:0;border-radius:8px;"
  "background:#2a7;color:#fff;font-size:17px;text-align:center;box-sizing:border-box}"
  ".btn.alt{background:#456}"
  "#pick{display:none}"          // the real input; the label below is what gets tapped
  "#chosen{margin-top:10px;color:#555;font-size:14px;word-break:break-all}"
  "</style></head><body>"
  "<h1>WiPhone &mdash; ";                                     // heading
static const char PAGE_2[] =
  "</h1>"
  "<form id=f method=POST action=/upload enctype=multipart/form-data>"
  "<input type=file id=pick name=rom multiple>"
  "<label class=btn for=pick>Choose files&hellip;</label>"
  "<div id=chosen>No files chosen yet.</div>"
  "<button class='btn alt' type=submit>Upload</button>"
  "<div id=drop style='margin-top:14px'>&hellip;or drag files here (on a computer)</div>"
  "</form>"
  "<h2>Or paste a download link</h2>"
  "<form method=POST action=/fetch>"
  "<input type=text name=url placeholder='https://... direct link to a file'>"
  "<button class='btn alt' type=submit>Download to phone</button></form>"
  "<p style='color:#888;margin-top:26px'>Saved to ";           // dir
static const char PAGE_3[] =
  " on the SD card.</p>"
  "<script>"
  "var d=document.getElementById('drop'),f=document.getElementById('f'),"
  "inp=document.getElementById('pick'),ch=document.getElementById('chosen');"
  "function names(fs){var a=[];for(var i=0;i<fs.length;i++)a.push(fs[i].name);return a.join(', ');}"
  "inp.addEventListener('change',function(){"
  "ch.textContent=inp.files.length?names(inp.files):'No files chosen yet.';});"
  "function sendAll(files){"
  "if(!files||!files.length){ch.textContent='No files chosen.';return;}"
  "var i=0,tries=0;"
  "function next(){"
  "if(i>=files.length){ch.textContent='Done! '+files.length+' file(s) on your phone.';return;}"
  "var file=files[i];"
  "ch.textContent='Uploading '+(i+1)+' of '+files.length+': '+file.name;"
  "var fd=new FormData();fd.append('rom',file);"
  "fetch('/upload',{method:'POST',body:fd}).then(function(r){"
  "if(!r.ok)throw 0;tries=0;i++;setTimeout(next,400);"   // breather: let the phone housekeep between files
  /* Retry rather than give up. A big file over a weak link genuinely fails part-way —
   * measured at 39 KB/s failing where 214 KB/s succeeded, same file, minutes apart — and
   * the upload restarts cleanly because the handler truncates the file at START. Asking
   * a person to notice and re-tap is worse than doing it for them. */
  "}).catch(function(){"
  "tries++;"
  "if(tries<4){ch.textContent='Retrying '+file.name+' ('+tries+')\u2026';setTimeout(next,1500);}"
  "else{ch.textContent='Gave up on '+file.name+' \u2014 move closer to the router and retry.';}"
  "});"
  "}"
  "next();"
  "}"
  "['dragenter','dragover'].forEach(function(e){d.addEventListener(e,function(ev){ev.preventDefault();d.classList.add('over')})});"
  "['dragleave','drop'].forEach(function(e){d.addEventListener(e,function(ev){ev.preventDefault();d.classList.remove('over')})});"
  "d.addEventListener('drop',function(ev){sendAll(ev.dataTransfer.files)});"
  "f.addEventListener('submit',function(ev){ev.preventDefault();sendAll(inp.files)});"
  "</script></body></html>";

static void handleRoot() {
  /* ⚠ Connection: close on EVERY response, and it is not cosmetic.
   *
   * This server is single-client and pumped once per main-loop pass. A browser that keeps
   * its socket open — which phone browsers do by default — occupies the one slot, and
   * every later request just hangs. That is the "load it once, then it never loads
   * again" this page used to do. Closing after each response gives the slot straight
   * back. */
  s_server->sendHeader("Connection", "close");
  s_server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_server->send(200, "text/html", "");
  s_server->sendContent(PAGE_1);
  s_server->sendContent(s_cfg->heading);
  s_server->sendContent(PAGE_2);
  s_server->sendContent(s_cfg->dir);
  s_server->sendContent(PAGE_3);
  s_server->sendContent("");            // terminate the chunked response
}

/* Browsers ask for this unprompted, and an unanswered request holds the only client slot
 * until it times out — the same starvation as keep-alive, from a request the user never
 * made. 204 costs nothing and frees the slot immediately. */
static void handleFavicon() {
  s_server->sendHeader("Connection", "close");
  s_server->send(204, "text/plain", "");
}

/* Hands back /health.log so the phone can be run on BATTERY for hours — the only way to
 * measure drain — and read afterwards without a cable. Also survives a reboot, which is
 * the point for chasing an unexplained restart: the reset reason of the run that died is
 * sitting at the top of the next run's entries. */
static void handleLog() {
  s_server->sendHeader("Connection", "close");
  File f = SD.open("/health.log");
  if (!f) {
    s_server->send(404, "text/plain", "no /health.log yet");
    return;
  }

  /* ?tail=N hands back only the LAST N bytes. Without it, read the whole file.
   *
   * Whole-file reads truncate in transit often enough that pulling 20 KB took thirty-odd
   * requests on 2026-08-14, and each attempt is a fresh TCP connection parsed through this
   * server's Strings. Internal heap is the one thing this phone has none of, so the reading
   * of the log was a real part of what the log was measuring: `largest` oscillated between
   * 3,084 and 6,660 bytes while that was going on, and the phone panicked during it.
   *
   * A truncated response also loses the END of the file — which is exactly where the reset
   * reason of the run that just died is written, so the one thing worth reading is the first
   * thing lost. The tail is both smaller and the part actually wanted.
   *
   * ⚠ Content-Length is taken AFTER the resync below, never from f.size(). A length that
   * disagrees with the body by one byte leaves the client waiting for a remainder that is
   * never sent. */
  const size_t total = f.size();
  if (s_server->hasArg("tail")) {
    const long want = s_server->arg("tail").toInt();
    if (want > 0 && (size_t)want < total) {
      f.seek(total - (size_t)want);
      /* Landing `want` bytes from the end lands mid-line almost every time. Step over the
       * remainder of it so the response starts on a whole line and stays parseable. */
      while (f.available() && f.read() != '\n') {
      }
    }
  }

  s_server->setContentLength(total - (size_t)f.position());
  s_server->send(200, "text/plain", "");
  uint8_t buf[512];
  while (f.available()) {
    const size_t n = f.read(buf, sizeof(buf));
    if (n == 0) {
      break;
    }
    s_server->client().write(buf, n);
    delay(1);                 // the same yield the upload path needs, for the same reason
  }
  f.close();
}

static void handleNotFound() {
  s_server->sendHeader("Connection", "close");
  s_server->send(404, "text/plain", "Not found");
}

// Streams an uploaded multipart file straight to the configured folder on the SD card.
/* Uploads arrive in ~2 KB pieces and used to go straight to the card, one write each.
 * A 6 MB track is then over three thousand small FAT writes, and the slow ones — the
 * cluster allocations — stall the main loop long enough for the phone to stop answering
 * the network at all. Measured mid-upload: ping to the phone went from ~4 ms to 390 ms,
 * throughput fell from 250 KB/s to 64 KB/s, and the connection was eventually reset by
 * the phone. That is the "loaded halfway then crashed" this used to do.
 *
 * So pieces are gathered in PSRAM and committed in 32 KB blocks: a couple of hundred big
 * writes instead of thousands of small ones. PSRAM because the internal heap has no room
 * to spare — the same rule as everything else here. */
#define XFER_SD_BLOCK  (32 * 1024)
static uint8_t* s_sdBuf = NULL;
static size_t   s_sdLen = 0;

static bool xferFlushBlock() {
  if (!s_uploadFile || !s_sdBuf || s_sdLen == 0) {
    return true;
  }
  const size_t wrote = s_uploadFile.write(s_sdBuf, s_sdLen);
  const bool ok = (wrote == s_sdLen);
  s_sdLen = 0;
  return ok;
}

static void handleUpload() {
  HTTPUpload& up = s_server->upload();
  if (up.status == UPLOAD_FILE_START) {
    if (s_uploadFile) {
      s_uploadFile.close();         // close a handle left over from an aborted upload
    }
    if (!s_sdBuf) {
      s_sdBuf = (uint8_t*)ps_malloc(XFER_SD_BLOCK);
    }
    s_sdLen = 0;
    SD.mkdir(s_cfg->dir);
    String path = String(s_cfg->dir) + "/" + up.filename;
    SD.remove(path.c_str());        // overwrite cleanly (SD write mode appends)
    s_uploadFile = SD.open(path.c_str(), FILE_WRITE);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    /* ── BACKPRESSURE: the fix for fast links ─────────────────────────────────
     * On a fast LAN (3 ms RTT) TCP opens wide and the radio floods internal
     * heap with RX buffers far faster than the SD drains — measured 2026-08-20
     * evening: heap 14 KB → 868 BYTES inside three seconds of one upload,
     * while the slow phone-hotspot link had masked the whole problem by
     * keeping bursts tiny. Flow control, the old way: while memory is tight,
     * STOP CONSUMING — the TCP window closes, the sender stalls, the radio's
     * buffers drain into the socket, heap comes back, reading resumes. The
     * cap bounds a stall against a dead client; the breaker (which now only
     * trips mid-upload on catastrophe) remains the last line. */
    {
      int spins = 0;
      while (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 7168 &&
             ++spins <= 600) {
        delay(5);                       // up to ~3 s of deliberate stall
      }
    }
    if (s_uploadFile) {
      if (s_sdBuf) {
        size_t off = 0;
        while (off < up.currentSize) {
          size_t room = XFER_SD_BLOCK - s_sdLen;
          size_t take = up.currentSize - off;
          if (take > room) {
            take = room;
          }
          memcpy(s_sdBuf + s_sdLen, up.buf + off, take);
          s_sdLen += take;
          off += take;
          if (s_sdLen == XFER_SD_BLOCK) {
            xferFlushBlock();
          }
        }
      } else {
        s_uploadFile.write(up.buf, up.currentSize);   // no PSRAM: the old slow path
      }
      /* Yield on EVERY piece, not every fourth. handleClient() owns the main loop for the
       * whole upload, so this is the only chance the WiFi and TCP stacks get to run —
       * starving them is what made the phone drop off the network mid-transfer. */
      delay(1);
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (s_uploadFile) {
      xferFlushBlock();             // the tail, which is almost never a whole block
      s_uploadFile.flush();         // commit size/data before it can be read
      s_uploadFile.close();
      s_filesAdded++;
      /* One line per file: the leak-hunting instrument. If these numbers walk
       * down across a session, something in the request path is not giving
       * memory back — measured, not guessed. */
      log_e("XFER: '%s' done (file %d this session)  heap=%u largest=%u",
            up.filename.c_str(), s_filesAdded,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    log_e("XFER: '%s' ABORTED  heap=%u largest=%u", up.filename.c_str(),
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_sdLen = 0;                    // drop the partial block rather than commit garbage
    if (s_uploadFile) {
      s_uploadFile.close();         // partial file stays; re-upload overwrites it
    }
  }
}

// Downloads a file from a (possibly https, possibly redirecting) URL to the SD card.
static bool downloadTo(const String& url, const String& path) {
  File f = SD.open(path.c_str(), FILE_WRITE);
  if (!f) {
    return false;
  }
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(10000);
  WiFiClientSecure secure;
  bool begun;
  if (url.startsWith("https")) {
    secure.setInsecure();
    begun = http.begin(secure, url);
  } else {
    begun = http.begin(url);
  }
  if (!begun) {
    f.close();
    return false;
  }
  bool ok = false;
  if (http.GET() == HTTP_CODE_OK) {
    WiFiClient* stream = http.getStreamPtr();
    int len = http.getSize();
    uint8_t buf[1024];
    uint32_t written = 0;
    uint32_t idle = millis();
    while (http.connected()) {
      size_t avail = stream->available();
      if (avail) {
        int r = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
        if (r > 0) {
          f.write(buf, r);
          idle = millis();
          written += r;
          if (written >= 32768) {   // feed the idle task/watchdog on long downloads
            written = 0;
            delay(1);
          }
          if (len > 0) {
            len -= r;
            if (len <= 0) {
              break;
            }
          }
        }
      } else if (millis() - idle > 8000) {
        break;                      // stalled
      } else {
        delay(2);
      }
    }
    ok = true;
  }
  http.end();
  f.close();
  return ok;
}

// True if `name` ends with one of the comma-separated extensions in `accept`.
static bool nameAccepted(const String& lowName, const char* accept) {
  const char* p = accept;
  while (*p) {
    const char* comma = strchr(p, ',');
    size_t n = comma ? (size_t)(comma - p) : strlen(p);
    if (n > 0 && lowName.endsWith(String(p).substring(0, n))) {
      return true;
    }
    p = comma ? comma + 1 : p + n;
  }
  return false;
}

static void handleFetch() {
  String url = s_server->arg("url");
  if (url.length() < 8) {
    s_server->send(400, "text/html", "<p>Bad URL. <a href=/>back</a></p>");
    return;
  }
  // Derive a filename from the URL; fall back to the config's default if unusable.
  String fn = url;
  int q = fn.indexOf('?');
  if (q >= 0) {
    fn = fn.substring(0, q);
  }
  int sl = fn.lastIndexOf('/');
  if (sl >= 0) {
    fn = fn.substring(sl + 1);
  }
  String low = fn;
  low.toLowerCase();
  if (!nameAccepted(low, s_cfg->accept)) {
    fn = s_cfg->defaultName;
  }
  SD.mkdir(s_cfg->dir);
  bool ok = downloadTo(url, String(s_cfg->dir) + "/" + fn);
  if (ok) {
    s_filesAdded++;
  }
  s_server->send(ok ? 200 : 500, "text/html",
                 ok ? "<p>Downloaded! <a href=/>back</a></p>"
                    : "<p>Download failed (check the link is a direct file link). <a href=/>back</a></p>");
}

/* Everything needed to serve: WebServer + handlers + mDNS. Factored so the
 * breaker's pause/resume uses the exact same lifecycle as start/stop — the
 * resume path being a lesser copy is how the first cuts went stale. */
static void xferServerUp() {
  MDNS.begin("wiphone");   // http://wiphone.local
  if (!s_server) {
    s_server = new WebServer(80);
  }
  s_server->on("/", HTTP_GET, handleRoot);
  s_server->on("/upload", HTTP_POST, []() {
    s_server->sendHeader("Connection", "close");
    s_server->send(200, "text/html", "<p>Uploaded! <a href=/>back</a></p>");
  }, handleUpload);
  s_server->on("/fetch", HTTP_POST, handleFetch);
  s_server->on("/favicon.ico", HTTP_GET, handleFavicon);
  s_server->on("/log", HTTP_GET, handleLog);
  s_server->onNotFound(handleNotFound);
  s_server->begin();
}

static void xferServerDown() {
  if (s_uploadFile) {
    s_uploadFile.close();   // an aborted upload's handle must not dangle across the pause
  }
  if (s_server) {
    s_server->stop();
    delete s_server;        // the object itself holds internal-heap Strings; free it all
    s_server = NULL;
  }
  MDNS.end();
}

void xferStart(const XferConfig* cfg) {
  s_breakerPaused = false;              // a fresh session always starts live
  s_breakerFlipMs = 0;
  if (!cfg) {
    cfg = &ROM_CFG;
  }
  if (s_on) {
    if (s_cfg == cfg) {
      return;                   // already serving this folder
    }
    xferStop();                 // a different app wants a different folder: there is one port
  }
  s_cfg = cfg;
  s_filesAdded = 0;

  // Use the joined WiFi network if we have one; otherwise host our own hotspot.
  /* ⚠ GIVE THE STATION A MOMENT BEFORE GIVING UP ON IT.
   *
   * This used to decide instantly, and deciding instantly is how opening the uploader
   * KNOCKED THE PHONE OFF WIFI: if the radio happened to be mid-association — which it
   * is after a boot, after a roam, or any time the link blipped — status() is not yet
   * WL_CONNECTED, so this fell through to softAP() and tore the station connection down
   * to host its own network. The phone then had no WiFi until something else noticed,
   * which is exactly the "no wifi for a while afterwards" that got reported.
   *
   * Two seconds is long enough to cover an association already in flight and short
   * enough not to feel like a hang when there genuinely is no network. */
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    s_usingAP = false;
    snprintf(s_addr, sizeof(s_addr), "%s", WiFi.localIP().toString().c_str());
  } else {
    s_usingAP = true;
    WiFi.mode(WIFI_AP);
    if (WiFi.softAP(s_cfg->apName)) {
      snprintf(s_addr, sizeof(s_addr), "%s", WiFi.softAPIP().toString().c_str());
    } else {
      snprintf(s_addr, sizeof(s_addr), "AP FAILED");   // surfaced on the phone screen
    }
  }

  xferServerUp();

  xferHoldAwake(true);
  s_on = true;   // gbcXferHandleClient() (main loop) now pumps it
}

void xferStop() {
  if (!s_on) {
    return;
  }
  xferHoldAwake(false);
  xferServerDown();
  if (s_usingAP) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_usingAP = false;
    /* ⚠ Setting the mode back to STA does NOT reconnect. Without this the phone sat with
     * no WiFi until some other timer got round to noticing, which felt like the uploader
     * had broken the network on its way out. begin() with no arguments re-uses the
     * credentials already in the driver. */
    WiFi.begin();
  }
  s_on = false;
}
