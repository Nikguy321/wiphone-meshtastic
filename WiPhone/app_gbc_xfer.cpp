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

/* ── IDLE AUTO-STOP ────────────────────────────────────────────────────────────────
 * The transfer server is a term in the DFS busy predicate (WiPhone.ino), so while it
 * is up the CPU is PINNED at 240 MHz and the idle tick runs 5x faster - roughly
 * DOUBLE idle current - and it also stretches the screen sleep timeout to 10 minutes.
 * Nothing ever released that. There was no idle timeout, no client-gone timeout, no
 * WiFi-loss timeout: it stayed on until something called xferStop().
 *
 * That was survivable while the ONLY way to start it was a transfer SCREEN, because
 * every such screen calls xferStop() from its destructor - the server could not
 * outlive its owner. `up on` / `up on books` (serial_cmd.cpp) start it HEADLESS, with
 * no owner at all, and nothing to press Back on.
 *
 * MEASURED, in /tmp/wiphone-serial.log on the affected phone: `up on books`, last file
 * landed at up=14min, then 328 consecutive health samples reading cpu=240MHz with
 * scr=0 until up=92min. Eighty-two minutes at double idle draw with the screen off,
 * ended only because `up off` was typed by hand.
 *
 * So it now stops itself. The window is generous on purpose - this is a safety net for
 * a FORGOTTEN server, not a policy on slow uploads - and any traffic at all defers it,
 * so a browser sitting on the page between files is never cut off mid-batch. */
#define XFER_IDLE_STOP_MS   (10UL * 60UL * 1000UL)
static uint32_t     s_lastActivityMs = 0;      // any client traffic; 0 = never started
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

static void xferHoldAwake(bool hold) {
  if (hold == s_heldAwake) {
    return;                     // stay idempotent: one owner, at most one count
  }
  gui.state.holdScreenAwake(hold);
  s_heldAwake = hold;
}
static char         s_addr[40] = {0};   // shown address (IP of STA or AP)
/* Breaker state is per-SERVER-SESSION: xferStart() clears it, so a fresh Start
 * (or `up on`) always begins live — the first cut kept it in a function-local
 * static, and one trip outlived every restart. */
/* Why a start was refused, for the screen and the serial console. NULL = no refusal.
 * Set by xferStart() and cleared by the next attempt, so it always describes the
 * most recent try rather than an old one. */
static const char*  s_startErr = NULL;
static bool         s_breakerPaused = false;
static uint32_t     s_breakerFlipMs = 0;
static uint32_t     s_lastChunkMs   = 0;       // when a /chunk piece last arrived (breaker pacing)
static uint32_t     s_lastRawMs     = 0;       // when the raw pump last served ANYTHING (breaker pacing)
static uint32_t     s_lowSinceMs    = 0;       // largest has been under the trip line since (0 = it isn't)

static void xferServerUp();     // create + register + begin (also the breaker's resume)
static void xferServerDown();   // stop + delete (also the breaker's pause; mDNS stays up)
static void rawXferPump();      // the raw transport (:80 + :8081), pumped every pass
static void rawAbort();         // drop any in-flight raw request (transport preemption)
static bool xferFetchUrl(const char* urlIn);   // the shared pull path (raw + legacy)

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
/* The photos uploader, same argument as the books one: the bench needs to feed the
 * gallery without hands on the phone, and the Files-app route needs a thumb on the
 * folder first. ⚠ The filter is .jpg/.jpeg/.bmp because that is exactly what
 * app_photos.cpp can DECODE (isPhotoName, app_photos.cpp:44) — PNG has no decoder
 * in this firmware or in the ESP32 ROM, so offering it would put files on the card
 * that the gallery then refuses with "baseline JPEG and BMP only". */
static const XferConfig SERIAL_PHOTOS_CFG = {
  "/photos", "Add photos", ".jpg,.jpeg,.bmp", "photos", "download.jpg", "WiPhone-Photos"
};
const XferConfig* xferPhotosConfig() { return &SERIAL_PHOTOS_CFG; }
static const XferConfig* s_cfg = &ROM_CFG;

void gbcXferHandleClient() {
  if (!s_on) {
    return;
  }
  /* Forgotten-server watchdog. Checked before the pumps so a server that has already
   * gone quiet costs one comparison rather than a full poll. */
  if (s_lastActivityMs && (uint32_t)(millis() - s_lastActivityMs) > XFER_IDLE_STOP_MS) {
    log_e("XFER: idle %lu min with no client - stopping itself (it was pinning 240 MHz)",
          (unsigned long)(XFER_IDLE_STOP_MS / 60000UL));
    xferStop();
    return;
  }
  /* The raw transport rides ABOVE the breaker: its per-request heap cost is
   * zero (static header buffer, bodies straight to PSRAM, one long-lived
   * connection), so pausing it with the WebServer would only punish the one
   * client that cannot cause the pressure. */
  rawXferPump();
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
   * FREES the server outright (delete — that is what lets the heap actually
   * recover; mDNS stays up, see xferServerUp), and resume fires on heap recovery OR a timer, whichever
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
    /* While the raw transport is mid-batch the breaker does NOT trip at all.
     * MEASURED (first 4-book raw batch): dips during streaming are the driver's
     * bounded working set; pausing the IDLE port-80 server relieved nothing,
     * and every down/up cycle (MDNS.end/begin + server delete/new inside tight
     * heap) leaked — the per-file floors stair-stepped 3328 → 1536 → 1032 →
     * 616 across one batch, tracking the cycle count, and recovered the moment
     * cycling stopped. The stock trip rules resume 30 s after the last piece. */
    const bool chunkPacedNow = (uint32_t)(now - s_lastChunkMs) < 30000 ||
                               (uint32_t)(now - s_lastRawMs) < 5000;
    /* ^ raw traffic of ANY kind inhibits the trip, not just chunk pieces.
     * MEASURED 2026-08-26 after the page moved to the raw pump: 20 rapid page
     * GETs dipped largest to 188 B in sub-ms bursts (send-buffer pbufs of 2-3
     * back-to-back connections coexisting) and the breaker cycled the IDLE
     * legacy server four times for it — the exact down/up-inside-tight-heap
     * churn the mid-batch rule below was written against, relieving nothing
     * because the WebServer wasn't serving anything. Raw pressure is bounded
     * by construction; the breaker's job is the WebServer's own traffic. */
    const size_t tripAt = s_uploadFile ? 3072 : 6144;
    /* ── TRIP ON SUSTAINED PRESSURE, NOT ON ONE REQUEST'S TRANSIENT ──────────
     * MEASURED 2026-08-26 (docs/uploader-bench-2026-08-26.md): a SINGLE 829-byte
     * page GET dips largest from ~12 K to 3-4 K for well under a second — the
     * WebServer's own request transient — and the old instant judgment read
     * that as the flood, paused, and served Nick "site cannot be loaded" on
     * nearly every browser visit (2 of 10 rapid GETs answered; backoff 90 →
     * 360 → 600 s). The 2026-08-20 flood this breaker was built for pins
     * largest DOWN for many seconds, so: the 6144 line now has to hold for
     * 1.5 s of consecutive passes before it counts. Below 3072 — the altitude
     * of the historic operator-new/PHY aborts — the trip stays INSTANT, so the
     * panic guard is exactly as strong as before. */
    if (largest >= tripAt) {
      s_lowSinceMs = 0;
    } else if (!s_lowSinceMs) {
      s_lowSinceMs = now;
    }
    const bool tripNow = largest < 3072 ||
                         (s_lowSinceMs && (uint32_t)(now - s_lowSinceMs) > 1500);
    if (!chunkPacedNow && tripNow && (uint32_t)(now - s_breakerFlipMs) > 3000) {
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
      s_lowSinceMs = 0;
      log_e("XFER: PAUSED (largest %u) - legacy server freed; back when largest >= 10240",
            (unsigned)largest);
    } else {
      s_server->handleClient();
    }
    return;
  }
  /* ── RESUME ON RECOVERY, NEVER ON A TIMER (changed 0.9.28) ────────────────
   * The timer-resume existed to escape v1/v2's catch-22, where the PAUSED
   * server's own allocation pinned largest below any threshold. Pause has
   * freed the server outright since v3, so heap-driven resume cannot
   * deadlock — and MEASURED tonight, the timer became the fault instead: it
   * re-planted a ~6 KB WebServer into a 4.5 K-largest heap every 90-180 s,
   * where the sustained rule promptly re-paused it. Allocate/free churn
   * inside tight heap is the documented leak, and the page no longer needs
   * this server anyway (the raw pump serves it). 10240 is the same bar
   * xferStart() demands to start serving at all — one number, one meaning:
   * the framework server LIVES here or it does not run. */
  if (largest >= 10240 && (uint32_t)(now - s_breakerFlipMs) > 3000) {
    s_breakerPaused = false;
    s_breakerFlipMs = now;
    xferServerUp();
    log_e("XFER: RESUMED (largest %u)", (unsigned)largest);
  }
}

bool        gbcXferOn()      { return s_on; }
bool        xferUsingAP()    { return s_usingAP; }
const char* xferAddr()       { return s_addr; }
const char* xferStartError() { return s_startErr; }
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
  /* THE CHUNKED SENDER (2026-08-20/21 redesign). One piece per request, each
   * awaiting the phone's ack before the next departs; every piece carries a
   * CRC32 the server verifies before committing, and a failed piece resyncs to
   * the server's held-byte count — so a dropped connection, a breaker pause, or
   * a reboot costs pieces, never files. On load the page probes the RAW port
   * (8081): one long-lived connection, raw text/plain bodies (CORS-simple),
   * 8 KB pieces — the fast path. No raw port answering ⇒ same-origin multipart
   * /chunk at 4 KB — the compatible path. Browsers with JS but no fetch() keep
   * the native form POST to /upload (nothing is preventDefault'ed for them);
   * curl keeps /upload too. */
  // CRC32 (IEEE) — must agree with chunk_proto.h's, or every piece is refused.
  "var CT=null;"
  "function crct(){if(CT)return CT;CT=[];for(var n=0;n<256;n++){var c=n;"
  "for(var k=0;k<8;k++)c=(c&1)?(3988292384^(c>>>1)):(c>>>1);CT[n]=c;}return CT;}"
  "function crc32(u){var t=crct(),c=4294967295;"
  "for(var i=0;i<u.length;i++)c=t[(c^u[i])&255]^(c>>>8);return(c^4294967295)>>>0;}"
  "function hex8(v){var s=v.toString(16);while(s.length<8)s='0'+s;return s;}"
  // Piece sizes are measured, not guessed: 4 KB costs the WebServer path
  // nothing; the raw path's in-flight is window-bounded so 8 KB is free there.
  "var PIECE=4096,BASE='',probed=false;"
  "var RAWB='http://'+location.hostname+':8081';"
  "function probe(cb){"
  /* A page served by the raw server SAYS SO (the spliced RAWPAGE tail): same
   * origin IS the raw transport there, so the probe has nothing to discover —
   * and must not run, because its failure path would select the multipart
   * fallback, which the raw server refuses. */
  "if(window.RAWPAGE&&!probed){probed=true;PIECE=16384;}"
  "if(probed){cb();return;}"
  "var done=false,t=setTimeout(function(){if(!done){done=true;probed=true;cb();}},1500);"
  "fetch(RAWB+'/chunk?name=probe.bin',{cache:'no-store'}).then(function(r){"
  "if(!done){done=true;probed=true;clearTimeout(t);if(r.ok){BASE=RAWB;PIECE=16384;}cb();}"
  "}).catch(function(){if(!done){done=true;probed=true;clearTimeout(t);cb();}});"
  "}"
  "function sendAll(files){"
  "if(!files||!files.length){ch.textContent='No files chosen.';return;}"
  "var fi=0,sent=0,failed=[],skipped=0;"
  "probe(function(){nextFile();});"
  "function finish(){"
  "var m='Done! '+sent+' file(s) on your phone.';"
  "if(skipped)m+=' ('+skipped+' empty file(s) skipped)';"
  "if(failed.length)m='Sent '+sent+' — FAILED: '+failed.join(', ');"
  "m+=' · '+(BASE?'fast':'compatible')+' chunked transport';"
  "ch.textContent=m;}"
  "function nextFile(){"
  "if(fi>=files.length){finish();return;}"
  "var file=files[fi],off=0,tries=0,lastSync=-1,stalls=0;"
  "if(!file.size){skipped++;fi++;nextFile();return;}"
  /* ⚠ NAME THE TRANSPORT ON SCREEN. Both paths look identical while they run, and
   * the difference is not cosmetic: the chunked paths cost the heap nothing, while
   * the native form POST to /upload fragments the largest free block ~1.5 KB PER
   * FILE and trips the breaker after about six of them. On 2026-08-25 that ambiguity
   * cost an hour — curl was silently on /upload while the page was chunking, and
   * nothing on either end said which was which. "fast" = raw port 8081, 16 KB
   * pieces; "compatible" = same-origin /chunk, 4 KB. Either is safe; the form POST
   * is the one to notice, and it cannot reach this line at all. */
  "function stat(x){ch.textContent='Uploading '+(fi+1)+' of '+files.length+': '+file.name+"
  "' — '+Math.floor(off*100/file.size)+'%'+(x||'')+' ['+(BASE?'fast':'compatible')+' chunked]';}"
  // A file that gives up does NOT sink the batch: note it, move on.
  "function giveUp(why){failed.push(file.name+(why?' ('+why+')':''));fi++;setTimeout(nextFile,300);}"
  "function fail(){tries++;"
  "if(tries>24){giveUp('unreachable');return;}"
  "stat(' (retry '+tries+')');setTimeout(resync,Math.min(500*tries,5000));}"
  // Resync = ask the phone how much it holds, continue from there. A resync
  // that never advances (a dying SD acks buffered pieces, then loses them)
  // trips the stall counter instead of cycling forever.
  "function resync(){fetch(BASE+'/chunk?name='+encodeURIComponent(file.name),{cache:'no-store'})"
  ".then(function(r){return r.text();}).then(function(t){"
  "var n=parseInt(t,10);"
  "if(n>=0&&n<=file.size){"
  "if(n<=lastSync){stalls++;if(stalls>5){giveUp('no progress — SD trouble?');return;}}"
  "else{stalls=0;}"
  "lastSync=n;off=n;}"
  "piece();})"
  ".catch(fail);}"
  "function piece(){"
  "if(off>=file.size){sent++;fi++;setTimeout(nextFile,300);return;}"
  "var end=Math.min(off+PIECE,file.size),fr=new FileReader();"
  "fr.onerror=fail;"
  "fr.onload=function(){"
  "var u=new Uint8Array(fr.result);"
  "var url=BASE+'/chunk?name='+encodeURIComponent(file.name)+'&off='+off+'&crc='+hex8(crc32(u))+"
  "'&last='+(end>=file.size?1:0);"
  "var opts;"
  "if(BASE){opts={method:'POST',body:new Blob([u],{type:'text/plain'})};}"
  "else{var fd=new FormData();fd.append('p',new Blob([u]),'p');opts={method:'POST',body:fd};}"
  "fetch(url,opts)"
  ".then(function(r){return r.text().then(function(t){return{ok:r.ok,c:r.status,t:t};});})"
  ".then(function(res){"
  "var m=res.t.match(/(\\d+)/),n=m?parseInt(m[1],10):-1;"
  "if(res.ok){tries=0;off=(n>off&&n<=file.size)?n:end;stat('');piece();}"
  "else if(res.c==409&&n>=0&&n<=file.size){off=n;tries++;"
  "if(tries>24){giveUp('kept resyncing');}else{setTimeout(piece,150);}}"
  "else if(res.c==400||res.c==413||res.c==404){giveUp('refused: '+res.t);}"
  "else{fail();}})"
  ".catch(fail);};"
  "fr.readAsArrayBuffer(file.slice(off,end));}"
  "stat('');piece();}"
  "}"
  /* fetch()-less JS browsers never enter the chunked path: no preventDefault,
   * so the form's native POST /upload still works — the fallback the first
   * version of this sender silently killed. */
  "if(window.fetch){"
  "['dragenter','dragover'].forEach(function(e){d.addEventListener(e,function(ev){ev.preventDefault();d.classList.add('over')})});"
  "['dragleave','drop'].forEach(function(e){d.addEventListener(e,function(ev){ev.preventDefault();d.classList.remove('over')})});"
  "d.addEventListener('drop',function(ev){sendAll(ev.dataTransfer.files)});"
  "f.addEventListener('submit',function(ev){ev.preventDefault();sendAll(inp.files)});"
  "}"
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
  /* _P variants, and it is not pedantry: sendContent(const char*) round-trips
   * every part through a heap String — ~6 KB of copies per page view on top of
   * the framework's own request machinery, deep enough on a 12-15 K largest to
   * reach the breaker's instant tier (measured 2026-08-26: page GETs bottoming
   * at 1.6 K). sendContent_P streams straight from flash through a small
   * chunk buffer; the page costs the heap nearly nothing. */
  s_server->sendContent_P(PAGE_1);
  s_server->sendContent_P(s_cfg->heading);
  s_server->sendContent_P(PAGE_2);
  s_server->sendContent_P(s_cfg->dir);
  s_server->sendContent_P(PAGE_3);
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
#define CHUNK_PIECE_MAX  8192     // WebServer-path cap (its page sends 4096)
#define CHUNK_FLUSH_AT   16384    // flush when this full: always one RAW piece of room left
static File     s_chunkFile;                // open across the pieces of one file
static char     s_chunkName[64] = {0};      // sanitized name of that file
/* ⚠ Durable bytes are COUNTED here, never stat'd. CONFIRMED in this core's
 * vfs_api.cpp: File::size() after a write stats the PATH, and FatFS updates a
 * file's directory entry only on sync/close — so size() on the open handle
 * reports the size as of the last flush, not the truth. The one moment size()
 * is trustworthy is right after open (the file was closed then, and close
 * syncs). So: read it once at open, then count. */
static size_t   s_chunkDurable = 0;         // bytes on the card through the open handle
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
  s_chunkDurable += wrote;      // count what actually landed, even on a short write
  s_sdLen = 0;
  if (ok) {
    /* f_sync per block: the directory entry stays honest (a crash costs at
     * most one block of resend) and it is one sync per 24 KB, not per piece. */
    s_chunkFile.flush();
  }
  return ok;
}

/* Commit one CRC-verified piece of `len` bytes sitting at s_sdBuf[s_sdLen..].
 * The ONE commit path, shared by both transports (WebServer /chunk and the raw
 * port), so the flush policy, the counters, and the reply arithmetic cannot
 * drift apart. Returns the HTTP code; writes the reply text. */
static void chunkFinalize();
static int chunkCommit(size_t len, bool last, char* reply, size_t replyCap) {
  s_sdLen += len;                     // the piece is now buffered file data
  bool ok = true;
  if (last || s_sdLen >= CHUNK_FLUSH_AT) {
    ok = chunkFlush();
  }
  if (!ok) {
    /* The card refused the write. Fall back to durable truth: close out,
     * answer 507; the client resyncs and re-sends what the card lost. */
    log_e("XFER: chunk SD write FAILED ('%s') - answering 507", s_chunkName);
    chunkFinalize();
    strlcpy(reply, "sd", replyCap);
    return 507;
  }
  s_chunkPieces++;                    // counted only past the failure exit: no double-count on a 507 retry
  if (last) {
    s_filesAdded++;
    log_e("XFER: '%s' chunked done (file %d this session) size=%u pieces=%u "
          "file-floor: largest=%u free=%u",
          s_chunkName, s_filesAdded, (unsigned)s_chunkDurable, (unsigned)s_chunkPieces,
          (unsigned)s_fileFloorLargest, (unsigned)s_fileFloorFree);
    s_chunkFile.close();              // close syncs the dir entry; size() is honest again
    s_chunkName[0] = '\0';
  }
  snprintf(reply, replyCap, "ok %u", (unsigned)(s_chunkDurable + s_sdLen));
  return 200;
}

/* Make every acked byte durable WITHOUT closing the batch: flush the buffered
 * block and sync. This is what a breaker pause wants — the FIRST hardware run
 * of the raw transport showed pauses calling full finalize and chopping one
 * 374-piece book into ~17 open/close segments for no benefit (the raw client
 * was not going down; only the port-80 server was). */
static void chunkPersist() {
  if (s_chunkFile) {
    if (!chunkFlush()) {
      log_e("XFER: chunk flush FAILED at persist ('%s') - client will resync", s_chunkName);
    }
    s_chunkFile.flush();
  }
}

/* Close out the open chunk file (server stop, path switch, or batch end).
 * Buffered bytes are flushed first — that is what makes an ack mean "will
 * survive a teardown". A partial file stays on the card; resume finds it. */
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

/* Open (or keep) the batch file for `name` with the client claiming offset
 * `off` — the ONE file-lifecycle path, shared by both transports. off==0 is a
 * deliberate fresh start and truncates. False = reply already decided. */
static bool chunkOpenFor(const char* name, size_t off, int* code, char* reply, size_t replyCap) {
  if (!s_sdBuf) {
    s_sdBuf = (uint8_t*)ps_malloc(XFER_SD_BLOCK);
    if (!s_sdBuf) {
      *code = 507;
      strlcpy(reply, "no mem", replyCap);
      return false;
    }
  }
  if (!s_chunkFile || strcmp(name, s_chunkName) || off == 0) {
    chunkFinalize();
    strlcpy(s_chunkName, name, sizeof(s_chunkName));
    String path = String(s_cfg->dir) + "/" + name;
    if (off == 0) {
      SD.mkdir(s_cfg->dir);
      SD.remove(path.c_str());
    }
    /* FILE_APPEND, emphatically not FILE_WRITE: in this core FILE_WRITE is
     * "w", which TRUNCATES an existing file — reopening a partial file to
     * resume it would silently discard every byte already uploaded. Append
     * mode creates-if-missing and positions at the end. */
    s_chunkFile = SD.open(path.c_str(), FILE_APPEND);
    if (!s_chunkFile) {
      s_chunkName[0] = '\0';
      *code = 507;
      strlcpy(reply, "sd open", replyCap);
      return false;
    }
    /* size() is trustworthy HERE and only here — the file was closed until a
     * moment ago (close syncs the dir entry). From now on, count. */
    s_chunkDurable = (off == 0) ? 0 : (size_t)s_chunkFile.size();
    /* Per-SEGMENT telemetry, initialized at EVERY open — a file resumed at
     * off>0 used to inherit floors of 0 (or the previous file's), and the
     * acceptance bar would have been read off garbage. */
    s_chunkPieces = 0;
    s_fileFloorLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_fileFloorFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  return true;
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
    held = s_chunkDurable + s_sdLen;   // counted, not stat'd — size() lies while open
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
    s_lastActivityMs = millis();
    s_chunkCode = 500;
    strlcpy(s_chunkReply, "err", sizeof(s_chunkReply));
    s_reqLargest0 = s_reqFloor =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    rawAbort();                     // a WebServer piece takes the shared buffer; raw resyncs
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
    if (!chunkOpenFor(name, s_chunkOff, &s_chunkCode, s_chunkReply, sizeof(s_chunkReply))) {
      return;
    }
    s_chunkHeld = s_chunkDurable + s_sdLen;
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
      const size_t pieceLen = s_chunkPieceLen;
      s_chunkPieceLen = 0;
      s_chunkCode = chunkCommit(pieceLen, s_chunkLast, s_chunkReply, sizeof(s_chunkReply));
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

/* ── THE RAW TRANSPORT (port 8081) — why a second server exists at all ────────────────
 *
 * MEASURED 2026-08-20/21 on hardware: the framework WebServer costs ~10 KB of
 * internal-heap transient and ~1.6-2 KB of fragmentation PER REQUEST, and every
 * request is its own TCP connection (the library hardcodes Connection: close)
 * whose TIME_WAIT pcb haunts the heap for 120 s. At one 4 KB piece per request a
 * book is hundreds of requests, and two real runs — unpaced AND paced to 150 ms —
 * both ground the largest block to ~5 K and died flapping. Pacing does not fix a
 * per-request cost.
 *
 * So the chunk protocol gets a transport built for it: ONE TCP connection carries
 * the whole batch (keep-alive), requests are parsed by a ~150-line incremental
 * state machine pumped from the main loop (never blocks: reads only what is
 * available, with a deadline for dead clients), bodies are RAW bytes read straight
 * into the PSRAM block at OUR pace. In-flight data is bounded by the TCP window
 * (5,744 B) AND by stop-and-wait, so the RX flood that started all this is
 * impossible by construction, and per-request heap cost is ZERO — no Strings, no
 * HTTPUpload, no new connections. The verdicts, the store, and the commit path are
 * the same chunk_proto.h + chunkOpenFor/chunkCommit the WebServer path uses; that
 * path stays as the fallback (and the page probes 8081 and falls back by itself).
 *
 * CORS: the page lives on port 80, so 8081 is cross-origin. Pieces are sent as
 * text/plain (a CORS-simple request — no preflight in any browser that matters)
 * and every reply carries Access-Control-Allow-Origin: *; OPTIONS is answered
 * anyway for the browsers that ask first. */
#define RAW_PORT       8081
/* ── AND SINCE 0.9.28, THE RAW SERVER IS ALSO THE FRONT DOOR ON PORT 80 ─────────────
 * MEASURED 2026-08-26 (docs/uploader-bench-2026-08-26.md): the WebServer's request
 * MACHINERY — not the page bytes; sendContent_P changed nothing — costs a ~10 KB
 * internal-heap transient per fresh connection, which from a 13-15 K idle largest
 * (SIP up) bottoms at 1.6-3 K: the instant breaker tier, the PHY-abort altitude.
 * So nearly every browser VISIT (page + favicon + probe) tripped the breaker and
 * the next visitor got "site cannot be loaded". No tuning fixes a per-request
 * cost that starts below the danger line. The raw state machine serves a request
 * for ~zero heap, so the PAGE now comes from it too: it listens on :80 (browser
 * habit) and :8081 (existing tools), one client at a time, keep-alive. The
 * WebServer still exists for what genuinely needs it — multipart /upload from
 * no-JS browsers and curl -F, /fetch's HTML replies, /log — but on LEGACY_PORT,
 * where its trips can no longer take the page down with them. */
#define LEGACY_PORT    8080
#define RAW_PIECE_MAX  16384     /* fits the shared block: flush-at 16 KB leaves 16 KB of room.
                                  * Piece size is pure round-trip amortization — in-flight
                                  * bytes are bounded by the TCP window (5,744) no matter the
                                  * piece, so bigger pieces cost the heap nothing (measured:
                                  * 5744-vs-8192 scaled with size, not segment alignment). */
#define RAW_HDR_CAP    768
static WiFiServer* s_rawSrv = NULL;
static WiFiServer* s_rawSrv80 = NULL;      // same pump, port 80 — the browser's front door
static WiFiClient  s_rawCli;
static char        s_rawHdr[RAW_HDR_CAP];
static size_t      s_rawHdrLen = 0;
static bool        s_rawBody = false;      // reading a POST body (vs. headers)
static bool        s_rawFetch = false;     // this body is a small POST /fetch form, not a piece
static char        s_rawFetchBody[608];    // fetch form body (a URL + margin); pieces go to PSRAM
static uint32_t    s_rawRxMs = 0;          // last-byte time, for the dead-client deadline
static bool        s_rawGather = false;    // commit this body (vs. consume-and-reply)
static bool        s_rawLast = false;
static size_t      s_rawOff = 0, s_rawCLen = 0, s_rawGot = 0;
static uint32_t    s_rawWantCrc = 0, s_rawCrc = 0;
static int         s_rawCode = 0;          // pre-decided verdict for consume mode
static char        s_rawReply[32];

static void rawReset() {
  s_rawHdrLen = 0;
  s_rawBody = false;
  s_rawGather = false;
  s_rawFetch = false;
  s_rawGot = 0;
}

/* Abort any in-flight raw request (a WebServer transfer is taking the shared
 * buffer, or the client went quiet). Committed/buffered bytes stand — the
 * client's resync picks up from held truth. */
static void rawAbort() {
  if (s_rawCli) {
    s_rawCli.stop();
  }
  rawReset();
}

/* `key=value` out of an &-separated query, URL-decoded. False if absent. */
static bool rawQueryArg(const char* q, const char* key, char* out, size_t cap) {
  const size_t klen = strlen(key);
  while (q && *q) {
    const char* eq = strchr(q, '=');
    const char* amp = strchr(q, '&');
    if (!eq || (amp && eq > amp)) {
      q = amp ? amp + 1 : NULL;
      continue;
    }
    if ((size_t)(eq - q) == klen && !strncmp(q, key, klen)) {
      const char* v = eq + 1;
      const char* end = amp ? amp : v + strlen(v);
      size_t o = 0;
      while (v < end && o + 1 < cap) {
        char c = *v++;
        if (c == '+') {
          c = ' ';
        } else if (c == '%' && v + 1 < end && isxdigit((int)v[0]) && isxdigit((int)v[1])) {
          char hx[3] = { v[0], v[1], 0 };
          c = (char)strtoul(hx, NULL, 16);
          v += 2;
        }
        out[o++] = c;
      }
      out[o] = '\0';
      return true;
    }
    q = amp ? amp + 1 : NULL;
  }
  return false;
}

static void rawReplyEx(int code, const char* ctype, const char* body) {
  const char* st = code == 200 ? "OK" : code == 204 ? "No Content"
                 : code == 400 ? "Bad Request" : code == 404 ? "Not Found"
                 : code == 409 ? "Conflict" : code == 413 ? "Payload Too Large"
                 : code == 422 ? "Unprocessable Entity"
                 : code == 501 ? "Not Implemented" : "Insufficient Storage";
  char h[192];
  const int bl = (int)strlen(body);
  const int n = snprintf(h, sizeof(h),
                         "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                         "Content-Length: %d\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
                         code, st, ctype, bl);
  s_rawCli.write((const uint8_t*)h, (size_t)n);
  if (bl > 0) {
    s_rawCli.write((const uint8_t*)body, (size_t)bl);
  }
}

static void rawReply(int code, const char* body) {
  rawReplyEx(code, "text/plain", body);
}

/* The page, off the raw pump: the exact bytes handleRoot serves, plus a tail that
 * (a) sets RAWPAGE so the page's JS skips the probe and chunks same-origin, and
 * (b) points no-JS browsers at the legacy port, since the raw server does not
 * speak multipart. Written with an exact Content-Length so the connection can
 * stay open — keep-alive is the whole reason this server exists. The five parts
 * total ~5.5 KB: one lands in the 5744-byte socket send buffer, the rest rides
 * the window; a browser drains it immediately. Per-request heap cost: the header
 * and tail on the stack, nothing else. */
static void rawSendPage() {
  char tail[224];
  snprintf(tail, sizeof(tail),
           "<script>var RAWPAGE=1</script>"
           "<p style='color:#888;font-size:13px'>No-JavaScript browser or curl -F? "
           "Legacy uploader: http://%s:%d/</p>", s_addr, LEGACY_PORT);
  const size_t len = strlen(PAGE_1) + strlen(s_cfg->heading) + strlen(PAGE_2) +
                     strlen(s_cfg->dir) + strlen(PAGE_3) + strlen(tail);
  char h[160];
  const int n = snprintf(h, sizeof(h),
                         "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                         "Content-Length: %u\r\n\r\n", (unsigned)len);
  s_rawCli.write((const uint8_t*)h, (size_t)n);
  s_rawCli.write((const uint8_t*)PAGE_1, strlen(PAGE_1));
  s_rawCli.write((const uint8_t*)s_cfg->heading, strlen(s_cfg->heading));
  s_rawCli.write((const uint8_t*)PAGE_2, strlen(PAGE_2));
  s_rawCli.write((const uint8_t*)s_cfg->dir, strlen(s_cfg->dir));
  s_rawCli.write((const uint8_t*)PAGE_3, strlen(PAGE_3));
  s_rawCli.write((const uint8_t*)tail, strlen(tail));
}

/* Headers are complete: decide everything decidable now. Raw bodies mean the
 * piece length is known BEFORE the body arrives (Content-Length), so the
 * verdict is made here and the body is either gathered or drained. */
static void rawOnRequest() {
  s_lastRawMs = millis();
  s_rawHdr[s_rawHdrLen] = '\0';
  char method[8] = {0}, target[320] = {0};
  if (sscanf(s_rawHdr, "%7s %319s", method, target) != 2) {
    rawAbort();
    return;
  }
  // Content-Length, case-insensitively, without strcasestr.
  s_rawCLen = 0;
  for (const char* p = s_rawHdr; (p = strchr(p, '\n')) != NULL; p++) {
    if (!strncasecmp(p + 1, "content-length:", 15)) {
      s_rawCLen = (size_t)strtoul(p + 16, NULL, 10);
      break;
    }
  }
  if (!strcmp(method, "OPTIONS")) {
    char h[224];
    const int n = snprintf(h, sizeof(h),
                           "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\n"
                           "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                           "Access-Control-Allow-Headers: content-type\r\nContent-Length: 0\r\n\r\n");
    s_rawCli.write((const uint8_t*)h, (size_t)n);
    rawReset();
    return;
  }
  char* qs = strchr(target, '?');
  if (qs) {
    *qs++ = '\0';
  }
  if (!strcmp(target, "/") && !strcmp(method, "GET")) {
    rawSendPage();
    s_lastActivityMs = millis();
    rawReset();
    return;
  }
  if (!strcmp(target, "/favicon.ico")) {
    rawReply(204, "");            // unprompted browser ask; free the pump immediately
    rawReset();
    return;
  }
  if (!strcmp(target, "/fetch") && !strcmp(method, "POST")) {
    /* The page's paste-a-link form, now landing on the raw server. The body is a
     * tiny urlencoded form; gather it into the small buffer and act on completion. */
    if (s_rawCLen == 0 || s_rawCLen >= sizeof(s_rawFetchBody)) {
      rawReplyEx(400, "text/html", "<p>Bad URL. <a href=/>back</a></p>");
      rawAbort();
      return;
    }
    s_rawFetch = true;
    s_rawGather = false;
    s_rawGot = 0;
    s_rawBody = true;
    s_lastActivityMs = millis();
    return;
  }
  if (!strcmp(target, "/upload")) {
    /* Multipart is the one thing this server refuses to speak — that parser is
     * exactly the per-request machinery being escaped. The legacy server keeps it. */
    char msg[160];
    snprintf(msg, sizeof(msg),
             "<p>This port does not take form uploads - use the page's own buttons, "
             "or the legacy uploader at http://%s:%d/</p>", s_addr, LEGACY_PORT);
    rawReplyEx(501, "text/html", msg);
    rawAbort();                   // a multipart body may be in flight; drop it
    return;
  }
  if (!strcmp(target, "/log")) {
    /* The battery-run workflow reads /log by name (docs carry the exact curl).
     * File streaming is framework work — bounce to the legacy port, same query.
     * curl -L and every browser follow it. */
    char h[224];
    const int n = snprintf(h, sizeof(h),
                           "HTTP/1.1 302 Found\r\nLocation: http://%s:%d/log%s%s\r\n"
                           "Content-Length: 0\r\n\r\n",
                           s_addr, LEGACY_PORT, qs ? "?" : "", qs ? qs : "");
    s_rawCli.write((const uint8_t*)h, (size_t)n);
    rawReset();
    return;
  }
  if (strcmp(target, "/chunk") != 0) {
    rawReply(404, "nf");
    rawReset();
    return;
  }
  char rawNameArg[96] = {0};
  char name[sizeof(s_chunkName)];
  rawQueryArg(qs, "name", rawNameArg, sizeof(rawNameArg));
  if (!chunkSafeName(rawNameArg, name, sizeof(name))) {
    rawReply(400, "bad name");
    rawReset();
    return;
  }
  if (!strcmp(method, "GET")) {
    size_t held;
    if (s_chunkFile && !strcmp(name, s_chunkName)) {
      held = s_chunkDurable + s_sdLen;
    } else {
      File f = SD.open((String(s_cfg->dir) + "/" + name).c_str());
      held = f ? (size_t)f.size() : 0;
      if (f) {
        f.close();
      }
    }
    char out[16];
    snprintf(out, sizeof(out), "%u", (unsigned)held);
    rawReply(200, out);
    rawReset();
    return;
  }
  if (strcmp(method, "POST") != 0) {
    rawReply(404, "nf");
    rawReset();
    return;
  }
  char argbuf[16];
  if (!rawQueryArg(qs, "crc", argbuf, sizeof(argbuf)) || !chunkParseHex32(argbuf, &s_rawWantCrc)) {
    rawReply(400, "bad crc");
    rawAbort();               // a body may be in flight; the stream is not worth resyncing
    return;
  }
  s_rawOff = rawQueryArg(qs, "off", argbuf, sizeof(argbuf)) ? (size_t)strtoul(argbuf, NULL, 10) : 0;
  s_rawLast = rawQueryArg(qs, "last", argbuf, sizeof(argbuf)) && !strcmp(argbuf, "1");
  if (s_rawCLen == 0) {
    rawReply(400, "empty");
    rawReset();
    return;
  }
  if (s_rawCLen > RAW_PIECE_MAX) {
    rawReply(413, "too big");
    rawAbort();               // cannot drain an oversized body at loop pace
    return;
  }
  int code = 500;
  char reply[32] = "err";
  if (!chunkOpenFor(name, s_rawOff, &code, reply, sizeof(reply))) {
    strlcpy(s_rawReply, reply, sizeof(s_rawReply));
    s_rawCode = code;
    s_rawGather = false;      // drain the body, then deliver the bad news
    s_rawBody = true;
    return;
  }
  s_lastChunkMs = millis();
  s_lastActivityMs = millis();
  const size_t held = s_chunkDurable + s_sdLen;
  switch (chunkDecide(held, s_rawOff, s_rawCLen)) {
    case CHUNK_APPEND:
      s_rawGather = true;
      s_rawCrc = chunkCrc32Init();
      break;
    case CHUNK_DUPLICATE:
      s_rawGather = false;
      s_rawCode = 200;
      snprintf(s_rawReply, sizeof(s_rawReply), "ok %u", (unsigned)held);
      break;
    default:
      s_rawGather = false;
      s_rawCode = 409;
      snprintf(s_rawReply, sizeof(s_rawReply), "have %u", (unsigned)held);
      break;
  }
  s_rawGot = 0;
  s_rawBody = true;
}

static void rawXferPump() {
  if (!s_rawSrv) {
    return;
  }
  /* Most-recent-wins accept, but never preempt a client mid-request: a page
   * reload or a second tool run takes over an IDLE held connection. Two
   * listeners feed the one slot — :80 (browser) checked first, :8081 (tools). */
  WiFiServer* src = (s_rawSrv80 && s_rawSrv80->hasClient()) ? s_rawSrv80
                    : (s_rawSrv && s_rawSrv->hasClient())   ? s_rawSrv : NULL;
  if (src) {
    if (!s_rawCli || !s_rawCli.connected() || (!s_rawBody && s_rawHdrLen == 0)) {
      if (s_rawCli) {
        s_rawCli.stop();
      }
      s_rawCli = src->available();
      s_rawCli.setNoDelay(true);
      rawReset();
      s_rawRxMs = millis();
      s_lastActivityMs = millis();
    }
  }
  if (!s_rawCli || (!s_rawCli.connected() && !s_rawCli.available())) {
    if (s_rawBody || s_rawHdrLen) {
      rawReset();             // client died mid-request; committed bytes stand
    }
    return;
  }
  if ((s_rawBody || s_rawHdrLen) && (uint32_t)(millis() - s_rawRxMs) > 4000) {
    rawAbort();               // mid-request silence: a dead client must not hold the port
    return;
  }
  if (!s_rawBody) {
    while (s_rawCli.available()) {
      if (s_rawHdrLen >= RAW_HDR_CAP - 1) {
        rawAbort();
        return;
      }
      const int c = s_rawCli.read();
      if (c < 0) {
        break;
      }
      s_rawHdr[s_rawHdrLen++] = (char)c;
      s_rawRxMs = millis();
      s_lastActivityMs = millis();
      if (s_rawHdrLen >= 4 && !memcmp(s_rawHdr + s_rawHdrLen - 4, "\r\n\r\n", 4)) {
        rawOnRequest();
        break;
      }
    }
  }
  if (s_rawBody) {
    /* Drain with a ~20 ms budget instead of one available() sweep per pass.
     * MEASURED: at loop pace (~10 ms/pass) a piece needed several passes, the
     * book ran at 56 KB/s, and driver RX buffers pooled behind the slow reader
     * (free-heap floor 6 K). Emptying the driver promptly raises the floor AND
     * the rate; the main loop already tolerates longer SD stalls than 20 ms. */
    const uint32_t budget0 = millis();
    while (s_rawGot < s_rawCLen && (uint32_t)(millis() - budget0) < 20) {
      if (!s_rawCli.available()) {
        if (!s_rawCli.connected()) {
          break;
        }
        delay(1);               // mid-piece lull: give the stack a beat, keep the budget
        continue;
      }
      int n;
      if (s_rawGather) {
        size_t want = s_rawCLen - s_rawGot;
        if (want > 1436) {
          want = 1436;
        }
        n = s_rawCli.read(s_sdBuf + s_sdLen + s_rawGot, want);
        if (n <= 0) {
          break;
        }
        s_rawCrc = chunkCrc32Update(s_rawCrc, s_sdBuf + s_sdLen + s_rawGot, (size_t)n);
      } else if (s_rawFetch) {
        size_t want = s_rawCLen - s_rawGot;                   // capped at parse time
        n = s_rawCli.read((uint8_t*)s_rawFetchBody + s_rawGot, want);
        if (n <= 0) {
          break;
        }
      } else {
        uint8_t scratch[256];
        size_t want = s_rawCLen - s_rawGot;
        if (want > sizeof(scratch)) {
          want = sizeof(scratch);
        }
        n = s_rawCli.read(scratch, want);
        if (n <= 0) {
          break;
        }
      }
      s_rawGot += (size_t)n;
      s_rawRxMs = millis();
      s_lastActivityMs = millis();
    }
    {
      // The acceptance bar reads these floors; the raw path samples once per pump.
      const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      const size_t freeb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (largest < s_fileFloorLargest) {
        s_fileFloorLargest = largest;
      }
      if (freeb < s_fileFloorFree) {
        s_fileFloorFree = freeb;
      }
    }
    if (s_rawGot >= s_rawCLen) {
      if (s_rawGather) {
        if (chunkCrc32Final(s_rawCrc) != s_rawWantCrc) {
          rawReply(422, "crc");         // piece dropped; buffered committed bytes stand
        } else {
          char reply[32];
          const int code = chunkCommit(s_rawCLen, s_rawLast, reply, sizeof(reply));
          rawReply(code, reply);
        }
      } else if (s_rawFetch) {
        s_rawFetchBody[s_rawGot] = '\0';
        char url[512];
        /* The form body is &-separated k=v, exactly what the query parser eats. */
        if (!rawQueryArg(s_rawFetchBody, "url", url, sizeof(url)) || strlen(url) < 8) {
          rawReplyEx(400, "text/html", "<p>Bad URL. <a href=/>back</a></p>");
        } else {
          /* Same synchronous pull the legacy path runs: the loop blocks for the
           * download's duration, bounded by downloadTo's own 8 s stall abort.
           * The proven full-speed path (2026-08-20: 3.9 MB at ~460 KB/s). */
          const bool ok = xferFetchUrl(url);
          rawReplyEx(ok ? 200 : 500, "text/html",
                     ok ? "<p>Downloaded! <a href=/>back</a></p>"
                        : "<p>Download failed (check the link is a direct file link). "
                          "<a href=/>back</a></p>");
        }
      } else {
        rawReply(s_rawCode, s_rawReply);
      }
      rawReset();
    }
  }
}

static void rawXferUp() {
  if (!s_rawSrv) {
    s_rawSrv = new WiFiServer(RAW_PORT);
    s_rawSrv->begin();
    s_rawSrv->setNoDelay(true);
  }
  if (!s_rawSrv80) {
    s_rawSrv80 = new WiFiServer(80);
    s_rawSrv80->begin();
    s_rawSrv80->setNoDelay(true);
  }
}

static void rawXferDown() {
  rawAbort();
  s_rawCli = WiFiClient();
  if (s_rawSrv) {
    s_rawSrv->end();
    delete s_rawSrv;
    s_rawSrv = NULL;
  }
  if (s_rawSrv80) {
    s_rawSrv80->end();
    delete s_rawSrv80;
    s_rawSrv80 = NULL;
  }
}

static void handleUpload() {
  HTTPUpload& up = s_server->upload();
  if (up.status == UPLOAD_FILE_START) {
    s_reqLargest0 = s_reqFloor =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    rawAbort();                     // the legacy path preempts an in-flight raw piece too
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

/* The pull path's core, shared by the raw front door and the legacy WebServer.
 * A fetch whose derived filename collides with the OPEN batch file (the
 * `download.epub` default makes this likelier than it sounds) would open a
 * second FIL on it and truncate under the live append handle — cross-linked
 * FAT clusters. Same rule as handleUpload: the batch yields first. */
static bool xferFetchUrl(const char* urlIn) {
  chunkFinalize();
  String url = urlIn;
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
  return ok;
}

static void handleFetch() {
  String url = s_server->arg("url");
  if (url.length() < 8) {
    s_server->send(400, "text/html", "<p>Bad URL. <a href=/>back</a></p>");
    return;
  }
  bool ok = xferFetchUrl(url.c_str());
  s_server->send(ok ? 200 : 500, "text/html",
                 ok ? "<p>Downloaded! <a href=/>back</a></p>"
                    : "<p>Download failed (check the link is a direct file link). <a href=/>back</a></p>");
}

/* Everything needed to serve: WebServer + handlers + mDNS. Factored so the
 * breaker's pause/resume uses the exact same lifecycle as start/stop — the
 * resume path being a lesser copy is how the first cuts went stale. */
static void xferServerUp() {
  /* ── mDNS COMES UP ONCE PER BOOT AND NEVER GOES DOWN ─────────────────────────
   * This core's ESPmDNS leaks by design: begin() registers a WiFi event callback
   * every call and end() never removes it (WiFiGeneric's cbEventList has no
   * dedupe), so every begin/end cycle grows a permanent list entry — and the
   * breaker used to cycle it on EVERY pause/resume, inside tight heap, which is
   * a measured share of the stair-stepping floors (3328 → 616 across one batch;
   * see the raw-transport rationale). One begin, guarded, on first use; nothing
   * ever calls end() again. wiphone.local keeps resolving while the server is
   * off — the connection is then refused, which is honest, and cheaper than a
   * leak that compounds per session. */
  static bool s_mdnsUp = false;
  if (!s_mdnsUp) {
    s_mdnsUp = MDNS.begin("wiphone");   // http://wiphone.local
  }
  if (!s_server) {
    /* LEGACY_PORT since 0.9.28 — the raw pump owns :80 now. What lives here is
     * exactly what needs the framework: multipart /upload (no-JS browsers,
     * curl -F), and /log's file streaming. Its ~10 KB/request transient and its
     * breaker trips no longer touch the page. */
    s_server = new WebServer(LEGACY_PORT);
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
  chunkPersist();           // acked bytes become durable; the batch file STAYS OPEN
                            // (the raw transport rides through this teardown)
  if (s_uploadFile) {
    s_uploadFile.close();   // an aborted upload's handle must not dangle across the pause
  }
  if (s_server) {
    s_server->stop();
    delete s_server;        // the object itself holds internal-heap Strings; free it all
    s_server = NULL;
  }
  // No MDNS.end() — deliberately. See xferServerUp(): end() leaks an event-list
  // entry per cycle in this core, and the responder is cheap to keep.
}

void xferStart(const XferConfig* cfg) {
  s_startErr = NULL;                    // this attempt gets its own verdict
  /* ── REFUSE TO START WHEN THERE IS NOT ENOUGH CONTIGUOUS HEAP TO SERVE FROM ──
   * 🔑 The breaker below trips at 6144 bytes largest-internal-block, and when it
   * trips the listener closes — which a browser reports as "site cannot be
   * loaded", with nothing on the phone saying why. Starting a server that is
   * already within a few hundred bytes of that line produces exactly that: it
   * comes up, says ON with a URL, and refuses every connection.
   *
   * MEASURED 2026-08-25 on phone 1: at largest=6884 the server reported ON at a
   * valid station IP and refused three connections in a row (ConnectionRefused,
   * 0.6 s) while ping was fine. A reboot took it to largest=19132 and the same
   * server answered 200 immediately. Phone 2 served fine at largest=12940.
   *
   * ⚠ 10240 is CHOSEN, NOT MEASURED: it sits between a measured failure (6884)
   * and a measured success (12940), with room above the 6144 trip line. If a
   * start is ever refused on a phone that would have worked, this is the number
   * to revisit — and the refusal says the figure so it can be argued with. */
  {
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (largest < 10240) {
      static char why[72];
      snprintf(why, sizeof(why), "Low memory (%u B free block) - restart the phone",
               (unsigned)largest);
      s_startErr = why;
      log_e("XFER: refusing to start - largest internal block %u B, need 10240", (unsigned)largest);
      return;
    }
  }
  s_breakerPaused = false;              // a fresh session always starts live
  s_breakerFlipMs = 0;
  s_lowSinceMs = 0;
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
      s_startErr = "Could not host a hotspot either";
      /* ⚠ FAIL CLEANLY RATHER THAN ACQUIRE. This used to fall through and take the
       * hold anyway: no AP, no client possible, and yet the CPU pinned at 240 MHz and
       * the screen timeouts stretched, with the only evidence a small "AP FAILED" on
       * a screen the serial path does not even show. A server that cannot serve must
       * not cost anything. */
      log_e("XFER: softAP failed - not starting (nothing would be able to connect)");
      return;
    }
  }

  xferServerUp();
  rawXferUp();

  xferHoldAwake(true);
  s_lastActivityMs = millis();   // a server nobody ever visits still ages out
  s_on = true;   // gbcXferHandleClient() (main loop) now pumps it
}

void xferStop() {
  if (!s_on) {
    return;
  }
  xferHoldAwake(false);
  rawXferDown();
  chunkFinalize();          // a real stop DOES close the batch (persist alone is for pauses)
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
