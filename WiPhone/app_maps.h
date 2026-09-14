/*
 * app_maps.h — an offline map. Tiles off the SD card, pins you drop yourself, and everything
 * the mesh already knows about where people are, on one screen.
 *
 * Main menu -> Tools -> Maps.
 *
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * WHAT IT IS FOR
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * The Meshtastic screens already carry every fact this app draws: camp is a waypoint, the
 * truck is a waypoint, everyone's last position arrives on port 3 every few minutes, and the
 * Nodes list turns them into "3.2km E of camp - 4 min ago". That sentence is correct and it
 * is the wrong shape for the question people actually ask, which is "is he on the other side
 * of the creek or this side". A map answers that and a list cannot.
 *
 * So: no new radio traffic, no new database, no new source of truth. This app is a VIEW of
 * state that already exists, plus one thing of its own — pins, which are your private marks
 * on your own map and are shared with the mesh only when you say so.
 *
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * THE TILES: RAW RGB565, AND WHY NOT PNG
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * /maps/<area>/<z>/<x>/<y>.565 — 256x256 pixels, RGB565, little-endian, 131072 bytes exactly.
 * Standard slippy-map z/x/y numbering, so the tiles a computer already has for COVEY's map are
 * the same tiles, converted once by tools/convert_tiles.py. docs/maps.md is the format.
 *
 * 🛑 THE PHONE DECODES NOTHING, AND THAT IS THE DESIGN, NOT A SHORTCUT. There is no PNG
 * decoder in this firmware and adding one costs a vendored library plus contiguous internal
 * RAM on a phone whose crashes are internal-heap fragmentation (app_photos.h has the whole
 * argument). The ESP32's ROM JPEG decoder exists but refuses greyscale outright — jpeg_grey.h
 * exists solely because 33 of 45 pictures in one book were 1-component JPEGs. A map made of
 * compressed tiles would therefore work until the day it met a tile of the wrong flavour, in
 * the woods, with no way to tell what was wrong.
 *
 * Raw costs 8x the card space (~85 MB for a 20x20 km area at z12-z15 — nothing on a 32 GB
 * card) and buys: zero decode time, zero decoder memory, and exactly ONE failure mode, which
 * is "the file is not 131072 bytes long" and is checkable in one comparison.
 *
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * WHY THE CARD IS READ IN PIECES
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * ⚠ ONE TILE IS 128 KB AND EVERYTHING IN THIS FIRMWARE SHARES ONE TASK. A 128 KB SD read is
 * 100-250 ms on this hardware, and 250 ms is precisely the threshold the superloop's own
 * stall detector was built to complain about (0.9.58) — the same 1-2 s freezes that drop WiFi
 * and eat keypresses. Four tiles for one screen, read the obvious way, is a second of frozen
 * phone every time you pan.
 *
 * So a tile arrives in MAPS_CHUNK_BYTES pieces, one piece per timer tick, and the map draws
 * the hole in the meantime as a plainly-marked grey square. The screen is honest about what
 * it does not have yet, the keypad never stops answering, and no single pass through the
 * superloop owes more than a fraction of a tile.
 *
 * The file is opened, seeked, read and CLOSED on every piece rather than held open across
 * ticks. That costs a directory lookup per piece and removes the entire class of bug where an
 * app is torn down, or the card is pulled, with a File still live.
 *
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * MEMORY
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * MAPS_TILE_SLOTS x 128 KB of PSRAM, allocated ONCE when the app opens and freed when it
 * closes — no churn, because heap churn is what this phone dies of. Internal RAM: a file
 * handle and some ints. The app itself is PSRAM too (WiPhoneApp::operator new).
 *
 * A 240x250 viewport over 256 px tiles touches at most 2x2 = 4 tiles, so 6 slots means
 * panning across a tile boundary does not immediately evict something about to be needed
 * again. Fewer than 2 slots and the app refuses to run and says why.
 */
#ifndef APP_MAPS_H
#define APP_MAPS_H

#include "GUI.h"
#include "map_tiles.h"
#include "map_pins.h"

#define MAPS_ROOT          "/maps"
#define MAPS_PINS_FILE     "/maps/pins.txt"

#define MAPS_MAX_PINS      64      // PSRAM, ~2 KB. A day's worth of marks, not a database.
#define MAPS_MAX_AREAS     12
#define MAPS_TILE_SLOTS    6       // x 128 KB PSRAM; see the note above
#define MAPS_MISS_CACHE    24      // tiles known not to be on the card: do not re-ask per frame
#define MAPS_CHUNK_BYTES   (32u * 1024u)
#define MAPS_PICK_RADIUS   14      // how near the crosshair a pin must be to be "under" it

/* Everything the serial console needs, so this app can be driven without a thumb.
 *
 * ⚠ THIS IS NOT A CONVENIENCE. "Set as wallpaper" reached a user completely untried because
 * every path into it needed a finger on a key and a serial cable cannot press keys
 * (app_photos.cpp). A map is worse: its failure mode is showing the wrong ground, which looks
 * exactly like showing the right ground. `maps` prints what the card holds and where the view
 * is; `maps goto` moves it. Both drive the same code the keypad does. */
bool mapsConsoleStatus(char* out, size_t cap);
bool mapsConsoleGoto(double lat, double lon, int z, const char* area, char* why, size_t whyCap);

class MapsApp : public WindowedApp {
public:
  MapsApp(LCD& disp, ControlState& state, HeaderWidget* header, FooterWidget* footer);
  virtual ~MapsApp();
  ActionID_t getId() {
    return GUI_APP_MAPS;
  }
  appEventResult processEvent(EventType event);
  void redrawScreen(bool redrawAll = false);

protected:
  typedef enum {
    MAPS_VIEW,          // the map itself
    MAPS_MENU,          // the map's own menu (Back from here returns to the map)
    MAPS_LIST,          // pins / places / nodes, whichever listKind says
    MAPS_PIN_OPTS,      // one pin: rename, move, share, delete
    MAPS_RENAME,        // the text field behind "Rename"
    MAPS_AREAS,         // which map folder on the card
    MAPS_CONFIRM_DEL,
    MAPS_HELP,
  } MapsState_t;

  typedef enum { LIST_PINS = 0, LIST_PLACES, LIST_NODES } MapsListKind_t;

  // ---- one cached tile ----
  struct TileSlot {
    uint16_t* px;          // MAP_TILE_PX*MAP_TILE_PX pixels, PSRAM. NULL = slot never allocated
    int       z, tx, ty;   // what it holds; z < 0 = empty
    uint32_t  used;        // LRU stamp
    bool      ready;       // fully read; false while a load is in flight
  };

  struct MapArea {
    char name[32];
    int  zMin, zMax;       // the zoom folders that actually exist
  };

  // ---- state ----
  MapsState_t appState;
  MapsListKind_t listKind;

  MenuWidget*          menu;
  MultilineTextWidget* textArea;
  char headerTitle[40];     // HeaderWidget stores the POINTER; this must outlive the call
  char note[72];            // one line of result/why nothing happened, shown on the map
  char statusLine[48];      // "loading 2 tiles" / "no tiles here" — the map's own honesty

  // The view. See map_tiles.h for why this is world pixels and not a lat/lon pair.
  int      zoom;
  int32_t  cx, cy;
  int      vpX, vpY, vpW, vpH;

  MapArea  areas[MAPS_MAX_AREAS];
  int      areaCount;
  int      areaSel;         // index into areas[], -1 when the card has no maps at all

  MapPin*  pins;            // PSRAM
  int      pinCount;
  int      pinSel;          // index into pins[] for PIN_OPTS/RENAME/CONFIRM_DEL
  bool     pinsDirty;
  int*     pinVx;           // projection scratch, PSRAM: members not stack, this is the GUI task
  int*     pinVy;

  TileSlot slots[MAPS_TILE_SLOTS];
  int      slotsAlive;
  uint32_t useSeq;

  struct { int z, tx, ty; } misses[MAPS_MISS_CACHE];
  int      missCount, missNext;

  // The load in flight (one at a time, one piece per tick).
  bool     loadActive;
  int      loadSlot, loadZ, loadTx, loadTy;
  uint32_t loadGot;

  // Wanted but not resident: what the next load should fetch.
  bool     wantAny;
  int      wantZ, wantTx, wantTy;
  int      pendingTiles;

  int      panRun;          // consecutive presses in panDir, for mapPanStep()
  int      panDir;
  uint32_t panLastMs;

  // ---- setup / teardown ----
  bool  allocSlots();
  void  freeSlots();
  void  scanAreas();
  void  loadPins();
  void  savePins();
  void  restoreView();      // last view, else GPS, else the mesh reference, else the map
  void  saveView();

  bool     tileMemFail;     // no PSRAM for tiles: say so once, stop asking the card

  // ---- tiles ----
  int   findSlot(int z, int tx, int ty) const;    // -1 = not cached
  int   claimSlot();                              // LRU victim, never one mid-load
  bool  isKnownMissing(int z, int tx, int ty) const;
  void  rememberMissing(int z, int tx, int ty);
  void  forgetMissing();
  bool  startLoad(int z, int tx, int ty);
  void  cancelLoad();                             // the zoom or area changed under it
  bool  tileLoadStep();                           // one piece; true = a tile just finished

  // ---- drawing ----
  /* ⚠ EVERY PIXEL THIS APP DRAWS GOES THROUGH fillClipped OR A GUARDED drawString.
   * The screen is one 240x320 sprite shared with the header and the footer, and
   * TFT_eSprite::pushImage clips only the right and bottom edges — a negative x or y walks
   * off the start of the buffer. Marker shapes near the edge of the map are exactly where
   * those coordinates come from, so nothing here is allowed to pass raw coordinates to the
   * display; it goes through the clip. */
  void  fillClipped(int x, int y, int w, int h, uint16_t colour);
  void  drawBlob(int x, int y, int r, uint16_t colour, bool diamond);
  void  drawMap();
  void  drawNoMapPage();
  void  drawOverlays();
  void  drawMarker(int vx, int vy, uint16_t colour, int shape);
  void  drawLabel(int vx, int vy, const char* text, uint16_t colour);
  void  drawCrosshair();
  void  drawBottomStrip();
  void  drawChips();

  // ---- actions ----
  void  centreOnMe();
  void  centreOn(double lat, double lon);
  int   pinUnderCrosshair();
  bool  dropPin();
  bool  sharePin(int idx, char* why, size_t whyCap);
  void  deletePin(int idx);
  void  stepPin(int delta);
  void  setArea(int idx);
  void  setNote(const char* fmt, ...);

  // ---- screens ----
  void  freeMenu();
  void  freeWidgets();
  void  enterState(MapsState_t st);
  MenuWidget* newMenu(const char* emptyMessage);
  void  buildMenu();
  void  buildList();
  void  buildPinOpts();
  void  buildRename();
  void  buildAreas();
  void  buildConfirmDelete();
  void  drawHelp();

  appEventResult onMapKey(EventType event);
};

#endif // APP_MAPS_H
