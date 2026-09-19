/*
 * tile_fetch.h — pulling map tiles off the network onto the card, from a task of their own.
 *
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * WHY A TASK, AND WHY THE ALLOCATOR HOOK
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * Measured 2026-09-18 on WiPhone 2 (0.9.63): an HTTPS pull of one USGS tile through the
 * uploader's /fetch path FAILED in start_ssl_client() with -32512 (SSL - Memory allocation
 * failed), took the internal heap's min-ever from 19,860 to 2,676 bytes, and left the largest
 * block fragmented at 19,160 for the rest of the session. Two causes, both in the precompiled
 * framework and neither fixable from a sketch the ordinary way:
 *
 *   1. CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y — every mbedTLS allocation comes from INTERNAL RAM,
 *      and mbedtls_ssl_setup() wants two 16,717-byte record buffers before the handshake even
 *      starts. 33.4 KB against a phone whose largest free block idles at ~25 KB.
 *   2. The handshake runs on the CALLING task's stack, and the loop task has 8 KB total
 *      (CONFIG_ARDUINO_LOOP_STACK_SIZE, baked into sdkconfig.h) with a measured floor of a few
 *      hundred bytes. TLS from loop() is impossible independent of heap.
 *
 * The escape, MEASURED the same night (`tlstest`, 70 kept-alive HTTPS GETs of a USGS tile,
 * every one a JPEG): esp_config.h defines MBEDTLS_PLATFORM_MEMORY, so mbedtls_calloc is a
 * function POINTER and libmbedtls.a exports mbedtls_platform_set_calloc_free(). Install a
 * PSRAM calloc once and every mbedTLS object — record buffers, contexts, the parsed
 * certificate chain — lands in the 3.4 MB that is actually free; PSRAM came back to the byte
 * afterwards. It is global: nothing else in this firmware allocates through mbedTLS
 * (mesh_crypto and the hashes use stack contexts), hardware AES is CPU-fed (no DMA, so PSRAM
 * operands are fine) and MPI/SHA are software. The work runs on a task with its own INTERNAL
 * stack (IDF 3.3 cannot place task stacks in PSRAM), allocated ONCE and never freed — the Game
 * Boy's rule, app_gbc.cpp:138-160. The handshake used ~4.2 KB of it; 10 KB is the margin.
 * The price: ~10 KB of internal RAM for the life of the firmware from the first download,
 * a handshake of ~1.5 s once per area (keep-alive after that: ~250 ms per tile), and a
 * transient internal dip to ~4 KB during the handshake — which is why a download PAUSES for
 * a call (tileFetchPause) rather than sharing the heap with SIP audio.
 *
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * WHAT ONE TILE COSTS, AND WHERE
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * GET (kept-alive) -> body in PSRAM -> sniff -> decode to a PSRAM 256x256 RGB565 -> write
 * /maps/<source>/<z>/<x>/<y>.tmp in four 32 KB pieces -> SD.rename to .565. The rename is
 * what makes a power-off mid-tile harmless: the viewer never sees a wrong-length file, and
 * the next run's skip test (size == 131072, NOT existence) repairs any hole.
 *
 * The task writes the card itself. The SPI bus is serialised at the HAL (TFT_eSPI force-
 * enables SUPPORT_TRANSACTIONS on ESP32; SD wraps every sector in AcquireSPI) and FatFs is
 * built FF_FS_REENTRANT, so a tile write cannot interleave bytes with a screen push — it can
 * only DELAY one. A whole tile is written between two draws rather than a sector at a time
 * for exactly that reason.
 *
 * Internal RAM per tile: the WiFiClient's 1.4 KB RX buffer and HTTPClient's URL strings.
 * Everything else — body, decoded tile, decoder pools — is PSRAM.
 */
#ifndef TILE_FETCH_H
#define TILE_FETCH_H

#include <stdint.h>
#include <stddef.h>

// ── sources ─────────────────────────────────────────────────────────────────────────────
typedef struct {
  const char* key;        // the /maps/<key> folder; must pass mapAreaNameOk()
  const char* label;      // what the screen calls it
  const char* url;        // template with {z} {x} {y}
  int         delayMs;    // politeness pause after each tile
  int         kbPerTile;  // rough download size, for the estimate
  int         zMax;       // deepest zoom the server has
  const char* credit;     // the attribution the source asks for
} TileSource;

#define TILE_SRC_CUSTOM 3                 // the serial-settable template (a LAN relay)
int               tileSourceCount();      // the fixed ones, plus custom when it is set
const TileSource* tileSource(int i);      // NULL past the end
/* `maps dlurl <template>` — a plain-HTTP relay for USGS (COVEY's cache, a Mac) when TLS is
 * not wanted. Empty string clears it. */
void              tileSetCustomUrl(const char* templ);

// ── the job ─────────────────────────────────────────────────────────────────────────────
#define TILE_ZOOM_BASE 11                 // every area starts here, like COVEY's downloader
#define TILE_MAPS_ROOT "/maps"            // == MAPS_ROOT in app_maps.h; the viewer's folder

typedef struct {
  int    source;
  double lat, lon;        // centre
  int    radiusKm;        // the +-radius square around it
  int    zMax;            // TILE_ZOOM_BASE..zMax
} TileJobSpec;

typedef struct {
  bool     active;        // the task is running (or about to)
  bool     finished;      // a run completed (look at the counters)
  bool     paused;        // held by tileFetchPause(true): a call, or WiFi is down
  bool     waitingRam;    // a new connection is due and the internal heap has no room for it yet
  bool     stopping;      // stop requested, finishing the current tile
  int      total;         // tiles in the job
  int      done;          // written this run
  int      skipped;       // already on the card at the right size
  int      noTile;        // the server said 404: nothing there (not an error)
  int      failed;        // network, decode or write failures
  int      reconnects;    // TLS/TCP connections made after the first (the server closed keep-alive)
  int      curZ;
  uint32_t bytes;         // downloaded
  uint32_t elapsedMs;
  uint32_t heapFloorLargest;   // the smallest "largest internal block" seen this run
  uint32_t heapMinEver;        // the allocator's own low-water mark when the run ended
  uint32_t stackFloor;         // the task's unused stack at the end of the run (bytes)
  char     lastErr[64];
  char     source[24];
} TileJobStatus;

/* Tiles in the job and the bytes it will put on the card / pull from the network. */
int  tileFetchEstimate(const TileJobSpec* s, uint32_t* cardBytes, uint32_t* netBytes);
/* Start. False with `why` filled: a run is already going, no WiFi, no task stack, ... */
bool tileFetchStart(const TileJobSpec* s, char* why, size_t whyCap);
void tileFetchStop();                     // ask; the task finishes the tile it is on
bool tileFetchActive();                   // the DFS / screen-hold predicates read this
void tileFetchPause(bool on);             // the loop sets this for calls and WiFi loss
void tileFetchStatus(TileJobStatus* out);
/* One line per call of `emit`, for the serial console. */
void tileFetchReport(void (*emit)(const char* line));

// ── the bench that proved the mechanism (`tlstest <url> [n]`) ────────────────────────────
bool tileFetchBenchStart(const char* url, int count);
void tileFetchBenchReport(void (*emit)(const char* line));

#endif // TILE_FETCH_H
