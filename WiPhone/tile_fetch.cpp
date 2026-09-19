/*
 * tile_fetch.cpp — see tile_fetch.h.
 */
#include "tile_fetch.h"
#include "tile_decode.h"
#include "map_tiles.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <math.h>

/* The fetch task's stack: internal RAM (IDF 3.3 cannot put a stack in PSRAM), allocated on
 * the first run and kept for the life of the firmware. Measured: a TLS handshake plus one
 * tile through decode and write used ~4.2 KB of it (tlstest, 2026-09-18). */
#define TF_STACK_BYTES   10240
#define TF_BODY_CAP      (96u * 1024u)     // one tile, with room: USGS/OTM are 13-55 KB
#define TF_WRITE_PIECE   (32u * 1024u)
#define TF_MAX_FAILS     12                // consecutive; then the run gives up
#define TF_RECONNECT_AT  3                 // consecutive; then drop and remake the socket

static StaticTask_t   s_tcb;
static StackType_t*   s_stack = NULL;
static volatile bool  s_busy  = false;     // a task (bench or job) is alive
static volatile bool  s_pause = false;
static volatile bool  s_stop  = false;
static bool           s_hookInstalled = false;

// ── sources ─────────────────────────────────────────────────────────────────────────────
static char s_customUrl[200] = "";

static const TileSource SOURCES[] = {
  { "usgs-topo", "USGS Topo",
    "https://basemap.nationalmap.gov/arcgis/rest/services/USGSTopo/MapServer/tile/{z}/{y}/{x}",
    50, 24, 16, "USGS National Map (public domain)" },
  { "usgs-img", "USGS Aerial",
    "https://basemap.nationalmap.gov/arcgis/rest/services/USGSImageryOnly/MapServer/tile/{z}/{y}/{x}",
    50, 28, 16, "USGS National Map (public domain)" },
  { "otm", "OpenTopoMap",
    "http://a.tile.opentopomap.org/{z}/{x}/{y}.png",
    600, 35, 17, "(c) OpenStreetMap contributors, SRTM | OpenTopoMap (CC-BY-SA)" },
  { "custom", "Custom (serial)", s_customUrl, 20, 24, 19, "" },
};

int tileSourceCount() {
  return s_customUrl[0] ? 4 : 3;
}

const TileSource* tileSource(int i) {
  if (i < 0 || i >= tileSourceCount()) {
    return NULL;
  }
  return &SOURCES[i];
}

void tileSetCustomUrl(const char* templ) {
  strlcpy(s_customUrl, templ ? templ : "", sizeof(s_customUrl));
}

/* Expand {z} {x} {y}. Returns false if the template does not fit. */
static bool expandUrl(const char* templ, int z, int x, int y, char* out, size_t cap) {
  size_t w = 0;
  for (const char* p = templ; *p; p++) {
    if (*p == '{' && p[1] && p[2] == '}') {
      int v;
      if (p[1] == 'z') v = z; else if (p[1] == 'x') v = x; else if (p[1] == 'y') v = y; else { return false; }
      const int n = snprintf(out + w, cap - w, "%d", v);
      if (n < 0 || (size_t)n >= cap - w) return false;
      w += (size_t)n;
      p += 2;
    } else {
      if (w + 1 >= cap) return false;
      out[w++] = *p;
    }
  }
  out[w] = '\0';
  return true;
}

// ── geometry ────────────────────────────────────────────────────────────────────────────
/* The +-radius square around the centre, in tile indices at zoom z. Same rule as COVEY's
 * tiles_for(): degrees of latitude per km, longitude scaled by cos(lat). x wraps, y clamps. */
static void tileRange(const TileJobSpec* s, int z, int* x0, int* x1, int* y0, int* y1) {
  const double dlat = s->radiusKm / 111.0;
  double c = cos(s->lat * M_PI / 180.0);
  if (c < 0.2) c = 0.2;
  const double dlon = s->radiusKm / (111.0 * c);
  double wx0, wy0, wx1, wy1;
  mapLatLonToWorld(mapClampLat(s->lat + dlat), mapWrapLon(s->lon - dlon), z, &wx0, &wy0);
  mapLatLonToWorld(mapClampLat(s->lat - dlat), mapWrapLon(s->lon + dlon), z, &wx1, &wy1);
  const int n = 1 << z;
  *x0 = (int)floor(wx0 / MAP_TILE_PX);
  *x1 = (int)floor(wx1 / MAP_TILE_PX);
  *y0 = (int)floor(wy0 / MAP_TILE_PX);
  *y1 = (int)floor(wy1 / MAP_TILE_PX);
  if (*x1 < *x0) *x1 += n;             // the square straddles the antimeridian
  if (*y0 < 0) *y0 = 0;
  if (*y1 > n - 1) *y1 = n - 1;
  if (*y1 < *y0) *y1 = *y0;
}

int tileFetchEstimate(const TileJobSpec* s, uint32_t* cardBytes, uint32_t* netBytes) {
  const TileSource* src = tileSource(s ? s->source : -1);
  if (!s || !src) {
    return 0;
  }
  int zMax = s->zMax;
  if (zMax > src->zMax) zMax = src->zMax;
  int total = 0;
  for (int z = TILE_ZOOM_BASE; z <= zMax; z++) {
    int x0, x1, y0, y1;
    tileRange(s, z, &x0, &x1, &y0, &y1);
    total += (x1 - x0 + 1) * (y1 - y0 + 1);
  }
  if (cardBytes) *cardBytes = (uint32_t)total * (uint32_t)MAP_TILE_BYTES;
  if (netBytes)  *netBytes  = (uint32_t)total * (uint32_t)src->kbPerTile * 1024u;
  return total;
}

// ── the job ─────────────────────────────────────────────────────────────────────────────
struct Job {
  TileJobSpec    spec;
  const TileSource* src;
  int            zMax;
  TileJobStatus  st;
  uint32_t       startMs;
  int            consecutiveFails;
};
static Job* s_job = NULL;                  // PSRAM

static void heapNow(uint32_t* freeB, uint32_t* largest, uint32_t* minEver) {
  multi_heap_info_t h;
  heap_caps_get_info(&h, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (freeB)   *freeB   = h.total_free_bytes;
  if (largest) *largest = h.largest_free_block;
  if (minEver) *minEver = h.minimum_free_bytes;
}

/* mbedTLS's allocator, pointed at PSRAM. calloc semantics (zeroed). Falls back to the
 * internal heap only if PSRAM itself is exhausted, which on this phone means something else
 * is already badly wrong. */
static void* tfCalloc(size_t n, size_t sz) {
  void* p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM);
  return p ? p : calloc(n, sz);
}
static void tfFree(void* p) {
  free(p);                                  // IDF's free() takes a block from any heap
}
static void installHook() {
  if (!s_hookInstalled) {
    mbedtls_platform_set_calloc_free(tfCalloc, tfFree);
    s_hookInstalled = true;
  }
}

static bool ensureStack(char* why, size_t whyCap) {
  if (!s_stack) {
    s_stack = (StackType_t*)heap_caps_malloc(TF_STACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_stack) {
      snprintf(why, whyCap, "no internal RAM for the download task");
      return false;
    }
  }
  return true;
}

/* One HTTP GET into `body`. Returns the HTTP code (200 = *got bytes are the tile), 0 for a
 * transport failure. `http`/`client` are the kept-alive pair for the whole run. */
static int fetchOne(HTTPClient& http, WiFiClient& client, const char* url,
                    uint8_t* body, size_t cap, size_t* got, char* err, size_t errCap) {
  *got = 0;
  if (!http.begin(client, url)) {
    strlcpy(err, "bad URL", errCap);
    return 0;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    if (code < 0) {
      snprintf(err, errCap, "%s", http.errorToString(code).c_str());
    } else {
      snprintf(err, errCap, "HTTP %d", code);
    }
    http.end();
    return code < 0 ? 0 : code;
  }
  const int   len = http.getSize();
  WiFiClient* s   = http.getStreamPtr();
  size_t   n    = 0;
  uint32_t idle = millis();
  bool     stalled = false;
  while (http.connected() && (len < 0 || n < (size_t)len)) {
    const size_t avail = s->available();
    if (avail) {
      size_t want = avail;
      if (want > cap - n) want = cap - n;
      const int r = s->read(body + n, want);
      if (r > 0) {
        n += (size_t)r;
        idle = millis();
      }
      if (n >= cap) break;               // bigger than any tile: stop reading, sniff will refuse
    } else if (millis() - idle > 8000) {
      stalled = true;
      break;
    } else {
      vTaskDelay(1);                     // never spin: the loop task shares this core
    }
  }
  http.end();                            // reuse=true: the socket stays if the server allows
  if (stalled || (len > 0 && n < (size_t)len)) {
    strlcpy(err, stalled ? "body stalled" : "body cut short", errCap);
    return 0;
  }
  *got = n;
  return 200;
}

/* Write the decoded tile as <dir>/<y>.tmp then rename it into place. False = nothing usable
 * was left behind (the .tmp is removed). */
static bool writeTile(const char* path, const char* tmpPath, const uint16_t* px, char* err, size_t errCap) {
  if (SD.exists(tmpPath)) {
    SD.remove(tmpPath);              // exists() first: SD.remove() logs an error line for a missing file
  }
  File f = SD.open(tmpPath, FILE_WRITE);
  if (!f) {
    strlcpy(err, "cannot create the tile file", errCap);
    return false;
  }
  const uint8_t* b = (const uint8_t*)px;
  size_t w = 0;
  while (w < (size_t)MAP_TILE_BYTES) {
    size_t piece = (size_t)MAP_TILE_BYTES - w;
    if (piece > TF_WRITE_PIECE) piece = TF_WRITE_PIECE;
    const size_t r = f.write(b + w, piece);
    if (r != piece) {
      f.close();
      SD.remove(tmpPath);
      strlcpy(err, "card refused the write", errCap);
      return false;
    }
    w += r;
    vTaskDelay(1);                       // let a screen push in between pieces
  }
  f.close();
  File chk = SD.open(tmpPath);
  const size_t size = chk ? (size_t)chk.size() : 0;
  if (chk) chk.close();
  if (size != (size_t)MAP_TILE_BYTES) {
    SD.remove(tmpPath);
    strlcpy(err, "tile file came back short", errCap);
    return false;
  }
  if (SD.exists(path)) {
    SD.remove(path);                     // a wrong-length leftover would make rename fail
  }
  if (!SD.rename(tmpPath, path)) {
    SD.remove(tmpPath);
    strlcpy(err, "rename failed", errCap);
    return false;
  }
  return true;
}

static bool tileOnCard(const char* path) {
  File f = SD.open(path);
  if (!f) {
    return false;
  }
  const size_t size = (size_t)f.size();
  f.close();
  return size == (size_t)MAP_TILE_BYTES;
}

static void mkdirChain(const char* dirPath) {
  /* /maps, /maps/<key>, /maps/<key>/<z>, /maps/<key>/<z>/<x> — mkdir is one level at a time
   * and false for an existing folder, which is the common case. */
  char tmp[96];
  strlcpy(tmp, dirPath, sizeof(tmp));
  for (char* p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      SD.mkdir(tmp);
      *p = '/';
    }
  }
  SD.mkdir(tmp);
}

static void jobTask(void* arg) {
  Job* j = (Job*)arg;
  TileJobStatus* st = &j->st;
  j->startMs = millis();
  uint32_t floorLargest = 0xFFFFFFFFu;

  uint8_t*  body = (uint8_t*)heap_caps_malloc(TF_BODY_CAP, MALLOC_CAP_SPIRAM);
  uint16_t* px   = (uint16_t*)heap_caps_malloc(MAP_TILE_BYTES, MALLOC_CAP_SPIRAM);
  if (!body || !px) {
    strlcpy(st->lastErr, "no PSRAM for the tile buffers", sizeof(st->lastErr));
    free(body);
    free(px);
    st->finished = true;
    st->active = false;
    s_busy = false;
    vTaskDelete(NULL);
    return;
  }

  const bool https = !strncmp(j->src->url, "https", 5);
  WiFiClientSecure* secure = NULL;
  WiFiClient*       plain  = NULL;
  WiFiClient*       client = NULL;
  if (https) {
    secure = new WiFiClientSecure();
    secure->setInsecure();               // the same trust model as the uploader's /fetch
    client = secure;
  } else {
    plain = new WiFiClient();
    client = plain;
  }
  HTTPClient http;
  http.setReuse(true);
  http.setTimeout(15000);
  http.setUserAgent("WiPhone-maps/0.1 (wiphone-meshtastic)");

  char url[300], dir[96], path[112], tmpPath[112], err[64];
  char lastDir[96] = "";
  for (int z = TILE_ZOOM_BASE; z <= j->zMax && !s_stop; z++) {
    st->curZ = z;
    int x0, x1, y0, y1;
    tileRange(&j->spec, z, &x0, &x1, &y0, &y1);
    const int n = 1 << z;
    for (int xi = x0; xi <= x1 && !s_stop; xi++) {
      const int x = ((xi % n) + n) % n;
      snprintf(dir, sizeof(dir), "%s/%s/%d/%d", TILE_MAPS_ROOT, j->src->key, z, x);
      if (strcmp(dir, lastDir) != 0) {
        mkdirChain(dir);
        strlcpy(lastDir, dir, sizeof(lastDir));
      }
      for (int y = y0; y <= y1 && !s_stop; y++) {
        while (s_pause && !s_stop) {
          st->paused = true;
          vTaskDelay(pdMS_TO_TICKS(200));
        }
        st->paused = false;
        if (s_stop) break;
        snprintf(path, sizeof(path), "%s/%d.565", dir, y);
        snprintf(tmpPath, sizeof(tmpPath), "%s/%d.tmp", dir, y);
        if (tileOnCard(path)) {
          st->skipped++;
          continue;
        }
        if (!expandUrl(j->src->url, z, x, y, url, sizeof(url))) {
          strlcpy(st->lastErr, "URL template too long", sizeof(st->lastErr));
          st->failed++;
          continue;
        }
        size_t got = 0;
        err[0] = '\0';
        const int code = fetchOne(http, *client, url, body, TF_BODY_CAP, &got, err, sizeof(err));
        if (code == 404) {
          st->noTile++;                  // the server has nothing there: not a failure
          j->consecutiveFails = 0;
        } else if (code != 200) {
          st->failed++;
          snprintf(st->lastErr, sizeof(st->lastErr), "z%d %d/%d: %s", z, x, y, err);
          if (++j->consecutiveFails >= TF_MAX_FAILS) {
            strlcpy(st->lastErr, "network gave up - stopped", sizeof(st->lastErr));
            s_stop = true;
            break;
          }
          if (j->consecutiveFails % TF_RECONNECT_AT == 0) {
            client->stop();              // remake the socket (and the TLS session) next time
          }
          vTaskDelay(pdMS_TO_TICKS(500));
          continue;
        } else {
          j->consecutiveFails = 0;
          st->bytes += (uint32_t)got;
          if (!tileDecode(body, got, px, err, sizeof(err))) {
            st->failed++;
            snprintf(st->lastErr, sizeof(st->lastErr), "z%d %d/%d: %s", z, x, y, err);
          } else if (!writeTile(path, tmpPath, px, err, sizeof(err))) {
            st->failed++;
            snprintf(st->lastErr, sizeof(st->lastErr), "z%d %d/%d: %s", z, x, y, err);
          } else {
            st->done++;
          }
        }
        uint32_t f, l, m;
        heapNow(&f, &l, &m);
        if (l < floorLargest) floorLargest = l;
        st->elapsedMs = millis() - j->startMs;
        vTaskDelay(pdMS_TO_TICKS(j->src->delayMs > 0 ? j->src->delayMs : 1));
      }
    }
  }
  http.end();
  if (secure) {
    secure->stop();
    delete secure;
  }
  if (plain) {
    plain->stop();
    delete plain;
  }
  free(body);
  free(px);
  st->heapFloorLargest = (floorLargest == 0xFFFFFFFFu) ? 0 : floorLargest;
  {
    uint32_t f, l, m;
    heapNow(&f, &l, &m);
    st->heapMinEver = m;
  }
  st->stackFloor = (uint32_t)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
  st->elapsedMs = millis() - j->startMs;
  st->stopping = false;
  st->paused = false;
  st->finished = true;
  st->active = false;
  s_busy = false;
  vTaskDelete(NULL);
}

bool tileFetchStart(const TileJobSpec* s, char* why, size_t whyCap) {
  if (!why || whyCap == 0) {
    return false;
  }
  why[0] = '\0';
  if (s_busy) {
    strlcpy(why, "a download is already running", whyCap);
    return false;
  }
  const TileSource* src = tileSource(s ? s->source : -1);
  if (!src) {
    strlcpy(why, "no such source", whyCap);
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    strlcpy(why, "not on WiFi", whyCap);
    return false;
  }
  /* The start gate. Measured 2026-09-18: the first TLS handshake of a run dips the internal
   * heap by ~12 KB on top of the task's stack (10 KB, once), and a run that starts from a
   * heap already fragmented by a long session has reached a min-ever under 3 KB — the zone
   * where SIP cannot re-register and lwIP's listener dies. Refuse to begin rather than find
   * out mid-handshake; the words say what to do. */
  {
    uint32_t f, l, m;
    heapNow(&f, &l, &m);
    const uint32_t need = s_stack ? 14000u : 24000u;
    if (l < need) {
      snprintf(why, whyCap, "phone low on memory (%u KB free block) - reboot first", (unsigned)(l / 1024));
      return false;
    }
  }
  if (!mapAreaNameOk(src->key)) {
    strlcpy(why, "source folder name refused", whyCap);
    return false;
  }
  if (!s_job) {
    s_job = (Job*)heap_caps_calloc(1, sizeof(Job), MALLOC_CAP_SPIRAM);
    if (!s_job) {
      strlcpy(why, "no PSRAM for the job", whyCap);
      return false;
    }
  }
  memset(s_job, 0, sizeof(Job));
  s_job->spec = *s;
  s_job->src = src;
  s_job->zMax = s->zMax > src->zMax ? src->zMax : s->zMax;
  if (s_job->zMax < TILE_ZOOM_BASE) s_job->zMax = TILE_ZOOM_BASE;
  s_job->st.total = tileFetchEstimate(s, NULL, NULL);
  strlcpy(s_job->st.source, src->key, sizeof(s_job->st.source));
  if (!ensureStack(why, whyCap)) {
    return false;
  }
  installHook();
  s_stop = false;
  s_pause = false;
  s_busy = true;
  s_job->st.active = true;
  TaskHandle_t h = xTaskCreateStaticPinnedToCore(jobTask, "tilefetch", TF_STACK_BYTES, s_job,
                                                 1, s_stack, &s_tcb, 1);
  if (!h) {
    s_busy = false;
    s_job->st.active = false;
    strlcpy(why, "could not create the download task", whyCap);
    return false;
  }
  return true;
}

void tileFetchStop() {
  if (s_busy && s_job) {
    s_job->st.stopping = true;
  }
  s_stop = true;
}

bool tileFetchActive() {
  return s_busy;
}

void tileFetchPause(bool on) {
  s_pause = on;
}

void tileFetchStatus(TileJobStatus* out) {
  if (!out) {
    return;
  }
  if (!s_job) {
    memset(out, 0, sizeof(*out));
    return;
  }
  *out = s_job->st;
  if (out->active) {
    out->elapsedMs = millis() - s_job->startMs;
  }
}

void tileFetchReport(void (*emit)(const char* line)) {
  char l[192];
  TileJobStatus st;
  tileFetchStatus(&st);
  if (!s_job) {
    emit("maps dl: nothing run yet");
    return;
  }
  snprintf(l, sizeof(l), "maps dl: %s %s%s%s | %d/%d done, %d skipped, %d no-tile, %d failed, z%d",
           st.source, st.active ? "RUNNING" : (st.finished ? "finished" : "idle"),
           st.paused ? " (paused)" : "", st.stopping ? " (stopping)" : "",
           st.done, st.total, st.skipped, st.noTile, st.failed, st.curZ);
  emit(l);
  snprintf(l, sizeof(l), "  %u KB down in %u s; internal largest floor %u, min-ever %u; task stack floor %u of %u%s%s",
           (unsigned)(st.bytes / 1024), (unsigned)(st.elapsedMs / 1000),
           (unsigned)st.heapFloorLargest, (unsigned)st.heapMinEver,
           (unsigned)st.stackFloor, (unsigned)TF_STACK_BYTES,
           st.lastErr[0] ? " | last: " : "", st.lastErr);
  emit(l);
}

// ── the bench ───────────────────────────────────────────────────────────────────────────
struct BenchJob {
  char     url[512];
  int      count;
  int      done, failed, wrongMagic;
  uint32_t bytes;
  uint32_t tFirstMs, tSumMs, tMinMs, tMaxMs;
  uint32_t freeBefore, largestBefore, minEverBefore;
  uint32_t freeAfter, largestAfter, minEverDuring;
  uint32_t psramBefore, psramAfter;
  uint32_t stackFloor;
  int      lastCode;
  char     lastErr[80];
  bool     finished;
};
static BenchJob* s_bench = NULL;

static void benchTask(void* arg) {
  BenchJob* j = (BenchJob*)arg;
  heapNow(&j->freeBefore, &j->largestBefore, &j->minEverBefore);
  j->psramBefore = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  j->tMinMs = 0xFFFFFFFFu;
  uint8_t* body = (uint8_t*)heap_caps_malloc(TF_BODY_CAP, MALLOC_CAP_SPIRAM);
  if (!body) {
    strlcpy(j->lastErr, "no PSRAM for the body", sizeof(j->lastErr));
    j->finished = true;
    s_busy = false;
    vTaskDelete(NULL);
    return;
  }
  const bool https = !strncmp(j->url, "https", 5);
  WiFiClientSecure* secure = NULL;
  WiFiClient*       plain  = NULL;
  WiFiClient*       client = NULL;
  if (https) {
    secure = new WiFiClientSecure();
    secure->setInsecure();
    client = secure;
  } else {
    plain = new WiFiClient();
    client = plain;
  }
  HTTPClient http;
  http.setReuse(true);
  http.setTimeout(15000);
  http.setUserAgent("WiPhone-maps/0.1 (wiphone-meshtastic)");
  uint32_t minDuring = 0xFFFFFFFFu;
  for (int i = 0; i < j->count; i++) {
    const uint32_t t0 = millis();
    size_t got = 0;
    char err[64];
    const int code = fetchOne(http, *client, j->url, body, TF_BODY_CAP, &got, err, sizeof(err));
    j->lastCode = code;
    if (code != 200) {
      snprintf(j->lastErr, sizeof(j->lastErr), "%s", err);
      j->failed++;
    } else {
      const uint32_t dt = millis() - t0;
      if (i == 0) j->tFirstMs = dt;
      j->tSumMs += dt;
      if (dt < j->tMinMs) j->tMinMs = dt;
      if (dt > j->tMaxMs) j->tMaxMs = dt;
      j->bytes += (uint32_t)got;
      j->done++;
      const TileFormat f = tileSniff(body, got);
      if (f != TILE_FMT_JPEG && f != TILE_FMT_PNG) j->wrongMagic++;
    }
    uint32_t f, l, m;
    heapNow(&f, &l, &m);
    if (l < minDuring) minDuring = l;
    vTaskDelay(1);
  }
  j->minEverDuring = minDuring;
  http.end();
  if (secure) { secure->stop(); delete secure; }
  if (plain)  { plain->stop();  delete plain; }
  free(body);
  j->stackFloor = (uint32_t)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
  heapNow(&j->freeAfter, &j->largestAfter, NULL);
  j->psramAfter = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  j->finished = true;
  s_busy = false;
  vTaskDelete(NULL);
}

bool tileFetchBenchStart(const char* url, int count) {
  if (s_busy) {
    return false;
  }
  if (!s_bench) {
    s_bench = (BenchJob*)heap_caps_calloc(1, sizeof(BenchJob), MALLOC_CAP_SPIRAM);
    if (!s_bench) {
      return false;
    }
  }
  memset(s_bench, 0, sizeof(BenchJob));
  strlcpy(s_bench->url, url, sizeof(s_bench->url));
  s_bench->count = count < 1 ? 1 : count;
  char why[64];
  if (!ensureStack(why, sizeof(why))) {
    strlcpy(s_bench->lastErr, why, sizeof(s_bench->lastErr));
    s_bench->finished = true;
    return false;
  }
  installHook();
  s_busy = true;
  TaskHandle_t h = xTaskCreateStaticPinnedToCore(benchTask, "tilebench", TF_STACK_BYTES, s_bench,
                                                 1, s_stack, &s_tcb, 1);
  if (!h) {
    s_busy = false;
    strlcpy(s_bench->lastErr, "xTaskCreateStatic failed", sizeof(s_bench->lastErr));
    s_bench->finished = true;
    return false;
  }
  return true;
}

void tileFetchBenchReport(void (*emit)(const char* line)) {
  char l[192];
  if (!s_bench) {
    emit("tlstest: nothing run yet. tlstest <url> [n]");
    return;
  }
  const BenchJob* j = s_bench;
  if (!j->finished) {
    snprintf(l, sizeof(l), "tlstest: RUNNING %d/%d done, %d failed, %u bytes so far",
             j->done, j->count, j->failed, (unsigned)j->bytes);
    emit(l);
    return;
  }
  const uint32_t avg = j->done ? j->tSumMs / (uint32_t)j->done : 0;
  snprintf(l, sizeof(l), "tlstest: %s", j->url);
  emit(l);
  snprintf(l, sizeof(l), "  %d/%d ok, %d failed, %d wrong-magic, %u bytes; last code %d %s",
           j->done, j->count, j->failed, j->wrongMagic, (unsigned)j->bytes, j->lastCode,
           j->lastErr[0] ? j->lastErr : "");
  emit(l);
  snprintf(l, sizeof(l), "  first GET %u ms (connect+handshake), then avg %u / min %u / max %u ms",
           (unsigned)j->tFirstMs, (unsigned)avg,
           (unsigned)(j->tMinMs == 0xFFFFFFFFu ? 0 : j->tMinMs), (unsigned)j->tMaxMs);
  emit(l);
  snprintf(l, sizeof(l), "  internal before free=%u largest=%u min-ever=%u",
           (unsigned)j->freeBefore, (unsigned)j->largestBefore, (unsigned)j->minEverBefore);
  emit(l);
  snprintf(l, sizeof(l), "  internal floor during: largest=%u | after free=%u largest=%u",
           (unsigned)(j->minEverDuring == 0xFFFFFFFFu ? 0 : j->minEverDuring),
           (unsigned)j->freeAfter, (unsigned)j->largestAfter);
  emit(l);
  snprintf(l, sizeof(l), "  psram before=%u after=%u (delta %d) | task stack floor %u of %u bytes",
           (unsigned)j->psramBefore, (unsigned)j->psramAfter,
           (int)j->psramAfter - (int)j->psramBefore,
           (unsigned)j->stackFloor, (unsigned)TF_STACK_BYTES);
  emit(l);
}
