/*
 * tile_decode.h — a downloaded tile (JPEG or PNG bytes) into the 256x256 RGB565 the viewer
 * reads (map_tiles.h: 131072 bytes, little-endian, what color565() produces).
 *
 * ── WHY THE PHONE DECODES AT DOWNLOAD TIME, NOT AT DRAW TIME ────────────────────────────
 * app_maps.h explains why the card holds raw tiles: zero decode on the draw path, one
 * checkable failure mode, and the same file whether it came from the Mac converter or from
 * here. So a tile is decoded ONCE, on the fetch task, straight after it arrives, and written
 * as the raw file. The viewer never learns where a tile came from.
 *
 * ── WHAT DECODES WHAT ───────────────────────────────────────────────────────────────────
 *   JPEG  the ESP32's ROM TJpgDec (rom/tjpgd.h, the decoder Photos and the wallpaper already
 *         use) driven with PRIVATE callbacks through the JDEC's `device` pointer — never
 *         display::load_jpg_at, whose globals and internal-RAM pool a wallpaper reload on
 *         the loop task would race. Baseline colour only: JDR_FMT3 for greyscale or
 *         progressive is REPORTED, not worked around (jpeg_grey.h exists for books, and a
 *         map source that hands out greyscale is telling you something about its coverage).
 *   PNG   tile_png.h — palette / grey / RGB / RGBA, 8-bit and sub-byte palette, no interlace.
 *
 * ── WHAT IS REFUSED, LOUDLY ─────────────────────────────────────────────────────────────
 *   - anything that is not exactly 256x256 (a 512 px "retina" tile would overflow the buffer
 *     through the output callback, which has no bounds of its own);
 *   - a body whose first bytes are neither JPEG nor PNG (an HTML 404 wearing a tile URL);
 *   - a decoder error of any kind. `why` says which, in words a serial log can carry.
 * Every buffer is the caller's: the compressed bytes, the 128 KB output, and (on the
 * device) the decoder's own work pool are all PSRAM, so a decode costs internal RAM nothing
 * but the stack of the task that calls it.
 */
#ifndef TILE_DECODE_H
#define TILE_DECODE_H

#include <stdint.h>
#include <stddef.h>

#define TILE_DECODE_PX      256
#define TILE_DECODE_WORDS   (TILE_DECODE_PX * TILE_DECODE_PX)

typedef enum { TILE_FMT_UNKNOWN = 0, TILE_FMT_JPEG, TILE_FMT_PNG, TILE_FMT_HTML } TileFormat;

/* What the first bytes say the body is. TILE_FMT_HTML is what a server's error page looks
 * like ('<' after optional whitespace) — reported separately so the log can say "the server
 * answered with a page, not a tile" rather than "corrupt". */
TileFormat tileSniff(const uint8_t* data, size_t len);

/* Decode `data` into out[TILE_DECODE_WORDS] (native uint16_t color565 words, row-major,
 * top-down). True on success; false with `why` filled. `out` is untouched on failure only
 * as far as the decoder got — never rely on it after a false. */
bool tileDecode(const uint8_t* data, size_t len, uint16_t* out, char* why, size_t whyCap);

/* The packing, spelled once: TFT_eSPI::color565 without a display object. */
static inline uint16_t tileColor565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

#endif // TILE_DECODE_H
