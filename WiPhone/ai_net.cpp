/*
 * ai_net.cpp - see ai_net.h. The decisions (the key file's format, the body, the answer, the
 * ladder, the chat's rules) are gemini.cpp's and host-tested - and since D-175 chutes.cpp's for the
 * second provider; this file is the card, the gates, the worker and the wire.
 */
#include "ai_net.h"
#include "gemini.h"
#include "chutes.h"             // D-175: Chutes' presets, body, answer, ladder and words (host-tested)
#include "wiki_lookup.h"        // the Wikipedia lookup before each question: keywords, URL, parse, background
#include "isrg_roots.h"         // ...and the roots en.wikipedia.org chains to (X1 + X2 + Root YE); all four in Chutes' set
#include "gts_roots.h"          // Google's GTS R1 + R4 (Gemini's pin; in Chutes' set too)
#include "godaddy_roots.h"      // D-175: the root llm.chutes.ai chains to today
#include "websearch.h"          // D-176: tavily.txt, the Auto rule, the body, the streaming parser, the block (host-tested)
#include "amazon_roots.h"       // D-176: the roots api.tavily.com chains to (Amazon Root CA 1 + Starfield G2)
#include "https_worker.h"       // the worker, shared with the weather since 0.9.81, and netHttps
#include "weather_net.h"        // wxRequestActive: a question asked behind a weather fetch
#include "tile_fetch.h"         // tlsInstallPsramHook, tileFetchActive, tileFetchCallRecent, filesJobActive
#include "app_gbc_xfer.h"       // xferOn / xferWindowUp: the uploader or a sync window holds the radio
#include "kosync_sync.h"        // kosyncWindowActive
#include "Networks.h"           // wifiState.radioOff()
#include "clock.h"              // ntpClock: the exchanges' times, the daily limits' deadlines
#include "tz_service.h"         // tzSvcEffective: the zone's name in the request's date sentence
#include "music_player.h"       // a question pauses the music (it needs the core for the handshake)

#include <Arduino.h>
#include <WiFi.h>
#include <SD.h>
#include <Preferences.h>        // "Retry if it times out", the model choice (NVS, the firmware's "wpmesh" namespace)
#include <esp_heap_caps.h>
#include <esp_system.h>         // esp_random: each Chutes attempt's seed
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

extern volatile bool gGbcActive;          // WiPhone.ino: the emulator owns the card and turns WiFi off

/* The worker's stack: internal RAM, carved on the first question and kept (tile_fetch.cpp's rule
 * and its reasons) - since 0.9.81 the shared HTTPS worker's (https_worker.h, NET_STACK_BYTES), the
 * weather's too. The tile worker's deepest path - a handshake, then a tile through decode and
 * the card - used ~5.2 KB of its 8 KB; this one does a handshake against a PINNED root (an
 * RSA-4096 signature check the insecure tile client never makes) and parses JSON that is read
 * with no recursion (gemini.cpp). `ai` prints the floor after every question: under ~1,500 B,
 * raise it. The de-chunked body goes into the worker's shared PSRAM sink (NET_SINK_CAP, 64 KB: a
 * 4096-token answer is ~16 KB of JSON-escaped text). */
#define AI_SINK_CAP         NET_SINK_CAP
#define AI_TEXT_CAP         (24u * 1024u)   // the joined answer before cleaning (cleaned and cut to GEM_A_MAX)
#define AI_KEY_READ_MAX     2048u
#define AI_CHAT_READ_MAX    (GEM_CHAT_FILE_MAX + 4096u)
#define AI_CONNECT_MS       10000           // TCP connect AND the TLS handshake (WiFiClientSecure's handshake_timeout)
/* The wait for the answer's headers (2026-10-11: was 60 s, the most HTTPClient's uint16_t timeout can
 * hold). A non-streaming answer's headers come only once the WHOLE answer is written: a thinking
 * model took ~6-12 s on 2026-10-03, but free-tier 3.8 Flash now takes 30-90 s (84 s for one
 * sentence, measured 2026-10-11) and COVEY logged two timeouts at its 60 s. https_worker's client
 * waits for the first byte itself, in AI_SLICE_MS slices that honour Cancel and a call
 * (NetHttpReq.waitSliceMs, .stopFn), so 150 s works. Nothing else watches the wait: the worker is
 * on no task watchdog (only the loop task and core 0's idle task are) and sleeps between slices,
 * the loop runs on beside it (LOOP STALL times the loop's own passes), and the WAITING screen only
 * counts the seconds. */
#define AI_TIMEOUT_MS       150000u
#define AI_BODY_IDLE_MS     30000u          // the body arrives in one go once the headers have; this is a dead socket
#define AI_SLICE_MS         200u            // every wait is cut into these, and honours Cancel
/* The internal heap a handshake may start from: tile_fetch.cpp's TF_CONNECT_LARGEST/FREE (the
 * handshake dips ~12 KB of lwIP buffers), plus the stack while it is not carved yet
 * (https_worker.h: NET_CONNECT_LARGEST / NET_CONNECT_FREE, netHeapOkForHandshake). */
#define AI_USER_AGENT       "WiPhone-ai/0.1 (wiphone-meshtastic)"

/* THE WIKIPEDIA LOOKUP (2026-10-11, wiki_lookup.h): one GET to en.wikipedia.org before each question,
 * never retried, its failures silent. Its own budget: 10 s for TCP + the handshake (the weather's and
 * beaconDB's cap), 20 s for the whole of it, DNS to the last byte (NetHttpReq.totalMs) - a question
 * is not held up long by a lookup that is not coming. The response is read into the shared sink up
 * to WIKI_RESPONSE_CAP (24 KB; two pages of six sentences is ~2 KB) and parsed before Gemini's
 * request reuses the sink. The roots are the ones Wi-Fi location pins (isrg_roots.h; the chain is
 * in wiki_lookup.h) - a literal, so the linker can share it with geoloc_net.cpp's. */
#define AI_WIKI_CONNECT_MS  10000u
#define AI_WIKI_TOTAL_MS    20000u
#define AI_WIKI_IDLE_MS     5000u           // a body that stops this long (it comes in one go)
#define AI_WIKI_ROOTS_PEM   ISRG_ROOT_X1_PEM ISRG_ROOT_X2_PEM ISRG_ROOT_YE_PEM

/* GOOGLE'S ROOTS, PINNED. generativelanguage.googleapis.com chains leaf <- WR2 <- GTS Root R1
 * (checked live with openssl, ECDHE-ECDSA and ECDHE-RSA, 2026-10); R4 (P-384) is here in case the
 * API moves to the WE* ECDSA intermediates. mbedTLS takes the two PEMs concatenated
 * (ssl_client.cpp: mbedtls_x509_crt_parse of the whole buffer). Exported from the macOS system
 * keychain (SystemRootCertificates) and checked against certifi's bundle - the same bytes:
 *   GTS Root R1  SHA-256 D9:47:43:2A:BD:E7:B7:FA:90:FC:2E:6B:59:10:1B:12:80:E0:E1:C7:E4:E4:0F:A3:C6:88:7F:FF:57:A7:F4:CF
 *   GTS Root R4  SHA-256 34:9D:FA:40:58:C5:E2:63:12:3B:39:8A:E7:95:57:3C:4E:13:13:C8:3F:E6:8F:93:55:6C:D5:E8:03:1B:3C:7D
 * Both valid 2016-06-22 to 2036-06-22. This build never checks certificate DATES
 * (CONFIG_MBEDTLS_HAVE_TIME_DATE is unset), so a question works before the clock is set.
 * ⚠ THESE ARE THE ONLY ROOTS, ON PURPOSE: no override file on the card. The uploader into
 * /API Keys has no password and can run on the phone's own OPEN hotspot, so a "gemini.pem"
 * override (the first cut had one, after ota.cpp's /user.pem) let anyone nearby plant their own
 * root and then read the key off the wire. If Google ever leaves both roots (they run to 2036),
 * that is a firmware update. */
static const char AI_ROOTS_PEM[] = GTS_ROOT_R1_PEM GTS_ROOT_R4_PEM;   // the bytes, moved to gts_roots.h (D-175)

/* CHUTES' ROOT SET (D-175, measured 2026-10-18 - godaddy_roots.h has the chain). llm.chutes.ai serves
 * *.chutes.ai <- GoDaddy Secure CA G2 <- Go Daddy Root CA G2 today; that wildcard expires 2026-11-30,
 * and CT logs already show *.chutes.ai from Let's Encrypt (YR2 RSA, YE2 ECDSA) and from Google Trust
 * Services (WR1, WE1: GTS R1, R4). For Let's Encrypt the Gen-Y roots are pinned as well as X1 + X2
 * (review 2026-10-18): YR2's issuer is "Root YR" and YE2's "Root YE", so a server that sends the SHORT
 * chain (leaf <- YR2, without the cross certificate to X1) has no anchor in X1 + X2 alone - measured
 * for exactly that on the weather's and beaconDB's hosts (isrg_roots.h), which is why they pin YR /
 * YE. With all seven a renewal from any of those CAs keeps working, whichever chain it is served
 * with. GoDaddy's next root ("GoDaddy TLS Root CA - R1") is in no macOS store yet, so it is not.
 * ROOTS ONLY, never a leaf or an intermediate. mbedTLS parses them all on each connection
 * (setCACert), ~12.6 KB of PEM, into PSRAM (tlsInstallPsramHook). The same rule as Google's: no
 * override file. tests/test_chutes.cpp checks each SHA-256; tests/check_ai_glue.py pins this list. */
#define AI_CHUTES_ROOTS_PEM GODADDY_ROOT_G2_PEM ISRG_ROOT_X1_PEM ISRG_ROOT_X2_PEM ISRG_ROOT_YR_PEM ISRG_ROOT_YE_PEM \
                            GTS_ROOT_R1_PEM GTS_ROOT_R4_PEM

/* THE WEB SEARCH'S ROOT SET (D-176, measured 2026-10-19 - amazon_roots.h has the chain): api.tavily.com
 * serves *.tavily.com <- Amazon RSA 2048 M01 <- Amazon Root CA 1 (sent as the cross certificate signed by
 * Starfield Services Root CA - G2). Both roots, as the spec says: M01 anchors at Amazon Root CA 1, and a
 * server that sends the cross certificate anchors at Starfield G2 even without it. ROOTS ONLY (the leaf
 * expires 2026-12-15); no override file. tests/test_websearch.cpp checks each SHA-256;
 * tests/check_ai_glue.py pins this list:
 *   Amazon Root CA 1                         SHA-256 8E:CD:E6:88:4F:3D:87:B1:...:CB:97:C6:AE:98:19:6E
 *   Starfield Services Root CA - G2          SHA-256 56:8D:69:05:A2:C8:87:08:...:0F:CB:2A:E6:3E:DA:B5 */
#define AI_TAVILY_ROOTS_PEM AMAZON_ROOT_CA1_PEM STARFIELD_SERVICES_ROOT_G2_PEM
/* THE SEARCH'S BUDGET. Tavily's answer gets 6 s (websearch.h WEB_ANSWER_MS: the spec said 3 s; Nick, 2026-10-19:
 * "I don't mind long search budgets" - a slow answer waits instead of being dropped) - the first-byte wait
 * after the request, the body inside the whole budget) - measured from the Mac, where the whole TLS 1.2
 * handshake is ~155 ms. HERE a handshake is software ECC: 2.0-3.1 s on the RSA + P-256 hosts this phone
 * already talks to (the weather's Open-Meteo and NWS, 2026-10-03), ~4.5 s to Google, 5.8 s on a P-384
 * chain. A 3 s TOTAL would end nearly every search inside its own handshake. So the handshake has its
 * own cap (6 s, TCP included) and the whole - DNS to the last byte - is the two together, 12 s at most;
 * never retried. A search that is not coming costs at most that, and the question goes on without it. */
#define AI_WEB_CONNECT_MS   6000u
#define AI_WEB_TOTAL_MS     (AI_WEB_CONNECT_MS + WEB_ANSWER_MS)
#define AI_WEB_IDLE_MS      1500u           // a body that stops this long (Tavily's comes in one go)

// ── module state ─────────────────────────────────────────────────────────────────────────

/* The question in flight. Written on the loop by aiAsk BEFORE the semaphore is given; the
 * worker's until s_done; read again by the loop only in aiLoopTick. PSRAM. */
struct AiReq {
  /* D-175: CHUTES_GEMINI (Gemini, the path of before) or the Chutes preset asked for - the choice
   * as it was when asked (aiModelChoice). */
  int          choice;
  /* ⚠ a copy of the key: wiped when the question ends. Gemini's file - or, for a Chutes question,
   * nothing but `lookup` (chutes.txt's), so the lookup's rule reads one place and the Gemini key
   * never rides along with a Chutes question. */
  GeminiConfig cfg;
  ChutesConfig chutes;                    // ⚠ D-175: a copy of the CHUTES key (Chutes questions only): wiped too
  char         question[GEM_Q_MAX];
  int          nCtx;
  char         ctxQ[GEM_CONTEXT][GEM_Q_MAX];
  char         ctxA[GEM_CONTEXT][GEM_A_MAX];
  bool         topic;                     // this question starts a topic (nothing earlier sent)
  int64_t      askedUtc;                  // 0 = the clock is not trusted
  int          tzS;
  char         zone[24];                  // the zone's name for the date sentence ("Pacific"), "" = none
  GeminiQuota  quota;                     // the remembered limits in; the worker marks it; folded back
  GeminiQuota  quotaIn;                   // ...as it went in (a changed one is saved)
  bool         musicPaused;               // aiAsk paused a playing track for this question
  bool         retryTimeouts;             // "Retry if it times out" as it was when asked (the ladder's rule 6)
  /* Asked while a weather fetch held the worker (0.9.81): aiAsk could not judge the heap then (the
   * weather's handshake was dipping it), so the worker checks the bar itself before the first
   * request. Otherwise aiAsk's check stands, as it always has. */
  bool         heapCheckAtStart;
  /* D-176: the web search, decided by aiAsk (webDecide) - and a copy of the TAVILY key only when it
   * searches (⚠ wiped when the question ends, as the others). */
  WebConfig    web;
  WebDecision  webDec;                    // what was decided, and why (`ai`)
  int          webNote;                   // WEB_NOTE_CAP: Auto would have searched, the day's are used
  bool         webOnly;                   // `ai web test`: the search alone - no lookup, no model, no chat
};
/* What the worker found. The worker's while s_busy; the loop's after (it stays for `ai`). */
struct AiRes {
  bool     ok, cancelled;
  char     answer[GEM_A_MAX];
  char     label[GEM_LABEL_MAX];
  char     fail[256];
  int      lastCode, net, attempts;
  uint32_t ms;                            // ask to result
  uint32_t lastMs;                        // the last request alone
  uint32_t bodyBytes;
  bool     wikiAsked;                     // the Wikipedia lookup's GET was made
  int      wikiPages;                     // pages that went with the question (0 = no background)
  uint32_t wikiMs;                        // the lookup, start to end
  char     wikiWhy[48];                   // why it gave no background ("" = it did, or none was asked)
  /* ...and why no GET was made (a literal; `ai` prints it - review 2026-10-11: a question that ended
   * before its lookup read "lookup off, or no keywords"). aiRun sets the "ended before" one first. */
  const char* wikiNot;
  uint32_t largestFloor, minEver, stackFloor;
  /* D-176: the web search (never its text, its query or the key: codes, sizes, times). */
  bool     webAsked;                      // its POST was attempted (`ai`'s diagnostics)
  /* ...and its request was WRITTEN on a live TLS connection (review 2026-10-19): only that one can
   * have cost a Tavily credit, so only that one is counted. DNS, a refused or failed handshake (a
   * captive portal against the pinned roots), the heap bar, a Cancel or a call before the send: not. */
  bool     webSent;
  int      webCode, webNet, webParsed;    // HTTP, GEM_NET_*, WEB_PARSE_*
  int      webKept;                       // lines in the block
  int      webNote;                       // WEB_NOTE_* (NONE: results went, or no search was wanted)
  uint32_t webMs, webHsMs, webBlockLen;   // the search start to end; TCP + handshake; the block's bytes
  long     webRespMs;                     // Tavily's response_time, ms (-1 none)
  int      webCredits;                    // usage.credits (-1 none)
};
/* The worker's buffers - PSRAM, so its 8 KB stack is left to the handshake. (The body itself goes
 * into the shared worker's sink: netWorkerSink().) */
struct AiWork {
  char         text[AI_TEXT_CAP];
  GeminiLadder ladder;
  char         label[GEM_LABEL_MAX];
  char         primaryLabel[GEM_LABEL_MAX];
  char         stage[128];
  char         sys[GEM_SYS_MAX];            // the system instruction, built for each request
  char         kw[WIKI_KW_CAP];             // the question's keywords: ALL that goes to Wikipedia
  char         wikiUrl[WIKI_URL_MAX];
  WikiResult   wiki;                        // the pages read (~3.9 KB)
  /* The background for the question's turn ("" = none): Wikipedia's block (WIKI_BG_MAX, its budget
   * unchanged) and - D-176 - the web's after it (webAppendBlock; its own WEB_BLOCK_MAX, EXTRA). Never
   * stored: only the answer is. */
  char         bg[WIKI_BG_MAX + 2 + WEB_BLOCK_MAX + 1];
  /* The sources under the answer: "Wikipedia: A; B" (with Wikipedia's background only), then - D-176 -
   * the web's "Web: a; b" or its note on a line of its own (webAppendLine). "" = no line. */
  char         src[WIKI_SRC_MAX + 1 + WEB_SRC_MAX + 1];
  ChutesLadder cl;                          // D-175: a Chutes question's ladder
  char         auth[CHUTES_AUTH_MAX];       // ⚠ "Bearer <key>": built for ONE request, wiped right after it
  /* D-176: the web search's. The parser is fed by https_worker's tap as the bytes arrive (~6.6 KB: the
   * fields it keeps, never the body); the block and its line as built (the block goes into bg). */
  WebParse     wp;
  char         web[WEB_BLOCK_MAX + 1];
  char         webSrc[WEB_SRC_MAX + 1];
  char         webBody[WEB_BODY_MAX];       // the /search body (the question's words: never logged)
  char         webAuth[WEB_AUTH_MAX];       // ⚠ "Bearer <tvly key>": built for the ONE request, wiped right after it
};

static AiReq*   s_req  = NULL;
static AiRes*   s_res  = NULL;
static AiWork*  s_work = NULL;
static char*    s_body = NULL;            // the request body, grown as needed (PSRAM)
static size_t   s_bodyCap = 0;

static volatile bool     s_busy   = false;   // asked; the worker has it (or will: queued behind a weather fetch)
static volatile bool     s_done   = false;   // the worker finished; the loop has not folded it yet
static volatile bool     s_cancel = false;
static bool              s_haveRes = false;  // s_res holds a finished question (for `ai`)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* The module's small state, in PSRAM rather than ~1.1 KB of the internal .bss (the internal heap
 * is what this phone runs out of). Allocated on first use; every entry point asks memOk(). */
struct AiMem {
  AiProgress    prog;                    // published under s_mux
  AiKeyInfo     key;                     // what the last read found
  GeminiQuota   quota;                   // the remembered daily limits
  GeminiSession session;                 // models that refuse thinkingConfig (this boot)
  bool          retryLoaded;             // "Retry if it times out" read from NVS (once)
  bool          retryOn;                 // ...and what it says (absent = ON)
  AiChutesKey   ckey;                    // D-175: what the last read of chutes.txt found
  bool          modelLoaded;             // the model choice read from NVS (once)
  char          modelStored[16];         // ...its id ("" = never set: the default rule)
  uint32_t      chutesGone;              // presets that answered "not found" this boot (chutes.h, rule 3)
  bool          chutesAnswered;          // Chutes has answered this boot (a 429 is load, not the key)
  /* The provider the last question went to THIS BOOT (CHUTES_BY_*; UNKNOWN until one is asked): a
   * question to the other starts a topic (chutesTopicDue - the X4's review, 2026-10-19). After a
   * restart the saved chat's labels say who answered; this says who was ASKED, failures included. */
  int           topicLast;
  AiWebKey      wkey;                    // D-176: what the last read of tavily.txt found
  bool          webLoaded;               // the web search setting read from NVS (once)
  char          webStored[8];            // ...its id ("" = never set: Auto)
  bool          dayLoaded;               // the day's counts read from NVS (once)
  WebDay        day;                     // ...the local day they belong to and the two counts
};
static AiMem* s_m = NULL;

static bool memOk() {
  if (!s_m) {
    s_m = (AiMem*)heap_caps_calloc(1, sizeof(AiMem), MALLOC_CAP_SPIRAM);
    if (s_m) {
      s_m->ckey.preset = -1;                // no chutes.txt model= until one is read
      s_m->topicLast = CHUTES_BY_UNKNOWN;   // nothing asked this boot
      s_m->day.day = -1;                    // D-176: no day's counts until NVS is read
    }
  }
  return s_m != NULL;
}

static GeminiConfig* s_cfg = NULL;           // the last GOOD gemini.txt (PSRAM)
static bool          s_haveGood = false;
static int           s_keyLogged = -1;       // the state last logged (log once per change)
static int           s_keyMisses = 0;        // reloads in a row that found no gemini.txt
static ChutesConfig* s_ccfg = NULL;          // D-175: the last GOOD chutes.txt (PSRAM)
static bool          s_cHaveGood = false;
static int           s_cKeyLogged = -1;
static int           s_cKeyMisses = 0;
static WebConfig*    s_wcfg = NULL;          // D-176: the last GOOD tavily.txt (PSRAM)
static bool          s_wHaveGood = false;
static int           s_wKeyLogged = -1;
static int           s_wKeyMisses = 0;

static GeminiChat*   s_chat = NULL;          // PSRAM, ~94 KB
static bool          s_chatLoaded = false;
/* /ai/chat.txt is there but the card would not give it up: NOT an empty chat. Never saved over
 * while this holds; what lands meanwhile is kept in memory and added to it once it reads. */
static bool          s_chatUnread = false;
/* Asked for while a question was still out (aiLoopTick applies them when it lands): "New topic"
 * goes after it, "Clear chat" drops it. */
static bool          s_breakAfter = false;
static bool          s_clearAfter = false;
static uint32_t      s_chatGen = 1;

static bool          s_quotaLoaded = false;
static bool          s_sessionInit = false;

static uint32_t s_asked = 0, s_answered = 0, s_failed = 0;

// ── small helpers ────────────────────────────────────────────────────────────────────────

static void heapNow(uint32_t* freeB, uint32_t* largest, uint32_t* minEver) {
  netHeapNow(freeB, largest, minEver);
}

static int64_t utcNow() {
  return (int64_t)ntpClock.getTrustedUtcTime();      // 0 = not trusted: no topic gaps, no saved deadlines
}

static int tzNow() {
  const int32_t d = (int32_t)(ntpClock.getExactUnixTime() - ntpClock.getExactUtcTime());
  const int32_t m = (d >= 0) ? (d + 30) / 60 : -((-d + 30) / 60);
  return (int)(m * 60);
}

/* "Phone low on memory (11 KB block, 30 KB free) - reboot first": aiAsk's refusal, and the worker's
 * when the heap bar or a handshake's allocation failed later in a question (review 2026-10-11: after
 * the Wikipedia lookup's handshake a fragmented heap read as a generic failure). The numbers now. */
static void lowMemText(char* out, size_t cap) {
  uint32_t f = 0, lg = 0;
  netHeapOkForHandshake(&f, &lg);
  snprintf(out, cap, "Phone low on memory (%u KB block, %u KB free) - reboot first", (unsigned)(lg / 1024),
           (unsigned)(f / 1024));
}

/* A small file into buf (NUL-terminated), retried once after 20 ms: the card shares the SPI bus
 * with the screen, and a failed open must never be read as "no file". -1 = unreadable. */
static int readSmall(const char* path, char* buf, size_t cap) {
  File f = SD.open(path, FILE_READ);
  if (!f) {
    delay(20);
    f = SD.open(path, FILE_READ);
  }
  if (!f) {
    return -1;
  }
  int n = f.read((uint8_t*)buf, cap - 1);
  f.close();
  if (n < 0) {
    return -1;
  }
  buf[n] = '\0';
  return n;
}

/* Write a whole file as <path>.tmp-style then rename into place (FAT cannot rename over a file). */
static bool writeFile(const char* path, const char* tmp, const char* data, size_t len) {
  if (!SD.exists(AI_DIR)) {
    SD.mkdir(AI_DIR);
  }
  if (SD.exists(tmp)) {
    SD.remove(tmp);                      // exists() first: remove() logs an error line for a missing file
  }
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) {
    return false;
  }
  const size_t w = f.write((const uint8_t*)data, len);
  f.close();
  if (w != len) {
    SD.remove(tmp);
    return false;
  }
  if (SD.exists(path)) {
    SD.remove(path);
  }
  return SD.rename(tmp, path);
}

/* D-175: the short name of the model being asked ("V4 Flash"), for the countdown's row. */
static void progShort(const char* shortName) {
  if (!s_m) {
    return;
  }
  portENTER_CRITICAL(&s_mux);
  strlcpy(s_m->prog.shortName, shortName ? shortName : "", sizeof(s_m->prog.shortName));
  s_m->prog.gen++;
  portEXIT_CRITICAL(&s_mux);
}

static void progSet(const char* stage, const char* label, uint32_t waitUntil, int attempt) {
  if (!s_m) {
    return;
  }
  portENTER_CRITICAL(&s_mux);
  if (stage) {
    strlcpy(s_m->prog.stage, stage, sizeof(s_m->prog.stage));
  }
  if (label) {
    strlcpy(s_m->prog.label, label, sizeof(s_m->prog.label));
  }
  s_m->prog.waitUntilMs = waitUntil;
  if (attempt >= 0) {
    s_m->prog.attempt = attempt;
  }
  s_m->prog.gen++;
  portEXIT_CRITICAL(&s_mux);
}

// ── the key ──────────────────────────────────────────────────────────────────────────────

static void keyInfoFrom(const GeminiConfig* c, int rc) {
  geminiConfigLine(c, rc, s_m->key.line, sizeof(s_m->key.line));
  strlcpy(s_m->key.model, c->model, sizeof(s_m->key.model));
  strlcpy(s_m->key.fallback, c->fallback, sizeof(s_m->key.fallback));
  s_m->key.thinking = c->thinking;
  s_m->key.lookup = c->lookup;
}

/* D-175: chutes.txt, the same pass and the same rules as gemini.txt below - exists() twice 20 ms
 * apart, only a GOOD read replaces the kept key, one missed read keeps it (two in a row: gone), the
 * read buffer PSRAM and wiped (geminiWipe: a memset right before free() is a dead store the compiler
 * drops - review 2026-10-18), the state logged once per change and never the key. Its path ONLY: no
 * root copy. */
static void chutesKeyLine(int rc) {
  chutesConfigLine(s_ccfg, rc, s_m->ckey.line, sizeof(s_m->ckey.line));
  s_m->ckey.preset = s_ccfg->preset;
  s_m->ckey.lookup = s_ccfg->lookup;
}

static void chutesKeyReload() {
  if (!s_ccfg) {
    s_ccfg = (ChutesConfig*)heap_caps_calloc(1, sizeof(ChutesConfig), MALLOC_CAP_SPIRAM);
    if (!s_ccfg) {
      s_m->ckey.state = AI_KEY_UNREADABLE;
      s_m->ckey.usable = false;
      strlcpy(s_m->ckey.line, "no memory to read the key file", sizeof(s_m->ckey.line));
      return;
    }
  }
  bool there = false;
  for (int tries = 0; tries < 2 && !there; tries++) {
    if (tries) {
      delay(20);
    }
    there = SD.exists(AI_CHUTES_FILE);
  }
  s_cKeyMisses = there ? 0 : (s_cKeyMisses < 2 ? s_cKeyMisses + 1 : 2);
  if (!there && s_cHaveGood && s_cKeyMisses < 2) {
    s_m->ckey.state = AI_KEY_UNREADABLE;          // a card hiccup until the next reload agrees
    s_m->ckey.usable = true;
    chutesKeyLine(CHUTES_CFG_OK);
  } else if (!there) {
    s_cHaveGood = false;
    chutesParseConfig("", 0, s_ccfg);             // no key; the defaults, for the line
    chutesKeyLine(CHUTES_CFG_NO_KEY);
    s_m->ckey.state = AI_KEY_NONE;
    s_m->ckey.usable = false;
    s_m->ckey.path = NULL;
  } else {
    char* buf = (char*)heap_caps_malloc(AI_KEY_READ_MAX, MALLOC_CAP_SPIRAM);   // ⚠ the key: wiped after
    const int n = buf ? readSmall(AI_CHUTES_FILE, buf, AI_KEY_READ_MAX) : -1;
    s_m->ckey.path = AI_CHUTES_FILE;
    if (n < 0) {
      s_m->ckey.state = AI_KEY_UNREADABLE;        // NOT "no key": the last good one stays
      s_m->ckey.usable = s_cHaveGood;
      if (s_cHaveGood) {
        chutesKeyLine(CHUTES_CFG_OK);
      } else {
        strlcpy(s_m->ckey.line, "key: the file could not be read - try again", sizeof(s_m->ckey.line));
      }
    } else {
      ChutesConfig* c = (ChutesConfig*)heap_caps_calloc(1, sizeof(ChutesConfig), MALLOC_CAP_SPIRAM);
      if (!c) {
        s_m->ckey.state = AI_KEY_UNREADABLE;
        s_m->ckey.usable = s_cHaveGood;
      } else {
        const int rc = chutesParseConfig(buf, (size_t)n, c);
        chutesConfigLine(c, rc, s_m->ckey.line, sizeof(s_m->ckey.line));
        s_m->ckey.preset = c->preset;
        s_m->ckey.lookup = c->lookup;
        if (rc == CHUTES_CFG_OK) {
          memcpy(s_ccfg, c, sizeof(*s_ccfg));
          s_cHaveGood = true;
          s_m->ckey.state = AI_KEY_OK;
          s_m->ckey.usable = true;
        } else {
          memset(s_ccfg, 0, sizeof(*s_ccfg));
          s_ccfg->preset = -1;
          s_cHaveGood = false;
          s_m->ckey.state = rc == CHUTES_CFG_NO_KEY ? AI_KEY_NONE : AI_KEY_BAD;
          s_m->ckey.usable = false;
        }
        geminiWipe(c, sizeof(*c));
        free(c);
      }
    }
    if (buf) {
      geminiWipe(buf, AI_KEY_READ_MAX);
      free(buf);
    }
  }
  if (s_m->ckey.state != s_cKeyLogged) {
    s_cKeyLogged = s_m->ckey.state;
    // ⚠ Never the key: its length, the preset and where the file is.
    log_e("AI: chutes %s (%s)", s_m->ckey.line, s_m->ckey.path ? s_m->ckey.path : "no chutes.txt");
  }
}

/* D-176: tavily.txt, the same pass and the same rules as chutes.txt above - exists() twice 20 ms apart,
 * only a GOOD read replaces the kept key, one missed read keeps it (two in a row: gone), the read buffer
 * PSRAM and wiped (geminiWipe), the state logged once per change and never the key. Its path ONLY. */
static void webKeyReload() {
  if (!s_wcfg) {
    s_wcfg = (WebConfig*)heap_caps_calloc(1, sizeof(WebConfig), MALLOC_CAP_SPIRAM);
    if (!s_wcfg) {
      s_m->wkey.state = AI_KEY_UNREADABLE;
      s_m->wkey.usable = false;
      strlcpy(s_m->wkey.line, "no memory to read the key file", sizeof(s_m->wkey.line));
      return;
    }
  }
  bool there = false;
  for (int tries = 0; tries < 2 && !there; tries++) {
    if (tries) {
      delay(20);
    }
    there = SD.exists(AI_TAVILY_FILE);
  }
  s_wKeyMisses = there ? 0 : (s_wKeyMisses < 2 ? s_wKeyMisses + 1 : 2);
  if (!there && s_wHaveGood && s_wKeyMisses < 2) {
    s_m->wkey.state = AI_KEY_UNREADABLE;          // a card hiccup until the next reload agrees
    s_m->wkey.usable = true;
    webConfigLine(s_wcfg, WEB_CFG_OK, s_m->wkey.line, sizeof(s_m->wkey.line));
  } else if (!there) {
    s_wHaveGood = false;
    memset(s_wcfg, 0, sizeof(*s_wcfg));
    webConfigLine(s_wcfg, WEB_CFG_NO_KEY, s_m->wkey.line, sizeof(s_m->wkey.line));
    s_m->wkey.state = AI_KEY_NONE;
    s_m->wkey.usable = false;
    s_m->wkey.path = NULL;
  } else {
    char* buf = (char*)heap_caps_malloc(AI_KEY_READ_MAX, MALLOC_CAP_SPIRAM);   // ⚠ the key: wiped after
    const int n = buf ? readSmall(AI_TAVILY_FILE, buf, AI_KEY_READ_MAX) : -1;
    s_m->wkey.path = AI_TAVILY_FILE;
    if (n < 0) {
      s_m->wkey.state = AI_KEY_UNREADABLE;        // NOT "no key": the last good one stays
      s_m->wkey.usable = s_wHaveGood;
      if (s_wHaveGood) {
        webConfigLine(s_wcfg, WEB_CFG_OK, s_m->wkey.line, sizeof(s_m->wkey.line));
      } else {
        strlcpy(s_m->wkey.line, "key: the file could not be read - try again", sizeof(s_m->wkey.line));
      }
    } else {
      WebConfig* c = (WebConfig*)heap_caps_calloc(1, sizeof(WebConfig), MALLOC_CAP_SPIRAM);
      if (!c) {
        s_m->wkey.state = AI_KEY_UNREADABLE;
        s_m->wkey.usable = s_wHaveGood;
      } else {
        const int rc = webParseConfig(buf, (size_t)n, c);
        webConfigLine(c, rc, s_m->wkey.line, sizeof(s_m->wkey.line));
        if (rc == WEB_CFG_OK) {
          memcpy(s_wcfg, c, sizeof(*s_wcfg));
          s_wHaveGood = true;
          s_m->wkey.state = AI_KEY_OK;
          s_m->wkey.usable = true;
        } else {
          memset(s_wcfg, 0, sizeof(*s_wcfg));
          s_wHaveGood = false;
          s_m->wkey.state = rc == WEB_CFG_NO_KEY ? AI_KEY_NONE : AI_KEY_BAD;
          s_m->wkey.usable = false;
        }
        geminiWipe(c, sizeof(*c));
        free(c);
      }
    }
    if (buf) {
      geminiWipe(buf, AI_KEY_READ_MAX);
      free(buf);
    }
  }
  if (s_m->wkey.state != s_wKeyLogged) {
    s_wKeyLogged = s_m->wkey.state;
    // ⚠ Never the key: its length and where the file is.
    log_e("AI: tavily %s (%s)", s_m->wkey.line, s_m->wkey.path ? s_m->wkey.path : "no tavily.txt");
  }
}

void aiKeyReload() {
  if (!memOk()) {
    return;
  }
  chutesKeyReload();                        // D-175: the second key file, the same rules
  webKeyReload();                           // D-176: the third (the web search's)
  if (!s_cfg) {
    s_cfg = (GeminiConfig*)heap_caps_calloc(1, sizeof(GeminiConfig), MALLOC_CAP_SPIRAM);
    if (!s_cfg) {
      s_m->key.state = AI_KEY_UNREADABLE;
      s_m->key.usable = false;
      strlcpy(s_m->key.line, "no memory to read the key file", sizeof(s_m->key.line));
      return;
    }
  }
  /* FAT is case-blind: "Gemini.txt" from a Mac is the same name. exists() first: in this core
   * SD.open() of a missing file prints "[E] ... does not exist". exists() is itself an open with
   * no retry, so "no file" is asked twice, 20 ms apart, as readSmall does. */
  const char* path = NULL;
  for (int tries = 0; tries < 2 && !path; tries++) {
    if (tries) {
      delay(20);
    }
    if (SD.exists(AI_KEY_FILE)) {
      path = AI_KEY_FILE;
    } else if (SD.exists(AI_KEY_FILE_ROOT)) {
      path = AI_KEY_FILE_ROOT;
    }
  }
  s_keyMisses = path ? 0 : (s_keyMisses < 2 ? s_keyMisses + 1 : 2);
  if (!path && s_haveGood && s_keyMisses < 2) {
    /* A good key, and now no file: a card hiccup until the next reload agrees (the spec: a failed
     * read is never "no key"). Two in a row (opening Key info reads twice) and it really is gone. */
    s_m->key.state = AI_KEY_UNREADABLE;
    s_m->key.usable = true;
    keyInfoFrom(s_cfg, GEM_CFG_OK);
  } else if (!path) {
    s_haveGood = false;
    geminiParseConfig("", 0, s_cfg);       // no key; the defaults, for the line
    keyInfoFrom(s_cfg, GEM_CFG_NO_KEY);
    s_m->key.state = AI_KEY_NONE;
    s_m->key.usable = false;
    s_m->key.path = NULL;
  } else {
    // PSRAM, and wiped after (geminiWipe - a memset before free() is dropped): the key is in these bytes.
    char* buf = (char*)heap_caps_malloc(AI_KEY_READ_MAX, MALLOC_CAP_SPIRAM);
    const int n = buf ? readSmall(path, buf, AI_KEY_READ_MAX) : -1;
    if (n < 0) {
      /* The card would not give it up. NOT "no key": the last good one stays (the T-Deck latched
       * "no key" for a session on one failed early read). */
      s_m->key.state = AI_KEY_UNREADABLE;
      s_m->key.usable = s_haveGood;
      s_m->key.path = path;
      if (s_haveGood) {
        keyInfoFrom(s_cfg, GEM_CFG_OK);
      } else {
        strlcpy(s_m->key.line, "key: the file could not be read - try again", sizeof(s_m->key.line));
      }
    } else {
      GeminiConfig* c = (GeminiConfig*)heap_caps_calloc(1, sizeof(GeminiConfig), MALLOC_CAP_SPIRAM);
      if (!c) {
        s_m->key.state = AI_KEY_UNREADABLE;
        s_m->key.usable = s_haveGood;
      } else {
        const int rc = geminiParseConfig(buf, (size_t)n, c);
        keyInfoFrom(c, rc);
        s_m->key.path = path;
        if (rc == GEM_CFG_OK) {
          memcpy(s_cfg, c, sizeof(*s_cfg));
          s_haveGood = true;
          s_m->key.state = AI_KEY_OK;
          s_m->key.usable = true;
        } else {
          memset(s_cfg, 0, sizeof(*s_cfg));
          s_haveGood = false;
          s_m->key.state = rc == GEM_CFG_NO_KEY ? AI_KEY_NONE : AI_KEY_BAD;
          s_m->key.usable = false;
        }
        geminiWipe(c, sizeof(*c));
        free(c);
      }
    }
    if (buf) {
      geminiWipe(buf, AI_KEY_READ_MAX);
      free(buf);
    }
  }
  if (s_m->key.state != s_keyLogged) {
    s_keyLogged = s_m->key.state;
    // ⚠ Never the key: its length, the model and where the file is.
    log_e("AI: %s (%s)", s_m->key.line, s_m->key.path ? s_m->key.path : "no gemini.txt");
  }
}

void aiKeyInfo(AiKeyInfo* out) {
  if (!memOk()) {
    memset(out, 0, sizeof(*out));
    strlcpy(out->line, "no memory", sizeof(out->line));
    return;
  }
  *out = s_m->key;
}

void aiChutesKeyInfo(AiChutesKey* out) {
  if (!memOk()) {
    memset(out, 0, sizeof(*out));
    out->preset = -1;
    strlcpy(out->line, "no memory", sizeof(out->line));
    return;
  }
  *out = s_m->ckey;
}

void aiWebKeyInfo(AiWebKey* out) {
  if (!memOk()) {
    memset(out, 0, sizeof(*out));
    strlcpy(out->line, "no memory", sizeof(out->line));
    return;
  }
  *out = s_m->wkey;
}

// ── the chat and the remembered limits ────────────────────────────────────────────────────

static void chatSave();

/* The saved chat, read on first use. Three outcomes, kept apart (review 2026-10-03): no file
 * (an empty chat), a file that is not a chat (empty - and the file set aside as chat.bad, never
 * just overwritten), and a file the card would not give up (NOT empty: s_chatUnread, read again
 * on the next call, and nothing is saved over it meanwhile). */
static GeminiChat* chatGet() {
  if (s_chatLoaded) {
    return s_chat;
  }
  if (!s_chat) {
    s_chat = (GeminiChat*)heap_caps_calloc(1, sizeof(GeminiChat), MALLOC_CAP_SPIRAM);
    if (!s_chat) {
      return NULL;
    }
    geminiChatClear(s_chat);
  }
  const char* path = NULL;
  for (int tries = 0; tries < 2 && !path; tries++) {
    if (tries) {
      delay(20);                           // exists() is an open with no retry (readSmall's rule)
    }
    path = SD.exists(AI_CHAT_FILE) ? AI_CHAT_FILE : (SD.exists(AI_CHAT_TMP) ? AI_CHAT_TMP : NULL);
  }
  if (!path) {
    s_chatLoaded = true;                   // no chat yet: what is in memory is the chat
    s_chatUnread = false;
    s_chatGen++;
    return s_chat;
  }
  char* buf = (char*)heap_caps_malloc(AI_CHAT_READ_MAX, MALLOC_CAP_SPIRAM);
  const int n = buf ? readSmall(path, buf, AI_CHAT_READ_MAX) : -1;
  if (n < 0) {
    if (!s_chatUnread) {
      log_e("AI: %s unreadable just now - kept, and read again later", path);
    }
    s_chatUnread = true;
    free(buf);
    return s_chat;
  }
  /* Exchanges that landed while the file could not be read go on after what it holds. */
  GeminiChat* held = (s_chatUnread && s_chat->n > 0) ? s_chat : NULL;
  GeminiChat* into = s_chat;
  if (held) {
    into = (GeminiChat*)heap_caps_calloc(1, sizeof(GeminiChat), MALLOC_CAP_SPIRAM);
    if (!into) {
      free(buf);
      return s_chat;                       // still unread: tried again on the next call
    }
  }
  if (!geminiChatLoad(buf, (size_t)n, into)) {
    log_e("AI: %s is not a chat this firmware reads - set aside as %s", path, AI_CHAT_BAD);
    geminiChatClear(into);
    if (SD.exists(AI_CHAT_BAD)) {
      SD.remove(AI_CHAT_BAD);
    }
    SD.rename(path, AI_CHAT_BAD);
  }
  free(buf);
  s_chatLoaded = true;
  s_chatUnread = false;
  if (held) {
    for (int i = 0; i < held->n; i++) {
      const GeminiExchange* e = &held->ex[i];
      geminiChatAdd(into, e->t, (e->flags & GEM_EX_TOPIC) != 0, (e->flags & GEM_EX_FAILED) != 0,
                    e->label, e->q, e->a);
    }
    if (held->breakPending) {
      geminiChatNewTopic(into);
    }
    free(held);
    s_chat = into;
    chatSave();
  }
  s_chatGen++;
  return s_chat;
}

static void chatSave() {
  if (!s_chat) {
    return;
  }
  if (s_chatUnread) {
    log_e("AI: %s not saved - the one on the card has not been read yet", AI_CHAT_FILE);
    return;
  }
  const size_t n = geminiChatSave(s_chat, NULL, 0);
  char* buf = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
  if (!buf) {
    log_e("AI: no memory to save the chat");
    return;
  }
  geminiChatSave(s_chat, buf, n + 1);
  if (!writeFile(AI_CHAT_FILE, AI_CHAT_TMP, buf, n)) {
    log_e("AI: the card refused %s", AI_CHAT_FILE);
  }
  free(buf);
}

const GeminiChat* aiChat() {
  return chatGet();
}

uint32_t aiChatGen() {
  return s_chatGen;
}

void aiChatNewTopic() {
  GeminiChat* c = chatGet();
  if (aiRequestActive()) {
    s_breakAfter = true;                   // the question out was asked BEFORE the break
  }
  if (c && c->n > 0 && !c->breakPending) {
    geminiChatNewTopic(c);
    chatSave();
    s_chatGen++;
  }
}

bool aiTopicDue(bool* otherProvider) {
  if (otherProvider) {
    *otherProvider = false;
  }
  const GeminiChat* c = aiRequestActive() ? s_chat : chatGet();
  if (!c || !memOk()) {
    return true;
  }
  const int64_t now = utcNow();
  /* aiAsk's own rule with aiAsk's own inputs: the choice in use, this boot's last provider. */
  const bool due = chutesTopicDue(c, now, aiModelChoice(), s_m->topicLast);
  if (otherProvider) {
    *otherProvider = due && !geminiChatTopicDue(c, now);   // only the provider's rule says so
  }
  return due;
}

void aiChatClear() {
  if (aiRequestActive()) {
    s_clearAfter = true;                   // the question out is dropped when it lands
  }
  if (!s_chat) {
    s_chat = (GeminiChat*)heap_caps_calloc(1, sizeof(GeminiChat), MALLOC_CAP_SPIRAM);
  }
  if (s_chat) {
    geminiChatClear(s_chat);
  }
  s_chatLoaded = s_chat != NULL;           // the user's word: whatever the card held is gone
  s_chatUnread = false;
  if (SD.exists(AI_CHAT_FILE)) SD.remove(AI_CHAT_FILE);
  if (SD.exists(AI_CHAT_TMP))  SD.remove(AI_CHAT_TMP);
  s_chatGen++;
}

static void quotaLoad() {
  if (s_quotaLoaded || !memOk()) {
    return;
  }
  s_quotaLoaded = true;
  geminiQuotaClear(&s_m->quota);
  /* state.tmp when state.txt is missing, as the chat does: writeFile removes the old file before
   * the rename, so a power cut between the two leaves only the .tmp - whole, and the newest. */
  const char* path = SD.exists(AI_STATE_FILE) ? AI_STATE_FILE : (SD.exists(AI_STATE_TMP) ? AI_STATE_TMP : NULL);
  if (!path) {
    return;
  }
  char* buf = (char*)heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
  const int n = buf ? readSmall(path, buf, 512) : -1;
  if (n < 0 || !geminiQuotaLoad(buf, (size_t)n, &s_m->quota)) {
    geminiQuotaClear(&s_m->quota);
  }
  free(buf);
}

static void quotaSave() {
  if (!s_m) {
    return;
  }
  char* buf = (char*)heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
  if (!buf) {
    return;
  }
  const size_t n = geminiQuotaSave(&s_m->quota, utcNow(), buf, 512);
  if (n < 512 && !writeFile(AI_STATE_FILE, AI_STATE_TMP, buf, n)) {
    log_e("AI: the card refused %s", AI_STATE_FILE);
  }
  free(buf);
}

size_t aiQuotaNote(char* out, size_t cap) {
  if (!out || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  quotaLoad();
  if (!s_m) {
    return 0;
  }
  const char* model = s_m->key.model[0] ? s_m->key.model : GEM_DEFAULT_MODEL;
  const int64_t now = utcNow();
  const uint32_t ms = millis();
  int64_t until;
  uint32_t remain;
  if (!geminiQuotaWhen(&s_m->quota, model, now, ms, &until, &remain)) {
    return 0;
  }
  if (s_m->key.fallback[0] && geminiQuotaOut(&s_m->quota, s_m->key.fallback, now, ms)) {
    geminiQuotaBack(&s_m->quota, model, s_m->key.fallback, now, ms, &until, &remain);
    return geminiQuotaAllOut(until, now, tzNow(), remain, out, cap);
  }
  char a[GEM_LABEL_MAX], b[GEM_LABEL_MAX];
  geminiModelLabel(model, a, sizeof(a));
  geminiModelLabel(s_m->key.fallback, b, sizeof(b));
  return geminiQuotaNotice(a, s_m->key.fallback[0] ? b : "", until, now, tzNow(), remain, out, cap);
}

// ── "Retry if it times out" (round 3) ────────────────────────────────────────────────────

#define AI_PREFS_NS     "wpmesh"                 // the firmware's settings namespace (prefs_almanac.cpp)
#define AI_PREFS_RETRY  "ai_retry"               // uchar: 1 on, 0 off; absent = ON
#define AI_PREFS_MODEL  "ai_model"               // D-175: string, a choice id (chutes.h); absent = the default rule
#define AI_PREFS_WEB    "ai_web"                 // D-176: string, a web search mode id (websearch.h); absent = Auto
#define AI_PREFS_WEBD   "ai_webd"                // D-176: int, the local day the counts below belong to
#define AI_PREFS_WEBN   "ai_webn"                // D-176: ushort, Auto's searches that day (WEB_DAILY_AUTO at most)
#define AI_PREFS_WEBO   "ai_webo"                // D-176: ushort, the forced and Always ones that day

bool aiRetryTimeouts() {
  if (!memOk()) {
    return true;                                   // the default
  }
  if (!s_m->retryLoaded) {
    s_m->retryLoaded = true;
    s_m->retryOn = true;
    Preferences p;
    /* Read-only begin() FAILS when the namespace was never written (a new phone): ON stands. */
    if (p.begin(AI_PREFS_NS, true)) {
      s_m->retryOn = p.getUChar(AI_PREFS_RETRY, 1) != 0;
      p.end();
    }
  }
  return s_m->retryOn;
}

void aiSetRetryTimeouts(bool on) {
  if (!memOk()) {
    return;
  }
  s_m->retryLoaded = true;
  s_m->retryOn = on;
  Preferences p;
  if (p.begin(AI_PREFS_NS, false)) {
    p.putUChar(AI_PREFS_RETRY, on ? 1 : 0);
    p.end();
  } else {
    log_e("AI: could not open NVS '%s' to save %s=%d (kept for this boot only)", AI_PREFS_NS, AI_PREFS_RETRY,
          on ? 1 : 0);
  }
}

// ── the model choice (D-175) ─────────────────────────────────────────────────────────────

/* NVS "wpmesh" / "ai_model", read once. NVS key names are 15 characters at most ("ai_model" is 8).
 * isKey() first: getString() of a key that was never written logs an error line (Preferences.cpp). */
static void modelLoad() {
  if (s_m->modelLoaded) {
    return;
  }
  s_m->modelLoaded = true;
  s_m->modelStored[0] = '\0';
  Preferences p;
  /* Read-only begin() FAILS when the namespace was never written (a new phone): the default rule. */
  if (p.begin(AI_PREFS_NS, true)) {
    if (p.isKey(AI_PREFS_MODEL)) {
      p.getString(AI_PREFS_MODEL, s_m->modelStored, sizeof(s_m->modelStored));
    }
    p.end();
  }
}

int aiModelChoice() {
  if (!memOk()) {
    return CHUTES_GEMINI;                          // the phone of before
  }
  modelLoad();
  /* Set: that, whatever the keys. Unset: v4flash (or chutes.txt's model=) with a usable Chutes key,
   * else Gemini - chutes.h's rule, host-tested. */
  return chutesChoiceResolve(s_m->modelStored, s_m->ckey.usable, s_m->ckey.preset);
}

bool aiModelChoiceStored() {
  if (!memOk()) {
    return false;
  }
  modelLoad();
  return chutesChoiceFind(s_m->modelStored) != CHUTES_NOT_A_CHOICE;
}

void aiSetModelChoice(int choice) {
  if (!memOk() || (choice != CHUTES_GEMINI && !chutesPreset(choice))) {
    return;                                        // the allowlist: nothing else is ever stored
  }
  s_m->modelLoaded = true;
  strlcpy(s_m->modelStored, chutesChoiceId(choice), sizeof(s_m->modelStored));
  Preferences p;
  if (p.begin(AI_PREFS_NS, false)) {
    p.putString(AI_PREFS_MODEL, s_m->modelStored);
    p.end();
  } else {
    log_e("AI: could not open NVS '%s' to save %s=%s (kept for this boot only)", AI_PREFS_NS, AI_PREFS_MODEL,
          s_m->modelStored);
  }
}

void aiClearModelChoice() {
  if (!memOk()) {
    return;
  }
  s_m->modelLoaded = true;
  s_m->modelStored[0] = '\0';
  Preferences p;
  if (p.begin(AI_PREFS_NS, false)) {
    if (p.isKey(AI_PREFS_MODEL)) {
      p.remove(AI_PREFS_MODEL);
    }
    p.end();
  }
}

bool aiChoiceUsable(int choice) {
  if (!memOk()) {
    return false;
  }
  return chutesPreset(choice) ? s_m->ckey.usable : s_m->key.usable;
}

// ── the web search's setting and the day's counts (D-176) ─────────────────────────────────

/* NVS "wpmesh" / "ai_web" (a mode id: websearch.h), "ai_webd" (the local day of the counts, days since
 * 1970), "ai_webn" (Auto's that day) and "ai_webo" (forced and Always ones) - every name 15 characters
 * at most. isKey() first, as modelLoad's (an absent key logs an error line in Preferences.cpp). */
static void webModeLoad() {
  if (s_m->webLoaded) {
    return;
  }
  s_m->webLoaded = true;
  s_m->webStored[0] = '\0';
  Preferences p;
  if (p.begin(AI_PREFS_NS, true)) {                // read-only begin() fails on a new phone: the default
    if (p.isKey(AI_PREFS_WEB)) {
      p.getString(AI_PREFS_WEB, s_m->webStored, sizeof(s_m->webStored));
    }
    p.end();
  }
}

int aiWebMode() {
  if (!memOk()) {
    return WEB_AUTO;
  }
  webModeLoad();
  return webModeResolve(s_m->webStored);           // unset (or a value this firmware does not know): Auto
}

bool aiWebModeStored() {
  if (!memOk()) {
    return false;
  }
  webModeLoad();
  return webModeFind(s_m->webStored) >= 0;
}

void aiSetWebMode(int mode) {
  if (!memOk() || mode < 0 || mode >= WEB_MODES) {
    return;                                        // the allowlist: nothing else is ever stored
  }
  s_m->webLoaded = true;
  strlcpy(s_m->webStored, webModeId(mode), sizeof(s_m->webStored));
  Preferences p;
  if (p.begin(AI_PREFS_NS, false)) {
    p.putString(AI_PREFS_WEB, s_m->webStored);
    p.end();
  } else {
    log_e("AI: could not open NVS '%s' to save %s=%s (kept for this boot only)", AI_PREFS_NS, AI_PREFS_WEB,
          s_m->webStored);
  }
}

bool aiWebUsable() {
  return memOk() && s_m->wkey.usable && s_wcfg && s_wHaveGood;
}

static void dayLoad() {
  if (s_m->dayLoaded) {
    return;
  }
  s_m->dayLoaded = true;
  s_m->day.day = -1;
  s_m->day.autoN = 0;
  s_m->day.otherN = 0;
  Preferences p;
  if (p.begin(AI_PREFS_NS, true)) {
    if (p.isKey(AI_PREFS_WEBD)) {
      s_m->day.day = p.getInt(AI_PREFS_WEBD, -1);
      s_m->day.autoN = p.isKey(AI_PREFS_WEBN) ? (int)p.getUShort(AI_PREFS_WEBN, 0) : 0;
      s_m->day.otherN = p.isKey(AI_PREFS_WEBO) ? (int)p.getUShort(AI_PREFS_WEBO, 0) : 0;
    }
    p.end();
  }
}

static void daySave() {
  Preferences p;
  if (p.begin(AI_PREFS_NS, false)) {
    p.putInt(AI_PREFS_WEBD, (int32_t)s_m->day.day);
    p.putUShort(AI_PREFS_WEBN, (uint16_t)s_m->day.autoN);
    p.putUShort(AI_PREFS_WEBO, (uint16_t)s_m->day.otherN);
    p.end();
  } else {
    log_e("AI: could not open NVS '%s' to save the web search counts (kept for this boot only)", AI_PREFS_NS);
  }
}

/* The local day now: a trusted clock only (utcNow() is 0 otherwise: -1, and the counts stay on the day
 * they hold - the cap never resets on an unknown day). */
static int32_t webToday() {
  return webLocalDay(utcNow(), tzNow());
}

void aiWebToday(int* autoN, int* otherN) {
  if (!memOk()) {
    *autoN = *otherN = 0;
    return;
  }
  dayLoad();
  WebDay d = s_m->day;
  webDayRoll(&d, webToday());
  *autoN = d.autoN;
  *otherN = d.otherN;
}

// ── the wire (WORKER) ────────────────────────────────────────────────────────────────────

/* One POST, through the shared worker's exchange (https_worker.cpp: netHttps - the code that was
 * this file's postOnce/readBody until 0.9.81, the same steps in the same order: DNS first, a new
 * WiFiClientSecure with Google's roots, HTTPClient's own connect under AI_CONNECT_MS, the key in a
 * header, the chunked body de-chunked by hand into the PSRAM sink). The HTTP code (> 0), or 0 with
 * *net saying what failed. A body that could not be read is a failure even under a 200, and so is
 * one past the sink (GEM_NET_TOO_BIG, as before). */
static int postOnce(const char* model, size_t bodyLen, size_t* got, int* net) {
  *got = 0;
  *net = GEM_NET_OK;
  char url[sizeof(GEM_URL_PREFIX) + GEM_MODEL_MAX + sizeof(GEM_URL_SUFFIX)];
  snprintf(url, sizeof(url), "%s%s%s", GEM_URL_PREFIX, model, GEM_URL_SUFFIX);
  NetHttpReq q;
  memset(&q, 0, sizeof(q));
  q.method = "POST";
  q.host = GEM_HOST;
  q.url = url;
  q.rootsPem = AI_ROOTS_PEM;
  q.userAgent = AI_USER_AGENT;
  q.hdrName[0] = "Content-Type";
  q.hdrValue[0] = "application/json";
  q.hdrName[1] = "x-goog-api-key";               // ⚠ a header, never the URL
  q.hdrValue[1] = s_req->cfg.key;
  q.body = (const uint8_t*)s_body;
  q.bodyLen = bodyLen;
  q.connectMs = AI_CONNECT_MS;
  /* The heap bar again right before EVERY Gemini handshake (review 2026-10-11): the Wikipedia
   * lookup's handshake - or a retry's wait - can leave the heap fragmented since aiAsk looked, and
   * a handshake started under the bar fails as a generic TLS error. Under it: GEM_NET_NOMEM, and
   * aiRun says "reboot first" with the numbers. */
  q.heapCheck = true;
  q.timeoutMs = AI_TIMEOUT_MS;
  q.waitSliceMs = AI_SLICE_MS;                   // the wait for the answer: the client's own, cancellable
  q.bodyIdleMs = AI_BODY_IDLE_MS;
  q.cancel = &s_cancel;
  /* ...and a CALL ends it too (review 2026-10-11): tileFetchCallRecent is the loop's per-pass
   * flag from the SIP state (a live or ringing call, or the minute after one - which aiAsk's
   * gate already refused, so true here means one rang in). Up to 150 s behind a ringing phone
   * was the alternative; aiRun says "A call came in". */
  q.stopFn = tileFetchCallRecent;
  q.sink = netWorkerSink();
  q.cap = AI_SINK_CAP;
  NetHttpRes r;
  int code = netHttps(&q, &r);
  if (r.largestAfterSend && r.largestAfterSend < s_res->largestFloor) {
    s_res->largestFloor = r.largestAfterSend;
  }
  *net = r.net;
  *got = r.got;
  if (code > 0 && r.overflow) {
    *net = GEM_NET_TOO_BIG;
    code = 0;
  }
  return code;
}

/* D-175: one POST to Chutes, through the same exchange as Gemini's (netHttps): DNS first, a fresh
 * client with the Chutes ROOT SET pinned (AI_CHUTES_ROOTS_PEM), the heap bar right before the
 * handshake, the client's own cancellable first-byte wait - the PRESET's (60 s, 120 s for the thinking
 * ones: the whole answer is written before a byte comes) - Cancel and a call, the shared sink.
 * ⚠ THE KEY'S ONLY WAY OUT: `Authorization: Bearer <key>`, built here into PSRAM (chutesAuthHeader:
 * nothing that is not cpk_... is ever sent as one), to CHUTES_HOST only (a constant URL; HTTPClient
 * follows no redirect unless told to, and nothing here tells it), and WIPED as soon as netHttps
 * returns (HTTPClient's own header copy dies with it). The HTTP code (> 0), or 0 with *net saying
 * what failed; past the sink is GEM_NET_TOO_BIG, as Gemini's. */
static int postOnceChutes(const ChutesPreset* p, size_t bodyLen, size_t* got, int* net) {
  *got = 0;
  *net = GEM_NET_OK;
  if (chutesAuthHeader(s_req->chutes.key, s_work->auth, sizeof(s_work->auth)) == 0 || !s_work->auth[0]) {
    *net = GEM_NET_NOMEM;                          // no key that can go (chutesParseConfig refused it first)
    return 0;
  }
  NetHttpReq q;
  memset(&q, 0, sizeof(q));
  q.method = "POST";
  q.host = CHUTES_HOST;
  q.url = CHUTES_URL;
  q.rootsPem = AI_CHUTES_ROOTS_PEM;
  q.userAgent = AI_USER_AGENT;
  q.hdrName[0] = "Content-Type";
  q.hdrValue[0] = "application/json";
  q.hdrName[1] = "Authorization";                // ⚠ a header, never the URL - and only to CHUTES_HOST
  q.hdrValue[1] = s_work->auth;
  q.body = (const uint8_t*)s_body;
  q.bodyLen = bodyLen;
  q.connectMs = AI_CONNECT_MS;
  q.heapCheck = true;                            // the bar before EVERY handshake (Gemini's rule)
  q.timeoutMs = p->waitMs;
  q.waitSliceMs = AI_SLICE_MS;
  q.bodyIdleMs = AI_BODY_IDLE_MS;
  q.cancel = &s_cancel;
  q.stopFn = tileFetchCallRecent;
  q.sink = netWorkerSink();
  q.cap = AI_SINK_CAP;
  NetHttpRes r;
  int code = netHttps(&q, &r);
  memset(s_work->auth, 0, sizeof(s_work->auth));
  if (r.largestAfterSend && r.largestAfterSend < s_res->largestFloor) {
    s_res->largestFloor = r.largestAfterSend;
  }
  *net = r.net;
  *got = r.got;
  if (code > 0 && r.overflow) {
    *net = GEM_NET_TOO_BIG;
    code = 0;
  }
  return code;
}

static bool waitSliced(uint32_t ms) {
  const uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    if (s_cancel) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(AI_SLICE_MS));
  }
  return !s_cancel;
}

/* aiAsk's gates, and its reasons: WiFi, then the gates of a map download's Start (tile_fetch.cpp):
 * a running download keeps a TLS session of its own (two handshakes ~24 KB); the uploader and a
 * sync window run the radio as a server; a call's audio cannot share the handshake's heap dip; a
 * game owns the card and the SPI bus; a Files folder job is the card's too. NULL = clear. Cheap
 * flag reads: aiAsk asks on the loop, and the worker again before the first request of a question
 * that waited behind a weather fetch (final review 2026-10-03: that wait can be a minute). */
static const char* aiGateWhy() {
  if (wifiState.radioOff()) {
    return "WiFi is off (Settings > WiFi)";
  }
  if (WiFi.status() != WL_CONNECTED) {
    return "No WiFi - join a network first";
  }
  if (tileFetchActive()) {
    return "A map download is running";
  }
  if (xferOn()) {
    return "The WiFi uploader is on - stop it first";
  }
  if (kosyncWindowActive() || xferWindowUp()) {
    return "A book sync window is open";
  }
  if (tileFetchCallRecent()) {
    return "A call is on, or ended under a minute ago";
  }
  if (gGbcActive) {
    return "A game is running";
  }
  if (filesJobActive()) {
    return "A Files folder job is running - let it finish";
  }
  return NULL;
}

/* The topic's earlier questions - the context aiAsk copied, oldest first - as pointers for
 * wikiKeywords. Returns how many (0 when the question starts a topic). */
static int topicQuestions(const AiReq* q, const char* out[GEM_CONTEXT]) {
  int n = 0;
  for (int i = 0; i < q->nCtx && i < GEM_CONTEXT; i++) {
    out[n++] = q->ctxQ[i];
  }
  return n;
}

/* THE WIKIPEDIA LOOKUP (WORKER; once a question, before its first request - never per retry; see
 * wiki_lookup.h). ONE GET, never retried whatever happens: Wikipedia answered 429 after ~8 quick
 * requests, and the question is worth asking without it. PRIVACY: the URL is built from the
 * question's KEYWORDS alone - no key (no header but the User-Agent), no whole question, no chat.
 * Fills s_work->bg and ->src, or leaves both "" with r->wikiWhy saying why (aiLoopTick logs it):
 * any failure is silent and harmless, and the question goes to Gemini as it would have.
 * noinline ON PURPOSE: inlined (as it was, into aiRun), its request and response would sit in
 * aiRun's frame under every Gemini handshake that follows, on the worker's 8 KB stack. */
static void __attribute__((noinline)) aiWikiLookup(const AiReq* q, AiRes* r) {
  s_work->bg[0] = '\0';
  s_work->src[0] = '\0';
  r->wikiWhy[0] = '\0';
  r->wikiPages = 0;
  if (!q->cfg.lookup) {
    r->wikiNot = q->choice == CHUTES_GEMINI ? "lookup=off in gemini.txt" : "lookup=off in chutes.txt";
    return;                                        // the key file's lookup=off (a Chutes question: chutes.txt's)
  }
  /* A follow-up ("and when?") borrows from the topic's newest question that named something
   * (round 3: wikiKeywords walks back) - the questions sent as context, oldest first; none when
   * this question starts a topic. */
  const char* earlier[GEM_CONTEXT];
  const int nEarlier = topicQuestions(q, earlier);
  if (wikiKeywords(q->question, earlier, nEarlier, s_work->kw, sizeof(s_work->kw)) == 0) {
    r->wikiNot = "no keywords in the question";
    return;                                        // nothing to search for: no lookup at all
  }
  if (wikiUrl(s_work->kw, s_work->wikiUrl, sizeof(s_work->wikiUrl)) >= sizeof(s_work->wikiUrl)) {
    r->wikiNot = "the keywords did not fit the URL";
    strlcpy(r->wikiWhy, r->wikiNot, sizeof(r->wikiWhy));
    return;
  }
  progSet("Looking up Wikipedia...", NULL, 0, -1);
  const uint32_t t0 = millis();
  NetHttpReq w;
  memset(&w, 0, sizeof(w));
  w.method = "GET";
  w.host = WIKI_HOST;
  w.url = s_work->wikiUrl;                         // ⚠ never logged: the question's words are in it
  w.rootsPem = AI_WIKI_ROOTS_PEM;
  w.userAgent = WIKI_USER_AGENT;                   // Wikimedia's policy: a name and a contact
  w.connectMs = AI_WIKI_CONNECT_MS;
  w.timeoutMs = AI_WIKI_TOTAL_MS;
  w.waitSliceMs = AI_SLICE_MS;                     // the client's own first-byte wait: Cancel and a call end it
  w.bodyIdleMs = AI_WIKI_IDLE_MS;
  w.totalMs = AI_WIKI_TOTAL_MS;                    // the whole lookup, DNS included
  w.connectFirst = true;                           // the handshake timed alone, under its cap
  w.heapCheck = true;                              // the bar again before THIS handshake
  w.cancel = &s_cancel;
  w.stopFn = tileFetchCallRecent;                  // a call rang in: stop (aiRun then says so)
  w.sink = netWorkerSink();
  w.cap = WIKI_RESPONSE_CAP;
  NetHttpRes res;
  int code = netHttps(&w, &res);
  r->wikiAsked = true;
  r->wikiMs = millis() - t0;
  if (res.largestAfterSend && res.largestAfterSend < r->largestFloor) {
    r->largestFloor = res.largestAfterSend;
  }
  int net = res.net;
  if (code > 0 && res.overflow) {
    code = 0;
    net = GEM_NET_TOO_BIG;
  }
  if (s_cancel) {
    strlcpy(r->wikiWhy, "cancelled", sizeof(r->wikiWhy));
    return;
  }
  if (tileFetchCallRecent()) {
    strlcpy(r->wikiWhy, "a call came in", sizeof(r->wikiWhy));
    return;
  }
  if (code != 200) {
    wikiSkipWhy(code, net, WIKI_BAD, r->wikiWhy, sizeof(r->wikiWhy));
    return;
  }
  const int parsed = wikiParse(netWorkerSink(), res.got, &s_work->wiki);
  if (parsed != WIKI_OK) {
    wikiSkipWhy(200, GEM_NET_OK, parsed, r->wikiWhy, sizeof(r->wikiWhy));
    return;
  }
  /* Only the pages ABOUT the keywords (round 3), the background, and the sources line naming exactly
   * them - all or nothing: no page left, none that fits, or no title the line can show, and neither
   * goes (wikiCompose). */
  int why = WIKI_OK;
  const int used = wikiCompose(&s_work->wiki, s_work->kw, WIKI_BG_MAX, s_work->bg, sizeof(s_work->bg), s_work->src,
                               sizeof(s_work->src), &why);
  if (used <= 0) {
    s_work->bg[0] = '\0';
    s_work->src[0] = '\0';
    wikiSkipWhy(200, GEM_NET_OK, why, r->wikiWhy, sizeof(r->wikiWhy));
    return;
  }
  r->wikiPages = used;
}

/* D-176: every de-chunked body byte of the search, as it arrives (https_worker's tap), into the
 * streaming parser - which keeps the fields it needs and never the body (the sink keeps nothing:
 * cap 0). On the worker, inside netHttps's body read: a cheap state machine, no allocation. */
static void webTap(void* ctx, const char* p, size_t n) {
  webParseFeed((WebParse*)ctx, p, n);
}

/* THE WEB SEARCH (WORKER; D-176, websearch.h). Once a question, AFTER the Wikipedia lookup and before
 * either provider - never per retry, never retried: ONE POST to api.tavily.com. PRIVACY: the body is
 * the question (whitespace collapsed, cut at a word to 200 bytes) and the fixed fields - no chat, no
 * other key. ⚠ THE TAVILY KEY'S ONLY WAY OUT: `Authorization: Bearer <key>`, built here into PSRAM
 * (webAuthHeader: nothing that is not tvly-... is ever sent as one), to WEB_HOST only (a constant URL;
 * HTTPClient follows no redirect unless told to, and nothing here tells it) and WIPED as soon as
 * netHttps returns. The reply goes through the tap into the parser as it arrives; the block is built
 * from what it kept and goes AFTER Wikipedia's in s_work->bg; the "Web: a; b" line - or the note that
 * says why nothing went ("Web: search unavailable") - after the Wikipedia line in s_work->src. Any
 * failure is the note and Wikipedia alone: the question is asked all the same. No text of Tavily's is
 * ever kept, shown or logged. noinline: its request and the block's locals are not aiRun's frame under
 * every handshake that follows. */
static void __attribute__((noinline)) aiWebSearch(const AiReq* q, AiRes* r) {
  s_work->web[0] = '\0';
  s_work->webSrc[0] = '\0';
  if (q->webOnly) {
    s_work->bg[0] = '\0';                          // `ai web test`: no lookup ran to clear them
    s_work->src[0] = '\0';
  }
  r->webNote = q->webNote;                         // WEB_NOTE_CAP, or NONE until the search says
  if (!q->webDec.search) {
    if (r->webNote != WEB_NOTE_NONE) {
      webAppendLine(s_work->src, sizeof(s_work->src), webNoteLine(r->webNote));
    }
    return;
  }
  const size_t bn = webBuildRequest(q->question, q->webDec.topic, q->webDec.timeRange, s_work->webBody,
                                    sizeof(s_work->webBody));
  if (bn == 0 || bn >= sizeof(s_work->webBody) || webAuthHeader(q->web.key, s_work->webAuth, sizeof(s_work->webAuth)) == 0 ||
      !s_work->webAuth[0]) {
    memset(s_work->webAuth, 0, sizeof(s_work->webAuth));
    r->webNote = WEB_NOTE_UNAVAILABLE;             // no body or no key that can go (webParseConfig refused it first)
    webAppendLine(s_work->src, sizeof(s_work->src), webNoteLine(r->webNote));
    return;
  }
  progSet(WEB_STAGE, NULL, 0, -1);
  webParseInit(&s_work->wp);
  const uint32_t t0 = millis();
  NetHttpReq w;
  memset(&w, 0, sizeof(w));
  w.method = "POST";
  w.host = WEB_HOST;
  w.url = WEB_URL;                                 // ⚠ a constant: the Bearer never goes anywhere else
  w.rootsPem = AI_TAVILY_ROOTS_PEM;
  w.userAgent = AI_USER_AGENT;
  w.hdrName[0] = "Content-Type";
  w.hdrValue[0] = "application/json";
  w.hdrName[1] = "Authorization";                  // ⚠ a header, never the URL or the body - only to WEB_HOST
  w.hdrValue[1] = s_work->webAuth;
  w.body = (const uint8_t*)s_work->webBody;
  w.bodyLen = bn;
  w.connectMs = AI_WEB_CONNECT_MS;                 // TCP + the handshake, timed alone (connectFirst)
  w.timeoutMs = WEB_ANSWER_MS;                     // Tavily's answer: the client's own first-byte wait
  w.waitSliceMs = AI_SLICE_MS;                     // ...in slices: Cancel and a call end it
  w.bodyIdleMs = AI_WEB_IDLE_MS;
  w.totalMs = AI_WEB_TOTAL_MS;                     // the whole, DNS to the last byte
  w.connectFirst = true;
  w.heapCheck = true;                              // the bar again before THIS handshake
  w.cancel = &s_cancel;
  w.stopFn = tileFetchCallRecent;                  // a call rang in: stop (aiRun then says so)
  w.sink = netWorkerSink();
  w.cap = 0;                                       // nothing raw is kept: the tap parses it as it comes
  w.tap = webTap;
  w.tapCtx = &s_work->wp;
  NetHttpRes res;
  const int code = netHttps(&w, &res);
  memset(s_work->webAuth, 0, sizeof(s_work->webAuth));
  r->webAsked = true;
  r->webSent = res.largestAfterSend != 0 || code > 0;   // set only once sendRequest ran (https_worker.h)
  r->webMs = millis() - t0;
  r->webHsMs = res.hsMs;
  r->webCode = code;
  r->webNet = res.net;
  if (res.largestAfterSend && res.largestAfterSend < r->largestFloor) {
    r->largestFloor = res.largestAfterSend;
  }
  if (s_cancel || tileFetchCallRecent()) {
    return;                                        // aiRun ends the question on these: nothing to add
  }
  const int parsed = code > 0 ? webParseFinish(&s_work->wp) : WEB_PARSE_BAD;
  r->webParsed = parsed;
  r->webRespMs = s_work->wp.responseMs;
  r->webCredits = s_work->wp.credits;
  int note = webNoteKind(code, res.net, code > 0 ? &s_work->wp : NULL, parsed);
  if (note == WEB_NOTE_NONE) {
    /* "searched <date>": the phone's local day from a trusted clock, else the reply's own Date header
     * (UTC), else "just now". */
    char date[16];
    if (!webDateLocal(utcNow(), q->tzS, date, sizeof(date))) {
      webDateOf(res.date, date, sizeof(date));
    }
    int used = 0;
    const size_t n = webBuildBlock(&s_work->wp, date, WEB_BLOCK_MAX, s_work->web, sizeof(s_work->web), s_work->webSrc,
                                   sizeof(s_work->webSrc), &used);
    r->webKept = used;
    r->webBlockLen = (uint32_t)n;
    if (n == 0 || !webAppendBlock(s_work->bg, sizeof(s_work->bg), s_work->web)) {
      note = WEB_NOTE_NOTHING;                     // nothing fitted (all or nothing)
      r->webKept = 0;
    }
    if (!q->webOnly) {
      /* A question's block lives on in bg for its requests only: this copy is wiped now, so no
       * question's block outlives it where the bench could print it (review 2026-10-19). */
      memset(s_work->web, 0, sizeof(s_work->web));
    }
  }
  r->webNote = note;
  webAppendLine(s_work->src, sizeof(s_work->src), note == WEB_NOTE_NONE ? s_work->webSrc : webNoteLine(note));
}

/* D-175: a CHUTES question's requests, after aiRun's gates and its one Wikipedia lookup - Gemini's
 * loop in shape (a LOOP, never recursion; the date per request; the background in the question's
 * turn; Cancel and a call ended before the ladder can call them a timeout; the answer scrubbed), with
 * chutes.h's ladder. noinline: its frame is not aiRun's under every handshake. */
static void __attribute__((noinline)) aiRunChutes(AiReq* q, AiRes* r, const GeminiTurnPair* ctx) {
  ChutesLadder& l = s_work->cl;
  char* label = s_work->label;
  const ChutesPreset* p = chutesPreset(l.cur);
  strlcpy(label, p->label, GEM_LABEL_MAX);
  progShort(p->shortName);
  if (l.onFallback) {
    progSet(l.note, label, 0, 0);                // the asked preset was seen gone this boot
  } else {
    snprintf(s_work->stage, sizeof(s_work->stage), q->musicPaused ? "Music paused - asking %s..." : "Asking %s...",
             p->shortName);
    progSet(s_work->stage, label, 0, 0);
  }
  ChutesReply* rep = (ChutesReply*)heap_caps_calloc(1, sizeof(ChutesReply), MALLOC_CAP_SPIRAM);
  if (!rep) {
    r->net = GEM_NET_NOMEM;
    chutesFailText(0, GEM_NET_NOMEM, NULL, NULL, q->chutes.key, r->fail, sizeof(r->fail));
    return;
  }
  uint32_t seed = 0;
  for (;;) {
    if (s_cancel) {
      r->cancelled = true;
      break;
    }
    if (r->attempts > 0 && tileFetchCallRecent()) {
      strlcpy(r->fail, "A call came in - ask again after it", sizeof(r->fail));
      break;
    }
    p = chutesPreset(l.cur);
    /* The system text NOW: today's date from a trusted clock only (utcNow() is 0 otherwise), then the
     * Chutes line. The background goes in the QUESTION'S turn, never in here. */
    chutesSystemText(utcNow(), q->tzS, q->zone, l.cur, s_work->sys, sizeof(s_work->sys));
    seed = chutesSeed(esp_random(), seed);       // a retry is never the same draw
    const size_t n = chutesBuildRequest(l.cur, ctx, q->nCtx, q->question, s_work->bg, s_work->sys, seed, NULL, 0);
    if (n + 1 > s_bodyCap) {
      free(s_body);
      s_bodyCap = 0;
      s_body = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
      if (!s_body) {
        r->net = GEM_NET_NOMEM;
        chutesFailText(0, GEM_NET_NOMEM, NULL, &l, q->chutes.key, r->fail, sizeof(r->fail));
        break;
      }
      s_bodyCap = n + 1;
    }
    chutesBuildRequest(l.cur, ctx, q->nCtx, q->question, s_work->bg, s_work->sys, seed, s_body, s_bodyCap);
    const uint32_t ta = millis();
    size_t got = 0;
    int net = GEM_NET_OK;
    const int code = postOnceChutes(p, n, &got, &net);
    r->lastMs = millis() - ta;
    r->lastCode = code;
    r->net = net;
    r->attempts++;
    r->bodyBytes = (uint32_t)got;
    progSet(NULL, NULL, 0, r->attempts);
    if (code <= 0 && s_cancel) {
      r->cancelled = true;                       // the user's word - never a timeout for rule 6
      break;
    }
    if (code <= 0 && !s_cancel && tileFetchCallRecent()) {
      strlcpy(r->fail, "A call came in - ask again after it", sizeof(r->fail));
      break;
    }
    if (code > 0) {
      chutesParseReply(l.cur, netWorkerSink(), got, s_work->text, sizeof(s_work->text), rep);
    } else {
      memset(rep, 0, sizeof(*rep));
      rep->kind = CHUTES_REPLY_BAD;
    }
    /* The answer as shown, BEFORE the ladder (the WiPhone's review, 2026-10-19): a text the screen
     * would not show (an emoji alone) is no text, so an "abort" with it is busy, not "empty". l.cur:
     * the preset that answered - a "content_filter" after text names it (chutes.h). The sources: Gemini's
     * rule (D-176: Wikipedia's line, then the web's). */
    chutesAnswerShown(rep, l.cur, s_work->text, s_work->src[0] ? s_work->src : NULL, r->answer, sizeof(r->answer));
    uint32_t delayMs = 0;
    const char* stage = NULL;
    const int step = chutesLadderNext(&l, code, net, rep, millis(), &delayMs, &stage);
    if (step == GEM_STEP_DONE) {
      s_m->chutesAnswered = true;                // a 200: the key works - a later 429 is load, not the key
      if (rep->kind == CHUTES_REPLY_TEXT && r->answer[0]) {
        r->ok = true;
        geminiScrub(r->answer, sizeof(r->answer), q->chutes.key);   // a model can echo what it was sent
        chutesAnswerLabel(&l, r->label, sizeof(r->label));
      } else {
        chutesNoTextMessage(rep, l.cur, r->fail, sizeof(r->fail));
      }
      break;
    }
    if (step == GEM_STEP_FAIL) {
      if (code <= 0 && net == GEM_NET_NOMEM) {
        lowMemText(r->fail, sizeof(r->fail));    // the heap bar (q.heapCheck) or the handshake's alloc
      } else {
        chutesFailText(code, net, rep, &l, q->chutes.key, r->fail, sizeof(r->fail));
      }
      break;
    }
    p = chutesPreset(l.cur);
    strlcpy(label, p->label, GEM_LABEL_MAX);
    snprintf(s_work->stage, sizeof(s_work->stage), "Asking %s...", p->shortName);
    progShort(p->shortName);
    progSet(stage ? stage : s_work->stage, label, delayMs ? millis() + delayMs : 0, -1);
    if (delayMs) {
      if (!waitSliced(delayMs)) {
        r->cancelled = true;
        break;
      }
      progSet("Asking again...", NULL, 0, -1);
    }
  }
  s_m->chutesGone = l.goneMask;                  // a model gone this boot is skipped by the next question
  geminiWipe(rep, sizeof(*rep));
  free(rep);
}

/* The ladder for one question: a LOOP (the T-Deck's first retry called itself mid-TLS and died). */
static void aiRun() {
  AiReq* q = s_req;
  AiRes* r = s_res;
  const uint32_t t0 = millis();
  heapNow(NULL, &r->largestFloor, NULL);
  r->wikiNot = "the question ended before the lookup";   // aiWikiLookup says otherwise once it runs
  GeminiTurnPair ctx[GEM_CONTEXT];
  for (int i = 0; i < q->nCtx; i++) {
    ctx[i].q = q->ctxQ[i];
    ctx[i].a = q->ctxA[i];
  }
  if (q->heapCheckAtStart) {
    /* Asked while a weather fetch held the worker: aiAsk's gates again (the wait was long enough for
     * a call to ring in or a download to start), then its heap gate, here, now that it is done. */
    const char* gate = aiGateWhy();
    if (gate) {
      strlcpy(r->fail, gate, sizeof(r->fail));
      r->ms = millis() - t0;
      return;
    }
    if (!netHeapOkForHandshake(NULL, NULL)) {
      r->net = GEM_NET_NOMEM;
      lowMemText(r->fail, sizeof(r->fail));
      r->ms = millis() - t0;
      return;
    }
  }
  if (q->webOnly) {
    /* `ai web test` (D-176): the search alone, exactly as a question makes it - no lookup, no model. */
    aiWebSearch(q, r);
    r->cancelled = s_cancel;
    r->ms = millis() - t0;
    return;
  }
  char* label = s_work->label;
  char* primaryLabel = s_work->primaryLabel;
  char* stageBuf = s_work->stage;
  const size_t stageCap = sizeof(s_work->stage);
  /* D-175: the choice the question was asked with. Chutes has its own ladder (chutes.h); everything
   * else up to the requests - the gates above, the lookup below, Cancel and a call - is shared. */
  const bool chutes = q->choice != CHUTES_GEMINI;
  GeminiLadder& l = s_work->ladder;
  int step = GEM_STEP_RETRY;
  if (chutes) {
    chutesLadderStart(&s_work->cl, q->choice, s_m->chutesGone, !s_m->chutesAnswered, millis());
    s_work->cl.retryTimeouts = q->retryTimeouts;   // "Retry if it times out": Chutes' rule 6
    strlcpy(label, chutesPreset(s_work->cl.cur)->label, GEM_LABEL_MAX);
  } else {
    geminiModelLabel(q->cfg.model, primaryLabel, GEM_LABEL_MAX);
    step = geminiLadderStart(&l, &q->cfg, &s_m->session, &q->quota, utcNow(), millis());
    l.retryTimeouts = q->retryTimeouts;   // "Retry if it times out" (the ladder's rule 6): the setting
    if (step == GEM_STEP_FAIL) {
      geminiFailText(0, GEM_NET_OK, NULL, &l, q->cfg.key, q->tzS, r->fail, sizeof(r->fail));
      r->ms = millis() - t0;
      return;
    }
    geminiModelLabel(l.model, label, GEM_LABEL_MAX);
  }
  /* What Wikipedia says, looked up once before the first request (a model is there to ask: the
   * ladder started). A Cancel or a call during it ends the question here. */
  progSet(NULL, label, 0, 0);
  aiWikiLookup(q, r);
  if (s_cancel) {
    r->cancelled = true;
    r->ms = millis() - t0;
    return;
  }
  if (tileFetchCallRecent()) {
    strlcpy(r->fail, "A call came in - ask again after it", sizeof(r->fail));
    r->ms = millis() - t0;
    return;
  }
  /* D-176: the web search, once, AFTER the lookup and before either provider (one HTTPS worker: one TLS
   * session at a time). It adds its block after Wikipedia's and its line after Wikipedia's - or, when it
   * does not search, nothing (the cap's note aside). A Cancel or a call during it ends the question. */
  aiWebSearch(q, r);
  if (s_cancel) {
    r->cancelled = true;
    r->ms = millis() - t0;
    return;
  }
  if (tileFetchCallRecent()) {
    strlcpy(r->fail, "A call came in - ask again after it", sizeof(r->fail));
    r->ms = millis() - t0;
    return;
  }
  if (chutes) {
    aiRunChutes(q, r, ctx);                 // D-175: its requests; Gemini's below are untouched
    r->ms = millis() - t0;
    return;
  }
  if (l.quotaSwitch) {
    geminiQuotaNotice(primaryLabel, label, l.primaryUntil, l.nowUtc, q->tzS, l.primaryRemainS, stageBuf, stageCap);
    progSet(stageBuf, label, 0, 0);
  } else {
    progSet(q->musicPaused ? "Music paused - asking Gemini..." : "Asking Gemini...", label, 0, 0);
  }
  GeminiReply* rep = (GeminiReply*)heap_caps_calloc(1, sizeof(GeminiReply), MALLOC_CAP_SPIRAM);
  if (!rep) {
    r->net = GEM_NET_NOMEM;
    geminiFailText(0, GEM_NET_NOMEM, NULL, NULL, q->cfg.key, q->tzS, r->fail, sizeof(r->fail));
    return;
  }
  for (;;) {
    if (s_cancel) {
      r->cancelled = true;
      break;
    }
    /* aiAsk's call gate, again before every RETRY: a 503's 2.5 s or a per-minute wait is long
     * enough for a call to ring in, and a call's audio cannot share the handshake's heap dip. */
    if (r->attempts > 0 && tileFetchCallRecent()) {
      strlcpy(r->fail, "A call came in - ask again after it", sizeof(r->fail));
      break;
    }
    /* The system instruction NOW (2026-10-11): today's date from the clock as it is when this request
     * goes - only a trusted one (utcNow() is 0 otherwise: no date beats a wrong one). The lookup's
     * background goes in the QUESTION'S turn (s_work->bg), never in here; the context turns are the
     * chat's plain questions. */
    geminiSystemText(utcNow(), q->tzS, q->zone, s_work->sys, sizeof(s_work->sys));
    const size_t n = geminiBuildRequest(ctx, q->nCtx, q->question, s_work->bg, s_work->sys, l.budget,
                                        NULL, 0);
    if (n + 1 > s_bodyCap) {
      free(s_body);
      s_bodyCap = 0;
      s_body = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
      if (!s_body) {
        r->net = GEM_NET_NOMEM;
        geminiFailText(0, GEM_NET_NOMEM, NULL, NULL, q->cfg.key, q->tzS, r->fail, sizeof(r->fail));
        break;
      }
      s_bodyCap = n + 1;
    }
    geminiBuildRequest(ctx, q->nCtx, q->question, s_work->bg, s_work->sys, l.budget, s_body, s_bodyCap);
    const uint32_t ta = millis();
    size_t got = 0;
    int net = GEM_NET_OK;
    const int code = postOnce(l.model, n, &got, &net);
    r->lastMs = millis() - ta;
    r->lastCode = code;
    r->net = net;
    r->attempts++;
    r->bodyBytes = (uint32_t)got;
    progSet(NULL, NULL, 0, r->attempts);
    if (code <= 0 && s_cancel) {
      /* Ended by Cancel (the client stop()s and reads LOST): the user's word - never a timeout
       * for rule 6 to retry. */
      r->cancelled = true;
      break;
    }
    if (code <= 0 && !s_cancel && tileFetchCallRecent()) {
      /* The wait was ended by a call (postOnce's stopFn) - or one rang in as the request failed:
       * no answer is coming, and no retry is made under a call. */
      strlcpy(r->fail, "A call came in - ask again after it", sizeof(r->fail));
      break;
    }
    if (code > 0) {
      geminiParseReply(netWorkerSink(), got, s_work->text, sizeof(s_work->text), rep);
    } else {
      memset(rep, 0, sizeof(*rep));
      rep->retryDelayS = -1;
    }
    uint32_t delayMs = 0;
    const char* stage = NULL;
    const bool wasFallback = l.onFallback;
    /* `net` says why there was no HTTP answer: a timeout or a lost connection is rule 6's (once more,
     * then the fallback once - with the background found above, never a second lookup). */
    step = geminiLadderNext(&l, &s_m->session, &q->quota, code, net, rep, utcNow(), millis(), &delayMs, &stage);
    if (step == GEM_STEP_DONE) {
      /* The answer, and - when background went with the question - the "Wikipedia: A; B" line
       * under it (the pages that went; cut off again before it is ever sent back as context). D-176:
       * then the web's "Web: a; b" (or its note) on a line of its own - s_work->src holds only what
       * wikiCompose and aiWebSearch put there, so "" is no line at all. */
      if (rep->kind == GEM_REPLY_TEXT &&
          geminiAnswerText(rep, s_work->text, s_work->src[0] ? s_work->src : NULL, r->answer,
                           sizeof(r->answer)) > 0) {
        r->ok = true;
        /* Google's text is drawn, saved and sent back: the scrub every other text from Google gets
         * (the key never goes in a request's text - but a model can echo what it was sent). */
        geminiScrub(r->answer, sizeof(r->answer), q->cfg.key);
        /* Just the model under each answer: the daily-limit notice was already on the waiting
         * screen, and "Flash-Lite - daily Flash limit reached" took two rows under EVERY answer. */
        geminiModelLabel(l.model, r->label, sizeof(r->label));
      } else {
        geminiNoTextMessage(rep, r->fail, sizeof(r->fail));
      }
      break;
    }
    if (step == GEM_STEP_FAIL) {
      if (code <= 0 && net == GEM_NET_NOMEM) {
        lowMemText(r->fail, sizeof(r->fail));    // the heap bar (q.heapCheck) or the handshake's alloc
      } else {
        geminiFailText(code, net, rep, &l, q->cfg.key, q->tzS, r->fail, sizeof(r->fail));
      }
      break;
    }
    geminiModelLabel(l.model, label, GEM_LABEL_MAX);
    if (l.quotaSwitch && l.onFallback && !wasFallback) {
      geminiQuotaNotice(primaryLabel, label, l.primaryUntil, l.nowUtc, q->tzS, l.primaryRemainS, stageBuf, stageCap);
      stage = stageBuf;
    }
    /* A quota switch's notice (no wait) stays up through the fallback's request; a busy wait
     * counts down, then says it is asking again. */
    progSet(stage ? stage : "Asking Gemini...", label, delayMs ? millis() + delayMs : 0, -1);
    if (delayMs) {
      if (!waitSliced(delayMs)) {
        r->cancelled = true;
        break;
      }
      progSet("Asking again...", NULL, 0, -1);
    }
  }
  geminiWipe(rep, sizeof(*rep));          // Google's message may quote what was sent
  free(rep);
  r->ms = millis() - t0;
}

/* The AI's job on the shared worker (https_worker.h): the question's ladder, then the key wiped -
 * exactly what the AI's own worker did per question before 0.9.81. */
static void aiJob() {
  aiRun();                                // returns, so its locals are destructed
  memset(s_req->cfg.key, 0, sizeof(s_req->cfg.key));
  memset(s_req->chutes.key, 0, sizeof(s_req->chutes.key));   // D-175: the Chutes copy too
  memset(s_work->auth, 0, sizeof(s_work->auth));             // ...and its header (wiped per request already)
  memset(s_req->web.key, 0, sizeof(s_req->web.key));         // D-176: the Tavily copy
  memset(s_work->webAuth, 0, sizeof(s_work->webAuth));       // ...and its header (wiped after its request already)
  s_res->stackFloor = netWorkerStackFloor();
  heapNow(NULL, NULL, &s_res->minEver);
  s_done = true;                          // before s_busy drops: aiRequestActive never blinks
  s_busy = false;                         // the worker goes on to a queued weather fetch, or sleeps
}

// ── the loop side ────────────────────────────────────────────────────────────────────────

bool aiRequestActive() {
  return s_busy || s_done;
}

void aiCancel() {
  if (s_busy) {
    s_cancel = true;
    progSet("Cancelling...", NULL, 0, -1);
  }
}

void aiProgress(AiProgress* out) {
  if (!s_m) {
    memset(out, 0, sizeof(*out));
    return;
  }
  portENTER_CRITICAL(&s_mux);
  *out = s_m->prog;
  portEXIT_CRITICAL(&s_mux);
  out->active = aiRequestActive();
  out->cancelled = s_cancel;
}

const char* aiPendingQuestion() {
  return (aiRequestActive() && s_req) ? s_req->question : NULL;
}

void aiLoopTick() {
  /* ⚠ Not while a game runs: the fold writes the card, and the game's blit task owns the SPI bus
   * the card shares with the screen (a loop-task LCD push during a game deadlocked both cores -
   * GUI.cpp). aiRequestActive() stays true meanwhile; it lands on the first pass after. */
  if (!s_done || gGbcActive) {
    return;
  }
  /* D-176: a search whose request was SENT is counted now, on the loop (NVS): Auto's against the day's
   * cap, forced and Always ones apart - a reboot keeps both (ai_webd / ai_webn / ai_webo). One that never
   * reached Tavily (DNS, the handshake, the heap bar) spent no credit and is not counted (review
   * 2026-10-19: a captive portal used up the day's automatic searches). ⚠ Its log line is numbers only:
   * never the key, the query or the block. */
  if (s_res->webAsked && memOk()) {
    dayLoad();
    if (s_res->webSent) {
      webDayCount(&s_m->day, webToday(), s_req->webDec.automatic);
      daySave();
    }
    log_e("AI: web search HTTP %d (net %d) in %u ms (connect %u)%s, %d result(s) in the block (%u bytes), Tavily %ld ms, "
          "%d credit(s), today %d auto + %d other",
          s_res->webCode, s_res->webNet, (unsigned)s_res->webMs, (unsigned)s_res->webHsMs,
          s_res->webSent ? "" : ", never sent: not counted", s_res->webKept, (unsigned)s_res->webBlockLen, s_res->webRespMs,
          s_res->webCredits, s_m->day.autoN, s_m->day.otherN);
  }
  if (s_req->webOnly) {
    /* `ai web test`: no chat, no question counted - only the search (above), its keys wiped. A Clear
     * chat or a New topic asked meanwhile was done to the chat at once; what they latched for the
     * question in flight has none to apply to here - and left set, it would drop the NEXT question's
     * answer or break its topic again after it (review 2026-10-19). */
    memset(s_req->web.key, 0, sizeof(s_req->web.key));
    s_breakAfter = false;
    s_clearAfter = false;
    s_haveRes = true;
    s_cancel = false;
    s_done = false;
    return;
  }
  const bool cancelled = s_res->cancelled || s_cancel;
  const bool ok = s_res->ok && !cancelled;
  GeminiChat* c = s_clearAfter ? NULL : chatGet();
  if (c) {
    const char* a = cancelled ? "Cancelled" : (ok ? s_res->answer : s_res->fail);
    geminiChatFold(c, s_req->askedUtc, s_req->topic, !ok, s_res->label, s_req->question, a,
                   s_breakAfter);
    chatSave();
    s_chatGen++;
  }
  s_breakAfter = false;
  s_clearAfter = false;
  if (memcmp(&s_req->quotaIn, &s_req->quota, sizeof(s_req->quota)) != 0) {
    geminiQuotaMerge(&s_m->quota, &s_req->quota);
    quotaSave();
  }
  memset(s_req->cfg.key, 0, sizeof(s_req->cfg.key));
  memset(s_req->chutes.key, 0, sizeof(s_req->chutes.key));
  memset(s_req->web.key, 0, sizeof(s_req->web.key));        // D-176
  if (ok) {
    s_answered++;
  } else {
    s_failed++;
  }
  s_haveRes = true;
  /* ⚠ Never the key, the question, its keywords or the answer: sizes, codes, the model, the time. */
  if (s_res->wikiWhy[0]) {
    log_e("AI: Wikipedia lookup skipped: %s", s_res->wikiWhy);   // one line; the question went without it
  }
  log_e("AI: %s in %u ms, %d request(s), HTTP %d, %s, question %u chars, answer %u chars, "
        "Wikipedia %d page(s) in %u ms, stack floor %u",
        cancelled ? "cancelled" : (ok ? "answered" : "failed"), (unsigned)s_res->ms, s_res->attempts,
        s_res->lastCode, s_res->label[0] ? s_res->label : "-", (unsigned)strlen(s_req->question),
        (unsigned)(ok ? strlen(s_res->answer) : 0), s_res->wikiPages, (unsigned)s_res->wikiMs,
        (unsigned)s_res->stackFloor);
  s_cancel = false;
  s_done = false;
}

/* The question, straight into the (idle) request - not on the loop's stack: printable ASCII (the
 * compose field's own rule), ends trimmed, 500 at most. aiAsk's, and (D-176) `ai web test`'s. */
static bool takeQuestion(const char* question, char* why, size_t whyCap) {
  const char* p = question ? question : "";
  while (*p == ' ') p++;
  const size_t len = strlen(p);
  if (len > GEM_Q_CAP) {
    strlcpy(why, "Too long - 500 characters at most", whyCap);
    return false;
  }
  char* q = s_req->question;
  size_t n = 0;
  for (; *p; p++) {
    const unsigned char ch = (unsigned char)*p;
    q[n++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : ' ';
  }
  while (n > 0 && q[n - 1] == ' ') n--;
  q[n] = '\0';
  if (n == 0) {
    strlcpy(why, "Type a question first", whyCap);
    return false;
  }
  return true;
}

bool aiAsk(const char* question, char* why, size_t whyCap, bool forceWeb) {
  if (!why || whyCap == 0) {
    return false;
  }
  why[0] = '\0';
  aiLoopTick();                           // a result still waiting goes into the chat first
  if (aiRequestActive()) {
    strlcpy(why, s_cancel ? "Still cancelling the last question" : "A question is already being asked",
            whyCap);
    return false;
  }
  if (!s_req) s_req = (AiReq*)heap_caps_calloc(1, sizeof(AiReq), MALLOC_CAP_SPIRAM);
  if (!s_res) s_res = (AiRes*)heap_caps_calloc(1, sizeof(AiRes), MALLOC_CAP_SPIRAM);
  if (!s_work) s_work = (AiWork*)heap_caps_calloc(1, sizeof(AiWork), MALLOC_CAP_SPIRAM);
  if (!s_req || !s_res || !s_work) {
    strlcpy(why, "No memory for the question", whyCap);
    return false;
  }
  if (!takeQuestion(question, why, whyCap)) {
    return false;
  }
  aiKeyReload();                          // a new or fixed key works without a reboot
  if (!s_m) {
    strlcpy(why, "No memory for the question", whyCap);
    return false;
  }
  /* D-175: which model - NVS ai_model, or the default rule (no chutes.txt: Gemini, as before) - and
   * THAT provider's key. */
  const int choice = aiModelChoice();
  const bool chutes = choice != CHUTES_GEMINI;
  const bool usable = chutes ? (s_m->ckey.usable && s_ccfg && s_cHaveGood) : s_m->key.usable;
  if (!usable) {
    const int st = chutes ? s_m->ckey.state : s_m->key.state;
    if (st == AI_KEY_BAD) {
      strlcpy(why, "The key file was refused - see Key info", whyCap);
    } else if (st == AI_KEY_UNREADABLE) {
      strlcpy(why, "Could not read the key file - try again", whyCap);
    } else {
      strlcpy(why, chutes ? "No key - put chutes.txt in API Keys (see Key info)"
                          : "No key - put gemini.txt in API Keys (see Key info)", whyCap);
    }
    return false;
  }
  {
    const char* gate = aiGateWhy();
    if (gate) {
      strlcpy(why, gate, whyCap);
      return false;
    }
  }
  /* The heap bar - unless a weather fetch holds the worker: its handshake is dipping the heap right
   * now, so the reading would refuse a question that would have been fine. Then the worker checks
   * the bar itself when the question's turn comes (heapCheckAtStart). */
  const bool behindWeather = wxRequestActive();
  if (!behindWeather && !netHeapOkForHandshake(NULL, NULL)) {
    lowMemText(why, whyCap);
    return false;
  }
  GeminiChat* chat = chatGet();
  if (!chat) {
    strlcpy(why, "No memory for the question", whyCap);
    return false;
  }
  if (!netWorkerReady(why, whyCap)) {
    return false;
  }
  if (!s_sessionInit) {
    geminiSessionInit(&s_m->session);
    s_sessionInit = true;
  }
  quotaLoad();

  // The request: everything the worker will read, copied now (the chat may change meanwhile).
  s_req->choice = choice;                         // D-175: the choice as the question is asked
  if (chutes) {
    /* Only the Chutes key goes with a Chutes question; `cfg` carries chutes.txt's lookup= alone
     * (aiWikiLookup reads one place). */
    memset(&s_req->cfg, 0, sizeof(s_req->cfg));
    s_req->cfg.lookup = s_ccfg->lookup;
    memcpy(&s_req->chutes, s_ccfg, sizeof(s_req->chutes));
  } else {
    memcpy(&s_req->cfg, s_cfg, sizeof(s_req->cfg));
    memset(&s_req->chutes, 0, sizeof(s_req->chutes));
  }
  s_req->askedUtc = utcNow();
  s_req->tzS = tzNow();
  {
    /* The zone's name for the date sentence - only when it is the offset the clock is on now (a
     * zone whose DST waits for a clock, or a typed offset, gives just "local time (UTC-7)"). */
    TzEff e;
    tzSvcEffective(&e);
    s_req->zone[0] = '\0';
    if (e.ok && e.name && e.minutes * 60 == s_req->tzS) {
      strlcpy(s_req->zone, e.name, sizeof(s_req->zone));
    }
  }
  /* The topic: geminiChatTopicDue's reasons and - the X4's review, 2026-10-19 - a topic never spans
   * Gemini and Chutes: a question to the other provider than this boot's last one, or than any
   * exchange it would be sent (the labels: a restart and a cloned card too), starts one. So Google
   * is never sent what Chutes was asked or answered (Key info: "never to Google"), nor Chutes what
   * Google was. chutesTopicDue, host-tested; aiTopicDue asks the same. */
  s_req->topic = chutesTopicDue(chat, s_req->askedUtc, choice, s_m->topicLast);
  s_m->topicLast = chutesChoiceProvider(choice);  // asked: nothing below refuses it
  s_req->nCtx = 0;
  if (!s_req->topic) {
    int idx[GEM_CONTEXT];
    const int k = geminiChatContext(chat, idx);
    for (int i = 0; i < k; i++) {
      strlcpy(s_req->ctxQ[i], chat->ex[idx[i]].q, sizeof(s_req->ctxQ[i]));
      strlcpy(s_req->ctxA[i], chat->ex[idx[i]].a, sizeof(s_req->ctxA[i]));
    }
    s_req->nCtx = k;
  }
  s_req->quota = s_m->quota;
  s_req->quotaIn = s_m->quota;
  s_req->heapCheckAtStart = behindWeather;
  s_req->retryTimeouts = aiRetryTimeouts();       // NVS, read once: the setting as the question is asked
  /* D-176: the web search, decided now (webDecide, host-tested: the setting, a usable tavily.txt, the
   * per-question force, the Auto rule and today's automatic searches - NVS, so a reboot keeps the cap) -
   * and the Tavily key copied into the question ONLY when it searches. */
  dayLoad();
  webDecide(aiWebMode(), aiWebUsable(), forceWeb, s_req->question, webDayAutoUsed(&s_m->day, webToday()),
            &s_req->webDec);
  s_req->webNote = s_req->webDec.why == WEB_WHY_CAP ? WEB_NOTE_CAP : WEB_NOTE_NONE;
  s_req->webOnly = false;
  if (s_req->webDec.search) {
    memcpy(&s_req->web, s_wcfg, sizeof(s_req->web));
  } else {
    memset(&s_req->web, 0, sizeof(s_req->web));
  }
  memset(s_res, 0, sizeof(*s_res));
  s_haveRes = false;
  /* MUSIC PAUSES FOR A QUESTION (Nick, 2026-10-03: "make music turn off when doing ai"). The
   * TLS handshake is ~4.5 s of software ECC on this core, and Google resets one that takes over
   * ~10 s: with a track decoding beside it, MEASURED on phone 1, 2 of 2 questions failed that
   * way (`start_ssl_client: -80`), and even with the loop's 10-tick yield the first try still
   * failed. Paused, not stopped: the track and its place stay, and Music's Play carries on. */
  const bool pausedMusic = musicPlayerIsPlaying();
  if (pausedMusic) {
    musicPlayerPause();
  }
  s_req->musicPaused = pausedMusic;
  portENTER_CRITICAL(&s_mux);
  memset(&s_m->prog, 0, sizeof(s_m->prog));
  s_m->prog.startMs = millis();
  strlcpy(s_m->prog.stage, behindWeather ? "Waiting for the weather fetch..."
                           : (pausedMusic ? "Music paused - connecting..." : "Connecting..."),
          sizeof(s_m->prog.stage));
  if (chutes) {
    strlcpy(s_m->prog.label, chutesChoiceLabel(choice), sizeof(s_m->prog.label));
    strlcpy(s_m->prog.shortName, chutesChoiceShort(choice), sizeof(s_m->prog.shortName));
  }
  s_m->prog.gen = 1;
  portEXIT_CRITICAL(&s_mux);
  tlsInstallPsramHook();                  // BEFORE the first WiFiClientSecure (tile_fetch.h)
  s_asked++;
  s_cancel = false;
  s_done = false;
  s_busy = true;
  netWorkerSubmit(NET_JOB_AI, aiJob);
  return true;
}

bool aiWebTest(const char* question, char* why, size_t whyCap) {
  if (!why || whyCap == 0) {
    return false;
  }
  why[0] = '\0';
  aiLoopTick();
  if (aiRequestActive()) {
    strlcpy(why, s_cancel ? "Still cancelling the last question" : "A question is already being asked", whyCap);
    return false;
  }
  if (!s_req) s_req = (AiReq*)heap_caps_calloc(1, sizeof(AiReq), MALLOC_CAP_SPIRAM);
  if (!s_res) s_res = (AiRes*)heap_caps_calloc(1, sizeof(AiRes), MALLOC_CAP_SPIRAM);
  if (!s_work) s_work = (AiWork*)heap_caps_calloc(1, sizeof(AiWork), MALLOC_CAP_SPIRAM);
  if (!s_req || !s_res || !s_work) {
    strlcpy(why, "No memory for the search", whyCap);
    return false;
  }
  if (!takeQuestion(question, why, whyCap)) {
    return false;
  }
  aiKeyReload();
  if (!s_m) {
    strlcpy(why, "No memory for the search", whyCap);
    return false;
  }
  /* Off keeps every question away from Tavily - a bench search too. */
  if (aiWebMode() == WEB_OFF) {
    strlcpy(why, "Web search is Off - nothing goes to Tavily (`ai web auto` first)", whyCap);
    return false;
  }
  if (!aiWebUsable()) {
    strlcpy(why, s_m->wkey.state == AI_KEY_BAD ? "tavily.txt was refused - see `ai web`"
                                               : "No usable tavily.txt in API Keys", whyCap);
    return false;
  }
  {
    const char* gate = aiGateWhy();
    if (gate) {
      strlcpy(why, gate, whyCap);
      return false;
    }
  }
  const bool behindWeather = wxRequestActive();
  if (!behindWeather && !netHeapOkForHandshake(NULL, NULL)) {
    lowMemText(why, whyCap);
    return false;
  }
  if (!netWorkerReady(why, whyCap)) {
    return false;
  }
  quotaLoad();
  /* The request: no provider, no key but Tavily's, no chat - the search alone, forced (counted apart). */
  s_req->choice = CHUTES_GEMINI;
  memset(&s_req->cfg, 0, sizeof(s_req->cfg));
  memset(&s_req->chutes, 0, sizeof(s_req->chutes));
  s_req->askedUtc = utcNow();
  s_req->tzS = tzNow();
  s_req->zone[0] = '\0';
  s_req->topic = false;
  s_req->nCtx = 0;
  s_req->quota = s_m->quota;                      // untouched, so nothing is saved for it
  s_req->quotaIn = s_m->quota;
  s_req->heapCheckAtStart = behindWeather;
  s_req->retryTimeouts = false;
  s_req->musicPaused = false;
  webDecide(aiWebMode(), true, true, s_req->question, 0, &s_req->webDec);
  s_req->webNote = WEB_NOTE_NONE;
  s_req->webOnly = true;
  memcpy(&s_req->web, s_wcfg, sizeof(s_req->web));
  /* The last search's block and line wiped (review 2026-10-19): a test that ends before its search
   * (a gate at the worker's start) must never let `ai web` print an earlier question's. The worker is
   * idle: aiRequestActive() was false above. */
  memset(s_work->web, 0, sizeof(s_work->web));
  memset(s_work->webSrc, 0, sizeof(s_work->webSrc));
  memset(s_res, 0, sizeof(*s_res));
  s_haveRes = false;
  portENTER_CRITICAL(&s_mux);
  memset(&s_m->prog, 0, sizeof(s_m->prog));
  s_m->prog.startMs = millis();
  strlcpy(s_m->prog.stage, behindWeather ? "Waiting for the weather fetch..." : WEB_STAGE, sizeof(s_m->prog.stage));
  strlcpy(s_m->prog.label, "Tavily", sizeof(s_m->prog.label));
  s_m->prog.gen = 1;
  portEXIT_CRITICAL(&s_mux);
  tlsInstallPsramHook();
  s_cancel = false;
  s_done = false;
  s_busy = true;
  netWorkerSubmit(NET_JOB_AI, aiJob);
  return true;
}

// ── the bench ────────────────────────────────────────────────────────────────────────────

/* D-176: the setting, the key's LENGTH (never the key) and the day's counts - `ai` and `ai web`. */
static void webStateLines(void (*emit)(const char* line), const char* first) {
  char l[192];
  int a = 0, o = 0;
  aiWebToday(&a, &o);
  snprintf(l, sizeof(l), "%sweb search: %s (%s)%s - today %d of %d automatic, %d other", first, webModeLabel(aiWebMode()),
           aiWebModeStored() ? "ai_web set" : "ai_web unset: Auto", aiWebUsable() ? "" : ", NO usable tavily.txt: nothing is searched",
           a, WEB_DAILY_AUTO, o);
  emit(l);
  snprintf(l, sizeof(l), "    tavily %s, file %s, roots built-in Amazon Root CA 1 + Starfield G2", s_m->wkey.line,
           s_m->wkey.path ? s_m->wkey.path : "(none: " AI_TAVILY_FILE ")");
  emit(l);
}

/* The last question's search, in numbers: never the query, the block or Tavily's text. */
static void webLastLine(char* l, size_t cap) {
  if (!s_res->webAsked) {
    char w[96];
    webWhyText(&s_req->webDec, w, sizeof(w));
    snprintf(l, cap, "    web: not searched (%s)%s", w, s_res->webNote == WEB_NOTE_CAP ? " - the cap's note went under it" : "");
    return;
  }
  const char* note = webNoteLine(s_res->webNote);
  snprintf(l, cap, "    web: HTTP %d (net %d), %u ms (connect %u), %d result(s) in the block (%u bytes), Tavily %ld ms, "
           "%d credit(s)%s%s%s", s_res->webCode, s_res->webNet, (unsigned)s_res->webMs, (unsigned)s_res->webHsMs, s_res->webKept,
           (unsigned)s_res->webBlockLen, s_res->webRespMs, s_res->webCredits, s_res->webSent ? "" : ", never sent (not counted)",
           note ? " - " : "", note ? note : "");
}

void aiReport(void (*emit)(const char* line)) {
  char l[192];
  if (!memOk()) {
    emit("ai: no PSRAM for the AI's state");
    return;
  }
  if (!aiRequestActive()) {
    aiKeyReload();
  }
  snprintf(l, sizeof(l), "ai: %s", s_m->key.line);
  emit(l);
  snprintf(l, sizeof(l), "    file %s, roots built-in GTS R1+R4",
           s_m->key.path ? s_m->key.path : "(none: " AI_KEY_FILE " or " AI_KEY_FILE_ROOT ")");
  emit(l);
  /* D-175: the provider and preset, and the Chutes key's LENGTH - never the key. */
  const int choice = aiModelChoice();
  snprintf(l, sizeof(l), "    model: %s (%s; %s)%s", chutesChoiceLabel(choice), chutesChoiceId(choice),
           aiModelChoiceStored() ? "ai_model set" : "ai_model unset: the default",
           aiChoiceUsable(choice) ? "" : " - NO usable key file for it: questions are refused");
  emit(l);
  snprintf(l, sizeof(l), "    chutes %s", s_m->ckey.line);
  emit(l);
  snprintf(l, sizeof(l), "    file %s, roots built-in GoDaddy G2 + ISRG X1/X2/YR/YE + GTS R1/R4%s",
           s_m->ckey.path ? s_m->ckey.path : "(none: " AI_CHUTES_FILE ")",
           s_m->chutesGone ? ", a preset answered not-found this boot" : "");
  emit(l);
  const GeminiChat* c = aiRequestActive() ? s_chat : chatGet();
  if (c) {
    int idx[GEM_CONTEXT];
    bool other = false;
    const bool topic = aiTopicDue(&other);        // aiAsk's rule: the provider's too
    snprintf(l, sizeof(l), "    chat: %d exchange%s saved (%u bytes), the next question %s",
             c->n, c->n == 1 ? "" : "s", (unsigned)geminiChatSave(c, NULL, 0),
             topic ? (other ? "starts a new topic (the model in use is not the topic's provider)"
                            : "starts a new topic")
                   : "");
    if (!topic) {
      const size_t at = strlen(l);
      snprintf(l + at, sizeof(l) - at, "sends %d earlier exchange(s)", geminiChatContext(c, idx));
    }
    emit(l);
  }
  quotaLoad();
  bool anyOut = false;
  for (int i = 0; i < s_m->quota.n; i++) {
    const int64_t until = geminiQuotaUntil(&s_m->quota, s_m->quota.e[i].model, utcNow(), millis());
    if (geminiQuotaOut(&s_m->quota, s_m->quota.e[i].model, utcNow(), millis())) {
      anyOut = true;
      if (until) {
        snprintf(l, sizeof(l), "    daily limit: %s used up for another %lld s%s", s_m->quota.e[i].model,
                 (long long)(until - utcNow()), s_m->quota.e[i].guess ? " (a guess: Google gave no time)" : "");
      } else {
        snprintf(l, sizeof(l), "    daily limit: %s used up until a restart (no trusted clock)",
                 s_m->quota.e[i].model);
      }
      emit(l);
    }
  }
  if (!anyOut) {
    emit("    daily limits: none reached");
  }
  if (choice == CHUTES_GEMINI) {
    emit(s_m->key.lookup ? "    Wikipedia lookup: on (keywords only; gemini.txt lookup=off stops it)"
                         : "    Wikipedia lookup: OFF (gemini.txt lookup=off)");
  } else {
    emit(s_m->ckey.lookup ? "    Wikipedia lookup: on (keywords only; chutes.txt lookup=off stops it)"
                          : "    Wikipedia lookup: OFF (chutes.txt lookup=off)");
  }
  emit(aiRetryTimeouts() ? "    " GEM_RETRY_LABEL ": on (once more, then the fallback once; `ai retry off`)"
                         : "    " GEM_RETRY_LABEL ": OFF (a timeout is shown at once; `ai retry on`)");
  webStateLines(emit, "    ");                    // D-176
  if (aiRequestActive()) {
    AiProgress p;
    aiProgress(&p);
    snprintf(l, sizeof(l), "    NOW: %s (%s, %u s, request %d)%s", p.stage, p.label[0] ? p.label : "-",
             (unsigned)((millis() - p.startMs) / 1000), p.attempt, p.cancelled ? " - cancelling" : "");
    emit(l);
  } else if (s_haveRes && s_res) {
    snprintf(l, sizeof(l), "    last: %s, HTTP %d, %s, %u ms (last request %u ms), %d request(s), body %u bytes",
             s_res->cancelled ? "cancelled" : (s_res->ok ? "answered" : "FAILED"), s_res->lastCode,
             s_res->label[0] ? s_res->label : "-", (unsigned)s_res->ms, (unsigned)s_res->lastMs,
             s_res->attempts, (unsigned)s_res->bodyBytes);
    emit(l);
    if (!s_res->wikiAsked) {
      snprintf(l, sizeof(l), "    Wikipedia: not asked (%s)", s_res->wikiNot ? s_res->wikiNot : "-");
      emit(l);
    } else if (s_res->wikiPages > 0) {
      snprintf(l, sizeof(l), "    Wikipedia: %d page(s) went with it, %u ms", s_res->wikiPages,
               (unsigned)s_res->wikiMs);
      emit(l);
    } else {
      snprintf(l, sizeof(l), "    Wikipedia: skipped after %u ms - %s", (unsigned)s_res->wikiMs, s_res->wikiWhy);
      emit(l);
    }
    if (s_req) {
      webLastLine(l, sizeof(l));                  // D-176
      emit(l);
    }
    if (s_res->ok) {
      snprintf(l, sizeof(l), "    answer (%u chars): %.100s", (unsigned)strlen(s_res->answer), s_res->answer);
      for (char* p = l; *p; p++) {
        if (*p == '\n') *p = ' ';
      }
    } else {
      snprintf(l, sizeof(l), "    why: %.170s", s_res->fail);
    }
    emit(l);
    snprintf(l, sizeof(l), "    worker stack floor %u of %u | internal largest floor %u, min-ever %u",
             (unsigned)s_res->stackFloor, (unsigned)NET_STACK_BYTES, (unsigned)s_res->largestFloor,
             (unsigned)s_res->minEver);
    emit(l);
  } else {
    emit("    nothing asked since boot. ai ask <question>");
  }
  uint32_t f, lg, m;
  heapNow(&f, &lg, &m);
  snprintf(l, sizeof(l), "    internal now free %u largest %u min-ever %u | psram free %u | asked %u, answered %u, failed %u",
           (unsigned)f, (unsigned)lg, (unsigned)m, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)s_asked, (unsigned)s_answered, (unsigned)s_failed);
  emit(l);
}

static void emitWrapped(void (*emit)(const char*), const char* prefix, const char* text) {
  char l[192];
  const size_t room = 170;
  const char* p = text;
  bool first = true;
  while (*p || first) {
    size_t n = 0;
    while (p[n] && p[n] != '\n' && n < room) n++;
    snprintf(l, sizeof(l), "%s%.*s", first ? prefix : "    ", (int)n, p);
    emit(l);
    first = false;
    p += n;
    if (*p == '\n') p++;
  }
}

void aiReportLast(void (*emit)(const char* line)) {
  const GeminiChat* c = aiRequestActive() ? s_chat : chatGet();
  if (!c || c->n == 0) {
    emit("ai last: the chat is empty");
    return;
  }
  const GeminiExchange* e = &c->ex[c->n - 1];
  char l[96];
  snprintf(l, sizeof(l), "ai last: %s%s%s", (e->flags & GEM_EX_FAILED) ? "FAILED" : "answered by ",
           (e->flags & GEM_EX_FAILED) ? "" : e->label, (e->flags & GEM_EX_TOPIC) ? " (a new topic)" : "");
  emit(l);
  emitWrapped(emit, "  Q: ", e->q);
  emitWrapped(emit, "  A: ", e->a);
}

void aiWebReport(void (*emit)(const char* line)) {
  char l[192];
  if (!memOk()) {
    emit("ai web: no PSRAM for the AI's state");
    return;
  }
  if (!aiRequestActive()) {
    aiKeyReload();
  }
  webStateLines(emit, "ai web: ");
  emit("    budget: the handshake 6 s, Tavily's answer 6 s, 12 s in all; never retried. The key goes only to " WEB_HOST);
  if (aiRequestActive()) {
    AiProgress p;
    aiProgress(&p);
    snprintf(l, sizeof(l), "    NOW: %s (%u s)%s", p.stage, (unsigned)((millis() - p.startMs) / 1000),
             p.cancelled ? " - cancelling" : "");
    emit(l);
    return;
  }
  if (!s_haveRes || !s_res || !s_req) {
    emit("    nothing searched since boot. ai web test <question> (a forced search, counted)");
    return;
  }
  webLastLine(l, sizeof(l));
  emit(l);
  if (!s_req->webOnly) {
    return;                                        // a question's block is never printed: only the bench test's
  }
  /* `ai web test`: what Auto would have said of the question, then - bench only, asked for by hand - the
   * block the model would get and its line. Never in a log, the chat or the card. */
  int a = 0, o = 0;
  aiWebToday(&a, &o);
  WebDecision d;
  webDecide(WEB_AUTO, true, false, s_req->question, a, &d);
  char w[96];
  webWhyText(&d, w, sizeof(w));
  const char* tr = webTimeId(s_req->webDec.timeRange);
  snprintf(l, sizeof(l), "    the test's question: %u chars; under Auto: %s; topic %s, time_range %s",
           (unsigned)strlen(s_req->question), w, webTopicId(s_req->webDec.topic), tr ? tr : "none");
  emit(l);
  if (s_res->webAsked && s_res->webNote == WEB_NOTE_NONE && s_work && s_work->web[0]) {
    /* Only THIS test's search, and only one that put a block in (review 2026-10-19). */
    emit("    the block (bench only - never stored or logged):");
    emitWrapped(emit, "    ", s_work->web);
    emitWrapped(emit, "    ", s_work->webSrc);
  }
}
