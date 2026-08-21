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
#include "chunk_proto.h"
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
static uint32_t     s_lastChunkMs   = 0;       // when a /chunk piece last arrived (breaker pacing)

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
      /* Every trip is now a RECYCLE-THEN-SETTLE, not an instant judgment.
       * MEASURED 2026-08-20 (first chunked hardware run): judging recovery
       * synchronously undersells it — lwIP frees pcbs from its own thread, so
       * a teardown that read "5644 → 5644, pause 600s" was at 10088 seconds
       * later. So: tear down, pause, and let the resume check (every loop
       * pass) catch the recovery the moment it lands.
       *
       * How LONG a failed settle may pause depends on who is talking. The
       * chunked page paces itself one 4 KB piece at a time — its traffic
       * cannot be the flood the old escalation was built for, and a long
       * pause just makes a patient client give up (measured: 600 s pause vs
       * the client's ~100 s of retries). Chunk traffic caps the fallback at
       * 8 s and never escalates; the legacy whole-file path keeps the
       * doubling fuse. */
      xferServerDown();
      s_breakerPaused = true;
      s_breakerFlipMs = now;
      const bool chunkPaced = (uint32_t)(now - s_lastChunkMs) < 30000;
      if (chunkPaced) {
        s_breakerWaitMs = 8000;
      } else if (s_lastResumeMs && (uint32_t)(now - s_lastResumeMs) < 60000) {
        s_breakerWaitMs = s_breakerWaitMs < 300000 ? s_breakerWaitMs * 2 : 600000;
      }
      log_e("XFER: PAUSED (largest %u%s) - settling; back on recovery or %lus",
            (unsigned)largest, chunkPaced ? ", chunk-paced" : "",
            (unsigned long)(s_breakerWaitMs / 1000));
    } else {
      if (s_lastResumeMs && (uint32_t)(now - s_lastResumeMs) > 300000) {
        s_breakerWaitMs = 90000;        // 5 min of stable service earns the short fuse back
        s_lastResumeMs = 0;
      }
      s_server->handleClient();
    }
    return;
  }
  /* 7168, the same line backpressure holds — resuming at 8192 once refused a
   * resume at 8172, which is the kind of margin nobody meant to write. */
  if ((largest >= 7168 || (uint32_t)(now - s_breakerFlipMs) >= s_breakerWaitMs) &&
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
  /* charset FIRST: the server sends no charset in Content-Type, so without
   * this meta the browser guesses Latin-1 and every em dash in the status
   * line renders as mojibake (caught on the pre-flash mock, 2026-08-20). */
  "<!doctype html><html><head><meta charset=utf-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
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
  /* THE CHUNKED SENDER (2026-08-20 redesign). One 4 KB piece per request, each
   * awaiting the phone's ack before the next departs \u2014 the sender can never hand
   * the radio more than one piece's burst, which is what keeps the phone's
   * internal heap alive on a fast LAN (see the /chunk handler's notes). Every
   * piece carries a CRC32 the server verifies before committing; a failed or
   * refused piece retries with backoff, resyncing to the server's held-byte
   * count, so a dropped connection or a breaker recycle costs one piece, not the
   * file. The old whole-file /upload stays for no-JS browsers and curl. */
  // CRC32 (IEEE) \u2014 must agree with chunk_proto.h's, or every piece is refused.
  "var CT=null;"
  "function crct(){if(CT)return CT;CT=[];for(var n=0;n<256;n++){var c=n;"
  "for(var k=0;k<8;k++)c=(c&1)?(3988292384^(c>>>1)):(c>>>1);CT[n]=c;}return CT;}"
  "function crc32(u){var t=crct(),c=4294967295;"
  "for(var i=0;i<u.length;i++)c=t[(c^u[i])&255]^(c>>>8);return(c^4294967295)>>>0;}"
  "function hex8(v){var s=v.toString(16);while(s.length<8)s='0'+s;return s;}"
  // 4096 is measured, not guessed: a 4 KB burst costs the phone's heap nothing,
  // 8 KB dips it 4 KB further, 16 KB grinds the largest free block to 636 bytes.
  "var PIECE=4096;"
  "function sendAll(files){"
  "if(!files||!files.length){ch.textContent='No files chosen.';return;}"
  "var fi=0;"
  "function nextFile(){"
  "if(fi>=files.length){ch.textContent='Done! '+files.length+' file(s) on your phone.';return;}"
  "var file=files[fi],off=0,tries=0;"
  "if(!file.size){fi++;nextFile();return;}"
  "function stat(x){ch.textContent='Uploading '+(fi+1)+' of '+files.length+': '+file.name+"
  "' \u2014 '+Math.floor(off*100/file.size)+'%'+(x||'');}"
  "function fail(){tries++;"
  "if(tries>24){ch.textContent='Gave up on '+file.name+' \u2014 is the phone still on the network?';return;}"
  "stat(' (retry '+tries+')');setTimeout(resync,Math.min(500*tries,5000));}"
  // Resync: ask the phone how much it holds, continue from there. This is
  // what makes retries idempotent and a mid-file reboot resumable.
  "function resync(){fetch('/chunk?name='+encodeURIComponent(file.name),{cache:'no-store'})"
  ".then(function(r){return r.text();}).then(function(t){"
  "var n=parseInt(t,10);if(n>=0&&n<=file.size)off=n;piece();})"
  ".catch(fail);}"
  "function piece(){"
  "if(off>=file.size){fi++;setTimeout(nextFile,300);return;}"
  "var end=Math.min(off+PIECE,file.size),fr=new FileReader();"
  "fr.onerror=fail;"
  "fr.onload=function(){"
  "var u=new Uint8Array(fr.result),fd=new FormData();"
  "fd.append('p',new Blob([u]),'p');"
  "fetch('/chunk?name='+encodeURIComponent(file.name)+'&off='+off+'&crc='+hex8(crc32(u))+"
  "'&last='+(end>=file.size?1:0),{method:'POST',body:fd})"
  ".then(function(r){return r.text().then(function(t){return{ok:r.ok,c:r.status,t:t};});})"
  ".then(function(res){"
  "var m=res.t.match(/(\\d+)/),n=m?parseInt(m[1],10):-1;"
  "if(res.ok){tries=0;off=(n>off&&n<=file.size)?n:end;stat('');piece();}"
  "else if(res.c==409&&n>=0&&n<=file.size){off=n;tries++;"
  "if(tries>24){fail();}else{setTimeout(piece,150);}}"
  "else{fail();}})"
  ".catch(fail);};"
  "fr.readAsArrayBuffer(file.slice(off,end));}"
  "stat('');piece();}"
  "nextFile();"
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

/* Per-request transient anatomy (upload-redesign brief): what ONE request
 * costs internal heap is the number that picks the chunk size, and it had
 * never been itemized. Captured here because serial cannot ask mid-POST —
 * handleClient() owns the main loop until the request completes, so the
 * handler is the only place that can watch its own footprint. */
static size_t s_reqLargest0 = 0;   // largest internal block at request START
static size_t s_reqFloor    = 0;   // lowest largest seen during this request

/* ── Chunked stop-and-wait upload — the 2026-08-20 redesign ─────────────────────────
 *
 * MEASURED (this hardware, fast LAN, docs/upload-redesign-brief.md): a whole-file
 * POST lets TCP run at line rate and the WiFi driver floods internal heap with RX
 * buffers below the application — 14 KB → 868 bytes in seconds. Graded probes put
 * numbers on it: a 4 KB body costs NOTHING beyond the ~10 KB request machinery, an
 * 8 KB body dips 4 KB further, 16 KB grinds the largest block to 636 BYTES, and a
 * vanished client mid-POST wedged the whole main loop (the flooded RX window kept
 * the FIN undeliverable, and the parser's read loop spins while `connected()`).
 *
 * So the page now sends ONE small piece per request and waits for the ack before
 * the next departs. That bounds the radio's burst at the piece size BY
 * CONSTRUCTION — no breaker or backpressure on the happy path — and it bounds the
 * wedge too: a piece is smaller than the TCP window, so the window can never fill
 * and a dead client's FIN always gets through. The protocol (offsets, CRC,
 * idempotence, the resync rule) lives in chunk_proto.h, host-tested.
 *
 * The file stays OPEN across pieces and bytes gather in the same 32 KB PSRAM block
 * the legacy path uses — SD sees the identical few-big-writes pattern that fixed
 * the mid-upload network dropouts. An acked piece is therefore in PSRAM or on the
 * card, never lost EXCEPT to a reboot; the client's resync (`GET /chunk`) covers
 * that. The buffer belongs to whichever path has a file open — the two STARTs
 * finalize each other, so the paths cannot interleave mid-file. */
#define CHUNK_PIECE_MAX  8192     // server-enforced piece cap (page sends 4096)
#define CHUNK_FLUSH_AT   24576    // flush when this full: always ≥ one piece of room left
static File     s_chunkFile;                // open across the pieces of one file
static char     s_chunkName[64] = {0};      // sanitized name of that file
static uint32_t s_chunkCrc = 0;             // running CRC of the arriving piece
static uint32_t s_chunkWantCrc = 0;         // the CRC the client claims
static size_t   s_chunkPieceLen = 0;        // bytes of the current piece gathered
static size_t   s_chunkOff = 0;             // offset the client claims
static size_t   s_chunkHeld = 0;            // durable + buffered when the piece began
static bool     s_chunkLast = false;
static bool     s_chunkGather = false;      // this piece lands at the held boundary
static bool     s_chunkSawPiece = false;    // a multipart part actually arrived
static int      s_chunkCode = 500;          // the reply, decided in the upload callback
static char     s_chunkReply[32] = "err";
static uint32_t s_chunkPieces = 0;          // pieces committed to the open file
static size_t   s_fileFloorLargest = 0;     // per-FILE heap floors (the acceptance bar)
static size_t   s_fileFloorFree = 0;

/* Flush the buffered block into the open chunk file. False = the SD lied or
 * failed; the caller must fall back to durable truth (the client resyncs). */
static bool chunkFlush() {
  if (!s_chunkFile || !s_sdBuf || s_sdLen == 0) {
    return true;
  }
  const size_t wrote = s_chunkFile.write(s_sdBuf, s_sdLen);
  const bool ok = (wrote == s_sdLen);
  s_sdLen = 0;
  return ok;
}

/* Close out the open chunk file (breaker recycle, server stop, path switch, or
 * batch end). Buffered bytes are flushed first — that is what makes an ack mean
 * "will survive a teardown". A partial file stays on the card; resume finds it. */
static void chunkFinalize() {
  if (s_chunkFile) {
    if (!chunkFlush()) {
      log_e("XFER: chunk flush FAILED at finalize ('%s') - client will resync", s_chunkName);
    }
    s_chunkFile.flush();
    s_chunkFile.close();
  }
  s_chunkName[0] = '\0';
  s_sdLen = 0;
  s_chunkPieceLen = 0;
  s_chunkGather = false;
}

static bool xferFlushBlock() {
  if (!s_uploadFile || !s_sdBuf || s_sdLen == 0) {
    return true;
  }
  const size_t wrote = s_uploadFile.write(s_sdBuf, s_sdLen);
  const bool ok = (wrote == s_sdLen);
  s_sdLen = 0;
  return ok;
}

/* GET /chunk?name= — how many bytes of <name> this card holds. The client's
 * resync question, answered with buffered bytes included (an acked piece is
 * as good as written short of a reboot, and after a reboot the buffer is
 * empty so the durable number comes out anyway). */
static void handleChunkGet() {
  s_server->sendHeader("Connection", "close");
  char name[sizeof(s_chunkName)];
  if (!chunkSafeName(s_server->arg("name").c_str(), name, sizeof(name))) {
    s_server->send(400, "text/plain", "bad name");
    return;
  }
  size_t held;
  if (s_chunkFile && !strcmp(name, s_chunkName)) {
    held = (size_t)s_chunkFile.size() + s_sdLen;
  } else {
    File f = SD.open((String(s_cfg->dir) + "/" + name).c_str());
    held = f ? (size_t)f.size() : 0;
    if (f) {
      f.close();
    }
  }
  char out[16];
  snprintf(out, sizeof(out), "%u", (unsigned)held);
  s_server->send(200, "text/plain", out);
}

/* The upload callback for POST /chunk — gathers ONE piece, decides the reply.
 * The verdict logic is chunk_proto.h's; this function only supplies the SD
 * facts and obeys. The reply is sent by the completion handler because the
 * framework calls this mid-parse, before a response may go out. */
static void handleChunkData() {
  HTTPUpload& up = s_server->upload();
  if (up.status == UPLOAD_FILE_START) {
    s_chunkSawPiece = true;
    s_chunkGather = false;
    s_chunkPieceLen = 0;
    s_lastChunkMs = millis();
    s_chunkCode = 500;
    strlcpy(s_chunkReply, "err", sizeof(s_chunkReply));
    s_reqLargest0 = s_reqFloor =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_uploadFile) {
      s_uploadFile.close();         // a dangling legacy upload must not share the buffer
      s_sdLen = 0;
    }
    char name[sizeof(s_chunkName)];
    if (!chunkSafeName(s_server->arg("name").c_str(), name, sizeof(name))) {
      s_chunkCode = 400;
      strlcpy(s_chunkReply, "bad name", sizeof(s_chunkReply));
      return;
    }
    if (!chunkParseHex32(s_server->arg("crc").c_str(), &s_chunkWantCrc)) {
      s_chunkCode = 400;
      strlcpy(s_chunkReply, "bad crc", sizeof(s_chunkReply));
      return;
    }
    s_chunkOff = (size_t)strtoul(s_server->arg("off").c_str(), NULL, 10);
    s_chunkLast = (s_server->arg("last") == "1");
    if (!s_sdBuf) {
      s_sdBuf = (uint8_t*)ps_malloc(XFER_SD_BLOCK);
      if (!s_sdBuf) {
        s_chunkCode = 507;
        strlcpy(s_chunkReply, "no mem", sizeof(s_chunkReply));
        return;
      }
    }
    /* A new name, or off==0 (a deliberate fresh start): (re)open. off==0
     * truncates — re-uploading a finished book is how the corpus tests run. */
    if (!s_chunkFile || strcmp(name, s_chunkName) || s_chunkOff == 0) {
      chunkFinalize();
      strlcpy(s_chunkName, name, sizeof(s_chunkName));
      String path = String(s_cfg->dir) + "/" + name;
      if (s_chunkOff == 0) {
        SD.mkdir(s_cfg->dir);
        SD.remove(path.c_str());
        s_chunkPieces = 0;
        s_fileFloorLargest = s_reqLargest0;
        s_fileFloorFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      }
      /* FILE_APPEND, emphatically not FILE_WRITE: in this core FILE_WRITE is
       * "w", which TRUNCATES an existing file — reopening a partial file to
       * resume it would silently discard every byte already uploaded. Append
       * mode creates-if-missing, positions at the end, and size() tells the
       * truth, which is exactly the held-bytes contract. (The legacy path gets
       * away with FILE_WRITE because it remove()s first, every time.) */
      s_chunkFile = SD.open(path.c_str(), FILE_APPEND);
      if (!s_chunkFile) {
        s_chunkName[0] = '\0';
        s_chunkCode = 507;
        strlcpy(s_chunkReply, "sd open", sizeof(s_chunkReply));
        return;
      }
    }
    s_chunkHeld = (size_t)s_chunkFile.size() + s_sdLen;
    /* Only a piece at the boundary gathers; the rest is judged at END, when
     * the length is known (a duplicate resend acks, anything else resyncs). */
    if (s_chunkOff == s_chunkHeld) {
      s_chunkGather = true;
      s_chunkCrc = chunkCrc32Init();
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    {
      const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      const size_t freeb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (largest < s_reqFloor) {
        s_reqFloor = largest;
      }
      if (largest < s_fileFloorLargest) {
        s_fileFloorLargest = largest;
      }
      if (freeb < s_fileFloorFree) {
        s_fileFloorFree = freeb;
      }
    }
    if (s_chunkGather) {
      if (s_chunkPieceLen + up.currentSize > CHUNK_PIECE_MAX ||
          s_sdLen + s_chunkPieceLen + up.currentSize > XFER_SD_BLOCK) {
        s_chunkGather = false;
        s_chunkCode = 413;
        strlcpy(s_chunkReply, "too big", sizeof(s_chunkReply));
      } else {
        memcpy(s_sdBuf + s_sdLen + s_chunkPieceLen, up.buf, up.currentSize);
        s_chunkCrc = chunkCrc32Update(s_chunkCrc, up.buf, up.currentSize);
        s_chunkPieceLen += up.currentSize;
      }
    }
    delay(1);                       // the same starvation-feeding yield as the legacy path
  } else if (up.status == UPLOAD_FILE_END) {
    if (s_chunkCode != 500 && !s_chunkGather) {
      return;                       // verdict already made at START (bad args / SD / cap)
    }
    if (s_chunkGather) {
      if (chunkCrc32Final(s_chunkCrc) != s_chunkWantCrc) {
        s_chunkCode = 422;
        strlcpy(s_chunkReply, "crc", sizeof(s_chunkReply));
        s_chunkPieceLen = 0;        // drop the piece; buffered committed bytes stand
        return;
      }
      s_sdLen += s_chunkPieceLen;   // the piece is now buffered file data
      s_chunkPieceLen = 0;
      s_chunkPieces++;
      bool ok = true;
      size_t finalSz = 0;
      if (s_chunkLast) {
        ok = chunkFlush();
        if (ok) {
          s_chunkFile.flush();
          finalSz = (size_t)s_chunkFile.size();
          s_filesAdded++;
          log_e("XFER: '%s' chunked done (file %d this session) size=%u pieces=%u "
                "file-floor: largest=%u free=%u",
                s_chunkName, s_filesAdded, (unsigned)finalSz, (unsigned)s_chunkPieces,
                (unsigned)s_fileFloorLargest, (unsigned)s_fileFloorFree);
          s_chunkFile.close();
          s_chunkName[0] = '\0';
        }
      } else if (s_sdLen >= CHUNK_FLUSH_AT) {
        ok = chunkFlush();
      }
      if (!ok) {
        /* The card refused the write. Fall back to durable truth: close out,
         * answer 507; the client resyncs and re-sends what the card lost. */
        log_e("XFER: chunk SD write FAILED ('%s') - answering 507", s_chunkName);
        chunkFinalize();
        s_chunkCode = 507;
        strlcpy(s_chunkReply, "sd", sizeof(s_chunkReply));
        return;
      }
      const size_t held = s_chunkFile ? (size_t)s_chunkFile.size() + s_sdLen : finalSz;
      s_chunkCode = 200;
      snprintf(s_chunkReply, sizeof(s_chunkReply), "ok %u", (unsigned)held);
      return;
    }
    /* Not gathered: judge the piece now that its length is known. */
    switch (chunkDecide(s_chunkHeld, s_chunkOff, up.totalSize)) {
      case CHUNK_DUPLICATE:
        s_chunkCode = 200;
        snprintf(s_chunkReply, sizeof(s_chunkReply), "ok %u", (unsigned)s_chunkHeld);
        break;
      case CHUNK_BAD:
        s_chunkCode = 400;
        strlcpy(s_chunkReply, "empty", sizeof(s_chunkReply));
        break;
      default:
        s_chunkCode = 409;
        snprintf(s_chunkReply, sizeof(s_chunkReply), "have %u", (unsigned)s_chunkHeld);
        break;
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    /* The connection died mid-piece. Committed bytes stand; the batch file
     * stays open — the client retries the piece on its next breath. The
     * saw-piece flag resets HERE because an aborted parse never reaches the
     * completion handler, and a stale flag would hand the NEXT request this
     * request's verdict. */
    s_chunkPieceLen = 0;
    s_chunkGather = false;
    s_chunkSawPiece = false;
  }
}

/* POST /chunk completion — sends whatever verdict the upload callback left.
 * A POST that carried no multipart part at all never ran the callback. */
static void handleChunkDone() {
  s_server->sendHeader("Connection", "close");
  if (!s_chunkSawPiece) {
    s_server->send(400, "text/plain", "no piece");
    return;
  }
  s_chunkSawPiece = false;
  s_server->send(s_chunkCode, "text/plain", s_chunkReply);
}

static void handleUpload() {
  HTTPUpload& up = s_server->upload();
  if (up.status == UPLOAD_FILE_START) {
    s_reqLargest0 = s_reqFloor =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_chunkFile) {
      chunkFinalize();              // the legacy path is taking the shared buffer
    }
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
      size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (largest < s_reqFloor) {
        s_reqFloor = largest;           // the anatomy: the request's true low point
      }
      int spins = 0;
      while (largest < 7168 && ++spins <= 600) {
        delay(5);                       // up to ~3 s of deliberate stall
        largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (largest < s_reqFloor) {
          s_reqFloor = largest;
        }
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
      log_e("XFER: '%s' done (file %d this session)  heap=%u largest=%u  req: start=%u floor=%u",
            up.filename.c_str(), s_filesAdded,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned)s_reqLargest0, (unsigned)s_reqFloor);
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    log_e("XFER: '%s' ABORTED  heap=%u largest=%u  req: start=%u floor=%u", up.filename.c_str(),
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
          (unsigned)s_reqLargest0, (unsigned)s_reqFloor);
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
  s_server->on("/chunk", HTTP_GET, handleChunkGet);
  s_server->on("/chunk", HTTP_POST, handleChunkDone, handleChunkData);
  s_server->on("/favicon.ico", HTTP_GET, handleFavicon);
  s_server->on("/log", HTTP_GET, handleLog);
  s_server->onNotFound(handleNotFound);
  s_server->begin();
}

static void xferServerDown() {
  chunkFinalize();          // buffered chunk bytes flush to the card; the ack contract holds
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
  s_breakerWaitMs = 90000;              // and with a fresh fuse — escalation is per-session
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
