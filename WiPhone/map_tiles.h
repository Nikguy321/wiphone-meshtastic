/*
 * map_tiles.h — the arithmetic behind the Maps app: Web Mercator, the slippy-tile
 * grid, and the rectangles that carry a tile onto the screen.
 *
 * ── WHY THIS IS ITS OWN FILE, FREE OF ARDUINO ────────────────────────────────────────────
 * Everything here is pure: integers, doubles, and no hardware. That is deliberate and it is
 * the same bargain mesh_pos.h struck — tests/test_maptiles.cpp compiles THESE SOURCES on the
 * host and checks them against values computed independently, so the map's geometry is proven
 * before a phone is involved. A map that is one tile out is not obviously wrong on a 240x320
 * screen; it just quietly shows you the wrong woods. That class of bug has to be caught by
 * arithmetic, not by looking.
 *
 * ── THE VIEW IS AN INTEGER, NOT A FLOAT ──────────────────────────────────────────────────
 * 🛑 The view centre is stored as WORLD PIXELS at the current zoom (int32), never as a
 * lat/lon pair. Panning is then integer addition and is exactly reversible: press right then
 * left and you are on the same pixel you started on. Storing lat/lon and re-projecting every
 * press accumulates float error in the projection's `log(tan(...))`, and it drifts fastest at
 * the latitudes furthest from the equator — i.e. everywhere anyone actually hunts. Latitude
 * and longitude are computed at the EDGES only: when the view is saved, when the crosshair is
 * read out, and when a pin is projected.
 *
 * World pixels at zoom z run 0 .. 256*2^z in both axes. At the MAP_ZOOM_MAX of 19 that is
 * 134,217,728 — comfortably inside int32, which is why int32 is enough and int64 is not
 * needed anywhere below. The intermediate `wx + vw` sums are still done in int64 so that a
 * viewport hanging off the right-hand edge cannot overflow on the way to being wrapped.
 *
 * ── X WRAPS, Y CLAMPS ────────────────────────────────────────────────────────────────────
 * Longitude is a circle and latitude is not. Panning east past 180 deg comes back at -180
 * (the tile column index is taken modulo the world width); panning north past the top stops.
 * Mercator cannot represent the poles at all, so latitude is clamped to +/-85.0511287798066
 * deg — the latitude at which the projection is exactly square, and the value every slippy
 * map on earth uses.
 */
#ifndef MAP_TILES_H
#define MAP_TILES_H

#include <stddef.h>
#include <stdint.h>

/* Raster tiles are 256x256, RGB565, 131072 bytes exactly. See docs/maps.md for the file
 * format and tools/convert_tiles.py for the converter that writes it.
 *
 * ⚠ THE PHONE DOES NOT DECODE ANYTHING. There is no PNG decoder in this firmware (see
 * app_photos.h for why), and the ESP32's ROM JPEG decoder refuses greyscale outright
 * (jpeg_grey.h). A map that depended on either would fail on exactly the tiles a user
 * happened to have. Raw RGB565 is 8x larger on the card and costs ZERO decode, zero decoder
 * RAM and has exactly one failure mode: the file is the wrong length, which is checkable in
 * one comparison. On a card with 32 GB and a phone with 19 KB of internal heap, that is the
 * right side of the trade. docs/maps-plan.md reached the same conclusion in 2026-08. */
#define MAP_TILE_PX       256
#define MAP_TILE_BYTES    ((size_t)MAP_TILE_PX * MAP_TILE_PX * 2)

#define MAP_ZOOM_MIN      0
#define MAP_ZOOM_MAX      19

// The latitude Web Mercator stops at. Not a rounding of 85: it is atan(sinh(pi)) in degrees.
#define MAP_LAT_LIMIT     85.0511287798066

/* One tile's worth of pixels on their way to the screen. Every field is already clipped:
 * src+w never leaves the tile, dst+w never leaves the viewport, and tileX/tileY are valid
 * indices at this zoom. A caller may blit these without re-checking anything. */
typedef struct {
  int tileX, tileY;      // which tile file
  int srcX, srcY;        // top-left pixel INSIDE that tile
  int dstX, dstY;        // top-left pixel inside the viewport
  int w, h;              // size of the piece
} MapBlit;

// ---------------------------------------------------------------- world geometry

// World width/height in pixels at this zoom (256 << z). Clamps z into range first.
int32_t mapWorldPx(int z);

// Clamp a latitude to what Mercator can represent.
double  mapClampLat(double lat);

/* Wrap a longitude into [-180, 180). ⚠ Not a clamp: 181 becomes -179, because a map that
 * clamps longitude silently teleports a pin 2 degrees west rather than round the back. */
double  mapWrapLon(double lon);

// Degrees <-> the 1e-7 fixed point the Meshtastic wire format (and this firmware) uses.
double  mapI7ToDeg(int32_t i7);
int32_t mapDegToI7(double deg);

// lat/lon -> world pixels (doubles, so a pin lands between pixels honestly).
void    mapLatLonToWorld(double lat, double lon, int z, double* wx, double* wy);
// world pixels -> lat/lon. wx is wrapped, wy is clamped to the world.
void    mapWorldToLatLon(double wx, double wy, int z, double* lat, double* lon);

/* Metres per screen pixel at this latitude and zoom. Used for the scale bar and for
 * "how far is that" readouts. */
double  mapMetersPerPixel(double lat, int z);

// ---------------------------------------------------------------- the view

/* Put a view centre back inside the world: y clamped so the viewport cannot show off the
 * top or bottom edge, x wrapped. When the whole world is NARROWER than the viewport (very
 * low zoom) the centre is parked at the middle and the caller letterboxes. */
void    mapClampView(int z, int vw, int vh, int32_t* cx, int32_t* cy);

/* Pan by whole pixels, then clamp/wrap. Returns true if the centre actually moved — false
 * means the view was already against the top or bottom stop, which is what the UI needs in
 * order to say so instead of repainting an identical screen. */
int     mapPanView(int z, int vw, int vh, int dx, int dy, int32_t* cx, int32_t* cy);

/* Change zoom by `delta` levels, keeping the centre over the same ground. Returns the new
 * zoom (unchanged if the step would leave [zMin, zMax]); cx and cy are rewritten only when the
 * zoom really changed. zMin/zMax are the levels that exist on the card, NOT the constants —
 * offering a zoom with no tiles behind it is offering a grey screen. */
int     mapZoomView(int zMin, int zMax, int z, int delta, int vw, int vh,
                    int32_t* cx, int32_t* cy);

/* Fill `out` with the blits that cover the viewport, top-to-bottom then left-to-right.
 * Returns the count, or -1 if `cap` was too small (in which case nothing is written —
 * a partially-drawn map is a lie, and a caller that ignored a truncated count would draw one).
 *
 * A 240x250 viewport over 256 px tiles needs at most 2x2 = 4 blits; MAP_MAX_BLITS is 12,
 * which covers a viewport up to 768 px across should the screen ever change. */
#define MAP_MAX_BLITS   12
int     mapViewBlits(int z, int32_t cx, int32_t cy, int vw, int vh,
                     MapBlit* out, int cap);

/* Project a lat/lon into viewport coordinates. Always writes through vx and vy (a caller drawing an
 * off-screen arrow needs them) and returns 1 only when the point is inside the viewport.
 * ⚠ The x is chosen as the representation NEAREST the view centre, so a pin just the other
 * side of the antimeridian draws beside you rather than a world away. */
int     mapLatLonToView(int z, int32_t cx, int32_t cy, int vw, int vh,
                        double lat, double lon, int* vx, int* vy);

// The ground under a viewport pixel. The crosshair is (vw/2, vh/2).
void    mapViewToLatLon(int z, int32_t cx, int32_t cy, int vw, int vh,
                        int px, int py, double* lat, double* lon);

/* How far one press of the d-pad moves the map. `run` is how many presses in this direction
 * have arrived in quick succession (0 for the first).
 *
 * Fixed steps are the wrong answer in both directions at once: small enough to place a
 * crosshair precisely is far too slow to cross a valley, and large enough to cross a valley
 * overshoots every time you are trying to sit on a creek bend. So the first press is fine and
 * a held-down key accelerates — and it RESETS the moment the presses stop or change
 * direction, so the next deliberate nudge is a nudge again. */
int     mapPanStep(int run);

/* Choose a scale bar: the largest of 1/2/5 x 10^n metres that fits in `maxPx` pixels.
 * Writes the bar's length in pixels to *px and returns its length in metres. */
int     mapScaleBar(double metersPerPixel, int maxPx, int* px);

/* Build the path of one tile file: "<root>/<area>/<z>/<x>/<y>.565".
 * Returns the length written, or 0 if it would not fit (and writes an empty string).
 * ⚠ Returning 0 rather than truncating is the whole point: a truncated path is a path to a
 * DIFFERENT, possibly existing file, and the map would show the wrong ground without an error
 * anywhere. */
int     mapTilePath(char* out, size_t cap, const char* root, const char* area,
                    int z, int x, int y);

/* Is this a plausible area name for a folder under /maps? Letters, digits, '-', '_', '.',
 * 1..31 chars, and never starting with '.' — which also keeps macOS's "._" sidecar files and
 * ".Spotlight-V100" out of the area list (the Game Boy picker learned that one in 0.9.57). */
int     mapAreaNameOk(const char* name);

#endif // MAP_TILES_H
