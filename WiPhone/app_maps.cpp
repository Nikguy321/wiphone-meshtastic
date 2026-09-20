/* app_maps.cpp — see app_maps.h for the design: why the tiles are raw, why the card is read
 * in pieces, and why a pin is not a waypoint until you say so. */

#include "app_maps.h"
#include "meshtastic_service.h"
#include "mesh_pos.h"
#include <SD.h>
#include <Preferences.h>
#include "menu_wrap.h"      // the strip's note, broken into rows that fit
#include <WiFi.h>
#include "tile_fetch.h"
#include <stdarg.h>

extern bool uiKeyStillHeld(uint32_t mask);   // WiPhone.ino: is this key physically down right now?

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

/* Hold-to-scroll: a press held this long starts repeating, then a repeat every
 * MAP_PAN_HOLD_STEP_MS, climbing mapPanStep()'s curve. 400 ms keeps a deliberate tap a single
 * nudge; 100 ms is just under the chip's ~109 ms heartbeat, so the poll never misses a beat
 * and the release is seen within one step. */
#define MAP_PAN_HOLD_DELAY_MS  400u
#define MAP_PAN_HOLD_STEP_MS   100u
/* ⚠ The delay must be LONGER than uiKeyStillHeld()'s 350 ms staleness window: at 300 ms a
 * tap whose release was lost (two keys down, or any release the keypad's stale sweep exists
 * for) still read as "held" at the first repeat and the map took one phantom step — off the
 * pin the tap had just snapped to (review, 2026-09-19). */

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
  ROW_M_DOWNLOAD   = ROW_ACT + 9,
  ROW_M_GOTO       = ROW_ACT + 10,
  ROW_M_FOLLOW     = ROW_ACT + 11,
  ROW_M_MEASURE    = ROW_ACT + 12,
  ROW_M_SNAP       = ROW_ACT + 13,
  ROW_P_RENAME     = ROW_ACT + 20,
  ROW_P_MOVE       = ROW_ACT + 21,
  ROW_P_SHARE      = ROW_ACT + 22,
  ROW_P_UNSHARE    = ROW_ACT + 23,
  ROW_P_CENTRE     = ROW_ACT + 24,
  ROW_P_DELETE     = ROW_ACT + 25,
  ROW_P_CANCEL     = ROW_ACT + 26,
  ROW_P_SHARE_ON   = ROW_ACT + 27,     // pick a different channel first
  ROW_DEL_YES      = ROW_ACT + 30,
  ROW_DEL_NO       = ROW_ACT + 31,
  ROW_D_SOURCE     = ROW_ACT + 40,
  ROW_D_RADIUS     = ROW_ACT + 41,
  ROW_D_DEPTH      = ROW_ACT + 42,
  ROW_D_START      = ROW_ACT + 43,
  ROW_D_STOP       = ROW_ACT + 44,
  ROW_D_BACK       = ROW_ACT + 45,
  ROW_C_CANCEL     = ROW_ACT + 59,
  ROW_C_BASE       = ROW_ACT + 60,     // channel i is ROW_C_BASE + i
};

/* The download form's choices. Radii are COVEY's; the depth stops at z16 because USGS Topo's
 * z16 is the z15 drawing scaled up (checked 2026-09-18) and every level quadruples the cost. */
static const int MAPS_DL_RADII[] = { 2, 5, 10, 20 };
#define MAPS_DL_RADII_N    ((int)(sizeof(MAPS_DL_RADII) / sizeof(MAPS_DL_RADII[0])))
#define MAPS_DL_DEPTH_MIN  13
#define MAPS_DL_DEPTH_MAX  16
/* Seconds per tile, all-in (fetch, decode, the 128 KB write), measured on WiPhone 2 the night
 * this was built: USGS over kept-alive HTTPS 24 s / 21 tiles; OpenTopoMap 74 s / 18 tiles
 * (server-bound). The estimate on the screen is these times the tile count. */
static const float MAPS_DL_SEC_PER_TILE[] = { 1.2f, 1.2f, 4.0f, 0.6f };
#define MAPS_DL_BATT_FLOOR 3.8f

static MapsApp* s_instance = NULL;
volatile bool gMapsActive = false;

bool mapsSaveOpenView() {
  if (!s_instance) {
    return false;
  }
  s_instance->saveView();
  if (s_instance->pinsDirty) {
    s_instance->savePins();
  }
  return true;
}

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
  loadRetries = 0;
  wantAny = false;
  wantZ = wantTx = wantTy = -1;
  pendingTiles = 0;
  tileMemFail = false;
  cardOk = false;
  pinsTruncated = false;
  panRun = 0;
  panLastMs = 0;
  panHoldMask = 0;
  panHoldSinceMs = 0;
  panHoldDx = panHoldDy = 0;
  panHoldRan = false;
  snapPins = true;
  labelCount = 0;
  listCount = 0;
  helpTop = 0;
  dlSource = 0;
  dlRadiusIdx = 1;                 // 5 km
  dlDepth = 15;
  dlHeld = false;
  dlLastMs = 0;
  dlKeep = ROW_D_START;
  dlWhy[0] = '\0';
  dlShownRunning = false;
  gotoLat[0] = gotoLon[0] = '\0';
  gotoField = 0;
  chanPending = 0;
  chanForPin = -1;
  followMe = false;
  followLastStamp = 0;
  measuring = false;
  measLatI = measLonI = 0;
  {
    /* The last download choices, so a second area is two presses. Same namespace as the view. */
    Preferences p;
    if (p.begin(MAPS_NVS, true)) {
      dlSource = p.getInt("dlsrc", dlSource);
      dlRadiusIdx = p.getInt("dlrad", dlRadiusIdx);
      dlDepth = p.getInt("dldep", dlDepth);
      snapPins = p.getInt("snap", 1) != 0;
      p.end();
    }
    if (dlSource < 0 || dlSource >= tileSourceCount()) dlSource = 0;
    if (dlRadiusIdx < 0 || dlRadiusIdx >= MAPS_DL_RADII_N) dlRadiusIdx = 1;
    if (dlDepth < MAPS_DL_DEPTH_MIN || dlDepth > MAPS_DL_DEPTH_MAX) dlDepth = 15;
  }
  s_instance = this;
  gMapsActive = true;

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
  /* First run, or a phone whose pins have all been deleted: the one key nothing on the screen
   * hints at is the one this app is for. The coordinates it displaces are on the row below on
   * every subsequent open, and the hint goes away the moment any key is pressed. */
  if (pinCount == 0 && !note[0]) {
    setNote("OK drops a pin here - Menu downloads maps");
  }
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
  if (dlHeld) {
    controlState.holdScreenAwake(false);
    dlHeld = false;
  }
  s_instance = NULL;
  gMapsActive = false;
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
  /* Is there a card at all? "No map tiles on the card" sends somebody to the computer to
   * convert tiles; "No SD card" sends them to find the card. Getting those two the wrong way
   * round costs a trip. */
  {
    File root = SD.open("/");
    cardOk = (bool)root && root.isDirectory();
    if (root) {
      root.close();
    }
  }
  if (!cardOk) {
    return;
  }
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
  pinsTruncated = false;
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
        if (pinCount >= MAPS_MAX_PINS) {
          /* 🛑 AND NOW NOTHING MAY SAVE. The file holds more pins than this app can hold, so
           * every later savePins() would write our 64 back over the user's 90 and silently
           * destroy the rest. A card edited on a computer is exactly how that happens. The
           * app stays usable read-only until the file is trimmed. */
          pinsTruncated = true;
        } else if (!overlong) {
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
  if (pinsTruncated) {
    log_e("MAPS: %s holds more than %d pins - refusing to save over it",
          MAPS_PINS_FILE, MAPS_MAX_PINS);
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
  static const char HDR[] = "# WiPhone map pins v1 - latI,lonI (1e-7 deg), shared waypoint id, name\n";
  size_t expect = strlen(HDR);
  f.print(HDR);
  char line[MAP_PIN_LINE_MAX];
  int written = 0;
  for (int i = 0; i < pinCount; i++) {
    const int len = mapPinFormatLine(&pins[i], line, sizeof(line));
    if (len > 0) {
      f.print(line);
      f.print("\n");
      expect += (size_t)len + 1;
      written++;
    }
  }
  f.flush();
  /* ⚠ THE TEST HAS TO BE ABOUT THE PINS. `f.size() > 0` is satisfied by the header comment
   * alone, so a card that accepted the first print and then failed every subsequent one
   * passed this check and was renamed over the real file — the exact loss the temp-file dance
   * exists to prevent. Compare against what we counted on the way in. */
  const size_t got = (size_t)f.size();
  f.close();
  if (got < expect) {
    log_e("MAPS: %s is %u bytes, expected %u - keeping the old file",
          tmp, (unsigned)got, (unsigned)expect);
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
  /* ⚠ memset, NOT just a terminator. putBytes writes all 32 bytes, and everything past the
   * NUL would otherwise be whatever was on this stack frame a moment ago — stack contents
   * committed to flash on every exit from the app, and a different 32 bytes each time, so the
   * NVS page churns for no reason too. */
  char area[32];
  memset(area, 0, sizeof(area));
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
  /* ⚠ isKnownMissing IS PART OF THE REFUSAL, not just a drawing hint. Without it a tile that
   * is not on the card is re-opened on every 25 ms tick forever: tileLoadStep() fails and
   * blacklists it, but nothing redraws (a failed piece returns DO_NOTHING), so `wantAny` still
   * points at it on the next tick and the whole cycle repeats forty times a second — an SD
   * open, a failure and a log line each time, on a phone in somebody's pocket. */
  if (loadActive || tileMemFail || areaSel < 0 || isKnownMissing(z, tx, ty)) {
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
  loadRetries = 0;
  loadActive = true;
  return true;
}

void MapsApp::cancelLoad() {
  /* The want was computed for the view being abandoned, so it is abandoned with it. Leaving it
   * set means the very next tick restarts the load this call exists to stop — a zoom key would
   * cancel a tile and immediately fetch it again for a zoom nobody is looking at. */
  wantAny = false;
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

/* A read or seek that failed part-way. ⚠ NOT THE SAME THING AS "not on the card": the file
 * opened and its length was right, so this is the card or the bus having a moment — and the
 * card in this phone is one the README already warns can throw write errors under load. A
 * blacklist is for the session, so a single hiccup would leave a grey square in the middle of
 * the map until the app was closed and reopened. One retry from the top first; a second
 * failure is real and gets the blacklist. Returns false (no tile finished) either way. */
bool MapsApp::tileReadFailed(const char* what) {
  if (loadRetries < 1) {
    loadRetries++;
    loadGot = 0;                     // start this tile again rather than trusting a torn read
    log_e("MAPS: tile %d/%d/%d %s failed - one more go", loadZ, loadTx, loadTy, what);
    return false;                    // still loadActive: the next tick picks it up
  }
  log_e("MAPS: tile %d/%d/%d %s failed twice - giving up on it", loadZ, loadTx, loadTy, what);
  rememberMissing(loadZ, loadTx, loadTy);
  cancelLoad();
  return false;
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
    return tileReadFailed("seek");
  }
  size_t want = MAP_TILE_BYTES - loadGot;
  if (want > MAPS_CHUNK_BYTES) {
    want = MAPS_CHUNK_BYTES;
  }
  const int got = f.read((uint8_t*)slots[loadSlot].px + loadGot, want);
  f.close();
  if (got <= 0) {
    log_e("MAPS: %s read failed at %u", path, (unsigned)loadGot);
    return tileReadFailed("read");
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

/* Claim a rectangle so no later label lands on it. Silently ignored once the table is full —
 * drawLabel refuses to place anything at that point anyway. */
void MapsApp::reserveBox(int x, int y, int w, int h) {
  if (labelCount >= (int)(sizeof(labelRects) / sizeof(labelRects[0]))) {
    return;
  }
  labelRects[labelCount].x = (int16_t)x;
  labelRects[labelCount].y = (int16_t)y;
  labelRects[labelCount].w = (int16_t)w;
  labelRects[labelCount].h = (int16_t)h;
  labelCount++;
}

void MapsApp::drawMarker(int vx, int vy, uint16_t colour, int shape) {
  const bool diamond = (shape == MAP_SHAPE_DIAMOND);
  drawBlob(vx, vy, 4, BLACK, diamond);     // the surround: markers sit on unknown colours
  drawBlob(vx, vy, 3, colour, diamond);
  /* ⚠ A LABEL MUST NEVER COVER A MARKER — least of all the one it is naming. The label boxes
   * are opaque black, so without this a name sitting slightly left of its neighbour's dot
   * erases that dot, and the map loses a person while gaining a word. */
  reserveBox(vx - 4, vy - 4, 9, 9);
}

bool MapsApp::drawLabel(int vx, int vy, const char* text, uint16_t colour) {
  if (!text || !text[0]) {
    return false;
  }
  SmoothFont* fnt = fonts[AKROBAT_BOLD_16];
  if (!fnt) {
    return false;
  }
  lcd.setTextFont(fnt);
  const int tw = lcd.textWidth(text);
  const int th = (int)fnt->height();
  int bx = vx + 7;
  const int by = vy - th / 2;
  if (bx + tw + 4 > vpX + vpW) {
    bx = vx - 7 - (tw + 4);      // no room on the right: put it on the left
  }
  /* ⚠ A THIRD TRY BEFORE GIVING UP. A label wider than about half the screen fits on NEITHER
   * side of a marker near the middle, so the all-or-nothing rule below silently created a
   * dead band through the centre of the map where long names never appeared — and the centre
   * is where the crosshair is, which is where people put the thing they are looking at.
   * Sliding it flush against the edge keeps it readable and still inside the viewport. */
  if (bx < vpX) {
    bx = vpX;
  }
  if (bx + tw + 4 > vpX + vpW) {
    bx = vpX + vpW - (tw + 4);
  }
  /* ⚠ ALL OR NOTHING. drawString cannot be clipped — it would write straight into the header
   * or the footer — so a label that does not fit entirely inside the map is not drawn at all.
   * Half a name is not information anyway. */
  if (bx < vpX || by < vpY || bx + tw + 4 > vpX + vpW || by + th + 2 > vpY + vpH) {
    return false;
  }
  const int w = tw + 4, h = th + 2;
  /* 🛑 ONCE THE TABLE IS FULL, NOTHING MORE IS DRAWN. The old code kept drawing labels it
   * could no longer record, so every one past the fourteenth was placed with no collision
   * test at all — the overprinting this whole mechanism exists to prevent, arriving exactly
   * when the screen is busiest and needs it most. */
  if (labelCount >= (int)(sizeof(labelRects) / sizeof(labelRects[0]))) {
    return false;
  }
  // Would it land on something already there — a label, or a marker? Then it is not placed.
  for (int i = 0; i < labelCount; i++) {
    const LabelRect* r = &labelRects[i];
    if (bx < r->x + r->w && bx + w > r->x && by < r->y + r->h && by + h > r->y) {
      return false;
    }
  }
  reserveBox(bx, by, w, h);
  lcd.fillRect(bx, by, w, h, BLACK);
  lcd.setTextColor(colour, BLACK);
  lcd.drawString(text, bx + 2, by + 1);
  return true;
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
  /* The byte order of a raw tile is the phone's own: RGB565 little-endian, exactly what
   * color565() produces — which is NOT what a sprite stores (a sprite keeps every pixel
   * byte-swapped, see lcdNativePixels in GUI.h). The first cut of this function set both swap
   * flags FALSE with a comment asserting the opposite, and every tile would have reached the
   * glass with its bytes exchanged. The flag is raised only around the row pushes below. */
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
        lcdNativePixels(lcd, true);
        for (int r = 0; r < q->h; r++) {
          uint16_t* row = slots[si].px + (size_t)(q->srcY + r) * MAP_TILE_PX + q->srcX;
          lcd.pushImage(vpX + q->dstX, vpY + q->dstY + r, (uint16_t)q->w, 1, row);
        }
        lcdNativePixels(lcd, false);
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

/* Where this phone thinks it is, for the marker and for "centre on me". Returns 0 when there
 * is nothing to draw, 1 for a live GPS fix, 2 for the user's declared pin. One function so
 * the marker, the label and the key never disagree about which of the two is being shown. */
int MapsApp::selfPosition(int32_t* latI, int32_t* lonI, uint32_t* ageMs) const {
  uint32_t age = 0;
  int32_t la = 0, lo = 0;
  int sats = 0, hdopX10 = 0;
  const bool haveFix = gGpsNmea && meshService.getGpsFix(&la, &lo, &age, &sats, &hdopX10);
  /* ⚠ THE SAME BAR THE DISTANCE LINES USE. resolveReference() refuses a fix under four
   * satellites or over HDOP 10 (meshPosFixUsable — the "twenty kilometres wrong" fix), and a
   * "centre on me" that trusted one would draw a confident white ring about YOU on the wrong
   * ridge. A fresh but poor fix ranks below a declared pin and is drawn grey, saying why. */
  const bool usable = haveFix && meshPosFixUsable(sats, hdopX10);
  /* 🛑 getGpsFix() RETURNS TRUE FOR A FIX FROM HOURS AGO — it reports "there has ever been
   * one", and the age is the caller's to judge. Drawing that as a live white ring is the same
   * lie the grey node dots exist to avoid, and worse, because it is a lie about YOU: a person
   * checking they are on the right ridge would be looking at where they were before the
   * canopy closed. MESH_GPS_FRESH_MS is the service's own bar and this uses it rather than
   * inventing a second one; past it the ring goes grey and wears its age.
   *
   * The order after that is resolveReference()'s, deliberately: a fresh fix beats everything,
   * then the pin the user declared by hand, then the stale fix. A phone with a pin set this
   * morning and a fix from yesterday should show the pin. */
  if (usable && age < MESH_GPS_FRESH_MS) {
    if (latI) {
      *latI = la;
    }
    if (lonI) {
      *lonI = lo;
    }
    if (ageMs) {
      *ageMs = age;
    }
    return MAPS_SELF_GPS;
  }
  if (meshService.getMyPin(latI, lonI, NULL)) {
    if (ageMs) {
      *ageMs = 0;
    }
    return MAPS_SELF_PIN;
  }
  if (haveFix && !usable && age < MESH_GPS_FRESH_MS) {
    if (latI) {
      *latI = la;
    }
    if (lonI) {
      *lonI = lo;
    }
    if (ageMs) {
      /* For this kind the "age" slot carries the REASON: sats in the low byte, HDOP x10
       * above it. Whichever failed the bar is what the label names. */
      *ageMs = ((uint32_t)(hdopX10 < 0 ? 0 : hdopX10) << 8) | (uint32_t)(sats & 0xFF);
    }
    return MAPS_SELF_POOR_GPS;
  }
  if (haveFix) {
    if (latI) {
      *latI = la;
    }
    if (lonI) {
      *lonI = lo;
    }
    if (ageMs) {
      *ageMs = age;
    }
    return MAPS_SELF_OLD_GPS;
  }
  return MAPS_SELF_NONE;
}

/* ── TWO PASSES, AND THE ORDERS ARE DELIBERATELY OPPOSITE ─────────────────────────────────
 * Markers are drawn back to front, so the one you most need to see ends up on top: other
 * people, then the mesh's places, then your own pins, then this phone.
 *
 * Labels are drawn afterwards in the REVERSE order, because the first label placed wins the
 * space (drawLabel drops one that would overlap). With eight places and twenty nodes in view
 * on a 240x250 screen, unmanaged labels overprint into a grey smear and the map stops
 * answering the question it exists to answer. A dropped label still leaves its MARKER, which
 * is the part that says where somebody is, and the name is on the Nodes list regardless.
 *
 * Node order is the service's own display order — starred first, then most recently heard —
 * so the people you marked as mattering get their names placed before strangers do. */
void MapsApp::drawOverlays() {
  resetLabels();
  /* The crosshair is painted last (it must be on top) but claims its space first: it is the
   * one thing on this screen that is always exactly where the user is looking, and a name
   * printed under it is a name obscuring the answer to "what is here". */
  reserveBox(vpX + vpW / 2 - 16, vpY + vpH / 2 - 16, 33, 33);

  for (int i = 0; i < pinCount && pins; i++) {
    mapLatLonToView(zoom, cx, cy, vpW, vpH,
                    mapI7ToDeg(pins[i].latI), mapI7ToDeg(pins[i].lonI),
                    &pinVx[i], &pinVy[i]);
  }

  const uint32_t me = meshService.getMyNodeNum();
  const int nodeN = meshService.getNodeCount();
  const int wpN = meshService.getWaypointCount();
  const uint32_t now = millis();
  int32_t selfLat = 0, selfLon = 0;
  uint32_t selfAge = 0;
  const int selfKind = selfPosition(&selfLat, &selfLon, &selfAge);

  if (measuring) {
    /* The anchor and a dashed line to the crosshair. Both ends may be far off-screen;
     * mapLatLonToView still returns their coordinates, and the sprite's drawLine clips per
     * pixel, so the line is drawn between the two points as they are and the parts inside
     * the viewport are what appear. Dashed by drawing every other 6 px segment. */
    int ax = 0, ay = 0;
    mapLatLonToView(zoom, cx, cy, vpW, vpH, mapI7ToDeg(measLatI), mapI7ToDeg(measLonI), &ax, &ay);
    const int x0 = vpX + ax, y0 = vpY + ay, x1 = vpX + vpW / 2, y1 = vpY + vpH / 2;
    const int dx = x1 - x0, dy = y1 - y0;
    const int steps = (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) / 6;
    for (int s = 0; s < steps; s += 2) {
      const int sx0 = x0 + dx * s / steps, sy0 = y0 + dy * s / steps;
      const int sx1 = x0 + dx * (s + 1) / steps, sy1 = y0 + dy * (s + 1) / steps;
      /* ⚠ ONLY SEGMENTS WITH BOTH ENDS INSIDE THE VIEWPORT: TFT_eSprite::drawLine clips
       * per pixel on the SPRITE, but the sprite is the whole screen, so a segment over the
       * header or the footer would be drawn on top of them. Clip to the map here. */
      if (sx0 >= vpX && sx0 < vpX + vpW && sy0 >= vpY && sy0 < vpY + vpH &&
          sx1 >= vpX && sx1 < vpX + vpW && sy1 >= vpY && sy1 < vpY + vpH) {
        lcd.drawLine(sx0, sy0, sx1, sy1, BLACK);
        lcd.drawLine(sx0, sy0 - 1, sx1, sy1 - 1, MAP_C_ME);
      }
    }
    if (ax >= 0 && ax < vpW && ay >= 0 && ay < vpH) {
      drawMarker(vpX + ax, vpY + ay, MAP_C_ME, MAP_SHAPE_DIAMOND);
      reserveBox(vpX + ax - 6, vpY + ay - 6, 13, 13);
    }
  }

  // ── pass 1: markers, back to front ───────────────────────────────────────────────────
  for (int i = 0; i < nodeN; i++) {
    const MeshNode* nd = meshService.getNode(i);
    if (!nd || !nd->nodeNum || nd->nodeNum == me || !nd->posHeardMs) {
      continue;                  // our own row holds the manual pin; it is drawn below
    }
    int vx = 0, vy = 0;
    if (!mapLatLonToView(zoom, cx, cy, vpW, vpH,
                         mapI7ToDeg(nd->latI), mapI7ToDeg(nd->lonI), &vx, &vy)) {
      continue;
    }
    const bool stale = (uint32_t)(now - nd->posHeardMs) > MAP_STALE_MS;
    drawMarker(vpX + vx, vpY + vy, stale ? MAP_C_NODE_OLD : MAP_C_NODE, MAP_SHAPE_DOT);
  }
  for (int i = 0; i < wpN; i++) {
    const MeshWaypoint* w = meshService.getWaypoint(i);
    if (!w || !w->id) {
      continue;
    }
    int vx = 0, vy = 0;
    if (mapLatLonToView(zoom, cx, cy, vpW, vpH,
                        mapI7ToDeg(w->latI), mapI7ToDeg(w->lonI), &vx, &vy)) {
      drawMarker(vpX + vx, vpY + vy, MAP_C_WP, MAP_SHAPE_DIAMOND);
    }
  }
  for (int i = 0; i < pinCount && pins; i++) {
    if (pinVx[i] < 0 || pinVx[i] >= vpW || pinVy[i] < 0 || pinVy[i] >= vpH) {
      continue;
    }
    drawMarker(vpX + pinVx[i], vpY + pinVy[i],
               pins[i].sharedId ? MAP_C_PIN_SH : MAP_C_PIN, MAP_SHAPE_DOT);
    if (pins[i].sharedId) {
      /* ⚠ SHAPE, NOT JUST HUE. Orange and yellow differ only in the green channel and this is
       * a small screen read in daylight, often through a polarised lens — "is that one shared
       * or not" is a question whose wrong answer is a location on the air, or a camp nobody
       * else can see. A hollow centre is unmistakable at arm's length and costs one call.
       * (The verifier called the colour pair adequate; it is adequate indoors.) */
      drawBlob(vpX + pinVx[i], vpY + pinVy[i], 1, BLACK, false);
    }
  }
  if (selfKind) {
    int vx = 0, vy = 0;
    if (mapLatLonToView(zoom, cx, cy, vpW, vpH,
                        mapI7ToDeg(selfLat), mapI7ToDeg(selfLon), &vx, &vy)) {
      if (selfKind == MAPS_SELF_PIN) {
        drawMarker(vpX + vx, vpY + vy, MAP_C_ME, MAP_SHAPE_DIAMOND);
      } else {
        const uint16_t c = (selfKind == MAPS_SELF_GPS) ? MAP_C_ME : MAP_C_NODE_OLD;
        drawBlob(vpX + vx, vpY + vy, 6, BLACK, false);
        drawBlob(vpX + vx, vpY + vy, 5, c, false);
        drawBlob(vpX + vx, vpY + vy, 2, BLACK, false);
        reserveBox(vpX + vx - 6, vpY + vy - 6, 13, 13);
      }
    }
  }

  // ── pass 2: labels, most important first ─────────────────────────────────────────────
  if (selfKind) {
    int vx = 0, vy = 0;
    if (mapLatLonToView(zoom, cx, cy, vpW, vpH,
                        mapI7ToDeg(selfLat), mapI7ToDeg(selfLon), &vx, &vy)) {
      char sl[24];
      uint16_t sc = MAP_C_ME;
      if (selfKind == MAPS_SELF_GPS) {
        strlcpy(sl, "me", sizeof(sl));
      } else if (selfKind == MAPS_SELF_PIN) {
        strlcpy(sl, "my pin", sizeof(sl));
      } else if (selfKind == MAPS_SELF_POOR_GPS) {
        const unsigned sats = selfAge & 0xFF, hdopX10 = selfAge >> 8;
        if (sats < 4) {
          snprintf(sl, sizeof(sl), "me? %u sats", sats);
        } else {
          snprintf(sl, sizeof(sl), "me? HDOP %u.%u", hdopX10 / 10, hdopX10 % 10);
        }
      } else {
        const unsigned mins = (unsigned)(selfAge / 60000u);
        snprintf(sl, sizeof(sl), "me, %um ago", mins);
        sc = MAP_C_NODE_OLD;
      }
      drawLabel(vpX + vx, vpY + vy, sl, sc);
    }
  }
  for (int i = 0; i < pinCount && pins; i++) {
    if (pinVx[i] < 0 || pinVx[i] >= vpW || pinVy[i] < 0 || pinVy[i] >= vpH) {
      continue;
    }
    drawLabel(vpX + pinVx[i], vpY + pinVy[i], pins[i].name,
              pins[i].sharedId ? MAP_C_PIN_SH : MAP_C_PIN);
  }
  for (int i = 0; i < wpN; i++) {
    const MeshWaypoint* w = meshService.getWaypoint(i);
    if (!w || !w->id) {
      continue;
    }
    int vx = 0, vy = 0;
    if (mapLatLonToView(zoom, cx, cy, vpW, vpH,
                        mapI7ToDeg(w->latI), mapI7ToDeg(w->lonI), &vx, &vy)) {
      drawLabel(vpX + vx, vpY + vy, w->name, MAP_C_WP);
    }
  }
  for (int i = 0; i < nodeN; i++) {
    const MeshNode* nd = meshService.getNode(i);
    if (!nd || !nd->nodeNum || nd->nodeNum == me || !nd->posHeardMs) {
      continue;
    }
    int vx = 0, vy = 0;
    if (!mapLatLonToView(zoom, cx, cy, vpW, vpH,
                         mapI7ToDeg(nd->latI), mapI7ToDeg(nd->lonI), &vx, &vy)) {
      continue;
    }
    const uint32_t ageMs = now - nd->posHeardMs;
    const bool stale = ageMs > MAP_STALE_MS;
    char lbl[MESH_NAME_LEN + 12];
    if (stale) {
      /* ⚠ THE AGE RIDES WITH THE NAME WHEN IT IS OLD. A grey dot says "not fresh"; it does
       * not say whether that is forty minutes or yesterday, and on foot those are completely
       * different pieces of information. */
      const unsigned mins = (unsigned)(ageMs / 60000u);
      if (mins >= 60u) {
        snprintf(lbl, sizeof(lbl), "%s %uh%02um", nd->name[0] ? nd->name : "?",
                 mins / 60u, mins % 60u);
      } else {
        snprintf(lbl, sizeof(lbl), "%s %um", nd->name[0] ? nd->name : "?", mins);
      }
    } else {
      strlcpy(lbl, nd->name[0] ? nd->name : "?", sizeof(lbl));
    }
    drawLabel(vpX + vx, vpY + vy, lbl, stale ? MAP_C_NODE_OLD : MAP_C_NODE);
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
  const int chipH = th + 4;
  if (chipH > vpH) {
    return;
  }

  /* ⚠ THE STATUS CHIP IS DRAWN FIRST AND TAKES THE ROOM IT NEEDS. "no map on the card" is the
   * most important sentence this screen can say, and the z/area chip beside it is the least;
   * when they compete for 240 pixels the wrong one used to win, because it was drawn first
   * and the other was simply skipped. */
  int rightEdge = vpX + vpW;
  if (statusLine[0]) {
    const int sw = lcd.textWidth(statusLine);
    if (sw + 8 <= vpW) {
      const int sx = vpX + vpW - sw - 8;
      fillClipped(sx, vpY, sw + 8, chipH, MAP_C_CHIP);
      lcd.setTextColor(0xFD20, MAP_C_CHIP);
      lcd.drawString(statusLine, sx + 4, vpY + 2);
      rightEdge = sx;
    }
  }

  /* An area name may be 31 characters and this chip is not that wide. Trim it to what is left
   * rather than drawing a grey bar with nothing in it, which is what a bare width test gives
   * you: the background had already been painted before the text was skipped. */
  char left[48];
  snprintf(left, sizeof(left), "z%d %s", zoom, areaSel >= 0 ? areas[areaSel].name : "-");
  const int room = rightEdge - vpX - 4;
  size_t n = strlen(left);
  int lw = lcd.textWidth(left);
  while (lw + 8 > room && n > 3) {
    left[--n] = '\0';
    lw = lcd.textWidth(left);
  }
  if (lw + 8 <= room) {
    fillClipped(vpX, vpY, lw + 8, chipH, MAP_C_CHIP);
    lcd.setTextColor(MAP_C_ME, MAP_C_CHIP);
    lcd.drawString(left, vpX + 4, vpY + 2);
  }
}

/* The note, broken into rows that fit `maxW` in the current font — at most MAPS_NOTE_ROWS,
 * the last one ellipsized if even that is not enough. Returns the row count. The breaking is
 * menu_wrap.h's (the same one the menus' notes use); this just measures for it. Nick,
 * 2026-09-20: "If I don't have a GPS fix and try to center on myself, the message gets cut
 * off" — "No fix yet, and no pin of your own to fall back on" is 50 characters, and the
 * strip's row holds about 30 of this font. */
struct NoteRowsCtx {
  SmoothFont* fnt;
  uint16_t maxW;
  char (*rows)[72];
  int n;
};
static size_t noteRowsFit(const char* s, void* v) {
  NoteRowsCtx* c = (NoteRowsCtx*)v;
  return (size_t)c->fnt->fitTextLength(s, c->maxW, 1);
}
static void noteRowsTake(const char* s, size_t len, void* v) {
  NoteRowsCtx* c = (NoteRowsCtx*)v;
  if (c->n >= MAPS_NOTE_ROWS) {
    return;
  }
  if (len >= sizeof(c->rows[0])) {
    len = sizeof(c->rows[0]) - 1;
  }
  memcpy(c->rows[c->n], s, len);
  c->rows[c->n][len] = 0;
  c->n++;
}
int MapsApp::noteRows(SmoothFont* fnt, uint16_t maxW, char (*rows)[72]) {
  if (!note[0]) {
    return 0;
  }
  NoteRowsCtx c = { fnt, maxW, rows, 0 };
  wrapNote(note, noteRowsFit, noteRowsTake, &c, MAPS_NOTE_ROWS);
  return c.n;
}

void MapsApp::drawBottomStrip() {
  SmoothFont* fnt = fonts[AKROBAT_BOLD_16];
  if (!fnt) {
    return;
  }
  lcd.setTextFont(fnt);
  const int th = (int)fnt->height();
  /* Row 1 is the scale bar; the note takes one row, or two when it needs them (noteRows).
   * The strip grows upward over the map for the second row, and shrinks back when the note
   * is retired by the next key. */
  char nrows[MAPS_NOTE_ROWS][72];
  const int noteN = noteRows(fnt, (uint16_t)(vpW - 8), nrows);
  const int stripH = th * (1 + (noteN > 1 ? noteN : 1)) + 6;
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
  int usedLeft = vpX + 4;               // how far along row 1 the scale bar reaches
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
    usedLeft = vpX + 8 + barPx + lcd.textWidth(sb);
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
    /* ⚠ AGAINST THE SCALE BAR, not against the screen. "1.4km NE Camp" is right-aligned, so a
     * long reference name grows LEFTWARDS — a width test against vpW lets it start at x=0 and
     * print straight over the scale bar, which is the one thing on this row that cannot be
     * read wrong without consequence. Dropped rather than overlapped: the distance is also on
     * the Places screen, and an unreadable scale bar is a map you cannot judge a walk from. */
    const int rw = lcd.textWidth(right);
    if (rw + 8 <= (vpX + vpW - 4) - usedLeft) {
      lcd.setTextColor(MAP_C_WP, MAP_C_STRIP);
      lcd.drawString(right, vpX + vpW - rw - 4, by);
    }
  }

  // Row 2 (and 3): the note if there is one, else the ruler's reading, else the crosshair.
  char line[64];
  if (noteN > 0) {
    lcd.setTextColor(0xFD20, MAP_C_STRIP);
    for (int i = 0; i < noteN; i++) {
      /* Ellipsized (see below); only the LAST row can still be too long, when the note ran
       * past MAPS_NOTE_ROWS rows and wrapNote's tail was cut where it stood. */
      guiDrawEllipsized(lcd, nrows[i], (uint16_t)(vpW - 8), (int16_t)(vpX + 4),
                        (int16_t)(sy + th * (1 + i) + 4));
    }
    return;
  } else if (measuring) {
    /* The whole row, not the right end of row 1: "1.2km SE" plus a word does not fit beside
     * a 1 km scale bar, and a measurement that is silently dropped for width is the one
     * failure a ruler must not have. */
    const int32_t cLat = mapDegToI7(lat), cLon = mapDegToI7(lon);
    char d[16];
    meshPosFmtDist(meshPosDistanceM(measLatI, measLonI, cLat, cLon), d, sizeof(d));
    snprintf(line, sizeof(line), "Ruler: %s %s of the anchor", d,
             meshPosCompass8(meshPosBearingDeg(measLatI, measLonI, cLat, cLon)));
    lcd.setTextColor(MAP_C_ME, MAP_C_STRIP);
  } else {
    snprintf(line, sizeof(line), "%.5f, %.5f", lat, lon);
    lcd.setTextColor(MAP_C_ME, MAP_C_STRIP);
  }
  /* ⚠ ELLIPSIZED, NOT drawString. This row carries a note this app composed from a pin name,
   * a place name or a node name — all of them other people's text — and plain drawString does
   * not clip: it paints to the screen edge and beyond. guiDrawEllipsized exists for exactly
   * this (GUI.h), cuts on a character boundary and appends ".." so a truncated string looks
   * truncated instead of looking complete. */
  guiDrawEllipsized(lcd, line, (uint16_t)(vpW - 8), (int16_t)(vpX + 4), (int16_t)(sy + th + 4));
}

void MapsApp::drawNoMapPage() {
  SmoothFont* fnt = fonts[AKROBAT_BOLD_18];
  lcd.fillRect(vpX, vpY, vpW, vpH, BLACK);
  if (!fnt) {
    return;
  }
  lcd.setTextFont(fnt);
  lcd.setTextColor(0xFD20, BLACK);
  lcd.drawString(cardOk ? "No map tiles on the card" : "No SD card", vpX + 8, vpY + 10);
  lcd.setTextColor(WHITE, BLACK);
  lcd.setTextFont(fonts[AKROBAT_BOLD_16]);
  static const char* noCard[] = {
    "The phone cannot see a card at all.",
    "",
    "Switch the phone OFF, seat the card,",
    "and switch it back on - the power",
    "switch is a hard cut, so 'off' is off.",
    "",
    "The card must be FAT32 (see the",
    "README); the phone does not speak",
    "exFAT, which is how cards over 32 GB",
    "come from the factory.",
  };
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
  const char* const* body = cardOk ? lines : noCard;
  const unsigned bodyN = cardOk ? (unsigned)(sizeof(lines) / sizeof(lines[0]))
                                : (unsigned)(sizeof(noCard) / sizeof(noCard[0]));
  int y = vpY + 40;
  for (unsigned i = 0; i < bodyN; i++) {
    if (y + 16 > vpY + vpH - 20) {
      break;
    }
    lcd.drawString(body[i], vpX + 8, y);
    y += 17;
  }
  /* ⚠ THE NOTE GOES HERE TOO. This page replaces the map entirely, and the note line is the
   * only feedback several keys have: on it, 0 (centre on me), 7/9 (step pins) and the zoom
   * keys all wrote a message that nothing ever drew, so they read as dead keys on the one
   * screen where a new user is most likely to be pressing things to see what happens. */
  if (note[0]) {
    char nrows[MAPS_NOTE_ROWS][72];
    const int n = noteRows(fonts[AKROBAT_BOLD_16], (uint16_t)(vpW - 12), nrows);
    lcd.setTextColor(0xFD20, BLACK);
    for (int i = 0; i < n; i++) {
      guiDrawEllipsized(lcd, nrows[i], (uint16_t)(vpW - 12), (int16_t)(vpX + 6),
                        (int16_t)(vpY + vpH - 18 - 17 * (n - 1 - i)));
    }
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
  /* "Me" means this phone: the GPS fix, or the position the user declared by hand. It does
   * NOT quietly mean the reference place — that may be a waypoint somebody else shared, and a
   * key labelled "centre on me" that lands on their camp has told you something false. The
   * reference is still offered as a last resort, and then the screen names it. */
  int32_t la = 0, lo = 0;
  uint32_t age = 0;
  const int kind = selfPosition(&la, &lo, &age);
  if (kind == MAPS_SELF_GPS) {
    centreOn(mapI7ToDeg(la), mapI7ToDeg(lo));
    setNote("Centred on the GPS fix (%us old)", (unsigned)(age / 1000u));
    return;
  }
  if (kind == MAPS_SELF_PIN) {
    centreOn(mapI7ToDeg(la), mapI7ToDeg(lo));
    setNote(gGpsNmea ? "No fresh GPS fix - centred on your pin"
                     : "GPS is off - centred on your pin");
    return;
  }
  if (kind == MAPS_SELF_POOR_GPS) {
    centreOn(mapI7ToDeg(la), mapI7ToDeg(lo));
    const unsigned sats = age & 0xFF, hdopX10 = age >> 8;
    setNote("Poor fix (%u sats, HDOP %u.%u) - could be well off", sats, hdopX10 / 10, hdopX10 % 10);
    return;
  }
  if (kind == MAPS_SELF_OLD_GPS) {
    centreOn(mapI7ToDeg(la), mapI7ToDeg(lo));
    setNote("No fresh fix - this is where you were %um ago",
            (unsigned)(age / 60000u));
    return;
  }
  char nm[24];
  if (meshService.resolveReference(&la, &lo, nm, sizeof(nm))) {
    centreOn(mapI7ToDeg(la), mapI7ToDeg(lo));
    setNote("Nothing of your own - centred on %s", nm);
    return;
  }
  if (!gGpsNmea) {
    setNote("GPS is off - turn it on in Meshtastic > My node");
  } else {
    setNote("No fix yet, and no pin of your own to fall back on");
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

/* ── THE MARKERS THE ARROWS CAN STOP ON ──────────────────────────────────────────────────
 * Three kinds, exactly the set the overlay draws: your pins, places heard from the mesh
 * (waypoints — the green gems) and other nodes with a position (cyan, grey when stale). One
 * enumerator feeds both searches below, so the arrows and the screen can never disagree
 * about what is there. Filters are the overlay's own (drawOverlays): a place needs an id, a
 * node needs a heard position and must not be this phone. Nick, 2026-09-19: "make it snap
 * to places in the mesh and nodes with locations too". */
#define SNAP_PIN    0
#define SNAP_PLACE  1
#define SNAP_NODE   2

int MapsApp::snapCount(int kind) {
  switch (kind) {
  case SNAP_PIN:   return pins ? pinCount : 0;
  case SNAP_PLACE: return meshService.getWaypointCount();
  default:         return meshService.getNodeCount();
  }
}

/* Marker `idx` of `kind` in view coordinates (which may lie off the screen — the corridor
 * search wants the ones just past the edge too). False when the slot is empty or filtered. */
bool MapsApp::snapMarker(int kind, int idx, int* vx, int* vy) {
  int32_t la = 0, lo = 0;
  if (kind == SNAP_PIN) {
    if (!pins || idx < 0 || idx >= pinCount) {
      return false;
    }
    la = pins[idx].latI;
    lo = pins[idx].lonI;
  } else if (kind == SNAP_PLACE) {
    const MeshWaypoint* w = meshService.getWaypoint(idx);
    if (!w || !w->id) {
      return false;
    }
    la = w->latI;
    lo = w->lonI;
  } else {
    const MeshNode* nd = meshService.getNode(idx);
    if (!nd || !nd->nodeNum || nd->nodeNum == meshService.getMyNodeNum() || !nd->posHeardMs) {
      return false;
    }
    la = nd->latI;
    lo = nd->lonI;
  }
  mapLatLonToView(zoom, cx, cy, vpW, vpH, mapI7ToDeg(la), mapI7ToDeg(lo), vx, vy);
  return true;
}

/* The marker nearest the crosshair within `radius` pixels, if any. Pins first at equal
 * distance, since a pin dropped ON a node (the usual "camp" case) is the thing OK would open. */
bool MapsApp::snapNear(int radius, MapsSnapHit* out) {
  bool found = false;
  long bestD2 = 0;
  for (int kind = SNAP_PIN; kind <= SNAP_NODE; kind++) {
    const int n = snapCount(kind);
    for (int i = 0; i < n; i++) {
      int vx = 0, vy = 0;
      if (!snapMarker(kind, i, &vx, &vy)) {
        continue;
      }
      const int ox = vx - vpW / 2, oy = vy - vpH / 2;
      /* 🛑 BY AXIS FIRST, BEFORE ANY SQUARING. A marker across the world has view coordinates
       * in the millions, and `long` is 32 bits on this chip: ox*ox wrapped to whatever, and
       * on phone 2 a tap off Pin 1 "landed" on a node an ocean away (2026-09-20; the same
       * arithmetic in yesterday's snapUnder could read "on a marker" while on nothing).
       * Inside the box the squares are small. */
      if (ox > radius || ox < -radius || oy > radius || oy < -radius) {
        continue;
      }
      const long d2 = (long)ox * ox + (long)oy * oy;
      if (!found || d2 < bestD2) {
        found = true;
        bestD2 = d2;
        out->kind = kind;
        out->idx = i;
      }
    }
  }
  return found;
}

/* Land on a marker: the crosshair exactly on it, the strip saying what it is. A pin becomes
 * the selected pin (7/9 step from it, OK opens it); a place or a node is named — OK there
 * drops a pin, as it does anywhere. */
void MapsApp::snapTo(const MapsSnapHit& h) {
  if (h.kind == SNAP_PIN) {
    pinSel = h.idx;
    centreOn(mapI7ToDeg(pins[h.idx].latI), mapI7ToDeg(pins[h.idx].lonI));
    setNote("%s (%d of %d)", pins[h.idx].name, h.idx + 1, pinCount);
  } else if (h.kind == SNAP_PLACE) {
    const MeshWaypoint* w = meshService.getWaypoint(h.idx);
    if (!w) {
      return;
    }
    centreOn(mapI7ToDeg(w->latI), mapI7ToDeg(w->lonI));
    setNote("%s (place from the mesh)", w->name[0] ? w->name : "unnamed");
  } else {
    const MeshNode* nd = meshService.getNode(h.idx);
    if (!nd) {
      return;
    }
    centreOn(mapI7ToDeg(nd->latI), mapI7ToDeg(nd->lonI));
    const uint32_t ageMs = millis() - nd->posHeardMs;
    if (ageMs > MAP_STALE_MS) {
      const unsigned mins = (unsigned)(ageMs / 60000u);
      if (mins >= 60) {
        setNote("%s (node, %uh%02um ago)", nd->name[0] ? nd->name : "?", mins / 60, mins % 60);
      } else {
        setNote("%s (node, %um ago)", nd->name[0] ? nd->name : "?", mins);
      }
    } else {
      setNote("%s (node)", nd->name[0] ? nd->name : "?");
    }
  }
}

/* One press (or one hold repeat) of an arrow.
 *
 * The snap is a LANDING rule now, and nothing else. Nick, 2026-09-20: "make the snap less
 * aggressive... I still want to be able to scroll around them, but if I just land near one
 * while scrolling (as near as I possibly can) then I want it to snap. It needs to feel
 * intentional." The first cut (2026-09-19) stopped a tap on any marker in its path and, from
 * a marker, jumped to the next one anywhere on screen — which meant a pin could not be
 * scrolled AWAY from by taps at all. So: a tap moves the nudge, a hold sweeps, and the map
 * snaps only when the crosshair LANDS within MAP_SNAP_RADIUS_PX of a marker — after a tap
 * (`discrete`), or after a hold when it ends (the release check in the APP_TIMER branch). A
 * hold's repeats never snap. The nudge is bigger than the radius, so the tap after a snap
 * steps clear; it is small enough that taps on the two axes can put the crosshair within
 * the radius of any marker (map_tiles.h has the arithmetic). Pins can still be walked in
 * order with 7 and 9. */
bool MapsApp::panOnce(int dx, int dy, int step, bool discrete) {
  /* Returns true when the press SNAPPED to a marker; a plain pan or a refused one returns
   * false. The map's edge is reported on the strip. */
  if (!mapPanView(zoom, vpW, vpH, dx * step, dy * step, &cx, &cy)) {
    if (dy) {
      /* Only north/south can refuse: longitude wraps, so a sideways press always moves. */
      setNote("That is as far %s as the map goes", dy < 0 ? "north" : "south");
    }
    return false;                        // nothing moved, so nothing landed
  }
  return discrete && snapLanding();
}

/* The landing: with Snap on, centre on the marker within MAP_SNAP_RADIUS_PX of the
 * crosshair, if there is one. True when it did. */
bool MapsApp::snapLanding() {
  if (!snapPins) {
    return false;
  }
  MapsSnapHit h;
  if (!snapNear(MAP_SNAP_RADIUS_PX, &h)) {
    return false;
  }
  snapTo(h);
  return true;
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
  const bool wasShared = (pins[idx].sharedId != 0);
  /* On the channel stored with the pin. NULL falls back to announceChannel() inside the
   * service — only reachable for a pin shared before channels were recorded (chan ""). A pin
   * whose recorded channel has LEFT this phone is not sent anywhere: an update on some other
   * channel is a second copy the radios holding the first will never see retracted. */
  const MeshChannel* ch = pinChannel(idx);
  if (pins[idx].chan[0] && !ch) {
    snprintf(why, whyCap, "Channel '%s' is gone from this phone - not sent", pins[idx].chan);
    return false;
  }
  const uint32_t id = meshService.shareWaypoint(pins[idx].sharedId,
                                                pins[idx].latI, pins[idx].lonI,
                                                pins[idx].name, 0, &onAir, ch);
  if (!id) {
    strlcpy(why, "Could not share - the mesh refused it", whyCap);
    return false;
  }
  if (!onAir && !wasShared) {
    /* 🛑 A SHARE THAT NEVER LEFT THE RADIO IS NOT A SHARE, and writing the id down makes the
     * claim permanent: the pin draws yellow, the list says "(shared)", and the options offer
     * "Update it on the mesh" — for a place nobody else has. Worse, a later Delete would then
     * send a retraction for a waypoint that was never announced. Roll the whole thing back so
     * the pin stays plainly local, and say what happened.
     * A RE-share (wasShared) is left alone on purpose: the mesh really does hold the old copy,
     * and forgetting the id would strand it there with no way to retract it. */
    meshService.unshareWaypoint(id, ch);
    snprintf(why, whyCap, "NOT sent - radio not ready. Still just yours.");
    return false;
  }
  pins[idx].sharedId = id;
  pinsDirty = true;
  savePins();
  /* 🔑 SAY WHETHER IT ACTUALLY LEFT THE PHONE. A place that is on your map and nobody else's,
   * shown as shared, is the failure the pin-announce path was built to avoid: it reads as
   * "they know where camp is" when nobody does. */
  if (onAir) {
    snprintf(why, whyCap, "Shared '%s' on %s", pins[idx].name, ch ? ch->name : "the mesh");
  } else {
    // Only reachable for a RE-share: the mesh still holds the old copy, so the id is kept.
    snprintf(why, whyCap, "NOT sent - the mesh still has the old one");
  }
  return onAir;
}

bool MapsApp::deletePin(int idx) {
  if (idx < 0 || idx >= pinCount || !pins) {
    return true;
  }
  bool retracted = true;
  if (pins[idx].sharedId) {
    /* It is on other people's maps too, and the confirm screen promised to take it off
     * theirs — a camp that has moved is worse than no camp at all.
     * ⚠ THE RESULT IS RETURNED, NOT DISCARDED. Deleting locally while the retraction failed
     * strands the waypoint on every other radio with nothing left on this phone that knows
     * its id, so the caller has to be able to say so. A pin whose channel has left this
     * phone is not sent anywhere: a retraction on some other channel reaches nobody who
     * holds the waypoint and would count as success. */
    if (pins[idx].chan[0] && !pinChannel(idx)) {
      retracted = false;
    } else {
      retracted = meshService.unshareWaypoint(pins[idx].sharedId, pinChannel(idx));
    }
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
  return retracted;
}

void MapsApp::stepPin(int delta) {
  if (!pins || pinCount <= 0) {
    setNote("No pins yet - OK drops one here");
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
  if (dlHeld && st != MAPS_DOWNLOAD) {
    controlState.holdScreenAwake(false);   // the progress screen is the only one that holds it
    dlHeld = false;
  }

  switch (st) {
  case MAPS_VIEW:
    snprintf(headerTitle, sizeof(headerTitle), "Maps");
    header->setTitle(headerTitle);
    footer->setButtons("Menu", "Back");     // the soft key IS the menu; OK is the pin
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
    // A pin opens its options; a place or a node only moves the map. Say which.
    footer->setButtons(listKind == LIST_PINS ? "Open" : "Go to", "Back");
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
    footer->setButtons("Save", "Clear");        // Back is backspace here; END cancels
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
    snprintf(headerTitle, sizeof(headerTitle), "The buttons");
    header->setTitle(headerTitle);
    footer->setButtons("More", "Back");
    helpTop = -1;                        // -1 = the drawn key; 0.. = the text rows after it
    break;
  case MAPS_DOWNLOAD:
    snprintf(headerTitle, sizeof(headerTitle), "Download maps");
    header->setTitle(headerTitle);
    footer->setButtons("Select", "Back");
    dlLastMs = millis();
    buildDownload();
    break;
  case MAPS_GOTO:
    snprintf(headerTitle, sizeof(headerTitle), "Go to coordinates");
    header->setTitle(headerTitle);
    footer->setButtons("Go", "Delete");      // Back is backspace here; END cancels
    gotoLat[0] = gotoLon[0] = '\0';
    gotoField = 0;
    break;
  case MAPS_CHANNEL:
    snprintf(headerTitle, sizeof(headerTitle), "Share on which channel?");
    header->setTitle(headerTitle);
    footer->setButtons("Share", "Back");
    chanPending = 0;
    buildChannels();
    break;
  }
}

void MapsApp::rescanCard() {
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
  /* ⚠ SAVE BEFORE RE-READING. loadPins() overwrites the in-RAM table from the file, so
   * any edit not yet on the card — every path here writes immediately today, but that
   * is a property of the callers, not of this code — would be silently discarded by a
   * menu row whose name promises only to look at the card again. */
  if (pinsDirty) {
    savePins();
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
    /* ⚠ cx/cy ARE WORLD PIXELS AT THE CURRENT ZOOM, so changing `zoom` without
     * rescaling them moves the view to completely different ground — at one level out
     * that is half the world away. setArea() already does this correctly; the rescan
     * path did not, and a card whose new area starts at a different zoom would have
     * dropped the user somewhere in the ocean. Read the ground, change the zoom, put
     * the ground back. */
    double keepLat = 0, keepLon = 0;
    mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &keepLat, &keepLon);
    if (zoom < areas[areaSel].zMin) {
      zoom = areas[areaSel].zMin;
    }
    if (zoom > areas[areaSel].zMax) {
      zoom = areas[areaSel].zMax;
    }
    centreOn(keepLat, keepLon);
  }
}

void MapsApp::buildMenu() {
  menu = newMenu("");
  char row[64];
  menu->addOption("What the buttons do...", ROW_M_HELP);
  snprintf(row, sizeof(row), "Pins (%d)...", pinCount);
  menu->addOption(row, ROW_M_PINS);
  snprintf(row, sizeof(row), "Places from the mesh (%d)...", meshService.getWaypointCount());
  menu->addOption(row, ROW_M_PLACES);
  snprintf(row, sizeof(row), "Nodes with a position...");
  menu->addOption(row, ROW_M_NODES);
  menu->addOption("Go to coordinates...", ROW_M_GOTO);
  menu->addOption("Centre on me", ROW_M_ME);
  menu->addOption(followMe ? "Follow me: ON (scroll stops it)" : "Follow me: off", ROW_M_FOLLOW);
  menu->addOption(measuring ? "Stop measuring" : "Measure from here...", ROW_M_MEASURE);
  menu->addOption(snapPins ? "Snap to markers: ON" : "Snap to markers: off", ROW_M_SNAP);
  menu->addOption(tileFetchActive() ? "Downloading maps..." : "Download maps...", ROW_M_DOWNLOAD);
  if (areaCount > 1) {
    snprintf(row, sizeof(row), "Map area: %s", areaSel >= 0 ? areas[areaSel].name : "-");
    menu->addOption(row, ROW_M_AREA);
  }
  menu->addOption("Rescan the card", ROW_M_RESCAN);
  menu->addOption("Back to the map", ROW_M_BACK);
  if (areaSel < 0) {
    menu->addNote("No tiles on the card - see docs/maps.md");
  }
}

/* ── THE LISTS ARE NEAREST-FIRST, FROM WHERE YOU ARE ────────────────────────────────────
 * COVEY's GO picker (D-126): every row carries "1.4km NE" and the list is sorted by it, so the
 * question "who is closest" is answered by the first row rather than by reading twenty. The
 * reference is the chain selfPosition() already ranks — a live fix, your declared pin, a
 * stale fix — and, with none of those, the crosshair: indoors the column still says how far
 * from the place you are LOOKING AT, which is what the map is for. The rows carry the
 * IDENTITY of the thing (see listId), so sorting here cannot change what OK opens. */
struct MapsListRow {
  uint32_t id;
  double   dist;         // metres from the reference
  char     text[72];
};

static int listRowCmp(const void* a, const void* b) {
  const double da = ((const MapsListRow*)a)->dist, db = ((const MapsListRow*)b)->dist;
  return da < db ? -1 : (da > db ? 1 : 0);
}

void MapsApp::buildList() {
  const char* empty = (listKind == LIST_PINS)
                      ? "No pins yet - OK on the map drops one"
                      : (listKind == LIST_PLACES ? "No places heard from the mesh yet"
                         : "No node has sent a position yet");
  menu = newMenu(empty);
  listCount = 0;

  // Where distances are measured from, and what to call it.
  int32_t rLat = 0, rLon = 0;
  uint32_t age = 0;
  const int selfKind = selfPosition(&rLat, &rLon, &age);
  const char* from = "you";
  if (selfKind == MAPS_SELF_NONE) {
    double lat = 0, lon = 0;
    mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &lat, &lon);
    rLat = mapDegToI7(lat);
    rLon = mapDegToI7(lon);
    from = "the crosshair";
  } else if (selfKind == MAPS_SELF_PIN) {
    from = "your pin";
  }

  /* PSRAM, not the GUI task's stack: 64 rows of 84 bytes is 5 KB, and the loop task's stack
   * has been measured at a few hundred bytes to spare. */
  MapsListRow* rows = (MapsListRow*)ps_malloc(sizeof(MapsListRow) * MAPS_MAX_LIST);
  if (!rows) {
    menu->addNote("No memory for the list");
    return;
  }
  int n = 0;
  char d[16];
  if (listKind == LIST_PINS) {
    for (int i = 0; i < pinCount && n < MAPS_MAX_LIST; i++) {
      MapsListRow* r = &rows[n++];
      r->id = (uint32_t)i;                     // this app owns the pin array; index is identity
      r->dist = meshPosDistanceM(rLat, rLon, pins[i].latI, pins[i].lonI);
      meshPosFmtDist(r->dist, d, sizeof(d));
      snprintf(r->text, sizeof(r->text), "%s %s  %s%s", d,
               meshPosCompass8(meshPosBearingDeg(rLat, rLon, pins[i].latI, pins[i].lonI)),
               pins[i].name, pins[i].sharedId ? " (shared)" : "");
    }
  } else if (listKind == LIST_PLACES) {
    const int cnt = meshService.getWaypointCount();
    for (int i = 0; i < cnt && n < MAPS_MAX_LIST; i++) {
      const MeshWaypoint* w = meshService.getWaypoint(i);
      if (!w || !w->id) {
        continue;
      }
      MapsListRow* r = &rows[n++];
      r->id = w->id;                           // the waypoint table compacts; the id does not
      r->dist = meshPosDistanceM(rLat, rLon, w->latI, w->lonI);
      meshPosFmtDist(r->dist, d, sizeof(d));
      snprintf(r->text, sizeof(r->text), "%s %s  %s", d,
               meshPosCompass8(meshPosBearingDeg(rLat, rLon, w->latI, w->lonI)), w->name);
    }
  } else {
    /* The service re-sorts the node list only when a screen asks it to, and this is such a
     * screen — the same contract the Meshtastic app's Nodes list follows. */
    meshService.refreshNodeOrder();
    const uint32_t me = meshService.getMyNodeNum();
    const uint32_t now = millis();
    const int cnt = meshService.getNodeCount();
    for (int i = 0; i < cnt && n < MAPS_MAX_LIST; i++) {
      const MeshNode* nd = meshService.getNode(i);
      if (!nd || !nd->nodeNum || nd->nodeNum == me || !nd->posHeardMs) {
        continue;
      }
      MapsListRow* r = &rows[n++];
      r->id = nd->nodeNum;
      r->dist = meshPosDistanceM(rLat, rLon, nd->latI, nd->lonI);
      meshPosFmtDist(r->dist, d, sizeof(d));
      const unsigned mins = (unsigned)((now - nd->posHeardMs) / 60000u);
      snprintf(r->text, sizeof(r->text), "%s %s  %s, %um ago", d,
               meshPosCompass8(meshPosBearingDeg(rLat, rLon, nd->latI, nd->lonI)),
               nd->name[0] ? nd->name : "?", mins);
    }
  }
  qsort(rows, (size_t)n, sizeof(MapsListRow), listRowCmp);
  for (int i = 0; i < n; i++) {
    listId[listCount] = rows[i].id;
    menu->addOption(rows[i].text, ROW_FIRST + listCount);
    listCount++;
  }
  free(rows);
  if (listCount > 0) {
    char note[48];
    snprintf(note, sizeof(note), "nearest first, from %s", from);
    menu->addNoteWrapped(note);
  }
  if (listCount >= MAPS_MAX_LIST) {
    menu->addNote("(more not shown)");
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
  /* The rows say WHERE. A pin that has a channel goes back there in one press; the picker is
   * a separate row so changing channel is a choice and never a surprise. */
  const MeshChannel* ch = pinChannel(pinSel);
  if (pins[pinSel].sharedId && pins[pinSel].chan[0] && !ch) {
    /* Shared on a channel this phone no longer has: nothing can be sent for it. Say so,
     * offer nothing that would pretend otherwise, and keep the id for the day it is back. */
    snprintf(row, sizeof(row), "Channel '%s' is gone - cannot update", pins[pinSel].chan);
    menu->addNoteWrapped(row);
    menu->addNoteWrapped("Add the channel back to send or retract");
  } else if (pins[pinSel].sharedId) {
    snprintf(row, sizeof(row), "Update it on %s", ch ? ch->name : "the mesh");
    menu->addOption(row, ROW_P_SHARE);
    menu->addOption("Take it off the mesh", ROW_P_UNSHARE);
  } else if (ch) {
    snprintf(row, sizeof(row), "Share it on %s", ch->name);
    menu->addOption(row, ROW_P_SHARE);
    menu->addOption("Share on another channel...", ROW_P_SHARE_ON);
  } else {
    menu->addOption("Share it on the mesh...", ROW_P_SHARE);
  }
  menu->addOption("Centre the map on it", ROW_P_CENTRE);
  menu->addOption("Delete", ROW_P_DELETE);
  menu->addOption("Back", ROW_P_CANCEL);
  snprintf(row, sizeof(row), "%.5f, %.5f",
           mapI7ToDeg(pins[pinSel].latI), mapI7ToDeg(pins[pinSel].lonI));
  menu->addNote(row);
}

void MapsApp::buildDownload() {
  /* Keep the thumb where it was across the once-a-second progress rebuilds; a row that no
   * longer exists (Start while running, Stop when done) falls back to the one that replaced it. */
  MenuOption::keyType keep = menu ? menu->currentKey() : dlKeep;
  if (keep == 0 || keep == MENU_ROW_NOTE) {
    keep = dlKeep;
  }
  menu = newMenu("");
  char row[72];
  const TileSource* src = tileSource(dlSource);
  if (!src) {
    dlSource = 0;
    src = tileSource(0);
  }
  TileJobStatus st;
  tileFetchStatus(&st);
  const bool running = st.active;

  snprintf(row, sizeof(row), "Source: %s", src->label);
  menu->addOption(row, ROW_D_SOURCE);
  snprintf(row, sizeof(row), "Radius: %d km", MAPS_DL_RADII[dlRadiusIdx]);
  menu->addOption(row, ROW_D_RADIUS);
  int depth = dlDepth;
  if (depth > src->zMax) {
    depth = src->zMax;
  }
  snprintf(row, sizeof(row), "Detail: z%d (%s)", depth,
           depth >= 16 ? "4x z15's cost" : (depth == 15 ? "3 m/pixel" : "coarser"));
  menu->addOption(row, ROW_D_DEPTH);

  TileJobSpec spec;
  memset(&spec, 0, sizeof(spec));
  spec.source = dlSource;
  mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &spec.lat, &spec.lon);
  spec.radiusKm = MAPS_DL_RADII[dlRadiusIdx];
  spec.zMax = depth;
  uint32_t card = 0, net = 0;
  const int tiles = tileFetchEstimate(&spec, &card, &net);
  const float sec = MAPS_DL_SEC_PER_TILE[dlSource < 4 ? dlSource : 3] * (float)tiles;
  /* Every note on this screen WRAPS (addNoteWrapped): these are the lines whose length the
   * numbers decide — "12853 tiles, 1606 MB, about 257 min", "Last run: 5517 new, 7332 had, 4
   * failed, 7013 s", an error from the fetcher — and a single row ended in `..` with the
   * part that mattered behind it (Nick, 2026-09-20). */
  const int mins = (int)(sec / 60.0f + 0.5f);
  snprintf(row, sizeof(row), "%d tiles, %u MB, about %d min",
           tiles, (unsigned)(card / (1024u * 1024u)), mins < 1 ? 1 : mins);
  menu->addNoteWrapped(row);

  if (running) {
    snprintf(row, sizeof(row), "%s %d/%d%s",
             st.paused ? "Paused" : (st.waitingRam ? "Waiting for memory" : "Downloading"),
             st.done + st.skipped + st.noTile + st.failed, st.total,
             st.stopping ? " (stopping)" : "");
    menu->addNoteWrapped(row);
    snprintf(row, sizeof(row), "z%d, %u KB, %u s, %d failed",
             st.curZ, (unsigned)(st.bytes / 1024), (unsigned)(st.elapsedMs / 1000), st.failed);
    menu->addNoteWrapped(row);
    menu->addOption("Stop", ROW_D_STOP);
  } else {
    if (st.finished) {
      snprintf(row, sizeof(row), "Last run: %d new, %d already had, %d failed, %u s",
               st.done, st.skipped, st.failed, (unsigned)(st.elapsedMs / 1000));
      menu->addNoteWrapped(row);
      if (st.lastErr[0]) {
        /* Named for what it is. "z16 10714/23006: card refused the write" on its own read
         * as a mystery (Nick: "something about card refused... idk what that means"); it is
         * the last tile that failed and why, and Start fetches the missing ones again. */
        char why[96];
        snprintf(why, sizeof(why), "Last problem: %s", st.lastErr);
        menu->addNoteWrapped(why);
        if (st.failed > 0) {
          menu->addNoteWrapped("Start again fetches only the missing tiles");
        }
      }
    }
    if (dlWhy[0]) {
      menu->addNoteWrapped(dlWhy);
    }
    if (WiFi.status() != WL_CONNECTED) {
      menu->addNoteWrapped("Not on WiFi - join a network first");
    } else if (!controlState.usbConnected && controlState.battVoltage < MAPS_DL_BATT_FLOOR) {
      snprintf(row, sizeof(row), "Battery %.2f V - plug in USB to download", controlState.battVoltage);
      menu->addNoteWrapped(row);
    } else {
      menu->addOption("Start download", ROW_D_START);
    }
  }
  menu->addOption("Back", ROW_D_BACK);
  if (src->credit[0]) {
    menu->addNoteWrapped(src->credit);
  }
  if (keep == ROW_D_START && running) {
    keep = ROW_D_STOP;
  } else if (keep == ROW_D_STOP && !running) {
    keep = ROW_D_START;
  }
  dlShownRunning = running;
  menu->select(keep);
  /* Hold the screen while the progress is on it: a 30 s sleep would lock the phone with the
   * cancel key behind the unlock chord. Released the moment another screen is entered. */
  if (running && !dlHeld) {
    controlState.holdScreenAwake(true);
    dlHeld = true;
  } else if (!running && dlHeld) {
    controlState.holdScreenAwake(false);
    dlHeld = false;
  }
}

void MapsApp::drawGoto() {
  const int top = (int)header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  lcd.fillRect(0, top, lcd.width(), bottom - top, BLACK);
  SmoothFont* big = fonts[AKROBAT_BOLD_20];
  SmoothFont* small = fonts[AKROBAT_BOLD_16];
  if (!big || !small) {
    return;
  }
  lcd.setTextDatum(TL_DATUM);
  const int boxH = 30;
  const char* labels[2] = { "Latitude", "Longitude" };
  const char* vals[2] = { gotoLat, gotoLon };
  int y = top + 12;
  for (int i = 0; i < 2; i++) {
    lcd.setTextFont(small);
    lcd.setTextColor(WHITE, BLACK);
    lcd.drawString(labels[i], 12, y);
    y += (int)small->height() + 2;
    const bool active = (gotoField == i);
    lcd.fillRect(8, y, lcd.width() - 16, boxH, active ? WHITE : MAP_C_STRIP);
    lcd.setTextFont(big);
    lcd.setTextColor(active ? BLACK : WHITE, active ? WHITE : MAP_C_STRIP);
    lcd.drawString(vals[i][0] ? vals[i] : (i == 0 ? "47.42" : "-121.75"), 14, y + 5);
    if (!vals[i][0]) {
      // the placeholder is an example, drawn dim
      lcd.fillRect(8, y, lcd.width() - 16, boxH, active ? WHITE : MAP_C_STRIP);
      lcd.setTextColor(active ? MAP_C_STRIP : MAP_C_NODE_OLD, active ? WHITE : MAP_C_STRIP);
      lcd.drawString(i == 0 ? "e.g. 47.42" : "e.g. -121.75", 14, y + 5);
    }
    y += boxH + 12;
  }
  lcd.setTextFont(small);
  lcd.setTextColor(MAP_C_NODE_OLD, BLACK);
  lcd.drawString("digits type   * point   # minus", 12, y);
  y += (int)small->height() + 2;
  lcd.drawString("up/down field   Back deletes", 12, y);
  y += (int)small->height() + 2;
  lcd.drawString("OK goes there   End cancels", 12, y);
  y += (int)small->height() + 8;
  double lat = 0, lon = 0;
  mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &lat, &lon);
  char now[48];
  snprintf(now, sizeof(now), "Crosshair: %.5f, %.5f", lat, lon);
  lcd.setTextColor(WHITE, BLACK);
  lcd.drawString(now, 12, y);
}

const MeshChannel* MapsApp::pinChannel(int idx) const {
  if (!pins || idx < 0 || idx >= pinCount || !pins[idx].chan[0]) {
    return NULL;
  }
  return meshService.findChannelByName(pins[idx].chan);
}

void MapsApp::buildChannels() {
  /* Rows with a subtitle need the taller pitch the Meshtastic screens use (five rows per
   * screen); the map's usual single-line menu draws the subtitle over the next row. */
  freeMenu();
  menu = new MenuWidget(0, header->height(), lcd.width(),
                        lcd.height() - header->height() - footer->height(),
                        "No channels on this phone", fonts[AKROBAT_EXTRABOLD_22], 5, 8);
  menu->setStyle(MenuWidget::DEFAULT_STYLE, WHITE, BLACK, BLACK, GREEN);
  char row[40];
  const int n = meshService.getChannelCount();
  const char* have = (chanForPin >= 0 && chanForPin < pinCount) ? pins[chanForPin].chan : "";
  const char* beacon = meshService.getPosChannelName();
  MenuOption::keyType pick = 0;
  for (int i = 0; i < n; i++) {
    const MeshChannel* c = meshService.getChannel(i);
    if (!c) {
      continue;
    }
    const MenuOption::keyType key = (MenuOption::keyType)(ROW_C_BASE + i);
    if (meshService.channelIsPublic(c)) {
      snprintf(row, sizeof(row), "PUBLIC %s", c->name);
      menu->addOption(row, chanPending == (int)key ? "PUBLIC - press again to confirm"
                                                   : "PUBLIC - any radio in range", key, 1);
    } else if (have[0] && strcmp(have, c->name) == 0) {
      menu->addOption(c->name, "private - this pin is here now", key, 1);
    } else if (beacon[0] && strcmp(beacon, c->name) == 0) {
      menu->addOption(c->name, "private - your position beacon's channel", key, 1);
    } else {
      menu->addOption(c->name, "private", key, 1);
    }
    /* Highlight, in order: where this pin already is, else the beacon's channel, else the
     * first private channel. Never a public one by default. */
    if (!pick) {
      if (have[0] && strcmp(have, c->name) == 0) {
        pick = key;
      } else if (beacon[0] && strcmp(beacon, c->name) == 0) {
        pick = key;
      }
    }
  }
  if (!pick) {
    for (int i = 0; i < n; i++) {
      const MeshChannel* c = meshService.getChannel(i);
      if (c && !meshService.channelIsPublic(c)) {
        pick = (MenuOption::keyType)(ROW_C_BASE + i);
        break;
      }
    }
  }
  menu->addOption("Back", ROW_C_CANCEL);
  if (pick) {
    menu->select(pick);
  }
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
  /* ⚠ THE OPTIONS COME FIRST, and that is not cosmetic: a MenuWidget opens on row 0, and with
   * the explanatory notes at the top that row was a display-only note. The footer said
   * "Select" and the first press of it did nothing at all — on the one screen in this app
   * whose whole job is to take an answer. Every other screen here puts its notes last. */
  menu->addOption("No, keep it", ROW_DEL_NO);
  menu->addOption("Yes, delete it", ROW_DEL_YES);
  char row[72];
  if (pinSel >= 0 && pinSel < pinCount) {
    snprintf(row, sizeof(row), "Delete '%s'", pins[pinSel].name);
    menu->addNoteWrapped(row);
    if (pins[pinSel].sharedId) {
      menu->addNoteWrapped(pins[pinSel].chan[0] && !pinChannel(pinSel)
                           ? "(its channel is gone: it cannot be retracted)"
                           : "...and take it off the mesh");
    }
  }
}

/* ⚠ THIS LIST DOES NOT FIT AND USED TO BE SILENTLY CUT. Twenty-odd rows at a 17 px pitch is
 * 370 px in a 250 px window, so the loop's bottom guard threw away everything from the colour
 * key down — the half of this screen that explains what the coloured dots on the map MEAN,
 * on the one screen whose entire job is to explain things. It scrolls now, and says so. */
/* ⚠ ROWS MUST FIT 240 PX AT AKROBAT_BOLD_16 — about 33 characters. A longer row is not
 * clipped at the right; the smooth-font renderer shifts it so BOTH ends are cut (measured:
 * "Hold an arrow..." lost its H and its last digit), which reads as a typo, not a cut. */
static const struct { const char* text; uint16_t colour; } MAPS_HELP_ROWS[] = {
  { "Hold an arrow: it speeds up.", WHITE },
  { "Land near a pin, place or node", WHITE },
  { "  and the map snaps onto it", WHITE },
  { "  (Snap, in the menu). 7 9: pins", WHITE },
  { "2 4 6 8 scroll too, for gloves.", WHITE },
  { "5 is OK. 0 is centre on me.", WHITE },
  { "4th side button: next map type", WHITE },
  { "", WHITE },
  { "In the menu:", MAP_C_PIN },
  { "Download maps - the area around", WHITE },
  { "  the crosshair, over WiFi", WHITE },
  { "Go to - a pin, a node, a place,", WHITE },
  { "  or typed coordinates", WHITE },
  { "Follow me - stays centred on you", WHITE },
  { "  until you scroll", WHITE },
  { "Measure - drops an anchor; scroll", WHITE },
  { "  away and the bar reads it", WHITE },
  { "", WHITE },
  { "orange dot   your pin", MAP_C_PIN },
  { "yellow ring  your pin, shared", MAP_C_PIN_SH },
  { "green gem    place from the mesh", MAP_C_WP },
  { "cyan dot     someone else", MAP_C_NODE },
  { "grey dot     ...over 30 min old", MAP_C_NODE_OLD },
  { "white ring   this phone, live GPS", MAP_C_ME },
  { "grey ring    ...a fix over 2 min old", MAP_C_NODE_OLD }, // 36 chars: fits, measured
  { "white gem    ...your declared pin", MAP_C_ME },
  { "", WHITE },
  { "A name is dropped, never squashed,", WHITE },
  { "when it would land on another.", WHITE },
  { "", WHITE },
  { "grey ring    ...or a poor fix", MAP_C_NODE_OLD },
  { "             (<4 sats or HDOP >10)", MAP_C_NODE_OLD },
  { "grey square  tile not on the card", WHITE },
  { "See docs/maps.md for the tiles.", WHITE },
  { "", WHITE },
  { "USGS: public domain (National Map)", WHITE },
  { "OpenTopoMap: (c) OSM contributors,", WHITE },
  { "  SRTM | OpenTopoMap, CC-BY-SA", WHITE },
};
static const int MAPS_HELP_N = (int)(sizeof(MAPS_HELP_ROWS) / sizeof(MAPS_HELP_ROWS[0]));

/* One labelled box of the drawn key. */
static void helpBox(LCD& lcd, int x, int y, int w, int h, const char* label, uint16_t fill) {
  lcd.fillRoundRect(x, y, w, h, 4, fill);
  lcd.drawRoundRect(x, y, w, h, 4, WHITE);
  if (label && label[0]) {
    lcd.setTextDatum(MC_DATUM);
    lcd.setTextColor(fill == BLACK ? WHITE : BLACK, fill);
    lcd.drawString(label, x + w / 2, y + h / 2);
    lcd.setTextDatum(TL_DATUM);
  }
}

/* The phone's buttons, drawn where they are on the phone, each saying what it does on the
 * map. Nick asked for a key that makes sense at a glance (2026-09-19); a table of key names
 * asks the reader to know which key is which — this shows them. Side buttons down the left
 * edge as they are on the case, soft keys under the screen, the D-pad in the middle. */
void MapsApp::drawHelpDiagram() {
  const int top = (int)header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  SmoothFont* small = fonts[AKROBAT_BOLD_16];
  if (!small) {
    return;
  }
  lcd.setTextFont(small);
  const int th = (int)small->height();
  const uint16_t KEY = 0x39E7;                // a dark grey key face

  // ── side buttons, top to bottom, at the left edge (as on the case) ──
  const int sx = 4, sw = 26, sh = 20;
  int sy = top + 6;
  const char* sideLabel[4] = { "1", "2", "3", "4" };
  /* ⚠ Row 4 shares its line with the D-pad's "arrows scroll" caption (x from ~111): "next
   * map type" ran under it, seen on phone 1. Eight characters is the room there is. */
  const char* sideDoes[4]  = { "zoom in", "zoom out", "centre on me", "map type" };
  for (int i = 0; i < 4; i++) {
    helpBox(lcd, sx, sy, sw, sh, sideLabel[i], KEY);
    lcd.setTextColor(WHITE, BLACK);
    lcd.drawString(sideDoes[i], sx + sw + 5, sy + 2);
    sy += sh + 4;
  }
  lcd.setTextColor(MAP_C_NODE_OLD, BLACK);
  lcd.drawString("side buttons", sx, sy);

  // ── the D-pad on the right: arrows scroll, the centre is the pin ──
  const int a = 20, gap = 2, cxp = 156, cyp = top + 6 + a + gap + a / 2;
  helpBox(lcd, cxp - a / 2, cyp - a / 2 - gap - a, a, a, "^", KEY);
  helpBox(lcd, cxp - a / 2, cyp + a / 2 + gap, a, a, "v", KEY);
  helpBox(lcd, cxp - a / 2 - gap - a, cyp - a / 2, a, a, "<", KEY);
  helpBox(lcd, cxp + a / 2 + gap, cyp - a / 2, a, a, ">", KEY);
  helpBox(lcd, cxp - a / 2, cyp - a / 2, a, a, "OK", MAP_C_PIN);
  lcd.setTextDatum(TC_DATUM);
  lcd.setTextColor(WHITE, BLACK);
  lcd.drawString("arrows scroll", cxp, cyp + a / 2 + gap + a + 4);
  lcd.setTextColor(MAP_C_PIN, BLACK);
  lcd.drawString("OK: drop / open a pin", cxp, cyp + a / 2 + gap + a + 4 + th);
  lcd.setTextDatum(TL_DATUM);

  // ── the keypad shortcuts ──
  int y = sy + th + 8;
  lcd.setTextColor(WHITE, BLACK);
  lcd.drawString("7 / 9    previous / next pin", 6, y);
  y += th + 1;
  lcd.drawString("* / #    zoom, like the side buttons", 6, y);
  y += th + 1;
  lcd.drawString("Back     leave; your view is kept", 6, y);
  y += th + 4;
  lcd.setTextColor(MAP_C_PIN, BLACK);
  lcd.drawString("More: colours and the rest v", 6, y);

  // ── the soft keys, under the screen ──
  const int kw = 92, kh = 20;
  const int ky = bottom - kh - 3;
  lcd.setTextColor(MAP_C_NODE_OLD, BLACK);
  lcd.setTextDatum(TC_DATUM);
  lcd.drawString("soft keys under the screen", lcd.width() / 2, ky - th - 1);
  lcd.setTextDatum(TL_DATUM);
  helpBox(lcd, 6, ky, kw, kh, "Menu", KEY);
  helpBox(lcd, lcd.width() - kw - 6, ky, kw, kh, "Back", KEY);
}

void MapsApp::drawHelp() {
  const int top = (int)header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  lcd.fillRect(0, top, lcd.width(), bottom - top, BLACK);
  if (helpTop < 0) {
    drawHelpDiagram();
    return;
  }
  SmoothFont* fnt = fonts[AKROBAT_BOLD_16];
  if (!fnt) {
    return;
  }
  lcd.setTextFont(fnt);
  const int pitch = (int)fnt->height() + 1;
  int perScreen = (bottom - top - 4) / pitch;
  if (perScreen < 1) {
    perScreen = 1;
  }
  // Keep the window inside the list however it was scrolled.
  int maxTop = MAPS_HELP_N - perScreen;
  if (maxTop < 0) {
    maxTop = 0;
  }
  if (helpTop > maxTop) {
    helpTop = maxTop;
  }
  if (helpTop < 0) {
    helpTop = 0;
  }
  int y = top + 2;
  for (int i = helpTop; i < MAPS_HELP_N && i < helpTop + perScreen; i++) {
    if (MAPS_HELP_ROWS[i].text[0]) {
      lcd.setTextColor(MAPS_HELP_ROWS[i].colour, BLACK);
      lcd.drawString(MAPS_HELP_ROWS[i].text, 6, y);
    }
    y += pitch;
  }
  // "there is more below" is the only thing that makes the scroll discoverable.
  if (maxTop > 0) {
    char more[32];
    snprintf(more, sizeof(more), "%s%s", "^ ",           // the drawn key is always above row 0
             helpTop < maxTop ? "more below v" : "");
    lcd.setTextColor(MAP_C_PIN, BLACK);
    lcd.drawString(more, 6, bottom - (int)fnt->height() - 1);
  }
}

// ---------------------------------------------------------------- events

appEventResult MapsApp::onMapKey(EventType event) {
  /* 🛑 A BATTERY BLINK IS NOT A KEYPRESS. Every non-keyboard event the phone raises — the
   * battery icon, the WiFi RSSI, the clock, a SIP registration — is delivered to the running
   * app, and this function ran for all of them. So the one line explaining what just happened
   * ("Saved, but NOT sent", "That place is gone") was wiped within a second or two of being
   * written, by something the user did not do. IS_KEYBOARD is GUI.h's own test. */
  if (!IS_KEYBOARD(event)) {
    return DO_NOTHING;
  }

  /* One place clears the note, and it is here: ANY key on the map retires the last one, and
   * a handler that has something new to say says it below. Clearing per-branch is how a
   * message from ten minutes ago ends up sitting under a map nobody is looking at any more —
   * and how one branch gets forgotten. */
  note[0] = '\0';
  panHoldMask = 0;                       // whatever this key is, the previous hold is over

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
    if (followMe) {
      followMe = false;                  // a scroll is a decision to look elsewhere
      setNote("Stopped following");
    }
    /* A tap is ALWAYS the nudge, however fast the taps come: only a hold climbs the
     * accelerator (below). Taps used to speed up too, which made the size of a tap depend
     * on its timing — the opposite of "as near as I possibly can". */
    const uint32_t now = millis();
    panRun = 0;
    panLastMs = now;
    panHoldRan = false;
    panOnce(dx, dy, mapPanStep(0), true);
    /* Remember which key this was, so the timer can keep scrolling while it stays down.
     * Nick, 2026-09-19: "holding the scroll button doesn't allow it to keep scrolling and
     * speeding up, it just does a small jump and stops" — the keypad path hides the hold
     * from every app on purpose, so the map polls for it itself (uiKeyStillHeld). */
    switch (event) {
    case WIPHONE_KEY_UP:    panHoldMask = WIPHONE_KEY_MASK_UP;    break;
    case WIPHONE_KEY_DOWN:  panHoldMask = WIPHONE_KEY_MASK_DOWN;  break;
    case WIPHONE_KEY_LEFT:  panHoldMask = WIPHONE_KEY_MASK_LEFT;  break;
    case WIPHONE_KEY_RIGHT: panHoldMask = WIPHONE_KEY_MASK_RIFHT; break;
    case '2':               panHoldMask = WIPHONE_KEY_MASK_2;     break;
    case '8':               panHoldMask = WIPHONE_KEY_MASK_8;     break;
    case '4':               panHoldMask = WIPHONE_KEY_MASK_4;     break;
    case '6':               panHoldMask = WIPHONE_KEY_MASK_6;     break;
    default:                panHoldMask = 0;                      break;
    }
    panHoldSinceMs = now;
    panHoldDx = dx;
    panHoldDy = dy;
    return REDRAW_SCREEN;
  }

  switch (event) {
  case WIPHONE_KEY_F2:              // the second side button: zoom out
  case '*':
  case '1': {
    /* ⚠ FALL BACK TO THE PROJECTION'S LIMITS, NOT TO THE CURRENT ZOOM. With no tiles on the
     * card zMin and zMax were both `zoom`, so both zoom keys were dead and said "z15 is the
     * widest this map has" — about a map that does not exist. The markers are still worth
     * zooming around without a basemap. */
    const int zMin = areaSel >= 0 ? areas[areaSel].zMin : MAP_ZOOM_MIN;
    const int zMax = areaSel >= 0 ? areas[areaSel].zMax : MAP_ZOOM_MAX;
    const int nz = mapZoomView(zMin, zMax, zoom, -1, vpW, vpH, &cx, &cy);
    if (nz == zoom) {
      setNote(areaSel >= 0 ? "z%d is the widest this map has" : "z%d is as far out as it goes",
              zoom);
    } else {
      cancelLoad();              // the tile on its way is for a zoom nobody is looking at
      zoom = nz;
    }
    return REDRAW_SCREEN;
  }
  case WIPHONE_KEY_F1:              // the top side button: zoom in
  case '#':
  case '3': {
    const int zMin = areaSel >= 0 ? areas[areaSel].zMin : MAP_ZOOM_MIN;
    const int zMax = areaSel >= 0 ? areas[areaSel].zMax : MAP_ZOOM_MAX;
    const int nz = mapZoomView(zMin, zMax, zoom, +1, vpW, vpH, &cx, &cy);
    if (nz == zoom) {
      setNote(areaSel >= 0 ? "z%d is the closest this map has" : "z%d is as far in as it goes",
              zoom);
    } else {
      cancelLoad();
      zoom = nz;
    }
    return REDRAW_SCREEN;
  }
  case '5': {
    const int hit = pinUnderCrosshair();   // the same key as OK, for gloves: open, else drop
    if (hit >= 0) {
      pinSel = hit;
      enterState(MAPS_PIN_OPTS);
      return REDRAW_ALL;
    }
    if (dropPin()) {
      setNote("Dropped '%s' - name it", pins[pinSel].name);
      enterState(MAPS_RENAME);
      return REDRAW_ALL;
    }
    return REDRAW_SCREEN;
  }
  case '7':
    stepPin(-1);
    return REDRAW_SCREEN;
  case '9':
    stepPin(1);
    return REDRAW_SCREEN;
  case WIPHONE_KEY_F3:              // the third side button: centre on me
  case '0':
    centreOnMe();
    return REDRAW_SCREEN;
  case WIPHONE_KEY_F4:              // the fourth side button: the next map type
    /* Nick, 2026-09-20: "make the last user button cycle through map types". The same
     * switch as Menu > Map area, one press at a time, round and round; setArea keeps the
     * ground under the crosshair and says which map it is on. */
    if (areaCount <= 0) {
      setNote("No maps on the card - see docs/maps.md");
    } else if (areaCount == 1) {
      setNote("Only one map on the card: %s", areas[0].name);
    } else {
      setArea((areaSel + 1) % areaCount);
    }
    return REDRAW_SCREEN;
  default:
    break;
  }

  /* ⚠ SELECT BEFORE LOGIC_BUTTON_OK, ALWAYS. LOGIC_BUTTON_OK is OK||CALL||SELECT on this
   * keyboard (Hardware.h:212), so a test for the macro first would swallow the soft key —
   * app_music.cpp:405-412 has exactly that dead branch. Here the top-left soft key is the
   * MENU, the D-pad centre is the PIN (options on the one under the crosshair, otherwise drop
   * a new one), and CALL does nothing on a map. */
  if (event == WIPHONE_KEY_SELECT) {
    enterState(MAPS_MENU);
    return REDRAW_ALL;
  }
  if (event == WIPHONE_KEY_OK) {
    const int hit = pinUnderCrosshair();
    if (hit >= 0) {
      pinSel = hit;
      enterState(MAPS_PIN_OPTS);
      return REDRAW_ALL;
    }
    if (dropPin()) {
      setNote("Dropped '%s' - name it", pins[pinSel].name);
      enterState(MAPS_RENAME);
      return REDRAW_ALL;
    }
    return REDRAW_SCREEN;
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
    if (panHoldMask) {
      if (appState != MAPS_VIEW || !uiKeyStillHeld(panHoldMask)) {
        panHoldMask = 0;                 // the finger came off (or a lost release aged out)
        if (panHoldRan) {
          panRun = 0;
          panLastMs = 0;
          panHoldRan = false;
          /* Where the sweep STOPPED is a landing too: letting go near a marker snaps onto
           * it, the same rule as a tap. Never mid-sweep — the repeats sail past. */
          if (appState == MAPS_VIEW && snapLanding()) {
            armTimer();
            return REDRAW_SCREEN;
          }
        }
        armTimer();                      // stand the 100 ms tick down unless tiles need it
      } else {
        const uint32_t now = millis();
        if (now - panHoldSinceMs >= MAP_PAN_HOLD_DELAY_MS && now - panLastMs >= MAP_PAN_HOLD_STEP_MS) {
          if (panRun < 1000) {
            panRun++;                    // speeding up: the same curve a run of taps climbs
          }
          panLastMs = now;
          panHoldRan = true;
          panOnce(panHoldDx, panHoldDy, mapPanStep(panRun), false);
          return REDRAW_SCREEN;
        }
      }
    }
    if (followMe && appState == MAPS_VIEW && !loadActive) {
      /* A newer usable fix than the one last centred on: follow it. A stale or poor fix, or
       * none, leaves the view exactly where it is. */
      int32_t la = 0, lo = 0;
      uint32_t age = 0;
      if (selfPosition(&la, &lo, &age) == MAPS_SELF_GPS) {
        /* The moment this fix arrived, from its age: the same fix reads the same stamp on
         * every tick (give or take a tick), a new one reads a later stamp. */
        const uint32_t stamp = millis() - age;
        if (stamp - followLastStamp > 500u) {
          followLastStamp = stamp;
          centreOn(mapI7ToDeg(la), mapI7ToDeg(lo));
          return REDRAW_SCREEN;
        }
      }
      if (!wantAny) {
        return DO_NOTHING;
      }
    }
    if (appState == MAPS_DOWNLOAD) {
      const bool running = tileFetchActive();
      if (running != dlShownRunning || (running && millis() - dlLastMs >= 1000)) {
        dlLastMs = millis();
        buildDownload();
        return REDRAW_SCREEN;
      }
      return DO_NOTHING;
    }
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
      case ROW_M_FOLLOW:
        followMe = !followMe;
        followLastStamp = 0;
        if (followMe) {
          centreOnMe();                  // one honest centre now; the timer keeps it there
          if (!gGpsNmea) {
            setNote("GPS is off - following waits for it");
          } else {
            setNote("Following you - any scroll stops it");
          }
        } else {
          setNote("Stopped following");
        }
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      case ROW_M_SNAP: {
        snapPins = !snapPins;
        Preferences p;
        if (p.begin(MAPS_NVS, false)) {
          p.putInt("snap", snapPins ? 1 : 0);
          p.end();
        }
        setNote(snapPins ? "Landing near a marker snaps onto it"
                         : "The arrows never snap");
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      }
      case ROW_M_MEASURE:
        measuring = !measuring;
        if (measuring) {
          double lat = 0, lon = 0;
          mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &lat, &lon);
          measLatI = mapDegToI7(lat);
          measLonI = mapDegToI7(lon);
          setNote("Scroll away - the bar reads the distance");
        } else {
          setNote("Stopped measuring");
        }
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      case ROW_M_DOWNLOAD:
        enterState(MAPS_DOWNLOAD);
        return REDRAW_ALL;
      case ROW_M_GOTO:
        enterState(MAPS_GOTO);
        return REDRAW_ALL;
      case ROW_M_RESCAN:
        rescanCard();
        setNote("%d map%s, %d pin%s on the card", areaCount, areaCount == 1 ? "" : "s",
                pinCount, pinCount == 1 ? "" : "s");
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
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

  case MAPS_DOWNLOAD:
    if (LOGIC_BUTTON_BACK(event)) {
      /* Coming back to the map after a download: the tiles it wrote are not in the miss
       * ring's memory, and a new source folder is a new area. Re-read the card, keep the view. */
      rescanCard();
      enterState(MAPS_MENU);
      return REDRAW_ALL;
    }
    if (menu) {
      menu->processEvent(event);
      /* ⚠ NOTE ROWS ARE STOPS TO MenuWidget — an arrow lands on the estimate line and OK there
       * does nothing. Step over them, in the direction of travel, so the form has four rows
       * to the thumb and not seven. */
      if (event == WIPHONE_KEY_UP || event == WIPHONE_KEY_DOWN) {
        for (int guard = 0; guard < 8 && menu->currentKey() == MENU_ROW_NOTE; guard++) {
          menu->processEvent(event);
        }
      }
    }
    if (LOGIC_BUTTON_OK(event) && menu) {
      const MenuOption::keyType sel = menu->readChosen();
      dlKeep = sel;
      switch (sel) {
      case ROW_D_SOURCE:
        dlSource = (dlSource + 1) % tileSourceCount();
        buildDownload();
        menu->select(sel);
        return REDRAW_SCREEN;
      case ROW_D_RADIUS:
        dlRadiusIdx = (dlRadiusIdx + 1) % MAPS_DL_RADII_N;
        buildDownload();
        menu->select(sel);
        return REDRAW_SCREEN;
      case ROW_D_DEPTH:
        dlDepth = dlDepth >= MAPS_DL_DEPTH_MAX ? MAPS_DL_DEPTH_MIN : dlDepth + 1;
        buildDownload();
        menu->select(sel);
        return REDRAW_SCREEN;
      case ROW_D_START: {
        TileJobSpec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source = dlSource;
        mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &spec.lat, &spec.lon);
        spec.radiusKm = MAPS_DL_RADII[dlRadiusIdx];
        spec.zMax = dlDepth;
        char why[80];
        dlKeep = ROW_D_STOP;             // BEFORE the rebuild: Start is gone from a running form
        dlWhy[0] = '\0';
        if (tileFetchStart(&spec, why, sizeof(why))) {
          Preferences p;
          if (p.begin(MAPS_NVS, false)) {
            p.putInt("dlsrc", dlSource);
            p.putInt("dlrad", dlRadiusIdx);
            p.putInt("dldep", dlDepth);
            p.end();
          }
          setNote("Downloading %s", tileSource(dlSource)->label);
        } else {
          /* `note` is drawn on the MAP, not here: a refusal has to live on this form. */
          snprintf(dlWhy, sizeof(dlWhy), "Not started: %s", why);
          setNote("Not started: %s", why);
        }
        buildDownload();
        return REDRAW_SCREEN;
      }
      case ROW_D_STOP:
        tileFetchStop();
        dlKeep = ROW_D_START;
        buildDownload();
        return REDRAW_SCREEN;
      case ROW_D_BACK:
        rescanCard();
        enterState(MAPS_MENU);
        return REDRAW_ALL;
      default:
        break;
      }
    }
    return REDRAW_SCREEN;

  case MAPS_GOTO: {
    /* Two fields, typed on the keypad: digits, `*` for the point, `#` for a minus sign.
     * UP/DOWN move between latitude and longitude; Back deletes; END cancels; OK goes. */
    char* f = gotoField == 0 ? gotoLat : gotoLon;
    if (event == WIPHONE_KEY_END) {
      enterState(MAPS_VIEW);
      return REDRAW_ALL;
    }
    if (event == WIPHONE_KEY_BACK) {
      const size_t n = strlen(f);
      if (n) {
        f[n - 1] = '\0';
      }
      return REDRAW_SCREEN;
    }
    if (event == WIPHONE_KEY_UP || event == WIPHONE_KEY_DOWN) {
      gotoField = gotoField ? 0 : 1;
      return REDRAW_SCREEN;
    }
    if (LOGIC_BUTTON_OK(event)) {
      if (!gotoLat[0] || !gotoLon[0]) {
        gotoField = gotoLat[0] ? 1 : 0;
        return REDRAW_SCREEN;              // the empty field is highlighted: type it
      }
      char* end = NULL;
      const double lat = strtod(gotoLat, &end);
      const bool latOk = end && *end == '\0' && lat >= -90.0 && lat <= 90.0;
      const double lon = strtod(gotoLon, &end);
      const bool lonOk = end && *end == '\0' && lon >= -180.0 && lon <= 180.0;
      if (!latOk || !lonOk) {
        gotoField = latOk ? 1 : 0;
        return REDRAW_SCREEN;              // the bad one is highlighted
      }
      centreOn(lat, lon);
      setNote("%.5f, %.5f", lat, lon);
      enterState(MAPS_VIEW);
      return REDRAW_ALL;
    }
    if (IS_KEYBOARD(event)) {
      char c = 0;
      if (event >= '0' && event <= '9') {
        c = (char)event;
      } else if (event == '*') {
        c = '.';
      } else if (event == '#') {
        /* Toggle the sign: a minus is only ever the first character. */
        if (f[0] == '-') {
          memmove(f, f + 1, strlen(f));
        } else if (strlen(f) < 14) {
          memmove(f + 1, f, strlen(f) + 1);
          f[0] = '-';
        }
        return REDRAW_SCREEN;
      }
      if (c) {
        const size_t n = strlen(f);
        if (c == '.' && strchr(f, '.')) {
          return REDRAW_SCREEN;            // one point per number
        }
        if (n < 14) {
          f[n] = c;
          f[n + 1] = '\0';
        }
        return REDRAW_SCREEN;
      }
    }
    return DO_NOTHING;
  }

  case MAPS_CHANNEL:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MAPS_PIN_OPTS);
      return REDRAW_ALL;
    }
    if (menu) {
      menu->processEvent(event);
    }
    if (LOGIC_BUTTON_OK(event) && menu) {
      const MenuOption::keyType k = menu->currentKey();
      if (k == ROW_C_CANCEL || chanForPin < 0 || chanForPin >= pinCount) {
        enterState(MAPS_PIN_OPTS);
        return REDRAW_ALL;
      }
      /* Re-resolve by index NOW, never trusting the key frozen when the rows were drawn —
       * applyChannelUrl() can have rewritten the table since (app_meshtastic.cpp does this). */
      const MeshChannel* c = meshService.getChannel((int)k - ROW_C_BASE);
      if (!c) {
        chanPending = 0;
        buildChannels();
        return REDRAW_SCREEN;
      }
      /* The PUBLIC row takes two presses: the same idiom as the beacon's channel picker. A
       * shared pin persists on every radio that hears it, so "any radio in range" is the
       * warning to read twice. */
      if (meshService.channelIsPublic(c) && chanPending != (int)k) {
        chanPending = (int)k;
        buildChannels();
        menu->select(k);
        return REDRAW_SCREEN;
      }
      chanPending = 0;
      strlcpy(pins[chanForPin].chan, c->name, sizeof(pins[chanForPin].chan));
      pinSel = chanForPin;
      char why[72];
      sharePin(pinSel, why, sizeof(why));
      setNote("%s", why);
      enterState(MAPS_VIEW);
      return REDRAW_ALL;
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
      if (idx < 0 || idx >= listCount) {
        return REDRAW_SCREEN;
      }
      if (listKind == LIST_PINS) {
        const int pi = (int)listId[idx];
        if (pi >= 0 && pi < pinCount) {
          pinSel = pi;
          centreOn(mapI7ToDeg(pins[pi].latI), mapI7ToDeg(pins[pi].lonI));
          enterState(MAPS_PIN_OPTS);
          return REDRAW_ALL;
        }
      } else if (listKind == LIST_PLACES) {
        /* Looked up again by id, because the waypoint table compacts under an open screen:
         * the expiry sweep moves the last entry into the hole it just made. */
        const MeshWaypoint* w = meshService.findWaypoint(listId[idx]);
        if (w && w->id) {
          centreOn(mapI7ToDeg(w->latI), mapI7ToDeg(w->lonI));
          setNote("%s", w->name);
          enterState(MAPS_VIEW);
          return REDRAW_ALL;
        }
        setNote("That place is gone - it expired or was deleted");
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      } else {
        const MeshNode* nd = meshService.findNode(listId[idx]);
        if (nd && nd->posHeardMs) {
          centreOn(mapI7ToDeg(nd->latI), mapI7ToDeg(nd->lonI));
          setNote("%s", nd->name[0] ? nd->name : "?");
          enterState(MAPS_VIEW);
          return REDRAW_ALL;
        }
        setNote("That node is no longer in the list");
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
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
      case ROW_P_SHARE_ON:
        chanForPin = pinSel;
        enterState(MAPS_CHANNEL);
        return REDRAW_ALL;
      case ROW_P_SHARE: {
        /* A pin that has never been shared asks which channel first. A re-share goes back out
         * where it already is — for a pin shared before channels were recorded (chan "") that
         * is the automatic pick it went out on, and sharePin() refuses one whose channel has
         * left the phone. */
        if (!pins[pinSel].sharedId && !pinChannel(pinSel)) {
          chanForPin = pinSel;
          enterState(MAPS_CHANNEL);
          return REDRAW_ALL;
        }
        char why[72];
        sharePin(pinSel, why, sizeof(why));
        setNote("%s", why);
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      }
      case ROW_P_UNSHARE: {
        /* 🛑 KEEP THE ID WHEN THE RETRACTION DID NOT GO OUT. Forgetting it removes the pin
         * from THIS phone's Places and leaves it on everyone else's forever, with nothing on
         * this phone that knows the id any more — the retraction can never be sent again. A
         * stale camp on other people's maps is the exact failure "Take it off the mesh"
         * exists to prevent. */
        if (pins[pinSel].chan[0] && !pinChannel(pinSel)) {
          /* The channel it went out on is no longer on this phone: a retraction sent
           * anywhere else reaches nobody who has it, and would report success. */
          setNote("Channel '%s' is gone from this phone - cannot retract", pins[pinSel].chan);
          enterState(MAPS_VIEW);
          return REDRAW_ALL;
        }
        const bool ok = meshService.unshareWaypoint(pins[pinSel].sharedId, pinChannel(pinSel));
        if (ok) {
          pins[pinSel].sharedId = 0;
          pinsDirty = true;
          savePins();
          setNote("Taken off the mesh");
        } else {
          setNote("Radio not ready - the mesh still has it. Try again.");
        }
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
          if (pins[pinSel].sharedId && pins[pinSel].chan[0] && !pinChannel(pinSel)) {
            /* The confirm promised "...and take it off the mesh", and that cannot be done:
             * the channel it lives on is gone. Deleting locally would throw away the only
             * id that can ever retract it. Keep the pin and say why. */
            setNote("Channel '%s' is gone - cannot retract, kept", pins[pinSel].chan);
            enterState(MAPS_VIEW);
            return REDRAW_ALL;
          }
        }
        const bool retracted = deletePin(pinSel);
        if (retracted) {
          setNote("Deleted '%s'", gone);
        } else {
          setNote("Deleted here - but the radio did not send the retraction");
        }
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
        /* ⚠ AN EMPTY FIELD KEEPS THE NAME IT HAD. mapPinSanitizeName() turns "" into "Pin"
         * because a nameless marker is unusable — but applied here that would quietly rename
         * "Pin 3" to "Pin", and a second empty save would give you two pins called "Pin".
         * Saving nothing means changing nothing. */
        char typed[MAP_PIN_NAME_LEN];
        mapPinSanitizeName(textArea ? textArea->getText() : NULL, typed, sizeof(typed));
        const char* raw = textArea ? textArea->getText() : NULL;
        bool blank = true;
        for (const char* q = raw; q && *q; q++) {
          if (*q != ' ' && *q != '\t') {
            blank = false;
            break;
          }
        }
        if (!blank) {
          strlcpy(pins[pinSel].name, typed, sizeof(pins[pinSel].name));
        }
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
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MAPS_MENU);
      return REDRAW_ALL;
    }
    if (event == WIPHONE_KEY_UP || event == '2') {
      if (helpTop > -1) {
        helpTop--;                       // from row 0, up goes back to the drawn key
      }
      return REDRAW_SCREEN;
    }
    if (event == WIPHONE_KEY_DOWN || event == '8' || LOGIC_BUTTON_OK(event)) {
      /* OK pages down rather than leaving: on a screen that scrolls, the key people press to
       * see more must show more. Back is how you leave, and the footer says so. */
      helpTop++;
      return REDRAW_SCREEN;
    }
    return DO_NOTHING;
  }
  return DO_NOTHING;
}

void MapsApp::redrawScreen(bool redrawAll) {
  /* 🛑 THE TEXT DATUM IS SHARED STATE AND IT IS NOT LEFT AT TL_DATUM.
   * `lcd` here is the same TFT_eSPI object the header, the footer and every MenuWidget draw
   * into — GUI makes one page sprite and hands it to everyone — and `textdatum` is a plain
   * member of it, not per-caller. GUI draws the app FIRST and the header/footer AFTER, so on
   * the second and every later repaint this app inherits whatever the footer left behind:
   * MR_DATUM for the right-hand soft key, or ML/CL from the header and the menus. With a
   * MIDDLE datum drawString subtracts half the font height from y, so every string on the map
   * — the chips, the scale bar, the coordinates, the whole help screen — is drawn 8 px HIGHER
   * than it was placed, and the top chip lands in the header band. With a RIGHT datum it also
   * runs backwards from x. One line, set once per repaint, before anything draws. */
  lcd.setTextDatum(TL_DATUM);

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
  case MAPS_GOTO:
    drawGoto();
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
  armTimer();
}

void MapsApp::armTimer() {
  const bool busy = (appState == MAPS_VIEW) && (loadActive || (wantAny && !tileMemFail));
  /* The download screen ticks at 4 Hz for its progress line only while a job is running:
   * a finished screen must not keep the CPU awake either. */
  /* ...and a slow one on a finished form, so a job that ended while a call had every event
   * (a callApp takes them all) is noticed and the Stop row becomes Start again. */
  const bool progress = (appState == MAPS_DOWNLOAD);
  /* Following polls the fix once a second — a slow tick, and only on the map. */
  const bool following = followMe && (appState == MAPS_VIEW) && gGpsNmea;
  /* A held arrow polls the key every 50 ms until the finger comes off — a bounded burst, and
   * the map is being scrolled, so the CPU is awake for it anyway. */
  const bool holding = panHoldMask && (appState == MAPS_VIEW);
  controlState.msAppTimerEventPeriod = busy ? 25 : (holding ? 50 : (progress ? 250 : (following ? 1000 : 0)));
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
      bool overlong = false;
      MapPin tmp;
      /* ⚠ THE SAME OVER-LONG RULE loadPins USES. Without it this counter silently parses the
       * FIRST MAP_PIN_LINE_MAX bytes of a long line as a pin and calls it good, while the app
       * counts the same line as unreadable — so the console would report more pins than the
       * map draws, which is precisely the question somebody runs `maps` to answer. */
      while (f.available()) {
        const int ch = f.read();
        if (ch < 0) {
          break;
        }
        if (ch == '\n' || ch == '\r') {
          line[n] = '\0';
          if (overlong) {
            badN++;
          } else if (n) {
            const int r = mapPinParseLine(line, &tmp);
            if (r == 1) {
              pinN++;
            } else if (r < 0) {
              badN++;
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
      if (n && !overlong) {
        line[n] = '\0';              // a last line with no newline is still a line
        const int r = mapPinParseLine(line, &tmp);
        if (r == 1) {
          pinN++;
        } else if (r < 0) {
          badN++;
        }
      } else if (overlong) {
        badN++;
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
  /* ⚠ SAY THAT AN OPEN APP WILL OVERWRITE THIS. MapsApp::saveView() runs in the destructor,
   * so closing the app writes wherever it was actually looking, over whatever was set here.
   * That precedence is right — the screen is the truth while somebody is using it — but a
   * person setting a coordinate over the cable with the app open would otherwise watch it
   * vanish with no explanation. */
  snprintf(why, whyCap, "next open: %.5f,%.5f z%d%s%s (close Maps first if it is open)",
           lat, lon, z, buf[0] ? " area " : "", buf);
  return true;
}
