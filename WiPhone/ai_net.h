/*
 * ai_net.h - the device half of Menu > AI: the key file on the card, the saved chat, the
 * remembered daily limits, the gates, and the worker that talks to Google. The decisions are
 * gemini.h's (host-tested); the screens are app_ai.cpp's; the bench is serial `ai`.
 *
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * ONE QUESTION AT A TIME, ON A WORKER OF ITS OWN
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * TLS cannot run on the loop task (tile_fetch.h, "WHY A TASK"): an 8 KB stack and mbedTLS's two
 * 16.7 KB record buffers. So, exactly like the map downloader:
 *   - ONE persistent worker ("aiworker"), its 8 KB INTERNAL stack carved on the first question and
 *     never freed, waiting on a semaphore. Never a per-run task (a static one races its own TCB on
 *     this FreeRTOS; a dynamic one fragmented `largest` 26 -> 11 KB on phone 1).
 *   - mbedTLS allocates from PSRAM through tile_fetch's hook (tlsInstallPsramHook), installed
 *     before the first WiFiClientSecure - an AI question may be the first TLS since boot.
 *   - Its OWN worker, not tile_fetch's: a map job holds that one for days. A question is refused
 *     while a download runs (two handshakes would dip the internal heap ~24 KB) - see aiAsk.
 *   - (0.9.81) That worker is now SHARED with the Almanac's weather (https_worker.h: job kinds on
 *     one stack, one request helper, netRequestActive() in the loop's hooks). The AI's job is what
 *     this worker did before - the ladder, the key wipe, the music pause, the gates; a question
 *     asked while a weather fetch holds the worker waits its turn ("Waiting for the weather
 *     fetch...") and the heap bar is checked by the worker when it starts, not by aiAsk.
 * Per question: a new WiFiClientSecure with Google's roots pinned (setCACert: GTS Root R1 + R4,
 * built in and the only ones - no override file), setConnectTimeout(10000) - which also caps the
 * handshake, 120 s by default - and up to 150 s for the answer (2026-10-11: HTTPClient's own
 * timeout is a uint16 and stops at 65.5 s, so https_worker's client waits for the first byte
 * itself, in AI_SLICE_MS slices that honour Cancel - and a call that rings in), the system
 * instruction built per request with today's date (a trusted clock only; gemini.h, "CURRENT
 * FACTS"), the key in an `x-goog-api-key` header, the chunked body de-chunked by hand into PSRAM
 * (never getString(), which grows on the internal heap), then http.end(), the block closes, stop(),
 * delete. The connection is closed after every answer.
 * Before the first request of a question (2026-10-11), unless gemini.txt says `lookup=off`: ONE
 * GET to en.wikipedia.org with the question's KEYWORDS (wiki_lookup.h - never the question, the key
 * or the chat), ISRG X1 + X2 + Root YE pinned, 20 s at most, never retried; what it finds about the
 * keywords goes in the question's own turn as labelled background, and the answer ends "Wikipedia:
 * A; B". Any failure is one log line and no background - the question is asked all the same. A
 * request that gets no answer in time is asked once more, then the fallback once (round 3, "Retry if
 * it times out", on by default: aiRetryTimeouts) - with the background already found, never a
 * second lookup.
 *
 * WHO OWNS WHAT. The question, its context and a copy of the key go into a module-static PSRAM
 * request on the LOOP (aiAsk) and belong to the worker until it finishes; the worker writes only
 * that request, its result and a small progress record published under a spinlock with a
 * generation counter. The loop folds a finished result into the chat and writes the card
 * (aiLoopTick, every pass - a flag test when idle). So leaving the app mid-question is safe: the
 * worker never touches an app, and the answer is in the chat when the app is opened again.
 *
 * THE CARD. gemini.txt is read from /API Keys/gemini.txt, then /gemini.txt (the T-Deck's place),
 * on every app open and before every question - so a new key works without a reboot - and only a
 * GOOD read is kept: a card that fails a read keeps the last good key rather than turning into
 * "no key". The chat is /ai/chat.txt and the remembered limits /ai/state.txt (never inside
 * /API Keys). Nothing here ever prints, logs or draws the key: `ai` says its length.
 *
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * THE SECOND PROVIDER: CHUTES (D-175, 2026-10-18)
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * OFF by default, no key shipped (the public rule): a user turns it on with their own
 * /API Keys/chutes.txt (that path only - chutes.h has the format), read in the same pass, with
 * the same good-read latch and wipe, as gemini.txt (geminiWipe: a memset before free() is dropped
 * by the compiler - review 2026-10-18). A Chutes key in gemini.txt is refused (geminiParseConfig).
 * Which model a question goes to is the CHOICE, NVS "wpmesh" / "ai_model" (Menu > AI > Model,
 * serial `ai model <id>`): gemini, v4flash, kimi, kimi_think or glm; unset, it is v4flash when a
 * Chutes key is there and Gemini when not (chutesChoiceResolve) - so a phone without chutes.txt
 * is the phone of before. aiAsk copies the
 * choice into the question; aiRun makes the same Wikipedia lookup, once, then either Gemini's
 * ladder (unchanged) or Chutes' (chutes.h: one busy retry, ONE fallback in the preset's chain,
 * never across providers). Nor does a TOPIC cross them (the X4's review, 2026-10-19): a question to
 * the other provider than the boot's last one, or than any exchange it would be sent, starts one
 * (chutesTopicDue), so neither provider is sent what the other was asked or answered. A Chutes request: a fresh client with the Chutes ROOT SET pinned
 * (GoDaddy G2 today; ISRG X1 + X2 + Root YR + Root YE and GTS R1 + R4 for the CA its 2026-11-30
 * renewal may bring, whichever chain it is served with - no override file, the reason above), the
 * key in an `Authorization: Bearer` header built in PSRAM for that one request and wiped after
 * it, ONLY to llm.chutes.ai (redirects are never followed:
 * HTTPClient's default), the preset's own wait (60 s, 120 s for the thinking ones), the same heap
 * bar, Cancel, call stop and sink as Gemini's. No Chutes text is ever kept or shown (a 402 names a
 * wallet): every failure is the phone's own sentence.
 *
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * THE WEB SEARCH (D-176, 2026-10-19)
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * OFF without the owner's own /API Keys/tavily.txt (that path only - websearch.h has the format),
 * read in the same pass, with the same good-read latch and wipe, as the other two. The setting is NVS
 * "wpmesh" / "ai_web" (Off / Auto / Always; absent = Auto - Menu > AI's "Web search:" row, serial `ai
 * web <id>`); "Ask with web search" forces one question's (never under Off). aiAsk decides on the loop
 * (webDecide: the Auto rule, the day's cap from NVS ai_webd / ai_webn / ai_webo) and copies the
 * Tavily key into the question only when it searches. aiRun makes the Wikipedia lookup, THEN the one
 * search (never two TLS sessions at once: one worker), THEN either provider: a fresh client with the
 * Amazon ROOT SET pinned (Amazon Root CA 1 + Starfield Services Root G2: amazon_roots.h), the key in
 * an `Authorization: Bearer` header built in PSRAM for that one request and wiped after it, ONLY to
 * api.tavily.com (no redirect followed), the handshake under AI_WEB_CONNECT_MS and Tavily's answer
 * under websearch.h's 3 s (WEB_ANSWER_MS), never retried, Cancel and a call honoured. The reply is
 * PARSED AS IT ARRIVES (https_worker's tap into websearch.h's streaming parser; nothing raw is kept),
 * the block goes after Wikipedia's in the question's turn (s_work->bg) and the "Web: a; b" line - or
 * the note saying why nothing went - after the Wikipedia line under the answer. Only the answer is
 * stored: the block never goes in the chat, a log or the console (but `ai web test`'s bench print).
 * A search whose request was SENT is counted on the loop when the question lands (aiLoopTick): Auto's
 * against the cap, forced and Always ones apart (one that never reached Tavily spent no credit).
 */
#ifndef AI_NET_H
#define AI_NET_H

#include <stddef.h>
#include <stdint.h>
#include "gemini.h"
#include "chutes.h"
#include "websearch.h"

#define AI_KEY_DIR        "/API Keys"
#define AI_KEY_FILE       "/API Keys/gemini.txt"
#define AI_KEY_FILE_ROOT  "/gemini.txt"         // where the T-Deck firmware keeps it
#define AI_CHUTES_FILE    "/API Keys/chutes.txt" // D-175: that path ONLY (no root copy: nothing else reads one)
#define AI_TAVILY_FILE    "/API Keys/tavily.txt" // D-176: that path ONLY, as chutes.txt
#define AI_DIR            "/ai"
#define AI_CHAT_FILE      "/ai/chat.txt"
#define AI_CHAT_TMP       "/ai/chat.tmp"
#define AI_CHAT_BAD       "/ai/chat.bad"        // a chat.txt this firmware could not read, set aside
#define AI_STATE_FILE     "/ai/state.txt"
#define AI_STATE_TMP      "/ai/state.tmp"

// ── the key ──────────────────────────────────────────────────────────────────────────────
#define AI_KEY_LINE_MAX   256
enum {
  AI_KEY_NOT_READ = 0,
  AI_KEY_NONE,              // no gemini.txt, or no key in it
  AI_KEY_OK,
  AI_KEY_BAD,               // gemini.txt refused (geminiConfigLine says why)
  AI_KEY_UNREADABLE,        // the card would not give it up (the last good key, if any, is kept)
};
typedef struct {
  int         state;        // AI_KEY_*
  bool        usable;       // a key a question can use (OK, or a kept good one)
  const char* path;         // the file it came from, or NULL
  /* geminiConfigLine: the key's LENGTH, the model... never the key. 256 since review 2026-10-11
   * (two 63-character model names and the lookup note passed 200); a longer line is cut "...". */
  char        line[AI_KEY_LINE_MAX];
  char        model[GEM_MODEL_MAX];
  char        fallback[GEM_MODEL_MAX];
  int         thinking;
  bool        lookup;       // the Wikipedia lookup is on (gemini.txt `lookup=off`: false)
} AiKeyInfo;
/* What the last read of chutes.txt found (D-175) - the same rules, never the key. */
typedef struct {
  int         state;        // AI_KEY_*
  bool        usable;       // a key a question can use (OK, or a kept good one)
  const char* path;         // AI_CHUTES_FILE, or NULL
  char        line[AI_KEY_LINE_MAX];   // chutesConfigLine: "key: set (102 chars), model= kimi"
  int         preset;       // chutes.txt model= (-1 = none): the default when the choice is unset
  bool        lookup;       // chutes.txt `lookup=off`: false
} AiChutesKey;
/* What the last read of tavily.txt found (D-176) - the same rules, never the key. */
typedef struct {
  int         state;        // AI_KEY_*
  bool        usable;       // a key a search can use (OK, or a kept good one)
  const char* path;         // AI_TAVILY_FILE, or NULL
  char        line[AI_KEY_LINE_MAX];   // webConfigLine: "key: set (41 chars)"
} AiWebKey;
/* LOOP TASK. Read gemini.txt, chutes.txt and tavily.txt now. */
void aiKeyReload();
void aiKeyInfo(AiKeyInfo* out);           // what the last read found (no card access)
void aiChutesKeyInfo(AiChutesKey* out);   // ...of chutes.txt
void aiWebKeyInfo(AiWebKey* out);         // ...of tavily.txt (D-176)

/* LOOP TASK. THE WEB SEARCH SETTING (D-176): WEB_OFF / WEB_AUTO / WEB_ALWAYS (websearch.h). NVS
 * "wpmesh" / "ai_web" when set (read once, on first use); absent = Auto (webModeResolve). Without a
 * usable tavily.txt nothing is searched whatever it says (Menu: "Web search: needs tavily.txt"). aiAsk
 * decides with it: a change applies to the NEXT question. */
int  aiWebMode();
bool aiWebModeStored();                   // NVS holds one (else the default, Auto)
void aiSetWebMode(int mode);              // saved to NVS (Menu > AI's row, `ai web <id>`)
bool aiWebUsable();                       // tavily.txt was read good
/* Today's searches (the LOCAL day; NVS ai_webd / ai_webn / ai_webo, so a reboot keeps them): Auto's,
 * capped at WEB_DAILY_AUTO, and the forced / Always ones, counted apart. */
void aiWebToday(int* autoN, int* otherN);

/* LOOP TASK. THE CHOICE (D-175): CHUTES_GEMINI, or a Chutes preset (chutes.h). NVS "wpmesh" /
 * "ai_model" when set (read once, on first use); unset (or a value this firmware does not know),
 * chutesChoiceResolve's default rule against the last key read. aiAsk copies it into the question:
 * a change applies to the NEXT one. */
int  aiModelChoice();
bool aiModelChoiceStored();               // NVS holds one (else the default rule decided)
void aiSetModelChoice(int choice);        // saved to NVS (Menu > AI > Model, `ai model <id>`)
void aiClearModelChoice();                // the default rule again (`ai model default`)
bool aiChoiceUsable(int choice);          // that provider's key file was read good

/* LOOP TASK. "Daily free limit for Flash reached - using Flash-Lite until about 17:00" (the
 * deadline Google gave; "for about 4 h" without a trusted clock, "for now" without a deadline)
 * when the configured model is remembered as used up for today; 0 (and "") when it is not. For
 * Key info. */
size_t aiQuotaNote(char* out, size_t cap);

/* LOOP TASK. "Retry if it times out" (round 3, Menu > AI > Menu): when Gemini sends nothing in time
 * or the connection drops, ask once more, then the fallback once (gemini.h, the ladder's rule 6).
 * ON unless set off. Kept in NVS ("wpmesh" / "ai_retry", the namespace every other on/off setting of
 * this firmware uses - the Almanac's units, Books, Wi-Fi location), so it survives a card swap and a
 * flash; read once, on first use. aiAsk copies it into the question (a change applies to the NEXT
 * question). */
bool aiRetryTimeouts();
void aiSetRetryTimeouts(bool on);

// ── the chat (LOOP TASK) ─────────────────────────────────────────────────────────────────
const GeminiChat* aiChat();               // /ai/chat.txt is read on first use; NULL without PSRAM
uint32_t aiChatGen();                     // changes whenever the chat does
void aiChatNewTopic();                    // and saved
void aiChatClear();                       // the saved file goes too
/* Does the next question - to the model in use, now - start a topic? aiAsk's own rule
 * (chutesTopicDue with this boot's last provider): an empty chat, New topic, two hours on, or (the
 * X4's review, 2026-10-19) the model in use is the other provider than the topic's - a topic never
 * spans Gemini and Chutes. *otherProvider (may be NULL): true when ONLY that last reason holds
 * (Menu > AI's note, `ai`). */
bool aiTopicDue(bool* otherProvider);

// ── a question ───────────────────────────────────────────────────────────────────────────
/* LOOP TASK. Start one. False with `why` (one short sentence for the screen) when it cannot:
 * no key, no WiFi, a download / the uploader / a sync window / a call (or the minute after) /
 * a game / a Files folder job, the internal heap under the handshake's bar, one already asked.
 * `forceWeb` (D-176, Menu > AI's "Ask with web search"): this question searches the web whatever
 * the Auto rule says - unless the setting is Off or there is no usable tavily.txt (webDecide). */
bool aiAsk(const char* question, char* why, size_t whyCap, bool forceWeb = false);
/* LOOP TASK. `ai web test <question>` (D-176): the web search ALONE, on the worker, as a question
 * would make it (the same request, roots, budget, parser and block) - no lookup, no model, no chat.
 * Counted as a forced search. False with `why` when it cannot (Off, no tavily.txt, aiAsk's gates).
 * `ai web` then prints what came back - and, for this bench test only, the block. */
bool aiWebTest(const char* question, char* why, size_t whyCap);
/* Serial `ai web`: the setting, the key's LENGTH, today's counts, the last search - never the key. */
void aiWebReport(void (*emit)(const char* line));
void aiCancel();                          // the answer is thrown away when it lands
/* Asked and not yet in the chat (queued behind a weather fetch included). WiPhone.ino puts it -
 * through netRequestActive() - in `busy` and `hardBusy`: the handshake runs at full speed (160 MHz
 * with WiFi on), never at the 80 MHz idle clock. */
bool aiRequestActive();
typedef struct {
  bool     active;
  bool     cancelled;
  uint32_t gen;             // bumped by every change below
  uint32_t startMs;         // when it was asked
  uint32_t waitUntilMs;     // a retry's wait ends then (0 = not waiting): the countdown
  int      attempt;         // requests made so far
  char     stage[128];      // "Asking Gemini...", "Google is busy - trying again..."
  char     label[GEM_LABEL_MAX];   // the model being asked ("Flash", "DeepSeek V4 Flash")
  char     shortName[24];   // ...its short name ("V4 Flash"; "" for Gemini): the countdown's row
} AiProgress;
void aiProgress(AiProgress* out);
const char* aiPendingQuestion();          // LOOP TASK: the question in flight, or NULL

/* LOOP TASK, every pass: fold a finished question into the chat and save it (a flag test when
 * there is none). */
void aiLoopTick();

/* Serial `ai`: one line per emit, never the key. */
void aiReport(void (*emit)(const char* line));
/* Serial `ai last`: the newest exchange, the answer in lines under 180 characters. */
void aiReportLast(void (*emit)(const char* line));

#endif // AI_NET_H
