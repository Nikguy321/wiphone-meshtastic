/*
 * kosync_sync.cpp — see kosync_sync.h. The protocol is kosync.cpp (host-tested); what is
 * here is the glue that needs the card, the radio and a socket, kept as thin as it can be.
 */
#include "kosync_sync.h"
#include "kosync.h"
#include "booksync.h"
#include "booksync_inbox.h"
#include "app_gbc_xfer.h"
#include "meshtastic_service.h"   // the default device name: the mesh long name
#include "clock.h"                // ntpClock: UTC for timestamps
#include "Networks.h"             // resolveDomain
#include "GUI.h"                  // gui.isAppRunning: no window under Settings > WiFi

extern GUI gui;

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <SD.h>
#include <Preferences.h>
#include <esp_system.h>           // esp_read_mac
#include <stdarg.h>
#include <string.h>
#include <strings.h>

static uint32_t nowUtc() {
  return ntpClock.isTimeKnown() ? (uint32_t)ntpClock.getExactUtcTime() : 0;
}

static size_t largestInternal() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void setNote(char* note, size_t cap, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static void setNote(char* note, size_t cap, const char* fmt, ...) {
  if (!note || !cap) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(note, cap, fmt, ap);
  va_end(ap);
}

static int pctInt(double p) {
  return (int)(p * 100.0 + 0.5);
}

// ================================================================ state
/* ⚠ THE TEXT AND THE CONFIG LIVE IN PSRAM, allocated the first time Books asks. As plain
 * statics they were ~1.2 KB of permanent INTERNAL RAM (measured: the 0.9.79 build's static
 * RAM went 88,828 -> 90,084) for a feature most phones never switch on — on a phone whose
 * internal heap is the thing that runs out. What stays static below is flags, counters and
 * pointers. */
struct KsText {
  KosyncConfig cfg;
  char cfgNote[80];
  char dev[40];
  char devId[KOSYNC_KEY_CHARS];
  char peer[KOSYNC_DEV_MAX];                  // who picked the window up
  char winLast[96];                           // how the last window ended, or why none opened
  char parkDev[KOSYNC_DEV_MAX];               // the PUT waiting to be parked
  char hline[128];                            // the home client's current header line
  char cliLast[96];                           // the last home job's outcome
  KosyncParkLedger ledger;                    // one KOSync offer per book (kosync.h)
};
static KsText* T = NULL;

static bool ksAlloc() {
  if (!T) {
    T = (KsText*)ps_malloc(sizeof(KsText));
    if (T) {
      memset(T, 0, sizeof(*T));
      snprintf(T->cfgNote, sizeof(T->cfgNote), "not read yet");
    }
  }
  return T != NULL;
}

// ================================================================ config
static bool         s_cfgTried = false;

bool kosyncReloadConfig() {
  s_cfgTried = true;
  if (!ksAlloc()) {
    return false;                                 // no PSRAM: KOSync is simply off
  }
  memset(&T->cfg, 0, sizeof(T->cfg));
  T->dev[0] = '\0';                               // re-derived: the name may have changed
  /* exists() first: in this core SD.open() of a missing file prints "[E] ... does not
   * exist" (vfs_api.cpp), and this runs on every entry to Books for every phone that has
   * never heard of KOSync. exists() asks the same question without the line. */
  File f;
  if (SD.exists(KOSYNC_CONFIG_FILE)) {
    f = SD.open(KOSYNC_CONFIG_FILE, FILE_READ);
  }
  if (!f) {
    snprintf(T->cfgNote, sizeof(T->cfgNote), "off (no %s)", KOSYNC_CONFIG_FILE);
    return false;
  }
  // PSRAM, and wiped after: the password is in these bytes until kosyncParseConfig hashes it.
  const size_t cap = 2048;
  char* buf = (char*)ps_malloc(cap);
  if (!buf) {
    f.close();
    snprintf(T->cfgNote, sizeof(T->cfgNote), "off (no memory to read the file)");
    return false;
  }
  int n = f.read((uint8_t*)buf, cap - 1);
  f.close();
  if (n < 0) {
    n = 0;
  }
  buf[n] = '\0';
  kosyncParseConfig(buf, (size_t)n, &T->cfg);
  memset(buf, 0, cap);
  free(buf);
  if (!T->cfg.ok) {
    snprintf(T->cfgNote, sizeof(T->cfgNote), "off - %s", T->cfg.problem);
  } else {
    snprintf(T->cfgNote, sizeof(T->cfgNote), "%s", T->cfg.problem);   // a warning, or ""
  }
  /* ⚠ The key is never logged, here or anywhere: it is the password as far as any KOSync
   * server is concerned. */
  log_e("KOSYNC config: %s user=%s home=%s:%u device=%s auto=%d open_window=%d hotspot=%s%s%s%s%s",
        T->cfg.ok ? "ON" : "off", T->cfg.user, T->cfg.home[0] ? T->cfg.home : "(none)",
        (unsigned)T->cfg.homePort, kosyncMyDevice(), (int)T->cfg.autoOnClose,
        (int)T->cfg.openWindow, T->cfg.hotspotPass[0] ? "WPA2" : "open",
        T->cfg.hotspotNote[0] ? " - " : "", T->cfg.hotspotNote,
        T->cfgNote[0] ? " - " : "", T->cfgNote);
  return T->cfg.ok;
}

bool kosyncConfigured() {
  if (!s_cfgTried) {
    kosyncReloadConfig();
  }
  return T && T->cfg.ok;
}

const KosyncConfig* kosyncConfig() {
  static const KosyncConfig kOff = KosyncConfig();   // const: flash, not RAM
  return T ? &T->cfg : &kOff;
}

const char* kosyncConfigNote() {
  return T ? T->cfgNote : "off (no memory)";
}

const char* kosyncMyDevice() {
  if (!T) {
    return "WiPhone";
  }
  if (T->cfg.device[0]) {
    return T->cfg.device;
  }
  if (!T->dev[0]) {
    /* The booksync device name (Sync settings > This device), which itself defaults to the
     * mesh long name — the same name the LoRa card already shows for this phone. */
    char name[24] = {0};
    Preferences p;
    if (p.begin("wpmesh", true)) {
      p.getString("bsdev", name, sizeof(name));
      p.end();
    }
    if (!name[0]) {
      const char* n = meshService.getMyLongName();
      snprintf(name, sizeof(name), "%s", (n && n[0]) ? n : "");
    }
    if (!name[0] || !strncasecmp(name, "WiPhone", 7)) {
      snprintf(T->dev, sizeof(T->dev), "%s", name[0] ? name : "WiPhone");
    } else {
      snprintf(T->dev, sizeof(T->dev), "WiPhone-%s", name);
    }
  }
  return T->dev;
}

const char* kosyncMyDeviceId() {
  if (!T) {
    return "";
  }
  if (!T->devId[0]) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    kosyncDeviceId(mac, T->devId);
  }
  return T->devId;
}

// On a network we joined ourselves — not our own hotspot, which has nothing behind it.
static bool onWifi() {
  return WiFi.status() == WL_CONNECTED && !xferUsingAP();
}

static bool allocBook(KosyncBook** p) {
  if (!*p) {
    *p = (KosyncBook*)ps_malloc(sizeof(KosyncBook));
  }
  return *p != NULL;
}

// ================================================================ parking a peer's place
/* The peer's percentage -> (reading chapter, within) -> a CBS1 record signed with the LOCAL
 * booksync key -> the inbox. The reading screen's one-second tick (or the next open of this
 * book) then raises the ordinary card. Called only from kosyncLoop(), never from inside the
 * raw pump: the packer and its HMAC want ~2 KB of stack, and the pump's own frames are
 * already on the 8 KB loop task when a request is being answered. */
static bool kosyncPark(const KosyncBook* b, double pct, const char* device, uint32_t turnedAt) {
  int r = 0;
  double w = 0.0;
  if (!T || !b || !epubKosyncLocate(&b->map, pct, &r, &w, NULL, NULL)) {
    log_e("KOSYNC park: '%s' cannot place %.4f (not syncable)", b ? b->title : "?", pct);
    return false;
  }
  char pass[24] = {0};
  Preferences p;
  if (p.begin("wpmesh", true)) {
    p.getString("bspw", pass, sizeof(pass));    // the reader verifies under this same key
    p.end();
  }
  uint8_t key[32];
  bookSyncDeriveKey(pass, key);
  memset(pass, 0, sizeof(pass));
  const char* idp[BOOKSYNC_MAX_IDS] = { b->ids[0], b->ids[1], b->ids[2] };
  char text[BOOKSYNC_MESH_TEXT_MAX];
  if (!kosyncParkText(idp, b->nIds, r, w, b->nRead, turnedAt, device, key, text, sizeof(text))) {
    log_e("KOSYNC park: could not build the record for '%s'", b->title);
    return false;
  }
  /* Through the ledger: this book's previous KOSync offer is REPLACED, so repeated PUTs (or
   * the same book opened again and again) can never push another book's LoRa position out
   * of the four-slot inbox. */
  const bool ok = kosyncParkInto(&T->ledger, b->byName, text, pct, nowUtc());
  log_e("KOSYNC parked %s's place in '%s': %.4f -> chapter %d, %d%% through it (%s)",
        device && device[0] ? device : "a peer", b->title, pct, r + 1, pctInt(w),
        ok ? "the card will offer it" : "inbox REFUSED it");
  return ok;
}

// ================================================================ the window
#define KOSYNC_REPLY_CAP 512
static KosyncBook*        s_win = NULL;       // PSRAM: the ONE book the window serves
static char*              s_reply = NULL;     // PSRAM: the JSON answer
static bool               s_winArmed = false;
static KosyncWindowClock  s_clock;
static uint32_t           s_gets = 0, s_puts = 0, s_parked = 0, s_other = 0, s_unauth = 0;
static uint32_t           s_healthMs = 0;
// A PUT worth a card, parked on the next kosyncLoop() pass (see kosyncPark) — WITH its own
// copy of the book it was for, so a window re-opened for another book in between (a serial
// `kosync open`, a close/open request) can never file this place under that one.
static KosyncBook*        s_parkBook = NULL;  // PSRAM
static bool               s_parkWant = false;
static double             s_parkPct = 0.0;
static uint32_t           s_parkTurned = 0;
// A window asked for by a book close/open, opened from the loop (see kosyncLoop).
static KosyncBook*        s_req = NULL;
static bool               s_reqWant = false;
static uint32_t           s_reqDurMs = 0;
static const char*        s_reqWhy = "";

bool kosyncWindowActive() {
  return s_winArmed;
}

bool kosyncWindowOpen(const KosyncBook* b, uint32_t durationMs, char* note, size_t cap) {
  if (note && cap) {
    note[0] = '\0';
  }
  if (!kosyncConfigured()) {
    setNote(note, cap, "KOSync is %s", kosyncConfigNote());
    return false;
  }
  if (!b || (!b->partial[0] && !b->byName[0])) {
    setNote(note, cap, "No KOSync id for this book");
    return false;
  }
  if (epubKosyncTotal(&b->map) == 0) {
    setNote(note, cap, "This book can't sync over KOSync (no spine sizes)");
    return false;
  }
  if (!allocBook(&s_win) || (!s_reply && !(s_reply = (char*)ps_malloc(KOSYNC_REPLY_CAP)))) {
    setNote(note, cap, "No memory for a sync window");
    return false;
  }
  const uint32_t now = millis();
  const bool sameBook = s_winArmed && !strcmp(s_win->byName, b->byName) &&
                        !strcmp(s_win->partial, b->partial);
  /* 🛑 NOT UNDER Settings > WiFi. That screen scans every 5 s and joins networks, and with a
   * hotspot live that is AP+STA channel-hopping at best and a station join under a live
   * softAP at worst — the combination WiPhone.ino's reconnect gate exists to prevent. Both
   * of its screens close any window as they open (GUI.cpp); this refuses a new one while
   * either is up (a close/open request queued a moment before, say). */
  if (gui.isAppRunning(GUI_APP_NETWORKS) || gui.isAppRunning(GUI_APP_EDITWIFI)) {
    setNote(note, cap, "Window not opened: Settings > WiFi is open");
    return false;
  }
  if (!s_winArmed && !xferWindowStart(KOSYNC_AP_NAME, T->cfg.hotspotPass)) {
    const char* why = xferWindowError();
    setNote(note, cap, "Window not opened: %s", why ? why : "no network");
    log_e("KOSYNC window NOT opened: %s", why ? why : "no network");
    return false;
  }
  uint32_t dur = durationMs;
  if (sameBook) {
    // A second ask never SHORTENS a window already open for this book (a 60 s open-window
    // arriving during a 5-minute one, say).
    const uint32_t left = kosyncClockRemainingMs(&s_clock, now);
    if (left > dur) {
      dur = left;
    }
  } else {
    s_gets = s_puts = s_parked = s_other = s_unauth = 0;
    T->peer[0] = '\0';
  }
  memcpy(s_win, b, sizeof(*b));
  kosyncClockOpen(&s_clock, now, dur);
  s_winArmed = true;
  s_healthMs = now;
  T->winLast[0] = '\0';
  char hs[48];
  kosyncHotspotLine(hs, sizeof(hs));
  log_e("KOSYNC window OPEN %lus for '%s' at %s%s%s (%s)  ids %s / %s  pct=%.4f  largest=%u",
        (unsigned long)(dur / 1000), s_win->title, xferAddr(),
        xferUsingAP() ? " on hotspot " : " (on WiFi)", xferUsingAP() ? xferApName() : "", hs,
        s_win->partial[0] ? s_win->partial : "-", s_win->byName, s_win->pct,
        (unsigned)largestInternal());
  return true;
}

void kosyncWindowClose(const char* why) {
  if (!s_winArmed) {
    return;
  }
  s_winArmed = false;
  s_clock.open = false;
  xferWindowStop();
  snprintf(T->winLast, sizeof(T->winLast), "Window closed: %s", why ? why : "");
  log_e("KOSYNC window CLOSED (%s): gets=%u puts=%u parked=%u other-book=%u unauthorised=%u",
        why ? why : "", (unsigned)s_gets, (unsigned)s_puts, (unsigned)s_parked,
        (unsigned)s_other, (unsigned)s_unauth);
}

const char* kosyncWindowServe(const char* method, const char* path, const char* hdrs,
                              const char* body, size_t bodyLen, int* code) {
  if (!s_winArmed || !s_win || !s_reply) {
    *code = 404;
    return "{\"message\":\"Not found\"}";
  }
  KosyncServed sv = { s_win->partial, s_win->byName, s_win->pctOk, s_win->pct, kosyncMyDevice(),
                      kosyncMyDeviceId(), s_win->turnedAt };
  KosyncServeOut o;
  kosyncServe(method, path, hdrs, body, bodyLen, T->cfg.user, T->cfg.key, &sv, nowUtc(),
              &o, s_reply, KOSYNC_REPLY_CAP);
  *code = o.code;
  if (o.code == 401) {
    s_unauth++;
  }
  if (o.pickedUp) {
    // A GET: the peer may now wait on a human before it PUTs — the long grace. A PUT: done.
    kosyncClockPicked(&s_clock, millis(), o.gotPut ? KOSYNC_GRACE_PUT_MS : KOSYNC_GRACE_GET_MS);
    if (o.gotPut) {
      s_puts++;
    } else {
      s_gets++;
    }
  }
  if (o.gotPut) {
    snprintf(T->peer, sizeof(T->peer), "%s", o.putDevice[0] ? o.putDevice : "a KOSync reader");
  }
  if (o.park && !allocBook(&s_parkBook)) {
    log_e("KOSYNC: no memory to hold %s's place - not offered", o.putDevice);
  } else if (o.park) {
    memcpy(s_parkBook, s_win, sizeof(*s_parkBook));
    s_parkWant = true;
    s_parkPct = o.putPct;
    snprintf(T->parkDev, sizeof(T->parkDev), "%s", o.putDevice[0] ? o.putDevice : "KOSync");
    s_parkTurned = nowUtc();
  }
  if (o.otherDoc) {
    s_other++;
  }
  /* log_e on purpose: only log_e is compiled into this firmware, and a window that "did
   * nothing" must be tellable from one that was never reached. A window sees a handful of
   * requests, so this is a handful of lines. */
  log_e("KOSYNC %s %s -> %d%s%s%s", method, path, o.code,
        o.code == 401 ? " (wrong user/password on the peer?)" : "",
        o.park ? " PARKED" : (o.gotPut ? " (already in step - no card)" : ""),
        o.otherDoc ? " (another book - discarded)" : "");
  return s_reply;
}

void kosyncNotePosition(const char* partial, const char* byName, double pct, bool pctOk,
                        uint32_t turnedAt) {
  if (!s_winArmed || !s_win || !byName) {
    return;
  }
  if (strcmp(s_win->byName, byName) != 0 || strcmp(s_win->partial, partial ? partial : "") != 0) {
    return;
  }
  /* ⚠ An update that CANNOT be expressed still updates: the reader has left the place the
   * window was holding, and a GET must now answer `{}`, not that old place. */
  s_win->pctOk = pctOk;
  s_win->pct = pctOk ? pct : 0.0;   // a GET now answers where the reader actually is
  if (turnedAt) {
    s_win->turnedAt = turnedAt;
  }
}

static void requestWindow(const KosyncBook* b, uint32_t durMs, const char* why) {
  if (!allocBook(&s_req)) {
    return;
  }
  memcpy(s_req, b, sizeof(*b));
  s_reqWant = true;
  s_reqDurMs = durMs;
  s_reqWhy = why;
}

// ================================================================ the home client
enum KsCliState { KS_IDLE = 0, KS_CONNECT, KS_HEADERS, KS_BODY };

#define KS_IO_CAP   1536                      // the request, then the response body
struct KsCliWork {
  KosyncBook     book;                        // the job's OWN copy: a new ask may not mix in
  char           io[KS_IO_CAP];
  KosyncProgress got[2];
  bool           have[2];
  int            sent;
};

static KsCliWork*  s_w = NULL;                // PSRAM
static KsCliState  s_cs = KS_IDLE;
static WiFiClient  s_client;
static KosyncBook* s_pushBook = NULL;
static KosyncBook* s_pullBook = NULL;
static bool        s_pushWant = false, s_pullWant = false;
static uint32_t    s_pushGen = 0, s_pullGen = 0, s_jobGen = 0;
static bool        s_curPush = false;
static int         s_step = 0;                // 0: the partial MD5 id, 1: the file-name id
static size_t      s_hlen = 0;
static size_t      s_bodyGot = 0;
static long        s_clen = -1;
static bool        s_chunked = false;
static int         s_code = -1;
static uint32_t    s_deadlineMs = 0, s_reqStartMs = 0;
static IPAddress   s_ip((uint32_t)0);
static uint32_t    s_ipAt = 0;

static const uint32_t KS_CONNECT_MS = 600;     // the ONLY call here that can block
static const uint32_t KS_STALL_MS   = 5000;    // silence once connected
static const uint32_t KS_TOTAL_MS   = 12000;   // one request, stall or not
static const int      KS_READ_BUDGET = 512;    // bytes per main-loop pass

bool kosyncClientBusy() {
  return s_cs != KS_IDLE;
}

static bool homeReady(char* note, size_t cap) {
  if (!kosyncConfigured()) {
    setNote(note, cap, "KOSync is %s", kosyncConfigNote());
    return false;
  }
  if (!T->cfg.home[0]) {
    setNote(note, cap, "No home= in %s", KOSYNC_CONFIG_FILE);
    return false;
  }
  if (!onWifi()) {
    setNote(note, cap, "Not on WiFi - nothing sent home");
    return false;
  }
  return true;
}

bool kosyncPush(const KosyncBook* b, char* note, size_t cap) {
  if (!homeReady(note, cap)) {
    return false;
  }
  if (!b || !b->pctOk) {
    setNote(note, cap, "This place can't be sent over KOSync");
    return false;
  }
  if (!allocBook(&s_pushBook)) {
    setNote(note, cap, "No memory to send");
    return false;
  }
  memcpy(s_pushBook, b, sizeof(*b));
  s_pushWant = true;
  s_pushGen++;
  snprintf(T->cliLast, sizeof(T->cliLast), "Sending %d%% to home...", pctInt(b->pct));
  return true;
}

bool kosyncPull(const KosyncBook* b, char* note, size_t cap) {
  if (!homeReady(note, cap)) {
    return false;
  }
  if (!b || epubKosyncTotal(&b->map) == 0) {
    setNote(note, cap, "This book can't sync over KOSync");
    return false;
  }
  if (!allocBook(&s_pullBook)) {
    setNote(note, cap, "No memory to ask");
    return false;
  }
  memcpy(s_pullBook, b, sizeof(*b));
  s_pullWant = true;
  s_pullGen++;
  snprintf(T->cliLast, sizeof(T->cliLast), "Asking home about this book...");
  return true;
}

static void finishJob(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void finishJob(const char* fmt, ...) {
  s_client.stop();
  s_cs = KS_IDLE;
  // Only retire the ask this job answered: a newer one made meanwhile still runs.
  if (s_curPush && s_pushGen == s_jobGen) {
    s_pushWant = false;
  } else if (!s_curPush && s_pullGen == s_jobGen) {
    s_pullWant = false;
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(T->cliLast, sizeof(T->cliLast), fmt, ap);
  va_end(ap);
  log_e("KOSYNC home %s: %s", s_curPush ? "push" : "pull", T->cliLast);
}

// Resolve home=, with our own 10-minute cache (resolveDomain does not cache .local).
static bool resolveHome(uint32_t now) {
  if (s_ipAt && (uint32_t)(now - s_ipAt) < 600000u && (uint32_t)s_ip != 0) {
    return true;
  }
  IPAddress ip;
  if (ip.fromString(T->cfg.home)) {
    s_ip = ip;
    s_ipAt = now | 1u;
    return true;
  }
  ip = resolveDomain(T->cfg.home);      // ⚠ can block (~500 ms for .local): prefer an IP
  if ((uint32_t)ip == 0) {
    return false;
  }
  s_ip = ip;
  s_ipAt = now | 1u;
  return true;
}

static void evaluatePull() {
  const KosyncBook* b = &s_w->book;
  int best = -1;
  bool sawAny = false;
  for (int i = 0; i < 2; i++) {
    if (!s_w->have[i] || !s_w->got[i].hasPct) {
      continue;
    }
    sawAny = true;
    const KosyncProgress& g = s_w->got[i];
    if (!kosyncWorthParking(g.pct, g.device, g.deviceId, b->pctOk ? b->pct : -1.0,
                            kosyncMyDevice(), kosyncMyDeviceId())) {
      continue;
    }
    if (!kosyncPullIsNews(g.pct, g.hasTimestamp, g.timestamp, b->pct, b->pctOk, b->turnedAt)) {
      continue;                              // older than our last page turn, or behind us
    }
    /* Both ids live on ONE server, so its timestamps are comparable: the newer record wins
     * (a stock X4 on "filename" matching and our own push under the partial MD5 can both be
     * there for the same book). A tie or a missing timestamp keeps the partial MD5's. */
    if (best < 0 || (g.hasTimestamp && s_w->got[best].hasTimestamp &&
                     g.timestamp > s_w->got[best].timestamp)) {
      best = i;
    }
  }
  if (best >= 0) {
    const KosyncProgress& g = s_w->got[best];
    const uint32_t ts = (g.hasTimestamp && g.timestamp > 0 && g.timestamp < 4000000000LL)
                        ? (uint32_t)g.timestamp : 0;
    const char* dev = g.device[0] ? g.device : "Home";
    if (kosyncPark(b, g.pct, dev, ts)) {
      finishJob("Home: %s is at %d%% - offered", dev, pctInt(g.pct));
    } else {
      finishJob("Home: %s is at %d%% - could not offer it", dev, pctInt(g.pct));
    }
  } else if (sawAny) {
    finishJob("Home: nothing newer from another device");
  } else {
    finishJob("Home: this book is not on the server yet");
  }
}

static void stepDone(int code) {
  // code -2: this id is missing on our side (an unreadable partial MD5) — skip the step.
  if (code == 401) {
    finishJob("Home: wrong user or password (401)");
    return;
  }
  if (s_curPush) {
    if (code != -2 && code != 200 && code != 202) {
      finishJob("Home: HTTP %d on PUT", code);
      return;
    }
    if (code != -2) {
      s_w->sent++;
    }
  } else if (code != -2) {
    if (code == 200) {
      s_w->have[s_step] = kosyncParseProgress(s_w->io, strlen(s_w->io), &s_w->got[s_step]);
    } else if (code != 404) {                // 404: an older server's "unknown document"
      finishJob("Home: HTTP %d on GET", code);
      return;
    }
  }
  if (s_step == 0) {
    s_step = 1;
    s_cs = KS_CONNECT;                         // a fresh connection next pass
    return;
  }
  if (s_curPush) {
    if (s_w->sent > 0) {
      finishJob("Sent %d%% to home", pctInt(s_w->book.pct));
    } else {
      finishJob("Home: no id to send under");
    }
  } else {
    evaluatePull();
  }
}

static void clientStep(bool mayUseNetwork, uint32_t now) {
  if (s_cs == KS_IDLE) {
    if (!s_pushWant && !s_pullWant) {
      return;
    }
    if (!mayUseNetwork) {
      return;                                  // held (the Game Boy owns the heap), not dropped
    }
    if (!T->cfg.ok || !T->cfg.home[0] || !onWifi()) {
      s_pushWant = s_pullWant = false;
      snprintf(T->cliLast, sizeof(T->cliLast), "Home: not on WiFi any more - nothing sent");
      log_e("KOSYNC home: %s", T->cliLast);
      return;
    }
    if (!s_w) {
      s_w = (KsCliWork*)ps_malloc(sizeof(KsCliWork));
      if (!s_w) {
        s_pushWant = s_pullWant = false;
        snprintf(T->cliLast, sizeof(T->cliLast), "Home: no memory");
        return;
      }
    }
    s_curPush = s_pushWant;                    // a push first: it is the fresher news
    s_jobGen = s_curPush ? s_pushGen : s_pullGen;
    memcpy(&s_w->book, s_curPush ? s_pushBook : s_pullBook, sizeof(KosyncBook));
    memset(s_w->got, 0, sizeof(s_w->got));
    s_w->have[0] = s_w->have[1] = false;
    s_w->sent = 0;
    s_step = 0;
    if (!resolveHome(now)) {
      finishJob("Home: '%s' not found", T->cfg.home);
      return;
    }
    s_cs = KS_CONNECT;
    return;                                    // connect on the NEXT pass: this one stays cheap
  }

  if (s_cs == KS_CONNECT) {
    const char* doc = s_step == 0 ? s_w->book.partial : s_w->book.byName;
    if (!doc[0]) {
      stepDone(-2);
      return;
    }
    // The one call here that can block, bounded to KS_CONNECT_MS (as the SMS mirror does).
    if (!s_client.connect(s_ip, T->cfg.homePort, KS_CONNECT_MS)) {
      s_ipAt = 0;                              // re-resolve next time: the address may be stale
      finishJob("Home: no answer from %s:%u", T->cfg.home, (unsigned)T->cfg.homePort);
      return;
    }
    size_t n;
    if (s_curPush) {
      char body[320];
      kosyncBuildPutBody(body, sizeof(body), doc, s_w->book.pct, kosyncMyDevice(),
                         kosyncMyDeviceId());
      n = kosyncBuildPut(s_w->io, KS_IO_CAP, T->cfg.home, T->cfg.homePort, T->cfg.user,
                         T->cfg.key, body);
    } else {
      n = kosyncBuildGet(s_w->io, KS_IO_CAP, T->cfg.home, T->cfg.homePort, T->cfg.user,
                         T->cfg.key, doc);
    }
    if (!n) {
      finishJob("Home: request too long");
      return;
    }
    s_client.write((const uint8_t*)s_w->io, n);
    s_cs = KS_HEADERS;
    s_hlen = 0;
    s_bodyGot = 0;
    s_clen = -1;
    s_chunked = false;
    s_code = -1;
    s_deadlineMs = now + KS_STALL_MS;
    s_reqStartMs = now;
    return;
  }

  // KS_HEADERS / KS_BODY: at most KS_READ_BUDGET bytes this pass.
  if ((int32_t)(now - s_deadlineMs) >= 0 || (uint32_t)(now - s_reqStartMs) > KS_TOTAL_MS) {
    finishJob("Home: timed out");
    return;
  }
  int budget = KS_READ_BUDGET;
  int consumed = 0;
  while (budget-- > 0 && s_client.available()) {
    const int c = s_client.read();
    if (c < 0) {
      break;
    }
    consumed++;
    if (s_cs == KS_HEADERS) {
      if (c == '\r') {
        continue;
      }
      if (c == '\n') {
        T->hline[s_hlen] = '\0';
        if (s_hlen == 0) {
          s_cs = KS_BODY;                      // the blank line
        } else if (s_code < 0) {
          s_code = kosyncStatusCode(T->hline);
        } else if (!strncasecmp(T->hline, "content-length:", 15)) {
          s_clen = strtol(T->hline + 15, NULL, 10);
        } else if (!strncasecmp(T->hline, "transfer-encoding:", 18) && strstr(T->hline, "chunked")) {
          s_chunked = true;
        }
        s_hlen = 0;
        continue;
      }
      if (s_hlen < sizeof(T->hline) - 1) {
        T->hline[s_hlen++] = (char)c;
      }
    } else {
      if (s_bodyGot < KS_IO_CAP - 1) {
        s_w->io[s_bodyGot] = (char)c;
      }
      s_bodyGot++;                             // counted even past the cap, for Content-Length
    }
  }
  if (consumed > 0) {
    s_deadlineMs = now + KS_STALL_MS;
  }
  const bool bodyDone = s_cs == KS_BODY && !s_chunked && s_clen >= 0 && s_bodyGot >= (size_t)s_clen;
  const bool closed = !s_client.connected() && !s_client.available();
  if (bodyDone || closed) {
    s_client.stop();
    if (s_code < 0) {
      finishJob("Home: no HTTP answer");
      return;
    }
    size_t len = s_bodyGot < KS_IO_CAP - 1 ? s_bodyGot : KS_IO_CAP - 1;
    s_w->io[len] = '\0';
    if (s_chunked) {
      const long d = kosyncDechunk(s_w->io, len);
      s_w->io[d > 0 ? (size_t)d : 0] = '\0';
    }
    stepDone(s_code);
  }
}

// ================================================================ from the reader
bool kosyncSyncMyPlace(const KosyncBook* b, char* note, size_t cap) {
  if (note && cap) {
    note[0] = '\0';
  }
  if (!kosyncConfigured()) {
    return false;
  }
  if (!b || !b->pctOk) {
    setNote(note, cap, "This place can't be sent over KOSync");
    return false;
  }
  const bool wifi = onWifi();
  const bool opened = kosyncWindowOpen(b, KOSYNC_WINDOW_SYNC_MS, note, cap);
  bool pushed = false;
  if (wifi && T->cfg.home[0]) {
    char pn[80];
    pushed = kosyncPush(b, pn, sizeof(pn));
    if (!pushed && note && !note[0]) {
      setNote(note, cap, "%s", pn);
    }
  }
  return opened || pushed;
}

void kosyncBookOpened(const KosyncBook* b) {
  if (!kosyncConfigured() || !b) {
    return;
  }
  if (onWifi()) {
    if (T->cfg.home[0]) {
      kosyncPull(b, NULL, 0);                  // a newer place from another device -> the card
    }
  } else if (T->cfg.openWindow && epubKosyncTotal(&b->map) > 0) {
    requestWindow(b, KOSYNC_WINDOW_OPEN_MS, "book opened");
  }
}

void kosyncBookClosed(const KosyncBook* b) {
  if (!kosyncConfigured() || !T->cfg.autoOnClose || !b || !b->pctOk) {
    return;
  }
  if (onWifi() && T->cfg.home[0]) {
    kosyncPush(b, NULL, 0);
  }
  requestWindow(b, KOSYNC_WINDOW_SYNC_MS, "book closed");
}

// ================================================================ status
/* Minutes, not m:ss. These lines are notes in a MenuWidget, which cannot change a row's text
 * in place, so a line that changed every second rebuilt the whole menu every second for as
 * long as a window stayed open. Whole minutes change the text ~5 times a window. */
static void minutesLeft(uint32_t ms, char* out, size_t cap) {
  if (ms < 60000u) {
    snprintf(out, cap, "under a minute");
  } else {
    snprintf(out, cap, "%u min", (unsigned)((ms + 59999u) / 60000u));
  }
}

size_t kosyncHotspotLine(char* out, size_t cap) {
  /* 🛑 WHETHER, never WHAT: "hotspot: WPA2" is the most any screen, serial line or log
   * says about hotspot_pass. */
  if (!cap) {
    return 0;
  }
  out[0] = '\0';
  if (!T) {
    return 0;
  }
  if (s_winArmed && xferUsingAP()) {
    // The hotspot actually on the air — which is the uploader's, as it was, if it got there first.
    snprintf(out, cap, "hotspot: %s%s", xferApProtected() ? "WPA2" : "open",
             xferApByUploader() ? " (uploader's)" : "");
  } else if (s_winArmed) {
    snprintf(out, cap, "hotspot: none (on WiFi)");
  } else if (T->cfg.hotspotPass[0]) {
    snprintf(out, cap, "hotspot: WPA2");
  } else if (T->cfg.hotspotNote[0]) {
    snprintf(out, cap, "hotspot: open - %s", T->cfg.hotspotNote);
  } else {
    snprintf(out, cap, "hotspot: open");
  }
  return strlen(out);
}

bool kosyncPeerPctFor(uint32_t inboxId, double* pct) {
  return T && kosyncParkedPeerPct(&T->ledger, inboxId, pct);
}

size_t kosyncWindowLine(char* out, size_t cap) {
  if (!cap) {
    return 0;
  }
  out[0] = '\0';
  if (!T) {
    return 0;
  }
  if (s_winArmed) {
    char left[24];
    minutesLeft(kosyncClockRemainingMs(&s_clock, millis()), left, sizeof(left));
    if (s_clock.picked) {
      snprintf(out, cap, "Picked up%s%s - open %s more", T->peer[0] ? " by " : "", T->peer, left);
    } else if (xferUsingAP()) {
      // "(password)": whoever joins must type the hotspot_pass — say so where they look.
      snprintf(out, cap, "Window open, %s left - %s%s %s", left, xferApName(),
               xferApProtected() ? " (password)" : "", xferAddr());
    } else {
      snprintf(out, cap, "Window open, %s left - on WiFi at %s", left, xferAddr());
    }
  } else {
    snprintf(out, cap, "%s", T->winLast);
  }
  return strlen(out);
}

size_t kosyncClientLine(char* out, size_t cap) {
  if (!cap) {
    return 0;
  }
  if (!T) {
    out[0] = '\0';
    return 0;
  }
  snprintf(out, cap, "%s", T->cliLast);
  return strlen(out);
}

void kosyncDumpStatus(void (*emit)(const char* line)) {
  char l[192];
  kosyncConfigured();
  if (!T) {
    emit("kosync: off - no PSRAM for its state");
    return;
  }
  snprintf(l, sizeof(l), "kosync: config %s%s%s  file %s", T->cfg.ok ? "ON" : "", T->cfg.ok && T->cfgNote[0] ? " - " : "",
           T->cfgNote, KOSYNC_CONFIG_FILE);
  emit(l);
  if (T->cfg.ok) {
    snprintf(l, sizeof(l), "kosync: user=%s home=%s:%u device=%s id=%.8s... auto=%s open_window=%s",
             T->cfg.user, T->cfg.home[0] ? T->cfg.home : "(none)", (unsigned)T->cfg.homePort,
             kosyncMyDevice(), kosyncMyDeviceId(), T->cfg.autoOnClose ? "on" : "off",
             T->cfg.openWindow ? "on" : "off");
    emit(l);
  }
  if (T->cfg.ok) {
    char hs[80];
    kosyncHotspotLine(hs, sizeof(hs));
    snprintf(l, sizeof(l), "kosync: %s", hs);
    emit(l);
  }
  if (s_winArmed && s_win) {
    snprintf(l, sizeof(l), "kosync: window OPEN %lus left at %s%s%s  gets=%u puts=%u parked=%u "
             "other-book=%u unauthorised=%u%s%s",
             (unsigned long)(kosyncClockRemainingMs(&s_clock, millis()) / 1000), xferAddr(),
             xferUsingAP() ? " hotspot " : " (on WiFi)", xferUsingAP() ? xferApName() : "",
             (unsigned)s_gets, (unsigned)s_puts, (unsigned)s_parked, (unsigned)s_other,
             (unsigned)s_unauth, s_clock.picked ? "  picked up" : "",
             T->peer[0] ? T->peer : "");
    emit(l);
    snprintf(l, sizeof(l), "kosync: serving '%s' partial=%s name=%s pct=%.6f",
             s_win->title, s_win->partial[0] ? s_win->partial : "-", s_win->byName, s_win->pct);
    emit(l);
  } else {
    snprintf(l, sizeof(l), "kosync: window closed%s%s", T->winLast[0] ? " - " : "", T->winLast);
    emit(l);
  }
  snprintf(l, sizeof(l), "kosync: home client %s%s%s", s_cs != KS_IDLE ? "BUSY" : "idle",
           T->cliLast[0] ? " - " : "", T->cliLast);
  emit(l);
  snprintf(l, sizeof(l), "kosync: heap largest=%u free=%u psram=%u", (unsigned)largestInternal(),
           (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  emit(l);
}

// ================================================================ the loop
void kosyncLoop(bool mayUseNetwork, bool callActive) {
  // The common case, every pass, for every phone: nothing asked, nothing open.
  if (!s_reqWant && !s_winArmed && !s_parkWant && s_cs == KS_IDLE && !s_pushWant && !s_pullWant) {
    return;
  }
  const uint32_t now = millis();

  if (s_parkWant) {
    s_parkWant = false;
    if (s_parkBook && kosyncPark(s_parkBook, s_parkPct, T->parkDev, s_parkTurned)) {
      s_parked++;
    }
  }

  if (s_reqWant) {
    s_reqWant = false;
    if (callActive) {
      /* A hotspot would take the phone off the WiFi the call is riding on. An automatic window
       * is not worth that. */
      snprintf(T->winLast, sizeof(T->winLast), "No window (%s during a call)", s_reqWhy);
      log_e("KOSYNC: %s - no window, a call is on", s_reqWhy);
    } else if (s_req) {
      char note[96];
      if (!kosyncWindowOpen(s_req, s_reqDurMs, note, sizeof(note))) {
        snprintf(T->winLast, sizeof(T->winLast), "%s", note);
        log_e("KOSYNC: %s - %s", s_reqWhy, note);
      }
    }
  }

  if (s_winArmed) {
    /* The hotspot can be taken away without the transport being told: "WiFi: off" in
     * Settings stops the radio (Networks::disable) whatever is running on it. A window with
     * no hotspot under it is not open — say so rather than count down to nothing. */
    if (xferWindowUp() && xferUsingAP() && !(WiFi.getMode() & WIFI_MODE_AP)) {
      kosyncWindowClose("the hotspot went off (WiFi switched off?)");
    }
  }
  if (s_winArmed) {
    if (!xferWindowUp()) {
      // The transport went from under us (the uploader started on another hotspot name).
      s_winArmed = false;
      s_clock.open = false;
      snprintf(T->winLast, sizeof(T->winLast), "Window closed: the uploader took the radio");
      log_e("KOSYNC window CLOSED: the uploader took the transport");
    } else {
      const int due = kosyncClockDue(&s_clock, now);
      if (due == KOSYNC_CLOCK_DEADLINE) {
        kosyncWindowClose((s_gets || s_puts) ? "time is up" : "nobody came");
      } else if (due == KOSYNC_CLOCK_PICKED) {
        char why[80];
        snprintf(why, sizeof(why), "picked up%s%s", T->peer[0] ? " by " : "", T->peer);
        kosyncWindowClose(why);
      } else if ((uint32_t)(now - s_healthMs) >= 15000u) {
        /* THE WINDOW'S HEALTH LINE — the bench's instrument for "what does a window cost":
         * the heap (a hotspot + a listening socket on a phone whose internal RAM is the
         * scarcest thing it has) and whether anyone is actually reaching it. */
        s_healthMs = now;
        log_e("KOSYNC window %lus left %s%s%s gets=%u puts=%u parked=%u 401=%u heap=%u largest=%u",
              (unsigned long)(kosyncClockRemainingMs(&s_clock, now) / 1000), xferAddr(),
              xferUsingAP() ? " ap=" : "", xferUsingAP() ? xferApName() : "",
              (unsigned)s_gets, (unsigned)s_puts, (unsigned)s_parked, (unsigned)s_unauth,
              (unsigned)ESP.getFreeHeap(), (unsigned)largestInternal());
      }
    }
  }

  clientStep(mayUseNetwork, now);
}
