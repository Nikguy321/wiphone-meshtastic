/*
 * epub_parse.cpp — see epub_parse.h.
 *
 * A port of COVEY's covey_ui/epub.py. Where this looks fussy it is matching that file,
 * because the ids it produces are what decide whether a sync packet is about the same book.
 *
 * EPUB_ENTITY_NOTE: COVEY parses XHTML with Python's HTMLParser(convert_charrefs=True), so
 * this decodes the SAME entity set — html_entities.h is generated from the very table Python
 * uses — plus numeric references, and it collapses the same Unicode whitespace set that
 * Python's str.split() does.
 *
 * That is not perfectionism. A reading position is a CHARACTER OFFSET into the extracted
 * text, so any entity decoded to a different length than COVEY produces shifts every offset
 * after it and keeps growing to the end of the chapter — a drift far too small to trip the
 * receiver's "offset past the end of the chapter" check, and therefore silent. A short
 * hand-picked entity list was the first attempt here and &nbsp; alone was enough to break it.
 * Only the ';'-terminated forms are handled; the legacy no-semicolon ones (&amp vs &amp;) are
 * invalid in XHTML.
 */
#include "epub_parse.h"
#include "book_hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ARDUINO)
  #include <Arduino.h>
  #include "rom/miniz.h"
  static void* ebAlloc(size_t n) {
    void* p = ps_malloc(n);                 // books belong in PSRAM; internal RAM is precious
    return p ? p : malloc(n);
  }
#else
  #include <zlib.h>
  static void* ebAlloc(size_t n) { return malloc(n); }
#endif

static void ebFree(void* p) { free(p); }

// ---------------------------------------------------------------- source helpers
static size_t srcRead(EpubSource* s, uint64_t off, void* buf, size_t len) {
  if (off >= s->size) {
    return 0;
  }
  if (off + len > s->size) {
    len = (size_t)(s->size - off);
  }
  return s->read(s->ctx, off, buf, len);
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---------------------------------------------------------------- inflate
static int rawInflate(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstCap) {
#if defined(ARDUINO)
  // The ROM build of miniz has no allocator, so tinfl_decompress() is used directly and the
  // decompressor state (~11 KB) is heap-allocated rather than put on a task stack.
  tinfl_decompressor* d = (tinfl_decompressor*)ebAlloc(sizeof(tinfl_decompressor));
  if (!d) {
    return -1;
  }
  tinfl_init(d);
  size_t inLen = srcLen, outLen = dstCap;
  tinfl_status st = tinfl_decompress(d, src, &inLen, dst, dst, &outLen,
                                     TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  ebFree(d);
  return (st == TINFL_STATUS_DONE) ? (int)outLen : -1;
#else
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {     // negative window = raw deflate, no header
    return -1;
  }
  zs.next_in = (Bytef*)src;
  zs.avail_in = (uInt)srcLen;
  zs.next_out = (Bytef*)dst;
  zs.avail_out = (uInt)dstCap;
  int r = inflate(&zs, Z_FINISH);
  size_t got = zs.total_out;
  inflateEnd(&zs);
  return (r == Z_STREAM_END) ? (int)got : -1;
#endif
}

// ---------------------------------------------------------------- zip
struct ZipEntry {
  uint64_t localOff;
  uint32_t compSize;
  uint32_t uncompSize;
  uint16_t method;
  char     name[EPUB_NAME_MAX];
};

// Offset of the central directory, or 0 if this is not a zip we can read.
static bool zipFindCentral(EpubSource* s, uint64_t* cdOff, uint32_t* nEntries) {
  // The End Of Central Directory record is last, but a zip comment can follow it, so scan
  // back over the largest comment a zip can carry (64 KB) plus the record itself.
  const size_t MAXSCAN = 66000;
  size_t scan = (s->size < MAXSCAN) ? (size_t)s->size : MAXSCAN;
  if (scan < 22) {
    return false;
  }
  uint8_t* buf = (uint8_t*)ebAlloc(scan);
  if (!buf) {
    return false;
  }
  uint64_t base = s->size - scan;
  size_t got = srcRead(s, base, buf, scan);
  bool found = false;
  if (got >= 22) {
    for (size_t i = got - 22 + 1; i-- > 0; ) {
      if (buf[i] == 0x50 && buf[i + 1] == 0x4b && buf[i + 2] == 0x05 && buf[i + 3] == 0x06) {
        *nEntries = rd16(buf + i + 10);
        *cdOff = rd32(buf + i + 16);
        found = true;
        break;
      }
    }
  }
  ebFree(buf);
  return found;
}

typedef bool (*ZipVisit)(const ZipEntry* e, void* user);   // return false to stop

static bool zipForEach(EpubSource* s, ZipVisit cb, void* user) {
  uint64_t cdOff = 0;
  uint32_t n = 0;
  if (!zipFindCentral(s, &cdOff, &n)) {
    return false;
  }
  uint8_t hdr[46];
  uint64_t at = cdOff;
  for (uint32_t i = 0; i < n; i++) {
    if (srcRead(s, at, hdr, 46) != 46) {
      return false;
    }
    if (rd32(hdr) != 0x02014b50) {
      return false;
    }
    ZipEntry e;
    e.method = rd16(hdr + 10);
    e.compSize = rd32(hdr + 20);
    e.uncompSize = rd32(hdr + 24);
    uint16_t nameLen = rd16(hdr + 28);
    uint16_t extraLen = rd16(hdr + 30);
    uint16_t cmtLen = rd16(hdr + 32);
    e.localOff = rd32(hdr + 42);
    size_t want = nameLen < EPUB_NAME_MAX - 1 ? nameLen : EPUB_NAME_MAX - 1;
    if (srcRead(s, at + 46, e.name, want) != want) {
      return false;
    }
    e.name[want] = '\0';
    at += 46 + nameLen + extraLen + cmtLen;
    if (nameLen < EPUB_NAME_MAX) {          // silently skip absurdly long paths
      if (!cb(&e, user)) {
        return true;
      }
    }
  }
  return true;
}

struct FindCtx { const char* want; ZipEntry* out; bool hit; };

static bool findVisit(const ZipEntry* e, void* user) {
  FindCtx* c = (FindCtx*)user;
  if (strcmp(e->name, c->want) == 0) {
    *c->out = *e;
    c->hit = true;
    return false;
  }
  return true;
}

static bool zipFind(EpubSource* s, const char* name, ZipEntry* out) {
  FindCtx c = { name, out, false };
  zipForEach(s, findVisit, &c);
  return c.hit;
}

/* Decompress an entry into `dst`. Returns the size, or -1.
 *
 * The local header's name and extra lengths are read fresh rather than reused from the
 * central directory: the extra field legitimately differs between the two, and using the
 * central one puts the read a few bytes off the start of the data. */
static int zipRead(EpubSource* s, const ZipEntry* e, uint8_t* dst, size_t dstCap) {
  uint8_t lh[30];
  if (srcRead(s, e->localOff, lh, 30) != 30 || rd32(lh) != 0x04034b50) {
    return -1;
  }
  uint64_t dataOff = e->localOff + 30 + rd16(lh + 26) + rd16(lh + 28);
  if (e->method == 0) {                              // stored
    if (e->uncompSize > dstCap) {
      return -1;
    }
    size_t got = srcRead(s, dataOff, dst, e->uncompSize);
    return got == e->uncompSize ? (int)got : -1;
  }
  if (e->method != 8) {                              // only deflate is used by real EPUBs
    return -1;
  }
  uint8_t* comp = (uint8_t*)ebAlloc(e->compSize ? e->compSize : 1);
  if (!comp) {
    return -1;
  }
  int out = -1;
  if (srcRead(s, dataOff, comp, e->compSize) == e->compSize) {
    out = rawInflate(comp, e->compSize, dst, dstCap);
  }
  ebFree(comp);
  return out;
}

// ---------------------------------------------------------------- paths
size_t epubNormPath(const char* base, const char* href, char* out, size_t cap) {
  // Drop a fragment first, exactly as COVEY does.
  char h[EPUB_NAME_MAX];
  size_t hl = 0;
  for (const char* p = href; *p && *p != '#' && hl < sizeof(h) - 1; p++) {
    h[hl++] = *p;
  }
  h[hl] = '\0';

  if (!base || !base[0]) {
    // ⚠ COVEY returns the href UNCHANGED when there is no base — it does not normalise.
    // Matching that matters: a name that came out normalised here would not be found in
    // the zip's namelist, and the spine would silently come out empty.
    size_t n = hl < cap - 1 ? hl : cap - 1;
    memcpy(out, h, n);
    out[n] = '\0';
    return n;
  }

  char joined[EPUB_NAME_MAX * 2];
  snprintf(joined, sizeof(joined), "%s/%s", base, h);
  for (char* p = joined; *p; p++) {
    if (*p == '\\') {
      *p = '/';
    }
  }
  // normpath: collapse "//", drop ".", resolve "..".
  const char* seg[64];
  size_t segLen[64];
  int nSeg = 0;
  char* p = joined;
  while (*p) {
    while (*p == '/') {
      p++;
    }
    if (!*p) {
      break;
    }
    char* s = p;
    while (*p && *p != '/') {
      p++;
    }
    size_t len = (size_t)(p - s);
    if (len == 1 && s[0] == '.') {
      continue;
    }
    if (len == 2 && s[0] == '.' && s[1] == '.') {
      if (nSeg > 0) {
        nSeg--;
      }
      continue;
    }
    if (nSeg < 64) {
      seg[nSeg] = s;
      segLen[nSeg] = len;
      nSeg++;
    }
  }
  size_t n = 0;
  for (int i = 0; i < nSeg; i++) {
    if (i && n < cap - 1) {
      out[n++] = '/';
    }
    size_t take = segLen[i];
    if (n + take > cap - 1) {
      take = cap - 1 - n;
    }
    memcpy(out + n, seg[i], take);
    n += take;
  }
  out[n] = '\0';
  return n;
}

// ---------------------------------------------------------------- slug + fingerprint
size_t epubSlug(const char* in, char* out, size_t cap) {
  /* re.sub(r"[^a-z0-9]+", "-", s.strip().lower()).strip("-")[:64]
   *
   * Only ASCII is lowercased here. That is not a shortcut: the character class replaces
   * every non-[a-z0-9] character with '-' anyway, so a non-ASCII character becomes '-'
   * whether or not it was lowercased first. (The exceptions are the handful of characters
   * that lowercase INTO ASCII, such as U+212A KELVIN SIGN; those would differ from COVEY.
   * Vanishingly rare in book metadata, and the fp: id still matches for identical files.) */
  size_t n = 0;
  bool pendingDash = false;
  bool any = false;
  for (const char* p = in ? in : ""; *p; p++) {
    unsigned char c = (unsigned char)*p;
    char lower = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    bool keep = (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9');
    if (keep) {
      if (pendingDash && any && n < cap - 1 && n < 64) {
        out[n++] = '-';
      }
      pendingDash = false;
      if (n < cap - 1 && n < 64) {
        out[n++] = lower;
        any = true;
      }
    } else {
      pendingDash = true;                      // a RUN of them collapses to one '-'
    }
  }
  out[n] = '\0';
  return n;
}

bool epubFingerprint(EpubSource* src, char out[17]) {
  /* sha1(str(size) + first 64 KB + last 64 KB), first 16 hex characters.
   * COVEY hashes the ends plus the length rather than the whole file: a full hash of a
   * 400 MB book on an SD card is not something to do every time a library screen opens,
   * and books are not adversarial input for this purpose. */
  out[0] = '\0';
  BsSha1 c;
  bsSha1Init(&c);
  char sizeStr[32];
  snprintf(sizeStr, sizeof(sizeStr), "%llu", (unsigned long long)src->size);
  bsSha1Update(&c, sizeStr, strlen(sizeStr));

  uint8_t* buf = (uint8_t*)ebAlloc(65536);
  if (!buf) {
    return false;
  }
  size_t got = srcRead(src, 0, buf, 65536);
  bsSha1Update(&c, buf, got);
  if (src->size > 131072) {
    size_t g2 = srcRead(src, src->size - 65536, buf, 65536);
    bsSha1Update(&c, buf, g2);
  }
  ebFree(buf);

  uint8_t d[20];
  bsSha1Final(&c, d);
  static const char* kHexDigits = "0123456789abcdef";  // not HEX: Arduino's Print.h defines that as 16
  for (int i = 0; i < 8; i++) {
    out[i * 2] = kHexDigits[d[i] >> 4];
    out[i * 2 + 1] = kHexDigits[d[i] & 0xF];
  }
  out[16] = '\0';
  return true;
}

// ---------------------------------------------------------------- entities + XML scan
/* The named-entity table is GENERATED from the same html.entities.html5 that COVEY's
 * HTMLParser decodes with (tools/gen_entities.py). It is not a hand-picked "common" list on
 * purpose: a reading position is a character offset into the extracted text, so an entity
 * this failed to decode would leave the WiPhone's text longer than COVEY's from that point
 * to the end of the chapter — a drift that grows rather than staying local, and that is too
 * small to trip the receiver's "offset past the end" sanity check. ~64 KB of flash, which is
 * cheap against the 4.5 MB free. */
#include "html_entities.h"

/* Byte length of the whitespace character at `p`, or 0.
 *
 * Matches Python's str.isspace(), which is what COVEY collapses on in
 * " ".join(text.split()) — and that set is WIDER than ASCII. U+00A0, the character &nbsp;
 * decodes to, is whitespace to Python and would otherwise survive here as a real character,
 * making the WiPhone's text one character longer at every &nbsp; in the chapter. Since a
 * reading position is a character offset, that is a drift, not a cosmetic difference.
 * (U+200B ZERO WIDTH SPACE is deliberately absent: Python does not call it whitespace.) */
static size_t utf8SpaceLen(const char* p, size_t avail) {
  if (avail == 0) {
    return 0;
  }
  unsigned char c = (unsigned char)p[0];
  if (c == ' ' || (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x1F)) {
    return 1;
  }
  if (c < 0x80) {
    return 0;
  }
  uint32_t cp = 0;
  size_t n = 0;
  if ((c & 0xE0) == 0xC0)      { n = 2; cp = c & 0x1Fu; }
  else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0Fu; }
  else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07u; }
  else                         { return 0; }
  if (avail < n) {
    return 0;
  }
  for (size_t k = 1; k < n; k++) {
    if (((unsigned char)p[k] & 0xC0) != 0x80) {
      return 0;
    }
    cp = (cp << 6) | ((unsigned char)p[k] & 0x3Fu);
  }
  if (cp == 0x85 || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
      cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000) {
    return n;
  }
  return 0;
}

static size_t utf8Put(uint32_t cp, char* out) {
  if (cp < 0x80) { out[0] = (char)cp; return 1; }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/* Decode one entity starting at `p` (which points at '&'). Writes UTF-8 into `out` and
 * returns the number of INPUT bytes consumed, or 0 if this is not an entity — in which case
 * the caller passes the '&' through unchanged, as a browser would. */
static size_t decodeEntity(const char* p, size_t avail, char* out, size_t* outLen) {
  if (avail < 3 || p[0] != '&') {
    return 0;
  }
  size_t end = 1;
  while (end < avail && end <= HTML_ENTITY_MAX_NAME && p[end] != ';') {
    end++;
  }
  if (end >= avail || p[end] != ';') {
    return 0;
  }
  if (p[1] == '#') {
    uint32_t cp = 0;
    size_t i = 2;
    if (i < end && (p[i] == 'x' || p[i] == 'X')) {
      i++;
      if (i >= end) {
        return 0;
      }
      for (; i < end; i++) {
        char ch = p[i];
        int v = (ch >= '0' && ch <= '9') ? ch - '0'
              : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
              : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
        if (v < 0) {
          return 0;
        }
        cp = cp * 16 + (uint32_t)v;
      }
    } else {
      if (i >= end) {
        return 0;
      }
      for (; i < end; i++) {
        if (p[i] < '0' || p[i] > '9') {
          return 0;
        }
        cp = cp * 10 + (uint32_t)(p[i] - '0');
      }
    }
    if (cp == 0 || cp > 0x10FFFF) {
      return 0;
    }
    *outLen = utf8Put(cp, out);
    return end + 1;
  }
  // Binary search the generated table; entity names are case-sensitive.
  char name[HTML_ENTITY_MAX_NAME + 1];
  size_t nameLen = end - 1;
  if (nameLen == 0 || nameLen > HTML_ENTITY_MAX_NAME) {
    return 0;
  }
  memcpy(name, p + 1, nameLen);
  name[nameLen] = '\0';
  int lo = 0, hi = HTML_ENTITY_N - 1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    int cmp = strcmp(name, HTML_ENTITIES[mid].name);
    if (cmp == 0) {
      *outLen = strlen(HTML_ENTITIES[mid].utf8);
      memcpy(out, HTML_ENTITIES[mid].utf8, *outLen);
      return end + 1;
    }
    if (cmp < 0) {
      hi = mid - 1;
    } else {
      lo = mid + 1;
    }
  }
  return 0;
}

// Local tag name (after any namespace prefix), lowercased, into `out`.
static void tagLocalName(const char* p, size_t len, char* out, size_t cap) {
  const char* colon = NULL;
  for (size_t i = 0; i < len; i++) {
    if (p[i] == ':') {
      colon = p + i;
    }
  }
  if (colon) {
    len -= (size_t)(colon + 1 - p);
    p = colon + 1;
  }
  size_t n = 0;
  for (size_t i = 0; i < len && n < cap - 1; i++) {
    char c = p[i];
    out[n++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
  out[n] = '\0';
}

/* Value of `name` in a raw attribute run. Namespace prefixes are ignored on the attribute
 * name too, so opf:role and role both match "role". Returns false if absent. */
static bool tagAttr(const char* attrs, size_t len, const char* name, char* out, size_t cap) {
  size_t nameLen = strlen(name);
  size_t i = 0;
  while (i < len) {
    while (i < len && (attrs[i] == ' ' || attrs[i] == '\t' || attrs[i] == '\n' || attrs[i] == '\r')) {
      i++;
    }
    size_t ks = i;
    while (i < len && attrs[i] != '=' && attrs[i] != ' ' && attrs[i] != '\t' &&
           attrs[i] != '\n' && attrs[i] != '\r') {
      i++;
    }
    size_t ke = i;
    while (i < len && (attrs[i] == ' ' || attrs[i] == '\t')) {
      i++;
    }
    if (i >= len || attrs[i] != '=') {
      if (ks == ke) {
        i++;
      }
      continue;
    }
    i++;
    while (i < len && (attrs[i] == ' ' || attrs[i] == '\t')) {
      i++;
    }
    char quote = 0;
    if (i < len && (attrs[i] == '"' || attrs[i] == '\'')) {
      quote = attrs[i++];
    }
    size_t vs = i;
    while (i < len && (quote ? attrs[i] != quote : (attrs[i] != ' ' && attrs[i] != '\t'))) {
      i++;
    }
    size_t ve = i;
    if (quote && i < len) {
      i++;
    }
    // Compare, ignoring any namespace prefix on the attribute name.
    const char* k = attrs + ks;
    size_t kl = ke - ks;
    for (size_t j = 0; j < kl; j++) {
      if (k[j] == ':') {
        k += j + 1;
        kl -= j + 1;
        break;
      }
    }
    if (kl == nameLen && strncmp(k, name, nameLen) == 0) {
      size_t n = 0;
      size_t vi = vs;
      while (vi < ve && n < cap - 1) {
        char ebuf[8];
        size_t elen = 0;
        size_t used = decodeEntity(attrs + vi, ve - vi, ebuf, &elen);
        if (used) {
          for (size_t z = 0; z < elen && n < cap - 1; z++) {
            out[n++] = ebuf[z];
          }
          vi += used;
        } else {
          out[n++] = attrs[vi++];
        }
      }
      out[n] = '\0';
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------- text extraction
static const char* BLOCK_TAGS[] = { "p", "div", "br", "li", "tr", "blockquote", "section",
                                    "article", "h1", "h2", "h3", "h4", "h5", "h6",
                                    "figcaption", "td", "pre" };
static const char* DROP_TAGS[] = { "script", "style", "head", "title", "svg", "nav" };

static bool inList(const char* const* list, size_t n, const char* name) {
  for (size_t i = 0; i < n; i++) {
    if (strcmp(list[i], name) == 0) {
      return true;
    }
  }
  return false;
}

struct TextOut {
  char*  buf;
  size_t cap;
  size_t len;        // committed blocks, joined by "\n\n"
  char*  pend;       // current block being accumulated
  size_t pendCap;
  size_t pendLen;
  bool   blocks;     // has at least one committed block
};

static void toPut(TextOut* t, const char* s, size_t n) {
  for (size_t i = 0; i < n && t->pendLen < t->pendCap - 1; i++) {
    t->pend[t->pendLen++] = s[i];
  }
}

static void toFlush(TextOut* t) {
  /* COVEY: " ".join("".join(buf).split()) — collapse every run of whitespace to one space
   * and trim; an empty result appends nothing at all. */
  t->pend[t->pendLen] = '\0';
  char* p = t->pend;
  size_t start = t->len;
  bool wroteAny = false;
  bool needSep = t->blocks;
  size_t i = 0;
  while (i < t->pendLen) {
    size_t sp;
    while (i < t->pendLen && (sp = utf8SpaceLen(p + i, t->pendLen - i)) != 0) {
      i += sp;
    }
    if (i >= t->pendLen) {
      break;
    }
    if (needSep && t->len + 2 < t->cap) {          // blank line between blocks
      t->buf[t->len++] = '\n';
      t->buf[t->len++] = '\n';
      needSep = false;
    } else if (wroteAny && t->len + 1 < t->cap) {
      t->buf[t->len++] = ' ';
    }
    while (i < t->pendLen && utf8SpaceLen(p + i, t->pendLen - i) == 0) {
      if (t->len < t->cap - 1) {
        t->buf[t->len++] = p[i];
      }
      i++;
    }
    wroteAny = true;
  }
  if (wroteAny) {
    t->blocks = true;
  } else {
    t->len = start;
  }
  t->pendLen = 0;
  t->buf[t->len] = '\0';
}

size_t epubExtractText(const char* html, size_t htmlLen, char* out, size_t cap) {
  if (cap < 4) {
    return 0;
  }
  size_t pendCap = cap;
  char* pend = (char*)ebAlloc(pendCap);
  if (!pend) {
    return 0;
  }
  TextOut t = { out, cap, 0, pend, pendCap, 0, false };
  out[0] = '\0';

  int skip = 0;
  size_t i = 0;
  while (i < htmlLen) {
    if (html[i] != '<') {
      if (!skip) {
        char ebuf[8];
        size_t elen = 0;
        size_t used = decodeEntity(html + i, htmlLen - i, ebuf, &elen);
        if (used) {
          toPut(&t, ebuf, elen);
          i += used;
          continue;
        }
        toPut(&t, html + i, 1);
      }
      i++;
      continue;
    }
    // Comments, PIs, doctype and CDATA carry no reading text.
    if (i + 3 < htmlLen && html[i + 1] == '!' && html[i + 2] == '-' && html[i + 3] == '-') {
      size_t j = i + 4;
      while (j + 2 < htmlLen && !(html[j] == '-' && html[j + 1] == '-' && html[j + 2] == '>')) {
        j++;
      }
      i = (j + 3 < htmlLen) ? j + 3 : htmlLen;
      continue;
    }
    if (i + 1 < htmlLen && (html[i + 1] == '?' || html[i + 1] == '!')) {
      size_t j = i + 1;
      while (j < htmlLen && html[j] != '>') {
        j++;
      }
      i = (j < htmlLen) ? j + 1 : htmlLen;
      continue;
    }
    bool closing = (i + 1 < htmlLen && html[i + 1] == '/');
    size_t ns = i + (closing ? 2 : 1);
    size_t j = ns;
    while (j < htmlLen && html[j] != '>' && html[j] != ' ' && html[j] != '\t' &&
           html[j] != '\n' && html[j] != '\r' && html[j] != '/') {
      j++;
    }
    char name[32];
    tagLocalName(html + ns, j - ns, name, sizeof(name));
    while (j < htmlLen && html[j] != '>') {         // skip attributes, honouring quotes
      if (html[j] == '"' || html[j] == '\'') {
        char q = html[j++];
        while (j < htmlLen && html[j] != q) {
          j++;
        }
      }
      j++;
    }
    size_t next = (j < htmlLen) ? j + 1 : htmlLen;

    if (inList(DROP_TAGS, sizeof(DROP_TAGS) / sizeof(DROP_TAGS[0]), name)) {
      if (closing) {
        skip = skip > 0 ? skip - 1 : 0;
      } else {
        skip++;
      }
      i = next;
      continue;
    }
    if (!skip && inList(BLOCK_TAGS, sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]), name)) {
      toFlush(&t);                                  // flush on BOTH open and close, as COVEY
    }
    i = next;
  }
  toFlush(&t);
  ebFree(pend);
  return t.len;
}

// ---------------------------------------------------------------- OPF
struct OpfItem { char id[64]; char href[EPUB_NAME_MAX]; char media[64]; };

struct OpfParse {
  OpfItem* items;
  int      nItems;
  char   (*spineIds)[64];
  int      nSpine;
};

static void collapseCut(const char* in, size_t inLen, char* out, size_t cap, size_t maxChars) {
  // " ".join(text.split())[:120] — collapse whitespace, then cut by CHARACTER.
  char tmp[EPUB_META_MAX * 2];
  size_t n = 0;
  bool wrote = false;
  size_t i = 0;
  while (i < inLen && n < sizeof(tmp) - 1) {
    size_t sp;
    while (i < inLen && (sp = utf8SpaceLen(in + i, inLen - i)) != 0) {
      i += sp;
    }
    if (i >= inLen) {
      break;
    }
    if (wrote && n < sizeof(tmp) - 1) {
      tmp[n++] = ' ';
    }
    while (i < inLen && utf8SpaceLen(in + i, inLen - i) == 0) {
      char ebuf[8];
      size_t elen = 0;
      size_t used = decodeEntity(in + i, inLen - i, ebuf, &elen);
      if (used) {
        for (size_t z = 0; z < elen && n < sizeof(tmp) - 1; z++) {
          tmp[n++] = ebuf[z];
        }
        i += used;
      } else if (n < sizeof(tmp) - 1) {
        tmp[n++] = in[i++];
      } else {
        i++;
      }
    }
    wrote = true;
  }
  tmp[n] = '\0';
  bsUtf8TruncChars(out, cap, tmp, maxChars);
}

int epubIds(const EpubBook* b, char out[3][EPUB_ID_MAX]) {
  int n = 0;
  char slug[EPUB_ID_MAX];
  if (b->identifier[0]) {
    epubSlug(b->identifier, slug, sizeof(slug));
    snprintf(out[n++], EPUB_ID_MAX, "id:%s", slug);
  }
  if (b->title[0]) {
    char joined[EPUB_META_MAX * 2];
    snprintf(joined, sizeof(joined), "%s|%s", b->title, b->author);
    epubSlug(joined, slug, sizeof(slug));
    snprintf(out[n++], EPUB_ID_MAX, "ta:%s", slug);
  }
  snprintf(out[n++], EPUB_ID_MAX, "fp:%s", b->fingerprint);
  return n;
}

// ---------------------------------------------------------------- open
struct NameListCtx { const char* want; bool found; };
static bool nameVisit(const ZipEntry* e, void* user) {
  NameListCtx* c = (NameListCtx*)user;
  if (strcmp(e->name, c->want) == 0) {
    c->found = true;
    return false;
  }
  return true;
}
static bool zipHasName(EpubSource* s, const char* name) {
  NameListCtx c = { name, false };
  zipForEach(s, nameVisit, &c);
  return c.found;
}

struct FallbackCtx { EpubBook* b; };
static bool fallbackVisit(const ZipEntry* e, void* user) {
  /* Some malformed files have a manifest but no usable spine. COVEY falls back to every
   * XHTML document in zip order — wrong in principle, right in practice: it is the
   * difference between a readable book and an error message. */
  FallbackCtx* c = (FallbackCtx*)user;
  size_t l = strlen(e->name);
  bool html = (l > 6 && strcmp(e->name + l - 6, ".xhtml") == 0) ||
              (l > 5 && strcmp(e->name + l - 5, ".html") == 0) ||
              (l > 4 && strcmp(e->name + l - 4, ".htm") == 0);
  if (html && c->b->nSpine < EPUB_MAX_SPINE) {
    snprintf(c->b->spine[c->b->nSpine].name, EPUB_NAME_MAX, "%s", e->name);
    c->b->spine[c->b->nSpine].title[0] = '\0';
    c->b->nSpine++;
  }
  return true;
}

static void stemOf(const char* displayName, char* out, size_t cap) {
  const char* dot = strrchr(displayName, '.');
  size_t n = dot ? (size_t)(dot - displayName) : strlen(displayName);
  if (n > cap - 1) {
    n = cap - 1;
  }
  memcpy(out, displayName, n);
  out[n] = '\0';
}

EpubStatus epubOpen(EpubBook* b, EpubSource* src, const char* displayName, bool isTextFile) {
  memset(b, 0, sizeof(*b));
  b->src = src;
  epubFingerprint(src, b->fingerprint);

  if (isTextFile) {
    b->isText = true;
    stemOf(displayName, b->title, sizeof(b->title));
    b->spine = (EpubSpineItem*)ebAlloc(sizeof(EpubSpineItem));
    if (!b->spine) {
      return EPUB_ERR_MEMORY;
    }
    snprintf(b->spine[0].name, EPUB_NAME_MAX, "%s", displayName);
    snprintf(b->spine[0].title, EPUB_CH_TITLE_MAX, "%s", b->title);
    b->nSpine = 1;
    return EPUB_OK;
  }

  ZipEntry ce;
  if (!zipFind(src, "META-INF/container.xml", &ce)) {
    return EPUB_ERR_NO_CONTAINER;
  }
  uint8_t* buf = (uint8_t*)ebAlloc(EPUB_MAX_DOC);
  if (!buf) {
    return EPUB_ERR_MEMORY;
  }
  int n = zipRead(src, &ce, buf, EPUB_MAX_DOC);
  if (n <= 0) {
    ebFree(buf);
    return EPUB_ERR_NO_CONTAINER;
  }

  // container.xml -> the OPF path
  char opfName[EPUB_NAME_MAX] = {0};
  {
    const char* p = (const char*)buf;
    size_t len = (size_t)n;
    size_t i = 0;
    while (i < len && !opfName[0]) {
      if (p[i] != '<') {
        i++;
        continue;
      }
      size_t ns = i + 1;
      size_t j = ns;
      while (j < len && p[j] != '>' && p[j] != ' ' && p[j] != '\t' && p[j] != '\n') {
        j++;
      }
      char name[32];
      tagLocalName(p + ns, j - ns, name, sizeof(name));
      size_t as = j;
      while (j < len && p[j] != '>') {
        j++;
      }
      if (strcmp(name, "rootfile") == 0) {
        tagAttr(p + as, j - as, "full-path", opfName, sizeof(opfName));
      }
      i = j + 1;
    }
  }
  if (!opfName[0]) {
    ebFree(buf);
    return EPUB_ERR_NO_ROOTFILE;
  }

  ZipEntry oe;
  if (!zipFind(src, opfName, &oe)) {
    ebFree(buf);
    return EPUB_ERR_NO_OPF;
  }
  n = zipRead(src, &oe, buf, EPUB_MAX_DOC);
  if (n <= 0) {
    ebFree(buf);
    return EPUB_ERR_NO_OPF;
  }

  // ⚠ THE HREF TRAP: manifest hrefs resolve against the OPF's OWN directory, not the zip
  // root. Getting this wrong yields a book with zero chapters and no error at all.
  char base[EPUB_NAME_MAX] = {0};
  {
    const char* slash = strrchr(opfName, '/');
    if (slash) {
      size_t bl = (size_t)(slash - opfName);
      memcpy(base, opfName, bl);
      base[bl] = '\0';
    }
  }

  OpfItem* items = (OpfItem*)ebAlloc(sizeof(OpfItem) * EPUB_MAX_SPINE);
  char (*spineIds)[64] = (char (*)[64])ebAlloc(64 * EPUB_MAX_SPINE);
  b->spine = (EpubSpineItem*)ebAlloc(sizeof(EpubSpineItem) * EPUB_MAX_SPINE);
  if (!items || !spineIds || !b->spine) {
    ebFree(buf); ebFree(items); ebFree(spineIds); ebFree(b->spine);
    b->spine = NULL;
    return EPUB_ERR_MEMORY;
  }
  int nItems = 0, nSpineIds = 0;

  {
    const char* p = (const char*)buf;
    size_t len = (size_t)n;
    size_t i = 0;
    while (i < len) {
      if (p[i] != '<') {
        i++;
        continue;
      }
      if (i + 3 < len && p[i + 1] == '!' && p[i + 2] == '-' && p[i + 3] == '-') {
        size_t j = i + 4;
        while (j + 2 < len && !(p[j] == '-' && p[j + 1] == '-' && p[j + 2] == '>')) {
          j++;
        }
        i = (j + 3 < len) ? j + 3 : len;
        continue;
      }
      bool closing = (i + 1 < len && p[i + 1] == '/');
      size_t ns = i + (closing ? 2 : 1);
      size_t j = ns;
      while (j < len && p[j] != '>' && p[j] != ' ' && p[j] != '\t' && p[j] != '\n' &&
             p[j] != '\r' && p[j] != '/') {
        j++;
      }
      char name[32];
      tagLocalName(p + ns, j - ns, name, sizeof(name));
      size_t as = j;
      while (j < len && p[j] != '>') {
        if (p[j] == '"' || p[j] == '\'') {
          char q = p[j++];
          while (j < len && p[j] != q) {
            j++;
          }
        }
        j++;
      }
      size_t ae = j;
      size_t textStart = j + 1;
      size_t textEnd = textStart;
      while (textEnd < len && p[textEnd] != '<') {
        textEnd++;
      }

      if (!closing && strcmp(name, "item") == 0 && nItems < EPUB_MAX_SPINE) {
        char id[64], href[EPUB_NAME_MAX], media[64];
        if (tagAttr(p + as, ae - as, "id", id, sizeof(id)) &&
            tagAttr(p + as, ae - as, "href", href, sizeof(href))) {
          snprintf(items[nItems].id, sizeof(items[nItems].id), "%s", id);
          epubNormPath(base, href, items[nItems].href, EPUB_NAME_MAX);
          if (tagAttr(p + as, ae - as, "media-type", media, sizeof(media))) {
            for (char* q = media; *q; q++) {
              if (*q >= 'A' && *q <= 'Z') {
                *q = (char)(*q + 32);
              }
            }
            snprintf(items[nItems].media, sizeof(items[nItems].media), "%s", media);
          } else {
            items[nItems].media[0] = '\0';
          }
          nItems++;
        }
      } else if (!closing && strcmp(name, "itemref") == 0 && nSpineIds < EPUB_MAX_SPINE) {
        char idref[64], linear[16];
        if (tagAttr(p + as, ae - as, "idref", idref, sizeof(idref))) {
          bool skip = tagAttr(p + as, ae - as, "linear", linear, sizeof(linear)) &&
                      (linear[0] == 'n' || linear[0] == 'N');
          if (!skip) {
            snprintf(spineIds[nSpineIds], 64, "%s", idref);
            nSpineIds++;
          }
        }
      } else if (!closing && strcmp(name, "title") == 0 && !b->title[0]) {
        collapseCut(p + textStart, textEnd - textStart, b->title, sizeof(b->title), 120);
      } else if (!closing && strcmp(name, "creator") == 0 && !b->author[0]) {
        collapseCut(p + textStart, textEnd - textStart, b->author, sizeof(b->author), 120);
      } else if (!closing && strcmp(name, "identifier") == 0 && !b->identifier[0]) {
        collapseCut(p + textStart, textEnd - textStart, b->identifier,
                    sizeof(b->identifier), 120);
      }
      i = ae < len ? ae + 1 : len;
    }
  }
  ebFree(buf);

  for (int s = 0; s < nSpineIds && b->nSpine < EPUB_MAX_SPINE; s++) {
    const OpfItem* it = NULL;
    for (int k = 0; k < nItems; k++) {
      if (strcmp(items[k].id, spineIds[s]) == 0) {
        it = &items[k];
        break;
      }
    }
    if (!it || !it->href[0]) {
      continue;
    }
    if (!zipHasName(src, it->href)) {
      continue;
    }
    if (it->media[0] && !strstr(it->media, "html")) {
      continue;                      // images, css and fonts are not reading order
    }
    snprintf(b->spine[b->nSpine].name, EPUB_NAME_MAX, "%s", it->href);
    b->spine[b->nSpine].title[0] = '\0';
    b->nSpine++;
  }
  ebFree(items);
  ebFree(spineIds);

  if (b->nSpine == 0) {
    FallbackCtx fc = { b };
    zipForEach(src, fallbackVisit, &fc);
  }
  if (b->nSpine == 0) {
    ebFree(b->spine);
    b->spine = NULL;
    return EPUB_ERR_NO_CHAPTERS;
  }
  if (!b->title[0]) {
    stemOf(displayName, b->title, sizeof(b->title));
  }
  return EPUB_OK;
}

void epubClose(EpubBook* b) {
  if (b->spine) {
    ebFree(b->spine);
    b->spine = NULL;
  }
  b->nSpine = 0;
}

const char* epubStatusText(EpubStatus s) {
  switch (s) {
    case EPUB_OK:               return "ok";
    case EPUB_ERR_NOT_ZIP:      return "cannot open: not a zip";
    case EPUB_ERR_NO_CONTAINER: return "not an EPUB (no container.xml)";
    case EPUB_ERR_NO_ROOTFILE:  return "no OPF rootfile";
    case EPUB_ERR_NO_OPF:       return "missing OPF";
    case EPUB_ERR_NO_CHAPTERS:  return "no readable chapters";
    case EPUB_ERR_MEMORY:       return "out of memory";
  }
  return "unknown";
}

// ---------------------------------------------------------------- chapters
size_t epubChapterText(EpubBook* b, int i, char* buf, size_t cap) {
  if (i < 0 || i >= b->nSpine || cap < 2) {
    return 0;
  }
  if (b->isText) {
    size_t n = srcRead(b->src, 0, buf, cap - 1);
    buf[n] = '\0';
    /* COVEY does NOT hand back the raw bytes here:
     *     "\n\n".join(b.strip() for b in re.split(r"\n\s*\n", text) if b.strip())
     * Blocks separated by a blank line are stripped, empties dropped, and the whole thing
     * rejoined — so a trailing newline disappears and ragged spacing is regularised. That
     * changes the character offsets, which are exactly what a sync position is expressed
     * in, so "close enough" is not close enough.
     *
     * Rewritten in place: the result can only ever be shorter than the input, and each
     * block is copied to a write cursor that never passes the block it is reading. */
    size_t w = 0, i = 0;
    bool wroteBlock = false;
    while (i < n) {
      size_t blockEnd = n, nextStart = n;
      for (size_t k = i; k < n; k++) {
        if (buf[k] != '\n') {
          continue;
        }
        // re.split's separator is \n\s*\n; \s is greedy with backtracking, so the match
        // ends at the LAST newline of the whitespace run that follows.
        size_t j = k + 1, lastNl = 0;
        bool haveNl = false;
        while (j < n && (buf[j] == ' ' || buf[j] == '\t' || buf[j] == '\n' ||
                         buf[j] == '\r' || buf[j] == '\f' || buf[j] == '\v')) {
          if (buf[j] == '\n') {
            lastNl = j;
            haveNl = true;
          }
          j++;
        }
        if (haveNl) {
          blockEnd = k;
          nextStart = lastNl + 1;
          break;
        }
        k = j - 1;                                  // that run held no newline; skip past it
      }
      size_t s = i, e = blockEnd;
      while (s < e && (buf[s] == ' ' || buf[s] == '\t' || buf[s] == '\n' ||
                       buf[s] == '\r' || buf[s] == '\f' || buf[s] == '\v')) {
        s++;
      }
      while (e > s && (buf[e - 1] == ' ' || buf[e - 1] == '\t' || buf[e - 1] == '\n' ||
                       buf[e - 1] == '\r' || buf[e - 1] == '\f' || buf[e - 1] == '\v')) {
        e--;
      }
      if (e > s) {
        if (wroteBlock) {
          buf[w++] = '\n';
          buf[w++] = '\n';
        }
        memmove(buf + w, buf + s, e - s);
        w += e - s;
        wroteBlock = true;
      }
      i = nextStart;
    }
    buf[w] = '\0';
    return w;
  }
  ZipEntry e;
  if (!zipFind(b->src, b->spine[i].name, &e)) {
    snprintf(buf, cap, "[chapter unreadable]");
    return strlen(buf);
  }
  if (e.uncompSize > EPUB_MAX_DOC) {
    snprintf(buf, cap, "[chapter too large to display]");
    return strlen(buf);
  }
  uint8_t* raw = (uint8_t*)ebAlloc(EPUB_MAX_DOC);
  if (!raw) {
    snprintf(buf, cap, "[chapter unreadable]");
    return strlen(buf);
  }
  int n = zipRead(b->src, &e, raw, EPUB_MAX_DOC);
  size_t out = 0;
  if (n > 0) {
    out = epubExtractText((const char*)raw, (size_t)n, buf, cap);
  } else {
    snprintf(buf, cap, "[chapter unreadable]");
    out = strlen(buf);
  }
  ebFree(raw);
  return out;
}

size_t epubChapterLen(EpubBook* b, int i) {
  char* tmp = (char*)ebAlloc(EPUB_MAX_DOC);
  if (!tmp) {
    return 0;
  }
  size_t n = epubChapterText(b, i, tmp, EPUB_MAX_DOC);
  ebFree(tmp);
  return n;
}

double epubFraction(EpubBook* b, int spine, uint32_t offset) {
  int n = b->nSpine > 0 ? b->nSpine : 1;
  size_t chars = epubChapterLen(b, spine);
  if (chars == 0) {
    chars = 1;
  }
  double within = (double)offset / (double)chars;
  if (within < 0.0) within = 0.0;
  if (within > 1.0) within = 1.0;
  int sp = spine < 0 ? 0 : (spine > n - 1 ? n - 1 : spine);
  double f = ((double)sp + within) / (double)n;
  if (f < 0.0) f = 0.0;
  if (f > 1.0) f = 1.0;
  return f;
}

void epubLocate(EpubBook* b, double fraction, int* spineOut, uint32_t* offsetOut) {
  int n = b->nSpine > 0 ? b->nSpine : 1;
  double f = fraction;
  if (f < 0.0) f = 0.0;
  if (f > 0.999999) f = 0.999999;
  int spine = (int)(f * n);
  if (spine > n - 1) {
    spine = n - 1;
  }
  double within = f * n - spine;
  size_t chars = epubChapterLen(b, spine);
  if (spineOut) {
    *spineOut = spine;
  }
  if (offsetOut) {
    *offsetOut = (uint32_t)(within * (double)chars);
  }
}

const char* epubChapterTitle(const EpubBook* b, int i, char* buf, size_t cap) {
  if (i < 0 || i >= b->nSpine) {
    buf[0] = '\0';
    return buf;
  }
  if (b->spine[i].title[0]) {
    snprintf(buf, cap, "%s", b->spine[i].title);
  } else {
    snprintf(buf, cap, "Chapter %d", i + 1);
  }
  return buf;
}
