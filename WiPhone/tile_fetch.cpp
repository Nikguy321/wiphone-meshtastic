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
#include <freertos/semphr.h>
#include <string.h>
#include <math.h>

/* The fetch task's stack: internal RAM (IDF 3.3 cannot put a stack in PSRAM), allocated on
 * the first run and kept for the life of the firmware. Measured over four runs (tlstest and
 * three areas, 2026-09-18/19): the deepest path — a TLS handshake, then a tile through decode
 * and the card — left a floor of 4,996-6,264 bytes of 10,240, i.e. ~5.2 KB used. 8 KB keeps
 * a 2.8 KB margin and gives 2 KB of internal heap back to the download itself, which is
 * where the margin is thinner (the handshake's ~12 KB of lwIP buffers). `maps dl` prints
 * the floor after every run: if it ever reads under ~1,500, put this back up. */
#define TF_STACK_BYTES   8192
#define TF_BODY_CAP      (96u * 1024u)     // one tile, with room: USGS/OTM are 13-55 KB
#define TF_WRITE_PIECE   (32u * 1024u)
#define TF_MAX_FAILS     12                // consecutive; then the run gives up
#define TF_CONNECT_LARGEST 10000u          // internal largest block a TLS handshake may start with (phone 1 with SIP up: ~11.4 KB after the stack)
#define TF_CONNECT_FREE    14000u          // ...and total internal free (phone 2 fresh boot after the stack ~17.7 KB, phone 1 ~17.1 KB)
#define TF_CONNECT_WAIT_MS 30000u          // how long to wait for that room before failing the tile
#define TF_PAUSE_MAX_MS  (10u * 60u * 1000u) // a job paused for a CALL this long gives up
#define TF_NOWIFI_MAX_MS (20u * 1000u)     // a job without WiFi this long gives up (and frees its RAM)
#define TF_RECONNECT_AT  3                 // consecutive; then drop and remake the socket
#define TF_DRAIN_CAP     (16u * 1024u)     // how much of an error body to read before giving up on the socket

/* ONE WORKER, CREATED ON THE FIRST RUN, NEVER DELETED. Three shapes were tried on hardware
 * (2026-09-19), and this is the one that survived phone 1:
 *   1. A STATIC task per run that deletes itself — races its own TCB: the task clears "busy"
 *      a few instructions before vTaskDelete and the loop task (same core, same priority) can
 *      re-create on the same TCB while the old one is still on a ready list; and
 *      eTaskGetState() on this FreeRTOS (8.2) reads a cleaned-up static task as eReady, so a
 *      liveness check refused every second run.
 *   2. A DYNAMIC task per run (heap stack, self-delete, idle task frees it) — the tidy answer
 *      on paper, and on phone 1 it left the internal heap FRAGMENTED: 26.3 KB largest before
 *      a run, 11.3 KB twenty minutes after, with the free total recovered. Whatever was
 *      allocated during the run (WiFi rejoined mid-run, SIP re-registered) landed inside the
 *      region the stack vacated, and "largest" is the number that predicts this phone's
 *      crashes. The superloop also never blocks for long, so the core-1 idle task that does
 *      the freeing is not guaranteed to run promptly.
 *   3. This: one worker, its 8 KB of internal stack carved once from a steady heap on the
 *      first download and held. The cost is fixed and visible — phone 1 measured five minutes
 *      after a run: free 17.3 KB / largest 12.3 KB, from 25.8 / 20.5 KB before — instead of a
 *      number that depends on what else happened during the run. `maps dl` prints it. */
static StaticTask_t      s_tcb;
static StackType_t*      s_stack = NULL;
static TaskHandle_t      s_task  = NULL;
static SemaphoreHandle_t s_go    = NULL;   // given once per run; the worker takes it
static volatile int      s_kind  = 0;      // 1 = job, 2 = bench
static volatile bool     s_busy  = false;  // a run is queued or in progress
static volatile bool  s_pause = false;     // a call: wait it out (up to TF_PAUSE_MAX_MS)
static volatile bool  s_noWifi = false;    // the network is gone: let go within TF_NOWIFI_MAX_MS
static volatile bool  s_stop  = false;
static bool           s_hookInstalled = false;

// ── sources ─────────────────────────────────────────────────────────────────────────────
static char s_customUrl[200] = "";
static bool customSourceBusy();           // below: is a running job reading s_customUrl?

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

bool tileSetCustomUrl(const char* templ, char* why, size_t whyCap) {
  if (why && whyCap) why[0] = '\0';
  if (!templ || !templ[0]) {
    if (customSourceBusy()) {
      if (why) strlcpy(why, "a download is using it - stop that first", whyCap);
      return false;
    }
    s_customUrl[0] = '\0';
    return true;
  }
  if (strlen(templ) >= sizeof(s_customUrl)) {
    if (why) snprintf(why, whyCap, "template longer than %u characters", (unsigned)(sizeof(s_customUrl) - 1));
    return false;
  }
  if (!strstr(templ, "{z}") || !strstr(templ, "{x}") || !strstr(templ, "{y}")) {
    if (why) strlcpy(why, "template needs {z} {x} and {y}", whyCap);
    return false;
  }
  if (strncmp(templ, "http://", 7) != 0 && strncmp(templ, "https://", 8) != 0) {
    if (why) strlcpy(why, "template must start with http:// or https://", whyCap);
    return false;
  }
  if (customSourceBusy()) {
    if (why) strlcpy(why, "a download is using it - stop that first", whyCap);
    return false;
  }
  strlcpy(s_customUrl, templ, sizeof(s_customUrl));
  return true;
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
  if (!s || !src || s->radiusKm <= 0) {
    return 0;
  }
  int zMax = s->zMax;
  if (zMax > src->zMax) zMax = src->zMax;
  int64_t total = 0;
  for (int z = TILE_ZOOM_BASE; z <= zMax; z++) {
    int x0, x1, y0, y1;
    tileRange(s, z, &x0, &x1, &y0, &y1);
    total += (int64_t)(x1 - x0 + 1) * (int64_t)(y1 - y0 + 1);
  }
  if (total > 1000000) total = 1000000;      // the caller refuses far below this; keep the bytes sane
  if (cardBytes) *cardBytes = (uint32_t)(total * (int64_t)MAP_TILE_BYTES > 0xFFFFFFFFll ? 0xFFFFFFFFu : total * (int64_t)MAP_TILE_BYTES);
  if (netBytes)  *netBytes  = (uint32_t)(total * (int64_t)src->kbPerTile * 1024);
  return (int)total;
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

static bool customSourceBusy() {
  return s_busy && s_job && s_job->src == &SOURCES[TILE_SRC_CUSTOM];
}

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
  /* PSRAM or nothing. A fallback to the internal heap would make a PSRAM shortage (3.4 MB
   * free — it has never happened) into an internal-heap raid by TLS, which is the one thing
   * this whole file exists to prevent. A NULL here fails the handshake, which is reported. */
  return heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM);
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
  const int len  = http.getSize();       // -1 = no Content-Length
  WiFiClient* s  = http.getStreamPtr();
  if (code != HTTP_CODE_OK) {
    if (code < 0) {
      snprintf(err, errCap, "%s", http.errorToString(code).c_str());
      http.end();
      client.stop();                     // a transport error leaves nothing worth keeping
      return 0;
    }
    snprintf(err, errCap, "HTTP %d", code);
    /* ⚠ THE SOCKET IS KEPT ALIVE, SO THE ERROR BODY MUST BE READ OR THE NEXT TILE STARTS
     * IN THE MIDDLE OF IT. HTTPClient::end() drains only what has ARRIVED; a 404 page whose
     * tail is still in flight would be parsed as the next response and burn two tiles.
     * Read it out (bounded), and if it cannot be read completely, drop the connection. */
    bool clean = false;
    if (len >= 0 && (size_t)len <= TF_DRAIN_CAP) {
      size_t n = 0;
      uint32_t idle = millis();
      while (n < (size_t)len && http.connected() && millis() - idle < 3000) {
        const size_t avail = s->available();
        if (avail) {
          size_t want = avail;
          if (want > cap) want = cap;
          const int r = s->read(body, want);
          if (r > 0) {
            n += (size_t)r;
            idle = millis();
          }
        } else {
          vTaskDelay(1);
        }
      }
      clean = (n >= (size_t)len);
    }
    http.end();
    if (!clean) {
      client.stop();
    }
    return code;
  }
  if (len < 0) {
    /* No Content-Length: a chunked or close-delimited body. HTTPClient's stream pointer gives
     * the raw bytes and no way to know where the tile ends on a kept-alive socket; every
     * such tile would sit out the 8 s stall and fail. Say so, once per tile, and drop the
     * socket so the next GET starts clean. (USGS and OpenTopoMap both send a length.) */
    strlcpy(err, "no Content-Length (chunked?)", errCap);
    http.end();
    client.stop();
    return 0;
  }
  size_t   n    = 0;
  uint32_t idle = millis();
  bool     stalled = false;
  bool     overCap = false;
  while (http.connected() && n < (size_t)len) {
    const size_t avail = s->available();
    if (avail) {
      size_t want = avail;
      if (want > cap - n) want = cap - n;
      const int r = s->read(body + n, want);
      if (r > 0) {
        n += (size_t)r;
        idle = millis();
      }
      if (n >= cap && n < (size_t)len) {   // bigger than any tile
        overCap = true;
        break;
      }
    } else if (millis() - idle > 8000) {
      stalled = true;
      break;
    } else {
      vTaskDelay(1);                     // never spin: the loop task shares this core
    }
  }
  http.end();                            // reuse=true: the socket stays if the server allows
  if (stalled || overCap || n < (size_t)len) {
    /* Whatever is left of the body is still in the socket; a kept-alive next GET would read
     * it as the next response. Drop the connection — the reconnect is the cheaper mistake. */
    client.stop();
    strlcpy(err, stalled ? "body stalled" : (overCap ? "tile larger than 96 KB" : "body cut short"), errCap);
    return 0;
  }
  *got = n;
  return 200;
}

/* Bytes free on the card, or 0xFFFFFFFF if the card cannot say. FatFs trusts FSINFO here
 * (FF_FS_NOFSINFO=0), so this is one read, not a walk. */
static uint64_t cardFreeBytes() {
  const uint64_t total = SD.totalBytes();
  const uint64_t used  = SD.usedBytes();
  if (total == 0) {
    return 0xFFFFFFFFFFFFFFFFull;
  }
  return total > used ? total - used : 0;
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

/* Wait out a pause (a call, no WiFi) and, when a new connection is about to be made, wait
 * for the internal RAM a TLS handshake needs. Returns false when the tile should be given up
 * on: the run was stopped, the pause outlived TF_PAUSE_MAX_MS (the run stops), or the RAM
 * never came (this tile fails, the run continues). `needRoom` is whether a handshake is due. */
static bool waitReady(Job* j, bool needRoom, WiFiClient* client) {
  TileJobStatus* st = &j->st;
  uint32_t pausedMs = 0, noWifiMs = 0, roomMs = 0;
  bool dropped = false;
  for (;;) {
    if (s_stop) {
      return false;
    }
    if (s_pause || s_noWifi) {
      st->paused = true;
      st->waitingRam = false;
      /* 🛑 LET GO OF THE CONNECTION WHILE PAUSED. Measured on phone 1 (2026-09-19): WiFi
       * dropped during a run's first handshake, the job sat paused holding its TLS session,
       * lwIP buffers and client objects, and the phone's WiFi RESCUE — the scan and rejoin,
       * which need internal heap of their own — had to run against largest = 8,984 bytes.
       * The socket and the TLS state are the part that can be given back at once. */
      if (client && !dropped) {
        client->stop();
        dropped = true;
      }
      if (s_noWifi) {
        /* No network: nothing a wait can fix that the phone's own rescue is not already
         * trying, and the rescue wants the RAM this job holds. Twenty seconds covers a blip;
         * past that the run ends, and a restart skips every tile already written. */
        noWifiMs += 200;
        if (noWifiMs >= TF_NOWIFI_MAX_MS) {
          strlcpy(st->lastErr, "WiFi dropped - stopped; start again to continue", sizeof(st->lastErr));
          s_stop = true;
          st->paused = false;
          return false;
        }
      } else {
        pausedMs += 200;
        if (pausedMs >= TF_PAUSE_MAX_MS) {
          strlcpy(st->lastErr, "paused too long (a call) - stopped", sizeof(st->lastErr));
          s_stop = true;
          st->paused = false;
          return false;
        }
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    st->paused = false;
    if (!needRoom) {
      st->waitingRam = false;
      return true;
    }
    uint32_t f, l, m;
    heapNow(&f, &l, &m);
    st->ramFree = f;
    st->ramLargest = l;
    st->ramNeedFree = TF_CONNECT_FREE;
    st->ramNeedLargest = TF_CONNECT_LARGEST;
    if (l >= TF_CONNECT_LARGEST && f >= TF_CONNECT_FREE) {
      st->waitingRam = false;
      return true;
    }
    if (roomMs >= TF_CONNECT_WAIT_MS) {
      st->waitingRam = false;
      return false;                       // this tile fails; the caller counts it
    }
    st->waitingRam = true;
    vTaskDelay(pdMS_TO_TICKS(250));
    roomMs += 250;
  }
}

/* "no RAM for a new connection", with the numbers: the two the bar is made of, so the screen
 * and the console say what is short by how much rather than only that something is. Phone 1
 * sat at 13.6 KB free under the 14 KB bar for two hours saying "waiting for memory"
 * (2026-09-21); this is what it should have said. */
static void ramShortMsg(char* out, size_t cap, const TileJobStatus* st, const char* prefix) {
  snprintf(out, cap, "%sno RAM for a connection: %u.%u of %u KB free, %u.%u of %u KB in one block",
           prefix, (unsigned)(st->ramFree / 1024), (unsigned)((st->ramFree % 1024) / 103),
           (unsigned)(st->ramNeedFree / 1024),
           (unsigned)(st->ramLargest / 1024), (unsigned)((st->ramLargest % 1024) / 103),
           (unsigned)(st->ramNeedLargest / 1024));
}

/* The whole run. A function of its own so that everything with a destructor — HTTPClient
 * and its Strings, the clients — is torn down by a normal return BEFORE the task deletes
 * itself: vTaskDelete(NULL) never returns, so locals of the task function itself would leak
 * their internal-heap buffers on every run. */
static void runJob(Job* j) {
  TileJobStatus* st = &j->st;
  uint32_t floorLargest = 0xFFFFFFFFu;

  uint8_t*  body = (uint8_t*)heap_caps_malloc(TF_BODY_CAP, MALLOC_CAP_SPIRAM);
  uint16_t* px   = (uint16_t*)heap_caps_malloc(MAP_TILE_BYTES, MALLOC_CAP_SPIRAM);
  if (!body || !px) {
    strlcpy(st->lastErr, "no PSRAM for the tile buffers", sizeof(st->lastErr));
    free(body);
    free(px);
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
        /* 🛑 A NEW TLS HANDSHAKE COSTS ~12 KB OF INTERNAL RAM FOR A SECOND, AND THE SERVER
         * CLOSES A KEPT-ALIVE CONNECTION EVERY SO MANY REQUESTS. Measured 2026-09-19: a 10 km
         * run with the map open took the internal heap's min-ever to 952 bytes — the
         * reconnect landed on top of the app's own use. So before any GET that will have to
         * reconnect, wait for room (and wait out a call or a WiFi drop, which is the same
         * loop): a handshake that would start below the bar is a reboot with extra steps. */
        const bool reconnecting = !client->connected() || s_pause || s_noWifi;
        if (!waitReady(j, reconnecting, client)) {
          if (s_stop) {
            break;
          }
          st->failed++;
          ramShortMsg(st->lastErr, sizeof(st->lastErr), st, "");
          /* 🛑 THE RAM IS NOT COMING. A tile that failed for want of a connection is not a
           * bad tile, and the next one meets the same heap: the old rule failed twelve of
           * them thirty seconds apart, six minutes of "waiting for memory" for the same
           * answer, and then stopped without saying what the numbers were. Three in a row
           * is the same fact three times: stop, and say how short by how much. */
          if (++j->consecutiveFails >= 3) {
            ramShortMsg(st->lastErr, sizeof(st->lastErr), st, "stopped: ");
            s_stop = true;
            break;
          }
          continue;
        }
        if (reconnecting && (st->done + st->failed + st->noTile > 0)) {
          st->reconnects++;
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
          st->bytes += (uint32_t)got;
          /* ⚠ THE CONSECUTIVE-FAILURE COUNT IS RESET ON A WRITTEN TILE, NOT ON A 200. The
           * first cut reset it here, before decode and write ran, so a card that refused
           * every write (or a server answering 200 with pages) could never reach the
           * give-up threshold — the run fetched the whole area for nothing (review). */
          if (!tileDecode(body, got, px, err, sizeof(err))) {
            st->failed++;
            snprintf(st->lastErr, sizeof(st->lastErr), "z%d %d/%d: %s", z, x, y, err);
            if (++j->consecutiveFails >= TF_MAX_FAILS) {
              strlcpy(st->lastErr, "nothing decodes - stopped", sizeof(st->lastErr));
              s_stop = true;
              break;
            }
          } else {
            /* 🛑 A CARD THAT IS BUSY IS NOT A CARD THAT IS BROKEN. "card refused the write" is a
             * short f.write(): the SD layer gives a card 500 ms to come out of busy before a
             * block (sd_diskio.cpp sdWriteBytes) and some cards pause longer than that for
             * their own housekeeping. Phone 2's 12,853-tile run lost 4 tiles to it; phone 1's
             * card does it to ONE WRITE IN EIGHT (2026-09-21: 35 rescued on a second try, 7
             * lost, in the first 311 tiles) — and every tile lost had already crossed the
             * network. So the write is tried again after a pause that grows — 200 ms, then a
             * second, then three — long enough for any card's pause to end; the pixels are
             * still in PSRAM, nothing is fetched twice. Only a card that refuses for five
             * seconds counts the tile as failed. The retries are counted (cardRetries) and
             * shown, because they are the measure of the card. */
            static const uint16_t WRITE_WAIT_MS[] = { 200, 1000, 3000 };
            bool wrote = writeTile(path, tmpPath, px, err, sizeof(err));
            for (int t = 0; !wrote && !s_stop && t < 3; t++) {
              vTaskDelay(pdMS_TO_TICKS(WRITE_WAIT_MS[t]));
              wrote = writeTile(path, tmpPath, px, err, sizeof(err));
              if (wrote) {
                st->cardRetries++;
                log_e("TILES: z%d %d/%d written on try %d", z, x, y, t + 2);
              }
            }
            if (!wrote) {
              /* A card that refuses twice is not going to accept the next 800 tiles either: a
               * full, absent or pulled card counts the same way the network does, and the run
               * stops rather than downloading the whole area for nothing. */
              st->failed++;
              snprintf(st->lastErr, sizeof(st->lastErr), "z%d %d/%d: %s", z, x, y, err);
              if (++j->consecutiveFails >= TF_MAX_FAILS) {
                strlcpy(st->lastErr, "the card keeps refusing writes - stopped", sizeof(st->lastErr));
                s_stop = true;
                break;
              }
            } else {
              st->done++;
              j->consecutiveFails = 0;
            }
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
}

static void jobRun(Job* j) {
  TileJobStatus* st = &j->st;
  j->startMs = millis();
  runJob(j);                              // every destructor has run by here
  {
    uint32_t f, l, m;
    heapNow(&f, &l, &m);
    st->heapMinEver = m;
  }
  st->stackFloor = (uint32_t)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
  st->elapsedMs = millis() - j->startMs;
  st->stopping = false;
  st->paused = false;
  st->waitingRam = false;
  st->finished = true;
  st->active = false;
}

static void benchRun();                   // below, with the bench

static void workerTask(void*) {
  for (;;) {
    xSemaphoreTake(s_go, portMAX_DELAY);
    if (s_kind == 1) {
      jobRun(s_job);
    } else if (s_kind == 2) {
      benchRun();
    }
    s_kind = 0;
    s_busy = false;                       // back on the semaphore: nothing else runs on this stack
  }
}

/* The worker, its stack and its semaphore, made on first use and kept. */
static bool ensureWorker(char* why, size_t whyCap) {
  if (!s_stack) {
    s_stack = (StackType_t*)heap_caps_malloc(TF_STACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_stack) {
      strlcpy(why, "no internal RAM for the download task", whyCap);
      return false;
    }
  }
  if (!s_go) {
    s_go = xSemaphoreCreateBinary();
    if (!s_go) {
      strlcpy(why, "no RAM for the download semaphore", whyCap);
      return false;
    }
  }
  if (!s_task) {
    s_task = xTaskCreateStaticPinnedToCore(workerTask, "tilefetch", TF_STACK_BYTES, NULL,
                                           1, s_stack, &s_tcb, 1);
    if (!s_task) {
      strlcpy(why, "could not create the download task", whyCap);
      return false;
    }
  }
  return true;
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
  /* The spec comes from the console as well as the form: check it. A negative radius reads
   * as an antimeridian crossing to tileRange() and becomes the whole world. */
  if (!(s->lat >= -85.0 && s->lat <= 85.0) || !(s->lon >= -180.0 && s->lon <= 180.0)) {
    strlcpy(why, "centre out of range", whyCap);
    return false;
  }
  if (s->radiusKm < 1 || s->radiusKm > 50) {
    strlcpy(why, "radius must be 1-50 km", whyCap);
    return false;
  }
  if (s->zMax < TILE_ZOOM_BASE || s->zMax > 19) {
    snprintf(why, whyCap, "detail must be z%d-z19", TILE_ZOOM_BASE);
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
    /* The SAME bar the reconnect wait uses (waitReady: largest >= TF_CONNECT_LARGEST, free >=
     * TF_CONNECT_FREE), plus the stack if it has not been allocated yet — the stack is carved
     * out of the largest block and out of the free total, so both bars move up by its size.
     * ⚠ Calibrated on phone 2 first (largest ~26 KB idle) and it refused phone 1 outright: the
     * SIP phone idles at largest ~21.9 KB with a registration up, which is exactly enough. */
    uint32_t f, l, m;
    heapNow(&f, &l, &m);
    const uint32_t needL = TF_CONNECT_LARGEST + (s_stack ? 0u : (uint32_t)TF_STACK_BYTES);
    const uint32_t needF = TF_CONNECT_FREE + (s_stack ? 0u : (uint32_t)TF_STACK_BYTES);
    if (l < needL || f < needF) {
      snprintf(why, whyCap, "phone low on memory (%u KB block, %u KB free) - reboot first",
               (unsigned)(l / 1024), (unsigned)(f / 1024));
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
  /* Every refusal below happens BEFORE the job record is touched, so a refused Start leaves
   * the previous run's "Last run: ..." summary on the form instead of wiping it. */
  uint32_t cardBytes = 0;
  const int total = tileFetchEstimate(s, &cardBytes, NULL);
  if (total > 20000) {
    snprintf(why, whyCap, "%d tiles is too many for one run (20000 max)", total);
    return false;
  }
  if (SD.totalBytes() == 0) {
    strlcpy(why, "no SD card", whyCap);
    return false;
  }
  {
    /* Room on the card for the whole area, assuming none of it is there yet (a re-run over a
     * half-done area asks for more than it needs — the honest direction to be wrong in). */
    const uint64_t freeB = cardFreeBytes();
    if (freeB < (uint64_t)cardBytes + (4u << 20)) {
      snprintf(why, whyCap, "card has %u MB free, this area needs %u MB",
               (unsigned)(freeB >> 20), (unsigned)(cardBytes >> 20));
      return false;
    }
  }
  if (!ensureWorker(why, whyCap)) {
    return false;
  }
  memset(s_job, 0, sizeof(Job));
  s_job->spec = *s;
  s_job->src = src;
  s_job->zMax = s->zMax > src->zMax ? src->zMax : s->zMax;
  if (s_job->zMax < TILE_ZOOM_BASE) s_job->zMax = TILE_ZOOM_BASE;
  s_job->st.total = total;
  strlcpy(s_job->st.source, src->key, sizeof(s_job->st.source));
  installHook();
  s_stop = false;
  s_pause = false;
  s_noWifi = false;
  s_busy = true;
  s_job->st.active = true;
  s_kind = 1;
  xSemaphoreGive(s_go);
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

void tileFetchPause(bool call, bool noWifi) {
  s_pause = call;
  s_noWifi = noWifi;
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
  snprintf(l, sizeof(l), "maps dl: %s %s%s%s%s | %d/%d done, %d skipped, %d no-tile, %d failed, %d reconnects, z%d",
           st.source, st.active ? "RUNNING" : (st.finished ? "finished" : "idle"),
           st.paused ? " (paused)" : "", st.waitingRam ? " (waiting for RAM)" : "",
           st.stopping ? " (stopping)" : "",
           st.done, st.total, st.skipped, st.noTile, st.failed, st.reconnects, st.curZ);
  emit(l);
  if (st.waitingRam) {
    ramShortMsg(l, sizeof(l), &st, "  ");
    emit(l);
  }
  if (s_job) {
    snprintf(l, sizeof(l), "  job: source %d, centre %.5f %.5f, %d km, z%d-%d",
             s_job->spec.source, s_job->spec.lat, s_job->spec.lon, s_job->spec.radiusKm,
             TILE_ZOOM_BASE, s_job->spec.zMax);
    emit(l);
  }
  snprintf(l, sizeof(l), "  %u KB down in %u s; card busy %d time%s; internal largest floor %u, min-ever %u; task stack floor %u of %u%s%s",
           (unsigned)(st.bytes / 1024), (unsigned)(st.elapsedMs / 1000),
           st.cardRetries, st.cardRetries == 1 ? "" : "s",
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

static void runBench(BenchJob* j) {
  heapNow(&j->freeBefore, &j->largestBefore, &j->minEverBefore);
  j->psramBefore = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  j->tMinMs = 0xFFFFFFFFu;
  uint8_t* body = (uint8_t*)heap_caps_malloc(TF_BODY_CAP, MALLOC_CAP_SPIRAM);
  if (!body) {
    strlcpy(j->lastErr, "no PSRAM for the body", sizeof(j->lastErr));
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
}

static void benchRun() {
  BenchJob* j = s_bench;
  runBench(j);                            // HTTPClient and its Strings destructed by the return
  j->stackFloor = (uint32_t)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
  heapNow(&j->freeAfter, &j->largestAfter, NULL);
  j->psramAfter = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  j->finished = true;
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
  if (!ensureWorker(why, sizeof(why))) {
    strlcpy(s_bench->lastErr, why, sizeof(s_bench->lastErr));
    s_bench->finished = true;
    return false;
  }
  installHook();
  s_busy = true;
  s_kind = 2;
  xSemaphoreGive(s_go);
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
