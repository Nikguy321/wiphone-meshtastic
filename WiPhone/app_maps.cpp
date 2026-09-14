/* app_maps.cpp — see app_maps.h for the design: why the tiles are raw, why the card is read
 * in pieces, and why a pin is not a waypoint until you say so. */

#include "app_maps.h"
#include "meshtastic_service.h"
#include "mesh_pos.h"
#include <SD.h>
#include <Preferences.h>
#include <stdarg.h>

extern bool gGpsNmea;         // WiPhone.ino: is the NMEA receiver switched on at all

// ── colours ───────────────────────────────────────────────────────────────────────────────
// Deliberately loud. These sit on top of an aerial or topo tile, which can be any colour at
// all, so every marker is drawn with a black surround and nothing relies on contrast alone.
#define MAP_C_VOID      0x0000      // outside the world (letterbox at low zoom)
#define MAP_C_NOTILE    0x2124      // this tile is not on the card
#define MAP_C_LOADING   0x4A69      // ...it is, and it is on its way
#define MAP_C_PIN       0xFD20      // your pins: orange
#define MAP_C_PIN_SH    0xFFE0      // ...yellow once shared with the mesh
#define MAP_C_WP        0x07E0      // places heard from the mesh: green
#define MAP_C_NODE      0x07FF      // other people: cyan
#define MAP_C_NODE_OLD  0x8410      // ...grey when the fix is old enough to mislead
#define MAP_C_ME        0xFFFF      // this phone
#define MAP_C_STRIP     0x18E3      // the readout band at the bottom
#define MAP_C_CHIP      0x0000

#define MAP_SHAPE_DOT      0
#define MAP_SHAPE_DIAMOND  1

/* A node position older than this is drawn grey and its age is shown. Same 30 minutes the
 * research note used: on foot, half an hour is far enough to be somewhere else entirely, and
 * a map that draws a stale dot in the same colour as a live one is lying by omission. */
#define MAP_STALE_MS   (30u * 60u * 1000u)

// A pan that arrives within this of the last one counts as "still holding the key".
#define MAP_PAN_RUN_MS  400

static const char MAPS_NVS[] = "maps";

/* Row keys. 🛑 NEVER 0 — MenuWidget::addOption() refuses a key of 0 outright and adds no row
 * at all (see the note on addNote() in GUI.h, and tests/check_menu_keys.py, which exists
 * because eight sites got this wrong on the same day). Display-only rows use addNote(). */
enum {
  ROW_FIRST        = 1,        // list rows are index + ROW_FIRST
  ROW_ACT          = 10000,
  ROW_M_PINS       = ROW_ACT + 1,
  ROW_M_PLACES     = ROW_ACT + 2,
  ROW_M_NODES      = ROW_ACT + 3,
  ROW_M_ME         = ROW_ACT + 4,
  ROW_M_AREA       = ROW_ACT + 5,
  ROW_M_RESCAN     = ROW_ACT + 6,
  ROW_M_HELP       = ROW_ACT + 7,
  ROW_M_BACK       = ROW_ACT + 8,
  ROW_P_RENAME     = ROW_ACT + 20,
  ROW_P_MOVE       = ROW_ACT + 21,
  ROW_P_SHARE      = ROW_ACT + 22,
  ROW_P_UNSHARE    = ROW_ACT + 23,
  ROW_P_CENTRE     = ROW_ACT + 24,
  ROW_P_DELETE     = ROW_ACT + 25,
  ROW_P_CANCEL     = ROW_ACT + 26,
  ROW_DEL_YES      = ROW_ACT + 30,
  ROW_DEL_NO       = ROW_ACT + 31,
};

// ---------------------------------------------------------------- construction

MapsApp::MapsApp(LCD& disp, ControlState& state, HeaderWidget* hdr, FooterWidget* ftr)
  : WindowedApp(disp, state, hdr, ftr) {
  log_d("create MapsApp");

  /* ⚠ SET WHAT THIS SCREEN NEEDS. msAppTimerEventPeriod is device-wide and apps routinely
   * leave their own value in it (the Game Boy leaves 250 ms). The map arms it only while a
   * tile is actually on its way; see the end of redrawScreen(). */
  controlState.msAppTimerEventPeriod = 0;

  appState = MAPS_VIEW;
  listKind = LIST_PINS;
  menu = NULL;
  textArea = NULL;
  headerTitle[0] = '\0';
  note[0] = '\0';
  statusLine[0] = '\0';

  vpX = 0;
  vpY = header->height();
  vpW = lcd.width();
  vpH = (int)lcd.height() - (int)header->height() - (int)footer->height();

  zoom = 15;
  cx = cy = 0;
  areaCount = 0;
  areaSel = -1;
  pinCount = 0;
  pinSel = -1;
  pinsDirty = false;
  slotsAlive = 0;
  useSeq = 0;
  missCount = missNext = 0;
  loadActive = false;
  loadSlot = -1;
  loadZ = loadTx = loadTy = -1;
  loadGot = 0;
  wantAny = false;
  wantZ = wantTx = wantTy = -1;
  pendingTiles = 0;
  tileMemFail = false;
  panRun = 0;
  panDir = 0;
  panLastMs = 0;

  for (int i = 0; i < MAPS_TILE_SLOTS; i++) {
    slots[i].px = NULL;
    slots[i].z = -1;
    slots[i].tx = slots[i].ty = 0;
    slots[i].used = 0;
    slots[i].ready = false;
  }

  /* PSRAM for everything that is not a handful of ints. The pins table is 2 KB and the
   * projection scratch is another 512 B; neither belongs on the GUI task's stack, and
   * neither belongs in the ~19 KB internal heap that SIP and WiFi fight over. */
  pins  = (MapPin*)ps_malloc(sizeof(MapPin) * MAPS_MAX_PINS);
  pinVx = (int*)ps_malloc(sizeof(int) * MAPS_MAX_PINS);
  pinVy = (int*)ps_malloc(sizeof(int) * MAPS_MAX_PINS);
  /* 🛑 THE THREE SUCCEED TOGETHER OR NOT AT ALL, and that is not tidiness. Every user of the
   * projection scratch is guarded on `pins` alone — because the three are conceptually one
   * table — so a run where `pins` came back and `pinVx` did not would walk straight into
   * `pinVx[i]` through a null pointer, from the draw path, on every frame. Collapsing the
   * three states into two means the one guard everybody writes is the right guard. */
  if (!pins || !pinVx || !pinVy) {
    log_e("MAPS: no PSRAM for the pin table - pins are off this session");
    free(pins);
    free(pinVx);
    free(pinVy);
    pins = NULL;
    pinVx = pinVy = NULL;
  }

  if (!allocSlots()) {
    tileMemFail = true;
    log_e("MAPS: only %d tile slots - the map will not draw", slotsAlive);
  }

  scanAreas();
  loadPins();
  restoreView();
  enterState(MAPS_VIEW);
}

MapsApp::~MapsApp() {
  log_d("destroy MapsApp");
  /* Save the view FIRST. Everything after this can fail; where the user was looking is the
   * one piece of state they will notice missing, and it costs one NVS write. */
  saveView();
  if (pinsDirty) {
    savePins();
  }
  cancelLoad();
  freeWidgets();
  freeSlots();
  free(pins);
  free(pinVx);
  free(pinVy);
  pins = NULL;
  pinVx = pinVy = NULL;
  controlState.msAppTimerEventPeriod = 0;    // do not leave the CPU waking for a closed app
}

bool MapsApp::allocSlots() {
  slotsAlive = 0;
  for (int i = 0; i < MAPS_TILE_SLOTS; i++) {
    slots[i].px = (uint16_t*)ps_malloc(MAP_TILE_BYTES);
    if (slots[i].px) {
      slotsAlive++;
    }
  }
  /* Two is the floor, not a nicety: a viewport can straddle 2x2 tiles, and with one slot the
   * cache would evict the tile it is about to need on every single blit — the card would be
   * read four times per repaint forever. Below two, the app says so instead. */
  return slotsAlive >= 2;
}

void MapsApp::freeSlots() {
  for (int i = 0; i < MAPS_TILE_SLOTS; i++) {
    if (slots[i].px) {
      free(slots[i].px);
      slots[i].px = NULL;
    }
    slots[i].z = -1;
    slots[i].ready = false;
  }
  slotsAlive = 0;
}

// ---------------------------------------------------------------- the card

/* A name that is a plain unsigned number, with an optional extension when `allowExt`:
 * "5249" -> 5249, "11443.565" -> 11443, "x" -> -1, "" -> -1, ".565" -> -1.
 *
 * ⚠ NOT atoi(). atoi("12abc") is 12 and atoi("abc") is 0, so with atoi a stray folder becomes
 * zoom 0 or tile column 0 — a real coordinate — and the map quietly offers a level, or a
 * column, with nothing behind it. Every name that is not exactly a number is refused here,
 * which is also what keeps macOS's "._11443.565" sidecars out (they start with a dot). */
static long numericName(const char* name, bool allowExt) {
  if (!name || !name[0]) {
    return -1;
  }
  long v = 0;
  int digits = 0;
  for (const char* p = name; *p; p++) {
    if (*p == '.' && allowExt && digits) {
      return v;                        // the extension starts here and the stem was a number
    }
    if (*p < '0' || *p > '9') {
      return -1;
    }
    v = v * 10 + (*p - '0');
    if (v > 134217727L) {              // past the deepest zoom's tile count: not one of ours
      return -1;
    }
    digits++;
  }
  return digits ? v : -1;
}

// A zoom folder: a number, and one this firmware can address.
static int zoomFolder(const char* name) {
  const long v = numericName(name, false);
  return (v >= 0 && v <= MAP_ZOOM_MAX) ? (int)v : -1;
}

// On this core File::name() returns the FULL path; every caller has to basename it itself.
static const char* baseName(const char* path) {
  const char* s = strrchr(path, '/');
  return s ? s + 1 : path;
}

void MapsApp::scanAreas() {
  areaCount = 0;
  areaSel = -1;
  if (!SD.exists(MAPS_ROOT)) {
    /* Make the folder so the message can name a place that exists. A user told "put tiles in
     * /maps" who then cannot find /maps has been given a puzzle, not an instruction. */
    SD.mkdir(MAPS_ROOT);
    return;
  }
  File dir = SD.open(MAPS_ROOT);
  if (!dir || !dir.isDirectory()) {
    if (dir) {
      dir.close();
    }
    return;
  }
  File f;
  while ((f = dir.openNextFile())) {
    const char* nm = baseName(f.name());
    if (!f.isDirectory() || !mapAreaNameOk(nm)) {
      f.close();
      continue;
    }
    if (areaCount >= MAPS_MAX_AREAS) {
      f.close();
      break;
    }
    MapArea* a = &areas[areaCount];
    strlcpy(a->name, nm, sizeof(a->name));
    a->zMin = MAP_ZOOM_MAX + 1;
    a->zMax = -1;
    // Which zoom folders are really there.
    char zpath[96];
    snprintf(zpath, sizeof(zpath), "%s/%s", MAPS_ROOT, nm);
    File zd = SD.open(zpath);
    if (zd && zd.isDirectory()) {
      File zf;
      while ((zf = zd.openNextFile())) {
        const int z = zf.isDirectory() ? zoomFolder(baseName(zf.name())) : -1;
        if (z >= 0) {
          if (z < a->zMin) {
            a->zMin = z;
          }
          if (z > a->zMax) {
            a->zMax = z;
          }
        }
        zf.close();
      }
    }
    if (zd) {
      zd.close();
    }
    f.close();
    if (a->zMax >= 0) {          // a folder with no zoom levels in it is not a map
      areaCount++;
    }
  }
  dir.close();
  if (areaCount > 0) {
    areaSel = 0;
  }
}

// ---------------------------------------------------------------- pins on the card

void MapsApp::loadPins() {
  pinCount = 0;
  if (!pins) {
    return;
  }
  File f = SD.open(MAPS_PINS_FILE, FILE_READ);
  if (!f) {
    return;                      // no pins yet is the ordinary case, not an error
  }
  char line[MAP_PIN_LINE_MAX];
  size_t n = 0;
  int bad = 0;
  /* Read a byte at a time. The file is a few kilobytes at most and this needs no buffer of
   * its own; a line longer than the buffer is consumed to its end rather than being split
   * into two half-lines, one of which would parse as a different pin. */
  bool overlong = false;
  while (f.available()) {
    const int ch = f.read();
    if (ch < 0) {
      break;
    }
    if (ch == '\n' || ch == '\r') {
      if (n || overlong) {
        line[n] = '\0';
        if (!overlong && pinCount < MAPS_MAX_PINS) {
          const int r = mapPinParseLine(line, &pins[pinCount]);
          if (r == 1) {
            pinCount++;
          } else if (r < 0) {
            bad++;
          }
        } else if (overlong) {
          bad++;
        }
      }
      n = 0;
      overlong = false;
      continue;
    }
    if (n + 1 < sizeof(line)) {
      line[n++] = (char)ch;
    } else {
      overlong = true;
    }
  }
  if ((n || overlong) && !overlong && pinCount < MAPS_MAX_PINS) {
    line[n] = '\0';               // a last line with no newline is still a line
    if (mapPinParseLine(line, &pins[pinCount]) == 1) {
      pinCount++;
    }
  }
  f.close();
  if (bad) {
    log_e("MAPS: %d unreadable line(s) in %s were skipped", bad, MAPS_PINS_FILE);
  }
}

void MapsApp::savePins() {
  if (!pins) {
    return;
  }
  if (!SD.exists(MAPS_ROOT)) {
    SD.mkdir(MAPS_ROOT);
  }
  /* Write to a temporary file and rename over the real one. A power cut in the middle of a
   * rewrite would otherwise leave a half-file — and this phone's power switch is a hard cut,
   * not a shutdown (README). Losing the last pin is a shrug; losing the list is a day. */
  const char tmp[] = MAPS_PINS_FILE ".new";
  SD.remove(tmp);
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) {
    log_e("MAPS: could not write %s", tmp);
    return;
  }
  f.print("# WiPhone map pins v1 - latI,lonI (1e-7 deg), shared waypoint id, name\n");
  char line[MAP_PIN_LINE_MAX];
  int written = 0;
  for (int i = 0; i < pinCount; i++) {
    if (mapPinFormatLine(&pins[i], line, sizeof(line)) > 0) {
      f.print(line);
      f.print("\n");
      written++;
    }
  }
  f.flush();
  const bool ok = (f.size() > 0);
  f.close();
  if (!ok) {
    log_e("MAPS: %s came out empty - keeping the old file", tmp);
    SD.remove(tmp);
    return;
  }
  SD.remove(MAPS_PINS_FILE);
  if (!SD.rename(tmp, MAPS_PINS_FILE)) {
    /* 🛑 THE OLD FILE IS ALREADY GONE AT THIS POINT, so a failed rename is not "no change",
     * it is "the pins are now only in a file called pins.txt.new". Write them again, straight
     * to the real path, rather than leaving a user to find that out in the woods. FAT rename
     * cannot overwrite, which is why the remove has to come first and why this exists. */
    log_e("MAPS: could not rename %s into place - writing directly", tmp);
    File d = SD.open(MAPS_PINS_FILE, FILE_WRITE);
    if (!d) {
      log_e("MAPS: and the direct write failed too - the pins are in %s", tmp);
      return;
    }
    d.print("# WiPhone map pins v1 - latI,lonI (1e-7 deg), shared waypoint id, name\n");
    for (int i = 0; i < pinCount; i++) {
      if (mapPinFormatLine(&pins[i], line, sizeof(line)) > 0) {
        d.print(line);
        d.print("\n");
      }
    }
    d.flush();
    d.close();
    SD.remove(tmp);
  }
  pinsDirty = false;
  log_d("MAPS: saved %d pin(s)", written);
}

// ---------------------------------------------------------------- the view

void MapsApp::saveView() {
  double lat = 0, lon = 0;
  mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &lat, &lon);
  Preferences p;
  if (!p.begin(MAPS_NVS, false)) {
    return;
  }
  p.putInt("lat", mapDegToI7(lat));
  p.putInt("lon", mapDegToI7(lon));
  p.putInt("z", zoom);
  char area[32];
  area[0] = '\0';
  if (areaSel >= 0) {
    strlcpy(area, areas[areaSel].name, sizeof(area));
  }
  /* putBytes, not putString: a String here is a heap allocation on the internal heap, on the
   * teardown path, for a 31-byte name. Same reason loadPosSettings() uses getBytes. */
  p.putBytes("area", area, sizeof(area));
  p.putInt("saved", 1);
  p.end();
}

void MapsApp::restoreView() {
  /* WHERE THE MAP OPENS, in order:
   *   1. where it was last closed — that is what was asked for, and it is the only one of
   *      these that survives being somewhere with no signal and no sky;
   *   2. this phone's own GPS fix, if the receiver is on and has one;
   *   3. the mesh's reference place (a chosen waypoint, a fresh fix, or the manual pin) —
   *      resolveReference() already encodes that precedence and it is not re-litigated here;
   *   4. the middle of whatever tiles the card actually holds;
   *   5. 0,0, with the status line saying there is nothing to show.
   * Each step is only taken when the one before it has nothing to offer. */
  double lat = 0, lon = 0;
  bool have = false;
  int z = -1;
  char area[32];
  area[0] = '\0';

  Preferences p;
  if (p.begin(MAPS_NVS, true)) {
    if (p.getInt("saved", 0) == 1) {
      lat = mapI7ToDeg(p.getInt("lat", 0));
      lon = mapI7ToDeg(p.getInt("lon", 0));
      z = p.getInt("z", -1);
      p.getBytes("area", area, sizeof(area));
      area[sizeof(area) - 1] = '\0';
      have = true;
    }
    p.end();
  }

  // The remembered area, if it is still on the card.
  if (area[0] && areaCount > 0) {
    for (int i = 0; i < areaCount; i++) {
      if (!strcmp(areas[i].name, area)) {
        areaSel = i;
        break;
      }
    }
  }

  if (!have) {
    int32_t la = 0, lo = 0;
    uint32_t age = 0;
    if (gGpsNmea && meshService.getGpsFix(&la, &lo, &age, NULL, NULL)) {
      lat = mapI7ToDeg(la);
      lon = mapI7ToDeg(lo);
      have = true;
      setNote("Opened on the GPS fix");
    } else if (meshService.resolveReference(&la, &lo, NULL, 0)) {
      lat = mapI7ToDeg(la);
      lon = mapI7ToDeg(lo);
      have = true;
      setNote("Opened on the reference place");
    }
  }

  if (z < MAP_ZOOM_MIN || z > MAP_ZOOM_MAX) {
    z = -1;
  }
  if (areaSel >= 0) {
    const MapArea* a = &areas[areaSel];
    if (z < a->zMin || z > a->zMax) {
      /* No remembered zoom, or one this area does not have: start at the deepest level that
       * is not the very deepest, so there is somewhere to zoom in TO. */
      z = (a->zMax > a->zMin) ? a->zMax - 1 : a->zMax;
    }
  } else if (z < 0) {
    z = 15;
  }
  zoom = z;

  if (!have && areaSel >= 0) {
    /* The middle of the tiles that exist. Scanned at the SHALLOWEST zoom the area has, where
     * there are a handful of folders rather than thousands — the same ground, a hundredth of
     * the directory reads.
     *
     * ⚠ The vertical range comes from the FIRST column only, and both loops stop at 64
     * entries. That is deliberate: this runs once, on a screen the user is waiting for, and
     * it only has to land somewhere on the map — anything more precise would be a directory
     * walk of the whole card to save a press of the d-pad. An area whose columns have very
     * different row ranges opens a little off centre and never wrongly. */
    const MapArea* a = &areas[areaSel];
    char zpath[96];
    snprintf(zpath, sizeof(zpath), "%s/%s/%d", MAPS_ROOT, a->name, a->zMin);
    long xMin = -1, xMax = -1, yMin = -1, yMax = -1;
    File xd = SD.open(zpath);
    if (xd && xd.isDirectory()) {
      int seen = 0;
      File xf;
      while (seen < 64 && (xf = xd.openNextFile())) {
        const long col = xf.isDirectory() ? numericName(baseName(xf.name()), false) : -1;
        if (col < 0) {
          xf.close();
          continue;                    // not a tile column; it does not count against the 64
        }
        if (xMin < 0 || col < xMin) {
          xMin = col;
        }
        if (col > xMax) {
          xMax = col;
        }
        if (yMin < 0) {
          char ypath[160];
          snprintf(ypath, sizeof(ypath), "%s/%ld", zpath, col);
          File yd = SD.open(ypath);
          if (yd && yd.isDirectory()) {
            int ys = 0;
            File yf;
            while (ys < 64 && (yf = yd.openNextFile())) {
              const long row = yf.isDirectory() ? -1 : numericName(baseName(yf.name()), true);
              if (row >= 0) {
                if (yMin < 0 || row < yMin) {
                  yMin = row;
                }
                if (row > yMax) {
                  yMax = row;
                }
                ys++;
              }
              yf.close();
            }
          }
          if (yd) {
            yd.close();
          }
        }
        seen++;
        xf.close();
      }
    }
    if (xd) {
      xd.close();
    }
    if (xMin >= 0 && yMin >= 0) {
      const double wx = ((double)xMin + (double)xMax + 1.0) * 0.5 * MAP_TILE_PX;
      const double wy = ((double)yMin + (double)yMax + 1.0) * 0.5 * MAP_TILE_PX;
      mapWorldToLatLon(wx, wy, a->zMin, &lat, &lon);
      have = true;
      setNote("Opened on the middle of the map");
    }
  }

  double wx = 0, wy = 0;
  mapLatLonToWorld(lat, lon, zoom, &wx, &wy);
  cx = (int32_t)(wx + 0.5);
  cy = (int32_t)(wy + 0.5);
  mapClampView(zoom, vpW, vpH, &cx, &cy);
  (void)have;
}

// ---------------------------------------------------------------- tile cache

int MapsApp::findSlot(int z, int tx, int ty) const {
  for (int i = 0; i < MAPS_TILE_SLOTS; i++) {
    if (slots[i].px && slots[i].z == z && slots[i].tx == tx && slots[i].ty == ty) {
      return i;
    }
  }
  return -1;
}

int MapsApp::claimSlot() {
  int best = -1;
  for (int i = 0; i < MAPS_TILE_SLOTS; i++) {
    if (!slots[i].px) {
      continue;
    }
    if (loadActive && i == loadSlot) {
      continue;                  // never evict the tile currently being read into
    }
    if (slots[i].z < 0) {
      return i;                  // an empty slot is always the right victim
    }
    if (best < 0 || (int32_t)(slots[i].used - slots[best].used) < 0) {
      best = i;                  // signed difference: `used` is a counter and counters wrap
    }
  }
  return best;
}

bool MapsApp::isKnownMissing(int z, int tx, int ty) const {
  for (int i = 0; i < missCount; i++) {
    if (misses[i].z == z && misses[i].tx == tx && misses[i].ty == ty) {
      return true;
    }
  }
  return false;
}

void MapsApp::rememberMissing(int z, int tx, int ty) {
  if (isKnownMissing(z, tx, ty)) {
    return;
  }
  if (missCount < MAPS_MISS_CACHE) {
    misses[missCount].z = z;
    misses[missCount].tx = tx;
    misses[missCount].ty = ty;
    missCount++;
    return;
  }
  misses[missNext].z = z;        // a ring: the oldest hole is the one we can afford to re-ask
  misses[missNext].tx = tx;
  misses[missNext].ty = ty;
  missNext = (missNext + 1) % MAPS_MISS_CACHE;
}

void MapsApp::forgetMissing() {
  missCount = 0;
  missNext = 0;
}

bool MapsApp::startLoad(int z, int tx, int ty) {
  if (loadActive || tileMemFail || areaSel < 0) {
    return false;
  }
  const int s = claimSlot();
  if (s < 0) {
    return false;
  }
  /* Claim the slot BEFORE the first byte arrives, marked not-ready. findSlot() then reports
   * the tile as "being fetched", which is what stops the next repaint from starting a second
   * load for the same tile and thrashing the card. */
  slots[s].z = z;
  slots[s].tx = tx;
  slots[s].ty = ty;
  slots[s].ready = false;
  slots[s].used = ++useSeq;
  loadSlot = s;
  loadZ = z;
  loadTx = tx;
  loadTy = ty;
  loadGot = 0;
  loadActive = true;
  return true;
}

void MapsApp::cancelLoad() {
  if (!loadActive) {
    return;
  }
  if (loadSlot >= 0 && loadSlot < MAPS_TILE_SLOTS) {
    slots[loadSlot].z = -1;      // a half-read tile is not a tile
    slots[loadSlot].ready = false;
  }
  loadActive = false;
  loadSlot = -1;
  loadGot = 0;
}

bool MapsApp::tileLoadStep() {
  if (!loadActive || loadSlot < 0 || !slots[loadSlot].px) {
    loadActive = false;
    return false;
  }
  char path[160];
  if (areaSel < 0 ||
      !mapTilePath(path, sizeof(path), MAPS_ROOT, areas[areaSel].name, loadZ, loadTx, loadTy)) {
    rememberMissing(loadZ, loadTx, loadTy);
    cancelLoad();
    return false;
  }
  File f = SD.open(path, FILE_READ);
  if (!f) {
    rememberMissing(loadZ, loadTx, loadTy);
    cancelLoad();
    return false;
  }
  /* 🛑 THE LENGTH IS THE WHOLE FORMAT CHECK, AND IT IS CHECKED ON EVERY PIECE. A raw tile has
   * no header, no magic and no way to be self-describing: a PNG that was copied without being
   * converted is simply a file of the wrong length, and if it were read anyway the screen
   * would fill with the PNG's compressed bytes as colour. The check is cheap and it is the
   * only thing standing between a mistake at the computer and a screenful of confetti. */
  if ((size_t)f.size() != MAP_TILE_BYTES) {
    log_e("MAPS: %s is %u bytes, expected %u - not a raw tile",
          path, (unsigned)f.size(), (unsigned)MAP_TILE_BYTES);
    f.close();
    rememberMissing(loadZ, loadTx, loadTy);
    cancelLoad();
    return false;
  }
  if (loadGot && !f.seek(loadGot)) {
    f.close();
    rememberMissing(loadZ, loadTx, loadTy);
    cancelLoad();
    return false;
  }
  size_t want = MAP_TILE_BYTES - loadGot;
  if (want > MAPS_CHUNK_BYTES) {
    want = MAPS_CHUNK_BYTES;
  }
  const int got = f.read((uint8_t*)slots[loadSlot].px + loadGot, want);
  f.close();
  if (got <= 0) {
    log_e("MAPS: %s read failed at %u", path, (unsigned)loadGot);
    rememberMissing(loadZ, loadTx, loadTy);
    cancelLoad();
    return false;
  }
  loadGot += (uint32_t)got;
  if (loadGot >= MAP_TILE_BYTES) {
    slots[loadSlot].ready = true;
    loadActive = false;
    loadSlot = -1;
    loadGot = 0;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- drawing primitives

void MapsApp::fillClipped(int x, int y, int w, int h, uint16_t colour) {
  int x0 = x < vpX ? vpX : x;
  int y0 = y < vpY ? vpY : y;
  int x1 = x + w, y1 = y + h;
  if (x1 > vpX + vpW) {
    x1 = vpX + vpW;
  }
  if (y1 > vpY + vpH) {
    y1 = vpY + vpH;
  }
  if (x1 <= x0 || y1 <= y0) {
    return;
  }
  lcd.fillRect(x0, y0, x1 - x0, y1 - y0, colour);
}

void MapsApp::drawBlob(int x, int y, int r, uint16_t colour, bool diamond) {
  if (r < 0) {
    return;
  }
  for (int dy = -r; dy <= r; dy++) {
    const int ady = dy < 0 ? -dy : dy;
    int half = diamond ? (r - ady) : r;
    if (!diamond && r >= 2 && ady == r) {
      half = r - 1;              // clip the corners so a "dot" reads as round, not square
    }
    if (half < 0) {
      continue;
    }
    fillClipped(x - half, y + dy, 2 * half + 1, 1, colour);
  }
}

void MapsApp::drawMarker(int vx, int vy, uint16_t colour, int shape) {
  const bool diamond = (shape == MAP_SHAPE_DIAMOND);
  drawBlob(vx, vy, 4, BLACK, diamond);     // the surround: markers sit on unknown colours
  drawBlob(vx, vy, 3, colour, diamond);
}

void MapsApp::drawLabel(int vx, int vy, const char* text, uint16_t colour) {
  if (!text || !text[0]) {
    return;
  }
  SmoothFont* fnt = fonts[AKROBAT_BOLD_16];
  if (!fnt) {
    return;
  }
  lcd.setTextFont(fnt);
  const int tw = lcd.textWidth(text);
  const int th = (int)fnt->height();
  int bx = vx + 7;
  const int by = vy - th / 2;
  if (bx + tw + 4 > vpX + vpW) {
    bx = vx - 7 - (tw + 4);      // no room on the right: put it on the left
  }
  /* ⚠ ALL OR NOTHING. drawString cannot be clipped — it would write straight into the header
   * or the footer — so a label that does not fit entirely inside the map is not drawn at all.
   * Half a name is not information anyway. */
  if (bx < vpX || by < vpY || bx + tw + 4 > vpX + vpW || by + th + 2 > vpY + vpH) {
    return;
  }
  lcd.fillRect(bx, by, tw + 4, th + 2, BLACK);
  lcd.setTextColor(colour, BLACK);
  lcd.drawString(text, bx + 2, by + 1);
}

void MapsApp::drawCrosshair() {
  const int mx = vpX + vpW / 2, my = vpY + vpH / 2;
  for (int pass = 0; pass < 2; pass++) {
    const uint16_t c = pass ? MAP_C_ME : BLACK;
    const int t = pass ? 1 : 3;            // a black bed under a white line: legible on anything
    const int ext = pass ? 0 : 1;
    fillClipped(mx - 15 - ext, my - t / 2, 10 + 2 * ext, t, c);
    fillClipped(mx + 6 - ext,  my - t / 2, 10 + 2 * ext, t, c);
    fillClipped(mx - t / 2, my - 15 - ext, t, 10 + 2 * ext, c);
    fillClipped(mx - t / 2, my + 6 - ext,  t, 10 + 2 * ext, c);
  }
}

// ---------------------------------------------------------------- the map itself

void MapsApp::drawMap() {
  /* The byte order of a raw tile is the phone's own: RGB565 little-endian, which is exactly
   * what color565() produces and what a sprite stores. Both flags are set explicitly because
   * `lcd` is the page sprite when there is one and the real panel when there is not, and the
   * two keep the flag in different members. */
  lcd.setSwapBytes(false);
  if (lcd.isSprite()) {
    ((TFT_eSprite&)lcd).setSwapBytes(false);
  }

  wantAny = false;
  pendingTiles = 0;
  statusLine[0] = '\0';

  MapBlit b[MAP_MAX_BLITS];
  const int n = mapViewBlits(zoom, cx, cy, vpW, vpH, b, MAP_MAX_BLITS);
  lcd.fillRect(vpX, vpY, vpW, vpH, MAP_C_VOID);

  int haveTiles = 0;
  if (n > 0 && areaSel >= 0 && !tileMemFail) {
    for (int i = 0; i < n; i++) {
      const MapBlit* q = &b[i];
      const int si = findSlot(zoom, q->tileX, q->tileY);
      if (si >= 0 && slots[si].ready) {
        slots[si].used = ++useSeq;
        haveTiles++;
        /* Row at a time. pushImage's `w` is both the width AND the source stride, so a
         * sub-rectangle of a 256-wide tile cannot be pushed in one call; a row can, and the
         * source pointer is the row's start. Every coordinate here is already inside both the
         * tile and the viewport — mapViewBlits guarantees it, and test_maptiles proves it. */
        for (int r = 0; r < q->h; r++) {
          uint16_t* row = slots[si].px + (size_t)(q->srcY + r) * MAP_TILE_PX + q->srcX;
          lcd.pushImage(vpX + q->dstX, vpY + q->dstY + r, (uint16_t)q->w, 1, row);
        }
      } else {
        const bool missing = isKnownMissing(zoom, q->tileX, q->tileY);
        fillClipped(vpX + q->dstX, vpY + q->dstY, q->w, q->h,
                    missing ? MAP_C_NOTILE : MAP_C_LOADING);
        if (!missing) {
          pendingTiles++;
          if (!wantAny && si < 0) {
            wantAny = true;
            wantZ = zoom;
            wantTx = q->tileX;
            wantTy = q->tileY;
          }
        }
      }
    }
  } else if (n > 0 && areaSel >= 0 && tileMemFail) {
    lcd.fillRect(vpX, vpY, vpW, vpH, MAP_C_NOTILE);
  }

  if (tileMemFail) {
    strlcpy(statusLine, "no memory for tiles", sizeof(statusLine));
  } else if (areaSel < 0) {
    strlcpy(statusLine, "no map on the card", sizeof(statusLine));
  } else if (pendingTiles > 0) {
    snprintf(statusLine, sizeof(statusLine), "%d tile%s...", pendingTiles,
             pendingTiles == 1 ? "" : "s");
  } else if (haveTiles == 0) {
    strlcpy(statusLine, "no tiles here", sizeof(statusLine));
  }

  drawOverlays();
}

void MapsApp::drawOverlays() {
  // ---- this phone's pins ----
  for (int i = 0; i < pinCount && pins; i++) {
    mapLatLonToView(zoom, cx, cy, vpW, vpH,
                    mapI7ToDeg(pins[i].latI), mapI7ToDeg(pins[i].lonI),
                    &pinVx[i], &pinVy[i]);
  }
  for (int i = 0; i < pinCount && pins; i++) {
    if (pinVx[i] < 0 || pinVx[i] >= vpW || pinVy[i] < 0 || pinVy[i] >= vpH) {
      continue;
    }
    const uint16_t c = pins[i].sharedId ? MAP_C_PIN_SH : MAP_C_PIN;
    drawMarker(vpX + pinVx[i], vpY + pinVy[i], c, MAP_SHAPE_DOT);
    drawLabel(vpX + pinVx[i], vpY + pinVy[i], pins[i].name, c);
  }

  // ---- places heard from the mesh ----
  const int wpN = meshService.getWaypointCount();
  for (int i = 0; i < wpN; i++) {
    const MeshWaypoint* w = meshService.getWaypoint(i);
    if (!w || !w->id) {
      continue;
    }
    int vx = 0, vy = 0;
    if (!mapLatLonToView(zoom, cx, cy, vpW, vpH,
                         mapI7ToDeg(w->latI), mapI7ToDeg(w->lonI), &vx, &vy)) {
      continue;
    }
    drawMarker(vpX + vx, vpY + vy, MAP_C_WP, MAP_SHAPE_DIAMOND);
    drawLabel(vpX + vx, vpY + vy, w->name, MAP_C_WP);
  }

  // ---- other people ----
  const uint32_t me = meshService.getMyNodeNum();
  const int nodeN = meshService.getNodeCount();
  const uint32_t now = millis();
  for (int i = 0; i < nodeN; i++) {
    const MeshNode* nd = meshService.getNode(i);
    if (!nd || !nd->nodeNum || nd->nodeNum == me || !nd->posHeardMs) {
      continue;                  // our own row holds the manual pin, drawn separately below
    }
    int vx = 0, vy = 0;
    if (!mapLatLonToView(zoom, cx, cy, vpW, vpH,
                         mapI7ToDeg(nd->latI), mapI7ToDeg(nd->lonI), &vx, &vy)) {
      continue;
    }
    const bool stale = (uint32_t)(now - nd->posHeardMs) > MAP_STALE_MS;
    const uint16_t c = stale ? MAP_C_NODE_OLD : MAP_C_NODE;
    drawMarker(vpX + vx, vpY + vy, c, MAP_SHAPE_DOT);
    char lbl[MESH_NAME_LEN + 12];
    if (stale) {
      const unsigned mins = (unsigned)((now - nd->posHeardMs) / 60000u);
      snprintf(lbl, sizeof(lbl), "%s %uh%um", nd->name[0] ? nd->name : "?",
               mins / 60u, mins % 60u);
    } else {
      strlcpy(lbl, nd->name[0] ? nd->name : "?", sizeof(lbl));
    }
    drawLabel(vpX + vx, vpY + vy, lbl, c);
  }

  // ---- this phone ----
  int32_t la = 0, lo = 0;
  uint32_t age = 0;
  if (gGpsNmea && meshService.getGpsFix(&la, &lo, &age, NULL, NULL)) {
    int vx = 0, vy = 0;
    if (mapLatLonToView(zoom, cx, cy, vpW, vpH, mapI7ToDeg(la), mapI7ToDeg(lo), &vx, &vy)) {
      drawBlob(vpX + vx, vpY + vy, 6, BLACK, false);
      drawBlob(vpX + vx, vpY + vy, 5, MAP_C_ME, false);
      drawBlob(vpX + vx, vpY + vy, 2, BLACK, false);
      drawLabel(vpX + vx, vpY + vy, "me", MAP_C_ME);
    }
  } else if (meshService.getMyPin(&la, &lo, NULL)) {
    int vx = 0, vy = 0;
    if (mapLatLonToView(zoom, cx, cy, vpW, vpH, mapI7ToDeg(la), mapI7ToDeg(lo), &vx, &vy)) {
      drawMarker(vpX + vx, vpY + vy, MAP_C_ME, MAP_SHAPE_DIAMOND);
      drawLabel(vpX + vx, vpY + vy, "my pin", MAP_C_ME);
    }
  }

  drawCrosshair();
  drawChips();
  drawBottomStrip();
}

void MapsApp::drawChips() {
  SmoothFont* fnt = fonts[AKROBAT_BOLD_16];
  if (!fnt) {
    return;
  }
  lcd.setTextFont(fnt);
  const int th = (int)fnt->height();
  char left[40];
  snprintf(left, sizeof(left), "z%d %s", zoom, areaSel >= 0 ? areas[areaSel].name : "-");
  const int lw = lcd.textWidth(left);
  fillClipped(vpX, vpY, lw + 8, th + 4, MAP_C_CHIP);
  if (vpY + th + 4 <= vpY + vpH && lw + 8 <= vpW) {
    lcd.setTextColor(MAP_C_ME, MAP_C_CHIP);
    lcd.drawString(left, vpX + 4, vpY + 2);
  }
  if (statusLine[0]) {
    const int sw = lcd.textWidth(statusLine);
    const int sx = vpX + vpW - sw - 8;
    if (sx > vpX + lw + 12) {
      fillClipped(sx, vpY, sw + 8, th + 4, MAP_C_CHIP);
      lcd.setTextColor(0xFD20, MAP_C_CHIP);
      lcd.drawString(statusLine, sx + 4, vpY + 2);
    }
  }
}

void MapsApp::drawBottomStrip() {
  SmoothFont* fnt = fonts[AKROBAT_BOLD_16];
  if (!fnt) {
    return;
  }
  lcd.setTextFont(fnt);
  const int th = (int)fnt->height();
  const int stripH = th * 2 + 6;
  const int sy = vpY + vpH - stripH;
  if (stripH >= vpH) {
    return;
  }
  fillClipped(vpX, sy, vpW, stripH, MAP_C_STRIP);

  // Row 1: the scale bar, and how far the crosshair is from the reference place.
  double lat = 0, lon = 0;
  mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &lat, &lon);
  int barPx = 0;
  const int barM = mapScaleBar(mapMetersPerPixel(lat, zoom), 84, &barPx);
  const int by = sy + 3;
  if (barM > 0 && barPx > 0) {
    fillClipped(vpX + 4, by + th / 2, barPx, 2, MAP_C_ME);
    fillClipped(vpX + 4, by + th / 2 - 3, 2, 8, MAP_C_ME);
    fillClipped(vpX + 4 + barPx - 2, by + th / 2 - 3, 2, 8, MAP_C_ME);
    char sb[24];
    if (barM >= 1000) {
      snprintf(sb, sizeof(sb), "%dkm", barM / 1000);
    } else {
      snprintf(sb, sizeof(sb), "%dm", barM);
    }
    lcd.setTextColor(MAP_C_ME, MAP_C_STRIP);
    lcd.drawString(sb, vpX + 8 + barPx, by);
  }

  int32_t rLat = 0, rLon = 0;
  char rName[24];
  if (meshService.resolveReference(&rLat, &rLon, rName, sizeof(rName))) {
    const int32_t cLat = mapDegToI7(lat), cLon = mapDegToI7(lon);
    const double m = meshPosDistanceM(rLat, rLon, cLat, cLon);
    char d[16];
    meshPosFmtDist(m, d, sizeof(d));
    char right[44];
    snprintf(right, sizeof(right), "%s %s %s", d,
             meshPosCompass8(meshPosBearingDeg(rLat, rLon, cLat, cLon)), rName);
    const int rw = lcd.textWidth(right);
    if (rw + 6 < vpW) {
      lcd.setTextColor(MAP_C_WP, MAP_C_STRIP);
      lcd.drawString(right, vpX + vpW - rw - 4, by);
    }
  }

  // Row 2: the note if there is one, otherwise where the crosshair is.
  char line[64];
  if (note[0]) {
    strlcpy(line, note, sizeof(line));
    lcd.setTextColor(0xFD20, MAP_C_STRIP);
  } else {
    snprintf(line, sizeof(line), "%.5f, %.5f", lat, lon);
    lcd.setTextColor(MAP_C_ME, MAP_C_STRIP);
  }
  lcd.drawString(line, vpX + 4, sy + th + 4);
}

void MapsApp::drawNoMapPage() {
  SmoothFont* fnt = fonts[AKROBAT_BOLD_18];
  lcd.fillRect(vpX, vpY, vpW, vpH, BLACK);
  if (!fnt) {
    return;
  }
  lcd.setTextFont(fnt);
  lcd.setTextColor(0xFD20, BLACK);
  lcd.drawString("No map tiles on the card", vpX + 8, vpY + 10);
  lcd.setTextColor(WHITE, BLACK);
  lcd.setTextFont(fonts[AKROBAT_BOLD_16]);
  const char* lines[] = {
    "Pins and mesh places still work -",
    "the map is just blank behind them.",
    "",
    "Put tiles on the card as",
    "  /maps/<area>/<z>/<x>/<y>.565",
    "made by tools/convert_tiles.py",
    "on the computer (docs/maps.md).",
    "",
    "Menu > Rescan card after copying.",
  };
  int y = vpY + 40;
  for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
    if (y + 16 > vpY + vpH) {
      break;
    }
    lcd.drawString(lines[i], vpX + 8, y);
    y += 17;
  }
}

// ---------------------------------------------------------------- actions

void MapsApp::setNote(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(note, sizeof(note), fmt, ap);
  va_end(ap);
}

void MapsApp::centreOn(double lat, double lon) {
  double wx = 0, wy = 0;
  mapLatLonToWorld(lat, lon, zoom, &wx, &wy);
  cx = (int32_t)(wx + 0.5);
  cy = (int32_t)(wy + 0.5);
  mapClampView(zoom, vpW, vpH, &cx, &cy);
}

void MapsApp::centreOnMe() {
  int32_t la = 0, lo = 0;
  uint32_t age = 0;
  if (gGpsNmea && meshService.getGpsFix(&la, &lo, &age, NULL, NULL)) {
    centreOn(mapI7ToDeg(la), mapI7ToDeg(lo));
    setNote("Centred on the GPS fix (%us old)", (unsigned)(age / 1000u));
    return;
  }
  char nm[24];
  if (meshService.resolveReference(&la, &lo, nm, sizeof(nm))) {
    centreOn(mapI7ToDeg(la), mapI7ToDeg(lo));
    setNote("No GPS fix - centred on %s", nm);
    return;
  }
  if (!gGpsNmea) {
    setNote("GPS is off - turn it on in Meshtastic > My node");
  } else {
    setNote("No fix and no reference place yet");
  }
}

int MapsApp::pinUnderCrosshair() {
  if (!pins || pinCount <= 0) {
    return -1;
  }
  for (int i = 0; i < pinCount; i++) {
    mapLatLonToView(zoom, cx, cy, vpW, vpH,
                    mapI7ToDeg(pins[i].latI), mapI7ToDeg(pins[i].lonI),
                    &pinVx[i], &pinVy[i]);
  }
  return mapPinPickNearest(pinVx, pinVy, pinCount, vpW / 2, vpH / 2, MAPS_PICK_RADIUS);
}

bool MapsApp::dropPin() {
  if (!pins) {
    setNote("No memory for pins");
    return false;
  }
  if (pinCount >= MAPS_MAX_PINS) {
    setNote("Full: %d pins. Delete one first.", MAPS_MAX_PINS);
    return false;
  }
  double lat = 0, lon = 0;
  mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &lat, &lon);
  MapPin* p = &pins[pinCount];
  memset(p, 0, sizeof(*p));
  p->latI = mapDegToI7(lat);
  p->lonI = mapDegToI7(lon);
  p->sharedId = 0;
  mapPinAutoName(pins, pinCount, p->name, sizeof(p->name));
  pinSel = pinCount;
  pinCount++;
  pinsDirty = true;
  savePins();                    // a pin that vanishes because the battery died is not a pin
  return true;
}

bool MapsApp::sharePin(int idx, char* why, size_t whyCap) {
  if (idx < 0 || idx >= pinCount || !pins) {
    strlcpy(why, "no pin", whyCap);
    return false;
  }
  bool onAir = false;
  const uint32_t id = meshService.shareWaypoint(pins[idx].sharedId,
                                                pins[idx].latI, pins[idx].lonI,
                                                pins[idx].name, 0, &onAir);
  if (!id) {
    strlcpy(why, "Could not share - the mesh refused it", whyCap);
    return false;
  }
  pins[idx].sharedId = id;
  pinsDirty = true;
  savePins();
  /* 🔑 SAY WHETHER IT ACTUALLY LEFT THE PHONE. A place that is on your map and nobody else's,
   * shown as shared, is the failure the pin-announce path was built to avoid: it reads as
   * "they know where camp is" when nobody does. */
  if (onAir) {
    snprintf(why, whyCap, "Shared '%s' on the mesh", pins[idx].name);
  } else {
    snprintf(why, whyCap, "Saved, but NOT sent - radio not ready");
  }
  return onAir;
}

void MapsApp::deletePin(int idx) {
  if (idx < 0 || idx >= pinCount || !pins) {
    return;
  }
  if (pins[idx].sharedId) {
    /* It is on other people's maps too. Taking it off theirs is the point of deleting it —
     * a camp that has moved is worse than no camp at all. */
    meshService.unshareWaypoint(pins[idx].sharedId);
  }
  for (int i = idx; i + 1 < pinCount; i++) {
    pins[i] = pins[i + 1];       // keep the order: the list on screen must not reshuffle
  }
  pinCount--;
  if (pinSel >= pinCount) {
    pinSel = pinCount - 1;
  }
  pinsDirty = true;
  savePins();
}

void MapsApp::stepPin(int delta) {
  if (!pins || pinCount <= 0) {
    setNote("No pins yet - press 5 to drop one");
    return;
  }
  pinSel += delta;
  if (pinSel < 0) {
    pinSel = pinCount - 1;
  }
  if (pinSel >= pinCount) {
    pinSel = 0;
  }
  centreOn(mapI7ToDeg(pins[pinSel].latI), mapI7ToDeg(pins[pinSel].lonI));
  setNote("%s (%d of %d)", pins[pinSel].name, pinSel + 1, pinCount);
}

void MapsApp::setArea(int idx) {
  if (idx < 0 || idx >= areaCount) {
    return;
  }
  // Where we are looking, in ground terms, before the zoom range changes under us.
  double lat = 0, lon = 0;
  mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &lat, &lon);
  areaSel = idx;
  cancelLoad();
  forgetMissing();
  for (int i = 0; i < MAPS_TILE_SLOTS; i++) {
    slots[i].z = -1;             // the cache is full of another map's ground
    slots[i].ready = false;
  }
  const MapArea* a = &areas[areaSel];
  if (zoom < a->zMin) {
    zoom = a->zMin;
  }
  if (zoom > a->zMax) {
    zoom = a->zMax;
  }
  centreOn(lat, lon);
  setNote("Map: %s (z%d-%d)", a->name, a->zMin, a->zMax);
}

// ---------------------------------------------------------------- screens

void MapsApp::freeWidgets() {
  if (menu) {
    delete menu;
    menu = NULL;
  }
  if (textArea) {
    delete textArea;
    textArea = NULL;
  }
}

void MapsApp::freeMenu() {
  if (menu) {
    delete menu;
    menu = NULL;
  }
}

MenuWidget* MapsApp::newMenu(const char* emptyMessage) {
  freeMenu();                    // see the note in app_meshtastic.cpp: rebuilds used to leak
  MenuWidget* m = new MenuWidget(0, header->height(), lcd.width(),
                                 lcd.height() - header->height() - footer->height(),
                                 emptyMessage, fonts[AKROBAT_BOLD_20], 0, 8);
  m->setStyle(MenuWidget::DEFAULT_STYLE, WHITE, BLACK, BLACK, GREEN);
  return m;
}

void MapsApp::enterState(MapsState_t st) {
  appState = st;
  freeWidgets();
  controlState.setInputState(InputType::Numeric);

  switch (st) {
  case MAPS_VIEW:
    snprintf(headerTitle, sizeof(headerTitle), "Maps");
    header->setTitle(headerTitle);
    footer->setButtons("Select", "Back");
    break;
  case MAPS_MENU:
    snprintf(headerTitle, sizeof(headerTitle), "Map menu");
    header->setTitle(headerTitle);
    footer->setButtons("Select", "Map");
    buildMenu();
    break;
  case MAPS_LIST:
    snprintf(headerTitle, sizeof(headerTitle), "%s",
             listKind == LIST_PINS ? "Pins" : (listKind == LIST_PLACES ? "Places" : "Nodes"));
    header->setTitle(headerTitle);
    footer->setButtons("Go to", "Back");
    buildList();
    break;
  case MAPS_PIN_OPTS:
    snprintf(headerTitle, sizeof(headerTitle), "%s",
             (pinSel >= 0 && pinSel < pinCount) ? pins[pinSel].name : "Pin");
    header->setTitle(headerTitle);
    footer->setButtons("Select", "Back");
    buildPinOpts();
    break;
  case MAPS_RENAME:
    snprintf(headerTitle, sizeof(headerTitle), "Rename pin");
    header->setTitle(headerTitle);
    footer->setButtons("Save", "Cancel");
    buildRename();
    break;
  case MAPS_AREAS:
    snprintf(headerTitle, sizeof(headerTitle), "Map area");
    header->setTitle(headerTitle);
    footer->setButtons("Use", "Back");
    buildAreas();
    break;
  case MAPS_CONFIRM_DEL:
    snprintf(headerTitle, sizeof(headerTitle), "Delete pin?");
    header->setTitle(headerTitle);
    footer->setButtons("Select", "Back");
    buildConfirmDelete();
    break;
  case MAPS_HELP:
    snprintf(headerTitle, sizeof(headerTitle), "Map keys");
    header->setTitle(headerTitle);
    footer->setButtons("", "Back");
    break;
  }
}

void MapsApp::buildMenu() {
  menu = newMenu("");
  char row[64];
  snprintf(row, sizeof(row), "Pins (%d)...", pinCount);
  menu->addOption(row, ROW_M_PINS);
  snprintf(row, sizeof(row), "Places from the mesh (%d)...", meshService.getWaypointCount());
  menu->addOption(row, ROW_M_PLACES);
  snprintf(row, sizeof(row), "Nodes with a position...");
  menu->addOption(row, ROW_M_NODES);
  menu->addOption("Centre on me", ROW_M_ME);
  if (areaCount > 1) {
    snprintf(row, sizeof(row), "Map area: %s", areaSel >= 0 ? areas[areaSel].name : "-");
    menu->addOption(row, ROW_M_AREA);
  }
  menu->addOption("Rescan the card", ROW_M_RESCAN);
  menu->addOption("Keys and colours...", ROW_M_HELP);
  menu->addOption("Back to the map", ROW_M_BACK);
  if (areaSel < 0) {
    menu->addNote("No tiles on the card - see docs/maps.md");
  }
}

void MapsApp::buildList() {
  const char* empty = (listKind == LIST_PINS)
                      ? "No pins yet - press 5 on the map to drop one"
                      : (listKind == LIST_PLACES ? "No places heard from the mesh yet"
                         : "No node has sent a position yet");
  menu = newMenu(empty);
  char row[72];
  if (listKind == LIST_PINS) {
    for (int i = 0; i < pinCount; i++) {
      snprintf(row, sizeof(row), "%s%s", pins[i].name, pins[i].sharedId ? " (shared)" : "");
      menu->addOption(row, ROW_FIRST + i);
    }
  } else if (listKind == LIST_PLACES) {
    const int n = meshService.getWaypointCount();
    for (int i = 0; i < n; i++) {
      const MeshWaypoint* w = meshService.getWaypoint(i);
      if (!w || !w->id) {
        continue;
      }
      snprintf(row, sizeof(row), "%s", w->name);
      menu->addOption(row, ROW_FIRST + i);
    }
  } else {
    const uint32_t me = meshService.getMyNodeNum();
    const uint32_t now = millis();
    const int n = meshService.getNodeCount();
    for (int i = 0; i < n; i++) {
      const MeshNode* nd = meshService.getNode(i);
      if (!nd || !nd->nodeNum || nd->nodeNum == me || !nd->posHeardMs) {
        continue;
      }
      const unsigned mins = (unsigned)((now - nd->posHeardMs) / 60000u);
      snprintf(row, sizeof(row), "%s - %um ago", nd->name[0] ? nd->name : "?", mins);
      menu->addOption(row, ROW_FIRST + i);
    }
  }
}

void MapsApp::buildPinOpts() {
  menu = newMenu("");
  if (pinSel < 0 || pinSel >= pinCount) {
    menu->addNote("That pin is gone");
    menu->addOption("Back", ROW_P_CANCEL);
    return;
  }
  char row[64];
  menu->addOption("Rename...", ROW_P_RENAME);
  menu->addOption("Move it to the crosshair", ROW_P_MOVE);
  if (pins[pinSel].sharedId) {
    menu->addOption("Update it on the mesh", ROW_P_SHARE);
    menu->addOption("Take it off the mesh", ROW_P_UNSHARE);
  } else {
    menu->addOption("Share it on the mesh", ROW_P_SHARE);
  }
  menu->addOption("Centre the map on it", ROW_P_CENTRE);
  menu->addOption("Delete", ROW_P_DELETE);
  menu->addOption("Back", ROW_P_CANCEL);
  snprintf(row, sizeof(row), "%.5f, %.5f",
           mapI7ToDeg(pins[pinSel].latI), mapI7ToDeg(pins[pinSel].lonI));
  menu->addNote(row);
}

void MapsApp::buildRename() {
  freeMenu();
  delete textArea;
  const int16_t padding = 4;
  textArea = new MultilineTextWidget(0, header->height(), lcd.width(),
                                     lcd.height() - header->height() - footer->height(),
                                     "Pin name", controlState, MAP_PIN_NAME_LEN - 1,
                                     fonts[OPENSANS_COND_BOLD_20], InputType::AlphaNum,
                                     padding, padding);
  textArea->setColors(WP_COLOR_1, WP_COLOR_0);
  if (pinSel >= 0 && pinSel < pinCount) {
    textArea->setText(pins[pinSel].name);
  }
  textArea->cursorToEnd();
  textArea->setFocus(true);
  controlState.setInputState(InputType::AlphaNum);
}

void MapsApp::buildAreas() {
  menu = newMenu("No maps on the card");
  char row[72];
  for (int i = 0; i < areaCount; i++) {
    snprintf(row, sizeof(row), "%s  z%d-%d%s", areas[i].name, areas[i].zMin, areas[i].zMax,
             i == areaSel ? "  *" : "");
    menu->addOption(row, ROW_FIRST + i);
  }
}

void MapsApp::buildConfirmDelete() {
  menu = newMenu("");
  char row[72];
  if (pinSel >= 0 && pinSel < pinCount) {
    snprintf(row, sizeof(row), "Delete '%s'", pins[pinSel].name);
    menu->addNote(row);
    if (pins[pinSel].sharedId) {
      menu->addNote("...and take it off the mesh");
    }
  }
  menu->addOption("No, keep it", ROW_DEL_NO);
  menu->addOption("Yes, delete it", ROW_DEL_YES);
}

void MapsApp::drawHelp() {
  lcd.fillRect(0, header->height(), lcd.width(),
               lcd.height() - header->height() - footer->height(), BLACK);
  SmoothFont* fnt = fonts[AKROBAT_BOLD_16];
  if (!fnt) {
    return;
  }
  lcd.setTextFont(fnt);
  struct HelpRow { const char* text; uint16_t colour; };
  static const HelpRow rows[] = {
    { "Arrows      scroll (hold = faster)", WHITE },
    { "2 4 6 8     scroll too", WHITE },
    { "* or 1      zoom out", WHITE },
    { "# or 3      zoom in", WHITE },
    { "5           drop a pin on the crosshair", WHITE },
    { "OK          the pin under the crosshair,", WHITE },
    { "            or the menu if there is none", WHITE },
    { "7 / 9       previous / next pin", WHITE },
    { "0           centre on me", WHITE },
    { "Back        leave (the view is saved)", WHITE },
    { "", WHITE },
    { "orange dot   your pin", MAP_C_PIN },
    { "yellow dot   your pin, shared", MAP_C_PIN_SH },
    { "green gem    place from the mesh", MAP_C_WP },
    { "cyan dot     someone else", MAP_C_NODE },
    { "grey dot     ...over 30 min old", MAP_C_NODE_OLD },
    { "white ring   this phone (GPS)", MAP_C_ME },
  };
  int y = (int)header->height() + 4;
  const int bottom = (int)lcd.height() - (int)footer->height();
  for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    if (y + (int)fnt->height() > bottom) {
      break;
    }
    if (rows[i].text[0]) {
      lcd.setTextColor(rows[i].colour, BLACK);
      lcd.drawString(rows[i].text, 6, y);
    }
    y += (int)fnt->height() + 1;
  }
}

// ---------------------------------------------------------------- events

appEventResult MapsApp::onMapKey(EventType event) {
  /* One place clears the note, and it is here: ANY key on the map retires the last one, and
   * a handler that has something new to say says it below. Clearing per-branch is how a
   * message from ten minutes ago ends up sitting under a map nobody is looking at any more —
   * and how one branch gets forgotten. */
  note[0] = '\0';

  int dx = 0, dy = 0;
  switch (event) {
  case WIPHONE_KEY_UP:
  case '2':
    dy = -1;
    break;
  case WIPHONE_KEY_DOWN:
  case '8':
    dy = 1;
    break;
  case WIPHONE_KEY_LEFT:
  case '4':
    dx = -1;
    break;
  case WIPHONE_KEY_RIGHT:
  case '6':
    dx = 1;
    break;
  default:
    break;
  }

  if (dx || dy) {
    /* The accelerator. A run is presses in the SAME direction arriving close together; any
     * pause or change of direction starts again at a nudge, so a deliberate correction after
     * a long sweep is still a correction and not another sweep. */
    const uint32_t now = millis();
    const int dir = (dx ? (dx > 0 ? 1 : 2) : 0) + (dy ? (dy > 0 ? 10 : 20) : 0);
    if (dir == panDir && (uint32_t)(now - panLastMs) < MAP_PAN_RUN_MS) {
      if (panRun < 1000) {
        panRun++;
      }
    } else {
      panRun = 0;
    }
    panDir = dir;
    panLastMs = now;
    const int step = mapPanStep(panRun);
    if (!mapPanView(zoom, vpW, vpH, dx * step, dy * step, &cx, &cy) && dy) {
      /* Only north/south can refuse: longitude wraps, so a sideways press always moves. */
      setNote("That is as far %s as the map goes", dy < 0 ? "north" : "south");
    }
    return REDRAW_SCREEN;
  }

  switch (event) {
  case '*':
  case '1': {
    const int zMin = areaSel >= 0 ? areas[areaSel].zMin : zoom;
    const int zMax = areaSel >= 0 ? areas[areaSel].zMax : zoom;
    const int nz = mapZoomView(zMin, zMax, zoom, -1, vpW, vpH, &cx, &cy);
    if (nz == zoom) {
      setNote("z%d is the widest this map has", zoom);
    } else {
      cancelLoad();              // the tile on its way is for a zoom nobody is looking at
      zoom = nz;
    }
    return REDRAW_SCREEN;
  }
  case '#':
  case '3': {
    const int zMin = areaSel >= 0 ? areas[areaSel].zMin : zoom;
    const int zMax = areaSel >= 0 ? areas[areaSel].zMax : zoom;
    const int nz = mapZoomView(zMin, zMax, zoom, +1, vpW, vpH, &cx, &cy);
    if (nz == zoom) {
      setNote("z%d is the closest this map has", zoom);
    } else {
      cancelLoad();
      zoom = nz;
    }
    return REDRAW_SCREEN;
  }
  case '5':
    if (dropPin()) {
      setNote("Dropped '%s' - name it", pins[pinSel].name);
      enterState(MAPS_RENAME);
      return REDRAW_ALL;
    }
    return REDRAW_SCREEN;
  case '7':
    stepPin(-1);
    return REDRAW_SCREEN;
  case '9':
    stepPin(1);
    return REDRAW_SCREEN;
  case '0':
    centreOnMe();
    return REDRAW_SCREEN;
  default:
    break;
  }

  if (LOGIC_BUTTON_OK(event)) {
    const int hit = pinUnderCrosshair();
    if (hit >= 0) {
      pinSel = hit;
      enterState(MAPS_PIN_OPTS);
      return REDRAW_ALL;
    }
    enterState(MAPS_MENU);
    return REDRAW_ALL;
  }
  return DO_NOTHING;
}

appEventResult MapsApp::processEvent(EventType event) {
  /* A position or a waypoint arrived (WiPhone.ino forwards it as this). Redraw the map so a
   * marker that has moved actually moves — but only the map: this must never steal a
   * keypress or rebuild a menu under the user's thumb. */
  if (event == NEW_MESSAGE_EVENT) {
    return (appState == MAPS_VIEW) ? REDRAW_SCREEN : DO_NOTHING;
  }

  if (event == APP_TIMER_EVENT) {
    if (!loadActive && wantAny && !startLoad(wantZ, wantTx, wantTy)) {
      /* ⚠ A REFUSED START MUST NOT BE RETRIED AT 40 Hz. startLoad only refuses for reasons
       * that will still be true in 25 ms — no area, no PSRAM, no free slot — so asking again
       * immediately is a spin that keeps the CPU awake and achieves nothing. Forget the want
       * and stand the timer down; the next repaint re-discovers the hole if it still matters. */
      wantAny = false;
      controlState.msAppTimerEventPeriod = 0;
      return DO_NOTHING;
    }
    if (loadActive && tileLoadStep()) {
      return (appState == MAPS_VIEW) ? REDRAW_SCREEN : DO_NOTHING;
    }
    /* Deliberately no redraw while a piece is merely in flight: repainting 240x250 to change
     * nothing is the sort of busy-work that shows up as a warm phone and a flat battery. */
    return DO_NOTHING;
  }

  switch (appState) {

  case MAPS_VIEW:
    if (LOGIC_BUTTON_BACK(event)) {
      return EXIT_APP;           // the destructor saves the view
    }
    return onMapKey(event);

  case MAPS_MENU:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MAPS_VIEW);
      return REDRAW_ALL;
    }
    if (menu) {
      menu->processEvent(event);
    }
    if (LOGIC_BUTTON_OK(event) && menu) {
      const MenuOption::keyType sel = menu->readChosen();
      switch (sel) {
      case ROW_M_PINS:
        listKind = LIST_PINS;
        enterState(MAPS_LIST);
        return REDRAW_ALL;
      case ROW_M_PLACES:
        listKind = LIST_PLACES;
        enterState(MAPS_LIST);
        return REDRAW_ALL;
      case ROW_M_NODES:
        listKind = LIST_NODES;
        enterState(MAPS_LIST);
        return REDRAW_ALL;
      case ROW_M_ME:
        centreOnMe();
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      case ROW_M_AREA:
        enterState(MAPS_AREAS);
        return REDRAW_ALL;
      case ROW_M_RESCAN: {
        char was[32];
        was[0] = '\0';
        if (areaSel >= 0) {
          strlcpy(was, areas[areaSel].name, sizeof(was));
        }
        cancelLoad();
        forgetMissing();
        for (int i = 0; i < MAPS_TILE_SLOTS; i++) {
          slots[i].z = -1;
          slots[i].ready = false;
        }
        scanAreas();
        loadPins();
        pinSel = -1;               // the array was just re-read; the old index means nothing
        areaSel = -1;
        for (int i = 0; i < areaCount; i++) {
          if (was[0] && !strcmp(areas[i].name, was)) {
            areaSel = i;
          }
        }
        if (areaSel < 0 && areaCount > 0) {
          areaSel = 0;
        }
        if (areaSel >= 0) {
          if (zoom < areas[areaSel].zMin) {
            zoom = areas[areaSel].zMin;
          }
          if (zoom > areas[areaSel].zMax) {
            zoom = areas[areaSel].zMax;
          }
        }
        setNote("%d map%s, %d pin%s on the card", areaCount, areaCount == 1 ? "" : "s",
                pinCount, pinCount == 1 ? "" : "s");
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      }
      case ROW_M_HELP:
        enterState(MAPS_HELP);
        return REDRAW_ALL;
      case ROW_M_BACK:
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      default:
        break;
      }
    }
    return REDRAW_SCREEN;

  case MAPS_LIST:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MAPS_MENU);
      return REDRAW_ALL;
    }
    if (menu) {
      menu->processEvent(event);
    }
    if (LOGIC_BUTTON_OK(event) && menu) {
      const MenuOption::keyType sel = menu->readChosen();
      /* ⚠ BEFORE the range test. MENU_ROW_NOTE minus ROW_FIRST underflows and casts to a
       * NEGATIVE index, which sails straight through an `idx < count` bounds check — the
       * trap Files, Books and Music each carry a guard for. */
      if (sel == MENU_ROW_NOTE || sel < ROW_FIRST || sel >= ROW_ACT) {
        return REDRAW_SCREEN;
      }
      const int idx = (int)(sel - ROW_FIRST);
      if (listKind == LIST_PINS) {
        if (idx < pinCount) {
          pinSel = idx;
          centreOn(mapI7ToDeg(pins[idx].latI), mapI7ToDeg(pins[idx].lonI));
          enterState(MAPS_PIN_OPTS);
          return REDRAW_ALL;
        }
      } else if (listKind == LIST_PLACES) {
        const MeshWaypoint* w = meshService.getWaypoint(idx);
        if (w && w->id) {
          centreOn(mapI7ToDeg(w->latI), mapI7ToDeg(w->lonI));
          setNote("%s", w->name);
          enterState(MAPS_VIEW);
          return REDRAW_ALL;
        }
      } else {
        const MeshNode* nd = meshService.getNode(idx);
        if (nd && nd->posHeardMs) {
          centreOn(mapI7ToDeg(nd->latI), mapI7ToDeg(nd->lonI));
          setNote("%s", nd->name[0] ? nd->name : "?");
          enterState(MAPS_VIEW);
          return REDRAW_ALL;
        }
      }
      return REDRAW_SCREEN;
    }
    return REDRAW_SCREEN;

  case MAPS_PIN_OPTS:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MAPS_VIEW);
      return REDRAW_ALL;
    }
    if (menu) {
      menu->processEvent(event);
    }
    if (LOGIC_BUTTON_OK(event) && menu) {
      const MenuOption::keyType sel = menu->readChosen();
      if (pinSel < 0 || pinSel >= pinCount) {
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      }
      switch (sel) {
      case ROW_P_RENAME:
        enterState(MAPS_RENAME);
        return REDRAW_ALL;
      case ROW_P_MOVE: {
        double lat = 0, lon = 0;
        mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &lat, &lon);
        pins[pinSel].latI = mapDegToI7(lat);
        pins[pinSel].lonI = mapDegToI7(lon);
        pinsDirty = true;
        savePins();
        if (pins[pinSel].sharedId) {
          /* It was shared from its old spot, so the mesh is holding the old one. Moving it
           * here and not there would leave two truths. */
          char why[72];
          sharePin(pinSel, why, sizeof(why));
          setNote("Moved. %s", why);
        } else {
          setNote("Moved '%s' here", pins[pinSel].name);
        }
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      }
      case ROW_P_SHARE: {
        char why[72];
        sharePin(pinSel, why, sizeof(why));
        setNote("%s", why);
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      }
      case ROW_P_UNSHARE: {
        const bool ok = meshService.unshareWaypoint(pins[pinSel].sharedId);
        pins[pinSel].sharedId = 0;
        pinsDirty = true;
        savePins();
        setNote(ok ? "Taken off the mesh" : "Removed here, but the radio did not send");
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      }
      case ROW_P_CENTRE:
        centreOn(mapI7ToDeg(pins[pinSel].latI), mapI7ToDeg(pins[pinSel].lonI));
        setNote("%s", pins[pinSel].name);
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      case ROW_P_DELETE:
        enterState(MAPS_CONFIRM_DEL);
        return REDRAW_ALL;
      case ROW_P_CANCEL:
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      default:
        break;
      }
    }
    return REDRAW_SCREEN;

  case MAPS_CONFIRM_DEL:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MAPS_PIN_OPTS);
      return REDRAW_ALL;
    }
    if (menu) {
      menu->processEvent(event);
    }
    if (LOGIC_BUTTON_OK(event) && menu) {
      const MenuOption::keyType sel = menu->readChosen();
      if (sel == ROW_DEL_YES) {
        char gone[MAP_PIN_NAME_LEN];
        gone[0] = '\0';
        if (pinSel >= 0 && pinSel < pinCount) {
          strlcpy(gone, pins[pinSel].name, sizeof(gone));
        }
        deletePin(pinSel);
        setNote("Deleted '%s'", gone);
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      }
      if (sel == ROW_DEL_NO) {
        enterState(MAPS_PIN_OPTS);
        return REDRAW_ALL;
      }
    }
    return REDRAW_SCREEN;

  case MAPS_AREAS:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MAPS_MENU);
      return REDRAW_ALL;
    }
    if (menu) {
      menu->processEvent(event);
    }
    if (LOGIC_BUTTON_OK(event) && menu) {
      const MenuOption::keyType sel = menu->readChosen();
      if (sel != MENU_ROW_NOTE && sel >= ROW_FIRST && sel < ROW_ACT) {
        setArea((int)(sel - ROW_FIRST));
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      }
    }
    return REDRAW_SCREEN;

  case MAPS_RENAME:
    /* 🛑 CANCEL IS END, NOT BACK — Back is BACKSPACE in a text field. Testing
     * LOGIC_BUTTON_BACK here would consume the key before the widget saw it, and the field
     * would have no way to delete a character: exactly the bug Photos shipped with
     * (app_photos.cpp), and the convention every other text screen here already follows. */
    if (event == WIPHONE_KEY_END) {
      controlState.setInputState(InputType::Numeric);
      /* The pin keeps the name it was given automatically - cancelling the FIELD is not
       * cancelling the PIN, and a person who pressed 5 meant to mark this spot. Say which
       * name it kept, rather than leaving "name it" sitting under a map nobody is naming. */
      if (pinSel >= 0 && pinSel < pinCount) {
        setNote("Kept the name '%s'", pins[pinSel].name);
      } else {
        note[0] = '\0';
      }
      enterState(MAPS_VIEW);
      return REDRAW_ALL;
    }
    if (LOGIC_BUTTON_OK(event)) {
      if (pinSel >= 0 && pinSel < pinCount) {
        mapPinSanitizeName(textArea ? textArea->getText() : NULL,
                           pins[pinSel].name, sizeof(pins[pinSel].name));
        pinsDirty = true;
        savePins();
        if (pins[pinSel].sharedId) {
          char why[72];
          sharePin(pinSel, why, sizeof(why));   // the mesh holds the old name otherwise
          setNote("Renamed. %s", why);
        } else {
          setNote("Named '%s'", pins[pinSel].name);
        }
      }
      controlState.setInputState(InputType::Numeric);
      enterState(MAPS_VIEW);
      return REDRAW_ALL;
    }
    if (textArea) {
      textArea->processEvent(event);
    }
    return REDRAW_SCREEN;

  case MAPS_HELP:
    if (LOGIC_BUTTON_BACK(event) || LOGIC_BUTTON_OK(event)) {
      enterState(MAPS_MENU);
      return REDRAW_ALL;
    }
    return DO_NOTHING;
  }
  return DO_NOTHING;
}

void MapsApp::redrawScreen(bool redrawAll) {
  switch (appState) {
  case MAPS_VIEW:
    if (areaSel < 0 && pinCount == 0 && meshService.getWaypointCount() == 0) {
      /* Nothing at all to draw: say what to do about it rather than showing a black
       * rectangle with a crosshair on it. With any pin or place to show, the map is drawn
       * (blank behind the markers) because the markers are the point. */
      drawNoMapPage();
    } else {
      drawMap();
    }
    break;
  case MAPS_HELP:
    drawHelp();
    break;
  case MAPS_RENAME:
    if (textArea) {
      ((GUIWidget*)textArea)->redraw(lcd);
    }
    break;
  default:
    if (menu) {
      ((GUIWidget*)menu)->redraw(lcd);
    }
    break;
  }

  /* ⚠ THE TIMER IS ARMED HERE, AT THE END OF THE DRAW, because this is the only place that
   * knows whether a tile is still missing — drawMap() is what discovers it. 25 ms while there
   * is reading to do, nothing at all otherwise: a map that is fully painted must not keep the
   * CPU awake, and the phone that runs this is one somebody is carrying all day. */
  const bool busy = (appState == MAPS_VIEW) && (loadActive || (wantAny && !tileMemFail));
  controlState.msAppTimerEventPeriod = busy ? 25 : 0;
}

// ---------------------------------------------------------------- the serial console

/* See the note in app_maps.h: a map's failure mode is showing the wrong ground, which looks
 * exactly like showing the right ground, and no screenshot can tell them apart. These two
 * functions are how the whole thing is driven over a cable. */
/* ⚠ `w += snprintf(out + w, cap - w, ...)` IS A BUFFER OVERFLOW WAITING FOR A LONG ENOUGH
 * AREA NAME. snprintf returns what it WOULD have written, so on truncation w passes cap, and
 * `cap - w` is size_t: it underflows to about four billion and the next call writes off the
 * end. This is the standard trap in the standard shape, and the fix is to append through one
 * function that cannot get it wrong. */
static size_t sayInto(char* out, size_t cap, size_t w, const char* fmt, ...) {
  if (!out || w >= cap) {
    return w;
  }
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(out + w, cap - w, fmt, ap);
  va_end(ap);
  if (n < 0) {
    return w;
  }
  const size_t room = cap - w - 1;
  return w + ((size_t)n > room ? room : (size_t)n);
}

bool mapsConsoleStatus(char* out, size_t cap) {
  if (!out || cap == 0) {
    return false;
  }
  out[0] = '\0';
  size_t w = 0;
  int areaN = 0;
  char first[32];
  first[0] = '\0';
  int zMin = -1, zMax = -1;
  if (SD.exists(MAPS_ROOT)) {
    File dir = SD.open(MAPS_ROOT);
    if (dir && dir.isDirectory()) {
      File f;
      while ((f = dir.openNextFile())) {
        const char* nm = baseName(f.name());
        if (f.isDirectory() && mapAreaNameOk(nm)) {
          if (!first[0]) {
            strlcpy(first, nm, sizeof(first));
            char zp[96];
            snprintf(zp, sizeof(zp), "%s/%s", MAPS_ROOT, nm);
            File zd = SD.open(zp);
            if (zd && zd.isDirectory()) {
              File zf;
              while ((zf = zd.openNextFile())) {
                const int z = zf.isDirectory() ? zoomFolder(baseName(zf.name())) : -1;
                if (z >= 0) {
                  if (zMin < 0 || z < zMin) {
                    zMin = z;
                  }
                  if (z > zMax) {
                    zMax = z;
                  }
                }
                zf.close();
              }
            }
            if (zd) {
              zd.close();
            }
          }
          areaN++;
        }
        f.close();
      }
    }
    if (dir) {
      dir.close();
    }
  }
  w = sayInto(out, cap, w, "maps: %d area(s) under %s", areaN, MAPS_ROOT);
  if (first[0]) {
    w = sayInto(out, cap, w, "; first '%s' z%d-%d", first, zMin, zMax);
  }

  Preferences p;
  if (p.begin(MAPS_NVS, true)) {
    if (p.getInt("saved", 0) == 1) {
      char area[32];
      area[0] = '\0';
      p.getBytes("area", area, sizeof(area));
      area[sizeof(area) - 1] = '\0';
      w = sayInto(out, cap, w, "\nlast view: %.5f,%.5f z%d area '%s'",
                  mapI7ToDeg(p.getInt("lat", 0)), mapI7ToDeg(p.getInt("lon", 0)),
                  p.getInt("z", 0), area);
    } else {
      w = sayInto(out, cap, w, "\nlast view: none saved yet");
    }
    p.end();
  }
  {
    File f = SD.open(MAPS_PINS_FILE, FILE_READ);
    int pinN = 0, badN = 0;
    if (f) {
      char line[MAP_PIN_LINE_MAX];
      size_t n = 0;
      MapPin tmp;
      while (f.available()) {
        const int ch = f.read();
        if (ch < 0) {
          break;
        }
        if (ch == '\n' || ch == '\r') {
          line[n] = '\0';
          if (n) {
            const int r = mapPinParseLine(line, &tmp);
            if (r == 1) {
              pinN++;
            } else if (r < 0) {
              badN++;
            }
          }
          n = 0;
          continue;
        }
        if (n + 1 < sizeof(line)) {
          line[n++] = (char)ch;
        }
      }
      f.close();
    }
    w = sayInto(out, cap, w, "\npins: %d good, %d unreadable in %s",
                pinN, badN, MAPS_PINS_FILE);
    (void)w;
  }
  return true;
}

bool mapsConsoleGoto(double lat, double lon, int z, const char* area,
                     char* why, size_t whyCap) {
  if (lat < -90 || lat > 90 || lon < -180 || lon > 180) {
    snprintf(why, whyCap, "lat must be -90..90 and lon -180..180");
    return false;
  }
  if (z < MAP_ZOOM_MIN || z > MAP_ZOOM_MAX) {
    snprintf(why, whyCap, "zoom must be %d..%d", MAP_ZOOM_MIN, MAP_ZOOM_MAX);
    return false;
  }
  Preferences p;
  if (!p.begin(MAPS_NVS, false)) {
    snprintf(why, whyCap, "could not open the maps settings");
    return false;
  }
  p.putInt("lat", mapDegToI7(mapClampLat(lat)));
  p.putInt("lon", mapDegToI7(mapWrapLon(lon)));
  p.putInt("z", z);
  char buf[32];
  memset(buf, 0, sizeof(buf));
  if (area && area[0]) {
    strlcpy(buf, area, sizeof(buf));
  }
  p.putBytes("area", buf, sizeof(buf));
  p.putInt("saved", 1);
  p.end();
  snprintf(why, whyCap, "next open: %.5f,%.5f z%d%s%s", lat, lon, z,
           buf[0] ? " area " : "", buf);
  return true;
}
