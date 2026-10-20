/*
 * chutes.h - the pure half of Menu > AI's SECOND provider (D-175, 2026-10-18): Chutes
 * (llm.chutes.ai), an OpenAI-shaped API in front of open models, with the owner's OWN key. Every
 * rule that can be decided without a network, a card or a screen, so the host suite
 * (tests/test_chutes.cpp) can prove it - the key file, the four presets and their fallback chains,
 * the request, the answer, the ladder, the words, and which provider a topic belongs to. The device
 * half is ai_net.cpp (the second key file, the Bearer header, the root set, NVS `ai_model`); the
 * screens are app_ai.cpp.
 *
 * ⚠ Deliberately free of every Arduino and ESP-IDF header (gemini.h's rule). Keep it that way.
 * What Gemini already had is REUSED, not copied: the chat (GeminiChat), the context rule
 * (geminiContextFirst/Len), the system text (geminiSystemText: the date, the freshness), the
 * screen's ASCII (geminiCleanText/geminiAnswerText), the scrub, json_read.h and the Wikipedia
 * background (wiki_lookup.h). COVEY (covey_ui/chutes.py) has the same presets, chains and words;
 * it streams, this phone does not (v1).
 *
 * THE PUBLIC RULE (Nick): OFF by default, no key shipped. Without /API Keys/chutes.txt nothing here
 * is ever asked, and the AI app is Gemini exactly as before. A user turns it on by putting their
 * own key in that file.
 *
 * WHAT CHUTES ACTUALLY DOES (measured from the Mac with the owner's key, 2026-10-18; the shapes are
 * the fixtures in tests/test_chutes.cpp, texts changed):
 *   - POST https://llm.chutes.ai/v1/chat/completions, `Authorization: Bearer cpk_...` (a header,
 *     never the URL), `Content-Type: application/json`. Not streamed (`"stream": false`), the
 *     answer comes back with a CONTENT-LENGTH (no chunking) once it is whole: 1.1-2.2 s for a
 *     sentence, 12-15 bytes a token plus the thinking text, so the 64 KB sink is ample.
 *   - The answer is choices[0].message.content (a leading space is common); the thinking, when a
 *     preset thinks, is message.reasoning_content - Kimi sends it TWICE, as `reasoning` too - and is
 *     never kept. Some models put it INLINE, as <think>...</think> at the START of the content:
 *     stripped there only (chutesStripThink) - a tag further on is the answer's own words.
 *   - finish_reason "stop"; "length" when max_tokens ran out (Kimi careful with 30 tokens: content
 *     null, all of it thinking); "content_filter" when it declined.
 *   - Errors are FastAPI's {"detail": ...}: 401 {"detail":"Missing or invalid authorization
 *     header(s)"} for a bad key, 404 {"detail":"model not found: <id>"} for a gone model, 400
 *     {"detail":"Invalid request: ..."} for a bad body. 402 (the plan or balance used up) is
 *     {"detail":{"message":"Quota exceeded ... send tao to <WALLET>"}} - 🛑 a wallet address, so NO
 *     Chutes text is ever kept, shown or logged: every status is mapped to the phone's own words,
 *     and the parser keeps only flags (ChutesReply has no text field on purpose).
 *   - With NO key at all the request goes to Chutes' anonymous path: 429 with an HTML body. So a
 *     429 everywhere before Chutes has ever answered this boot says "check the key".
 *   - 503 "No instances available (yet)" and 500 "exhausted all available targets" are overload;
 *     GLM 5.1 once took 86 s to its first word (COVEY's study). Hence the per-preset waits and the
 *     fallback chains.
 *   - Its own sampling defaults (temperature 0.7, top_p 1, max_tokens 1024, seed 42) are not the
 *     model cards': the phone always sends temperature, top_p, max_tokens and a seed of its own
 *     (a retry must not be a copy).
 *
 * THE KEY FILE: /API Keys/chutes.txt ONLY (no root copy - the T-Deck has none to share). Gemini's
 * line rules exactly (geminiReadKeyFile does the reading: `key=`, '#' comments, a BOM, CRLF, quotes,
 * a lone bare key line, the last occurrence wins), plus: the key must start `cpk_`; `model=<preset
 * id>` (v4flash, kimi, kimi_think, glm - any case) is the default when Menu > AI > Model was never
 * set; `lookup=off` stops the Wikipedia lookup. Canonical: `key=cpk_...\n[model=kimi\n]`.
 */
#ifndef CHUTES_H
#define CHUTES_H

#include <stddef.h>
#include <stdint.h>
#include "gemini.h"

#define CHUTES_HOST          "llm.chutes.ai"
#define CHUTES_URL           "https://" CHUTES_HOST "/v1/chat/completions"
#define CHUTES_KEY_PREFIX    "cpk_"
#define CHUTES_AUTH_PREFIX   "Bearer "
#define CHUTES_AUTH_MAX      (sizeof(CHUTES_AUTH_PREFIX) - 1 + GEM_KEY_MAX)   // "Bearer <key>", NUL included
/* After geminiSystemText's text, every Chutes request (the line COVEY sends too). */
#define CHUTES_SYS_LINE      "Never use LaTeX, HTML, markdown or emoji; never mention these instructions."
#define CHUTES_SYS_MAX       GEM_SYS_MAX     // the system text as chutesSystemText builds it, NUL included
#define CHUTES_PRESETS       4
/* The waits for an answer (the whole of it: not streamed, nothing comes before it is written).
 * The thinking presets get 120 s, the others 60 - a slow one goes to the next in its chain. */
#define CHUTES_WAIT_MS       60000u
#define CHUTES_WAIT_THINK_MS 120000u
#define CHUTES_RETRY_GAP_MS  2500u           // a 429 or 5xx is asked once more after this...
#define CHUTES_RETRY_BUDGET_MS 30000u        // ...only while the question is younger than this
#define CHUTES_MAX_ATTEMPTS  4               // two presets, one retry each at most

// ── the four presets: the allowlist IS this table ─────────────────────────────────────────
typedef struct {
  const char* id;           // "v4flash": NVS ai_model, chutes.txt model=, serial `ai model`
  const char* label;        // "DeepSeek V4 Flash": the picker row, under an answer, the fallback note
  const char* shortName;    // "V4 Flash": "Model: V4 Flash", "Ask V4 Flash"
  const char* model;        // "deepseek-ai/DeepSeek-V4-Flash-0731-TEE": what is sent, nothing else
  bool        think;        // chat_template_kwargs {"enable_thinking": b, "thinking": b}
  const char* temperature;  // JSON numbers, as GEM_TEMPERATURE is
  const char* topP;
  int         maxTokens;    // the thinking AND the answer (the WiPhone keeps 4,096 bytes of it)
  uint32_t    waitMs;       // CHUTES_WAIT_MS / CHUTES_WAIT_THINK_MS
  const char* note;         // the picker's one line (COVEY's words)
  const char* nudge;        // this preset's own line after CHUTES_SYS_LINE ("" until the bench says)
  int         chain[2];     // where it falls back to, in order (never across providers)
} ChutesPreset;
const ChutesPreset* chutesPreset(int i);  // NULL outside 0..CHUTES_PRESETS-1
int  chutesPresetFind(const char* id);    // an id (any case) -> its index; -1 = not a preset
/* The first of `preset`'s chain that is not in goneMask (bit i = preset i is gone this boot);
 * -1 = none left. */
int  chutesFallbackOf(int preset, uint32_t goneMask);

// ── the choice: Menu > AI > Model, NVS "ai_model", serial `ai model` ──────────────────────
#define CHUTES_GEMINI        (-1)           // the choice that is not a preset: Gemini (gemini.h)
#define CHUTES_NOT_A_CHOICE  (-2)
#define CHUTES_CHOICES       5              // the picker's rows: the four presets, then Gemini
#define CHUTES_GEMINI_ID     "gemini"
int  chutesChoiceAt(int row);             // picker row 0..4 -> its choice
int  chutesChoiceFind(const char* id);    // "gemini" / a preset id (any case) -> the choice; else CHUTES_NOT_A_CHOICE
const char* chutesChoiceId(int choice);    // "gemini", "v4flash"...
const char* chutesChoiceLabel(int choice); // "Gemini Flash", "DeepSeek V4 Flash"...
const char* chutesChoiceShort(int choice); // "Gemini", "V4 Flash"...
const char* chutesChoiceNote(int choice);  // "Google, free daily limit", "thinks briefly - best all-round"...
const char* chutesChoiceNeeds(int choice); // the note instead when its key file is missing: "needs chutes.txt"
/* THE DEFAULT RULE. `stored` is NVS ai_model (NULL or "" = never set): a choice id is that choice
 * whatever keys there are (the owner's word). Anything else - unset, or a value this firmware does
 * not know - is the default: with a usable Chutes key, chutes.txt's model= (filePreset >= 0) or
 * else v4flash; without one, Gemini - exactly the phone before D-175. */
int  chutesChoiceResolve(const char* stored, bool chutesKey, int filePreset);

// ── the key file ─────────────────────────────────────────────────────────────────────────
enum {
  CHUTES_CFG_OK = 0,
  CHUTES_CFG_NO_KEY,        // no key line, or `key=` empty
  CHUTES_CFG_BAD_KEY,       // not cpk_..., or a space, a control character or a non-ASCII byte in it
};
enum {
  CHUTES_WARN_SHAPE  = 1,   // outside [A-Za-z0-9._-] or 40-200 characters: sent anyway
  CHUTES_WARN_MODEL  = 2,   // a model= that is not a preset id: the default rule
  CHUTES_WARN_LOOKUP = 4,   // a lookup= that is neither on nor off: on
};
typedef struct {
  char key[GEM_KEY_MAX];    // ⚠ the secret. Never printed, logged or drawn.
  int  keyLen;
  int  preset;              // model=: a preset index; -1 = none named (the default rule)
  bool lookup;              // the Wikipedia lookup before each question (`lookup=off`: false)
  int  warn;                // CHUTES_WARN_*
  const char* problem;      // why CHUTES_CFG_BAD_KEY (a literal), else NULL
  /* geminiParseConfig's reading of the file - the parse's own scratch, ALL ZERO outside it (wiped
   * before chutesParseConfig returns). Here, not a local: ~400 bytes on the loop task's 8 KB stack,
   * which app_ai.h keeps free of big locals, and a key copy where nothing would wipe it. */
  GeminiConfig scratch;
} ChutesConfig;
/* Parse a chutes.txt already in memory. Always fills `out`. `out->key` is empty unless CHUTES_CFG_OK. */
int  chutesParseConfig(const char* text, size_t len, ChutesConfig* out);
/* One line, NEVER the key: "key: set (102 chars), model= kimi" (+ ", Wikipedia lookup off") and its
 * warnings; a line that does not fit is CUT with "..." (geminiConfigLine's rule). */
size_t chutesConfigLine(const ChutesConfig* c, int parsed, char* out, size_t cap);

// ── the request ──────────────────────────────────────────────────────────────────────────
/* geminiSystemText (the date from a trusted clock, the freshness, the 240x320 ASCII rules), then
 * " " CHUTES_SYS_LINE, then the preset's nudge when it has one. Same length rule as
 * geminiBuildRequest; the longest form fits CHUTES_SYS_MAX. */
size_t chutesSystemText(int64_t nowUtc, int tzOffsetS, const char* zone, int preset, char* out, size_t cap);
/* This attempt's seed from the phone's RNG: 1..2^31-1, and never the previous attempt's (a retry
 * that sent the same seed could get the same failure back, word for word). */
uint32_t chutesSeed(uint32_t rnd, uint32_t prev);
/* The chat/completions body: model, messages = system, then the context as alternating
 * user/assistant turns (geminiContextFirst's: the newest that fit GEM_SEND_TEXT_CAP, each answer
 * without its own "Wikipedia:" line), then the question's turn - `background` + GEM_BG_QUESTION +
 * `question` when there is background (ONLY this turn carries it), else the question - then
 * max_tokens, temperature, top_p, seed, "stream":false and chat_template_kwargs {"enable_thinking",
 * "thinking"} as the preset says. Never "tools". Returns the length; `out` holds it only when
 * length < cap, else it is left EMPTY (geminiBuildRequest's rule: n = build(NULL, 0) first). */
size_t chutesBuildRequest(int preset, const GeminiTurnPair* ctx, int nCtx, const char* question,
                          const char* background, const char* sysText, uint32_t seed, char* out,
                          size_t cap);
/* "Bearer <key>" for the Authorization header - ⚠ the key: the caller wipes it after the request.
 * Same length rule; a key that is not cpk_... gives "" (nothing is sent as a Bearer that is not a
 * Chutes key). */
size_t chutesAuthHeader(const char* key, char* out, size_t cap);

// ── the answer ───────────────────────────────────────────────────────────────────────────
enum {
  CHUTES_REPLY_TEXT = 0,    // choices[0].message.content had text after the <think> strip (and, after
                            // chutesAnswerShown, text the screen SHOWS)
  CHUTES_REPLY_EMPTY,       // a choice with no text left: `finish` says why
  CHUTES_REPLY_ERROR,       // {"detail": ...} or {"error": ...} (a 200 can carry one too)
  CHUTES_REPLY_BAD,         // not JSON we understand (the 429's and a 404's HTML)
};
/* What the phone keeps of an answer: flags and the finish reason. 🛑 NO TEXT FROM AN ERROR - not
 * the detail, not a message: a 402 names a wallet. */
typedef struct {
  int  kind;                // CHUTES_REPLY_*
  char finish[24];          // choices[0].finish_reason ("stop", "length", "content_filter"); "" = null
  bool textTruncated;       // the content did not fit `textCap`
  bool thought;             // reasoning_content / reasoning / an inline <think> came (never kept)
  bool errModel;            // the error says the model is not found / not there (a 400 that does: gone)
} ChutesReply;
/* Parse a whole body - the answer of `preset` (what was asked: its thinking switch says whether a
 * prompt-opened <think> can be there). The content, every JSON escape decoded, into `text` (raw
 * UTF-8, NUL-terminated), the thinking stripped (chutesStripThink: a prompt-opened tag only for a
 * thinking preset whose thinking came in no reasoning field). Returns out->kind. */
int  chutesParseReply(int preset, const char* body, size_t len, char* text, size_t textCap, ChutesReply* out);
/* In place, AT THE START ONLY (review 2026-10-18): a <think>...</think> the content begins with
 * (after blanks; again while one follows) is cut, an unclosed one takes all; then, when
 * `templateOpened` (the template opened the tag in the prompt), everything up to the FIRST </think>.
 * A tag anywhere else is the answer's own words and stays. Tags in any case. Returns the new length. */
size_t chutesStripThink(char* text, bool templateOpened);
/* geminiAnswerText's (the ASCII, " (cut short)" on "length" or a cut, the "Wikipedia:" line). The
 * X4's review (2026-10-19, COVEY's FINISHES): `preset` is the one that answered; a finish that does
 * not end an answer ("abort", "error": the engine gave up) is " (cut short)" too, and
 * "content_filter" after text is " (stopped - <label> declined to finish)", before the sources line. */
size_t chutesAnswerText(const ChutesReply* r, int preset, const char* text, const char* sources, char* out,
                        size_t cap);
/* The answer as shown, built BEFORE the ladder (the WiPhone's review, 2026-10-19 - COVEY's rule:
 * "any text" is what the screen would SHOW). chutesAnswerText into `out` for a TEXT reply; when
 * that shows nothing (an emoji, a control character or a markdown marker alone), `r` becomes
 * CHUTES_REPLY_EMPTY, so the ladder reads its finish as for no text. Any other reply: `out` "" and
 * `r` untouched (and with no `out`, nothing is judged). Returns the answer's length (0 = none). */
size_t chutesAnswerShown(ChutesReply* r, int preset, const char* text, const char* sources, char* out, size_t cap);
/* Why there is no answer, for a 200 without text: "<label> ran out of room before answering - ask
 * for less" ("length": the thinking took it all), "<label> declined to answer" (content_filter),
 * "Could not read Chutes' answer", "<label> sent an empty answer" ("stop" with nothing in it, and
 * only that: a failed finish or none said is the ladder's busy, worded as chutesFailText's 200). */
size_t chutesNoTextMessage(const ChutesReply* r, int preset, char* out, size_t cap);

// ── the ladder (a loop on the worker, never recursion) ───────────────────────────────────
enum {
  CHUTES_FB_NONE = 0,
  CHUTES_FB_SLOW,           // nothing came within its wait: "<label> was slow - <next> answered"
  CHUTES_FB_DROPPED,        // the connection dropped before the answer: "... dropped out - ..."
  CHUTES_FB_BUSY,           // 429 / 5xx after its one retry: "... was busy - ..."
  CHUTES_FB_GONE,           // 404, or a 400 naming the model: "... was not found - ..."
};
typedef struct {
  int      asked;           // the preset asked for
  int      cur;             // the preset of the next attempt
  bool     onFallback;      // ONE fallback a question, never a second
  int      fbWhy;           // CHUTES_FB_*: why (the answer's label says so)
  int      fbFrom;          // the preset that was left
  bool     retried;         // the one 429/5xx retry, spent on `cur`
  int      attempts;        // requests made for this question
  int      httpAnswers;     // ...that got an HTTP status (for the record: the key rule counts them all)
  int      answers429;      // ...that were 429
  bool     firstOfBoot;     // Chutes has not answered this boot: a 429 on EVERY attempt is the key
  bool     retryTimeouts;   // "Retry if it times out": rule 6 (TRUE at the start; the caller sets it)
  uint32_t startMs;
  uint32_t goneMask;        // presets seen gone this boot (bit i); the caller keeps it between questions
  char     note[GEM_LABEL_MAX + 48];   // a stage worded here ("GLM 5.1 was busy - asking Kimi K2.6...")
} ChutesLadder;
/* EVERY question starts at the preset asked for - unless it was seen gone this boot (goneMask):
 * then straight at its fallback (no request to a model that answered 404), and when none is left,
 * at it all the same (it may be back). Always GEM_STEP_RETRY (go). */
int  chutesLadderStart(ChutesLadder* l, int preset, uint32_t goneMask, bool firstOfBoot, uint32_t nowMs);
/* After an attempt answered `httpCode` (<= 0 = no HTTP answer: `net`, GEM_NET_*, says why). The
 * rules, in order (GEM_STEP_*; RETRY: *delayMs to wait, *stage what to show, `l` holds the next):
 *   0. 200 is DONE - a text, an empty answer (finish "length": the thinking took it all - shown,
 *      never retried blindly), or a body that does not read. A 200 that carries an error object
 *      is rule 5's, and so (the X4's review, 2026-10-19) is a 200 with no text whose finish is not
 *      stop/length/content_filter ("abort": the engine gave up before a word) - or (the WiPhone's
 *      review, 2026-10-19, COVEY's rule) says none at all. "No text" is the screen's: the caller
 *      runs chutesAnswerShown on the reply first.
 *   1. 401 / 403: the key. FAIL - never a retry, never a fallback (every preset has the same key).
 *   2. 402: the plan's daily requests or the balance. FAIL, the same.
 *   3. 404, or a 400 whose error names the model: the model has left Chutes. It is remembered in
 *      goneMask - every preset that sends that model id (kimi and kimi_think share one) - so
 *      later questions skip it and the fallback never is its twin; the fallback is asked at once.
 *      (The X4's review, 2026-10-19: remembered on the question's last attempt too.)
 *   4+5. 429, any 5xx, or a 200 with an error in it: busy. Once more after CHUTES_RETRY_GAP_MS when
 *      the question is younger than CHUTES_RETRY_BUDGET_MS, then the fallback.
 *   6. No answer - the wait ran out (GEM_NET_TIMEOUT) or the connection dropped (GEM_NET_LOST) -
 *      with "Retry if it times out" on: the fallback, at once (a slow model is a busy one; asking
 *      it again is another wait). Off: FAIL. Never a Cancel or a call: the caller ends those first.
 *   ONE fallback a question - the next in the ASKED preset's chain that is not gone - never a
 *   second, never Gemini; and never more than CHUTES_MAX_ATTEMPTS requests.
 * Everything else is FAIL (chutesFailText says what). */
int  chutesLadderNext(ChutesLadder* l, int httpCode, int net, const ChutesReply* r, uint32_t nowMs,
                      uint32_t* delayMs, const char** stage);
/* What goes under the answer: the preset that answered ("Kimi K2.6"), or after the fallback
 * "<left> was slow - <answered> answered". Fits GEM_LABEL_MAX for every pair. */
size_t chutesAnswerLabel(const ChutesLadder* l, char* out, size_t cap);
/* One sentence for a failed question, in the phone's OWN words - never Chutes' text (a 402's names
 * a wallet). `l` (may be NULL) says which preset and whether every ATTEMPT was a first-boot 429 (a
 * timeout or a drop among them is load: "check the key" needs an unbroken run of 429s);
 * `key` is scrubbed anyway (belt and braces: nothing here can hold it). */
size_t chutesFailText(int httpCode, int net, const ChutesReply* r, const ChutesLadder* l, const char* key,
                      char* out, size_t cap);

// ── a topic never spans the two providers (the X4's review, 2026-10-19) ──────────────────
/* Who answered an exchange of the saved chat, told from the label kept under it - the chat keeps
 * nothing else (its saved form is gemini.cpp's, shared with the X4's lib/Gemini, and an older
 * firmware sets aside a file it does not read). The two kinds of label never meet:
 *   CHUTES_BY_CHUTES  chutesAnswerLabel's: a preset's label first ("Kimi K2.6", "GLM 5.1 was busy -
 *                     Kimi K2.6 answered" - every label has a space), or its "Chutes";
 *   CHUTES_BY_GEMINI  geminiModelLabel's: a model name's letters, digits, '.' and '-' ("Flash",
 *                     "Flash-Lite", "2.5-Flash") - never a space;
 *   CHUTES_BY_UNKNOWN anything else: "" (a failed exchange), a label an older firmware wrote
 *                     ("Flash-Lite - daily Flash limit reached"), a preset renamed since. Neither
 *                     provider is sent one. */
enum { CHUTES_BY_UNKNOWN = -1, CHUTES_BY_GEMINI = 0, CHUTES_BY_CHUTES = 1 };
int  chutesAnsweredBy(const char* label);
int  chutesChoiceProvider(int choice);    // CHUTES_BY_CHUTES for a preset, CHUTES_BY_GEMINI otherwise
/* Does a question asked at `now` of `choice` start a topic? geminiChatTopicDue's reasons (an empty
 * chat, New topic, two hours on), and - so that nothing Chutes answered is ever sent to Google, nor
 * Google's answers to Chutes - when `last` (the provider the previous question went to THIS BOOT,
 * CHUTES_BY_*; UNKNOWN after a restart) is the other one, or when any exchange the question would be
 * sent (geminiChatContext's) was not answered by `choice`'s provider (chutesAnsweredBy). `last` is
 * the boot's word, the labels are the card's: a restart, or a chat cloned from another phone, still
 * never crosses. A preset to another preset keeps the topic: the same provider, the same key. */
bool chutesTopicDue(const GeminiChat* c, int64_t now, int choice, int last);

// ── the screen's words that the suite measures (app_ai.cpp draws them) ────────────────────
/* Menu > AI's two notes for the choice in use. Gemini's are the words of before D-175 exactly; a
 * preset's topic note adds that a change to Gemini starts a topic too (the X4's review, 2026-10-19). */
size_t chutesTopicNote(int choice, char* out, size_t cap);
size_t chutesRetryNote(int choice, char* out, size_t cap);
/* Menu > AI's note above those two when the next question starts a topic ONLY because the model in
 * use is the other provider than the topic's (chutesTopicDue; ai_net.h aiTopicDue). */
#define CHUTES_TOPIC_SWITCH  "The next question starts a new topic: a topic never spans Gemini and Chutes."
/* Key info's Chutes section, after Gemini's: NULL-terminated, each one addNoteWrapped's (8 rows). */
extern const char* const CHUTES_KEYINFO_NOTES[];
#define CHUTES_XFER_LINE     "Send gemini.txt or chutes.txt."
/* The uploader's green line once the CHOICE IN USE has its key (review 2026-10-18: "Key found" for
 * either file read a gemini.txt as the chutes.txt being sent). Akrobat 18, measured by test_chutes. */
const char* chutesXferFound(int choice);   // "chutes.txt found - Back to use it." / "gemini.txt found - ..."

#endif // CHUTES_H
