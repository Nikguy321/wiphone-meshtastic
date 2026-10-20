/*
 * placesearch_net.cpp - see placesearch_net.h. The decisions are placesearch.cpp's (host-tested);
 * this file is NVS, the gates, the job on the shared worker, the fold and the bench.
 */
#include "placesearch_net.h"
#include "placesearch.h"
#include "config.h"             // FIRMWARE_VERSION, for the User-Agent
#include "https_worker.h"       // the shared worker, netHttps
#include "isrg_roots.h"         // ISRG Root X1 + the self-signed Root YR
#include "gemini.h"             // GEM_NET_*: the worker's one vocabulary for a failed request
#include "ai_net.h"             // aiRequestActive: another job on the worker
#include "weather_net.h"        // wxRequestActive
#include "geoloc_net.h"         // geoRequestActive
#include "tile_fetch.h"         // tlsInstallPsramHook, tileFetchActive, tileFetchCallRecent, filesJobActive
#include "app_gbc_xfer.h"       // xferOn / xferWindowUp
#include "kosync_sync.h"        // kosyncWindowActive
#include "Networks.h"           // wifiState.radioOff()
#include "music_player.h"       // a Search pauses the music (a weather Refresh's rule)
#include "meshtastic_service.h" // meshService: the console's reference (a fix, the pin)
#include "self_pos.h"           // SelfFix
#include "map_tiles.h"          // mapI7ToDeg
#include "prefs_almanac.h"      // gUnits: the console's rows in the chosen units

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <string.h>

/* The spec's User-Agent for a shared firmware with no key: the firmware, its version, the feature
 * and the repo - so a Photon operator can see who is asking and write to someone. */
#define PLACE_USER_AGENT   "WiPhone-NH/" FIRMWARE_VERSION " maps-search (+https://github.com/Nikguy321/wiphone-meshtastic)"
#define PLACE_TIMEOUT_MS   PLACE_TOTAL_MS         // the header wait: the client's own, clamped to the budget
#define PLACE_SLICE_MS     100u                   // ...in slices: Cancel and a call end it
#define PLACE_BODY_IDLE_MS 5000u
#define PLACE_NVS          "maps"                 // app_maps.cpp's MAPS_NVS
#define PLACE_NVS_ON       "search"               // int: 1 on (absent = on), 0 off
#define PLACE_NVS_URL      "searchurl"            // string: the template ("" / absent = the default)

extern volatile bool gGbcActive;                  // WiPhone.ino: the emulator owns the card and turns WiFi off

/* photon.komoot.io chains leaf <- YR1 <- Root YR (cross-signed by X1): the weather's pair. */
static const char PLACE_ROOTS_PEM[] = ISRG_ROOT_X1_PEM ISRG_ROOT_YR_PEM;

// ── module state (PSRAM: internal RAM is what this phone runs out of) ────────────────────

struct PlaceMem {
  // the settings
  bool        loaded;
  bool        enabled;
  char        tmpl[PLACE_TMPL_MAX];             // "" = the default
  char        host[PLACE_HOST_MAX];             // of the template in use
  // the request (built on the loop before the job is queued; the worker reads it)
  char        url[PLACE_URL_MAX];               // ⚠ never logged: the query is in it
  char        query[PLACE_QUERY_RAW];           // as sent (the session's last query)
  double      refLat, refLon;
  bool        haveRef;
  bool        musicPaused;
  // the answer (the worker's; the loop reads it after s_done)
  PlaceResult res[PLACE_LIMIT];
  int         n;
  int         parsed;                           // PLACE_PARSE_* (-1 = not parsed)
  int         code, net, tlsErr;
  uint32_t    dnsMs, hsMs, ms, bytes;
  bool        overflow;
  char        skipped[48];                      // a gate closed on the worker: why nothing was asked
  // the fold's reading
  int         state;                            // PLACE_IDLE / RUNNING / DONE / ERROR
  int         err;                              // PLACE_ERR_*
  char        errLine[96];
  bool        haveRes;                          // a finished search to report
  uint32_t    gen;
  // the chosen place
  bool        haveLast;
  char        lastName[PLACE_NAME_MAX];
  double      lastLat, lastLon;
  // the console
  bool        consoleAsked;                     // `maps search <text>` started the running one
  bool        consolePending;                   // ...and it finished: placeConsoleDrain prints it
  char        refWhat[24];                      // "you", "your pin"... (the echo)
};
static PlaceMem* s_m = NULL;
static volatile bool s_busy = false;              // queued or running on the worker
static volatile bool s_done = false;              // finished, not folded yet
static volatile bool s_cancel = false;            // set from the loop: the request ends

static bool memOk() {
  if (!s_m) {
    s_m = (PlaceMem*)heap_caps_calloc(1, sizeof(PlaceMem), MALLOC_CAP_SPIRAM);
    if (s_m) {
      s_m->enabled = true;
      s_m->parsed = -1;
      s_m->gen = 1;
    }
  }
  return s_m != NULL;
}

// ── NVS ──────────────────────────────────────────────────────────────────────────────────

static void loadSettings() {
  if (!memOk() || s_m->loaded) {
    return;
  }
  s_m->loaded = true;
  s_m->enabled = true;
  s_m->tmpl[0] = '\0';
  Preferences p;
  if (p.begin(PLACE_NVS, true)) {
    s_m->enabled = p.getInt(PLACE_NVS_ON, 1) != 0;
    /* isKey() first: getString() of a key never written logs an error line (ai_net.cpp's rule). */
    if (p.isKey(PLACE_NVS_URL)) {
      p.getString(PLACE_NVS_URL, s_m->tmpl, sizeof(s_m->tmpl));
    }
    p.end();
  }
  /* A stored template this firmware refuses (an older rule, a corrupt string) is not used: the
   * default stands until `maps searchurl` sets a good one. Never a URL in the log. */
  if (s_m->tmpl[0] && !placeTemplateCheck(s_m->tmpl, NULL, 0)) {
    log_e("place search: the stored endpoint template was refused - using the built-in one");
    s_m->tmpl[0] = '\0';
  }
  placeTemplateHost(s_m->tmpl, s_m->host, sizeof(s_m->host));
}

bool placeSearchEnabled() {
  loadSettings();
  return s_m ? s_m->enabled : true;
}

void placeSetEnabled(bool on) {
  loadSettings();
  if (!s_m) {
    return;
  }
  s_m->enabled = on;
  Preferences p;
  if (p.begin(PLACE_NVS, false)) {
    p.putInt(PLACE_NVS_ON, on ? 1 : 0);
    p.end();
  }
}

size_t placeSearchUrl(char* out, size_t cap) {
  if (!out || cap == 0) {
    return 0;
  }
  loadSettings();
  if (!s_m) {
    out[0] = '\0';
    return 0;
  }
  strlcpy(out, s_m->tmpl, cap);
  return strlen(out);
}

bool placeSetSearchUrl(const char* tmpl, char* why, size_t whyCap) {
  if (why && whyCap) {
    why[0] = '\0';
  }
  /* The worker reads s_m->host (DNS, the TCP connect, the SNI) while a request runs: not
   * rewritten under it. */
  if (placeRequestActive()) {
    if (why) strlcpy(why, "a search is running - set it when it ends", whyCap);
    return false;
  }
  loadSettings();
  if (!s_m) {
    if (why) strlcpy(why, "no memory for the setting", whyCap);
    return false;
  }
  if (tmpl && *tmpl && !placeTemplateCheck(tmpl, why, whyCap)) {
    return false;                                 // https only, {q}, a host...
  }
  strlcpy(s_m->tmpl, (tmpl && *tmpl) ? tmpl : "", sizeof(s_m->tmpl));
  placeTemplateHost(s_m->tmpl, s_m->host, sizeof(s_m->host));
  Preferences p;
  if (!p.begin(PLACE_NVS, false)) {
    if (why) strlcpy(why, "could not open the maps settings", whyCap);
    return false;
  }
  if (s_m->tmpl[0]) {
    p.putString(PLACE_NVS_URL, s_m->tmpl);
  } else if (p.isKey(PLACE_NVS_URL)) {
    p.remove(PLACE_NVS_URL);
  }
  p.end();
  return true;
}

size_t placeSearchHost(char* out, size_t cap) {
  if (!out || cap == 0) {
    return 0;
  }
  loadSettings();
  strlcpy(out, s_m ? s_m->host : PLACE_HOST_DEFAULT, cap);
  return strlen(out);
}

// ── the gates (the weather's, weather_net.cpp's wxGateWhy, with this feature's words) ─────

/* NULL = clear. Cheap flag reads: asked on the loop at the start and on the WORKER right before
 * the request (a queued job can wait a moment - long enough for a call to ring in). */
static const char* gateWhy() {
  if (wifiState.radioOff() || WiFi.status() != WL_CONNECTED) {
    return "not on WiFi";
  }
  if (tileFetchActive()) {
    return "a map download is running";
  }
  if (xferOn()) {
    return "the WiFi uploader is on - stop it first";
  }
  if (kosyncWindowActive() || xferWindowUp()) {
    return "a book sync window is open";
  }
  if (tileFetchCallRecent()) {
    return "a call is on, or ended under a minute ago";
  }
  if (gGbcActive) {
    return "a game is running";
  }
  if (filesJobActive()) {
    return "a Files folder job is running";
  }
  return NULL;
}

// ── the job (WORKER) ─────────────────────────────────────────────────────────────────────

static void placeGet(PlaceMem* m) {
  NetHttpReq q;
  NetHttpRes r;
  memset(&q, 0, sizeof(q));
  memset(&r, 0, sizeof(r));
  q.method = "GET";
  q.host = m->host;
  q.url = m->url;                                 // ⚠ never logged: the query is in it
  q.rootsPem = PLACE_ROOTS_PEM;                   // ISRG Root X1 + Root YR (isrg_roots.h)
  q.userAgent = PLACE_USER_AGENT;                 // "WiPhone-NH/<ver> maps-search (+<repo>)"
  q.hdrName[0] = "Accept";
  q.hdrValue[0] = "application/json";
  q.connectMs = PLACE_CONNECT_MS;                 // TCP + the handshake, timed alone (connectFirst)
  q.timeoutMs = PLACE_TIMEOUT_MS;                 // the client's own first-byte wait...
  q.waitSliceMs = PLACE_SLICE_MS;                 // ...in slices: Cancel and a call end it
  q.bodyIdleMs = PLACE_BODY_IDLE_MS;
  q.totalMs = PLACE_TOTAL_MS;                     // the whole, DNS to the last byte
  q.connectFirst = true;
  q.heapCheck = true;                             // the internal-heap bar again before the handshake
  q.cancel = &s_cancel;
  q.stopFn = tileFetchCallRecent;                 // a call rang in: stop
  q.sink = netWorkerSink();
  q.cap = NET_SINK_CAP;                           // past it: "could not read the answer"
  netHttps(&q, &r);
  m->code = r.code;
  m->net = r.net;
  m->tlsErr = r.tlsErr;
  m->bytes = (uint32_t)r.got;
  m->dnsMs = r.dnsMs;
  m->hsMs = r.hsMs;
  m->ms = r.ms;
  m->overflow = r.overflow;
  m->parsed = -1;
  if (r.code == 200 && !r.overflow && !s_cancel) {
    m->parsed = placeParse(netWorkerSink(), r.got, m->res, PLACE_LIMIT, &m->n);
  }
}

static void placeJob() {
  PlaceMem* m = s_m;
  m->n = 0;
  m->skipped[0] = '\0';
  const char* gate = gateWhy();                   // again, on the worker: it may have closed meanwhile
  if (gate) {
    m->code = 0;
    m->net = GEM_NET_CONNECT;
    m->parsed = -1;
    strlcpy(m->skipped, gate, sizeof(m->skipped));
  } else {
    placeGet(m);
  }
  s_done = true;                                  // before s_busy drops: placeRequestActive never blinks
  s_busy = false;
}

// ── the loop side ────────────────────────────────────────────────────────────────────────

bool placeRequestActive() {
  return s_busy || s_done;
}

int placeSearchState() {
  return s_m ? s_m->state : PLACE_IDLE;
}

uint32_t placeSearchGen() {
  return s_m ? s_m->gen : 0;
}

const char* placeSearchError() {
  return (s_m && s_m->state == PLACE_ERROR) ? s_m->errLine : "";
}

void placeSearchCancel() {
  if (s_busy) {
    s_cancel = true;                              // the request ends (GEM_NET_LOST); the fold reads it
  }
}

void placeSearchClear() {
  if (s_m && (s_m->state == PLACE_DONE || s_m->state == PLACE_ERROR)) {
    s_m->state = PLACE_IDLE;
    s_m->gen++;
  }
}

/* "dns" - what a failed request was, in one word (the log's; never a URL or a coordinate). */
static const char* netWord(int net) {
  switch (net) {
  case GEM_NET_DNS:       return "dns";
  case GEM_NET_CONNECT:   return "connect";
  case GEM_NET_TLS:
  case GEM_NET_TLS_OTHER: return "tls";
  case GEM_NET_NOMEM:     return "nomem";
  case GEM_NET_TIMEOUT:   return "timeout";
  case GEM_NET_LOST:      return "lost";
  case GEM_NET_TOO_BIG:   return "toobig";
  case GEM_NET_BAD_BODY:  return "badbody";
  default:                return "?";
  }
}

void placeLoopTick() {
  if (!s_done || !s_m) {
    return;
  }
  PlaceMem* m = s_m;
  if (s_cancel) {
    /* Back while it ran: no error, no results - the field again. */
    m->state = PLACE_IDLE;
    m->err = PLACE_ERR_CANCELLED;
    placeErrLine(m->err, 0, m->host, m->errLine, sizeof(m->errLine));
    m->n = 0;
    log_e("place search: cancelled");
  } else if (m->skipped[0]) {
    m->state = PLACE_ERROR;
    m->err = PLACE_ERR_BUSY;
    snprintf(m->errLine, sizeof(m->errLine), "Place search: %s", m->skipped);
    m->n = 0;
    log_e("place search: skipped - %s", m->skipped);
  } else {
    m->err = placeOutcome(m->code, m->net, m->overflow, m->parsed);
    if (m->err == PLACE_ERR_NONE) {
      m->state = PLACE_DONE;
      m->errLine[0] = '\0';
      log_e("place search: ok %d %u.%u s", m->n, (unsigned)(m->ms / 1000), (unsigned)((m->ms % 1000) / 100));
    } else {
      m->state = PLACE_ERROR;
      m->n = 0;
      placeErrLine(m->err, m->code, m->host, m->errLine, sizeof(m->errLine));
      if (m->code > 0) {
        log_e("place search: failed http %d", m->code);
      } else {
        log_e("place search: failed %s", netWord(m->net));
      }
    }
  }
  m->haveRes = true;
  if (m->consoleAsked) {
    m->consolePending = true;                     // serialCmdLoop prints it
  }
  m->consoleAsked = false;
  m->gen++;
  s_cancel = false;
  s_done = false;
}

bool placeSearchStart(const char* query, double refLat, double refLon, char* why, size_t whyCap) {
  if (!why || whyCap == 0) {
    return false;
  }
  why[0] = '\0';
  placeLoopTick();                                // a result still waiting is folded first
  /* A console search the tick above (or an earlier pass) folded is printed by serialCmdLoop's
   * drain; a start now would clear consolePending below and the answer would never be printed. */
  if (s_m && s_m->consolePending) {
    strlcpy(why, "Place search: the console's answer is not printed yet - try again", whyCap);
    return false;
  }
  if (placeRequestActive()) {
    strlcpy(why, s_cancel ? "Place search: still cancelling the last search" : "Place search: already searching",
            whyCap);
    return false;
  }
  if (!memOk()) {
    placeErrLine(PLACE_ERR_NOMEM, 0, NULL, why, whyCap);
    return false;
  }
  loadSettings();
  PlaceMem* m = s_m;
  if (!m->enabled) {
    placeErrLine(PLACE_ERR_OFF, 0, NULL, why, whyCap);
    return false;
  }
  char q[PLACE_QUERY_RAW];
  if (placeQuery(query, q, sizeof(q)) == 0) {
    placeErrLine(PLACE_ERR_EMPTY, 0, NULL, why, whyCap);
    return false;
  }
  if (wifiState.radioOff() || WiFi.status() != WL_CONNECTED) {
    placeErrLine(PLACE_ERR_NOWIFI, 0, NULL, why, whyCap);
    return false;
  }
  /* Not queued behind another job: a search a person is waiting on behind a 150 s AI question
   * would make its 10 s budget a lie, and two handshakes never run at once anyway. */
  if (aiRequestActive() || wxRequestActive() || geoRequestActive()) {
    placeErrLine(PLACE_ERR_BUSY, 0, NULL, why, whyCap);
    return false;
  }
  const char* gate = gateWhy();
  if (gate) {
    snprintf(why, whyCap, "Place search: %s", gate);
    return false;
  }
  {
    uint32_t fr, lg;
    if (!netHeapOkForHandshake(&fr, &lg)) {
      placeErrLine(PLACE_ERR_NOMEM, 0, NULL, why, whyCap);
      return false;
    }
  }
  {
    char w[64];                                   // the worker's longest refusal is 51 bytes
    if (!netWorkerReady(w, sizeof(w))) {
      snprintf(why, whyCap, "Place search: %s", w);   // every gate's words start so
      return false;
    }
  }
  // The request: everything the worker reads, built now.
  if (placeBuildUrl(m->tmpl, q, refLat, refLon, PLACE_LIMIT, m->url, sizeof(m->url)) == 0) {
    strlcpy(why, "Place search: could not build the request", whyCap);
    return false;
  }
  strlcpy(m->query, q, sizeof(m->query));
  m->refWhat[0] = '\0';                           // the app's: placeConsoleSearch sets its own after
  m->refLat = refLat;
  m->refLon = refLon;
  m->haveRef = true;
  m->n = 0;
  m->parsed = -1;
  m->code = m->net = m->tlsErr = 0;
  m->dnsMs = m->hsMs = m->ms = m->bytes = 0;
  m->overflow = false;
  m->skipped[0] = '\0';
  m->errLine[0] = '\0';
  m->err = PLACE_ERR_NONE;
  m->haveRes = false;
  m->consolePending = false;
  /* A Search pauses the music, as a weather Refresh and an AI question do (a handshake beside a
   * decoding track failed on phone 1). Paused, not stopped: Music's Play carries on. */
  m->musicPaused = musicPlayerIsPlaying();
  if (m->musicPaused) {
    musicPlayerPause();
  }
  tlsInstallPsramHook();                          // BEFORE the first WiFiClientSecure (tile_fetch.h)
  m->state = PLACE_RUNNING;
  m->gen++;
  s_cancel = false;
  s_done = false;
  s_busy = true;
  netWorkerSubmit(NET_JOB_PLACE, placeJob);
  return true;
}

// ── what came back ───────────────────────────────────────────────────────────────────────

int placeResultCount() {
  /* n is zeroed at every start and on every error / cancel, so the count and placeResult()'s
   * pointers always agree - and a DONE list survives placeSearchClear() until the next start. */
  return s_m ? s_m->n : 0;
}

const PlaceResult* placeResult(int i) {
  if (!s_m || i < 0 || i >= s_m->n) {
    return NULL;
  }
  return &s_m->res[i];
}

const char* placeLastQuery() {
  return s_m ? s_m->query : "";
}

bool placeLastRef(double* lat, double* lon) {
  if (!s_m || !s_m->haveRef) {
    return false;
  }
  if (lat) *lat = s_m->refLat;
  if (lon) *lon = s_m->refLon;
  return true;
}

void placeSetLastPlace(const char* name, double lat, double lon) {
  if (!memOk()) {
    return;
  }
  strlcpy(s_m->lastName, name ? name : "", sizeof(s_m->lastName));
  s_m->lastLat = lat;
  s_m->lastLon = lon;
  s_m->haveLast = s_m->lastName[0] != '\0';
}

bool placeLastPlace(char* name, size_t cap, double* lat, double* lon) {
  if (!s_m || !s_m->haveLast) {
    return false;
  }
  if (name && cap) strlcpy(name, s_m->lastName, cap);
  if (lat) *lat = s_m->lastLat;
  if (lon) *lon = s_m->lastLon;
  return true;
}

void placeForgetLastPlace() {
  if (s_m) {
    s_m->haveLast = false;
    s_m->lastName[0] = '\0';
  }
}

// ── the console ──────────────────────────────────────────────────────────────────────────

/* The phone's own reference (app_maps.cpp's selfPosition, without the app): a live fix, the pin,
 * the newest fix of a source that is on (never an expired Wi-Fi one), the saved map view. */
static bool reference(double* lat, double* lon, char* what, size_t cap) {
  SelfFix fx;
  if (meshService.getSelfFix(&fx, false) != SELF_NONE) {
    *lat = mapI7ToDeg(fx.latI);
    *lon = mapI7ToDeg(fx.lonI);
    strlcpy(what, fx.src == SELF_NET ? "you (Wi-Fi)" : "you", cap);
    return true;
  }
  int32_t la = 0, lo = 0;
  if (meshService.getMyPin(&la, &lo, NULL)) {
    *lat = mapI7ToDeg(la);
    *lon = mapI7ToDeg(lo);
    strlcpy(what, "your pin", cap);
    return true;
  }
  if (meshService.getSelfNewest(&fx) != SELF_NONE && fx.src != SELF_NET) {
    *lat = mapI7ToDeg(fx.latI);
    *lon = mapI7ToDeg(fx.lonI);
    strlcpy(what, "your last fix", cap);
    return true;
  }
  Preferences p;
  bool saved = false;
  if (p.begin(PLACE_NVS, true)) {
    if (p.getInt("saved", 0)) {
      la = p.getInt("lat", 0);
      lo = p.getInt("lon", 0);
      saved = true;
    }
    p.end();
  }
  if (saved) {
    *lat = mapI7ToDeg(la);
    *lon = mapI7ToDeg(lo);
    strlcpy(what, "the saved map view", cap);
    return true;
  }
  return false;
}

bool placeConsoleSearch(const char* text, char* why, size_t whyCap) {
  if (!why || whyCap == 0) {
    return false;
  }
  double lat = 0, lon = 0;
  char what[24];
  if (!reference(&lat, &lon, what, sizeof(what))) {
    strlcpy(why, "no reference - no fix, no pin, no saved map view (`maps goto` sets one)", whyCap);
    return false;
  }
  if (!placeSearchStart(text, lat, lon, why, whyCap)) {
    return false;
  }
  s_m->consoleAsked = true;
  strlcpy(s_m->refWhat, what, sizeof(s_m->refWhat));
  char la[16], lo[16];
  placeFmtBias(lat, la, sizeof(la));
  placeFmtBias(lon, lo, sizeof(lo));
  /* The console's own echo: the query as sent and the ROUNDED point (what Photon gets). */
  snprintf(why, whyCap, "searching '%s' from %s (%s,%s) - the rows follow", s_m->query, what, la, lo);
  return true;
}

static void timingLine(const PlaceMem* m, char* l, size_t cap) {
  snprintf(l, cap, "  HTTP %d, %s, %u B%s, dns %u ms, connect+handshake %u ms, all %u ms%s",
           m->code, m->code > 0 ? "net ok" : netWord(m->net), (unsigned)m->bytes,
           m->overflow ? " (past the 64 KB buffer)" : "", (unsigned)m->dnsMs, (unsigned)m->hsMs,
           (unsigned)m->ms, m->tlsErr ? " (tls error)" : "");
}

void placeConsoleDrain(void (*emit)(const char* line)) {
  if (!s_m || !s_m->consolePending) {
    return;
  }
  PlaceMem* m = s_m;
  m->consolePending = false;
  char l[160];
  if (m->state == PLACE_DONE) {
    char head[PLACE_ROW_MAX + 32];
    placeHeader(m->query, m->n, head, sizeof(head));
    snprintf(l, sizeof(l), "maps search: %s (from %s)", head, m->refWhat);
    emit(l);
    /* The rows exactly as the list shows them: PSRAM for the moment it takes (8 x 160 bytes). */
    PlaceRow* rows = (PlaceRow*)heap_caps_malloc(sizeof(PlaceRow) * PLACE_LIMIT, MALLOC_CAP_SPIRAM);
    if (rows) {
      const int n = placeRows(m->res, m->n, m->refLat, m->refLon, gUnits, NULL, rows);
      for (int i = 0; i < n; i++) {
        snprintf(l, sizeof(l), "  %d. %s", i + 1, rows[i].text);
        emit(l);
        snprintf(l, sizeof(l), "     %s", rows[i].sub);
        emit(l);
      }
      free(rows);
    } else {
      emit("  (no memory for the rows)");
    }
  } else {
    snprintf(l, sizeof(l), "maps search: %s", m->errLine);
    emit(l);
  }
  if (m->code > 0 || m->net != GEM_NET_OK || m->ms) {
    timingLine(m, l, sizeof(l));
    emit(l);
  }
}

void placeReport(void (*emit)(const char* line)) {
  loadSettings();
  if (!s_m) {
    emit("maps search: no PSRAM for the search's state");
    return;
  }
  const PlaceMem* m = s_m;
  char l[192];
  snprintf(l, sizeof(l), "maps search: %s; endpoint %s%s (https, ISRG roots)", placeSettingRow(m->enabled),
           m->host, m->tmpl[0] ? " (custom: maps searchurl)" : " (built-in Photon)");
  emit(l);
  const int run = netWorkerRunning();
  snprintf(l, sizeof(l), "  state: %s", s_busy ? (run == NET_JOB_PLACE ? "running" : "queued")
                                        : s_done ? "done, folding next pass"
                                        : m->state == PLACE_DONE ? "done" : m->state == PLACE_ERROR ? "error" : "idle");
  emit(l);
  if (m->query[0]) {
    snprintf(l, sizeof(l), "  last query: '%s' from %s", m->query, m->refWhat[0] ? m->refWhat : "the app");
    emit(l);
  } else {
    emit("  last query: none this boot");
  }
  if (m->haveRes) {
    /* err, not the state: a cancel leaves PLACE_IDLE with its line ("Place search: cancelled"). */
    if (m->err != PLACE_ERR_NONE) {
      snprintf(l, sizeof(l), "  last result: %s", m->errLine);
    } else {
      snprintf(l, sizeof(l), "  last result: %d place(s)", m->n);
    }
    emit(l);
    timingLine(m, l, sizeof(l));
    emit(l);
  }
  if (m->haveLast) {
    snprintf(l, sizeof(l), "  last place chosen: %s", m->lastName);
    emit(l);
  }
}
