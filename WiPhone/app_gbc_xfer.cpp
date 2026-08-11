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

static WebServer*   s_server = NULL;
static File         s_uploadFile;
static volatile int s_filesAdded = 0;   // uploads + downloads this session (for status)
static bool         s_on = false;
static bool         s_usingAP = false;  // true if we had to bring up our own hotspot
static char         s_addr[40] = {0};   // shown address (IP of STA or AP)

static const XferConfig ROM_CFG = {
  "/roms", "Add Game Boy ROMs", ".gb,.gbc", "ROMs", "download.gbc", "WiPhone-ROMs"
};
static const XferConfig* s_cfg = &ROM_CFG;

void gbcXferHandleClient() {
  if (s_server) {
    s_server->handleClient();
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
static const char PAGE_1[] =
  "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>WiPhone</title><style>"
  "body{font-family:sans-serif;max-width:520px;margin:24px auto;padding:0 16px;color:#222}"
  "h2{margin-top:28px}#drop{border:2px dashed #888;border-radius:10px;padding:30px;text-align:center;color:#666}"
  "#drop.over{border-color:#2a7;background:#eafaf1}input[type=text]{width:100%;padding:8px;box-sizing:border-box}"
  "button{padding:9px 16px;margin-top:8px;border:0;border-radius:6px;background:#2a7;color:#fff;font-size:15px}"
  "</style></head><body>"
  "<h1>WiPhone &mdash; ";                                     // heading
static const char PAGE_2[] =
  "</h1>"
  "<h2>1. Drag &amp; drop files</h2>"
  "<form id=f method=POST action=/upload enctype=multipart/form-data>"
  "<div id=drop>Drop ";                                        // accept
static const char PAGE_3[] =
  " files here, or <input type=file name=rom accept='";        // accept again
static const char PAGE_4[] =
  "' multiple></div>"
  "<button type=submit>Upload</button></form>"
  "<h2>2. &hellip;or paste a download link</h2>"
  "<form method=POST action=/fetch>"
  "<input type=text name=url placeholder='https://... direct link to a file'>"
  "<button type=submit>Download to phone</button></form>"
  "<p style='color:#888;margin-top:30px'>Files are saved to ";  // dir
static const char PAGE_5[] =
  " on the SD card.</p>"
  "<script>"
  "var d=document.getElementById('drop'),f=document.getElementById('f'),inp=f.querySelector('input[type=file]');"
  "function sendAll(files){"
  "if(!files||!files.length){d.textContent='No files chosen.';return;}"
  "var i=0;"
  "function next(){"
  "if(i>=files.length){d.textContent='Done! '+files.length+' file(s) on your phone.';return;}"
  "var file=files[i];"
  "d.textContent='Uploading '+(i+1)+' of '+files.length+': '+file.name;"
  "var fd=new FormData();fd.append('rom',file);"
  "fetch('/upload',{method:'POST',body:fd}).then(function(r){"
  "if(!r.ok)throw 0;i++;setTimeout(next,400);"   // breather: let the phone housekeep between files
  "}).catch(function(){d.textContent='Failed on '+file.name+' — try again.';});"
  "}"
  "next();"
  "}"
  "['dragenter','dragover'].forEach(function(e){d.addEventListener(e,function(ev){ev.preventDefault();d.classList.add('over')})});"
  "['dragleave','drop'].forEach(function(e){d.addEventListener(e,function(ev){ev.preventDefault();d.classList.remove('over')})});"
  "d.addEventListener('drop',function(ev){sendAll(ev.dataTransfer.files)});"
  "f.addEventListener('submit',function(ev){ev.preventDefault();sendAll(inp.files)});"
  "</script></body></html>";

static void handleRoot() {
  s_server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_server->send(200, "text/html", "");
  s_server->sendContent(PAGE_1);
  s_server->sendContent(s_cfg->heading);
  s_server->sendContent(PAGE_2);
  s_server->sendContent(s_cfg->accept);
  s_server->sendContent(PAGE_3);
  s_server->sendContent(s_cfg->accept);
  s_server->sendContent(PAGE_4);
  s_server->sendContent(s_cfg->dir);
  s_server->sendContent(PAGE_5);
  s_server->sendContent("");            // terminate the chunked response
}

// Streams an uploaded multipart file straight to the configured folder on the SD card.
static void handleUpload() {
  HTTPUpload& up = s_server->upload();
  if (up.status == UPLOAD_FILE_START) {
    if (s_uploadFile) {
      s_uploadFile.close();         // close a handle left over from an aborted upload
    }
    SD.mkdir(s_cfg->dir);
    String path = String(s_cfg->dir) + "/" + up.filename;
    SD.remove(path.c_str());        // overwrite cleanly (SD write mode appends)
    s_uploadFile = SD.open(path.c_str(), FILE_WRITE);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (s_uploadFile) {
      s_uploadFile.write(up.buf, up.currentSize);
      // Let the idle task run so the task watchdog doesn't reboot the phone:
      // handleClient() blocks the main loop for this entire upload.
      static uint8_t chunks = 0;
      if (++chunks >= 4) {
        chunks = 0;
        delay(1);
      }
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (s_uploadFile) {
      s_uploadFile.flush();         // commit size/data before it can be read
      s_uploadFile.close();
      s_filesAdded++;
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
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

void xferStart(const XferConfig* cfg) {
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

  MDNS.begin("wiphone");   // http://wiphone.local

  if (!s_server) {
    s_server = new WebServer(80);
  }
  s_server->on("/", HTTP_GET, handleRoot);
  s_server->on("/upload", HTTP_POST, []() {
    s_server->send(200, "text/html", "<p>Uploaded! <a href=/>back</a></p>");
  }, handleUpload);
  s_server->on("/fetch", HTTP_POST, handleFetch);
  s_server->begin();

  s_on = true;   // gbcXferHandleClient() (main loop) now pumps it
}

void xferStop() {
  if (!s_on) {
    return;
  }
  if (s_uploadFile) {
    s_uploadFile.close();   // don't leave a dangling handle from an aborted upload
  }
  if (s_server) {
    s_server->stop();
    delete s_server;
    s_server = NULL;
  }
  MDNS.end();
  if (s_usingAP) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_usingAP = false;
  }
  s_on = false;
}
