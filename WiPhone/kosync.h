/*
 * kosync.h — the KOSync protocol (KOReader's reading-position sync), the pure half.
 *
 * KOSync is what KOReader, the Xteink X4's CrossPoint firmware and COVEY all speak: a tiny
 * HTTP+JSON protocol that moves ONE number per book — a whole-book percentage — keyed by a
 * document id. On the WiPhone it rides beside LoRa booksync, never instead of it:
 *
 *   the WINDOW  the phone serves KOSync for one book for a few minutes, on its own hotspot
 *               'WiPhone-Books' (192.168.4.1) in the woods, or on its WiFi address at home.
 *               An X4 joins and syncs against it. See kosync_sync.h.
 *   the CLIENT  at home, the phone pushes/pulls its place to/from the configured server
 *               (COVEY) over the WiFi it is already on. It never joins anything.
 *
 * Everything here is plain C++ with no Arduino headers — formatting, JSON, headers, the
 * server's routing and answers, the window's clock, the config file — so tests/test_kosync.cpp
 * compiles these exact bytes on a Mac. What CANNOT be tested there (sockets, the radio, the
 * card) lives in kosync_sync.cpp and is kept as thin as it can be.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * THE WIRE (contract §1 + §7, pinned by tools/gen_kosync_vectors.py)
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 *   GET  /users/auth                 200 {"authorized":"OK"} | 401
 *   GET  /syncs/progress/<document>  200 {document,percentage,progress,device,device_id,
 *                                    timestamp} | 200 {} for a document we do not know | 401
 *   PUT  /syncs/progress             body {document,progress,percentage,device,device_id}
 *                                    -> 200 {"document":..,"timestamp":<server unix s>} | 401
 *   GET  /healthcheck                200 {"state":"OK"} (no auth)
 *   POST /users/create               402 (registration is off: one user, from the config file)
 *
 *   Clients send x-auth-user: <user> and x-auth-key: <MD5(password), lower-case hex>; servers
 *   check those two and nothing else. Our clients always send Accept:
 *   application/vnd.koreader.v1+json (and Content-Type: application/json on a PUT); our
 *   server never requires it.
 *
 * 🛑 AN UNKNOWN DOCUMENT IS `200 {}`, NOT A 404 (contract §7.1 — the reference server's own
 *    answer; KOReader treats anything else as an error). CrossPoint reads {} as "0 %, nobody
 *    has synced this yet", which is exactly true.
 * ⚠ `progress` (KOReader's XPath) is sent EMPTY and ignored on read: this reader has no
 *    XPath to give, and faking one would move a KOReader device to the wrong place. A
 *    KOReader device cannot be MOVED by a percentage-only record; it can still send to us.
 */
#ifndef KOSYNC_H
#define KOSYNC_H

#include <stddef.h>
#include <stdint.h>

#define KOSYNC_ACCEPT          "application/vnd.koreader.v1+json"
#define KOSYNC_EPSILON         0.001      // |Δp| at or under this is "already synchronized"
#define KOSYNC_DOC_MAX         72         // a document id we will echo (ours are 32 hex)
#define KOSYNC_DEV_MAX         48         // a device name / device id from a peer
#define KOSYNC_USER_MAX        64
#define KOSYNC_KEY_CHARS       33         // MD5 hex + NUL
#define KOSYNC_HOST_MAX        64

// ---------------------------------------------------------------- numbers
/* A percentage as a JSON number: at most 6 decimals, trailing zeros and a bare point
 * stripped ("0", "1", "0.5", "0.231"), clamped to [0, 1]. Returns the length. */
size_t kosyncFormatPct(double p, char* out, size_t cap);

// ---------------------------------------------------------------- JSON
struct KosyncProgress {
  bool    hasPct;                    // false for `{}` — nobody has synced this document
  double  pct;
  bool    hasTimestamp;
  int64_t timestamp;                 // the SERVER's receive time, unix seconds
  char    document[KOSYNC_DOC_MAX];
  char    device[KOSYNC_DEV_MAX];
  char    deviceId[KOSYNC_DEV_MAX];
};

/* Read a progress object (a GET's answer, or a PUT's body). Any key order, any whitespace,
 * any JSON number (exponent form included), unknown keys and nested values skipped, string
 * escapes (\uXXXX too) decoded. False only when `body` is not a JSON object at all. */
bool kosyncParseProgress(const char* body, size_t len, KosyncProgress* out);

// JSON-escape `in` into `out` (no surrounding quotes). Returns the length.
size_t kosyncJsonEscape(const char* in, char* out, size_t cap);

// ---------------------------------------------------------------- HTTP
/* A header's value from a raw request/response header block (first line skipped, names
 * matched case-insensitively, surrounding whitespace trimmed). False when absent. */
bool kosyncHeader(const char* hdrs, const char* name, char* out, size_t cap);

/* x-auth-user and x-auth-key against the configured pair. The key is compared EXACTLY (it is
 * already lower-case hex on every client that exists) and in constant time. An empty
 * configured user or key authorises nobody. */
bool kosyncAuthOk(const char* hdrs, const char* user, const char* keyHex);

// "HTTP/1.1 200 OK" -> 200; anything else -> -1.
int kosyncStatusCode(const char* statusLine);

/* Undo `Transfer-Encoding: chunked` in place (a home server is free to answer that way even
 * to a Connection: close request). Returns the decoded length, or -1 when it is not valid
 * chunked framing. Chunk extensions are ignored; trailers are dropped. */
long kosyncDechunk(char* buf, size_t len);

/* The request bytes the client sends, headers per contract §7.4. `port` 80 is left off the
 * Host header, as every client does. Return the length, or 0 if it did not fit. */
size_t kosyncBuildGet(char* out, size_t cap, const char* host, uint16_t port,
                      const char* user, const char* key, const char* doc);
size_t kosyncBuildPut(char* out, size_t cap, const char* host, uint16_t port,
                      const char* user, const char* key, const char* body);
size_t kosyncBuildPutBody(char* out, size_t cap, const char* doc, double pct,
                          const char* device, const char* deviceId);

// ---------------------------------------------------------------- the server
enum KosyncRoute {
  KOSYNC_ROUTE_NONE = 0,
  KOSYNC_ROUTE_HEALTH,
  KOSYNC_ROUTE_AUTH,
  KOSYNC_ROUTE_CREATE,
  KOSYNC_ROUTE_GET_PROGRESS,
  KOSYNC_ROUTE_PUT_PROGRESS,
};

// Does this path belong to KOSync at all? (/users/..., /syncs/..., /healthcheck)
bool kosyncIsPath(const char* path);

// Which route, and for GET_PROGRESS the document id from the path.
KosyncRoute kosyncRoute(const char* method, const char* path, char* doc, size_t cap);

/* The ONE book a window serves, as the phone sees it right now. Both ids answer: the partial
 * MD5 (KOReader's and CrossPoint's "binary" matcher) and the MD5 of the file name
 * (CrossPoint's default). */
struct KosyncServed {
  const char* partial;
  const char* byName;
  bool        pctOk;                 // false: this place has no KOSync percentage -> `{}`
  double      pct;                   // the phone's place, as a KOSync percentage
  const char* device;                // what the phone calls itself ("WiPhone-NICK")
  const char* deviceId;
  uint32_t    turnedAt;              // the phone's last page turn, UTC, or 0 when unknown
};

struct KosyncServeOut {
  int    code;
  bool   pickedUp;                   // a GET or PUT of the window's book: start the grace
  bool   gotPut;                     // a PUT of the window's book arrived
  bool   park;                       // ...and is worth offering (another device, a real move)
  double putPct;
  char   putDevice[KOSYNC_DEV_MAX];
  char   putDeviceId[KOSYNC_DEV_MAX];
  bool   otherDoc;                   // a PUT for some other book: answered 200, discarded
};

/* Answer one request. `reply` receives the JSON body. `nowUnix` is 0 when the phone has no
 * clock, and is then what `timestamp` says (the receivers treat 0 as "unknown").
 * 🛑 A PUT for another document is answered 200 and DISCARDED: the window is about one book,
 * and the X4 must not see an error for a book the phone simply was not asked about. */
void kosyncServe(const char* method, const char* path, const char* hdrs,
                 const char* body, size_t bodyLen,
                 const char* user, const char* key, const KosyncServed* book,
                 uint32_t nowUnix, KosyncServeOut* out, char* reply, size_t replyCap);

/* Is a peer's place worth a card? Not when it is our own record coming back (same device
 * name or same device id), and not when it is within KOSYNC_EPSILON of where we are. */
bool kosyncWorthParking(double theirs, const char* theirDevice, const char* theirDeviceId,
                        double mine, const char* myDevice, const char* myDeviceId);
// Our own record coming back (same device name, or same device id).
bool kosyncIsOwnRecord(const char* theirDevice, const char* theirDeviceId,
                       const char* myDevice, const char* myDeviceId);

/* For the home PULL on book open, which runs on its own every time a book is opened: is the
 * server's record NEWS worth a card? Newer than our last page turn when both clocks are known
 * (the server's timestamp is its own receive time, UTC, and ours is NTP's); otherwise AHEAD
 * of us by more than KOSYNC_EPSILON — the contract's "compare by percentage when either
 * side's time is unknown". Without this, an X4 that simply had not synced lately would offer
 * the same BACKWARD jump on every open. (A live window offers both directions: someone is
 * syncing on purpose, and the card says "BACKWARDS" in red.) */
bool kosyncPullIsNews(double theirs, bool theirHasTs, int64_t theirTs,
                      double mine, bool mineOk, uint32_t myTurnedAt);

// ---------------------------------------------------------------- the window's clock
/* A hard millis() deadline plus the picked-up grace. Pure so the wrap and the ordering can
 * be proven on a Mac: `now` is whatever millis() said. */
#define KOSYNC_WINDOW_SYNC_MS   (300UL * 1000UL)   // "Sync my place" / close
#define KOSYNC_WINDOW_OPEN_MS   (60UL * 1000UL)    // opening a book while off WiFi
/* 🛑 TWO GRACES, and the GET's is the LONG one. A stock CrossPoint GETs the place, then loads
 * and maps the book, and in ASK mode (or for a record it has never seen) waits for a HUMAN to
 * choose before it PUTs anything. A 10 s grace from the GET closed the hotspot under that
 * reader while it was still deciding — re-arming on the next request cannot help when the
 * next request is exactly what the close prevents. Only a PUT (the peer has said its piece)
 * earns the short close. */
#define KOSYNC_GRACE_GET_MS     (120UL * 1000UL)   // after a GET for the book
#define KOSYNC_GRACE_PUT_MS     (10UL * 1000UL)    // after a PUT for the book

struct KosyncWindowClock {
  bool     open;
  uint32_t openedMs;
  uint32_t durationMs;
  bool     picked;
  uint32_t pickedMs;                 // the LAST request for the book
  uint32_t graceMs;                  // ...and the grace IT armed (GET long, PUT short)
};

enum KosyncClockState {
  KOSYNC_CLOCK_RUNNING = 0,
  KOSYNC_CLOCK_DEADLINE,             // the hard deadline passed
  KOSYNC_CLOCK_PICKED,               // picked up, and the grace is over
  KOSYNC_CLOCK_CLOSED,               // was never open
};

void     kosyncClockOpen(KosyncWindowClock* c, uint32_t now, uint32_t durationMs);
/* The LAST request for the book decides: a GET arms KOSYNC_GRACE_GET_MS, a PUT
 * KOSYNC_GRACE_PUT_MS, each counted from that request and never past the hard deadline. */
void     kosyncClockPicked(KosyncWindowClock* c, uint32_t now, uint32_t graceMs);
int      kosyncClockDue(const KosyncWindowClock* c, uint32_t now);
uint32_t kosyncClockRemainingMs(const KosyncWindowClock* c, uint32_t now);

// ---------------------------------------------------------------- /books/kosync.txt
/* key=value lines, '#' comments:
 *   user=<name>            the ONE KOSync account shared by all your devices
 *   password=<secret>      stored only as its MD5 (what goes on the wire); or key=<32 hex>
 *   home=<host or IP>[:port]   the server at home (COVEY: port 8088); http only, no https
 *   device=<name>          what this phone calls itself (default "WiPhone-<device name>")
 *   auto=on|off            open a window (and push home) when a book is closed
 *   open_window=on|off     open a 60 s window when a book is opened while off WiFi
 *   hotspot_pass=<8-63 printable ASCII>   the window's own hotspot is WPA2 with this
 *                          password; absent (or invalid — and then SAID so) = open
 * A missing file means the feature is OFF and LoRa booksync is exactly as before. */
struct KosyncConfig {
  bool     ok;                       // user and a password/key: the feature is on
  char     user[KOSYNC_USER_MAX];
  char     key[KOSYNC_KEY_CHARS];    // MD5(password) — the plaintext is never kept
  bool     passwordShort;            // under 12 characters: shown as a warning
  char     home[KOSYNC_HOST_MAX];    // "" = no client
  uint16_t homePort;
  char     device[40];               // "" = let the caller default it
  bool     autoOnClose;
  bool     openWindow;
  char     problem[64];              // why it is not ok, or a warning; "" when all is well
  /* 🛑 NEVER PRINTED. The one secret here that must be kept in the clear (it is what
   * softAP() takes); shown only as "hotspot: WPA2". "" = the hotspot is open. */
  char     hotspotPass[64];
  char     hotspotNote[48];          // why a hotspot_pass line was ignored; "" otherwise
};

/* A WPA2 passphrase the ESP32's softAP will take: 8-63 characters, printable ASCII (0x20-0x7E).
 * On failure `why` says which rule, in words fit for the Sync settings screen. */
bool kosyncHotspotPassValid(const char* v, const char** why);

bool kosyncParseConfig(const char* text, size_t len, KosyncConfig* out);

// A stable per-device id from the MAC: MD5("WiPhone-" + 12 hex digits), lower-case hex.
void kosyncDeviceId(const uint8_t mac[6], char out[KOSYNC_KEY_CHARS]);

// ---------------------------------------------------------------- into the booksync inbox
/* A peer's place, already converted to (reading chapter, how far through it), as the
 * reader's own SPINE-EQUAL fraction — the number the sync card, applyPending() and
 * epubLocate() all work in. Nothing downstream of the inbox knows KOSync exists.
 *
 * ⚠ Nudged HALF A QUANTUM FORWARD (0.5/65535 of the book). The record is rounded to 6 dp and
 * then quantised UP to 1/65535ths on its way through the CBS1 packer (booksync.cpp), and a
 * chapter START (within 0 — every "fell in the cover, go to the next chapter" case) that the
 * 6 dp rounding nudged below the boundary would locate to the END OF THE PREVIOUS CHAPTER.
 * Half a quantum forward is a few characters at most and keeps every landing at or after the
 * true point; tests/test_kosync.cpp runs every inverse vector through the whole chain. */
double kosyncToReaderFraction(int readIdx, double readWithin, int nRead);

/* The CBS1 text that parks a peer's place in the booksync inbox so the EXISTING card raises
 * ("CrossPoint says: they are at 61%"), with its undo, arming and backward warning. Signed
 * with the LOCAL booksync key (bookSyncDeriveKey of this phone's passcode) — it never goes on
 * the air; it only has to verify here. `device` is the peer's name, as the card shows it.
 * `ids` are the book's booksync ids (epubIds), which is what the inbox matches on. */
bool kosyncParkText(const char* const* ids, int nIds, int readIdx, double readWithin, int nRead,
                    uint32_t turnedAt, const char* device, const uint8_t key[32],
                    char* out, size_t cap);

/* 🛑 ONE KOSync OFFER PER BOOK IN THE INBOX. The inbox holds FOUR records for every book and
 * both transports, and each parked KOSync record carries a fresh random nonce — so a peer
 * PUTting on every page turn, or a reader opening the same book five times, would push
 * OTHER books' LoRa positions out the far end. The ledger remembers which inbox record each
 * KOSync park became, and a new park for the same book REPLACES it. It also keeps the peer's
 * own percentage, so the card can show the number the X4 shows. */
#define KOSYNC_LEDGER_MAX 4
struct KosyncParkLedger {
  char     book[KOSYNC_LEDGER_MAX][KOSYNC_KEY_CHARS];   // the file-name id of the book
  uint32_t id[KOSYNC_LEDGER_MAX];                       // its inbox record (0 = none)
  double   peerPct[KOSYNC_LEDGER_MAX];                  // the peer's own KOSync percentage
  int      next;                                        // round-robin slot for a new book
};

/* Park `text` for `book`: remove this ledger's previous record for the book, push, remember
 * the new one. True when the inbox took it. Plain C++ over booksync_inbox — host-tested. */
bool kosyncParkInto(KosyncParkLedger* l, const char* book, const char* text, double peerPct,
                    uint32_t rxUnix);

// The peer's own percentage for the inbox record `inboxId`, if a KOSync park made it.
bool kosyncParkedPeerPct(const KosyncParkLedger* l, uint32_t inboxId, double* pct);

#endif // KOSYNC_H
