/*
 * test_maptiles.cpp — map_tiles.cpp and map_pins.cpp on the host.
 *
 * The projection numbers are computed independently (Python, same Web Mercator formulas,
 * double precision) and pasted in; the blit geometry is checked STRUCTURALLY rather than
 * against a golden list, because "did it cover the screen exactly once, and did each piece
 * come from the right place in the right tile" is the property that actually matters and it
 * is one a golden list does not state.
 *
 * Why this suite is worth its weight: a map that is one tile out looks perfectly fine on a
 * 240x320 screen. It just shows you the wrong woods, and there is nothing on the screen that
 * says so. Arithmetic errors here cannot be caught by looking, so they are caught here.
 */

#include "../WiPhone/map_tiles.h"
#include "../WiPhone/map_pins.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, name) do { \
    checks++; \
    if (cond) { printf("  ok  %s\n", name); } \
    else { printf("  FAIL %s (line %d)\n", name, __LINE__); failures++; } \
  } while (0)

static bool nearly(double a, double b, double eps) {
  return fabs(a - b) <= eps;
}

/* Every viewport pixel the world covers is painted exactly once, by a piece whose source
 * pixel is the right one. This is the whole correctness argument for the renderer. */
static void coverCheck(int z, int32_t cx, int32_t cy, int vw, int vh, const char* label) {
  MapBlit b[MAP_MAX_BLITS];
  const int n = mapViewBlits(z, cx, cy, vw, vh, b, MAP_MAX_BLITS);
  char nm[160];
  snprintf(nm, sizeof(nm), "%s: blits returned (%d)", label, n);
  CHECK(n > 0, nm);
  if (n <= 0) {
    return;
  }

  const int32_t ws = mapWorldPx(z);
  const long long left = (long long)cx - vw / 2;
  const long long top  = (long long)cy - vh / 2;

  int* paint = (int*)malloc(sizeof(int) * (size_t)vw * (size_t)vh);
  for (int i = 0; i < vw * vh; i++) {
    paint[i] = 0;
  }

  bool srcOk = true, mapOk = true, boundsOk = true;
  for (int i = 0; i < n; i++) {
    const MapBlit* q = &b[i];
    if (q->srcX < 0 || q->srcY < 0 || q->w <= 0 || q->h <= 0 ||
        q->srcX + q->w > MAP_TILE_PX || q->srcY + q->h > MAP_TILE_PX) {
      srcOk = false;
    }
    if (q->dstX < 0 || q->dstY < 0 || q->dstX + q->w > vw || q->dstY + q->h > vh) {
      boundsOk = false;
      continue;
    }
    if (q->tileX < 0 || q->tileY < 0 ||
        q->tileX >= ws / MAP_TILE_PX || q->tileY >= ws / MAP_TILE_PX) {
      boundsOk = false;
    }
    for (int y = 0; y < q->h; y++) {
      for (int x = 0; x < q->w; x++) {
        paint[(q->dstY + y) * vw + (q->dstX + x)]++;
        // The world pixel this screen pixel claims to be, two independent ways.
        long long wantX = (left + q->dstX + x) % ws;
        if (wantX < 0) {
          wantX += ws;
        }
        const long long gotX = (long long)q->tileX * MAP_TILE_PX + q->srcX + x;
        const long long wantY = top + q->dstY + y;
        const long long gotY = (long long)q->tileY * MAP_TILE_PX + q->srcY + y;
        if (gotX != wantX || gotY != wantY) {
          mapOk = false;
        }
      }
    }
  }
  snprintf(nm, sizeof(nm), "%s: every piece lies inside its tile", label);
  CHECK(srcOk, nm);
  snprintf(nm, sizeof(nm), "%s: every piece lies inside the viewport and a real tile", label);
  CHECK(boundsOk, nm);
  snprintf(nm, sizeof(nm), "%s: every pixel comes from the right world pixel", label);
  CHECK(mapOk, nm);

  bool exact = true;
  for (int y = 0; y < vh; y++) {
    const long long worldY = top + y;
    const int want = (worldY >= 0 && worldY < ws) ? 1 : 0;   // computed WITHOUT the code under test
    for (int x = 0; x < vw; x++) {
      if (paint[y * vw + x] != want) {
        exact = false;
      }
    }
  }
  snprintf(nm, sizeof(nm), "%s: covers the world exactly once, and nothing else", label);
  CHECK(exact, nm);
  free(paint);
}

int main() {
  printf("test_maptiles\n");

  // ---- world geometry -------------------------------------------------------
  CHECK(mapWorldPx(0) == 256 && mapWorldPx(15) == 8388608 && mapWorldPx(19) == 134217728,
        "world size at z0/z15/z19");
  CHECK(mapWorldPx(-3) == 256 && mapWorldPx(99) == mapWorldPx(MAP_ZOOM_MAX),
        "zoom clamped both ways");

  {
    double wx = 0, wy = 0;
    mapLatLonToWorld(47.6062, -122.3321, 15, &wx, &wy);      // Seattle
    CHECK(nearly(wx, 1343759.464676, 1e-3) && nearly(wy, 2929662.722766, 1e-3),
          "Seattle z15 world pixels match the reference");
    CHECK((int)wx / MAP_TILE_PX == 5249 && (int)wy / MAP_TILE_PX == 11443,
          "Seattle z15 lands in tile 5249/11443");

    mapLatLonToWorld(-33.8688, 151.2093, 14, &wx, &wy);      // Sydney: southern + eastern
    CHECK(nearly(wx, 3858868.032853, 1e-3) && nearly(wy, 2516969.330663, 1e-3),
          "Sydney z14 world pixels match the reference");

    mapLatLonToWorld(0, 0, 0, &wx, &wy);
    CHECK(nearly(wx, 128.0, 1e-9) && nearly(wy, 128.0, 1e-9), "null island is the middle of z0");

    double lat = 0, lon = 0;
    mapLatLonToWorld(47.6062, -122.3321, 15, &wx, &wy);
    mapWorldToLatLon(wx, wy, 15, &lat, &lon);
    CHECK(nearly(lat, 47.6062, 1e-9) && nearly(lon, -122.3321, 1e-9),
          "lat/lon -> world -> lat/lon round trips");
  }

  CHECK(nearly(mapWrapLon(180.0), -180.0, 1e-12), "+180 and -180 are the same meridian");
  CHECK(nearly(mapWrapLon(-180.0), -180.0, 1e-12), "-180 stays -180");
  CHECK(nearly(mapWrapLon(190.0), -170.0, 1e-12), "190 wraps to -170, it does not clamp");
  CHECK(nearly(mapWrapLon(-190.0), 170.0, 1e-12), "-190 wraps to 170");
  CHECK(nearly(mapClampLat(90.0), MAP_LAT_LIMIT, 1e-12) &&
        nearly(mapClampLat(-90.0), -MAP_LAT_LIMIT, 1e-12), "poles clamp to the Mercator limit");

  CHECK(mapDegToI7(47.6062) == 476062000 && mapDegToI7(-122.3321) == -1223321000,
        "degrees -> 1e-7 fixed point");
  CHECK(mapDegToI7(1.00000005) == 10000001 && mapDegToI7(-1.00000005) == -10000001,
        "rounding is symmetric about zero, not truncation");
  CHECK(nearly(mapI7ToDeg(476062000), 47.6062, 1e-9), "1e-7 fixed point -> degrees");

  CHECK(nearly(mapMetersPerPixel(47.6062, 15), 3.220972649878169, 1e-9),
        "metres per pixel at Seattle z15");
  CHECK(nearly(mapMetersPerPixel(0, 0), 156543.03392804097, 1e-6),
        "metres per pixel at the equator, z0");

  // ---- the blits ------------------------------------------------------------
  {
    const int VW = 240, VH = 250;
    int32_t cx = 1343759, cy = 2929662;
    coverCheck(15, cx, cy, VW, VH, "Seattle z15");

    // Exactly on a tile corner: the case where an off-by-one hides.
    coverCheck(15, 5249 * 256, 11443 * 256, VW, VH, "tile corner");
    coverCheck(15, 5249 * 256 + 128, 11443 * 256 + 128, VW, VH, "tile centre");

    // Straddling the antimeridian: the right-hand tiles must wrap to column 0.
    const int32_t ws12 = mapWorldPx(12);
    coverCheck(12, ws12 - 4, 200000, VW, VH, "antimeridian seam");
    {
      MapBlit b[MAP_MAX_BLITS];
      const int n = mapViewBlits(12, ws12 - 4, 200000, VW, VH, b, MAP_MAX_BLITS);
      bool sawWrap = false;
      for (int i = 0; i < n; i++) {
        if (b[i].tileX == 0) {
          sawWrap = true;
        }
      }
      CHECK(sawWrap, "the seam really does pull in tile column 0");
    }

    // A world shorter than the screen: letterboxed, never repeated vertically.
    coverCheck(0, 128, 128, VW, 300, "z0 with a 300px viewport (letterbox)");

    // A world NARROWER than the viewport: the world repeats sideways (longitude is a
    // circle) rather than looping forever or overrunning the blit array.
    coverCheck(0, 128, 128, 300, 200, "z0 with a 300px-wide viewport (world repeats)");

    // An odd viewport height: the clamp's vh/2 must not lose a row.
    coverCheck(14, 3858868, 2516969, 240, 251, "odd viewport height");

    // A pan far past the world, at the deepest zoom, must fold rather than overflow int32.
    {
      int32_t px = mapWorldPx(19) - 3, py = 100000;
      mapClampView(19, 240, 250, &px, &py);
      CHECK(mapPanView(19, 240, 250, 2000000000, 0, &px, &py) == 1 &&
            px >= 0 && px < mapWorldPx(19), "a two-billion-pixel pan folds into the world");
      coverCheck(19, px, py, 240, 250, "z19 after the huge pan");
    }

    // A cap that is too small must refuse outright, not half-draw.
    MapBlit one[1];
    CHECK(mapViewBlits(15, cx, cy, VW, VH, one, 1) == -1, "too few blit slots refuses");
    CHECK(mapViewBlits(15, cx, cy, VW, VH, NULL, 8) == -1, "a NULL blit array refuses");
  }

  // ---- panning and zooming --------------------------------------------------
  {
    const int VW = 240, VH = 250;
    int32_t cx = 1343759, cy = 2929662;
    const int32_t x0 = cx, y0 = cy;
    CHECK(mapPanView(15, VW, VH, 96, 0, &cx, &cy) == 1, "panning east moves the view");
    CHECK(mapPanView(15, VW, VH, -96, 0, &cx, &cy) == 1, "panning back west moves it again");
    CHECK(cx == x0 && cy == y0, "east then west lands on the same pixel");

    // Against the northern stop: the clamp holds and says nothing moved.
    cx = 1000;
    cy = 0;
    mapClampView(15, VW, VH, &cx, &cy);
    CHECK(cy == VH / 2, "the view clamps to the top of the world");
    CHECK(mapPanView(15, VW, VH, 0, -40, &cx, &cy) == 0, "panning past the top reports no move");

    // Longitude wraps instead of stopping.
    const int32_t ws = mapWorldPx(15);
    cx = ws - 10;
    cy = 2929662;
    CHECK(mapPanView(15, VW, VH, 40, 0, &cx, &cy) == 1 && cx == 30,
          "panning east off the edge comes back on the west side");

    // Zoom keeps the ground under the crosshair and is reversible.
    cx = 1343759;
    cy = 2929662;
    double lat0 = 0, lon0 = 0;
    mapViewToLatLon(15, cx, cy, VW, VH, VW / 2, VH / 2, &lat0, &lon0);
    int z = mapZoomView(10, 17, 15, +1, VW, VH, &cx, &cy);
    CHECK(z == 16, "zoom in goes to 16");
    double lat1 = 0, lon1 = 0;
    mapViewToLatLon(z, cx, cy, VW, VH, VW / 2, VH / 2, &lat1, &lon1);
    CHECK(nearly(lat0, lat1, 1e-4) && nearly(lon0, lon1, 1e-4),
          "the crosshair is still over the same ground after zooming in");
    z = mapZoomView(10, 17, z, -1, VW, VH, &cx, &cy);
    CHECK(z == 15 && cx == 1343759 && cy == 2929662, "zoom in then out is exactly reversible");
    CHECK(mapZoomView(12, 15, 15, +1, VW, VH, &cx, &cy) == 15,
          "zoom stops at the deepest level the card has");
    CHECK(mapZoomView(12, 15, 12, -1, VW, VH, &cx, &cy) == 12,
          "zoom stops at the shallowest level the card has");
  }

  CHECK(mapPanStep(0) == 24 && mapPanStep(1) == 24, "the first presses are a nudge");
  {
    bool grows = true;
    for (int i = 1; i < 30; i++) {
      if (mapPanStep(i) < mapPanStep(i - 1)) {
        grows = false;
      }
    }
    CHECK(grows && mapPanStep(100) == 96, "a held key accelerates, and stops accelerating");
  }

  {
    int px = 0;
    CHECK(mapScaleBar(3.220972649878169, 100, &px) == 200 && px == 62,
          "Seattle z15 scale bar is 200 m / 62 px");
    CHECK(mapScaleBar(0.0, 100, &px) == 0 && px == 0, "a nonsense scale asks for no bar");
    const int m = mapScaleBar(156543.0, 100, &px);
    CHECK(m > 0 && px >= 1 && px <= 100, "a z0 scale bar still fits the box");
  }

  // ---- tile paths and area names -------------------------------------------
  {
    char p[128];
    CHECK(mapTilePath(p, sizeof(p), "/maps", "home", 15, 5249, 11443) > 0 &&
          !strcmp(p, "/maps/home/15/5249/11443.565"), "tile path");
    char tiny[8];
    CHECK(mapTilePath(tiny, sizeof(tiny), "/maps", "home", 15, 5249, 11443) == 0 &&
          tiny[0] == '\0', "a path that will not fit is refused, never truncated");
    CHECK(mapTilePath(p, sizeof(p), "/maps", "home", 15, -1, 0) == 0, "a negative tile is refused");
  }
  CHECK(mapAreaNameOk("home") && mapAreaNameOk("unit-3") && mapAreaNameOk("elk_2026"),
        "ordinary area names are accepted");
  CHECK(!mapAreaNameOk(".DS_Store") && !mapAreaNameOk("._home"),
        "macOS sidecar files are not areas");
  CHECK(!mapAreaNameOk("") && !mapAreaNameOk("a/b") && !mapAreaNameOk("a b"),
        "empty, pathy and spacey names are refused");
  CHECK(!mapAreaNameOk("0123456789012345678901234567890123"), "an over-long name is refused");

  // ---- pins -----------------------------------------------------------------
  {
    MapPin p;
    CHECK(mapPinParseLine("476062000,-1223321000,0,Camp", &p) == 1 &&
          p.latI == 476062000 && p.lonI == -1223321000 && p.sharedId == 0 &&
          !strcmp(p.name, "Camp") && p.chan[0] == '\0',
          "the older four-field line parses, with no channel");
    CHECK(mapPinParseLine("476062000,-1223321000,99,hunt-group,Camp", &p) == 1 &&
          p.sharedId == 99 && !strcmp(p.chan, "hunt-group") && !strcmp(p.name, "Camp"),
          "the five-field line carries the channel before the name");
    CHECK(mapPinParseLine("476062000,-1223321000,99,,Camp, the far one", &p) == 1 &&
          p.chan[0] == '\0' && !strcmp(p.name, "Camp  the far one"),
          "an empty channel field is fine, and a comma in the name is still a space");
    CHECK(mapPinParseLine("476062000,-1223321000,3735928559,The gate", &p) == 1 &&
          p.sharedId == 3735928559u && !strcmp(p.name, "The gate"),
          "a shared pin keeps its waypoint id, and a name may hold spaces");
    CHECK(mapPinParseLine("", &p) == 0 && mapPinParseLine("  # a note", &p) == 0,
          "blank lines and comments are skipped, not errors");
    CHECK(mapPinParseLine("476062000,-1223321000,0", &p) == -1, "a short line is malformed");
    CHECK(mapPinParseLine("47.6062,-122.3,0,Camp", &p) == -1,
          "a float latitude is malformed, not silently truncated");
    CHECK(mapPinParseLine("950000000,0,0,North pole", &p) == -1, "an impossible latitude is refused");
    CHECK(mapPinParseLine("0,1900000000,0,East of east", &p) == -1, "an impossible longitude is refused");
    CHECK(mapPinParseLine("476062000,-1223321000,0,", &p) == 1 && !strcmp(p.name, "Pin"),
          "an empty name becomes something you can see");

    char line[MAP_PIN_LINE_MAX];
    MapPin q;
    memset(&q, 0, sizeof(q));
    q.latI = -338688000;
    q.lonI = 1512093000;
    q.sharedId = 7;
    strcpy(q.name, "Truck");
    CHECK(mapPinFormatLine(&q, line, sizeof(line)) > 0 &&
          !strcmp(line, "-338688000,1512093000,7,,Truck"), "a never-shared pin formats with an empty channel");
    strcpy(q.chan, "hunt-group");
    CHECK(mapPinFormatLine(&q, line, sizeof(line)) > 0 &&
          !strcmp(line, "-338688000,1512093000,7,hunt-group,Truck"), "a shared pin formats with its channel");
    MapPin r;
    CHECK(mapPinParseLine(line, &r) == 1 && r.latI == q.latI && r.lonI == q.lonI &&
          r.sharedId == q.sharedId && !strcmp(r.name, q.name) && !strcmp(r.chan, q.chan),
          "format -> parse round trips, channel included");
    strcpy(q.chan, "odd,name");
    CHECK(mapPinFormatLine(&q, line, sizeof(line)) > 0 && mapPinParseLine(line, &r) == 1 &&
          !strcmp(r.chan, "odd name") && !strcmp(r.name, "Truck"),
          "a comma in a channel name cannot become a sixth field");
    char small[10];
    CHECK(mapPinFormatLine(&q, small, sizeof(small)) == 0 && small[0] == '\0',
          "a line that will not fit is refused, never truncated");

    char nm[MAP_PIN_NAME_LEN];
    mapPinSanitizeName("  Big, Rock  ", nm, sizeof(nm));
    CHECK(!strcmp(nm, "Big  Rock"), "commas become spaces and the ends are trimmed");
    mapPinSanitizeName("a\nb\tc", nm, sizeof(nm));
    CHECK(!strcmp(nm, "a b c"), "control characters become spaces");
    mapPinSanitizeName("   ", nm, sizeof(nm));
    CHECK(!strcmp(nm, "Pin"), "a name of nothing becomes 'Pin'");
    mapPinSanitizeName("012345678901234567890123456789", nm, sizeof(nm));
    CHECK(strlen(nm) == MAP_PIN_NAME_LEN - 1, "a long name is cut to the waypoint's length");

    /* A cut that lands mid-codepoint must drop the whole character. These bytes go into a
     * protobuf string field on the air; half a sequence is invalid UTF-8 and a strict reader
     * may reject the entire Waypoint. 19 usable bytes: 18 ASCII then a 2-byte 'e' with an
     * acute, whose second byte does not fit. */
    mapPinSanitizeName("aaaaaaaaaaaaaaaaaa\xc3\xa9zz", nm, sizeof(nm));
    CHECK(strlen(nm) == 18 && nm[17] == 'a',
          "a name cut mid-UTF-8 drops the whole character, not half of it");
    bool tail_ok = true;
    for (size_t i = 0; i < strlen(nm); i++) {
      if (((unsigned char)nm[i] & 0xC0) == 0x80) {
        tail_ok = false;                     // a bare continuation byte survived
      }
    }
    CHECK(tail_ok, "...and no orphan continuation byte is left behind");
    // A sequence that FITS is untouched: 17 ASCII + a 2-byte character = 19 bytes.
    mapPinSanitizeName("aaaaaaaaaaaaaaaaa\xc3\xa9", nm, sizeof(nm));
    CHECK(strlen(nm) == 19, "a multi-byte character that fits is kept whole");
  }

  {
    MapPin pins[3];
    memset(pins, 0, sizeof(pins));
    strcpy(pins[0].name, "Pin 1");
    strcpy(pins[1].name, "Pin 3");
    strcpy(pins[2].name, "Camp");
    char nm[MAP_PIN_NAME_LEN];
    CHECK(mapPinAutoName(pins, 3, nm, sizeof(nm)) == 2 && !strcmp(nm, "Pin 2"),
          "the next pin fills the gap rather than counting up forever");
    CHECK(mapPinAutoName(NULL, 0, nm, sizeof(nm)) == 1 && !strcmp(nm, "Pin 1"),
          "the first pin on an empty map is Pin 1");
  }

  {
    const int vx[4] = { 120, 130,  10, 120 };
    const int vy[4] = { 125, 128, 240, 125 };
    CHECK(mapPinPickNearest(vx, vy, 4, 120, 125, 12) == 0, "the pin under the crosshair wins");
    CHECK(mapPinPickNearest(vx, vy, 4, 131, 129, 12) == 1, "the nearer of two close pins wins");
    CHECK(mapPinPickNearest(vx, vy, 4, 200, 20, 12) == -1, "nothing near the crosshair picks nothing");
    CHECK(mapPinPickNearest(vx, vy, 4, 120, 125, 0) == 0, "a zero radius still matches an exact hit");

    /* 🛑 THE OVERFLOW. mapLatLonToView writes through for points nowhere near the screen, and
     * at z19 the world is 134 million pixels across. Squared in 32-bit that wraps, and a
     * wrapped product can come out NEGATIVE — which beats every real distance, so a pin on
     * the far side of the world would be picked as the one under the crosshair. */
    const int farX[3] = { 120, 46341, 134217000 };
    const int farY[3] = { 999, 46341, 134217000 };
    CHECK(mapPinPickNearest(farX, farY, 3, 120, 125, 14) == -1,
          "pins a world away are not 'under the crosshair', whatever the arithmetic wraps to");
    const int mixX[3] = { 134217000, 121, -134217000 };
    const int mixY[3] = { 134217000, 126, -134217000 };
    CHECK(mapPinPickNearest(mixX, mixY, 3, 120, 125, 14) == 1,
          "...and the one that IS under it still wins, with huge ones on both sides");
  }

  printf("\n%d checks, %d failures\n", checks, failures);
  if (failures) {
    printf("test_maptiles: %d FAILURE(S)\n", failures);
  } else {
    printf("test_maptiles: all passed\n");
  }
  return failures ? 1 : 0;
}
