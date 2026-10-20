/*
 * websearch.cpp - see websearch.h. Pure C++: no Arduino, no IDF, no allocation, no recursion. Every
 * buffer is the caller's; the reply parser is a byte-at-a-time state machine (any split of the body
 * gives the same result), so it can be fed from https_worker's tap as the bytes arrive and never
 * holds more than the fields it keeps. Small locals only (the loop task's 8 KB stack decides; the
 * block is built on the worker's after the request is over).
 */
#include "websearch.h"
#include "gemini.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// ── small helpers ─────────────────────────────────────────────────────────────────────────

/* gemini.cpp's writer rule: bytes go in while they fit, the count goes on regardless, and a text
 * that did not fit is left EMPTY - never half a body. */
struct WsW {
  char*  out;
  size_t cap;
  size_t n;
};

static void wPut(WsW* w, const char* s, size_t len) {
  if (w->out && w->n + len < w->cap) {
    memcpy(w->out + w->n, s, len);
  }
  w->n += len;
}

static void wStr(WsW* w, const char* s) {
  wPut(w, s, strlen(s));
}

static size_t wEnd(WsW* w) {
  if (w->out && w->cap) {
    if (w->n < w->cap) {
      w->out[w->n] = '\0';
    } else {
      w->out[0] = '\0';
    }
  }
  return w->n;
}

/* JSON-escaped (geminiJsonEscape's rules: " and \ escaped, control characters \u00XX, bytes >= 0x80
 * as they are). */
static void wEsc(WsW* w, const char* s) {
  for (const unsigned char* p = (const unsigned char*)s; p && *p; p++) {
    const unsigned char c = *p;
    if (c == '"') {
      wStr(w, "\\\"");
    } else if (c == '\\') {
      wStr(w, "\\\\");
    } else if (c < 0x20 || c == 0x7F) {
      char u[8];
      snprintf(u, sizeof(u), "\\u%04x", (unsigned)c);
      wStr(w, u);
    } else {
      wPut(w, (const char*)p, 1);
    }
  }
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

static size_t lineEnd(char* out, size_t cap, size_t n) {
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

static bool wsSpace(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// ── the key file ─────────────────────────────────────────────────────────────────────────

int webParseConfig(const char* text, size_t len, WebConfig* out) {
  memset(out, 0, sizeof(*out));
  if (!text) {
    len = 0;
  }
  /* THE READING IS GEMINI'S (chutesParseConfig's way): geminiReadKeyFile has the file rules - `key=`,
   * a lone bare key line, quotes, a BOM, CRLF, the last one wins, a space or an odd byte refused (so
   * whatever passes is header-safe) and the GEM_KEY_MAX cap. Every other line is ignored here.
   * ⚠ `g` (the caller's struct) holds the key for these lines: wiped below. */
  GeminiConfig* g = &out->scratch;
  const int rc = geminiReadKeyFile(text, len, g);
  int ret;
  if (rc == GEM_CFG_NO_KEY) {
    ret = WEB_CFG_NO_KEY;
  } else if (rc != GEM_CFG_OK) {
    out->problem = g->problem;                   // a literal: never the key
    ret = WEB_CFG_BAD_KEY;
  } else if (strncmp(g->key, WEB_KEY_PREFIX, sizeof(WEB_KEY_PREFIX) - 1) != 0) {
    /* Not a Tavily key - refused: it would go to Tavily in a Bearer header. A Chutes key is named
     * (the spec: a cpk_ key never lands in tavily.txt). */
    out->problem = strncmp(g->key, GEM_CHUTES_KEY_PREFIX, sizeof(GEM_CHUTES_KEY_PREFIX) - 1) == 0
                     ? "this is a Chutes key - put it in chutes.txt"
                     : "the key does not start tvly- - is it a Tavily key?";
    ret = WEB_CFG_BAD_KEY;
  } else if (g->keyLen <= (int)(sizeof(WEB_KEY_PREFIX) - 1)) {
    out->problem = "the key is only tvly-";
    ret = WEB_CFG_BAD_KEY;
  } else {
    memcpy(out->key, g->key, (size_t)g->keyLen + 1);
    out->keyLen = g->keyLen;
    bool shapeOk = g->keyLen >= 20 && g->keyLen <= 200;
    for (int i = 0; i < g->keyLen && shapeOk; i++) {
      const char c = g->key[i];
      if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) {
        shapeOk = false;
      }
    }
    if (!shapeOk) {
      out->warn |= WEB_WARN_SHAPE;
    }
    ret = WEB_CFG_OK;
  }
  geminiWipe(g, sizeof(*g));                     // the scratch is all zero outside the parse
  return ret;
}

size_t webConfigLine(const WebConfig* c, int parsed, char* out, size_t cap) {
  size_t n = 0;
  char num[48];
  if (parsed == WEB_CFG_OK) {
    snprintf(num, sizeof(num), "key: set (%d chars)", c->keyLen);
    lineAdd(out, cap, &n, num);
    if (c->warn & WEB_WARN_SHAPE) {
      lineAdd(out, cap, &n, "; the key's shape is unusual (sent anyway)");
    }
  } else if (parsed == WEB_CFG_BAD_KEY) {
    lineAdd(out, cap, &n, "key: refused - ");
    lineAdd(out, cap, &n, c->problem ? c->problem : "unreadable");
  } else {
    lineAdd(out, cap, &n, "key: none");
  }
  return lineEnd(out, cap, n);
}

size_t webAuthHeader(const char* key, char* out, size_t cap) {
  WsW w = { out, cap, 0 };
  if (!key || strncmp(key, WEB_KEY_PREFIX, sizeof(WEB_KEY_PREFIX) - 1) != 0 || !key[sizeof(WEB_KEY_PREFIX) - 1]) {
    return wEnd(&w);                             // "": nothing goes as a Bearer that is not a Tavily key
  }
  wStr(&w, WEB_AUTH_PREFIX);
  wStr(&w, key);
  return wEnd(&w);
}

// ── the setting ──────────────────────────────────────────────────────────────────────────

static const char* const MODE_IDS[WEB_MODES] = { "off", "auto", "always" };
static const char* const MODE_LABELS[WEB_MODES] = { "Off", "Auto", "Always" };

const char* webModeId(int mode) {
  return (mode >= 0 && mode < WEB_MODES) ? MODE_IDS[mode] : MODE_IDS[WEB_AUTO];
}

const char* webModeLabel(int mode) {
  return (mode >= 0 && mode < WEB_MODES) ? MODE_LABELS[mode] : MODE_LABELS[WEB_AUTO];
}

int webModeFind(const char* id) {
  if (!id) {
    return -1;
  }
  for (int i = 0; i < WEB_MODES; i++) {
    if (!strcasecmp(id, MODE_IDS[i])) {
      return i;
    }
  }
  return -1;
}

int webModeResolve(const char* stored) {
  const int m = webModeFind(stored);
  return m >= 0 ? m : WEB_AUTO;                  // never set (or a value this firmware does not know): Auto
}

int webModeNext(int mode) {
  return mode == WEB_OFF ? WEB_AUTO : (mode == WEB_AUTO ? WEB_ALWAYS : WEB_OFF);
}

size_t webModeRow(int mode, bool keyUsable, char* out, size_t cap) {
  WsW w = { out, cap, 0 };
  wStr(&w, "Web search: ");
  wStr(&w, keyUsable ? webModeLabel(mode) : WEB_FILE_NEEDS);
  return wEnd(&w);
}

// ── when to search ───────────────────────────────────────────────────────────────────────

const char WEB_TRIGGER_WORDS[] =
  "latest newest new current currently now today tonight yesterday tomorrow recent recently still update "
  "updated patch season meta tier best top price prices cost release released launch launched score scores "
  "news announced rumor rumour leak leaked trailer version";
/* The topic/time rule's two lists (the spec's): news + week, then general + month. */
static const char NEWS_WORDS[] = "today tonight yesterday news announced score scores";
static const char MONTH_WORDS[] = "latest newest current currently now meta patch best new release released launch "
                                  "launched update updated season tier version";

/* The question folded to ASCII one byte at a time (geminiFoldNext: the Wikipedia keywords' fold). */
struct WFold {
  const char* s;
  const char* pend;
  char        one[2];
};

static int foldGet(WFold* f) {
  for (;;) {
    if (f->pend && *f->pend) {
      return (unsigned char)*f->pend++;
    }
    if (!f->s) {
      return -1;
    }
    const char* r = geminiFoldNext(&f->s, f->one);
    if (!r) {
      return -1;
    }
    f->pend = r;
  }
}

static bool wordCh(int c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '\'';
}

/* The next word (wikiKeywords' rule): a run of [a-z0-9'] of the folded, lowercased text, apostrophes
 * off both ends, then ONE contraction ending off ('s 'm 're 'll 've 'd). *letter: it has a letter;
 * *tooLong: it passed `cap` (no trigger then). False at the end. */
static bool nextWord(WFold* f, char* w, size_t cap, bool* letter, bool* tooLong) {
  for (;;) {
    int c;
    while ((c = foldGet(f)) >= 0 && !wordCh(tolower(c))) {
    }
    if (c < 0) {
      return false;
    }
    size_t n = 0;
    bool over = false, anyLetter = false;
    do {
      c = tolower(c);
      if (!wordCh(c)) {
        break;
      }
      if (c >= 'a' && c <= 'z') {
        anyLetter = true;                        // the whole run's, kept or not
      }
      if (n + 1 < cap) {
        w[n++] = (char)c;
      } else {
        over = true;
      }
    } while ((c = foldGet(f)) >= 0);
    w[n] = '\0';
    size_t s = 0;
    while (s < n && w[s] == '\'') s++;
    while (n > s && w[n - 1] == '\'') n--;
    if (s) {
      memmove(w, w + s, n - s);
      n -= s;
    }
    w[n] = '\0';
    static const char* const ENDS[] = { "'s", "'m", "'re", "'ll", "'ve", "'d" };
    for (size_t k = 0; k < sizeof(ENDS) / sizeof(ENDS[0]); k++) {
      const size_t el = strlen(ENDS[k]);
      if (n > el && !strcmp(w + n - el, ENDS[k])) {
        n -= el;
        w[n] = '\0';
        break;
      }
    }
    if (n == 0 && !over) {
      continue;                                  // apostrophes alone: no word
    }
    bool l = false;
    for (size_t i = 0; i < n; i++) {
      if (w[i] >= 'a' && w[i] <= 'z') {
        l = true;
        break;
      }
    }
    *letter = over ? anyLetter : l;              // (a contraction's letters are off: "2's" is a number)
    *tooLong = over;
    return true;
  }
}

/* `w` is a whole token of the space-separated `list`. */
static bool inList(const char* w, const char* list) {
  const size_t wl = strlen(w);
  if (!wl) {
    return false;
  }
  for (const char* p = list; *p;) {
    while (*p == ' ') p++;
    const char* e = p;
    while (*e && *e != ' ') e++;
    if ((size_t)(e - p) == wl && !memcmp(p, w, wl)) {
      return true;
    }
    p = e;
  }
  return false;
}

static bool isYear(const char* w) {
  if (strlen(w) != 4) {
    return false;
  }
  int y = 0;
  for (int i = 0; i < 4; i++) {
    if (w[i] < '0' || w[i] > '9') {
      return false;
    }
    y = y * 10 + (w[i] - '0');
  }
  return y >= 2025 && y <= 2099;
}

int webLetterWords(const char* q) {
  WFold f = { q, NULL, { 0, 0 } };
  char w[WEB_WORD_MAX];
  bool letter = false, longW = false;
  int n = 0;
  while (q && nextWord(&f, w, sizeof(w), &letter, &longW)) {
    if (letter) {
      n++;
    }
  }
  return n;
}

bool webTriggered(const char* q, char* word, size_t wordCap) {
  if (word && wordCap) {
    word[0] = '\0';
  }
  WFold f = { q, NULL, { 0, 0 } };
  char w[WEB_WORD_MAX];
  bool letter = false, longW = false;
  while (q && nextWord(&f, w, sizeof(w), &letter, &longW)) {
    if (!longW && (inList(w, WEB_TRIGGER_WORDS) || isYear(w))) {
      if (word && wordCap) {
        snprintf(word, wordCap, "%s", w);
      }
      return true;
    }
  }
  return false;
}

void webTopicFor(const char* q, int* topic, int* timeRange) {
  bool news = false, month = false;
  WFold f = { q, NULL, { 0, 0 } };
  char w[WEB_WORD_MAX];
  bool letter = false, longW = false;
  while (q && nextWord(&f, w, sizeof(w), &letter, &longW)) {
    if (longW) {
      continue;
    }
    if (inList(w, NEWS_WORDS)) {
      news = true;
    } else if (inList(w, MONTH_WORDS)) {
      month = true;
    }
  }
  *topic = news ? WEB_TOPIC_NEWS : WEB_TOPIC_GENERAL;
  *timeRange = news ? WEB_TIME_WEEK : (month ? WEB_TIME_MONTH : WEB_TIME_NONE);
}

const char* webTopicId(int topic) {
  return topic == WEB_TOPIC_NEWS ? "news" : "general";
}

const char* webTimeId(int timeRange) {
  switch (timeRange) {
  case WEB_TIME_DAY:   return "day";
  case WEB_TIME_WEEK:  return "week";
  case WEB_TIME_MONTH: return "month";
  case WEB_TIME_YEAR:  return "year";
  default:             return NULL;
  }
}

static bool hasText(const char* q) {
  for (const unsigned char* p = (const unsigned char*)q; p && *p; p++) {
    if (*p > 0x20 && *p != 0x7F) {
      return true;
    }
  }
  return false;
}

void webDecide(int mode, bool keyUsable, bool forced, const char* question, int autoToday, WebDecision* d) {
  memset(d, 0, sizeof(*d));
  d->topic = WEB_TOPIC_GENERAL;
  d->timeRange = WEB_TIME_NONE;
  /* Off and no key win over everything, the per-question force included: Off keeps every question
   * away from Tavily (Key info's promise), and without a key there is nothing to search with. */
  if (mode != WEB_AUTO && mode != WEB_ALWAYS) {
    d->why = WEB_WHY_OFF;
    return;
  }
  if (!keyUsable) {
    d->why = WEB_WHY_NO_KEY;
    return;
  }
  if (!hasText(question)) {
    d->why = WEB_WHY_EMPTY;
    return;
  }
  if (forced || mode == WEB_ALWAYS) {
    d->search = true;
    d->why = forced ? WEB_WHY_FORCED : WEB_WHY_ALWAYS;
    webTopicFor(question, &d->topic, &d->timeRange);
    return;
  }
  // Auto
  if (webLetterWords(question) < 2) {
    d->why = WEB_WHY_FEW_WORDS;                  // "2+2", "5 km in miles" is 4 - but no trigger in it
    return;
  }
  if (!webTriggered(question, d->word, sizeof(d->word))) {
    d->why = WEB_WHY_NO_TRIGGER;
    return;
  }
  if (autoToday >= WEB_DAILY_AUTO) {
    d->why = WEB_WHY_CAP;
    return;
  }
  d->search = true;
  d->automatic = true;
  d->why = WEB_WHY_TRIGGER;
  webTopicFor(question, &d->topic, &d->timeRange);
}

size_t webWhyText(const WebDecision* d, char* out, size_t cap) {
  WsW w = { out, cap, 0 };
  switch (d ? d->why : WEB_WHY_OFF) {
  case WEB_WHY_OFF:        wStr(&w, "Off - nothing goes to Tavily"); break;
  case WEB_WHY_NO_KEY:     wStr(&w, "no usable tavily.txt - nothing is searched"); break;
  case WEB_WHY_EMPTY:      wStr(&w, "nothing to search for"); break;
  case WEB_WHY_FORCED:     wStr(&w, "asked for (" WEB_ASK_ROW ")"); break;
  case WEB_WHY_ALWAYS:     wStr(&w, "Always"); break;
  case WEB_WHY_TRIGGER:
    wStr(&w, isYear(d->word) ? "Auto: the question names " : "Auto: the question says \"");
    wStr(&w, d->word);
    if (!isYear(d->word)) {
      wStr(&w, "\"");
    }
    break;
  case WEB_WHY_NO_TRIGGER: wStr(&w, "Auto: no recent word or year in the question"); break;
  case WEB_WHY_FEW_WORDS:  wStr(&w, "Auto: fewer than 2 words (arithmetic, a conversion)"); break;
  case WEB_WHY_CAP:        wStr(&w, "Auto: today's " WEB_DAILY_AUTO_S " automatic searches are used"); break;
  default:                 wStr(&w, "?"); break;
  }
  return wEnd(&w);
}

// ── the day's counts ─────────────────────────────────────────────────────────────────────

int32_t webLocalDay(int64_t utc, int tzOffsetS) {
  if (utc <= 0) {
    return -1;                                   // not a trusted clock: no day
  }
  const int64_t local = utc + tzOffsetS;
  return local < 0 ? -1 : (int32_t)(local / 86400);
}

void webDayRoll(WebDay* d, int32_t today) {
  if (today >= 0 && today != d->day) {
    d->day = today;
    d->autoN = 0;
    d->otherN = 0;
  }
}

int webDayAutoUsed(const WebDay* d, int32_t today) {
  WebDay x = *d;
  webDayRoll(&x, today);
  return x.autoN;
}

void webDayCount(WebDay* d, int32_t today, bool automatic) {
  webDayRoll(d, today);
  int* n = automatic ? &d->autoN : &d->otherN;
  if (*n < 9999) {
    (*n)++;
  }
}

size_t webTodayNote(int autoN, int otherN, char* out, size_t cap) {
  char b[96];
  snprintf(b, sizeof(b), "Web searches today: %d of %d automatic, %d other.", autoN, WEB_DAILY_AUTO, otherN);
  WsW w = { out, cap, 0 };
  wStr(&w, b);
  return wEnd(&w);
}

// ── the request ──────────────────────────────────────────────────────────────────────────

size_t webQuery(const char* q, char* out, size_t cap) {
  /* Collapsed into b, one byte past the cap at most: that byte says whether the cut is at a word. */
  char b[WEB_QUERY_MAX + 2];
  size_t n = 0;
  bool space = false;
  for (const unsigned char* p = (const unsigned char*)q; p && *p && n < WEB_QUERY_MAX + 1; p++) {
    const unsigned char c = *p;
    if (c <= 0x20 || c == 0x7F) {
      space = n > 0;                             // a run of them is one space (none at the start)
      continue;
    }
    if (space) {
      b[n++] = ' ';
      space = false;
      if (n >= WEB_QUERY_MAX + 1) {
        break;
      }
    }
    b[n++] = (char)c;
  }
  size_t len = n;
  if (n > WEB_QUERY_MAX) {
    if (b[WEB_QUERY_MAX] == ' ') {
      len = WEB_QUERY_MAX;                       // the 200th byte ends a word
    } else {
      len = 0;
      for (size_t i = WEB_QUERY_MAX; i > 0; i--) {
        if (b[i - 1] == ' ') {
          len = i - 1;                           // the last whole word that fits
          break;
        }
      }
      if (len == 0) {
        len = WEB_QUERY_MAX;                     // one long word: at a character
        while (len > 0 && ((unsigned char)b[len] & 0xC0) == 0x80) {
          len--;
        }
      }
    }
  }
  WsW w = { out, cap, 0 };
  wPut(&w, b, len);
  return wEnd(&w);
}

size_t webBuildRequest(const char* question, int topic, int timeRange, char* out, size_t cap) {
  char qb[WEB_QUERY_MAX + 1];
  webQuery(question, qb, sizeof(qb));
  WsW w = { out, cap, 0 };
  wStr(&w, "{\"query\":\"");
  wEsc(&w, qb);
  /* 🛑 basic, auto_parameters false, no raw content: anything else costs 2 credits or whole pages. */
  wStr(&w, "\",\"search_depth\":\"basic\",\"auto_parameters\":false,\"topic\":\"");
  wStr(&w, webTopicId(topic));
  wStr(&w, "\"");
  const char* t = webTimeId(timeRange);
  if (t) {
    wStr(&w, ",\"time_range\":\"");
    wStr(&w, t);
    wStr(&w, "\"");
  }
  char num[96];
  snprintf(num, sizeof(num), ",\"max_results\":%d,\"chunks_per_source\":1,", WEB_ASK_RESULTS);
  wStr(&w, num);
  /* include_answer "basic" (tweak 1, measured: a ~450-character synthesis, 1 credit as before). */
  wStr(&w, "\"include_answer\":\"basic\",\"include_raw_content\":false}");
  return wEnd(&w);
}

// ── the reply: a streaming parser ─────────────────────────────────────────────────────────

/* What a value (or a container) is, by where it sits. */
enum {
  R_SKIP = 0, R_TOP, R_ROOT, R_RESULTS, R_RESULT, R_USAGE, R_DETAIL,
  R_TITLE, R_URL, R_CONTENT, R_SCORE, R_DATE, R_RTIME, R_CREDITS, R_ANSWER,
};
enum {
  S_VALUE = 0, S_VALUE_OR_END, S_KEY_OR_END, S_KEY, S_COLON, S_AFTER, S_STR, S_ESC, S_U, S_U_BS, S_U_U,
  S_NUM, S_LIT, S_DONE,
};

void webParseInit(WebParse* p) {
  memset(p, 0, sizeof(*p));
  p->st = S_VALUE;
  p->valRole = R_TOP;
  p->responseMs = -1;
  p->credits = -1;
}

static WebResult* curResult(WebParse* p) {
  return &p->r[WEB_HOLD];
}

/* A JSON number's text (-?int frac? exp?) to a double, no locale, no libm. False when it is not one. */
static bool numOf(const char* s, double* v) {
  const char* p = s;
  bool neg = false;
  if (*p == '-') {
    neg = true;
    p++;
  }
  if (!isdigit((unsigned char)*p)) {
    return false;
  }
  double x = 0;
  while (isdigit((unsigned char)*p)) {
    x = x * 10 + (*p++ - '0');
  }
  if (*p == '.') {
    p++;
    if (!isdigit((unsigned char)*p)) {
      return false;
    }
    double f = 0.1;
    while (isdigit((unsigned char)*p)) {
      x += (*p++ - '0') * f;
      f /= 10;
    }
  }
  if (*p == 'e' || *p == 'E') {
    p++;
    bool en = false;
    if (*p == '+' || *p == '-') {
      en = *p == '-';
      p++;
    }
    if (!isdigit((unsigned char)*p)) {
      return false;
    }
    int e = 0;
    while (isdigit((unsigned char)*p)) {
      if (e < 400) {
        e = e * 10 + (*p - '0');
      }
      p++;
    }
    for (int i = 0; i < e && x != 0; i++) {
      x = en ? x / 10 : x * 10;
      if (x > 1e300) {
        break;
      }
    }
  }
  if (*p) {
    return false;
  }
  *v = neg ? -x : x;
  return true;
}

static void numDone(WebParse* p, uint8_t role) {
  p->num[p->nlen] = '\0';
  double v = 0;
  if (!numOf(p->num, &v)) {
    return;                                      // not a number: nothing kept
  }
  if (role == R_SCORE) {
    curResult(p)->score = v;
  } else if (role == R_RTIME && v >= 0 && v < 1e6) {
    p->responseMs = (long)(v * 1000 + 0.5);
  } else if (role == R_CREDITS && v >= 0 && v < 1e6) {
    p->credits = (int)(v + 0.5);
  }
}

/* The limit-word scan of an error's text, a word at a time - its first letters and its length, never
 * the text: "limit..." / "credit..." (limits, credits), or exactly "plan" / "plans" ("plan's" too;
 * never "planet" or "explanation"). */
static void detailWordEnd(WebParse* p) {
  if (p->wlen && (!strncmp(p->word, "limit", 5) || !strncmp(p->word, "credit", 6) ||
                  (p->wlen == 4 && !strcmp(p->word, "plan")) || (p->wlen == 5 && !strcmp(p->word, "plans")))) {
    p->limitWord = true;
  }
  p->wlen = 0;
  p->word[0] = '\0';
}

static void detailByte(WebParse* p, unsigned char c) {
  const unsigned char l = (unsigned char)tolower(c);
  if (c < 0x80 && l >= 'a' && l <= 'z') {
    if (p->wlen < sizeof(p->word) - 1) {
      p->word[p->wlen] = (char)l;
      p->word[p->wlen + 1] = '\0';
    }
    if (p->wlen < 255) {
      p->wlen++;
    }
  } else {
    detailWordEnd(p);
  }
}

/* One byte of a string's DECODED text. Into its buffer whole characters only: a lead byte goes in
 * only with room for its whole sequence, and once a string is cut, the rest of it is skipped. */
static void strByte(WebParse* p, unsigned char c) {
  if (p->isKey) {
    if (p->klen + 1 < sizeof(p->key)) {
      p->key[p->klen++] = (char)c;
      p->key[p->klen] = '\0';
    } else {
      p->kcut = true;
    }
    return;
  }
  if (p->valRole == R_DETAIL) {
    detailByte(p, c);
    return;
  }
  if (p->numStr) {
    if (p->nlen + 1 < sizeof(p->num)) {
      p->num[p->nlen++] = (char)c;
    } else {
      p->num[0] = '?';                           // too long to be a number: none (numOf refuses it)
    }
    return;
  }
  if (!p->sbuf || p->strCut) {
    return;
  }
  size_t need = 1;
  if (c >= 0xF0) {
    need = 4;
  } else if (c >= 0xE0) {
    need = 3;
  } else if (c >= 0xC0) {
    need = 2;
  }
  if (p->slen + need >= p->scap) {
    p->strCut = true;
    /* The flag is the FIELD's: only a cut content (or answer) is an excerpt that goes on - a URL, a
     * title or a date that overflows says nothing about it (review 2026-10-19). */
    if (p->sbuf == curResult(p)->content) {
      curResult(p)->cut = true;
    } else if (p->sbuf == p->answer) {
      p->answerCut = true;
    }
    return;
  }
  p->sbuf[p->slen++] = (char)c;
  p->sbuf[p->slen] = '\0';
}

static void cpOut(WebParse* p, uint32_t cp) {
  if (cp == 0) {
    return;                                      // \u0000: dropped (json_read.h's rule)
  }
  if (cp < 0x80) {
    strByte(p, (unsigned char)cp);
  } else if (cp < 0x800) {
    strByte(p, (unsigned char)(0xC0 | (cp >> 6)));
    strByte(p, (unsigned char)(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    strByte(p, (unsigned char)(0xE0 | (cp >> 12)));
    strByte(p, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)));
    strByte(p, (unsigned char)(0x80 | (cp & 0x3F)));
  } else {
    strByte(p, (unsigned char)(0xF0 | (cp >> 18)));
    strByte(p, (unsigned char)(0x80 | ((cp >> 12) & 0x3F)));
    strByte(p, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)));
    strByte(p, (unsigned char)(0x80 | (cp & 0x3F)));
  }
}

static void unitDone(WebParse* p, int u) {
  if (p->uHigh) {
    if (u >= 0xDC00 && u <= 0xDFFF) {
      cpOut(p, 0x10000u + ((uint32_t)(p->uHigh - 0xD800) << 10) + (uint32_t)(u - 0xDC00));
      p->uHigh = 0;
      p->st = S_STR;
      return;
    }
    cpOut(p, 0xFFFD);                            // a high surrogate alone
    p->uHigh = 0;
  }
  if (u >= 0xD800 && u <= 0xDBFF) {
    p->uHigh = u;
    p->st = S_U_BS;                              // its low half should follow: \uDCxx
    return;
  }
  cpOut(p, (u >= 0xDC00 && u <= 0xDFFF) ? 0xFFFD : (uint32_t)u);
  p->st = S_STR;
}

static void push(WebParse* p, char type, uint8_t role) {
  if (p->depth >= WEB_JS_DEPTH) {
    p->bad = true;                               // deeper than any reply this reads
    return;
  }
  p->ctype[p->depth] = (uint8_t)type;
  p->role[p->depth] = role;
  p->depth++;
}

/* The role of an array's elements, by the array's. */
static uint8_t elemRole(uint8_t arr) {
  return arr == R_RESULTS ? R_RESULT : (arr == R_DETAIL ? R_DETAIL : R_SKIP);
}

/* A held result goes in by score, best first; one that is not better than the last held is dropped.
 * Arrival order between equal scores. Only a result with something in its content. */
static void resultDone(WebParse* p) {
  p->seen++;
  WebResult* c = curResult(p);
  bool any = false;
  for (const unsigned char* q = (const unsigned char*)c->content; *q && !any; q++) {
    any = isalnum(*q) || *q >= 0x80;
  }
  if (!any) {
    return;
  }
  int at = p->n;
  while (at > 0 && p->r[at - 1].score < c->score) {
    at--;
  }
  if (at >= WEB_HOLD) {
    return;                                      // no better than any held
  }
  const int last = p->n < WEB_HOLD ? p->n : WEB_HOLD - 1;
  if (last > at) {
    memmove(&p->r[at + 1], &p->r[at], (size_t)(last - at) * sizeof(WebResult));
  }
  memcpy(&p->r[at], c, sizeof(WebResult));
  if (p->n < WEB_HOLD) {
    p->n++;
  }
}

static void closeContainer(WebParse* p) {
  if (p->depth <= 0) {
    p->bad = true;
    return;
  }
  const uint8_t role = p->role[p->depth - 1];
  if (role == R_RESULT && p->ctype[p->depth - 1] == '{') {
    resultDone(p);
  }
  p->depth--;
  if (p->depth == 0) {
    p->st = S_DONE;
    p->done = true;
  } else {
    p->st = S_AFTER;
  }
}

/* The key just read sets the role of its value. */
static void keyDone(WebParse* p) {
  const uint8_t in = p->role[p->depth - 1];
  const char* k = p->kcut ? "" : p->key;
  uint8_t r = R_SKIP;
  if (in == R_ROOT) {
    if (!strcmp(k, "results")) {
      r = R_RESULTS;
    } else if (!strcmp(k, "answer")) {
      r = R_ANSWER;                              // include_answer "basic": the summary line
    } else if (!strcmp(k, "response_time")) {
      r = R_RTIME;
    } else if (!strcmp(k, "usage")) {
      r = R_USAGE;
    } else if (!strcmp(k, "detail") || !strcmp(k, "error")) {
      r = R_DETAIL;
      p->hasError = true;
    }
  } else if (in == R_RESULT) {
    if (!strcmp(k, "title")) {
      r = R_TITLE;
    } else if (!strcmp(k, "url")) {
      r = R_URL;
    } else if (!strcmp(k, "content")) {
      r = R_CONTENT;
    } else if (!strcmp(k, "score")) {
      r = R_SCORE;
    } else if (!strcmp(k, "published_date")) {
      r = R_DATE;
    }
  } else if (in == R_USAGE) {
    if (!strcmp(k, "credits")) {
      r = R_CREDITS;
    }
  } else if (in == R_DETAIL) {
    r = R_DETAIL;
  }
  p->valRole = r;
  p->st = S_COLON;
}

static void beginValue(WebParse* p, unsigned char c) {
  const uint8_t role = p->valRole;
  if (c == '{') {
    uint8_t cr = R_SKIP;
    if (role == R_TOP) {
      cr = R_ROOT;
      p->rootObj = true;
    } else if (role == R_RESULT) {
      cr = R_RESULT;
      memset(curResult(p), 0, sizeof(WebResult));
      curResult(p)->score = -1;                  // none given: it ranks last
    } else if (role == R_USAGE || role == R_DETAIL) {
      cr = role;
    }
    push(p, '{', cr);
    p->st = S_KEY_OR_END;
  } else if (c == '[') {
    const uint8_t ar = (role == R_RESULTS || role == R_DETAIL) ? role : R_SKIP;
    if (role == R_RESULTS) {
      p->hasResults = true;
    }
    push(p, '[', ar);
    p->valRole = elemRole(ar);
    p->st = S_VALUE_OR_END;
  } else if (c == '"') {
    p->isKey = false;
    p->sbuf = NULL;
    p->scap = 0;
    p->slen = 0;
    p->strCut = false;
    p->numStr = false;
    p->nlen = 0;
    p->wlen = 0;
    WebResult* r = curResult(p);
    switch (role) {
    case R_TITLE:   p->sbuf = r->title;   p->scap = sizeof(r->title);   break;
    case R_URL:     p->sbuf = r->url;     p->scap = sizeof(r->url);     break;
    case R_CONTENT: p->sbuf = r->content; p->scap = sizeof(r->content); break;
    case R_DATE:    p->sbuf = r->date;    p->scap = sizeof(r->date);    break;
    case R_ANSWER:  p->sbuf = p->answer;  p->scap = sizeof(p->answer);  break;
    case R_RTIME:
    case R_SCORE:
    case R_CREDITS: p->numStr = true;                                   break;
    default: break;
    }
    if (p->sbuf) {
      p->sbuf[0] = '\0';                         // a second "title" replaces the first
      if (role == R_CONTENT) {
        r->cut = false;                          // ...and its cut flag with it
      } else if (role == R_ANSWER) {
        p->answerCut = false;
      }
    }
    p->st = S_STR;
  } else if (c == '-' || (c >= '0' && c <= '9')) {
    p->numStr = false;
    p->nlen = 0;
    p->num[p->nlen++] = (char)c;
    p->st = S_NUM;
  } else if (c == 't' || c == 'f' || c == 'n') {
    p->lit = c == 't' ? "true" : (c == 'f' ? "false" : "null");
    p->litPos = 1;
    p->st = S_LIT;
  } else {
    p->bad = true;
  }
}

static void beginKey(WebParse* p) {
  p->isKey = true;
  p->klen = 0;
  p->key[0] = '\0';
  p->kcut = false;
  p->st = S_STR;
}

static void endString(WebParse* p) {
  if (p->isKey) {
    p->isKey = false;
    keyDone(p);
    return;
  }
  if (p->numStr) {
    numDone(p, p->valRole);                      // "response_time":"1.67"
    p->numStr = false;
  }
  if (p->valRole == R_DETAIL) {
    detailWordEnd(p);                            // the text's last word
  }
  p->sbuf = NULL;
  p->st = S_AFTER;
}

static bool hexOf(unsigned char c, int* v) {
  if (c >= '0' && c <= '9') {
    *v = c - '0';
  } else if (c >= 'a' && c <= 'f') {
    *v = c - 'a' + 10;
  } else if (c >= 'A' && c <= 'F') {
    *v = c - 'A' + 10;
  } else {
    return false;
  }
  return true;
}

/* One byte through the machine. False = the byte was not consumed (look at it again). */
static bool step(WebParse* p, unsigned char c) {
  switch (p->st) {
  case S_DONE:
    if (!wsSpace(c)) {
      p->bad = true;                             // something after the reply's one value
    }
    return true;
  case S_VALUE:
    if (!wsSpace(c)) {
      beginValue(p, c);
    }
    return true;
  case S_VALUE_OR_END:
    if (wsSpace(c)) {
      return true;
    }
    if (c == ']') {
      closeContainer(p);
    } else {
      beginValue(p, c);
    }
    return true;
  case S_KEY_OR_END:
    if (wsSpace(c)) {
      return true;
    }
    if (c == '}') {
      closeContainer(p);
    } else if (c == '"') {
      beginKey(p);
    } else {
      p->bad = true;
    }
    return true;
  case S_KEY:
    if (wsSpace(c)) {
      return true;
    }
    if (c == '"') {
      beginKey(p);
    } else {
      p->bad = true;
    }
    return true;
  case S_COLON:
    if (wsSpace(c)) {
      return true;
    }
    if (c == ':') {
      p->st = S_VALUE;
    } else {
      p->bad = true;
    }
    return true;
  case S_AFTER: {
    if (wsSpace(c)) {
      return true;
    }
    const char type = p->depth > 0 ? (char)p->ctype[p->depth - 1] : 0;
    if (c == ',' && type == '{') {
      p->st = S_KEY;
    } else if (c == ',' && type == '[') {
      p->valRole = elemRole(p->role[p->depth - 1]);
      p->st = S_VALUE;
    } else if ((c == '}' && type == '{') || (c == ']' && type == '[')) {
      closeContainer(p);
    } else {
      p->bad = true;
    }
    return true;
  }
  case S_STR:
    if (c == '"') {
      endString(p);
    } else if (c == '\\') {
      p->st = S_ESC;
    } else if (c < 0x20) {
      p->bad = true;                             // a raw control character: not JSON
    } else {
      strByte(p, c);
    }
    return true;
  case S_ESC:
    p->st = S_STR;
    switch (c) {
    case '"': case '\\': case '/': strByte(p, c); break;
    case 'b': strByte(p, '\b'); break;
    case 'f': strByte(p, '\f'); break;
    case 'n': strByte(p, '\n'); break;
    case 'r': strByte(p, '\r'); break;
    case 't': strByte(p, '\t'); break;
    case 'u': p->st = S_U; p->uDigits = 0; p->uPending = 0; break;
    default: p->bad = true; break;
    }
    return true;
  case S_U: {
    int v = 0;
    if (!hexOf(c, &v)) {
      p->bad = true;
      return true;
    }
    p->uPending = (p->uPending << 4) | v;
    if (++p->uDigits == 4) {
      unitDone(p, p->uPending);
    }
    return true;
  }
  case S_U_BS:
    if (c == '\\') {
      p->st = S_U_U;
      return true;
    }
    cpOut(p, 0xFFFD);                            // a high surrogate with no low half after it
    p->uHigh = 0;
    p->st = S_STR;
    return false;
  case S_U_U:
    if (c == 'u') {
      p->st = S_U;
      p->uDigits = 0;
      p->uPending = 0;
      return true;
    }
    cpOut(p, 0xFFFD);
    p->uHigh = 0;
    p->st = S_ESC;                               // the '\\' read was another escape's
    return false;
  case S_NUM:
    if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
      if (p->nlen + 1 < sizeof(p->num)) {
        p->num[p->nlen++] = (char)c;
      } else {
        p->num[0] = '?';                         // too long to be one this keeps: numOf refuses it
      }
      return true;
    }
    numDone(p, p->valRole);
    p->st = S_AFTER;
    return false;
  case S_LIT:
    if (p->lit && c == (unsigned char)p->lit[p->litPos]) {
      if (!p->lit[++p->litPos]) {
        p->st = S_AFTER;
      }
    } else {
      p->bad = true;
    }
    return true;
  default:
    p->bad = true;
    return true;
  }
}

void webParseFeed(WebParse* p, const char* data, size_t n) {
  for (size_t i = 0; i < n && !p->bad;) {
    if (step(p, (unsigned char)data[i])) {
      i++;
    }
  }
}

int webParseFinish(WebParse* p) {
  if (!p->bad && p->st == S_NUM && p->depth == 0) {
    p->bad = true;                               // a bare number is no reply
  }
  if (p->bad || !p->done || !p->rootObj) {
    p->n = 0;                                    // a cut or broken reply: nothing from it goes
    p->answer[0] = '\0';
    return WEB_PARSE_BAD;
  }
  if (p->hasError) {
    p->n = 0;
    p->answer[0] = '\0';
    return WEB_PARSE_ERROR;
  }
  if (!p->hasResults) {
    p->answer[0] = '\0';
    return WEB_PARSE_BAD;
  }
  bool said = false;                             // an answer with something in it (tweak 1)
  for (const unsigned char* q = (const unsigned char*)p->answer; *q && !said; q++) {
    said = isalnum(*q) || *q >= 0x80;
  }
  return (p->n > 0 || said) ? WEB_PARSE_OK : WEB_PARSE_NONE;
}

// ── the block and the sources line ───────────────────────────────────────────────────────

static int digits(const char* s, int n) {
  int v = 0;
  for (int i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') {
      return -1;
    }
    v = v * 10 + (s[i] - '0');
  }
  return v;
}

static bool dateOut(int y, int m, int d, char* out, size_t cap) {
  if (y < 1990 || y > 2199 || m < 1 || m > 12 || d < 1 || d > 31 || !out || cap < 11) {
    return false;
  }
  snprintf(out, cap, "%04d-%02d-%02d", y, m, d);
  return true;
}

bool webDateOf(const char* s, char* out, size_t cap) {
  if (out && cap) {
    out[0] = '\0';
  }
  if (!s) {
    return false;
  }
  while (*s == ' ') s++;
  // ISO: 2026-10-14[T...]
  if (strlen(s) >= 10 && s[4] == '-' && s[7] == '-') {
    const int y = digits(s, 4), m = digits(s + 5, 2), d = digits(s + 8, 2);
    if (y >= 0 && m >= 0 && d >= 0 && (s[10] == '\0' || s[10] == 'T' || s[10] == ' ')) {
      return dateOut(y, m, d, out, cap);
    }
    return false;
  }
  // RFC 1123: [Tue, ]14 Oct 2026 17:41:23 GMT
  const char* p = s;
  const char* comma = strchr(p, ',');
  if (comma && comma - p <= 9) {
    p = comma + 1;
    while (*p == ' ') p++;
  }
  int d = 0, nd = 0;
  while (nd < 2 && p[nd] >= '0' && p[nd] <= '9') {
    d = d * 10 + (p[nd] - '0');
    nd++;
  }
  if (nd == 0 || p[nd] != ' ') {
    return false;
  }
  p += nd + 1;
  static const char* const MON[] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };
  int m = 0;
  for (int i = 0; i < 12; i++) {
    if (!strncasecmp(p, MON[i], 3)) {
      m = i + 1;
      break;
    }
  }
  if (!m || p[3] != ' ') {
    return false;
  }
  const int y = digits(p + 4, 4);
  if (y < 0 || (p[8] >= '0' && p[8] <= '9')) {
    return false;
  }
  return dateOut(y, m, d, out, cap);
}

bool webHostOf(const char* url, char* out, size_t cap) {
  if (out && cap) {
    out[0] = '\0';
  }
  if (!url || !out || cap < 2) {
    return false;
  }
  const char* p = strstr(url, "://");
  p = p ? p + 3 : url;
  const char* e = p;
  while (*e && *e != '/' && *e != '?' && *e != '#') e++;
  const char* at = NULL;
  for (const char* q = p; q < e; q++) {
    if (*q == '@') at = q;                       // user:pass@host - only the host
  }
  if (at) {
    p = at + 1;
  }
  const char* colon = (const char*)memchr(p, ':', (size_t)(e - p));
  if (colon) {
    e = colon;                                   // :443
  }
  if (e - p >= 4 && !strncasecmp(p, "www.", 4)) {
    p += 4;
  }
  size_t n = 0;
  bool dot = false;
  for (const char* q = p; q < e; q++) {
    const char c = (char)tolower((unsigned char)*q);
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) {
      out[0] = '\0';
      return false;                              // an IDN or junk: nothing a line may show
    }
    if (c == '.') dot = true;
    if (n + 1 >= cap) {
      out[0] = '\0';
      return false;
    }
    out[n++] = c;
  }
  out[n] = '\0';
  if (n == 0 || !dot || out[0] == '.' || out[n - 1] == '.') {
    out[0] = '\0';
    return false;
  }
  return true;
}

/* Days since 1970-01-01 to a civil date (Howard Hinnant's civil_from_days, gemini.cpp's). */
static void civil(int64_t z, int* y, int* m, int* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = (int64_t)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = (int)(yy + (*m <= 2));
}

bool webDateLocal(int64_t utc, int tzOffsetS, char* out, size_t cap) {
  if (out && cap) {
    out[0] = '\0';
  }
  const int32_t day = webLocalDay(utc, tzOffsetS);
  if (day < 0) {
    return false;
  }
  int y, m, d;
  civil(day, &y, &m, &d);
  return dateOut(y, m, d, out, cap);
}

/* A raw (UTF-8) text cleaned for the block, into out (< cap): folded to ASCII (geminiFoldNext, the
 * Wikipedia text's fold), every control character and whitespace a space, "[...]" (Tavily's chunk
 * separator; "[…]" folds to it) taken out, runs of spaces one, both ends trimmed. *more: there was
 * more text than fitted. Returns the length. */
struct WClean {
  char*  out;
  size_t cap, n;
  bool   space, more;
};

/* One folded byte of text: whitespace and control characters a pending space (none at the start),
 * a byte past the room `more`. */
static void cleanPut(WClean* k, int c) {
  if (c <= 0x20 || c == 0x7F) {
    k->space = k->n > 0;
    return;
  }
  if (k->space) {
    k->space = false;
    if (k->n + 1 < k->cap) {
      k->out[k->n++] = ' ';
    } else {
      k->more = true;
    }
  }
  if (k->n + 1 < k->cap) {
    k->out[k->n++] = (char)c;
  } else {
    k->more = true;
  }
}

static size_t cleanText(const char* in, char* out, size_t cap, bool* more) {
  static const char SEP[] = "[...]";
  WClean k = { out, cap, 0, false, false };
  char held[sizeof(SEP)];
  size_t hl = 0;                                 // bytes of a possible separator held back
  WFold f = { in, NULL, { 0, 0 } };
  int c = foldGet(&f);
  while (c >= 0 || hl) {
    if (c >= 0 && c == (unsigned char)SEP[hl]) {
      held[hl++] = (char)c;
      if (hl == sizeof(SEP) - 1) {
        hl = 0;                                  // "[...]": out, a space in its place
        k.space = k.n > 0;
      }
      c = foldGet(&f);
      continue;
    }
    if (hl) {
      /* Not the separator after all: what was held is text, and c is looked at again - it may start
       * a separator of its own ("[[...]"). */
      for (size_t i = 0; i < hl; i++) {
        cleanPut(&k, held[i]);
      }
      hl = 0;
      continue;
    }
    cleanPut(&k, c);
    c = foldGet(&f);
  }
  size_t n = k.n;
  while (n > 0 && out[n - 1] == ' ') n--;
  out[n] = '\0';
  *more = k.more;
  return n;
}

/* `host` is already one of the sources line's names ("Web: a.com; b.com"). */
static bool srcHas(const char* src, const char* host) {
  const char* p = src + sizeof(GEM_WEB_PREFIX) - 1;
  const size_t hl = strlen(host);
  while (*p) {
    const char* e = strstr(p, "; ");
    const size_t l = e ? (size_t)(e - p) : strlen(p);
    if (l == hl && !memcmp(p, host, hl)) {
      return true;
    }
    if (!e) {
      break;
    }
    p = e + 2;
  }
  return false;
}

/* Cut a clean ASCII text to `max` bytes at a word: with "..." when cut and `dots` (a full stop
 * before them is dropped: "...", never "...."). In place. */
static size_t cutAtWord(char* s, size_t len, size_t max, bool more, bool dots) {
  if (len <= max && !more) {
    return len;
  }
  const size_t room = dots ? (max > 3 ? max - 3 : 0) : max;
  size_t cut = len < room ? len : room;
  if (cut < len) {
    size_t sp = cut;
    while (sp > 0 && s[sp] != ' ') sp--;
    if (sp > room / 2) {
      cut = sp;                                  // at the last space, when it keeps half or more
    }
  }
  while (cut > 0 && (s[cut - 1] == ' ' || s[cut - 1] == ',' || s[cut - 1] == ';' || (dots && s[cut - 1] == '.'))) {
    cut--;
  }
  if (dots && room > 0) {
    memcpy(s + cut, "...", 4);
    return cut + 3;
  }
  s[cut] = '\0';
  return cut;
}

/* The summary cut to `max` bytes (wiki_lookup.cpp's sentenceEnd rule, COVEY's cut_at_sentence): whole
 * when it fits; else after the last sentence end (". " "! " "? ", or its end) in the first `max`, when
 * there is one past a third of it; else at a word with "..." (cutAtWord). Clean ASCII; in place. */
static size_t cutAtSentence(char* s, size_t len, size_t max, bool more) {
  if (!more && len <= max) {
    return len;
  }
  for (size_t i = (max < len ? max : len); i > max / 3; i--) {
    const char c = s[i - 1];
    if ((c == '.' || c == '!' || c == '?') && (i == len ? !more : s[i] == ' ')) {
      s[i] = '\0';
      return i;
    }
  }
  return cutAtWord(s, len, max, true, true);
}

/* A line (the summary, or the last result's content) goes only with at least this many of its bytes. */
static const size_t LINE_MIN = 60;

size_t webBuildBlock(const WebParse* p, const char* searched, size_t maxBytes, char* out, size_t cap, char* src,
                     size_t srcCap, int* used) {
  if (used) {
    *used = 0;
  }
  if (src && srcCap) {
    src[0] = '\0';
  }
  if (!out || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  const size_t limit = maxBytes < cap - 1 ? maxBytes : cap - 1;
  size_t n = 0;
  // the header and the framing line
  {
    WsW w = { out, limit + 1, 0 };
    wStr(&w, WEB_BLOCK_HEAD);
    wStr(&w, (searched && searched[0]) ? searched : "just now");
    wStr(&w, ":\n" WEB_BLOCK_FRAMING);
    n = wEnd(&w);
    if (n > limit) {
      out[0] = '\0';
      return 0;
    }
  }
  /* Tweak 1: Tavily's answer, "Summary: <at most 400 bytes, cut at a sentence end>", before the results
   * (cleaned as they are: ASCII-folded, "[...]" out, whitespace collapsed). Less room than LINE_MIN: none. */
  bool summary = false;
  if (p && p->answer[0]) {
    char sum[WEB_SUMMARY_CUT + 8];
    bool smore = false;
    size_t sl = cleanText(p->answer, sum, sizeof(sum), &smore);
    const size_t hl = 1 + sizeof(WEB_SUMMARY_HEAD) - 1;   // "\nSummary: "
    size_t want = WEB_SUMMARY_CUT;
    if (n + hl + want > limit) {
      want = limit > n + hl ? limit - n - hl : 0;
    }
    if (sl > 0 && want >= LINE_MIN) {
      sl = cutAtSentence(sum, sl, want, smore || p->answerCut);
      memcpy(out + n, "\n" WEB_SUMMARY_HEAD, hl);
      memcpy(out + n + hl, sum, sl);
      n += hl + sl;
      out[n] = '\0';
      summary = true;
    }
  }
  size_t sn = 0;                                // the sources line's length
  int lines = 0;
  char content[WEB_CONTENT_CUT + 8];
  char title[WEB_TITLE_CUT + 8];
  char date[16];
  char host[64];
  for (int i = 0; p && i < p->n && lines < WEB_KEEP_RESULTS; i++) {
    const WebResult* r = &p->r[i];
    if (!(r->score >= WEB_MIN_SCORE)) {
      continue;                                  // tweak 2: scored under 0.30, or not at all - off-topic
    }
    bool more = false;
    size_t cl = cleanText(r->content, content, sizeof(content), &more);
    if (cl == 0) {
      continue;                                  // no content left once cleaned: not a line
    }
    cl = cutAtWord(content, cl, WEB_CONTENT_CUT, more || r->cut, true);
    bool tmore = false;
    size_t tl = cleanText(r->title, title, sizeof(title), &tmore);
    tl = cutAtWord(title, tl, WEB_TITLE_CUT, tmore, false);
    if (!webDateOf(r->date, date, sizeof(date))) {
      snprintf(date, sizeof(date), "undated");
    }
    const bool haveHost = webHostOf(r->url, host, sizeof(host));
    // "\n- <date> <host>: <title>. <content>"
    char pre[160];
    const char* sep = "";
    if (tl) {
      const char last = title[tl - 1];
      sep = (last == '.' || last == '!' || last == '?' || last == ':') ? " " : ". ";
    }
    snprintf(pre, sizeof(pre), "\n- %s %s: %s%s", date, haveHost ? host : "unknown site", title, sep);
    const size_t pl = strlen(pre);
    size_t room = limit - n;
    if (pl + cl > room) {
      /* The last line that goes: its content cut at a word to what is left - when at least 60 bytes of
       * it are; else the block ends with the line before. */
      if (room < pl + LINE_MIN) {
        break;
      }
      cl = cutAtWord(content, cl, room - pl, true, true);
    }
    memcpy(out + n, pre, pl);
    memcpy(out + n + pl, content, cl);
    n += pl + cl;
    out[n] = '\0';
    lines++;
    // the sources line: each host once, in the block's order, while it fits
    if (haveHost && src && srcCap) {
      if (!(sn && srcHas(src, host))) {
        const size_t add = (sn ? 2 : sizeof(GEM_WEB_PREFIX) - 1) + strlen(host);
        if (sn + add <= WEB_SRC_MAX && sn + add < srcCap) {
          if (!sn) {
            memcpy(src, GEM_WEB_PREFIX, sizeof(GEM_WEB_PREFIX) - 1);
            sn = sizeof(GEM_WEB_PREFIX) - 1;
          } else {
            memcpy(src + sn, "; ", 2);
            sn += 2;
          }
          memcpy(src + sn, host, strlen(host));
          sn += strlen(host);
          src[sn] = '\0';
        }
      }
    }
    if (n >= limit) {
      break;
    }
  }
  if (lines == 0 && !summary) {
    out[0] = '\0';
    if (src && srcCap) {
      src[0] = '\0';
    }
    return 0;
  }
  if (src && srcCap && sn == 0) {
    /* Lines went, but no host a line may show (an IDN, junk): the sources line still says the web was
     * used - it never names what it cannot show. Only the summary went: Tavily's summary, said so. */
    if (lines > 0) {
      snprintf(src, srcCap, "%s%d result(s)", GEM_WEB_PREFIX, lines);
    } else {
      snprintf(src, srcCap, "%s", WEB_SUMMARY_SRC);
    }
  }
  if (used) {
    *used = lines;
  }
  return n;
}

static bool appendWith(char* dst, size_t cap, const char* sep, const char* s) {
  if (!dst || !cap || !s || !s[0]) {
    return false;
  }
  size_t have = 0;
  while (have < cap && dst[have]) {
    have++;
  }
  if (have >= cap) {
    return false;
  }
  const size_t sl = have ? strlen(sep) : 0;
  const size_t n = strlen(s);
  if (have + sl + n >= cap) {
    return false;                                // never half a block, never half a line
  }
  memcpy(dst + have, sep, sl);
  memcpy(dst + have + sl, s, n + 1);
  return true;
}

bool webAppendBlock(char* bg, size_t cap, const char* block) {
  return appendWith(bg, cap, "\n\n", block);
}

bool webAppendLine(char* src, size_t cap, const char* line) {
  return appendWith(src, cap, "\n", line);
}

// ── when no results go ───────────────────────────────────────────────────────────────────

int webNoteKind(int httpCode, int net, const WebParse* p, int parsed) {
  (void)net;
  if (httpCode == 200) {
    if (parsed == WEB_PARSE_OK) {
      return WEB_NOTE_NONE;                      // (the caller: a block that came out empty is NOTHING)
    }
    return parsed == WEB_PARSE_NONE ? WEB_NOTE_NOTHING : WEB_NOTE_UNAVAILABLE;
  }
  if (httpCode == 401 || httpCode == 403) {
    return WEB_NOTE_KEY;
  }
  if (httpCode == 429 || httpCode == 432 || httpCode == 433) {
    return WEB_NOTE_LIMIT;
  }
  if (httpCode >= 400 && httpCode < 500 && p && p->limitWord) {
    return WEB_NOTE_LIMIT;                       // a plan or credit word in Tavily's detail (never its text)
  }
  return WEB_NOTE_UNAVAILABLE;                   // a timeout, no connection, a 3xx (never followed), a 5xx...
}

const char* webNoteLine(int kind) {
  switch (kind) {
  case WEB_NOTE_UNAVAILABLE: return GEM_WEB_PREFIX "search unavailable";
  case WEB_NOTE_KEY:         return GEM_WEB_PREFIX "Tavily refused the key - check tavily.txt";
  case WEB_NOTE_LIMIT:       return GEM_WEB_PREFIX "search limit reached";
  case WEB_NOTE_NOTHING:     return GEM_WEB_PREFIX "no results";
  case WEB_NOTE_CAP:         return GEM_WEB_PREFIX "not searched - today's " WEB_DAILY_AUTO_S " automatic searches are used";
  default:                   return NULL;
  }
}

// ── the screen's words ────────────────────────────────────────────────────────────────────

const char* const WEB_KEYINFO_NOTES[] = {
  "Web search (optional, free to start) uses Tavily: before a question about something recent, the "
  "phone searches the web and the model is given the top results.",
  "On a computer make an account at tavily.com and copy your API key (it starts tvly-). The free plan "
  "had 1,500 searches a month in October 2026.",
  "Make a text file named tavily.txt with one line: key=YOUR_KEY and put it in the API Keys folder, the "
  "same ways as gemini.txt.",
  "Menu > Web search: Auto searches only questions about recent things (" WEB_DAILY_AUTO_S " a day at most), "
  "Always every question, Off none. Ask with web search searches one question.",
  WEB_PRIVACY,
  "The key stays on the card, is never shown and goes only to Tavily (api.tavily.com). Such an answer "
  "ends with a Web: line naming the sites.",
  NULL,
};
