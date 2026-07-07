/*
 * app_gbc_xfer.cpp — WiPhone "Transfer ROMs" screen (see app_gbc_xfer.h).
 */

#include "app_gbc_xfer.h"
#include "WiFi.h"
#include "WebServer.h"
#include "ESPmDNS.h"
#include "HTTPClient.h"
#include "WiFiClientSecure.h"
#include "SD.h"

// The web server (file-scope so the C-style handlers can reach it). Pumped from
// the main loop via gbcXferHandleClient(). Only one Transfer screen at a time.
static WebServer*        s_server = NULL;
static File              s_uploadFile;
static volatile int      s_romsAdded = 0;   // uploads + downloads this session (for status)

void gbcXferHandleClient() {
  if (s_server) {
    s_server->handleClient();
  }
}

// The page served to the computer's browser: drag-and-drop / file-pick upload
// plus a "download from URL" box. Kept small and dependency-free.
static const char PAGE[] =
  "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>WiPhone ROMs</title><style>"
  "body{font-family:sans-serif;max-width:520px;margin:24px auto;padding:0 16px;color:#222}"
  "h2{margin-top:28px}#drop{border:2px dashed #888;border-radius:10px;padding:30px;text-align:center;color:#666}"
  "#drop.over{border-color:#2a7;background:#eafaf1}input[type=text]{width:100%;padding:8px;box-sizing:border-box}"
  "button{padding:9px 16px;margin-top:8px;border:0;border-radius:6px;background:#2a7;color:#fff;font-size:15px}"
  "</style></head><body>"
  "<h1>WiPhone — Add Game Boy ROMs</h1>"
  "<h2>1. Drag &amp; drop a ROM</h2>"
  "<form id=f method=POST action=/upload enctype=multipart/form-data>"
  "<div id=drop>Drop a .gb / .gbc file here, or <input type=file name=rom accept='.gb,.gbc' multiple></div>"
  "<button type=submit>Upload</button></form>"
  "<h2>2. …or paste a download link</h2>"
  "<form method=POST action=/fetch>"
  "<input type=text name=url placeholder='https://... direct link to a .gb/.gbc file'>"
  "<button type=submit>Download to phone</button></form>"
  "<p style='color:#888;margin-top:30px'>ROMs are saved to /roms and show up in the Game Boy game list.</p>"
  "<script>var d=document.getElementById('drop'),f=document.getElementById('f');"
  "['dragenter','dragover'].forEach(function(e){d.addEventListener(e,function(ev){ev.preventDefault();d.classList.add('over')})});"
  "['dragleave','drop'].forEach(function(e){d.addEventListener(e,function(ev){ev.preventDefault();d.classList.remove('over')})});"
  "d.addEventListener('drop',function(ev){var fd=new FormData();for(var i=0;i<ev.dataTransfer.files.length;i++)fd.append('rom',ev.dataTransfer.files[i]);"
  "d.textContent='Uploading...';fetch('/upload',{method:'POST',body:fd}).then(function(){d.textContent='Done! It is on your phone.'})});"
  "</script></body></html>";

static void handleRoot() {
  s_server->send(200, "text/html", PAGE);
}

// Streams an uploaded multipart file straight to /roms on the SD card.
static void handleUpload() {
  HTTPUpload& up = s_server->upload();
  if (up.status == UPLOAD_FILE_START) {
    if (s_uploadFile) {
      s_uploadFile.close();         // close a handle left over from an aborted upload
    }
    SD.mkdir("/roms");
    String path = "/roms/" + up.filename;
    SD.remove(path.c_str());        // overwrite cleanly (SD write mode appends)
    s_uploadFile = SD.open(path.c_str(), FILE_WRITE);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (s_uploadFile) {
      s_uploadFile.write(up.buf, up.currentSize);
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (s_uploadFile) {
      s_uploadFile.flush();         // commit size/data before it can be read
      s_uploadFile.close();
      s_romsAdded++;
    }
  }
}

// Downloads a ROM from a (possibly https, possibly redirecting) URL to the SD card.
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
    uint32_t idle = millis();
    while (http.connected()) {
      size_t avail = stream->available();
      if (avail) {
        int r = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
        if (r > 0) {
          f.write(buf, r);
          idle = millis();
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

static void handleFetch() {
  String url = s_server->arg("url");
  if (url.length() < 8) {
    s_server->send(400, "text/html", "<p>Bad URL. <a href=/>back</a></p>");
    return;
  }
  // Derive a filename from the URL; default to download.gbc if none/unusable.
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
  if (!low.endsWith(".gb") && !low.endsWith(".gbc")) {
    fn = "download.gbc";
  }
  SD.mkdir("/roms");
  bool ok = downloadTo(url, "/roms/" + fn);
  if (ok) {
    s_romsAdded++;
  }
  s_server->send(ok ? 200 : 500, "text/html",
                 ok ? "<p>Downloaded! <a href=/>back</a></p>"
                    : "<p>Download failed (check the link is a direct file link). <a href=/>back</a></p>");
}

GbcXferApp::GbcXferApp(LCD& disp, ControlState& state) : WiPhoneApp(disp, state) {
  log_d("create GbcXferApp");
}

GbcXferApp::~GbcXferApp() {
  log_d("destroy GbcXferApp");
  if (serverOn) {
    stopServer();
  }
}

void GbcXferApp::startServer() {
  s_romsAdded = 0;

  // Use the joined WiFi network if we have one; otherwise host our own hotspot.
  if (WiFi.status() == WL_CONNECTED) {
    usingAP = false;
    snprintf(addr, sizeof(addr), "%s", WiFi.localIP().toString().c_str());
  } else {
    usingAP = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("WiPhone-ROMs");
    snprintf(addr, sizeof(addr), "%s", WiFi.softAPIP().toString().c_str());
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

  serverOn = true;   // gbcXferHandleClient() (main loop) now pumps it
}

void GbcXferApp::stopServer() {
  if (s_uploadFile) {
    s_uploadFile.close();   // don't leave a dangling handle from an aborted upload
  }
  if (s_server) {
    s_server->stop();
    delete s_server;
    s_server = NULL;
  }
  MDNS.end();
  if (usingAP) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    usingAP = false;
  }
  serverOn = false;
}

appEventResult GbcXferApp::processEvent(EventType event) {
  if (LOGIC_BUTTON_OK(event)) {
    if (serverOn) {
      stopServer();
    } else {
      startServer();
    }
    return REDRAW_SCREEN;
  }
  if (LOGIC_BUTTON_BACK(event)) {
    return EXIT_APP;
  }
  return DO_NOTHING;
}

void GbcXferApp::redrawScreen(bool redrawAll) {
  SmoothFont* font = fonts[OPENSANS_COND_BOLD_20];
  lcd.fillScreen(BLACK);
  lcd.setTextFont(font);
  lcd.setTextDatum(TL_DATUM);
  int lh = font->height() + 4;
  int y = 8;

  lcd.setTextColor(TFT_GREENYELLOW, BLACK);
  lcd.drawString("Transfer ROMs", 8, y);
  y += lh + 4;

  lcd.setTextColor(TFT_WHITE, BLACK);
  if (!serverOn) {
    lcd.drawString("Put Game Boy ROMs on the", 8, y); y += lh;
    lcd.drawString("SD card from a computer:", 8, y); y += lh + 4;
    lcd.drawString("1. Connect phone to WiFi", 8, y); y += lh;
    lcd.drawString("2. Press OK to start", 8, y); y += lh;
    lcd.drawString("3. On your computer open", 8, y); y += lh;
    lcd.setTextColor(TFT_YELLOW, BLACK);
    lcd.drawString("   wiphone.local", 8, y); y += lh;
    lcd.setTextColor(TFT_WHITE, BLACK);
    lcd.drawString("4. Drag a ROM or paste a", 8, y); y += lh;
    lcd.drawString("   link. Done!", 8, y); y += lh + 8;
    lcd.setTextColor(TFT_DARKGREY, BLACK);
    lcd.drawString("OK: start   Back: exit", 8, lcd.height() - 24);
  } else {
    lcd.setTextColor(TFT_GREEN, BLACK);
    lcd.drawString("Server ON", 8, y); y += lh + 4;
    lcd.setTextColor(TFT_WHITE, BLACK);
    lcd.drawString("On your computer, open:", 8, y); y += lh;
    lcd.setTextColor(TFT_YELLOW, BLACK);
    lcd.drawString("  wiphone.local", 8, y); y += lh;
    char line[40];
    snprintf(line, sizeof(line), "  or http://%s", addr);
    lcd.drawString(line, 8, y); y += lh + 4;
    lcd.setTextColor(TFT_WHITE, BLACK);
    if (usingAP) {
      lcd.drawString("(join WiFi 'WiPhone-ROMs'", 8, y); y += lh;
      lcd.drawString(" first, no password)", 8, y); y += lh + 4;
    } else {
      lcd.drawString("(same WiFi as the phone)", 8, y); y += lh + 4;
    }
    snprintf(line, sizeof(line), "ROMs added: %d", s_romsAdded);
    lcd.drawString(line, 8, y); y += lh;
    lcd.setTextColor(TFT_DARKGREY, BLACK);
    lcd.drawString("OK: stop   Back: exit", 8, lcd.height() - 24);
  }
}
