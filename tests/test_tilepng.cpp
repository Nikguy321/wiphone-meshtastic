/*
 * test_tilepng.cpp — the PNG decoder a map tile goes through on the phone, proven on the host.
 *
 * The fixtures are built HERE, in memory, with zlib: for every colour type and bit depth the
 * decoder claims, a 256x256 image with a known pattern is filtered (a different filter type
 * on every row, so all five are exercised), deflated, wrapped in PNG chunks with real CRCs,
 * decoded by tilePngDecode(), and compared pixel-for-pixel against the same pattern packed
 * by tileColor565(). No Pillow, no fixture files, nothing to regenerate.
 *
 * The refusals are tested too — the half that matters on a card in the woods: a 512 px tile,
 * an interlaced one, 16-bit samples, a truncated stream, a bad filter byte, an HTML body.
 */
#include "../WiPhone/tile_png.h"
#include "../WiPhone/tile_decode.h"

#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vector>

static int g_pass = 0, g_fail = 0;
static void ok(bool c, const char* what) {
  if (c) {
    g_pass++;
  } else {
    g_fail++;
    printf("  FAIL  %s\n", what);
  }
}
static void group(const char* name) {
  printf("%s\n", name);
}

// ---- a tiny PNG writer ---------------------------------------------------------------------
static void put32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back((uint8_t)(x >> 24));
  v.push_back((uint8_t)(x >> 16));
  v.push_back((uint8_t)(x >> 8));
  v.push_back((uint8_t)x);
}
static void chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& body) {
  put32(png, (uint32_t)body.size());
  std::vector<uint8_t> tb;
  tb.insert(tb.end(), type, type + 4);
  tb.insert(tb.end(), body.begin(), body.end());
  png.insert(png.end(), tb.begin(), tb.end());
  put32(png, (uint32_t)crc32(0, tb.data(), (uInt)tb.size()));
}

struct Img {
  int ctype, depth;
  std::vector<uint8_t> raw;        // unfiltered rows, rowBytes each
  std::vector<uint8_t> plte;       // RGB triples
  std::vector<uint8_t> trns;
  std::vector<uint16_t> expect;    // 256*256
  size_t rowBytes;
  int    bpp;
};

/* Filter `raw` with filter type (y % 5) per row, exactly per the spec, then deflate. */
static std::vector<uint8_t> filterAndDeflate(const Img& im, int forceFilter, int level) {
  std::vector<uint8_t> filt;
  std::vector<uint8_t> prev(im.rowBytes, 0);
  for (int y = 0; y < 256; y++) {
    const uint8_t* row = im.raw.data() + (size_t)y * im.rowBytes;
    const int t = forceFilter >= 0 ? forceFilter : (y % 5);
    filt.push_back((uint8_t)t);
    for (size_t i = 0; i < im.rowBytes; i++) {
      const int a = i >= (size_t)im.bpp ? row[i - im.bpp] : 0;
      const int b = prev[i];
      const int c = i >= (size_t)im.bpp ? prev[i - im.bpp] : 0;
      int pred = 0;
      switch (t) {
      case 0: pred = 0; break;
      case 1: pred = a; break;
      case 2: pred = b; break;
      case 3: pred = (a + b) / 2; break;
      case 4: {
        const int p = a + b - c;
        const int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
        pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
        break;
      }
      }
      filt.push_back((uint8_t)(row[i] - pred));
    }
    memcpy(prev.data(), row, im.rowBytes);
  }
  uLongf cap = compressBound((uLong)filt.size());
  std::vector<uint8_t> z(cap);
  if (compress2(z.data(), &cap, filt.data(), (uLong)filt.size(), level) != Z_OK) {
    return std::vector<uint8_t>();
  }
  z.resize(cap);
  return z;
}

static std::vector<uint8_t> makePng(const Img& im, const std::vector<uint8_t>& z, int w, int h,
                                    int interlace, int depthOverride, size_t idatSplit) {
  std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
  std::vector<uint8_t> ihdr;
  put32(ihdr, (uint32_t)w);
  put32(ihdr, (uint32_t)h);
  ihdr.push_back((uint8_t)(depthOverride ? depthOverride : im.depth));
  ihdr.push_back((uint8_t)im.ctype);
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back((uint8_t)interlace);
  chunk(png, "IHDR", ihdr);
  if (!im.plte.empty()) {
    chunk(png, "PLTE", im.plte);
  }
  if (!im.trns.empty()) {
    chunk(png, "tRNS", im.trns);
  }
  // IDAT, split into pieces so the concatenation path is exercised.
  size_t off = 0;
  while (off < z.size()) {
    size_t n = idatSplit ? idatSplit : z.size();
    if (n > z.size() - off) n = z.size() - off;
    chunk(png, "IDAT", std::vector<uint8_t>(z.begin() + off, z.begin() + off + n));
    off += n;
  }
  chunk(png, "IEND", std::vector<uint8_t>());
  return png;
}

// ---- patterns ------------------------------------------------------------------------------
static void rgbAt(int x, int y, uint8_t* r, uint8_t* g, uint8_t* b) {
  *r = (uint8_t)(x);
  *g = (uint8_t)(y);
  *b = (uint8_t)((x * 3 + y * 7) & 0xFF);
}

static Img makeImg(int ctype, int depth) {
  Img im;
  im.ctype = ctype;
  im.depth = depth;
  im.expect.assign(256 * 256, 0);
  const int channels = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : 4;
  im.rowBytes = ((size_t)channels * depth * 256 + 7) / 8;
  im.bpp = (channels * depth) < 8 ? 1 : (channels * depth) / 8;
  im.raw.assign(im.rowBytes * 256, 0);
  if (ctype == 3) {
    const int n = 1 << depth;
    for (int i = 0; i < n; i++) {
      im.plte.push_back((uint8_t)(i * 255 / (n - 1)));
      im.plte.push_back((uint8_t)(255 - i * 255 / (n - 1)));
      im.plte.push_back((uint8_t)((i * 37) & 0xFF));
    }
    // index 1 is fully transparent via tRNS (only when there is more than one entry)
    if (n > 1) {
      im.trns.push_back(255);
      im.trns.push_back(0);
    }
  }
  for (int y = 0; y < 256; y++) {
    uint8_t* row = im.raw.data() + (size_t)y * im.rowBytes;
    for (int x = 0; x < 256; x++) {
      uint8_t r, g, b;
      rgbAt(x, y, &r, &g, &b);
      switch (ctype) {
      case 2:
        row[x * 3] = r; row[x * 3 + 1] = g; row[x * 3 + 2] = b;
        im.expect[y * 256 + x] = tileColor565(r, g, b);
        break;
      case 6: {
        const uint8_t a = (uint8_t)((x + y) & 1 ? 255 : 0);
        row[x * 4] = r; row[x * 4 + 1] = g; row[x * 4 + 2] = b; row[x * 4 + 3] = a;
        im.expect[y * 256 + x] = a >= 128 ? tileColor565(r, g, b) : TILE_PNG_NODATA_565;
        break;
      }
      case 4: {
        const uint8_t a = (uint8_t)(x < 128 ? 200 : 20);
        row[x * 2] = r; row[x * 2 + 1] = a;
        im.expect[y * 256 + x] = a >= 128 ? tileColor565(r, r, r) : TILE_PNG_NODATA_565;
        break;
      }
      case 0:
      case 3: {
        const int n = 1 << depth;
        const int v = (x + y) % n;
        const int perByte = 8 / depth;
        const int shift = 8 - depth * (x % perByte + 1);
        row[x / perByte] |= (uint8_t)(v << shift);
        if (ctype == 3) {
          const bool transparent = (n > 1 && v == 1);
          im.expect[y * 256 + x] = transparent ? TILE_PNG_NODATA_565
                                   : tileColor565(im.plte[v * 3], im.plte[v * 3 + 1], im.plte[v * 3 + 2]);
        } else {
          uint8_t gv;
          switch (depth) {
          case 1: gv = v ? 255 : 0; break;
          case 2: gv = (uint8_t)(v * 85); break;
          case 4: gv = (uint8_t)(v * 17); break;
          default: gv = (uint8_t)v; break;
          }
          im.expect[y * 256 + x] = tileColor565(gv, gv, gv);
        }
        break;
      }
      }
    }
  }
  return im;
}

static bool decodeMatches(const std::vector<uint8_t>& png, const Img& im, char* why, size_t cap) {
  std::vector<uint16_t> out(256 * 256, 0xFFFF);
  if (!tilePngDecode(png.data(), png.size(), out.data(), TILE_PNG_NODATA_565, why, cap)) {
    return false;
  }
  for (size_t i = 0; i < out.size(); i++) {
    if (out[i] != im.expect[i]) {
      snprintf(why, cap, "pixel %zu: got %04x want %04x", i, out[i], im.expect[i]);
      return false;
    }
  }
  return true;
}

int main() {
  group("every supported colour type and depth round-trips pixel for pixel");
  const struct { int ctype, depth; const char* name; } CASES[] = {
    { 3, 8, "palette 8-bit (OpenTopoMap)" }, { 3, 4, "palette 4-bit" },
    { 3, 2, "palette 2-bit" }, { 3, 1, "palette 1-bit" },
    { 2, 8, "RGB 8-bit" }, { 6, 8, "RGBA 8-bit (USGS MIXED edge)" },
    { 0, 8, "grey 8-bit" }, { 0, 4, "grey 4-bit" }, { 0, 2, "grey 2-bit" }, { 0, 1, "grey 1-bit" },
    { 4, 8, "grey+alpha 8-bit" },
  };
  for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
    Img im = makeImg(CASES[i].ctype, CASES[i].depth);
    std::vector<uint8_t> z = filterAndDeflate(im, -1, 6);
    ok(!z.empty(), "deflate");
    std::vector<uint8_t> png = makePng(im, z, 256, 256, 0, 0, 3000);
    char why[96];
    const bool m = decodeMatches(png, im, why, sizeof(why));
    char what[160];
    snprintf(what, sizeof(what), "%s decodes exactly%s%s", CASES[i].name, m ? "" : " - ", m ? "" : why);
    ok(m, what);
  }

  group("each filter type on its own, and a stored (level 0) stream");
  for (int f = 0; f < 5; f++) {
    Img im = makeImg(2, 8);
    std::vector<uint8_t> z = filterAndDeflate(im, f, f == 4 ? 0 : 9);
    std::vector<uint8_t> png = makePng(im, z, 256, 256, 0, 0, 0);
    char why[96];
    char what[64];
    snprintf(what, sizeof(what), "filter %d decodes exactly", f);
    ok(decodeMatches(png, im, why, sizeof(why)), what);
  }

  group("what is refused, and that it says why");
  {
    Img im = makeImg(2, 8);
    std::vector<uint8_t> z = filterAndDeflate(im, -1, 6);
    char why[96];
    std::vector<uint16_t> out(256 * 256);

    std::vector<uint8_t> big = makePng(im, z, 512, 512, 0, 0, 0);
    ok(!tilePngDecode(big.data(), big.size(), out.data(), 0, why, sizeof(why)) && strstr(why, "512x512"),
       "a 512 px tile is refused and named");
    std::vector<uint8_t> il = makePng(im, z, 256, 256, 1, 0, 0);
    ok(!tilePngDecode(il.data(), il.size(), out.data(), 0, why, sizeof(why)) && strstr(why, "interlaced"),
       "an interlaced PNG is refused");
    std::vector<uint8_t> d16 = makePng(im, z, 256, 256, 0, 16, 0);
    ok(!tilePngDecode(d16.data(), d16.size(), out.data(), 0, why, sizeof(why)) && strstr(why, "16-bit"),
       "16-bit samples are refused");
    std::vector<uint8_t> cut = makePng(im, std::vector<uint8_t>(z.begin(), z.begin() + z.size() / 2),
                                       256, 256, 0, 0, 0);
    ok(!tilePngDecode(cut.data(), cut.size(), out.data(), 0, why, sizeof(why)),
       "a truncated stream is refused");
    std::vector<uint8_t> whole = makePng(im, z, 256, 256, 0, 0, 0);
    ok(!tilePngDecode(whole.data(), whole.size() - 40, out.data(), 0, why, sizeof(why)),
       "a body cut mid-chunk is refused");
    // A bad filter byte: re-filter with type 7 on the first row.
    std::vector<uint8_t> filt;
    filt.push_back(7);
    filt.insert(filt.end(), im.raw.begin(), im.raw.begin() + im.rowBytes);
    for (int y = 1; y < 256; y++) {
      filt.push_back(0);
      filt.insert(filt.end(), im.raw.begin() + (size_t)y * im.rowBytes, im.raw.begin() + (size_t)(y + 1) * im.rowBytes);
    }
    uLongf cap = compressBound((uLong)filt.size());
    std::vector<uint8_t> zz(cap);
    compress2(zz.data(), &cap, filt.data(), (uLong)filt.size(), 6);
    zz.resize(cap);
    std::vector<uint8_t> badf = makePng(im, zz, 256, 256, 0, 0, 0);
    ok(!tilePngDecode(badf.data(), badf.size(), out.data(), 0, why, sizeof(why)) && strstr(why, "filter"),
       "a bad filter byte is refused and named");
    const char* html = "<html><body>404</body></html>";
    ok(!tilePngDecode((const uint8_t*)html, strlen(html), out.data(), 0, why, sizeof(why)),
       "an HTML body is not a PNG");
  }

  group("tRNS on grey and RGB images names one value as transparent");
  {
    // grey 8-bit: sample 0x40 transparent
    Img im = makeImg(0, 8);
    im.trns = { 0x00, 0x40 };
    for (int y = 0; y < 256; y++) {
      for (int x = 0; x < 256; x++) {
        const int v = (x + y) % 256;
        if (v == 0x40) im.expect[y * 256 + x] = TILE_PNG_NODATA_565;
      }
    }
    std::vector<uint8_t> z = filterAndDeflate(im, -1, 6);
    std::vector<uint8_t> png = makePng(im, z, 256, 256, 0, 0, 0);
    char why[96];
    ok(decodeMatches(png, im, why, sizeof(why)), "grey tRNS pixels become nodata");
    // RGB: the colour at (x=10,y=20) transparent wherever it occurs
    Img rgb = makeImg(2, 8);
    uint8_t tr, tg, tb;
    rgbAt(10, 20, &tr, &tg, &tb);
    rgb.trns = { 0, tr, 0, tg, 0, tb };
    for (int y = 0; y < 256; y++) {
      for (int x = 0; x < 256; x++) {
        uint8_t r, g, b;
        rgbAt(x, y, &r, &g, &b);
        if (r == tr && g == tg && b == tb) rgb.expect[y * 256 + x] = TILE_PNG_NODATA_565;
      }
    }
    std::vector<uint8_t> z2 = filterAndDeflate(rgb, -1, 6);
    std::vector<uint8_t> png2 = makePng(rgb, z2, 256, 256, 0, 0, 0);
    ok(decodeMatches(png2, rgb, why, sizeof(why)), "RGB tRNS colour becomes nodata");
  }

  group("tileSniff and tileDecode's dispatch on the host");
  {
    const uint8_t jpg[8] = { 0xFF, 0xD8, 0xFF, 0xE0, 0, 0x10, 'J', 'F' };
    ok(tileSniff(jpg, sizeof(jpg)) == TILE_FMT_JPEG, "JPEG magic");
    const uint8_t png[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    ok(tileSniff(png, sizeof(png)) == TILE_FMT_PNG, "PNG magic");
    const char* html = "\n  <!DOCTYPE html>";
    ok(tileSniff((const uint8_t*)html, strlen(html)) == TILE_FMT_HTML, "a page, after whitespace");
    const uint8_t junk[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    ok(tileSniff(junk, sizeof(junk)) == TILE_FMT_UNKNOWN, "junk is unknown");
    ok(tileSniff(png, 4) == TILE_FMT_UNKNOWN, "too short to say");
    Img im = makeImg(3, 8);
    std::vector<uint8_t> z = filterAndDeflate(im, -1, 6);
    std::vector<uint8_t> p = makePng(im, z, 256, 256, 0, 0, 0);
    std::vector<uint16_t> out(256 * 256);
    char why[96];
    ok(tileDecode(p.data(), p.size(), out.data(), why, sizeof(why)) && out[0] == im.expect[0],
       "tileDecode routes a PNG to the PNG decoder");
    ok(!tileDecode((const uint8_t*)html, strlen(html), out.data(), why, sizeof(why)) && strstr(why, "page"),
       "tileDecode names an HTML body for what it is");
  }

  printf("\n%s%d passed, %d failed%s\n", g_fail ? "\033[31m" : "\033[32m", g_pass, g_fail, "\033[0m");
  return g_fail ? 1 : 0;
}
