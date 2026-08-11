/*
 * epub_parse.h — EPUB and plain-text parsing, matching COVEY's covey_ui/epub.py.
 *
 * An EPUB is a zip of XHTML. This reads it straight off the SD card: central directory,
 * inflate (ESP32 ROM miniz on the phone, zlib in the host tests), then a deliberately dumb
 * XHTML-to-text pass. No layout, no CSS — a 240x320 panel re-wraps everything anyway.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * POSITION IS (spine, offset), NEVER A PAGE NUMBER
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * A page number is a property of THIS screen at THIS font size. COVEY's 480x800 panel and
 * the WiPhone's are not the same, so a page number means nothing across devices — which is
 * the entire problem booksync exists to solve. The portable unit is the spine index plus a
 * character offset into that spine item's extracted text, both of which come out of the
 * file's own structure.
 *
 * ⚠ The offset is only exactly comparable between readers that flatten XHTML identically.
 * Across implementations it drifts by the length of whatever markup one keeps and the other
 * drops — a few characters per paragraph, and more here than on COVEY because this parser
 * decodes a smaller set of HTML entities (see EPUB_ENTITY_NOTE in the .cpp). That drift is
 * expected and handled: booksync also carries the whole-book FRACTION, and epubLocate()
 * turns a fraction back into a position, which is what makes a jump from COVEY land
 * somewhere sane. It is also why a jump is confirmed by the reader rather than taken
 * silently.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * IDENTITY — this is what makes sync match, so it must agree with COVEY exactly
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * Three ids, best first, and ANY overlap means "same book":
 *   "id:<slug of dc:identifier>"   absent from plenty of files
 *   "ta:<slug of title|author>"    matches the same book from two different sources
 *   "fp:<content fingerprint>"     matches only a byte-identical copy
 *
 * The filename is NOT an id for an EPUB. It is for a .txt, where the title comes from the
 * filename stem and there is no dc:identifier.
 *
 * Only the FIRST id travels over LoRa, so byte-identical copies on both devices is the
 * happy path — it makes all three agree at once.
 */
#ifndef EPUB_PARSE_H
#define EPUB_PARSE_H

#include <stddef.h>
#include <stdint.h>

#define EPUB_NAME_MAX      192      // a zip entry path
#define EPUB_META_MAX      512      // 120 characters of UTF-8 metadata, with room
#define EPUB_ID_MAX        72       // "ta:" + a 64-character slug
#define EPUB_CH_TITLE_MAX  84       // COVEY caps chapter titles at 80 characters
#define EPUB_MAX_SPINE     512
#define EPUB_MAX_DOC       (512 * 1024)   // per spine document, decompressed (COVEY: 4 MB)

/* Random-access bytes. Backed by an SD File on the phone and by a FILE* in the tests, which
 * is what lets the same parser be proven on a Mac. */
struct EpubSource {
  void*    ctx;
  uint64_t size;
  size_t (*read)(void* ctx, uint64_t off, void* buf, size_t len);
};

struct EpubSpineItem {
  char name[EPUB_NAME_MAX];              // zip entry path, already resolved against the OPF
  char title[EPUB_CH_TITLE_MAX];         // "" when the book has no usable table of contents
};

struct EpubBook {
  EpubSource*     src;
  bool            isText;                // a plain .txt "book": one spine item, no zip
  char            title[EPUB_META_MAX];
  char            author[EPUB_META_MAX];
  char            identifier[EPUB_META_MAX];
  EpubSpineItem*  spine;
  int             nSpine;
  char            fingerprint[17];       // 16 hex characters, or "" if unreadable
};

enum EpubStatus {
  EPUB_OK = 0,
  EPUB_ERR_NOT_ZIP,
  EPUB_ERR_NO_CONTAINER,
  EPUB_ERR_NO_ROOTFILE,
  EPUB_ERR_NO_OPF,
  EPUB_ERR_NO_CHAPTERS,
  EPUB_ERR_MEMORY,
};

/* Open `src` as a book. `displayName` is the file's basename, used for the title of a .txt
 * and as the fallback title of an EPUB with no dc:title — pass it, not a path.
 * `isTextFile` selects the plain-text path (caller checks the extension). */
EpubStatus epubOpen(EpubBook* b, EpubSource* src, const char* displayName, bool isTextFile);
void       epubClose(EpubBook* b);
const char* epubStatusText(EpubStatus s);

/* The ids another device might match this book by, best first. Writes up to 3 into `out`,
 * returns how many. Each buffer must be EPUB_ID_MAX. */
int epubIds(const EpubBook* b, char out[3][EPUB_ID_MAX]);

// Extracted text of spine item `i`. Writes into `buf`; returns the length, or 0.
// Never fails loudly: an unreadable chapter yields a short placeholder, as on COVEY.
size_t epubChapterText(EpubBook* b, int i, char* buf, size_t cap);

// Length of a chapter's extracted text without keeping it — for fraction()/locate().
size_t epubChapterLen(EpubBook* b, int i);

// 0.0-1.0 through the whole book. Costs ONE chapter extraction, not the whole book.
double epubFraction(EpubBook* b, int spine, uint32_t offset);

// Inverse of epubFraction: the (spine, offset) nearest a whole-book fraction. This is what
// makes a position from a foreign reader land somewhere sane.
void epubLocate(EpubBook* b, double fraction, int* spineOut, uint32_t* offsetOut);

// The chapter's title, or "Chapter N".
const char* epubChapterTitle(const EpubBook* b, int i, char* buf, size_t cap);

// ---------------------------------------------------------------- exposed for tests
// COVEY's _slug: lowercase, runs of non-[a-z0-9] become '-', trimmed, cut to 64 characters.
size_t epubSlug(const char* in, char* out, size_t cap);

// COVEY's _fingerprint: sha1(str(size) + first 64 KB + last 64 KB), first 16 hex characters.
bool epubFingerprint(EpubSource* src, char out[17]);

// COVEY's _TextExtractor: XHTML in, paragraphs joined by blank lines out.
size_t epubExtractText(const char* html, size_t htmlLen, char* out, size_t cap);

// COVEY's _norm: resolve `href` against the OPF's directory (the trap that yields a book
// with zero chapters and no error when it is skipped).
size_t epubNormPath(const char* base, const char* href, char* out, size_t cap);

#endif // EPUB_PARSE_H
