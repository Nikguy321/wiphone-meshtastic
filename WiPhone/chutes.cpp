/*
 * chutes.cpp - see chutes.h. Pure C: no Arduino, no IDF, no allocation. Every buffer is the
 * caller's (PSRAM on the phone), and nothing here recurses - the JSON reader (json_read.cpp) skips
 * nested values by counting brackets - so it is as safe on the AI worker's 8 KB stack as in the
 * host suite (gemini.cpp's rule, and its helpers where they are public).
 */
#include "chutes.h"
#include "gemini.h"
#include "json_read.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// ── small helpers ─────────────────────────────────────────────────────────────────────────

static void cCopy(char* dst, size_t cap, const char* src) {
  if (!dst || cap == 0) {
    return;
  }
  size_t n = src ? strlen(src) : 0;
  if (n >= cap) {
    n = cap - 1;
  }
  if (n) {
    memcpy(dst, src, n);
  }
  dst[n] = '\0';
}

/* `needle` in [s, e), any case. */
static bool cHasNoCaseN(const char* s, const char* e, const char* needle) {
  const size_t nl = strlen(needle);
  for (const char* p = s; p && p + nl <= e; p++) {
    if (strncasecmp(p, needle, nl) == 0) {
      return true;
    }
  }
  return false;
}

static char* cFindNoCase(char* s, const char* needle) {
  const size_t nl = strlen(needle);
  for (char* p = s; *p; p++) {
    if (strncasecmp(p, needle, nl) == 0) {
      return p;
    }
  }
  return NULL;
}

/* gemini.cpp's writer (GemW), the same rule: bytes go in while they fit, the count goes on
 * regardless, and a body that did not fit is left EMPTY - never half a JSON body. */
struct ChW {
  char*  out;
  size_t cap;
  size_t n;
};

static void wPut(ChW* w, const char* s, size_t len) {
  if (w->out && w->n + len < w->cap) {
    memcpy(w->out + w->n, s, len);
  }
  w->n += len;
}

static void wStr(ChW* w, const char* s) {
  wPut(w, s, strlen(s));
}

static size_t wEnd(ChW* w) {
  if (w->out && w->cap) {
    if (w->n < w->cap) {
      w->out[w->n] = '\0';
    } else {
      w->out[0] = '\0';
    }
  }
  return w->n;
}

/* JSON-escaped, the first `n` bytes of `s` at most (geminiJsonEscape's rules: " and \ escaped, \n
 * and \t kept as escapes, \r dropped, other control characters \u00XX, bytes >= 0x80 as they are). */
static void wEscN(ChW* w, const char* s, size_t n) {
  if (!s) {
    return;
  }
  const unsigned char* end = (const unsigned char*)s + n;
  for (const unsigned char* p = (const unsigned char*)s; p < end && *p; p++) {
    const unsigned char c = *p;
    if (c == '"') {
      wStr(w, "\\\"");
    } else if (c == '\\') {
      wStr(w, "\\\\");
    } else if (c == '\n') {
      wStr(w, "\\n");
    } else if (c == '\t') {
      wStr(w, "\\t");
    } else if (c == '\r') {
      // dropped
    } else if (c < 0x20 || c == 0x7F) {
      char u[8];
      snprintf(u, sizeof(u), "\\u%04x", (unsigned)c);
      wStr(w, u);
    } else {
      wPut(w, (const char*)p, 1);
    }
  }
}

static void wEsc(ChW* w, const char* s) {
  wEscN(w, s, s ? strlen(s) : 0);
}

/* geminiConfigLine's writer: a status line that does not fit is CUT, "..." at its end. */
static void lineAdd(char* out, size_t cap, size_t* n, const char* s) {
  const size_t l = strlen(s);
  if (out && cap && *n + 1 < cap) {
    const size_t room = cap - 1 - *n;
    memcpy(out + *n, s, l < room ? l : room);
  }
  *n += l;
}

// ── the presets ───────────────────────────────────────────────────────────────────────────

enum { P_V4FLASH = 0, P_KIMI, P_KIMI_THINK, P_GLM };

/* Nick's menu (2026-10-18, COVEY's study: eight questions each, every answer read by hand), the
 * WiPhone's max_tokens (D-175: a 4,096-byte ASCII answer and 24 KB of text, so less room than COVEY's
 * 2000/1200/4096/1200). Thinking counts against max_tokens. Off measured 0 reasoning tokens on all
 * four; V4 Flash thinks ~200-300 tokens for ~2 s, Kimi careful ~15 s. */
static const ChutesPreset PRESETS[CHUTES_PRESETS] = {
  { "v4flash", "DeepSeek V4 Flash", "V4 Flash", "deepseek-ai/DeepSeek-V4-Flash-0731-TEE", true, "0.7", "0.95",
    1500, CHUTES_WAIT_THINK_MS, "thinks briefly - best all-round", "", { P_KIMI, P_GLM } },
  { "kimi", "Kimi K2.6", "Kimi", "moonshotai/Kimi-K2.6-TEE", false, "0.6", "0.95",
    1000, CHUTES_WAIT_MS, "fast, no thinking, wide knowledge", "", { P_V4FLASH, P_GLM } },
  { "kimi_think", "Kimi K2.6 careful", "Kimi careful", "moonshotai/Kimi-K2.6-TEE", true, "1.0", "0.95",
    3000, CHUTES_WAIT_THINK_MS, "thinks it through, about 15 s", "", { P_V4FLASH, P_KIMI } },
  { "glm", "GLM 5.1", "GLM", "zai-org/GLM-5.1-TEE", false, "0.6", "0.95",
    1000, CHUTES_WAIT_MS, "a second opinion; slow when busy", "", { P_KIMI, P_V4FLASH } },
};

const ChutesPreset* chutesPreset(int i) {
  return (i >= 0 && i < CHUTES_PRESETS) ? &PRESETS[i] : NULL;
}

int chutesPresetFind(const char* id) {
  if (!id) {
    return -1;
  }
  for (int i = 0; i < CHUTES_PRESETS; i++) {
    if (!strcasecmp(id, PRESETS[i].id)) {
      return i;
    }
  }
  return -1;
}

int chutesFallbackOf(int preset, uint32_t goneMask) {
  const ChutesPreset* p = chutesPreset(preset);
  if (!p) {
    return -1;
  }
  for (int k = 0; k < 2; k++) {
    const int c = p->chain[k];
    if (c != preset && chutesPreset(c) && !(goneMask & (1u << c))) {
      return c;
    }
  }
  return -1;
}

// ── the choice ────────────────────────────────────────────────────────────────────────────

int chutesChoiceAt(int row) {
  if (row >= 0 && row < CHUTES_PRESETS) {
    return row;
  }
  return row == CHUTES_PRESETS ? CHUTES_GEMINI : CHUTES_NOT_A_CHOICE;
}

int chutesChoiceFind(const char* id) {
  if (!id || !id[0]) {
    return CHUTES_NOT_A_CHOICE;
  }
  if (!strcasecmp(id, CHUTES_GEMINI_ID)) {
    return CHUTES_GEMINI;
  }
  const int p = chutesPresetFind(id);
  return p >= 0 ? p : CHUTES_NOT_A_CHOICE;
}

const char* chutesChoiceId(int choice) {
  const ChutesPreset* p = chutesPreset(choice);
  return p ? p->id : CHUTES_GEMINI_ID;
}

const char* chutesChoiceLabel(int choice) {
  const ChutesPreset* p = chutesPreset(choice);
  return p ? p->label : "Gemini Flash";
}

const char* chutesChoiceShort(int choice) {
  const ChutesPreset* p = chutesPreset(choice);
  return p ? p->shortName : "Gemini";
}

const char* chutesChoiceNote(int choice) {
  const ChutesPreset* p = chutesPreset(choice);
  return p ? p->note : "Google, free daily limit";
}

const char* chutesChoiceNeeds(int choice) {
  return chutesPreset(choice) ? "needs chutes.txt" : "needs gemini.txt";
}

int chutesChoiceResolve(const char* stored, bool chutesKey, int filePreset) {
  const int c = chutesChoiceFind(stored);
  if (c != CHUTES_NOT_A_CHOICE) {
    return c;                                    // set on the phone: the owner's word, whatever the keys
  }
  if (chutesKey) {
    return chutesPreset(filePreset) ? filePreset : P_V4FLASH;
  }
  return CHUTES_GEMINI;                          // no Chutes key: the phone of before D-175
}

// ── the key file ─────────────────────────────────────────────────────────────────────────

static bool cSpace(char c) {
  return c == ' ' || c == '\t';
}

/* [s, e) with spaces off both ends and one pair of matching quotes off the outside (gemini.cpp's
 * gTrimValue, the same file rules). */
static void cTrimValue(const char** s, const char** e) {
  while (*s < *e && cSpace(**s)) (*s)++;
  while (*e > *s && cSpace((*e)[-1])) (*e)--;
  if (*e - *s >= 2 && ((**s == '"' && (*e)[-1] == '"') || (**s == '\'' && (*e)[-1] == '\''))) {
    (*s)++;
    (*e)--;
    while (*s < *e && cSpace(**s)) (*s)++;
    while (*e > *s && cSpace((*e)[-1])) (*e)--;
  }
}

/* The value of the LAST `name=` line (any case, spaces around '=', '#' comments, a BOM, LF or CRLF -
 * geminiParseConfig's line rules). False when there is none. */
static bool cfgLast(const char* text, size_t len, const char* name, const char** vs, const char** ve) {
  const char* p = text;
  const char* end = text + len;
  if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
    p += 3;
  }
  bool found = false;
  const size_t nl = strlen(name);
  while (p < end) {
    const char* ls = p;
    const char* le = p;
    while (le < end && *le != '\n' && *le != '\r') le++;
    p = le;
    while (p < end && (*p == '\n' || *p == '\r')) p++;
    while (ls < le && cSpace(*ls)) ls++;
    while (le > ls && cSpace(le[-1])) le--;
    if (ls == le || *ls == '#') {
      continue;
    }
    const char* eq = (const char*)memchr(ls, '=', (size_t)(le - ls));
    if (!eq) {
      continue;
    }
    const char* ne = eq;
    while (ne > ls && cSpace(ne[-1])) ne--;
    if ((size_t)(ne - ls) != nl || strncasecmp(ls, name, nl) != 0) {
      continue;
    }
    const char* s = eq + 1;
    const char* e = le;
    cTrimValue(&s, &e);
    *vs = s;
    *ve = e;
    found = true;                                // the last one wins
  }
  return found;
}

int chutesParseConfig(const char* text, size_t len, ChutesConfig* out) {
  memset(out, 0, sizeof(*out));
  out->preset = -1;
  out->lookup = true;
  if (!text) {
    len = 0;
  }
  /* THE READING IS GEMINI'S: one reader of the file rules for both key files (`key=`, a lone bare
   * key line, quotes, a BOM, CRLF, the last one wins, a space or an odd byte refused) and of
   * `lookup=` - geminiReadKeyFile, the rules without Gemini's own key rule (which refuses cpk_).
   * Only `model=` means something else here - a preset id - so it is read below.
   * ⚠ `g` (the caller's struct, PSRAM on the phone) holds the key for these lines: wiped below. */
  GeminiConfig* g = &out->scratch;
  const int rc = geminiReadKeyFile(text, len, g);
  out->lookup = g->lookup;
  if (g->warn & GEM_WARN_LOOKUP) {
    out->warn |= CHUTES_WARN_LOOKUP;
  }
  const char* vs = NULL;
  const char* ve = NULL;
  if (text && cfgLast(text, len, "model", &vs, &ve) && vs < ve) {
    /* An exact preset id or nothing: the allowlist is the table. A key pasted on this line is no
     * id (none starts cpk_), so it is never sent as a model, drawn or printed - it is dropped. */
    char id[24];
    const size_t n = (size_t)(ve - vs);
    if (n < sizeof(id)) {
      memcpy(id, vs, n);
      id[n] = '\0';
      out->preset = chutesPresetFind(id);
    }
    geminiWipe(id, sizeof(id));                  // a memset of a local about to die is dropped
    if (out->preset < 0) {
      out->warn |= CHUTES_WARN_MODEL;
    }
  }
  int ret;
  if (rc == GEM_CFG_NO_KEY) {
    ret = CHUTES_CFG_NO_KEY;
  } else if (rc != GEM_CFG_OK) {
    out->problem = g->problem;                    // a literal: never the key
    ret = CHUTES_CFG_BAD_KEY;
  } else if (strncmp(g->key, CHUTES_KEY_PREFIX, sizeof(CHUTES_KEY_PREFIX) - 1) != 0) {
    /* Not a Chutes key - a Gemini key in the wrong file, most likely, or (D-176) a Tavily one, which
     * is named. Refused: it would go to Chutes in a Bearer header. */
    out->problem = strncmp(g->key, GEM_TAVILY_KEY_PREFIX, sizeof(GEM_TAVILY_KEY_PREFIX) - 1) == 0
                     ? "this is a Tavily key - put it in tavily.txt"
                     : "the key does not start cpk_ - is it a Chutes key?";
    ret = CHUTES_CFG_BAD_KEY;
  } else if (g->keyLen <= (int)(sizeof(CHUTES_KEY_PREFIX) - 1)) {
    out->problem = "the key is only cpk_";
    ret = CHUTES_CFG_BAD_KEY;
  } else {
    memcpy(out->key, g->key, (size_t)g->keyLen + 1);
    out->keyLen = g->keyLen;
    bool shapeOk = g->keyLen >= 40 && g->keyLen <= 200;    // measured: 102
    for (int i = 0; i < g->keyLen && shapeOk; i++) {
      const char c = g->key[i];
      if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) {
        shapeOk = false;
      }
    }
    if (!shapeOk) {
      out->warn |= CHUTES_WARN_SHAPE;
    }
    ret = CHUTES_CFG_OK;
  }
  geminiWipe(g, sizeof(*g));                     // the scratch is all zero outside the parse
  return ret;
}

size_t chutesConfigLine(const ChutesConfig* c, int parsed, char* out, size_t cap) {
  size_t n = 0;
  char num[48];
  if (parsed == CHUTES_CFG_OK) {
    snprintf(num, sizeof(num), "key: set (%d chars)", c->keyLen);
    lineAdd(out, cap, &n, num);
  } else if (parsed == CHUTES_CFG_BAD_KEY) {
    lineAdd(out, cap, &n, "key: refused - ");
    lineAdd(out, cap, &n, c->problem ? c->problem : "unreadable");
  } else {
    lineAdd(out, cap, &n, "key: none");
  }
  if (chutesPreset(c->preset)) {
    lineAdd(out, cap, &n, ", model= ");
    lineAdd(out, cap, &n, PRESETS[c->preset].id);
  }
  if (!c->lookup)                     lineAdd(out, cap, &n, ", Wikipedia lookup off");
  if (c->warn & CHUTES_WARN_SHAPE)    lineAdd(out, cap, &n, "; the key's shape is unusual (sent anyway)");
  if (c->warn & CHUTES_WARN_MODEL)    lineAdd(out, cap, &n, "; model= line ignored (not v4flash, kimi, kimi_think or glm)");
  if (c->warn & CHUTES_WARN_LOOKUP)   lineAdd(out, cap, &n, "; lookup= must be on or off (on used)");
  if (out && cap) {
    if (n < cap) {
      out[n] = '\0';
    } else {
      out[cap - 1] = '\0';
      if (cap >= 4) {
        memcpy(out + cap - 4, "...", 3);         // cut, and says so: never an empty line
      }
    }
  }
  return n;
}

// ── the request ──────────────────────────────────────────────────────────────────────────

size_t chutesSystemText(int64_t nowUtc, int tzOffsetS, const char* zone, int preset, char* out, size_t cap) {
  /* Gemini's text first (out holds it only when it fits), then this one's lines after it - the
   * writer goes on from its length, so a text that does not fit still comes back EMPTY. */
  const size_t g = geminiSystemText(nowUtc, tzOffsetS, zone, out, cap);
  ChW w = { out, cap, g };
  wStr(&w, " " CHUTES_SYS_LINE);
  const ChutesPreset* p = chutesPreset(preset);
  if (p && p->nudge && p->nudge[0]) {
    wStr(&w, " ");
    wStr(&w, p->nudge);
  }
  return wEnd(&w);
}

uint32_t chutesSeed(uint32_t rnd, uint32_t prev) {
  uint32_t s = rnd & 0x7FFFFFFFu;
  if (s == 0) {
    s = 1;
  }
  if (s == prev) {
    s = s == 0x7FFFFFFFu ? 1u : s + 1u;          // a retry is never the same draw
  }
  return s;
}

size_t chutesBuildRequest(int preset, const GeminiTurnPair* ctx, int nCtx, const char* question,
                          const char* background, const char* sysText, uint32_t seed, char* out,
                          size_t cap) {
  const ChutesPreset* p = chutesPreset(preset);
  ChW w = { out, cap, 0 };
  if (!p) {
    return wEnd(&w);                             // 0: nothing to send for a preset that is not one
  }
  const bool bg = background && background[0];
  const int first = geminiContextFirst(ctx, nCtx, question, background);   // Gemini's turns, exactly
  wStr(&w, "{\"model\":\"");
  wEsc(&w, p->model);
  wStr(&w, "\",\"messages\":[{\"role\":\"system\",\"content\":\"");
  wEsc(&w, sysText ? sysText : "");
  wStr(&w, "\"}");
  for (int i = first; i < nCtx; i++) {
    wStr(&w, ",{\"role\":\"user\",\"content\":\"");
    wEsc(&w, ctx[i].q);
    wStr(&w, "\"},{\"role\":\"assistant\",\"content\":\"");
    wEscN(&w, ctx[i].a, geminiContextLen(ctx[i].a));   // without its own "Wikipedia:" line
    wStr(&w, "\"}");
  }
  /* The question's own turn - and only it - carries the lookup's background, labelled as such:
   * Wikipedia's text is data, never the system text (gemini.cpp's rule). */
  wStr(&w, ",{\"role\":\"user\",\"content\":\"");
  if (bg) {
    wEsc(&w, background);
    wEsc(&w, GEM_BG_QUESTION);
  }
  wEsc(&w, question ? question : "");
  wStr(&w, "\"}]");                              // and never "tools" (gemini.h, CURRENT FACTS)
  char num[160];
  snprintf(num, sizeof(num), ",\"max_tokens\":%d,\"temperature\":%s,\"top_p\":%s,\"seed\":%lu,\"stream\":false",
           p->maxTokens, p->temperature, p->topP, (unsigned long)seed);
  wStr(&w, num);
  /* Both spellings of the switch (measured: "thinking" is Kimi's and DeepSeek's, "enable_thinking"
   * GLM's; a template ignores the one it does not know). */
  wStr(&w, p->think ? ",\"chat_template_kwargs\":{\"enable_thinking\":true,\"thinking\":true}}"
                    : ",\"chat_template_kwargs\":{\"enable_thinking\":false,\"thinking\":false}}");
  return wEnd(&w);
}

size_t chutesAuthHeader(const char* key, char* out, size_t cap) {
  ChW w = { out, cap, 0 };
  if (!key || strncmp(key, CHUTES_KEY_PREFIX, sizeof(CHUTES_KEY_PREFIX) - 1) != 0) {
    return wEnd(&w);                             // "": nothing goes as a Bearer that is not a Chutes key
  }
  wStr(&w, CHUTES_AUTH_PREFIX);
  wStr(&w, key);
  return wEnd(&w);
}

// ── the answer ───────────────────────────────────────────────────────────────────────────

/* A raw JSON value [s, e) - a "detail" or an "error", read only for this - that says the MODEL is
 * not there: "model not found: <id>" (measured), "... does not exist", "unknown model"... Its text
 * is never copied anywhere (a 402's names a wallet). */
static bool spanNamesModel(const char* s, const char* e) {
  if (!cHasNoCaseN(s, e, "model")) {
    return false;
  }
  static const char* const GONE[] = { "not found", "not exist", "unknown", "not available", "unavailable",
                                      "no longer", "not supported", "invalid model", "no such" };
  for (size_t i = 0; i < sizeof(GONE) / sizeof(GONE[0]); i++) {
    if (cHasNoCaseN(s, e, GONE[i])) {
      return true;
    }
  }
  return false;
}

static int parseMessage(Js* j, char* text, size_t textCap, size_t* tlen, ChutesReply* out) {
  if (!jsOpen(j, '{')) return jsSkip(j) ? 0 : -1;
  char key[24];
  int r;
  while ((r = jsObjNext(j, key, sizeof(key))) == 1) {
    if (!strcmp(key, "content")) {
      jsWs(j);
      if (j->p < j->e && *j->p == '"') {
        if (!jsString(j, text, textCap, tlen, &out->textTruncated)) return -1;
      } else if (!jsSkip(j)) {
        return -1;                               // null: all of it was thinking ("length")
      }
    } else if (!strcmp(key, "reasoning_content") || !strcmp(key, "reasoning")) {
      /* The thinking (Kimi sends it under both names): skipped UNREAD - never the answer, never
       * kept, never sent back. Only that it came. */
      jsWs(j);
      if (j->p < j->e && *j->p == '"') out->thought = true;
      if (!jsSkip(j)) return -1;
    } else if (!jsSkip(j)) {
      return -1;
    }
  }
  return r < 0 ? -1 : 0;
}

static int parseChoice(Js* j, char* text, size_t textCap, size_t* tlen, ChutesReply* out) {
  if (!jsOpen(j, '{')) return jsSkip(j) ? 0 : -1;
  char key[24];
  int r;
  while ((r = jsObjNext(j, key, sizeof(key))) == 1) {
    if (!strcmp(key, "message")) {
      if (parseMessage(j, text, textCap, tlen, out) < 0) return -1;
    } else if (!strcmp(key, "finish_reason")) {
      if (!jsStrInto(j, out->finish, sizeof(out->finish))) return -1;   // null: skipped, "" stays
    } else if (!jsSkip(j)) {
      return -1;
    }
  }
  return r < 0 ? -1 : 0;
}

size_t chutesStripThink(char* text, bool templateOpened) {
  if (!text) {
    return 0;
  }
  /* ONLY AT THE START (review 2026-10-18). The inline shape is the model writing its thinking
   * BEFORE the answer; a tag further on is the answer's own words ("its reasoning starts with a
   * <think> tag"). Cut there - as the first cut did, anywhere - half an answer was shown as a whole
   * one, with no "(cut short)", saved to the chat and sent back as context. */
  for (;;) {
    char* s = text;
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++;
    if (strncasecmp(s, "<think>", 7) != 0) {
      break;
    }
    char* close = cFindNoCase(s + 7, "</think>");
    if (!close) {
      text[0] = '\0';                            // never closed: all thinking (max_tokens ran out in it)
      break;
    }
    memmove(text, close + 8, strlen(close + 8) + 1);
    templateOpened = false;                      // the model opened its own: the prompt did not
  }
  /* The template opened the tag in the PROMPT: "...thinking...</think>answer". All up to the FIRST
   * </think> is thinking, whatever is in it (a "<think>" too). Only when the caller says so - a
   * thinking preset whose thinking came in no field of its own: never Kimi's or GLM's answer with
   * thinking off, which may name the tag. */
  if (templateOpened) {
    char* close = cFindNoCase(text, "</think>");
    if (close) {
      memmove(text, close + 8, strlen(close + 8) + 1);
    }
  }
  return strlen(text);
}

int chutesParseReply(int preset, const char* body, size_t len, char* text, size_t textCap, ChutesReply* out) {
  memset(out, 0, sizeof(*out));
  out->kind = CHUTES_REPLY_BAD;
  if (text && textCap) text[0] = '\0';
  if (!body) {
    return out->kind;
  }
  Js j = { body, body + len };
  size_t tlen = 0;
  bool choices = false, err = false;
  if (!jsOpen(&j, '{')) {
    return out->kind;                            // the anonymous path's 429 and a 404 are HTML
  }
  char key[24];
  int r;
  while ((r = jsObjNext(&j, key, sizeof(key))) == 1) {
    if (!strcmp(key, "choices") && jsOpen(&j, '[')) {
      choices = true;                            // [] too: a reply, with no answer in it
      int r2;
      bool firstDone = false;
      while ((r2 = jsArrNext(&j)) == 1) {
        if (!firstDone) {
          if (parseChoice(&j, text, textCap, &tlen, out) < 0) return out->kind;
          firstDone = true;
        } else if (!jsSkip(&j)) {
          return out->kind;
        }
      }
      if (r2 < 0) return out->kind;
    } else if (!strcmp(key, "detail") || !strcmp(key, "error")) {
      /* FastAPI's {"detail": ...} (every error measured) or OpenAI's {"error": {...}}: skipped
       * UNREAD, its raw bytes only asked whether they say the model is gone. */
      jsWs(&j);
      const char* s = j.p;
      if (!jsSkip(&j)) return out->kind;
      out->errModel = out->errModel || spanNamesModel(s, j.p);
      err = true;
    } else if (!jsSkip(&j)) {
      return out->kind;
    }
  }
  if (r < 0) {
    return out->kind;                            // cut short or not JSON: BAD, whatever was read
  }
  if (err) {
    out->kind = CHUTES_REPLY_ERROR;
    if (text && textCap) text[0] = '\0';
    return out->kind;
  }
  if (!choices) {
    return out->kind;
  }
  if (text && textCap) {
    /* out->thought here is the reasoning fields' alone: all four presets send their thinking there
     * (measured), so a prompt-opened tag is asked about only when a thinking preset sent none. */
    const ChutesPreset* p = chutesPreset(preset);
    const size_t before = strlen(text);
    if (chutesStripThink(text, p && p->think && !out->thought) != before) {
      out->thought = true;
    }
  }
  bool any = false;
  for (const char* p = text; p && *p && !any; p++) {
    any = !isspace((unsigned char)*p);
  }
  out->kind = any ? CHUTES_REPLY_TEXT : CHUTES_REPLY_EMPTY;
  return out->kind;
}

/* The X4's review (2026-10-19, COVEY's FINISHES): the finish_reasons that END an answer. Any
 * other - vLLM's and SGLang's "abort", newer vLLM's "error" - is a generation that FAILED. ""
 * (null) is none said: after text, an answer as before; with none, the ladder's busy (rule 0). */
static bool finishFailed(const char* finish) {
  return finish && finish[0] && strcmp(finish, "stop") && strcmp(finish, "length") &&
         strcmp(finish, "content_filter");
}

size_t chutesAnswerText(const ChutesReply* r, int preset, const char* text, const char* sources, char* out,
                        size_t cap) {
  /* Gemini's answer rule, told what Chutes said in its own spelling: "length" is MAX_TOKENS - and
   * so (the X4's review, 2026-10-19) is a FAILED finish ("abort", "error"): cut short, never shown
   * or kept as a whole answer. "content_filter" after text gets its own note below. */
  const char* f = r ? r->finish : "";
  const bool declined = !strcmp(f, "content_filter");
  GeminiReply g;
  memset(&g, 0, sizeof(g));
  g.kind = GEM_REPLY_TEXT;
  g.retryDelayS = -1;
  g.textTruncated = r && r->textTruncated;
  cCopy(g.finish, sizeof(g.finish), (!strcmp(f, "length") || finishFailed(f)) ? "MAX_TOKENS" : "STOP");
  /* Kimi starts its content with a space (measured), and a stripped <think> leaves the blank lines
   * after it: the cleaner keeps a line's leading spaces as indentation, so they go here. */
  while (text && (*text == ' ' || *text == '\n' || *text == '\r' || *text == '\t')) {
    text++;
  }
  if (!declined) {
    return geminiAnswerText(&g, text, sources, out, cap);
  }
  /* The X4's review (2026-10-19): "content_filter" after text - the model stopped itself partway.
   * COVEY's words, where the cut note goes (before the sources line), its room kept as that one's is. */
  char note[GEM_LABEL_MAX + 40];
  snprintf(note, sizeof(note), " (stopped - %s declined to finish)",
           chutesPreset(preset) ? PRESETS[preset].label : "the model");
  const size_t nl = strlen(note);
  if (!out || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  if (cap <= nl + 1) {
    return 0;
  }
  const size_t n = geminiAnswerText(&g, text, sources, out, cap - nl);
  if (n == 0) {
    return 0;
  }
  size_t at = n;
  const size_t sl = (sources && sources[0]) ? strlen(sources) : 0;
  if (sl && n >= sl + 2 && !memcmp(out + n - sl - 2, "\n\n", 2) && !memcmp(out + n - sl, sources, sl)) {
    at = n - sl - 2;
  }
  memmove(out + at + nl, out + at, n - at + 1);
  memcpy(out + at, note, nl);
  return n + nl;
}

size_t chutesAnswerShown(ChutesReply* r, int preset, const char* text, const char* sources, char* out, size_t cap) {
  if (!out || cap == 0) {
    return 0;                                    // nowhere to build it: nothing judged, `r` untouched
  }
  out[0] = '\0';
  if (!r || r->kind != CHUTES_REPLY_TEXT) {
    return 0;
  }
  /* COVEY's rule (clean_text; the WiPhone's review, 2026-10-19): "any text" is what the screen would
   * SHOW. The parser calls any byte that is not a blank TEXT, so an emoji, a control character or a
   * markdown marker alone is TEXT there - and the cleaner drops them all. Judged here, before the
   * ladder, so rule 0 reads its finish as for no text: "abort" or none said is busy, never "sent an
   * empty answer". */
  const size_t n = chutesAnswerText(r, preset, text, sources, out, cap);
  if (n == 0) {
    r->kind = CHUTES_REPLY_EMPTY;
  }
  return n;
}

size_t chutesNoTextMessage(const ChutesReply* r, int preset, char* out, size_t cap) {
  ChW w = { out, cap, 0 };
  const char* label = chutesPreset(preset) ? PRESETS[preset].label : "Chutes";
  if (!r || r->kind == CHUTES_REPLY_BAD) {
    wStr(&w, "Could not read Chutes' answer");
  } else if (r->kind == CHUTES_REPLY_ERROR) {
    wStr(&w, "Chutes could not answer just now - ask again");
  } else if (!strcmp(r->finish, "length")) {
    /* The thinking took the whole of max_tokens (measured: content null, finish "length"). Never
     * retried blindly: the same question would think the same way. */
    wStr(&w, label);
    wStr(&w, " ran out of room before answering - ask for less");
  } else if (!strcmp(r->finish, "content_filter")) {
    wStr(&w, label);
    wStr(&w, " declined to answer");
  } else if (!strcmp(r->finish, "stop")) {
    /* COVEY's words, and only for this (the WiPhone's review, 2026-10-19): a model that SAID it was
     * done, with nothing in it. */
    wStr(&w, label);
    wStr(&w, " sent an empty answer");
  } else {
    /* A failed finish or none said: the ladder's busy (rule 0), so never DONE - chutesFailText's
     * words for a 200, should a caller show it anyway. */
    wStr(&w, "Chutes is busy right now - try again in a minute");
  }
  return wEnd(&w);
}

// ── the ladder ───────────────────────────────────────────────────────────────────────────

static void ladderNote(ChutesLadder* l, int why, int from, int to) {
  const char* a = PRESETS[from].label;
  const char* b = PRESETS[to].label;
  switch (why) {
  case CHUTES_FB_SLOW:    snprintf(l->note, sizeof(l->note), "%s was slow - asking %s...", a, b); break;
  case CHUTES_FB_DROPPED: snprintf(l->note, sizeof(l->note), "%s dropped out - asking %s...", a, b); break;
  case CHUTES_FB_GONE:    snprintf(l->note, sizeof(l->note), "%s is not offered any more - asking %s...", a, b); break;
  default:                snprintf(l->note, sizeof(l->note), "%s is busy - asking %s...", a, b); break;
  }
}

int chutesLadderStart(ChutesLadder* l, int preset, uint32_t goneMask, bool firstOfBoot, uint32_t nowMs) {
  memset(l, 0, sizeof(*l));
  l->asked = chutesPreset(preset) ? preset : P_V4FLASH;
  l->cur = l->asked;
  l->goneMask = goneMask;
  l->firstOfBoot = firstOfBoot;
  l->retryTimeouts = true;                       // the setting's default: the caller sets it after
  l->startMs = nowMs;
  if (goneMask & (1u << l->asked)) {
    /* Seen gone this boot (rule 3): straight to its fallback - that IS the question's one fallback. */
    const int fb = chutesFallbackOf(l->asked, goneMask);
    if (fb >= 0) {
      l->fbFrom = l->asked;
      l->cur = fb;
      l->onFallback = true;
      l->fbWhy = CHUTES_FB_GONE;
      ladderNote(l, CHUTES_FB_GONE, l->asked, fb);
    }
  }
  return GEM_STEP_RETRY;
}

/* To the next in the ASKED preset's chain that is not gone - ONCE a question; else FAIL. */
static int ladderFallback(ChutesLadder* l, int why, uint32_t delay, uint32_t* delayMs, const char** stage) {
  if (l->onFallback) {
    return GEM_STEP_FAIL;                        // one fallback, never a second (nor Gemini)
  }
  const int fb = chutesFallbackOf(l->asked, l->goneMask);
  if (fb < 0 || fb == l->cur) {
    return GEM_STEP_FAIL;
  }
  l->fbFrom = l->cur;
  l->cur = fb;
  l->onFallback = true;
  l->fbWhy = why;
  l->retried = false;                            // its own one retry
  ladderNote(l, why, l->fbFrom, fb);
  *delayMs = delay;
  *stage = l->note;
  return GEM_STEP_RETRY;
}

int chutesLadderNext(ChutesLadder* l, int httpCode, int net, const ChutesReply* r, uint32_t nowMs,
                     uint32_t* delayMs, const char** stage) {
  *delayMs = 0;
  *stage = NULL;
  l->attempts++;
  if (httpCode > 0) {
    l->httpAnswers++;
    if (httpCode == 429) {
      l->answers429++;
    }
  }
  const bool errorIn200 = httpCode == 200 && r && r->kind == CHUTES_REPLY_ERROR;
  /* The X4's review (2026-10-19, COVEY's FINISHES): no text and a finish that does not end an answer
   * ("abort": the engine gave up before a word) is a server failure - rule 5's, never "sent an
   * empty answer". So (the WiPhone's review, 2026-10-19 - COVEY's rule) is no text and NO finish
   * said (null, or no finish_reason in the body): only "stop" with nothing in it is an empty answer.
   * "No text" is what the screen would show (chutesAnswerShown, before this). */
  const bool failed200 = httpCode == 200 && r && r->kind == CHUTES_REPLY_EMPTY &&
                         (finishFailed(r->finish) || !r->finish[0]);
  // 0. An answer - a text, an empty one ("stop", "length", "content_filter"), or one that does not read: DONE.
  if (httpCode == 200 && !errorIn200 && !failed200) {
    return GEM_STEP_DONE;
  }
  // 6. No answer: the wait ran out or the connection dropped. The fallback at once (setting on).
  if (httpCode <= 0) {
    if (!l->retryTimeouts || (net != GEM_NET_TIMEOUT && net != GEM_NET_LOST) || l->attempts >= CHUTES_MAX_ATTEMPTS) {
      return GEM_STEP_FAIL;                      // DNS, a refused connect, TLS, memory...: at once
    }
    return ladderFallback(l, net == GEM_NET_TIMEOUT ? CHUTES_FB_SLOW : CHUTES_FB_DROPPED, 0, delayMs, stage);
  }
  // 3. The model has left Chutes: remembered for the boot, and the fallback at once. Gone is the
  //    MODEL ("model not found: <id>"), not the preset: kimi and kimi_think send one id, so both are
  //    marked (review 2026-10-18: the twin was the next asked - a request sure to 404).
  //    The X4's review (2026-10-19): remembered BEFORE the attempt cap - a 404 on the question's last
  //    attempt says "not offered any more" and the next question must skip it too.
  const bool gone = httpCode == 404 || (httpCode == 400 && r && r->kind == CHUTES_REPLY_ERROR && r->errModel);
  if (gone) {
    const char* model = PRESETS[l->cur].model;
    for (int i = 0; i < CHUTES_PRESETS; i++) {
      if (!strcmp(PRESETS[i].model, model)) {
        l->goneMask |= 1u << i;
      }
    }
  }
  if (l->attempts >= CHUTES_MAX_ATTEMPTS) {
    return GEM_STEP_FAIL;
  }
  // 1. The key; 2. the plan or balance. Every preset would say the same: no retry, no fallback.
  if (httpCode == 401 || httpCode == 403 || httpCode == 402) {
    return GEM_STEP_FAIL;
  }
  if (gone) {
    return ladderFallback(l, CHUTES_FB_GONE, 0, delayMs, stage);
  }
  // 4 + 5. Busy (429, every 5xx, a 200 carrying an error, or no text with a failed finish or none):
  //    once more inside the budget, then the fallback.
  if (httpCode == 429 || (httpCode >= 500 && httpCode <= 599) || errorIn200 || failed200) {
    if (!l->retried && (uint32_t)(nowMs - l->startMs) + CHUTES_RETRY_GAP_MS <= CHUTES_RETRY_BUDGET_MS) {
      l->retried = true;
      *delayMs = CHUTES_RETRY_GAP_MS;
      *stage = "Chutes is busy - trying again...";
      return GEM_STEP_RETRY;
    }
    return ladderFallback(l, CHUTES_FB_BUSY, GEM_LITE_DELAY_MS, delayMs, stage);
  }
  return GEM_STEP_FAIL;
}

size_t chutesAnswerLabel(const ChutesLadder* l, char* out, size_t cap) {
  ChW w = { out, cap, 0 };
  if (!l || !chutesPreset(l->cur)) {
    wStr(&w, "Chutes");
    return wEnd(&w);
  }
  if (!l->onFallback || !chutesPreset(l->fbFrom)) {
    wStr(&w, PRESETS[l->cur].label);
    return wEnd(&w);
  }
  wStr(&w, PRESETS[l->fbFrom].label);
  switch (l->fbWhy) {
  case CHUTES_FB_SLOW:    wStr(&w, " was slow - "); break;
  case CHUTES_FB_DROPPED: wStr(&w, " dropped out - "); break;
  case CHUTES_FB_GONE:    wStr(&w, " was not found - "); break;
  default:                wStr(&w, " was busy - "); break;
  }
  wStr(&w, PRESETS[l->cur].label);
  wStr(&w, " answered");
  return wEnd(&w);
}

size_t chutesFailText(int httpCode, int net, const ChutesReply* r, const ChutesLadder* l, const char* key,
                      char* out, size_t cap) {
  ChW w = { out, cap, 0 };
  const char* label = (l && chutesPreset(l->cur)) ? PRESETS[l->cur].label : "Chutes";
  char head[48];
  if (httpCode <= 0) {
    switch (net) {
    case GEM_NET_DNS:       wStr(&w, "No internet - Chutes' name did not resolve"); break;
    case GEM_NET_CONNECT:   wStr(&w, "Could not connect to Chutes - no internet?"); break;
    case GEM_NET_TLS:       wStr(&w, "Secure connection failed - Chutes' certificate did not verify"); break;
    case GEM_NET_TLS_OTHER: wStr(&w, "Secure connection failed"); break;
    case GEM_NET_NOMEM:     wStr(&w, "The phone is low on memory - try again, or reboot"); break;
    case GEM_NET_TIMEOUT:   wStr(&w, label); wStr(&w, " did not answer in time"); break;
    case GEM_NET_LOST:      wStr(&w, "The connection dropped before the answer came"); break;
    case GEM_NET_TOO_BIG:   wStr(&w, "The answer was too big to read"); break;
    case GEM_NET_BAD_BODY:  wStr(&w, "Could not read Chutes' answer"); break;
    default:                wStr(&w, "No network"); break;
    }
  } else if (httpCode == 401 || httpCode == 403) {
    wStr(&w, "Chutes refused the key - check chutes.txt");
  } else if (httpCode == 402) {
    /* 🛑 Never the body: it names a wallet to pay into. */
    wStr(&w, "Chutes: your plan's daily requests or balance are used up - check chutes.ai");
  } else if (httpCode == 404 || (httpCode == 400 && r && r->errModel)) {
    wStr(&w, label);
    wStr(&w, " is not offered by Chutes any more");
  } else if (httpCode == 429) {
    /* Chutes' anonymous path answers 429 to a request whose key it does not take: when nothing has
     * come from Chutes this boot but 429s, it is the key, not the load. EVERY attempt a 429 (review
     * 2026-10-18): a timeout or a drop among them was a request Chutes held, and a key it does not
     * know is answered at once - that is load, and "check the key" would send the owner to edit a
     * good one. */
    if (l && l->firstOfBoot && l->answers429 > 0 && l->answers429 == l->attempts) {
      wStr(&w, "Chutes said too many requests - check the key in chutes.txt");
    } else {
      wStr(&w, "Chutes is busy (too many requests) - try again in a minute");
    }
  } else if ((httpCode >= 500 && httpCode <= 599) || httpCode == 200) {
    wStr(&w, "Chutes is busy right now - try again in a minute");
  } else {
    snprintf(head, sizeof(head), "Chutes error %d", httpCode);
    wStr(&w, head);
  }
  const size_t n = wEnd(&w);
  if (out && n < cap) {
    geminiScrub(out, cap, key);                  // belt and braces: nothing above can hold it
    return strlen(out);
  }
  return n;
}

// ── a topic never spans the two providers ─────────────────────────────────────────────────

int chutesAnsweredBy(const char* label) {
  if (!label || !label[0]) {
    return CHUTES_BY_UNKNOWN;
  }
  /* chutesAnswerLabel's: a preset's label first (whole: "Kimi K2.6", not "Kimi"), alone or before
   * " was busy - ..." - or its word for no preset. Case and all, as it writes them. */
  if (!strcmp(label, "Chutes")) {
    return CHUTES_BY_CHUTES;
  }
  for (int i = 0; i < CHUTES_PRESETS; i++) {
    const size_t n = strlen(PRESETS[i].label);
    if (!strncmp(label, PRESETS[i].label, n) && (label[n] == '\0' || label[n] == ' ')) {
      return CHUTES_BY_CHUTES;
    }
  }
  /* geminiModelLabel's: a model name (geminiModelNameOk: [a-z0-9.-]) with letters capitalised. A
   * space, or anything else, is no label it writes. */
  for (const char* p = label; *p; p++) {
    const char c = *p;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) {
      return CHUTES_BY_UNKNOWN;
    }
  }
  return CHUTES_BY_GEMINI;
}

int chutesChoiceProvider(int choice) {
  return chutesPreset(choice) ? CHUTES_BY_CHUTES : CHUTES_BY_GEMINI;
}

bool chutesTopicDue(const GeminiChat* c, int64_t now, int choice, int last) {
  if (!c || geminiChatTopicDue(c, now)) {
    return true;
  }
  const int by = chutesChoiceProvider(choice);
  if (last != CHUTES_BY_UNKNOWN && last != by) {
    return true;                                 // this boot's last question went to the other one
  }
  /* What the question would be sent - each answered by its own provider, or a topic starts. */
  int idx[GEM_CONTEXT];
  const int n = geminiChatContext(c, idx);
  for (int i = 0; i < n; i++) {
    if (chutesAnsweredBy(c->ex[idx[i]].label) != by) {
      return true;
    }
  }
  return false;
}

// ── the screen's words ────────────────────────────────────────────────────────────────────

const char* chutesXferFound(int choice) {
  return chutesPreset(choice) ? "chutes.txt found - Back to use it." : "gemini.txt found - Back to use it.";
}

size_t chutesTopicNote(int choice, char* out, size_t cap) {
  ChW w = { out, cap, 0 };
  wStr(&w, "New topic: ");
  wStr(&w, chutesChoiceShort(choice));
  wStr(&w, " forgets the earlier questions (they stay on screen). A topic also starts by itself two "
           "hours after the last question");
  /* A preset's (the X4's review, 2026-10-19): a topic never spans the two providers. Gemini's stays
   * the words of before - a phone without chutes.txt has nothing to switch to. */
  wStr(&w, chutesPreset(choice) ? ", and on a change to Gemini." : ".");
  return wEnd(&w);
}

size_t chutesRetryNote(int choice, char* out, size_t cap) {
  ChW w = { out, cap, 0 };
  const int fb = chutesFallbackOf(choice, 0);
  if (!chutesPreset(choice) || fb < 0) {
    wStr(&w, GEM_RETRY_LABEL ": when Gemini sends nothing in time, ask once more, then Flash-Lite once.");
    return wEnd(&w);
  }
  wStr(&w, GEM_RETRY_LABEL ": when ");
  wStr(&w, PRESETS[choice].shortName);
  wStr(&w, " sends nothing in time, ask ");
  wStr(&w, PRESETS[fb].shortName);
  wStr(&w, " once.");
  return wEnd(&w);
}

const char* const CHUTES_KEYINFO_NOTES[] = {
  "Chutes (optional, paid) runs DeepSeek, Kimi and GLM. On a computer make an account at chutes.ai and "
  "create an API key (it starts cpk_).",
  "Make a text file named chutes.txt with one line: key=YOUR_KEY and put it in the API Keys folder, the "
  "same ways as gemini.txt.",
  "Optional lines: model=v4flash (or kimi, kimi_think, glm) until Menu > Model is set; lookup=off.",
  "Questions to a Chutes model go to Chutes (llm.chutes.ai), whose models run on rented GPUs, never "
  "to Google. The key stays on the card, is never shown and goes only to Chutes.",
  /* The X4's review (2026-10-19): true only since a topic never spans the two (chutesTopicDue). */
  "A topic never spans Gemini and Chutes: a change between them starts a new one, so neither is sent "
  "what the other was asked or answered.",
  NULL,
};
