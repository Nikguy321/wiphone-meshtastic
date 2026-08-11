/*
 * test_bookstore.cpp — the position store: id-overlap matching, no-forking, durability.
 *
 * The store has no wire format to match, so these are behaviour tests rather than interop
 * vectors. They target the two things that lose someone's place silently: a lookup that
 * misses because the position arrived under a different id, and the same book ending up
 * stored twice with two different positions.
 */
#include "../WiPhone/bookstore.h"

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

static void eqU32(unsigned long got, unsigned long want, const char* what) {
  if (got == want) {
    g_pass++;
    return;
  }
  g_fail++;
  printf("  \033[31mFAIL\033[0m %s :: %s  want=%lu got=%lu\n", g_group, what, want, got);
}

// ---------------------------------------------------------------- a file-backed io
struct FileCtx { const char* path; };

static bool fileLoad(void* ctx, char* buf, size_t cap, size_t* outLen) {
  FILE* f = fopen(((FileCtx*)ctx)->path, "rb");
  if (!f) {
    return false;
  }
  size_t n = fread(buf, 1, cap - 1, f);
  fclose(f);
  buf[n] = '\0';
  *outLen = n;
  return true;
}

static bool fileStore(void* ctx, const char* buf, size_t len) {
  // The same temp+rename the SD implementation must do: a torn write costs the last
  // session, not the whole library.
  const char* path = ((FileCtx*)ctx)->path;
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE* f = fopen(tmp, "wb");
  if (!f) {
    return false;
  }
  bool ok = fwrite(buf, 1, len, f) == len;
  fflush(f);
  fclose(f);
  if (!ok) {
    remove(tmp);
    return false;
  }
  return rename(tmp, path) == 0;
}

// ---------------------------------------------------------------- tests
static void testPutGet() {
  group("put / get");
  BookStore s;
  s.init();
  const char* a[] = { "id:pg-1342", "ta:pride-and-prejudice-jane-austen", "fp:aaaa000011112222" };
  s.put(a, 3, 7, 4213, 0.3125, 1754870400);
  eqU32((unsigned long)s.count, 1, "one book stored");
  ok(s.dirty, "put marks dirty");

  BookPos got;
  ok(s.get(a, 3, &got), "found by full id list");
  eqU32(got.spine, 7, "spine");
  eqU32(got.offset, 4213, "offset");
  eqU32(got.turnedAt, 1754870400, "turnedAt");
  ok(got.fraction > 0.3124 && got.fraction < 0.3126, "fraction");

  const char* other[] = { "id:something-else", "fp:ffff" };
  ok(!s.get(other, 2, &got), "unrelated book not found");
}

static void testAnyOverlap() {
  group("lookup succeeds on ANY id — the mesh case");
  BookStore s;
  s.init();
  const char* full[] = { "id:pg-1342", "ta:pride-and-prejudice-jane-austen", "fp:aaaa000011112222" };
  s.put(full, 3, 3, 100, 0.25, 1000);

  // A mesh packet carries only the FIRST id. That must still find this entry.
  const char* justFirst[] = { "id:pg-1342" };
  BookPos got;
  ok(s.get(justFirst, 1, &got), "found by the primary id alone");
  eqU32(got.spine, 3, "same record");

  // A different copy of the same book: same title/author, different file, so a different
  // fingerprint and possibly no dc:identifier at all.
  const char* otherCopy[] = { "ta:pride-and-prejudice-jane-austen", "fp:bbbb333344445555" };
  ok(s.get(otherCopy, 2, &got), "found by the title/author id");

  // Overlap on the fingerprint only.
  const char* byFp[] = { "fp:aaaa000011112222" };
  ok(s.get(byFp, 1, &got), "found by the fingerprint alone");

  const char* empty[] = { "" };
  ok(!s.get(empty, 1, &got), "an empty id never matches");
}

static void testNoFork() {
  group("one book cannot hold two positions");
  BookStore s;
  s.init();
  // Stored first under its full id set...
  const char* full[] = { "id:pg-1342", "ta:pride-and-prejudice", "fp:aaaa" };
  s.put(full, 3, 1, 10, 0.1, 1000);
  // ...then written again under a DIFFERENT primary id that still overlaps. Without the
  // drop-on-overlap rule this creates a second entry and the two diverge forever.
  const char* alt[] = { "ta:pride-and-prejudice", "fp:cccc" };
  s.put(alt, 2, 5, 500, 0.5, 2000);

  eqU32((unsigned long)s.count, 1, "still one entry");
  BookPos got;
  ok(s.get(full, 3, &got), "old id set still finds it");
  eqU32(got.spine, 5, "and gets the NEWER position");
  eqU32(got.turnedAt, 2000, "newer turnedAt");

  // An entry overlapping two existing ones must collapse both, not just the first.
  BookStore t;
  t.init();
  const char* x[] = { "id:x", "fp:1111" };
  const char* y[] = { "id:y", "fp:2222" };
  t.put(x, 2, 1, 1, 0.1, 100);
  t.put(y, 2, 2, 2, 0.2, 200);
  eqU32((unsigned long)t.count, 2, "two distinct books");
  const char* both[] = { "fp:1111", "fp:2222" };
  t.put(both, 2, 9, 9, 0.9, 300);
  eqU32((unsigned long)t.count, 1, "an entry overlapping both collapses both");
}

static void testRemove() {
  group("remove");
  BookStore s;
  s.init();
  const char* a[] = { "id:a", "fp:1111" };
  const char* b[] = { "id:b", "fp:2222" };
  s.put(a, 2, 1, 1, 0.1, 100);
  s.put(b, 2, 2, 2, 0.2, 200);
  const char* byFp[] = { "fp:1111" };
  ok(s.remove(byFp, 1), "removed by a secondary id");
  eqU32((unsigned long)s.count, 1, "one left");
  ok(!s.get(a, 2, NULL), "gone");
  ok(s.get(b, 2, NULL), "the other survives");
  ok(!s.remove(byFp, 1), "removing again reports nothing removed");
}

static void testRoundTrip() {
  group("serialise / parse round trip");
  BookStore s;
  s.init();
  const char* a[] = { "id:pg-1342", "ta:pride-and-prejudice-jane-austen", "fp:aaaa000011112222" };
  const char* b[] = { "ta:my-notes", "fp:bbbb111122223333" };
  s.put(a, 3, 7, 4213, 0.312500, 1754870400);
  s.put(b, 2, 0, 987, 0.5, 1754870401);

  char blob[BOOKSTORE_BLOB_MAX];
  size_t n = s.serialise(blob, sizeof(blob));
  ok(n > 0, "serialised");

  BookStore t;
  t.init();
  ok(t.parse(blob, n), "parsed");
  eqU32((unsigned long)t.count, 2, "two books back");
  BookPos got;
  ok(t.get(a, 3, &got), "first found");
  eqU32(got.spine, 7, "spine survived");
  eqU32(got.offset, 4213, "offset survived");
  eqU32(got.turnedAt, 1754870400, "turnedAt survived");
  ok(got.fraction > 0.31249 && got.fraction < 0.31251, "fraction survived");
  eqU32(got.nIds, 3, "all three ids survived");
  ok(t.get(b, 2, &got), "second found");
  eqU32(got.nIds, 2, "two ids survived");
}

static void testCorruption() {
  group("a damaged file loses as little as possible");
  BookStore s;
  s.init();
  ok(!s.parse("", 0), "empty file rejected");
  ok(!s.parse("garbage not our format\n", 23), "foreign file rejected, store left clean");
  eqU32((unsigned long)s.count, 0, "nothing loaded from a foreign file");

  // A torn final line must cost that record only — the earlier ones still load.
  const char* torn = "CBSTORE1\n"
                     "1\t10\t0.100000\t1000\tid:a,fp:1111\n"
                     "2\t20\t0.200000\t2000\tid:b,fp:2222\n"
                     "3\t30\t0.3000";
  ok(s.parse(torn, strlen(torn)), "parsed despite a torn tail");
  eqU32((unsigned long)s.count, 2, "the two complete records survived");

  // A record with no ids is unreachable forever, so it is dropped rather than kept.
  const char* noIds = "CBSTORE1\n1\t10\t0.100000\t1000\t\n2\t20\t0.200000\t2000\tid:b\n";
  ok(s.parse(noIds, strlen(noIds)), "parsed");
  eqU32((unsigned long)s.count, 1, "the id-less record was dropped");
}

static void testPersistence() {
  group("load / save through a file");
  const char* path = "tests/fixtures/_store.tsv";
  remove(path);
  FileCtx ctx = { path };
  BookStoreIo io = { &ctx, fileLoad, fileStore };

  BookStore s;
  s.init();
  ok(!s.load(&io), "missing file is not an error, just an empty store");
  eqU32((unsigned long)s.count, 0, "empty");

  const char* a[] = { "id:pg-1342", "fp:aaaa" };
  s.put(a, 2, 7, 4213, 0.3125, 1754870400);
  ok(s.dirty, "dirty after put");
  ok(s.saveIfDirty(&io), "saved");
  ok(!s.dirty, "clean after save");
  ok(s.saveIfDirty(&io), "saveIfDirty is a no-op when clean");

  BookStore t;
  t.init();
  ok(t.load(&io), "loaded from disk");
  BookPos got;
  ok(t.get(a, 2, &got), "found after reload");
  eqU32(got.spine, 7, "spine persisted");
  eqU32(got.offset, 4213, "offset persisted");
  remove(path);
}

static void testEviction() {
  group("a full store evicts the least recently read");
  BookStore s;
  s.init();
  for (int i = 0; i < BOOKSTORE_MAX_BOOKS; i++) {
    char id[64];
    snprintf(id, sizeof(id), "id:book-%03d", i);
    const char* ids[] = { id };
    s.put(ids, 1, (uint32_t)i, 0, 0.0, (uint32_t)(1000 + i));   // book-000 is the oldest
  }
  eqU32((unsigned long)s.count, BOOKSTORE_MAX_BOOKS, "full");

  const char* fresh[] = { "id:brand-new" };
  s.put(fresh, 1, 99, 0, 0.0, 9999);
  eqU32((unsigned long)s.count, BOOKSTORE_MAX_BOOKS, "still at capacity");
  ok(s.get(fresh, 1, NULL), "the new book is in");

  const char* oldest[] = { "id:book-000" };
  ok(!s.get(oldest, 1, NULL), "the least recently read one was evicted");
  const char* newest[] = { "id:book-047" };
  ok(s.get(newest, 1, NULL), "the most recently read one survived");
}

int main() {
  printf("\033[1mbookstore — reading positions\033[0m\n");
  testPutGet();
  testAnyOverlap();
  testNoFork();
  testRemove();
  testRoundTrip();
  testCorruption();
  testPersistence();
  testEviction();
  printf("\n%s%d passed, %d failed\033[0m\n",
         g_fail ? "\033[31m" : "\033[32m", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
