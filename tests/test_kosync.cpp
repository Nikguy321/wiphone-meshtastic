/*
 * test_kosync.cpp — KOSync (KOReader's sync protocol) on the WiPhone, proven on a Mac.
 *
 * Three things are at stake and each has its own section below:
 *
 *   1. AGREEMENT. Every number in tests/vectors_kosync.h (tools/gen_kosync_vectors.py: MD5,
 *      partial MD5, CrossPoint's byte-weighted spine and its inverse, the JSON number format,
 *      the response shapes) is reproduced by the SHIPPING epub_parse.cpp / kosync.cpp. A
 *      mismatch here is a position that lands in the wrong book or chapter on the X4 or COVEY,
 *      with nothing logged anywhere.
 *   2. THE PROTOCOL. Routing, auth, the window's answers, the request bytes the client sends,
 *      the config file, the window clock — the parts of the feature that are not sockets.
 *   3. NOBODY'S PLACE MOVES. Nick: "we can't let it screw up the position people are at in
 *      their own wiphones after they update". tests/golden_positions.h was generated ONCE
 *      from the parser as it stood before KOSync (git 1de15a6); the current parser must give
 *      every fixture the same reading spine, chapter lengths, ids, epubFraction and
 *      epubLocate, and a 0.9.78 positions file must load and re-save byte for byte.
 */
#include "../WiPhone/epub_parse.h"
#include "../WiPhone/book_hash.h"
#include "../WiPhone/bookstore.h"
#include "../WiPhone/booksync.h"
#include "../WiPhone/booksync_inbox.h"
#include "../WiPhone/kosync.h"
#include "vectors_kosync.h"
#include "golden_positions.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
static const char* g_group = "";

static void group(const char* name) {
  g_group = name;
  printf("\n\033[1m%s\033[0m\n", name);
}

static void ok(bool cond, const char* what) {
  if (cond) {
    g_pass++;
  } else {
    g_fail++;
    printf("  \033[31mFAIL\033[0m %s :: %s\n", g_group, what);
  }
}

static void eqInt(long long got, long long want, const char* what) {
  if (got == want) {
    g_pass++;
    return;
  }
  g_fail++;
  printf("  \033[31mFAIL\033[0m %s :: %s  want=%lld got=%lld\n", g_group, what, want, got);
}

static void eqStr(const char* got, const char* want, const char* what) {
  if (got && want && strcmp(got, want) == 0) {
    g_pass++;
    return;
  }
  g_fail++;
  printf("  \033[31mFAIL\033[0m %s :: %s\n        want: [%s]\n        got:  [%s]\n", g_group, what,
         want ? want : "(null)", got ? got : "(null)");
}

// Same double, or within `tol` (0 = bit-exact).
static void eqDbl(double got, double want, double tol, const char* what) {
  if (got == want || fabs(got - want) <= tol) {
    g_pass++;
    return;
  }
  g_fail++;
  printf("  \033[31mFAIL\033[0m %s :: %s  want=%.17g got=%.17g\n", g_group, what, want, got);
}

// ---------------------------------------------------------------- FILE*-backed source
static size_t fileRead(void* ctx, uint64_t off, void* buf, size_t len) {
  FILE* f = (FILE*)ctx;
  if (fseek(f, (long)off, SEEK_SET) != 0) {
    return 0;
  }
  return fread(buf, 1, len, f);
}

static bool openSource(const char* file, EpubSource* s, FILE** fOut) {
  char path[256];
  snprintf(path, sizeof(path), "tests/fixtures/%s", file);
  FILE* f = fopen(path, "rb");
  if (!f) {
    return false;
  }
  fseek(f, 0, SEEK_END);
  s->ctx = f;
  s->size = (uint64_t)ftell(f);
  s->read = fileRead;
  *fOut = f;
  return true;
}

static bool isTxt(const char* file) {
  const size_t n = strlen(file);
  return n > 4 && !strcmp(file + n - 4, ".txt");
}

// ================================================================ 1. AGREEMENT
static void testMd5() {
  group("MD5 vs RFC 1321");
  for (size_t i = 0; i < sizeof(KS_MD5) / sizeof(KS_MD5[0]); i++) {
    char hex[33];
    bsMd5Hex(KS_MD5[i].in, strlen(KS_MD5[i].in), hex);
    eqStr(hex, KS_MD5[i].hex, KS_MD5[i].in[0] ? KS_MD5[i].in : "(empty)");
    // The same bytes fed one at a time: the block boundary and the padding must not care.
    BsMd5 c;
    uint8_t d[16];
    bsMd5Init(&c);
    for (const char* p = KS_MD5[i].in; *p; p++) {
      bsMd5Update(&c, p, 1);
    }
    bsMd5Final(&c, d);
    bsHex(d, 16, hex);
    eqStr(hex, KS_MD5[i].hex, "byte-at-a-time");
  }
  // Lengths either side of the 56-byte padding edge and the 64-byte block.
  static const char* EDGE[] = {
    "0123456789012345678901234567890123456789012345678901234",       // 55
    "01234567890123456789012345678901234567890123456789012345",      // 56
    "0123456789012345678901234567890123456789012345678901234567890123",  // 64
  };
  static const char* EDGE_HEX[] = {
    // hashlib.md5(s).hexdigest()
    "6e7a4fc92eb1c3f6e652425bcc8d44b5", "8af270b2847610e742b0791b53648c09",
    "7f7bfd348709deeaace19e3f535f8c54",
  };
  for (int i = 0; i < 3; i++) {
    char hex[33];
    bsMd5Hex(EDGE[i], strlen(EDGE[i]), hex);
    eqStr(hex, EDGE_HEX[i], "padding edge");
  }
}

static void testDocIds() {
  group("document ids: partial MD5 + md5(filename) vs hashlib");
  for (size_t k = 0; k < sizeof(KS_BOOKS) / sizeof(KS_BOOKS[0]); k++) {
    const KsBook& v = KS_BOOKS[k];
    EpubSource s;
    FILE* f = NULL;
    if (!openSource(v.file, &s, &f)) {
      ok(false, v.file);
      continue;
    }
    eqInt((long long)s.size, v.size, "fixture size");
    char id[33];
    ok(epubKosyncPartialMd5(&s, id), "partial MD5 computed");
    eqStr(id, v.partialMd5, v.file);
    epubKosyncFilenameMd5(v.file, id);
    eqStr(id, v.filenameMd5, "filename id");
    char a[33], bn[33];
    ok(epubKosyncIds(&s, v.file, a, bn), "both ids");
    ok(!strcmp(a, v.partialMd5) && !strcmp(bn, v.filenameMd5), "both ids agree");
    fclose(f);
  }
  for (size_t k = 0; k < sizeof(KS_TXT) / sizeof(KS_TXT[0]); k++) {
    const KsTxt& v = KS_TXT[k];
    EpubSource s;
    FILE* f = NULL;
    if (!openSource(v.file, &s, &f)) {
      ok(false, v.file);
      continue;
    }
    char id[33];
    ok(epubKosyncPartialMd5(&s, id), "txt partial MD5 computed");
    eqStr(id, v.partialMd5, v.file);
    epubKosyncFilenameMd5(v.file, id);
    eqStr(id, v.filenameMd5, "txt filename id");
    fclose(f);
  }
}

/* A card that refuses a read part-way must give NO id, not a confident wrong one. */
struct ShortCtx { FILE* f; uint64_t failAt; };
static size_t shortRead(void* ctx, uint64_t off, void* buf, size_t len) {
  ShortCtx* c = (ShortCtx*)ctx;
  if (off >= c->failAt) {
    return len / 2;
  }
  return fileRead(c->f, off, buf, len);
}

static void testShortRead() {
  group("partial MD5: a failed card read gives no id");
  EpubSource s;
  FILE* f = NULL;
  if (!openSource("kosync-mixed.epub", &s, &f)) {
    ok(false, "fixture");
    return;
  }
  ShortCtx c = { f, 65536 };
  EpubSource bad = { &c, s.size, shortRead };
  char id[33] = "x";
  ok(!epubKosyncPartialMd5(&bad, id), "refused");
  eqStr(id, "", "and left empty");
  fclose(f);
}

static bool openBook(const char* file, EpubBook* b, FILE** f, EpubSource* s) {
  if (!openSource(file, s, f)) {
    return false;
  }
  if (epubOpen(b, s, file, isTxt(file)) != EPUB_OK) {
    fclose(*f);
    return false;
  }
  return true;
}

static void testSpineAndPercent() {
  for (size_t k = 0; k < sizeof(KS_BOOKS) / sizeof(KS_BOOKS[0]); k++) {
    const KsBook& v = KS_BOOKS[k];
    char g[96];
    snprintf(g, sizeof(g), "CrossPoint spine + percentage: %s", v.file);
    group(g);
    EpubSource s;
    FILE* f = NULL;
    EpubBook b;
    if (!openBook(v.file, &b, &f, &s)) {
      ok(false, "opens");
      continue;
    }
    const EpubKosyncMap* m = b.kosync;
    ok(m != NULL, "has a KOSync map");
    if (!m) {
      epubClose(&b);
      fclose(f);
      continue;
    }
    ok(m->sizesKnown, "sizes known");
    eqInt(m->nCp, v.nCp, "CrossPoint item count");
    for (int i = 0; i < v.nCp && i < m->nCp; i++) {
      char w[128];
      snprintf(w, sizeof(w), "cum[%d] (%s, %u B)", i, v.cp[i].path, (unsigned)v.cp[i].size);
      eqInt(m->cum[i], v.cum[i], w);
    }
    eqInt(epubKosyncTotal(m), v.total, "total");
    // The reading spine is the WiPhone's own and must be what the generator modelled.
    eqInt(b.nSpine, v.nReading, "reading spine length");
    for (int r = 0; r < v.nReading && r < b.nSpine; r++) {
      eqStr(b.spine[r].name, v.reading[r], "reading spine entry");
      eqInt(m->readToCp[r], v.readingToCp[r], "reading -> CrossPoint");
    }
    for (int i = 0; i < v.nFwd; i++) {
      double p = -1;
      char w[96];
      snprintf(w, sizeof(w), "forward r=%d within=%g", v.fwd[i].r, v.fwd[i].within);
      ok(epubKosyncPercent(m, v.fwd[i].r, v.fwd[i].within, &p), w);
      eqDbl(p, v.fwd[i].pct, 0.0, w);
    }
    for (int i = 0; i < v.nInv; i++) {
      int r = -9, c = -9;
      double rw = -1, cw = -1;
      char w[96];
      snprintf(w, sizeof(w), "inverse p=%.17g", v.inv[i].p);
      ok(epubKosyncLocate(m, v.inv[i].p, &r, &rw, &c, &cw), w);
      eqInt(c, v.inv[i].cp, w);
      eqDbl(cw, v.inv[i].within, 1e-12, w);
      eqInt(r, v.inv[i].r, w);
      eqDbl(rw, v.inv[i].rwithin, 1e-12, w);
    }
    /* Offsets: a reader position -> percentage -> back to the same place (one byte of slack
     * for the double -> uint32 truncation). This is the round trip a Sync makes. */
    for (int r = 0; r < b.nSpine; r++) {
      if (m->readToCp[r] < 0) {
        continue;
      }
      const size_t len = epubChapterLen(&b, r);
      const uint32_t offs[] = { 0, (uint32_t)(len / 4), (uint32_t)(len / 2), (uint32_t)(len * 9 / 10) };
      for (int j = 0; j < 4; j++) {
        double p = -1;
        ok(epubKosyncPercentAt(&b, r, offs[j], len, &p), "percent at an offset");
        int sp = -1;
        uint32_t off = 0;
        ok(epubKosyncLocateOffset(&b, p, &sp, &off), "locate an offset");
        char w[96];
        snprintf(w, sizeof(w), "round trip r=%d off=%u", r, (unsigned)offs[j]);
        eqInt(sp, r, w);
        ok((off > offs[j] ? off - offs[j] : offs[j] - off) <= 1, w);
        // Independently: the percentage is the vector rule applied to offset / length.
        double direct = -1;
        epubKosyncPercent(m, r, len ? (double)offs[j] / (double)len : 0.0, &direct);
        eqDbl(p, direct, 0.0, "percentAt == percent(offset / length)");
      }
    }
    epubClose(&b);
    fclose(f);
  }
}

static void testTextAndUnsyncable() {
  group("a .txt is one item; a book with no spine is not syncable");
  EpubSource s;
  FILE* f = NULL;
  EpubBook b;
  if (openBook("my-notes.txt", &b, &f, &s)) {
    ok(b.kosync && b.kosync->nCp == 1, "one item");
    eqInt(epubKosyncTotal(b.kosync), 54, "total = the file size");
    double p = -1;
    ok(epubKosyncPercentAt(&b, 0, 13, 52, &p), "percent");
    eqDbl(p, 0.25, 0.0, "pct = offset / extracted length");
    epubClose(&b);
    fclose(f);
  } else {
    ok(false, "my-notes.txt");
  }
  // empty-spine.epub reads (a zip-order fallback chapter) but CrossPoint lists nothing.
  if (openBook("empty-spine.epub", &b, &f, &s)) {
    eqInt(b.nSpine, 1, "still reads");
    eqInt(epubKosyncTotal(b.kosync), 0, "total 0");
    double p = -1;
    ok(!epubKosyncPercentAt(&b, 0, 5, 30, &p), "percent refused");
    int r = 0;
    double w = 0;
    ok(!epubKosyncLocate(b.kosync, 0.5, &r, &w, NULL, NULL), "locate refused");
    epubClose(&b);
    fclose(f);
  } else {
    ok(false, "empty-spine.epub");
  }
  // A map whose zip walk failed: sizes unknown, never a guessed total.
  static EpubKosyncMap m;
  memset(&m, 0, sizeof(m));
  m.nCp = 1; m.nRead = 1; m.cum[0] = 100; m.readToCp[0] = 0; m.cpToRead[0] = 0;
  m.sizesKnown = false;
  double p = -1;
  ok(!epubKosyncPercent(&m, 0, 0.5, &p), "sizes unknown -> refused");
  m.sizesKnown = true;
  ok(epubKosyncPercent(&m, 0, 0.5, &p) && p == 0.5, "sizes known -> 0.5");
  ok(!epubKosyncPercent(&m, 1, 0.5, &p), "a chapter out of range -> refused");
  m.readToCp[0] = -1;
  ok(!epubKosyncPercent(&m, 0, 0.5, &p), "a chapter CrossPoint does not list -> refused");
  // NaN / out of range within and pct are clamped, not propagated.
  m.readToCp[0] = 0;
  ok(epubKosyncPercent(&m, 0, NAN, &p) && p == 0.0, "NaN within -> 0");
  int r = -1;
  double w = -1;
  ok(epubKosyncLocate(&m, 7.0, &r, &w, NULL, NULL) && r == 0 && w == 1.0, "pct > 1 clamps");
  ok(epubKosyncLocate(&m, -3.0, &r, &w, NULL, NULL) && r == 0 && w == 0.0, "pct < 0 clamps");
}

static void testOversized() {
  group("a chapter too big to display sends within 0");
  static EpubBook b;
  static EpubKosyncMap m;
  memset(&b, 0, sizeof(b));
  memset(&m, 0, sizeof(m));
  m.nCp = 2; m.nRead = 2; m.sizesKnown = true;
  m.cum[0] = 1000; m.cum[1] = 1000 + 600 * 1024;
  m.readToCp[0] = 0; m.readToCp[1] = 1; m.cpToRead[0] = 0; m.cpToRead[1] = 1;
  b.kosync = &m;
  b.nSpine = 2;
  double p = -1;
  ok(epubKosyncPercentAt(&b, 1, 20, 30, &p), "percent");
  eqDbl(p, 1000.0 / (1000 + 600 * 1024), 0.0, "the chapter's start, not 2/3 of a placeholder");
  ok(epubKosyncPercentAt(&b, 0, 500, 1000, &p), "a normal chapter");
  eqDbl(p, 500.0 / (1000 + 600 * 1024), 0.0, "normal chapter unaffected");
}

static void testPaths() {
  group("CrossPoint's path rules (its spine only)");
  struct { const char* in; const char* dec; const char* norm; } V[] = {
    { "OEBPS/Text/chapter%202.xhtml", "OEBPS/Text/chapter 2.xhtml", "OEBPS/Text/chapter 2.xhtml" },
    { "a%2Fb%2fc", "a/b/c", "a/b/c" },
    { "bad%zzescape", "bad%zzescape", "bad%zzescape" },
    { "tail%4", "tail%4", "tail%4" },
    { "tail%41", "tailA", "tailA" },
    { "OEBPS/./Text/one.xhtml", "OEBPS/./Text/one.xhtml", "OEBPS/./Text/one.xhtml" },
    { "OEBPS/../Text/one.xhtml", "OEBPS/../Text/one.xhtml", "Text/one.xhtml" },
    { "../../x.xhtml", "../../x.xhtml", "x.xhtml" },
    { "a//b///c/", "a//b///c/", "a/b/c" },
    { "/lead/ing", "/lead/ing", "lead/ing" },
    { "a/b/../../c", "a/b/../../c", "c" },
  };
  for (size_t i = 0; i < sizeof(V) / sizeof(V[0]); i++) {
    char d[256], n[256];
    epubKosyncDecodePath(V[i].in, d, sizeof(d));
    eqStr(d, V[i].dec, V[i].in);
    epubKosyncNormPath(d, n, sizeof(n));
    eqStr(n, V[i].norm, V[i].in);
    // In place, as epubOpen does it.
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", V[i].in);
    epubKosyncDecodePath(buf, buf, sizeof(buf));
    epubKosyncNormPath(buf, buf, sizeof(buf));
    eqStr(buf, V[i].norm, "in place");
  }
}

static void testFormat() {
  group("percentage as a JSON number (KS_FMT)");
  for (size_t i = 0; i < sizeof(KS_FMT) / sizeof(KS_FMT[0]); i++) {
    char out[32];
    kosyncFormatPct(KS_FMT[i].p, out, sizeof(out));
    eqStr(out, KS_FMT[i].text, KS_FMT[i].text);
  }
  char out[32];
  kosyncFormatPct(-0.2, out, sizeof(out));
  eqStr(out, "0", "negative clamps to 0");
  kosyncFormatPct(1.7, out, sizeof(out));
  eqStr(out, "1", "over 1 clamps to 1");
  kosyncFormatPct(NAN, out, sizeof(out));
  eqStr(out, "0", "NaN -> 0");
  kosyncFormatPct(0.61, out, sizeof(out));
  eqStr(out, "0.61", "0.61");
}

static void testParse() {
  group("reading a progress object (KS_PARSE + the awkward ones)");
  for (size_t i = 0; i < sizeof(KS_PARSE) / sizeof(KS_PARSE[0]); i++) {
    const KsParseVec& v = KS_PARSE[i];
    KosyncProgress p;
    ok(kosyncParseProgress(v.body, strlen(v.body), &p), v.body);
    eqInt(p.hasPct, v.has, "has a percentage");
    if (v.has) {
      eqDbl(p.pct, v.pct, 0.0, "percentage");
      eqStr(p.device, v.device, "device");
      ok(p.hasTimestamp, "has a timestamp");
      eqInt(p.timestamp, v.ts, "timestamp");
    }
  }
  KosyncProgress p;
  const char* nested = "{ \"metadata\": {\"a\":[1,2,{\"b\":\"}\"}]}, \"percentage\" : 1e-1 ,"
                       "\"device\":\"Caf\\u00e9 \\\"X4\\\"\",\"device_id\":\"\\ud83d\\ude00\","
                       "\"document\":\"abc\",\"progress\":null, \"extra\": true }";
  ok(kosyncParseProgress(nested, strlen(nested), &p), "nested values are skipped");
  eqDbl(p.pct, 0.1, 0.0, "1e-1");
  eqStr(p.device, "Caf\xc3\xa9 \"X4\"", "escapes decoded");
  eqStr(p.deviceId, "\xf0\x9f\x98\x80", "a surrogate pair");
  eqStr(p.document, "abc", "document");
  const char* quoted = "{\"percentage\":\"0.25\",\"timestamp\":\"12\"}";
  ok(kosyncParseProgress(quoted, strlen(quoted), &p) && p.hasPct && p.pct == 0.25, "a quoted number");
  eqInt(p.timestamp, 12, "a quoted timestamp");
  const char* nul = "{\"percentage\":null,\"device\":null}";
  ok(kosyncParseProgress(nul, strlen(nul), &p) && !p.hasPct, "null is absent");
  static const char* BAD[] = { "", "[]", "not json", "{\"percentage\":0.5", "{\"a\" 1}",
                               "{\"percentage\":0.5,}", "{\"x\":\"\\q\"}", "{\"x\":\"unterminated}" };
  for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
    ok(!kosyncParseProgress(BAD[i], strlen(BAD[i]), &p), BAD[i][0] ? BAD[i] : "(empty)");
  }
  // Length-bounded: bytes after `len` are not read (a body in a bigger buffer).
  const char* two = "{\"percentage\":0.5}{\"percentage\":0.9}";
  ok(kosyncParseProgress(two, 18, &p) && p.pct == 0.5, "stops at len");
  char esc[64];
  kosyncJsonEscape("a\"b\\c\nd", esc, sizeof(esc));
  eqStr(esc, "a\\\"b\\\\c\\u000ad", "escape");
}

// ================================================================ 2. THE PROTOCOL
static const char* HDRS_OK =
    "PUT /syncs/progress HTTP/1.1\r\nHost: 192.168.4.1\r\nUser-Agent: ESP32HTTPClient\r\n"
    "X-Auth-User:   nick  \r\nx-auth-key: 5f4dcc3b5aa765d61d8327deb882cf99\r\n"
    "Accept: application/vnd.koreader.v1+json\r\nAuthorization: Basic bmljazpwYXNzd29yZA==\r\n"
    "Content-Length: 12\r\n\r\n";
static const char* USER = "nick";
static const char* KEY = "5f4dcc3b5aa765d61d8327deb882cf99";   // md5("password") — a test value

static void testHeaders() {
  group("headers and auth");
  char v[64];
  ok(kosyncHeader(HDRS_OK, "x-auth-user", v, sizeof(v)), "found case-insensitively");
  eqStr(v, "nick", "trimmed");
  ok(kosyncHeader(HDRS_OK, "CONTENT-LENGTH", v, sizeof(v)) && !strcmp(v, "12"), "content-length");
  ok(!kosyncHeader(HDRS_OK, "x-auth", v, sizeof(v)), "a prefix is not a match");
  ok(!kosyncHeader("GET / HTTP/1.1\r\n\r\nx-auth-user: late\r\n", "x-auth-user", v, sizeof(v)),
     "nothing after the blank line");
  ok(kosyncAuthOk(HDRS_OK, USER, KEY), "right user and key");
  ok(!kosyncAuthOk(HDRS_OK, "nick2", KEY), "wrong user");
  ok(!kosyncAuthOk(HDRS_OK, USER, "5f4dcc3b5aa765d61d8327deb882cf98"), "wrong key");
  ok(!kosyncAuthOk(HDRS_OK, USER, "5F4DCC3B5AA765D61D8327DEB882CF99"), "the key is exact (lower case)");
  ok(!kosyncAuthOk(HDRS_OK, "", ""), "nothing configured authorises nobody");
  ok(!kosyncAuthOk("GET /users/auth HTTP/1.1\r\nx-auth-user: nick\r\n\r\n", USER, KEY), "no key header");
  eqInt(kosyncStatusCode("HTTP/1.1 200 OK"), 200, "status 200");
  eqInt(kosyncStatusCode("HTTP/1.0 401 Unauthorized"), 401, "status 401");
  eqInt(kosyncStatusCode("garbage"), -1, "not a status line");

  char ch[128];
  strcpy(ch, "7\r\n{\"perce\r\nB;ext=1\r\nntage\":0.5}\r\n0\r\nTrailer: x\r\n\r\n");
  long n = kosyncDechunk(ch, strlen(ch));
  ok(n == 18 && !memcmp(ch, "{\"percentage\":0.5}", 18), "chunked body decoded");
  strcpy(ch, "5\r\nabc");
  ok(kosyncDechunk(ch, strlen(ch)) < 0, "a truncated chunk is refused");
  strcpy(ch, "zz\r\n");
  ok(kosyncDechunk(ch, strlen(ch)) < 0, "no size is refused");
  strcpy(ch, "3\r\nabcX\r\n0\r\n\r\n");
  ok(kosyncDechunk(ch, strlen(ch)) < 0, "a chunk not followed by CRLF is refused");
}

static void testRequests() {
  group("the client's request bytes (contract 7.4)");
  char out[768];
  size_t n = kosyncBuildGet(out, sizeof(out), "192.168.1.20", 8088, USER, KEY,
                            "7db24c08211c49e8e0c2b8522e9efc1b");
  eqStr(out,
        "GET /syncs/progress/7db24c08211c49e8e0c2b8522e9efc1b HTTP/1.1\r\n"
        "Host: 192.168.1.20:8088\r\n"
        "Accept: application/vnd.koreader.v1+json\r\n"
        "x-auth-user: nick\r\n"
        "x-auth-key: 5f4dcc3b5aa765d61d8327deb882cf99\r\n"
        "Connection: close\r\n\r\n", "GET");
  eqInt((long long)n, (long long)strlen(out), "GET length");
  char body[256];
  kosyncBuildPutBody(body, sizeof(body), "d1d6d400f5a32bfd6a3534f2a71c0ded", 0.61, "WiPhone-NICK",
                     "0123456789abcdef0123456789abcdef");
  eqStr(body,
        "{\"document\":\"d1d6d400f5a32bfd6a3534f2a71c0ded\",\"progress\":\"\",\"percentage\":0.61,"
        "\"device\":\"WiPhone-NICK\",\"device_id\":\"0123456789abcdef0123456789abcdef\"}", "PUT body");
  n = kosyncBuildPut(out, sizeof(out), "covey.local", 80, USER, KEY, body);
  char want[768];
  snprintf(want, sizeof(want),
           "PUT /syncs/progress HTTP/1.1\r\n"
           "Host: covey.local\r\n"
           "Accept: application/vnd.koreader.v1+json\r\n"
           "Content-Type: application/json\r\n"
           "x-auth-user: nick\r\n"
           "x-auth-key: 5f4dcc3b5aa765d61d8327deb882cf99\r\n"
           "Content-Length: %u\r\n"
           "Connection: close\r\n\r\n%s", (unsigned)strlen(body), body);
  eqStr(out, want, "PUT (port 80 left off Host)");
  KosyncProgress p;
  ok(kosyncParseProgress(body, strlen(body), &p) && p.hasPct && p.pct == 0.61, "our body parses back");
  eqStr(p.device, "WiPhone-NICK", "device survives");
  ok(kosyncBuildGet(out, 64, "h", 80, USER, KEY, "doc") == 0, "too small a buffer builds nothing");
}

static const char* PARTIAL = "7db24c08211c49e8e0c2b8522e9efc1b";
static const char* BYNAME = "ad7a1fcd11740eb658e5cd1742a8bfed";

static void serve(const char* method, const char* path, const char* hdrs, const char* body,
                  KosyncServeOut* o, char* reply) {
  static KosyncServed book = { PARTIAL, BYNAME, true, 0.61, "WiPhone-NICK", "wiphone-id", 1790000100u };
  kosyncServe(method, path, hdrs, body, body ? strlen(body) : 0, USER, KEY, &book, 1790000200u,
              o, reply, 512);
}

static void testServe() {
  group("the window's answers");
  KosyncServeOut o;
  char reply[512];
  char hdrs[512];
  snprintf(hdrs, sizeof(hdrs), "X HTTP/1.1\r\nx-auth-user: %s\r\nx-auth-key: %s\r\n\r\n", USER, KEY);
  const char* noAuth = "X HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n";

  ok(kosyncIsPath("/users/auth") && kosyncIsPath("/syncs/progress") && kosyncIsPath("/healthcheck"),
     "our paths");
  ok(!kosyncIsPath("/") && !kosyncIsPath("/chunk") && !kosyncIsPath("/syncsx"), "not ours");

  serve("GET", "/healthcheck", noAuth, NULL, &o, reply);
  eqInt(o.code, 200, "healthcheck needs no auth");
  eqStr(reply, "{\"state\":\"OK\"}", "healthcheck body");
  serve("GET", "/users/auth", hdrs, NULL, &o, reply);
  eqInt(o.code, 200, "auth ok");
  eqStr(reply, "{\"authorized\":\"OK\"}", "auth body");
  serve("GET", "/users/auth", noAuth, NULL, &o, reply);
  eqInt(o.code, 401, "auth refused");
  eqStr(reply, "{\"message\":\"Unauthorized\",\"code\":2001}", "401 body");
  serve("POST", "/users/create", hdrs, "{\"username\":\"a\",\"password\":\"b\"}", &o, reply);
  eqInt(o.code, 402, "no registration");

  serve("GET", "/syncs/progress/0000000000000000000000000000beef", hdrs, NULL, &o, reply);
  eqInt(o.code, 200, "another document: 200");
  eqStr(reply, "{}", "another document: {} (never a 404)");
  ok(!o.pickedUp, "another document is not a pick-up");

  serve("GET", "/syncs/progress/7db24c08211c49e8e0c2b8522e9efc1b", hdrs, NULL, &o, reply);
  eqInt(o.code, 200, "our book by partial MD5");
  ok(o.pickedUp, "picked up");
  eqStr(reply, "{\"document\":\"7db24c08211c49e8e0c2b8522e9efc1b\",\"percentage\":0.61,\"progress\":\"\","
               "\"device\":\"WiPhone-NICK\",\"device_id\":\"wiphone-id\",\"timestamp\":1790000100}",
        "our place");
  KosyncProgress p;
  ok(kosyncParseProgress(reply, strlen(reply), &p) && p.pct == 0.61, "which parses as 0.61");
  serve("GET", "/syncs/progress/ad7a1fcd11740eb658e5cd1742a8bfed", hdrs, NULL, &o, reply);
  ok(o.code == 200 && o.pickedUp && strstr(reply, "\"document\":\"ad7a1fcd11740eb658e5cd1742a8bfed\""),
     "our book by filename id, echoed");
  serve("GET", "/syncs/progress/7db24c08211c49e8e0c2b8522e9efc1b", noAuth, NULL, &o, reply);
  ok(o.code == 401 && !o.pickedUp, "no auth, no place");

  serve("PUT", "/syncs/progress", hdrs,
        "{\"document\":\"7db24c08211c49e8e0c2b8522e9efc1b\",\"progress\":\"/body/DocFragment[8]\","
        "\"percentage\":0.7,\"device\":\"CrossPoint\",\"device_id\":\"x4\"}", &o, reply);
  eqInt(o.code, 200, "PUT ours: 200");
  eqStr(reply, "{\"document\":\"7db24c08211c49e8e0c2b8522e9efc1b\",\"timestamp\":1790000200}", "PUT reply");
  ok(o.pickedUp && o.gotPut && o.park && !o.otherDoc, "parked");
  eqDbl(o.putPct, 0.7, 0.0, "their percentage");
  eqStr(o.putDevice, "CrossPoint", "their name");

  serve("PUT", "/syncs/progress", hdrs,
        "{\"document\":\"ad7a1fcd11740eb658e5cd1742a8bfed\",\"percentage\":0.6105,\"device\":\"CrossPoint\"}",
        &o, reply);
  ok(o.gotPut && !o.park, "within 0.001 of us: synchronized, no card");
  serve("PUT", "/syncs/progress", hdrs,
        "{\"document\":\"ad7a1fcd11740eb658e5cd1742a8bfed\",\"percentage\":0.2,\"device\":\"WiPhone-NICK\"}",
        &o, reply);
  ok(o.gotPut && !o.park, "our own name coming back: no card");
  serve("PUT", "/syncs/progress", hdrs,
        "{\"document\":\"ad7a1fcd11740eb658e5cd1742a8bfed\",\"percentage\":0.2,\"device\":\"Other\","
        "\"device_id\":\"wiphone-id\"}", &o, reply);
  ok(o.gotPut && !o.park, "our own device id coming back: no card");

  serve("PUT", "/syncs/progress", hdrs,
        "{\"document\":\"someotherbook\",\"percentage\":0.2,\"device\":\"CrossPoint\"}", &o, reply);
  ok(o.code == 200 && o.otherDoc && !o.gotPut && !o.pickedUp, "another book: 200 and discarded");
  eqStr(reply, "{\"document\":\"someotherbook\",\"timestamp\":1790000200}", "still answered properly");

  serve("PUT", "/syncs/progress", hdrs, "{\"percentage\":0.2}", &o, reply);
  eqInt(o.code, 400, "no document: 400");
  serve("PUT", "/syncs/progress", hdrs, "garbage", &o, reply);
  eqInt(o.code, 400, "not JSON: 400");
  serve("PUT", "/syncs/progress", noAuth, "{\"document\":\"x\",\"percentage\":0.2}", &o, reply);
  eqInt(o.code, 401, "PUT without auth: 401");
  serve("GET", "/syncs/progress", hdrs, NULL, &o, reply);
  eqInt(o.code, 404, "GET without a document: 404");
  serve("GET", "/syncs/progress/a/b", hdrs, NULL, &o, reply);
  eqInt(o.code, 404, "a document id with a slash: 404");
  serve("DELETE", "/syncs/progress", hdrs, NULL, &o, reply);
  eqInt(o.code, 404, "unknown method: 404");

  // (6) The phone somewhere it cannot express: `{}` for our book, never 0 or a stale place.
  {
    KosyncServed lost = { PARTIAL, BYNAME, false, 0.61, "WiPhone-NICK", "wiphone-id", 1790000100u };
    kosyncServe("GET", "/syncs/progress/7db24c08211c49e8e0c2b8522e9efc1b", hdrs, NULL, 0, USER, KEY,
                &lost, 1790000200u, &o, reply, 512);
    ok(o.code == 200 && o.pickedUp, "no place of ours: still our book, still 200");
    eqStr(reply, "{}", "no place of ours: {}");
    const char* put = "{\"document\":\"7db24c08211c49e8e0c2b8522e9efc1b\",\"percentage\":0.6105,"
                      "\"device\":\"CrossPoint\"}";
    kosyncServe("PUT", "/syncs/progress", hdrs, put, strlen(put), USER, KEY, &lost, 1790000200u,
                &o, reply, 512);
    ok(o.gotPut && o.park, "no place of ours: another device's place is offered (no epsilon)");
    const char* own = "{\"document\":\"7db24c08211c49e8e0c2b8522e9efc1b\",\"percentage\":0.2,"
                      "\"device\":\"WiPhone-NICK\"}";
    kosyncServe("PUT", "/syncs/progress", hdrs, own, strlen(own), USER, KEY, &lost, 1790000200u,
                &o, reply, 512);
    ok(o.gotPut && !o.park, "no place of ours: our own record is still not offered");
  }

  // The worth-parking rule on its own.
  ok(kosyncWorthParking(0.5, "X4", "", 0.3, "WiPhone-A", "id"), "another device, a real move");
  ok(!kosyncWorthParking(0.5009, "X4", "", 0.5, "WiPhone-A", "id"), "within epsilon");
  ok(kosyncWorthParking(0.4985, "X4", "", 0.5, "WiPhone-A", "id"), "backwards counts too");
  ok(!kosyncWorthParking(0.9, "WiPhone-A", "", 0.1, "WiPhone-A", "id"), "our own name");

  // The pull-on-open rule: news = newer (both clocks) or ahead (either clock unknown).
  ok(kosyncPullIsNews(0.3, true, 2000, 0.5, true, 1000), "newer than our last turn, even if behind");
  ok(!kosyncPullIsNews(0.9, true, 900, 0.5, true, 1000), "older than our last turn, even if ahead");
  ok(kosyncPullIsNews(0.6, false, 0, 0.5, true, 1000), "no server time: ahead is news");
  ok(!kosyncPullIsNews(0.4, false, 0, 0.5, true, 1000), "no server time: behind is not");
  ok(!kosyncPullIsNews(0.4, true, 2000, 0.5, true, 0), "our clock unknown: behind is not");
  ok(kosyncPullIsNews(0.6, true, 2000, 0.5, true, 0), "our clock unknown: ahead is");
  ok(!kosyncPullIsNews(0.5005, false, 0, 0.5, true, 0), "ahead by less than epsilon is not");
  ok(kosyncPullIsNews(0.1, false, 0, 0.5, false, 0), "we cannot place ourselves: offer it");
}

static void testClock() {
  group("the window's clock");
  KosyncWindowClock c;
  memset(&c, 0, sizeof(c));
  eqInt(kosyncClockDue(&c, 5), KOSYNC_CLOCK_CLOSED, "never opened");
  kosyncClockOpen(&c, 1000, KOSYNC_WINDOW_SYNC_MS);
  eqInt(kosyncClockDue(&c, 1000), KOSYNC_CLOCK_RUNNING, "running");
  eqInt(kosyncClockRemainingMs(&c, 1000), 300000, "300 s left");
  eqInt(kosyncClockDue(&c, 1000 + 299999), KOSYNC_CLOCK_RUNNING, "1 ms before the deadline");
  eqInt(kosyncClockDue(&c, 1000 + 300000), KOSYNC_CLOCK_DEADLINE, "the deadline");

  group("the window's clock: a GET's grace is long, a PUT's short (3)");
  /* A stock CrossPoint: GET at 5 s, then loads and maps the book, then (ASK mode) waits for a
   * human. The window must still be there at 5 s + 119 s for the PUT that follows. */
  kosyncClockOpen(&c, 1000, KOSYNC_WINDOW_SYNC_MS);
  kosyncClockPicked(&c, 5000, KOSYNC_GRACE_GET_MS);
  eqInt(kosyncClockRemainingMs(&c, 5000), 120000, "a GET arms 120 s");
  eqInt(kosyncClockDue(&c, 5000 + 10000), KOSYNC_CLOCK_RUNNING, "still open 10 s after the GET");
  eqInt(kosyncClockDue(&c, 5000 + 119999), KOSYNC_CLOCK_RUNNING, "still open for the human's PUT");
  kosyncClockPicked(&c, 90000, KOSYNC_GRACE_PUT_MS);       // the PUT, 85 s later
  eqInt(kosyncClockRemainingMs(&c, 90000), 10000, "a PUT arms 10 s");
  eqInt(kosyncClockDue(&c, 99999), KOSYNC_CLOCK_RUNNING, "grace running");
  eqInt(kosyncClockDue(&c, 100000), KOSYNC_CLOCK_PICKED, "closes 10 s after the PUT");
  kosyncClockOpen(&c, 1000, KOSYNC_WINDOW_SYNC_MS);
  kosyncClockPicked(&c, 5000, KOSYNC_GRACE_GET_MS);
  eqInt(kosyncClockDue(&c, 5000 + 120000), KOSYNC_CLOCK_PICKED, "a GET with no PUT: 120 s, then shut");
  kosyncClockOpen(&c, 1000, KOSYNC_WINDOW_SYNC_MS);
  kosyncClockPicked(&c, 5000, KOSYNC_GRACE_PUT_MS);
  kosyncClockPicked(&c, 8000, KOSYNC_GRACE_GET_MS);        // a new round starts with a GET
  eqInt(kosyncClockDue(&c, 20000), KOSYNC_CLOCK_RUNNING, "the LAST request decides");
  kosyncClockOpen(&c, 1000, KOSYNC_WINDOW_OPEN_MS);
  kosyncClockPicked(&c, 30000, KOSYNC_GRACE_GET_MS);
  eqInt(kosyncClockRemainingMs(&c, 30000), 31000, "the deadline caps the long grace");
  eqInt(kosyncClockDue(&c, 61000), KOSYNC_CLOCK_DEADLINE, "60 s window: the deadline wins");
  kosyncClockOpen(&c, 1000, KOSYNC_WINDOW_OPEN_MS);
  kosyncClockPicked(&c, 59000, KOSYNC_GRACE_PUT_MS);
  eqInt(kosyncClockRemainingMs(&c, 59000), 2000, "the deadline caps the grace");
  eqInt(kosyncClockDue(&c, 61000), KOSYNC_CLOCK_DEADLINE, "the deadline wins");
  // millis() wraps every ~49.7 days: a window opened just before must still close on time.
  kosyncClockOpen(&c, 0xFFFFF000u, KOSYNC_WINDOW_OPEN_MS);
  eqInt(kosyncClockDue(&c, 0x00000100u), KOSYNC_CLOCK_RUNNING, "across the wrap: running");
  eqInt(kosyncClockDue(&c, 0xFFFFF000u + 60000u), KOSYNC_CLOCK_DEADLINE, "across the wrap: closes");
}

static void testConfig() {
  group("/books/kosync.txt");
  KosyncConfig c;
  const char* full =
      "# KOSync\r\n"
      "user = nick\r\n"
      "password=correct horse battery\r\n"
      "home=http://192.168.1.20:8088/\r\n"
      "device=WiPhone-NICK\r\n"
      "auto=on\r\n"
      "open_window = yes\r\n"
      "unknown=whatever\r\n";
  ok(kosyncParseConfig(full, strlen(full), &c), "a full file");
  ok(c.ok, "ok");
  eqStr(c.user, "nick", "user");
  char want[33];
  bsMd5Hex("correct horse battery", 21, want);
  eqStr(c.key, want, "the key is MD5(password)");
  ok(!c.passwordShort, "21 characters is long enough");
  eqStr(c.home, "192.168.1.20", "home host, scheme and slash stripped");
  eqInt(c.homePort, 8088, "home port");
  eqStr(c.device, "WiPhone-NICK", "device");
  ok(c.autoOnClose && c.openWindow, "switches");
  eqStr(c.problem, "", "no problem");
  ok(!strstr((const char*)&c, "correct horse"), "the plaintext is not kept");

  const char* minimal = "user=a\npassword=short\n";
  ok(kosyncParseConfig(minimal, strlen(minimal), &c) && c.passwordShort, "short password warns");
  eqStr(c.problem, "password is under 12 characters", "and says so");
  ok(!c.autoOnClose && !c.openWindow && !c.home[0] && c.homePort == 80, "defaults: off, no home");
  const char* keyed = "user=a\nkey=5F4DCC3B5AA765D61D8327DEB882CF99\nhome=covey.local\n";
  ok(kosyncParseConfig(keyed, strlen(keyed), &c), "key= instead of a password");
  eqStr(c.key, KEY, "lower-cased");
  eqStr(c.home, "covey.local", "a hostname");
  const char* noPw = "user=a\n";
  ok(!kosyncParseConfig(noPw, strlen(noPw), &c) && !c.ok, "no password: off");
  eqStr(c.problem, "no password= line", "and says why");
  const char* noUser = "password=abcdefghijklmnop\n";
  ok(!kosyncParseConfig(noUser, strlen(noUser), &c), "no user: off");
  const char* https = "user=a\npassword=abcdefghijklmnop\nhome=https://x.example\n";
  ok(kosyncParseConfig(https, strlen(https), &c) && !c.home[0], "https refused, no client");
  eqStr(c.problem, "home= must be http:// (no TLS here)", "and says so");
  ok(!kosyncParseConfig("", 0, &c), "empty: off");

  group("/books/kosync.txt: hotspot_pass");
  const char* base = "user=a\npassword=abcdefghijklmnop\n";
  char txt[256];
  ok(kosyncParseConfig(base, strlen(base), &c) && !c.hotspotPass[0] && !c.hotspotNote[0],
     "absent: open, and nothing to say");
  snprintf(txt, sizeof(txt), "%shotspot_pass= trail and lead  \r\n", base);
  ok(kosyncParseConfig(txt, strlen(txt), &c), "valid parses");
  eqStr(c.hotspotPass, "trail and lead", "valid: kept, spaces inside, ends trimmed");
  eqStr(c.hotspotNote, "", "valid: no note");
  eqStr(c.problem, "", "and no effect on the account's own note");
  snprintf(txt, sizeof(txt), "%shotspot_pass=1234567\n", base);
  ok(kosyncParseConfig(txt, strlen(txt), &c) && c.ok, "too short: KOSync still on");
  eqStr(c.hotspotPass, "", "too short (7): OPEN");
  eqStr(c.hotspotNote, "hotspot_pass ignored: 8-63 characters", "too short: says why");
  snprintf(txt, sizeof(txt), "%shotspot_pass=12345678\n", base);
  ok(kosyncParseConfig(txt, strlen(txt), &c) && !strcmp(c.hotspotPass, "12345678"), "8: accepted");
  char p63[64], p64[65];
  memset(p63, 'x', 63); p63[63] = '\0';
  memset(p64, 'x', 64); p64[64] = '\0';
  snprintf(txt, sizeof(txt), "%shotspot_pass=%s\n", base, p63);
  ok(kosyncParseConfig(txt, strlen(txt), &c) && !strcmp(c.hotspotPass, p63), "63: accepted whole");
  snprintf(txt, sizeof(txt), "%shotspot_pass=%s\n", base, p64);
  ok(kosyncParseConfig(txt, strlen(txt), &c) && !c.hotspotPass[0], "too long (64): OPEN");
  eqStr(c.hotspotNote, "hotspot_pass ignored: 8-63 characters", "too long: says why");
  snprintf(txt, sizeof(txt), "%shotspot_pass=caf\xc3\xa9-pass-word\n", base);
  ok(kosyncParseConfig(txt, strlen(txt), &c) && !c.hotspotPass[0], "non-ASCII: OPEN");
  eqStr(c.hotspotNote, "hotspot_pass ignored: plain ASCII only", "non-ASCII: says why");
  snprintf(txt, sizeof(txt), "%shotspot_pass=tab\there-12\n", base);
  ok(kosyncParseConfig(txt, strlen(txt), &c) && !c.hotspotPass[0], "a control character: OPEN");
  snprintf(txt, sizeof(txt), "%shotspot_pass=\n", base);
  ok(kosyncParseConfig(txt, strlen(txt), &c) && !c.hotspotPass[0] && c.hotspotNote[0],
     "empty: open, and said");
  snprintf(txt, sizeof(txt), "%shotspot_pass=good-password\nhotspot_pass=bad\n", base);
  ok(kosyncParseConfig(txt, strlen(txt), &c) && !c.hotspotPass[0] && c.hotspotNote[0],
     "the last line wins, invalid included");

  uint8_t mac[6] = { 0x24, 0x0a, 0xc4, 0x01, 0x02, 0x03 };
  char id1[33], id2[33];
  kosyncDeviceId(mac, id1);
  kosyncDeviceId(mac, id2);
  eqStr(id1, id2, "device id is stable");
  eqInt((long long)strlen(id1), 32, "32 hex");
}

/* The whole inbound chain: a peer's percentage -> (reading chapter, within) -> the reader's
 * spine-equal fraction -> a CBS1 record signed with the local key -> the inbox -> found for
 * this book -> epubLocate. Every inverse vector must land in the chapter the generator names
 * (or, for "the very end of chapter r", at the start of r+1 — the same place), never in an
 * earlier chapter. */
static void testInboundChain() {
  group("inbound: percentage -> inbox -> the card's fraction -> the right chapter");
  uint8_t key[32];
  bookSyncDeriveKey("test-passcode", key);
  for (size_t k = 0; k < sizeof(KS_BOOKS) / sizeof(KS_BOOKS[0]); k++) {
    const KsBook& v = KS_BOOKS[k];
    EpubSource s;
    FILE* f = NULL;
    EpubBook b;
    if (!openBook(v.file, &b, &f, &s)) {
      ok(false, v.file);
      continue;
    }
    char ids[3][EPUB_ID_MAX];
    const int nIds = epubIds(&b, ids);
    const char* idp[3] = { ids[0], ids[1], ids[2] };
    for (int i = 0; i < v.nInv; i++) {
      int r = -1;
      double w = -1;
      epubKosyncLocate(b.kosync, v.inv[i].p, &r, &w, NULL, NULL);
      char text[BOOKSYNC_MESH_TEXT_MAX];
      ok(kosyncParkText(idp, nIds, r, w, b.nSpine, 0, "CrossPoint", key, text, sizeof(text)),
         "packs");
      bookSyncInboxInit();
      ok(bookSyncInboxPush(text, 0, 0), "the inbox takes it");
      BookSyncRecord rec;
      uint32_t from = 1;
      ok(bookSyncInboxFindFor(key, idp, nIds, &rec, &from) >= 0, "found for this book");
      eqStr(rec.dev, "CrossPoint", "the card will name the peer");
      int sp = -1;
      uint32_t off = 0;
      epubLocate(&b, rec.fraction, &sp, &off);
      const size_t len = epubChapterLen(&b, sp);
      char what[128];
      snprintf(what, sizeof(what), "%s p=%.6f -> r=%d w=%.4f, landed %d at byte %u of %u",
               v.file, v.inv[i].p, r, w, sp, (unsigned)off, (unsigned)len);
      /* In BYTES: epubLocate truncates to a whole byte (as it always has), so on a 7-byte
       * chapter a within of 0.975 is byte 6, not 6.825. Never more than a byte early; never
       * more than 1 % of the chapter (+1 byte) late — the half-quantum nudge. */
      const double want = w * (double)len;
      const bool sameChapter = (sp == r && (double)off + 1.0 >= want &&
                                (double)off <= want + 0.01 * (double)len + 1.0);
      const bool nextStart = (w >= 0.999 && sp == r + 1 && (double)off <= 0.01 * (double)len + 1.0);
      const bool lastEnd = (r == b.nSpine - 1 && w >= 0.999 && sp == r);
      ok(sameChapter || nextStart || lastEnd, what);
    }
    epubClose(&b);
    fclose(f);
  }
  bookSyncInboxInit();
}

/* (5) The inbox holds FOUR records for every book and both transports. A peer PUTting
 * over and over, or the same book opened again and again, must replace its own KOSync offer,
 * never push another book's LoRa position out of the far end. */
static void testLedger() {
  group("the park ledger: one KOSync offer per book, other books' positions kept (5)");
  uint8_t key[32];
  bookSyncDeriveKey("test-passcode", key);
  bookSyncInboxInit();
  // Three LoRa positions for three OTHER books, as COVEY would have sent them.
  const char* others[3] = { "id:book-one", "id:book-two", "id:book-three" };
  for (int i = 0; i < 3; i++) {
    BookSyncRecord r;
    const char* ids[1] = { others[i] };
    bookSyncMakeRecord(&r, ids, 1, 2, 100, 0.25 * (i + 1), 1790000000u + i, "COVEY", NULL);
    char text[BOOKSYNC_MESH_TEXT_MAX];
    ok(bookSyncPackMesh(&r, key, text, sizeof(text)), "pack a LoRa record");
    ok(bookSyncInboxPush(text, 0x1234, 0), "park it");
  }
  static KosyncParkLedger L;
  memset(&L, 0, sizeof(L));
  const char* mine[1] = { "id:urn-uuid-kosync-mixed-0001" };
  uint32_t lastId = 0;
  for (int n = 0; n < 10; n++) {
    char text[BOOKSYNC_MESH_TEXT_MAX];
    ok(kosyncParkText(mine, 1, 1, 0.1 * n, 4, 0, "CrossPoint", key, text, sizeof(text)), "pack");
    ok(kosyncParkInto(&L, BYNAME, text, 0.05 * n, 0), "parked");
    eqInt(bookSyncInboxCount(), 4, "three others + ONE for this book, every time");
    lastId = bookSyncInboxIdAt(bookSyncInboxCount() - 1);
  }
  for (int i = 0; i < 3; i++) {
    const char* ids[1] = { others[i] };
    BookSyncRecord r;
    uint32_t from = 0;
    ok(bookSyncInboxFindFor(key, ids, 1, &r, &from) >= 0, "another book's LoRa position survived");
  }
  BookSyncRecord r;
  uint32_t from = 1;
  ok(bookSyncInboxFindFor(key, mine, 1, &r, &from) >= 0, "our book's offer is there");
  eqInt(r.spine, 1, "and it is the newest one");
  double peer = -1;
  ok(kosyncParkedPeerPct(&L, lastId, &peer), "the peer's own percentage is kept");
  eqDbl(peer, 0.45, 1e-12, "for the card");
  ok(!kosyncParkedPeerPct(&L, bookSyncInboxIdAt(0), &peer), "a LoRa record has none");
  // A second book gets its own slot; the first book's offer stays.
  char text[BOOKSYNC_MESH_TEXT_MAX];
  const char* b2[1] = { "ta:plain" };
  ok(kosyncParkText(b2, 1, 0, 0.5, 3, 0, "CrossPoint", key, text, sizeof(text)), "pack book 2");
  ok(kosyncParkInto(&L, "otherbook", text, 0.3, 0), "park book 2");
  ok(bookSyncInboxFindFor(key, mine, 1, &r, &from) >= 0, "book 1's offer still there");
  // The reader took the offer (the card removes it): the next park simply adds.
  bookSyncInboxDropForBook(key, mine, 1);
  ok(kosyncParkText(mine, 1, 2, 0.5, 4, 0, "CrossPoint", key, text, sizeof(text)), "pack");
  ok(kosyncParkInto(&L, BYNAME, text, 0.7, 0), "parks after its predecessor was taken");
  bookSyncInboxInit();
}

/* (7) KOSync's per-book data is OPTIONAL: with every one of its allocations failing, every
 * fixture opens exactly as 0.9.78 opened it, and is simply not syncable. */
static void testAllocFailure() {
  group("KOSync's allocations all fail: every book still opens, identically (7)");
  epubTestFailKosyncAllocs = 1;
  for (size_t k = 0; k < sizeof(GP_BOOKS) / sizeof(GP_BOOKS[0]); k++) {
    const GpBook& g = GP_BOOKS[k];
    EpubSource s;
    FILE* f = NULL;
    if (!openSource(g.file, &s, &f)) {
      ok(false, g.file);
      continue;
    }
    EpubBook b;
    const EpubStatus st = epubOpen(&b, &s, g.file, isTxt(g.file));
    eqInt(st, g.status, g.file);
    eqInt(b.nSpine, g.nSpine, "reading spine length");
    for (int i = 0; i < g.nSpine && i < b.nSpine; i++) {
      eqStr(b.spine[i].name, g.names[i], "reading spine entry");
      eqInt(epubChapterLen(&b, i), g.chapLen[i], "chapter length");
    }
    char ids[3][EPUB_ID_MAX];
    const int nIds = epubIds(&b, ids);
    eqInt(nIds, g.nIds, "ids");
    ok(b.kosync == NULL && epubKosyncTotal(b.kosync) == 0, "not syncable");
    double p = -1;
    ok(!epubKosyncPercentAt(&b, 0, 0, 10, &p), "no percentage is invented");
    char id[33];
    ok(!epubKosyncPartialMd5(&s, id), "no partial id without its buffer");
    if (st == EPUB_OK) {
      epubClose(&b);
    }
    fclose(f);
  }
  epubTestFailKosyncAllocs = 0;
}

// ================================================================ 3. NOBODY'S PLACE MOVES
static void testGolden() {
  group("before/after: every fixture reads and locates exactly as 0.9.78 did");
  for (size_t k = 0; k < sizeof(GP_BOOKS) / sizeof(GP_BOOKS[0]); k++) {
    const GpBook& g = GP_BOOKS[k];
    EpubSource s;
    FILE* f = NULL;
    if (!openSource(g.file, &s, &f)) {
      ok(false, g.file);
      continue;
    }
    EpubBook b;
    const EpubStatus st = epubOpen(&b, &s, g.file, isTxt(g.file));
    eqInt(st, g.status, g.file);
    eqInt(b.nSpine, g.nSpine, "reading spine length");
    for (int i = 0; i < g.nSpine && i < b.nSpine; i++) {
      eqStr(b.spine[i].name, g.names[i], "reading spine entry");
      eqInt(epubChapterLen(&b, i), g.chapLen[i], "chapter length");
    }
    char ids[3][EPUB_ID_MAX];
    const int nIds = epubIds(&b, ids);
    eqInt(nIds, g.nIds, "id count");
    for (int i = 0; i < nIds && i < g.nIds; i++) {
      eqStr(ids[i], g.ids[i], "id");
    }
    for (int i = 0; i < g.nFr; i++) {
      char w[80];
      snprintf(w, sizeof(w), "epubFraction(%d, %u)", g.fr[i].spine, (unsigned)g.fr[i].offset);
      eqDbl(epubFraction(&b, g.fr[i].spine, g.fr[i].offset), g.fr[i].fraction, 0.0, w);
    }
    for (int i = 0; i < g.nLoc; i++) {
      int sp = -1;
      uint32_t off = 0;
      epubLocate(&b, g.loc[i].fraction, &sp, &off);
      char w[80];
      snprintf(w, sizeof(w), "epubLocate(%.6f)", g.loc[i].fraction);
      eqInt(sp, g.loc[i].spine, w);
      eqInt(off, g.loc[i].offset, w);
    }
    if (st == EPUB_OK) {
      epubClose(&b);
    }
    fclose(f);
  }

  group("before/after: a 0.9.78 positions file loads, lands and re-saves unchanged");
  static BookStore store;
  store.init();
  ok(store.parse(GP_STORE_BLOB, strlen(GP_STORE_BLOB)), "parses");
  for (size_t k = 0; k < sizeof(GP_BOOKS) / sizeof(GP_BOOKS[0]); k++) {
    const GpBook& g = GP_BOOKS[k];
    if (g.nIds <= 0) {
      continue;
    }
    // Look it up the way openBook() does: by the ids the CURRENT parser derives.
    EpubSource s;
    FILE* f = NULL;
    EpubBook b;
    if (!openBook(g.file, &b, &f, &s)) {
      ok(false, g.file);
      continue;
    }
    char ids[3][EPUB_ID_MAX];
    const int nIds = epubIds(&b, ids);
    const char* idp[3] = { ids[0], ids[1], ids[2] };
    BookPos pos;
    char w[96];
    snprintf(w, sizeof(w), "%s: its saved place is found", g.file);
    ok(store.get(idp, nIds, &pos), w);
    eqInt(pos.spine, g.savedSpine, "saved spine");
    eqInt(pos.offset, g.savedOffset, "saved offset");
    char six[32];
    snprintf(six, sizeof(six), "%.6f", g.savedFraction);     // the store keeps 6 decimals
    eqDbl(pos.fraction, strtod(six, NULL), 0.0, "saved fraction");
    // openBook() keeps the place only if the spine is in range: it must still be.
    ok((int)pos.spine < b.nSpine, "the saved chapter still exists");
    // ...and loadChapter() + gotoOffset() land on the same text: same chapter length.
    eqInt(epubChapterLen(&b, (int)pos.spine), g.chapLen[pos.spine], "the same chapter text");
    epubClose(&b);
    fclose(f);
  }
  static char again[BOOKSTORE_BLOB_MAX];
  const size_t n = store.serialise(again, sizeof(again));
  ok(n == strlen(GP_STORE_BLOB) && !memcmp(again, GP_STORE_BLOB, n), "re-saved byte for byte");
}

int main() {
  testMd5();
  testDocIds();
  testShortRead();
  testSpineAndPercent();
  testTextAndUnsyncable();
  testOversized();
  testPaths();
  testFormat();
  testParse();
  testHeaders();
  testRequests();
  testServe();
  testClock();
  testConfig();
  testInboundChain();
  testLedger();
  testAllocFailure();
  testGolden();
  printf("\n%s%d passed, %d failed\033[0m\n", g_fail ? "\033[31m" : "\033[32m", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
