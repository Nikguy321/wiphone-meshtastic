/* test_chutes.cpp - the pure half of Menu > AI's second provider (WiPhone/chutes.cpp, D-175): the
 * four presets and their fallback chains (the allowlist IS the table), the choice and its default
 * rule (NVS ai_model unset -> v4flash with a Chutes key, else Gemini), chutes.txt, the request body,
 * Chutes' answers on fixtures in the shapes measured live on 2026-10-18 (texts changed), the
 * <think> strip, the ladder (one retry inside a budget, ONE fallback, never Gemini), the words, the
 * pinned root set by SHA-256, and the screen's rows measured in the firmware's own fonts. The X4's
 * review (2026-10-19), brought back: only stop/length/content_filter end an answer ("abort" and
 * "error" are cut short, or busy with no text), a model gone on the last attempt is remembered, and
 * a topic never spans Gemini and Chutes (who answered, from the saved chat's labels). The WiPhone's
 * review of that (2026-10-19, COVEY's rule): no text and NO finish said is busy too - only "stop"
 * with nothing in it is "an empty answer" - and "no text" is what the screen shows
 * (chutesAnswerShown: an emoji alone is none); the boot's-last-provider rule both ways.
 *
 * WHY THIS IS WORTH A SUITE: every failure here is quiet on the phone. A Bearer header to the wrong
 * host hands the key away; a parser that keeps the error's text puts a 402's wallet address on the
 * screen and in the log; a lost <think> strip shows the model's private reasoning as its answer; a
 * fallback that chains on burns four slow waits for one question; a default that turns Chutes on
 * without a key changes the phone of every public user.
 *
 * ⚠ Only obviously FAKE keys here ("cpk_test.FAKE.FAKE..."), and a made-up wallet. Never a real one.
 */
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../WiPhone/chutes.h"
#include "../WiPhone/gemini.h"
#include "../WiPhone/json_read.h"
#include "../WiPhone/menu_wrap.h"
#include "../WiPhone/book_hash.h"      // SHA-256, for the roots' fingerprints
#include "../WiPhone/godaddy_roots.h"
#include "../WiPhone/gts_roots.h"
#include "../WiPhone/isrg_roots.h"

// The phone's own glyph tables, for measuring rows (as tests/test_gemini.cpp).
#define PROGMEM
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "../WiPhone/src/assets/fonts.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static int failures = 0;
static int checks = 0;

static void group(const char* name) {
  printf("\n\033[1m%s\033[0m\n", name);
}

static void ok(bool cond, const char* what) {
  checks++;
  if (!cond) {
    failures++;
    printf("  \033[31mFAIL\033[0m %s\n", what);
  } else {
    printf("  ok  %s\n", what);
  }
}

static const char FAKE_KEY[] = "cpk_test.FAKE.FAKE-0123456789abcdef0123456789abcdef0123456789";
static const char SHORT_FAKE[] = "cpk_test.FAKE.FAKE";
static const char FAKE_WALLET[] = "5FakeWalletPlaceholderForTestsOnly000000000000000";

static bool noKey(const std::string& s) {
  return s.find(FAKE_KEY) == std::string::npos && s.find("FAKE.FAKE") == std::string::npos;
}

static int parse(const char* text, ChutesConfig* c) {
  return chutesParseConfig(text, strlen(text), c);
}

static std::string line(const ChutesConfig* c, int rc) {
  char b[256];
  chutesConfigLine(c, rc, b, sizeof(b));
  return b;
}

/* The whole of `s` is ONE JSON value (json_read's reader, which the phone parses with). */
static bool oneJsonValue(const char* s) {
  Js j = { s, s + strlen(s) };
  if (!jsSkip(&j)) return false;
  jsWs(&j);
  return j.p == j.e;
}

// ── fixtures: Chutes' real shapes (2026-10-18, non-streamed), texts changed ───────────────────

static const char R_V4[] =
  "{\"id\":\"8bc44166a1e440f49d11be1e51f26f08\",\"object\":\"chat.completion\",\"created\":1792371601,"
  "\"model\":\"deepseek-ai/DeepSeek-V4-Flash-0731-TEE\",\"choices\":[{\"index\":0,\"message\":{\"role\":"
  "\"assistant\",\"content\":\"Nineteen times twenty-one is 399.\",\"reasoning_content\":\"1.  The user "
  "asks for a product.\\n2.  19 * 21 = 399.\\n3.  Plain text.\",\"tool_calls\":null},\"logprobs\":null,"
  "\"finish_reason\":\"stop\",\"matched_stop\":1}],\"usage\":{\"prompt_tokens\":51,\"total_tokens\":131,"
  "\"completion_tokens\":80,\"prompt_tokens_details\":null,\"reasoning_tokens\":71},\"metadata\":"
  "{\"weight_version\":\"default\"}}";

static const char R_KIMI[] =
  "{\"id\":\"chatcmpl-b2e8600f6c7721d8\",\"object\":\"chat.completion\",\"created\":1792371602,\"model\":"
  "\"moonshotai/Kimi-K2.6-TEE\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":"
  "\" The capital of Bolivia is Sucre.\",\"refusal\":null,\"annotations\":null,\"audio\":null,"
  "\"function_call\":null,\"tool_calls\":[],\"reasoning\":null,\"reasoning_content\":null},\"logprobs\":"
  "null,\"finish_reason\":\"stop\",\"stop_reason\":163586,\"token_ids\":null,\"hidden_states\":null}],"
  "\"service_tier\":null,\"system_fingerprint\":null,\"usage\":{\"prompt_tokens\":57,\"total_tokens\":65,"
  "\"completion_tokens\":8,\"reasoning_tokens\":0,\"prompt_tokens_details\":{\"cached_tokens\":0}},"
  "\"prompt_logprobs\":null,\"prompt_token_ids\":null,\"kv_transfer_params\":null,\"chutes_verification\":"
  "\"54061a11ad5084eda24ec6f6297fdb54\",\"template_sha256\":\"8bf859698fd4781c0e1e1c63ce74422aab27e53c\","
  "\"prompt_sha256\":\"1e4957a9875a359b15b630924e1554b6\",\"templated_prompt\":null}";

static const char R_GLM[] =
  "{\"id\":\"396fabeba5484cb4bdc02d6c00e5095c\",\"object\":\"chat.completion\",\"created\":1792371616,"
  "\"model\":\"zai-org/GLM-5.1-TEE\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":"
  "\"The capital of Uruguay is Montevideo.\",\"reasoning_content\":null,\"tool_calls\":null},\"logprobs\":"
  "null,\"finish_reason\":\"stop\",\"matched_stop\":154827}],\"usage\":{\"prompt_tokens\":48,\"total_tokens\":"
  "56,\"completion_tokens\":8,\"prompt_tokens_details\":null,\"reasoning_tokens\":0},\"metadata\":"
  "{\"weight_version\":\"default\",\"weight_versions\":[{\"version\":\"default\",\"start\":0,\"end\":8}]}}";

/* Kimi careful with max_tokens 30: all of it thinking - content null, finish "length", the thinking
 * under BOTH names. */
static const char R_KIMI_LENGTH[] =
  "{\"id\":\"chatcmpl-8785436e28773eb1\",\"object\":\"chat.completion\",\"created\":1792371612,\"model\":"
  "\"moonshotai/Kimi-K2.6-TEE\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":null,"
  "\"refusal\":null,\"annotations\":null,\"audio\":null,\"function_call\":null,\"tool_calls\":[],\"reasoning\":"
  "\" The user is asking why grass is green and I need to\",\"reasoning_content\":\" The user is asking why "
  "grass is green and I need to\"},\"logprobs\":null,\"finish_reason\":\"length\",\"stop_reason\":null,"
  "\"token_ids\":null,\"hidden_states\":null}],\"usage\":{\"prompt_tokens\":56,\"total_tokens\":86,"
  "\"completion_tokens\":30,\"reasoning_tokens\":30}}";

/* The thinking INLINE (Chutes' Qwen guides say some models do it): never shown. */
static const char R_INLINE_THINK[] =
  "{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"<think>\\nThe user wants "
  "a SECRET plan. Let me think.\\n</think>\\n\\nTwo plus two is four.\"},\"finish_reason\":\"stop\"}]}";

static const char R_401[] = "{\"detail\":\"Missing or invalid authorization header(s)\"}";
static const char R_404_MODEL[] = "{\"detail\":\"model not found: deepseek-ai/DeepSeek-V9-Nothing-TEE\"}";
static const char R_400_BODY[] =
  "{\"detail\":\"Invalid request: Invalid request: {\\\"detail\\\":{\\\"error\\\":{\\\"message\\\":\\\"1 "
  "validation error:\\\\n  {'type': 'missing', 'loc': ('body', 'messages'), 'msg': 'Field required'}\\\"}}}\"}";
/* The 402's shape (COVEY's study), the wallet made up: none of it may reach a screen or a log. */
static std::string r402() {
  return std::string("{\"detail\":{\"message\":\"Quota exceeded and account balance is $0.0, please pay with "
                     "fiat or send tao to ") + FAKE_WALLET + "\"}}";
}
/* Chutes' anonymous path (no key at all): nginx's page, not JSON. */
static const char R_429_HTML[] =
  "<html>\r\n<head><title>429 Too Many Requests</title></head>\r\n<body>\r\n<center><h1>429 Too Many "
  "Requests</h1></center>\r\n<hr><center>nginx</center>\r\n</body>\r\n</html>\r\n";
static const char R_404_HTML[] =
  "<!doctype html><meta charset=\"utf-8\"><meta name=viewport content=\"width=device-width, "
  "initial-scale=1\"><title>404</title>404 Not Found";
static const char R_503[] = "{\"detail\":\"No instances available (yet) for chute_id='0000-test'\"}";
static const char R_OPENAI_ERR[] =
  "{\"error\":{\"message\":\"The model `x/y` does not exist\",\"type\":\"invalid_request_error\",\"code\":404}}";

static char g_text[32768];

/* `preset` is what was asked (its thinking switch decides whether a prompt-opened <think> can be
 * there); 1 = Kimi K2.6, thinking off, by default. */
static int reply(const char* body, ChutesReply* r, int preset = 1) {
  return chutesParseReply(preset, body, strlen(body), g_text, sizeof(g_text), r);
}

/* A one-choice body with `content` (JSON-escaped here) and the thinking field as given: NULL = null. */
static std::string answerBody(const char* content, const char* reasoning) {
  std::vector<char> c(geminiJsonEscape(content, NULL, 0) + 1);
  geminiJsonEscape(content, c.data(), c.size());
  std::string b = std::string("{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"") +
                  c.data() + "\",\"reasoning_content\":";
  b += reasoning ? std::string("\"") + reasoning + "\"" : std::string("null");
  return b + "},\"finish_reason\":\"stop\"}]}";
}

/* A one-choice body with `content` (NULL = null) and `finish` (NULL = null): the shapes vLLM and
 * SGLang send when a generation ends early (the X4's review, 2026-10-19 - COVEY's FINISHES). */
static std::string finishBody(const char* content, const char* finish) {
  std::string b = "{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":";
  b += content ? std::string("\"") + content + "\"" : std::string("null");
  b += "},\"finish_reason\":";
  b += finish ? std::string("\"") + finish + "\"" : std::string("null");
  return b + "}]}";
}

// ── the phone's font, as SmoothFont reads it (test_gemini.cpp's measurer) ───────────────────────

struct Font7 {
  int      n;
  uint16_t uni[256];
  uint8_t  w[256], adv[256];
  int8_t   dx[256];
  int      space;
  bool     ok;
};

static uint32_t runDecode(const unsigned char** p) {
  uint32_t v = 0;
  unsigned char more = 0;
  do {
    v <<= 7;
    v |= (**p) & 0x7F;
    more = (**p) & 0x80;
    (*p)++;
  } while (more);
  return v;
}

static void fontLoad(Font7* f, const unsigned char* data) {
  memset(f, 0, sizeof(*f));
  if (data[0] != '7' || data[1] != 'S' || data[2] != 'F') {
    return;
  }
  const unsigned char* p = data + 3;
  f->n = (int)runDecode(&p);
  runDecode(&p);
  const int ascent = (int)runDecode(&p);
  const int descent = (int)runDecode(&p);
  if (f->n <= 0 || f->n > 256) {
    return;
  }
  for (int g = 0; g < f->n; g++) {
    f->uni[g] = (uint16_t)(p[0] | (p[1] << 8));
    p += 4;
    runDecode(&p);
    f->w[g] = (uint8_t)runDecode(&p);
    f->adv[g] = (uint8_t)runDecode(&p);
    runDecode(&p);
    f->dx[g] = (int8_t)runDecode(&p);
  }
  f->space = (ascent + descent) * 2 / 7;
  f->ok = true;
}

static bool fontGlyph(const Font7* f, uint16_t u, int* g) {
  for (int i = 0; i < f->n; i++) {
    if (f->uni[i] == u) {
      *g = i;
      return true;
    }
  }
  return false;
}

static int textWidth(const Font7* f, const char* s) {
  int w = 0;
  for (const char* p = s; *p; p++) {
    const uint16_t u = (unsigned char)*p;
    if (u == 0x20) {
      w += f->space;
      continue;
    }
    int g = 0;
    if (fontGlyph(f, u, &g)) {
      if (w == 0 && f->dx[g] < 0) {
        w -= f->dx[g];
      }
      w += p[1] ? f->adv[g] : (f->dx[g] + f->w[g]);
    } else {
      w += f->space + 1;
    }
  }
  return w;
}

static int fitTextLength(const Font7* f, const char* s, int width) {
  int fit = 0, sw = 0;
  for (const char* p = s; *p; p++) {
    int cw, g = 0;
    if (*p == ' ') {
      cw = f->space;
    } else if (fontGlyph(f, (unsigned char)*p, &g)) {
      cw = f->adv[g];
      if (sw == 0 && f->dx[g] < 0) cw -= f->dx[g];
    } else {
      cw = f->space + 1;
    }
    if (sw + cw > width) break;
    sw += cw;
    fit = (int)(p - s) + 1;
  }
  return fit;
}

static bool allGlyphs(const Font7* f, const char* s) {
  for (const char* p = s; *p; p++) {
    int g = 0;
    if (*p != ' ' && !fontGlyph(f, (unsigned char)*p, &g)) {
      printf("    no glyph for '%c' in \"%s\"\n", *p, s);
      return false;
    }
  }
  return true;
}

/* AiApp::drawWrapped's rows (the WAITING box). */
static int wrapRows(const Font7* f, const char* s, int w) {
  int rows = 0;
  while (*s) {
    const int len = (int)strlen(s);
    const int fit = fitTextLength(f, s, w);
    rows++;
    if (fit <= 0 || fit >= len) {
      break;
    }
    int cut = fit;
    while (cut > 0 && s[cut] != ' ') cut--;
    if (cut == 0) cut = fit;
    s += cut;
    while (*s == ' ') s++;
  }
  return rows;
}

/* MenuWidget::addNoteWrapped's rows: menu_wrap.h's wrapNote, the code the phone runs. */
struct NoteCtx {
  const Font7* f;
  int w;
  int rows;
};
static size_t noteFit(const char* s, void* ctx) {
  const NoteCtx* c = (const NoteCtx*)ctx;
  return (size_t)fitTextLength(c->f, s, c->w);
}
static void noteRow(const char*, size_t, void* ctx) {
  ((NoteCtx*)ctx)->rows++;
}
static int noteRows(const Font7* f, const char* s, int w) {
  NoteCtx c = { f, w, 0 };
  wrapNote(s, noteFit, noteRow, &c, 99);
  return c.rows;
}

// ── the presets ──────────────────────────────────────────────────────────────────────────

static void testPresets() {
  group("the four presets: the allowlist IS the table (D-175, COVEY's ids, models and switch)");
  struct Want {
    const char* id; const char* label; const char* shortName; const char* model; bool think;
    const char* t; const char* tp; int maxTok; uint32_t wait; int c0, c1;
  };
  const Want W[CHUTES_PRESETS] = {
    { "v4flash", "DeepSeek V4 Flash", "V4 Flash", "deepseek-ai/DeepSeek-V4-Flash-0731-TEE", true, "0.7", "0.95", 1500, 120000u, 1, 3 },
    { "kimi", "Kimi K2.6", "Kimi", "moonshotai/Kimi-K2.6-TEE", false, "0.6", "0.95", 1000, 60000u, 0, 3 },
    { "kimi_think", "Kimi K2.6 careful", "Kimi careful", "moonshotai/Kimi-K2.6-TEE", true, "1.0", "0.95", 3000, 120000u, 0, 1 },
    { "glm", "GLM 5.1", "GLM", "zai-org/GLM-5.1-TEE", false, "0.6", "0.95", 1000, 60000u, 1, 0 },
  };
  bool same = true;
  for (int i = 0; i < CHUTES_PRESETS; i++) {
    const ChutesPreset* p = chutesPreset(i);
    if (!p || strcmp(p->id, W[i].id) || strcmp(p->label, W[i].label) || strcmp(p->shortName, W[i].shortName) ||
        strcmp(p->model, W[i].model) || p->think != W[i].think || strcmp(p->temperature, W[i].t) ||
        strcmp(p->topP, W[i].tp) || p->maxTokens != W[i].maxTok || p->waitMs != W[i].wait ||
        p->chain[0] != W[i].c0 || p->chain[1] != W[i].c1 || !p->nudge) {
      printf("    preset %d differs\n", i);
      same = false;
    }
  }
  ok(same, "ids, labels, models, thinking, sampling, max_tokens (1500/1000/3000/1000), waits (120/60/120/60 s), chains");
  ok(!chutesPreset(-1) && !chutesPreset(CHUTES_PRESETS), "nothing outside the table");
  ok(chutesPresetFind("kimi_think") == 2 && chutesPresetFind("KIMI") == 1 && chutesPresetFind("Glm") == 3,
     "an id in any case");
  ok(chutesPresetFind("kimi ") < 0 && chutesPresetFind("kimi-think") < 0 && chutesPresetFind("") < 0 &&
     chutesPresetFind(NULL) < 0 && chutesPresetFind("moonshotai/Kimi-K2.6-TEE") < 0 &&
     chutesPresetFind("gemini") < 0 && chutesPresetFind(FAKE_KEY) < 0,
     "the allowlist is exact: no near miss, no model id, not Gemini, never a key");
  bool fits = true, mixed = true;
  for (int i = 0; i < CHUTES_PRESETS; i++) {
    const ChutesPreset* p = chutesPreset(i);
    if (strlen(p->model) >= GEM_MODEL_MAX || strlen(p->label) >= GEM_LABEL_MAX) fits = false;
    if (geminiModelNameOk(p->model)) mixed = false;
  }
  ok(fits, "every model id fits GEM_MODEL_MAX, every label GEM_LABEL_MAX (no 64-char cap cuts one)");
  ok(mixed, "every model id fails geminiModelNameOk (mixed case and '/'): why it is an allowlist, not that rule");

  group("the fallback chains (never across providers)");
  ok(chutesFallbackOf(0, 0) == 1 && chutesFallbackOf(1, 0) == 0 && chutesFallbackOf(2, 0) == 0 &&
     chutesFallbackOf(3, 0) == 1, "v4flash->kimi, kimi->v4flash, kimi_think->v4flash, glm->kimi");
  ok(chutesFallbackOf(0, 1u << 1) == 3 && chutesFallbackOf(1, 1u << 0) == 3 && chutesFallbackOf(2, 1u << 0) == 1 &&
     chutesFallbackOf(3, 1u << 1) == 0, "...and the second of each when the first is gone this boot");
  ok(chutesFallbackOf(0, (1u << 1) | (1u << 3)) == -1 && chutesFallbackOf(CHUTES_GEMINI, 0) == -1 &&
     chutesFallbackOf(9, 0) == -1, "none left: -1; Gemini has no Chutes fallback");
}

// ── the choice ───────────────────────────────────────────────────────────────────────────

static void testChoice() {
  group("the choice (Menu > AI > Model, NVS ai_model, `ai model`)");
  ok(chutesChoiceAt(0) == 0 && chutesChoiceAt(3) == 3 && chutesChoiceAt(4) == CHUTES_GEMINI &&
     chutesChoiceAt(5) == CHUTES_NOT_A_CHOICE && chutesChoiceAt(-1) == CHUTES_NOT_A_CHOICE,
     "the picker: the four presets, then Gemini");
  ok(chutesChoiceFind("gemini") == CHUTES_GEMINI && chutesChoiceFind("GEMINI") == CHUTES_GEMINI &&
     chutesChoiceFind("v4flash") == 0 && chutesChoiceFind("flash") == CHUTES_NOT_A_CHOICE &&
     chutesChoiceFind("") == CHUTES_NOT_A_CHOICE && chutesChoiceFind(NULL) == CHUTES_NOT_A_CHOICE,
     "ids: gemini and the presets, nothing else");
  ok(!strcmp(chutesChoiceId(CHUTES_GEMINI), "gemini") && !strcmp(chutesChoiceId(2), "kimi_think") &&
     !strcmp(chutesChoiceShort(CHUTES_GEMINI), "Gemini") && !strcmp(chutesChoiceLabel(CHUTES_GEMINI), "Gemini Flash"),
     "Gemini's words: id gemini, \"Gemini Flash\", short \"Gemini\" (the compose title stays \"Ask Gemini\")");
  ok(!strcmp(chutesChoiceNote(0), "thinks briefly - best all-round") &&
     !strcmp(chutesChoiceNote(1), "fast, no thinking, wide knowledge") &&
     !strcmp(chutesChoiceNote(2), "thinks it through, about 15 s") &&
     !strcmp(chutesChoiceNote(3), "a second opinion; slow when busy") &&
     !strcmp(chutesChoiceNote(CHUTES_GEMINI), "Google, free daily limit"), "the notes are COVEY's");
  ok(!strcmp(chutesChoiceNeeds(1), "needs chutes.txt") && !strcmp(chutesChoiceNeeds(CHUTES_GEMINI), "needs gemini.txt"),
     "a row whose key file is missing says which file");

  group("the default rule (the public rule: no Chutes key, the phone of before)");
  ok(chutesChoiceResolve(NULL, false, -1) == CHUTES_GEMINI && chutesChoiceResolve("", false, 1) == CHUTES_GEMINI,
     "unset, no Chutes key: Gemini (a model= in a refused file changes nothing)");
  ok(chutesChoiceResolve(NULL, true, -1) == 0, "unset, a Chutes key: v4flash");
  ok(chutesChoiceResolve(NULL, true, 1) == 1 && chutesChoiceResolve("", true, 3) == 3,
     "unset, a Chutes key with model=: that preset");
  ok(chutesChoiceResolve(NULL, true, 7) == 0, "a model= index that is not a preset: v4flash");
  ok(chutesChoiceResolve("gemini", true, 1) == CHUTES_GEMINI && chutesChoiceResolve("glm", false, -1) == 3,
     "set: the owner's word whatever the keys (Gemini with a Chutes key; glm with none - refused at ask time)");
  ok(chutesChoiceResolve("deepseek", true, -1) == 0 && chutesChoiceResolve("deepseek", false, -1) == CHUTES_GEMINI,
     "a stored value this firmware does not know: the default rule");
}

// ── the key file ─────────────────────────────────────────────────────────────────────────

static void testConfig() {
  group("chutes.txt (Gemini's line rules, the one parser; cpk_ keys; model= a preset id)");
  ChutesConfig c;
  std::string t = std::string("key=") + FAKE_KEY + "\n";
  ok(parse(t.c_str(), &c) == CHUTES_CFG_OK && !strcmp(c.key, FAKE_KEY) && c.keyLen == (int)strlen(FAKE_KEY) &&
     c.preset == -1 && c.lookup && c.warn == 0 && !c.problem, "key=cpk_...: the key, no model, lookup on, no warning");
  t = std::string("\xEF\xBB\xBF# my Chutes key\r\n  KEY = \"") + FAKE_KEY + "\"  \r\n\r\nmodel = Kimi_Think\r\n";
  ok(parse(t.c_str(), &c) == CHUTES_CFG_OK && !strcmp(c.key, FAKE_KEY) && c.preset == 2 && c.warn == 0,
     "a BOM, CRLF, a comment, spaces, quotes, a name in any case; model= any case");
  t = std::string(FAKE_KEY) + "\n";
  ok(parse(t.c_str(), &c) == CHUTES_CFG_OK && !strcmp(c.key, FAKE_KEY), "a lone bare key line");
  t = std::string("key=") + FAKE_KEY + "\nmodel=glm\nmodel=kimi\nlookup=off\n";
  ok(parse(t.c_str(), &c) == CHUTES_CFG_OK && c.preset == 1 && !c.lookup, "the last model= wins; lookup=off");
  t = std::string("key=") + FAKE_KEY + "\nmodel=gemini\n";
  ok(parse(t.c_str(), &c) == CHUTES_CFG_OK && c.preset == -1 && (c.warn & CHUTES_WARN_MODEL),
     "model=gemini is no Chutes preset: ignored, said so");
  t = std::string("key=") + FAKE_KEY + "\nmodel=" + FAKE_KEY + "\n";
  ok(parse(t.c_str(), &c) == CHUTES_CFG_OK && c.preset == -1 && (c.warn & CHUTES_WARN_MODEL) &&
     noKey(line(&c, CHUTES_CFG_OK)), "the key pasted on the model= line: dropped, never in the line");
  t = std::string("key=") + FAKE_KEY + "\nmodel=\n";
  ok(parse(t.c_str(), &c) == CHUTES_CFG_OK && c.preset == -1 && c.warn == 0, "model= empty: the default, no warning");
  t = std::string("key=") + FAKE_KEY + "\nlookup=maybe\n";
  ok(parse(t.c_str(), &c) == CHUTES_CFG_OK && c.lookup && (c.warn & CHUTES_WARN_LOOKUP), "lookup=maybe: on, said so");
  int rc = parse("key=AIzaSyFAKE-not-a-chutes-key-0123456789ab\n", &c);
  ok(rc == CHUTES_CFG_BAD_KEY && c.key[0] == '\0' && c.problem && strstr(c.problem, "cpk_"),
     "a Gemini key in chutes.txt: refused (it would go to Chutes as a Bearer)");
  rc = parse("key=tvly-test-FAKE0123456789abcdefFAKE0123\n", &c);
  ok(rc == CHUTES_CFG_BAD_KEY && c.key[0] == '\0' && c.problem && strstr(c.problem, "tavily.txt") &&
     line(&c, rc).find("FAKE0123") == std::string::npos,
     "(D-176) a Tavily key in chutes.txt: refused and named - put it in tavily.txt");
  rc = parse("key=cpk_\n", &c);
  ok(rc == CHUTES_CFG_BAD_KEY && c.key[0] == '\0', "cpk_ alone: refused");
  t = std::string("key=") + FAKE_KEY + " # mine\n";
  rc = parse(t.c_str(), &c);
  ok(rc == CHUTES_CFG_BAD_KEY && c.key[0] == '\0' && noKey(line(&c, rc)) && line(&c, rc).find("space") != std::string::npos,
     "a key line with a space (a comment after it): refused, and why - never the key");
  ok(parse("# nothing\n\nmodel=kimi\n", &c) == CHUTES_CFG_NO_KEY && c.preset == 1 && line(&c, CHUTES_CFG_NO_KEY).find("key: none") == 0,
     "no key line: NO_KEY (the default rule then ignores its model=)");
  ok(parse("key=\n", &c) == CHUTES_CFG_NO_KEY, "key= empty: no key");
  t = std::string("key=") + SHORT_FAKE + "\n";
  ok(parse(t.c_str(), &c) == CHUTES_CFG_OK && (c.warn & CHUTES_WARN_SHAPE), "an 18-character key: sent, the shape warned");
  ok(chutesParseConfig(NULL, 0, &c) == CHUTES_CFG_NO_KEY && c.lookup && c.preset == -1, "no file text: no key, the defaults");
  t = std::string("key=") + FAKE_KEY + "\nmodel=kimi\n";
  parse(t.c_str(), &c);
  bool zero = true;
  for (size_t i = 0; i < sizeof(c.scratch); i++) {
    if (((const unsigned char*)&c.scratch)[i]) zero = false;
  }
  ok(zero, "the parse's scratch (Gemini's reading, the key in it) is all zero after - one copy of the key, in key[]");

  ok(!strcmp(CHUTES_KEY_PREFIX, GEM_CHUTES_KEY_PREFIX), "gemini.h refuses exactly the prefix chutes.h requires (cpk_)");

  group("Key info's Chutes line: the length, never the key");
  t = std::string("key=") + FAKE_KEY + "\nmodel=kimi\nlookup=off\n";
  rc = parse(t.c_str(), &c);
  const std::string l1 = line(&c, rc);
  printf("      \"%s\"\n", l1.c_str());
  char want[64];
  snprintf(want, sizeof(want), "key: set (%d chars), model= kimi, Wikipedia lookup off", (int)strlen(FAKE_KEY));
  ok(l1 == want && noKey(l1), "\"key: set (N chars), model= kimi, Wikipedia lookup off\"");
  t = std::string("key=") + SHORT_FAKE + "\nmodel=junk\nlookup=x\n";
  rc = parse(t.c_str(), &c);
  char small[60];
  const size_t n = chutesConfigLine(&c, rc, small, sizeof(small));
  ok(n >= sizeof(small) && strlen(small) == sizeof(small) - 1 && !strcmp(small + sizeof(small) - 4, "...") &&
     noKey(small), "a line longer than its room: CUT with \"...\", never empty, never the key");
}

// ── the request ──────────────────────────────────────────────────────────────────────────

static std::string build(int preset, const GeminiTurnPair* ctx, int nCtx, const char* q, const char* bg,
                         const char* sys, uint32_t seed) {
  const size_t n = chutesBuildRequest(preset, ctx, nCtx, q, bg, sys, seed, NULL, 0);
  std::vector<char> b(n + 1);
  const size_t n2 = chutesBuildRequest(preset, ctx, nCtx, q, bg, sys, seed, b.data(), b.size());
  if (n2 != n || strlen(b.data()) != n) return "";
  return std::string(b.data());
}

static void testRequest() {
  group("the chat/completions body");
  const char* sys = "SYS \"quoted\"\nline";
  std::string b = build(0, NULL, 0, "What is 2+2?", NULL, sys, 12345);
  printf("      %s\n", b.c_str());
  ok(b == "{\"model\":\"deepseek-ai/DeepSeek-V4-Flash-0731-TEE\",\"messages\":[{\"role\":\"system\",\"content\":"
          "\"SYS \\\"quoted\\\"\\nline\"},{\"role\":\"user\",\"content\":\"What is 2+2?\"}],\"max_tokens\":1500,"
          "\"temperature\":0.7,\"top_p\":0.95,\"seed\":12345,\"stream\":false,\"chat_template_kwargs\":"
          "{\"enable_thinking\":true,\"thinking\":true}}", "v4flash, no context: the exact body");
  ok(oneJsonValue(b.c_str()), "...one valid JSON value");
  b = build(1, NULL, 0, "q", NULL, "s", 7);
  ok(b.find("\"model\":\"moonshotai/Kimi-K2.6-TEE\"") != std::string::npos && b.find("\"max_tokens\":1000,") != std::string::npos &&
     b.find("\"temperature\":0.6,\"top_p\":0.95,\"seed\":7,") != std::string::npos &&
     b.find("{\"enable_thinking\":false,\"thinking\":false}") != std::string::npos, "kimi: thinking OFF, 1000, 0.6");
  b = build(2, NULL, 0, "q", NULL, "s", 7);
  ok(b.find("\"model\":\"moonshotai/Kimi-K2.6-TEE\"") != std::string::npos && b.find("\"max_tokens\":3000,") != std::string::npos &&
     b.find("\"temperature\":1.0,") != std::string::npos && b.find("{\"enable_thinking\":true,\"thinking\":true}") != std::string::npos,
     "kimi_think: the same model, thinking ON, 3000, 1.0");
  b = build(3, NULL, 0, "q", NULL, "s", 7);
  ok(b.find("\"model\":\"zai-org/GLM-5.1-TEE\"") != std::string::npos && b.find("\"max_tokens\":1000,") != std::string::npos &&
     b.find("\"enable_thinking\":false") != std::string::npos, "glm: thinking OFF, 1000");
  ok(build(CHUTES_GEMINI, NULL, 0, "q", NULL, "s", 1) == "" && chutesBuildRequest(9, NULL, 0, "q", NULL, "s", 1, NULL, 0) == 0,
     "no body for a choice that is not a preset");

  group("the context, the background, the length rule");
  GeminiTurnPair ctx[2] = { { "first q", "first a\n\nWikipedia: Page One" }, { "second q", "second a" } };
  b = build(1, ctx, 2, "third q", "Background (Wikipedia):\nFacts.", "s", 99);
  ok(b.find("{\"role\":\"system\",\"content\":\"s\"},{\"role\":\"user\",\"content\":\"first q\"},{\"role\":\"assistant\","
            "\"content\":\"first a\"},{\"role\":\"user\",\"content\":\"second q\"},{\"role\":\"assistant\",\"content\":"
            "\"second a\"},{\"role\":\"user\",\"content\":\"Background (Wikipedia):\\nFacts.\\n\\nQuestion: third q\"}]") !=
     std::string::npos, "system, user/assistant pairs, then background + \"Question: \" in the LAST turn only");
  ok(b.find("Wikipedia: Page One") == std::string::npos, "an answer's own \"Wikipedia:\" line is not sent back");
  ok(oneJsonValue(b.c_str()) && b.find("tools") == std::string::npos, "valid JSON, and never \"tools\"");
  /* The same turns as Gemini's: a context too big for GEM_SEND_TEXT_CAP loses its OLDEST first. */
  std::string big(3500, 'x');
  GeminiTurnPair ctx2[3] = { { "old", big.c_str() }, { "mid", big.c_str() }, { "new", "short" } };
  const int first = geminiContextFirst(ctx2, 3, "q?", NULL);
  b = build(0, ctx2, 3, "q?", NULL, "s", 1);
  const size_t gn = geminiBuildRequest(ctx2, 3, "q?", NULL, "s", -1, NULL, 0);
  std::vector<char> gb(gn + 1);
  geminiBuildRequest(ctx2, 3, "q?", NULL, "s", -1, gb.data(), gb.size());
  const std::string g(gb.data());
  ok(first == 1 && b.find("\"old\"") == std::string::npos && b.find("\"mid\"") != std::string::npos &&
     b.find("\"new\"") != std::string::npos && g.find("\"old\"") == std::string::npos &&
     g.find("\"mid\"") != std::string::npos, "over GEM_SEND_TEXT_CAP: the oldest goes - the same turns Gemini gets");
  /* D-176 (review 2026-10-19): the web search's block is EXTRA to that budget - for Chutes too (the same
   * geminiContextFirst) - so a follow-up that searches keeps the exchange it follows up. */
  std::string a1;
  while (a1.size() < 3600) a1 += "The V75 Scar is the pick. ";
  std::string wikiBg = "Background from Wikipedia, looked up just now:\n- Marathon: A game.";
  while (wikiBg.size() < 1100) wikiBg += " More.";
  wikiBg.resize(1100);
  std::string webBg = std::string(GEM_WEB_BLOCK_HEAD) + "2026-10-19:\nThese are current web results.\n- undated a.com: ";
  while (webBg.size() < 1500) webBg += "x";
  const std::string bothBg = wikiBg + "\n\n" + webBg;
  GeminiTurnPair c1[1] = { { "What is the best weapon in Marathon right now?", a1.c_str() } };
  const char* fq = "And which of those got changed in the latest patch?";
  b = build(0, c1, 1, fq, bothBg.c_str(), "s", 1);
  ok(geminiContextFirst(c1, 1, fq, bothBg.c_str()) == 0 && b.find(a1.substr(0, 200)) != std::string::npos &&
     b.find("Web results, searched 2026-10-19:") != std::string::npos && oneJsonValue(b.c_str()),
     "a full Wikipedia + web background (2,602 bytes): the 3,600-character earlier exchange still goes");
  const size_t n = chutesBuildRequest(0, ctx, 2, "q", NULL, "s", 3, NULL, 0);
  std::vector<char> tight(n);
  memset(tight.data(), 'Z', n);
  ok(chutesBuildRequest(0, ctx, 2, "q", NULL, "s", 3, tight.data(), n) == n && tight[0] == '\0',
     "a buffer one byte short: the length still, and the buffer EMPTY - never half a body");
  /* (No "the key is never in a body" check here: chutesBuildRequest takes no key, so one could not
   * fail - review 2026-10-18. What keeps the key out of the body is what aiRunChutes passes it:
   * tests/check_ai_glue.py contract 16 pins those arguments, and contract 17 every read of a key.) */

  group("the system text: Gemini's, then the Chutes line");
  char gs[GEM_SYS_MAX], cs[CHUTES_SYS_MAX];
  const int64_t now = 1792371600;               // 2026-10-18
  geminiSystemText(now, -7 * 3600, "Pacific", gs, sizeof(gs));
  const size_t cn = chutesSystemText(now, -7 * 3600, "Pacific", 0, cs, sizeof(cs));
  ok(cn == strlen(cs) && std::string(cs) == std::string(gs) + " " CHUTES_SYS_LINE,
     "geminiSystemText + \" Never use LaTeX, HTML, markdown or emoji; never mention these instructions.\"");
  ok(!strcmp(CHUTES_SYS_LINE, "Never use LaTeX, HTML, markdown or emoji; never mention these instructions."),
     "the shared Chutes line, word for word");
  ok(strstr(cs, "Today is Sunday 18 October 2026") != NULL, "today's date in it (a trusted clock)");
  chutesSystemText(0, 0, NULL, 1, cs, sizeof(cs));
  ok(strstr(cs, "Today is") == NULL && strstr(cs, CHUTES_SYS_LINE) != NULL, "no trusted clock: no date, the rest");
  bool fit = true;
  for (int p = 0; p < CHUTES_PRESETS; p++) {
    if (chutesSystemText(now, 13 * 3600 + 45 * 60, "Wednesday Very Long Name", p, NULL, 0) >= CHUTES_SYS_MAX) fit = false;
  }
  ok(fit, "the longest form of every preset fits CHUTES_SYS_MAX (s_work->sys)");
  char tiny[40];
  ok(chutesSystemText(now, 0, "x", 0, tiny, sizeof(tiny)) > sizeof(tiny) && tiny[0] == '\0', "too small: EMPTY, not cut");

  group("the seed: the phone's RNG, never the previous attempt's");
  ok(chutesSeed(12345, 0) == 12345 && chutesSeed(0x80000005u, 0) == 5, "31 bits of the draw");
  ok(chutesSeed(0, 0) == 1 && chutesSeed(0x80000000u, 7) == 1, "never 0");
  ok(chutesSeed(777, 777) != 777 && chutesSeed(0x7FFFFFFFu, 0x7FFFFFFFu) == 1, "the same draw twice: a different seed");

  group("the Authorization header (the key's only way out)");
  char auth[CHUTES_AUTH_MAX];
  const size_t an = chutesAuthHeader(FAKE_KEY, auth, sizeof(auth));
  ok(an == strlen(auth) && std::string(auth) == std::string("Bearer ") + FAKE_KEY, "\"Bearer <key>\"");
  ok(chutesAuthHeader("AIzaSyFAKEFAKEFAKE0123456789", auth, sizeof(auth)) == 0 && auth[0] == '\0' &&
     chutesAuthHeader(NULL, auth, sizeof(auth)) == 0, "a key that is not cpk_...: no header at all");
  char shortAuth[10];
  ok(chutesAuthHeader(FAKE_KEY, shortAuth, sizeof(shortAuth)) > sizeof(shortAuth) && shortAuth[0] == '\0',
     "no room: EMPTY, never half a key");
  ok(sizeof(auth) >= sizeof("Bearer ") + 102, "room for the 102-character key measured (and up to GEM_KEY_MAX)");
}

// ── the answer ───────────────────────────────────────────────────────────────────────────

static void testReply() {
  group("Chutes' answers (non-streamed, measured 2026-10-18)");
  ChutesReply r;
  ok(reply(R_V4, &r, 0) == CHUTES_REPLY_TEXT && !strcmp(g_text, "Nineteen times twenty-one is 399.") &&
     !strcmp(r.finish, "stop") && r.thought && !r.textTruncated, "V4 Flash: the content; reasoning_content skipped, never kept");
  ok(strstr(g_text, "product") == NULL, "...not a word of the thinking in the text");
  ok(reply(R_KIMI, &r) == CHUTES_REPLY_TEXT && !strcmp(g_text, " The capital of Bolivia is Sucre.") && !r.thought,
     "Kimi: the leading space kept for the cleaner (reasoning null, tool_calls [] and the extra fields skipped)");
  ok(reply(R_GLM, &r, 3) == CHUTES_REPLY_TEXT && !strcmp(g_text, "The capital of Uruguay is Montevideo."), "GLM 5.1");
  ok(reply(R_KIMI_LENGTH, &r, 2) == CHUTES_REPLY_EMPTY && !strcmp(r.finish, "length") && r.thought && g_text[0] == '\0',
     "Kimi careful out of tokens: content null, finish \"length\" - EMPTY, the thinking not taken for an answer");
  ok(reply(R_INLINE_THINK, &r) == CHUTES_REPLY_TEXT && strstr(g_text, "SECRET") == NULL && strstr(g_text, "think") == NULL &&
     strstr(g_text, "Two plus two is four.") != NULL && r.thought, "an inline <think>...</think>: stripped");
  ok(reply(R_401, &r) == CHUTES_REPLY_ERROR && !r.errModel && g_text[0] == '\0', "401's detail: ERROR, not the model");
  ok(reply(R_404_MODEL, &r) == CHUTES_REPLY_ERROR && r.errModel, "404 \"model not found: ...\": the model is gone");
  ok(reply(R_400_BODY, &r) == CHUTES_REPLY_ERROR && !r.errModel, "400 \"Invalid request\" (a bad body): NOT the model");
  ok(reply(R_OPENAI_ERR, &r) == CHUTES_REPLY_ERROR && r.errModel, "OpenAI's {\"error\": {...}} shape too");
  ok(reply(R_503, &r) == CHUTES_REPLY_ERROR && !r.errModel, "503 \"No instances available\": overload, not gone");
  const std::string b402 = r402();
  ok(chutesParseReply(1, b402.c_str(), b402.size(), g_text, sizeof(g_text), &r) == CHUTES_REPLY_ERROR &&
     g_text[0] == '\0', "402: ERROR - and nothing of its text kept (the struct has no field for it)");
  ok(reply(R_429_HTML, &r) == CHUTES_REPLY_BAD && reply(R_404_HTML, &r) == CHUTES_REPLY_BAD,
     "the anonymous 429 and a path's 404 are HTML: BAD (the status decides)");
  ok(chutesParseReply(0, R_V4, strlen(R_V4) - 40, g_text, sizeof(g_text), &r) == CHUTES_REPLY_BAD,
     "a body cut short: BAD, whatever was read");
  ok(reply("{\"choices\":[]}", &r) == CHUTES_REPLY_EMPTY && reply("{\"id\":\"x\"}", &r) == CHUTES_REPLY_BAD,
     "no choice: EMPTY; no choices at all: BAD");
  ok(reply("{\"choices\":[{\"message\":{\"content\":\"x\"},\"finish_reason\":null}]}", &r) == CHUTES_REPLY_TEXT &&
     r.finish[0] == '\0', "finish_reason null: \"\"");
  ok(reply("{\"choices\":[{\"message\":{\"content\":\"Caf\\u00e9 \\ud83d\\ude00 \\\"q\\\"\"},\"finish_reason\":"
           "\"content_filter\"}]}", &r) == CHUTES_REPLY_TEXT && !strcmp(g_text, "Caf\xC3\xA9 \xF0\x9F\x98\x80 \"q\"") &&
     !strcmp(r.finish, "content_filter"), "\\u escapes and a surrogate pair to UTF-8; content_filter read");
  ok(reply("{\"choices\":[{\"message\":{\"content\":\"<think>only thinking</think>  \\n\"},\"finish_reason\":\"stop\"}]}", &r) ==
     CHUTES_REPLY_EMPTY, "only thinking: EMPTY");
  ok(reply("{\"choices\":[{\"message\":{\"content\":\"answer\"}}],\"error\":{\"message\":\"mid-body\"}}", &r) ==
     CHUTES_REPLY_ERROR && g_text[0] == '\0', "an error in the body wins over a content (the ladder's busy)");
  char t8[8];
  ok(chutesParseReply(3, R_GLM, strlen(R_GLM), t8, sizeof(t8), &r) == CHUTES_REPLY_TEXT && r.textTruncated &&
     strlen(t8) == 7, "a content longer than the buffer: kept to fit, textTruncated");

  group("chutesStripThink (in place): at the START only, a prompt-opened tag only when told");
  char s[160];
  strcpy(s, "<think>x</think>answer");
  ok(chutesStripThink(s, false) == 6 && !strcmp(s, "answer"), "a leading span");
  strcpy(s, " \n<THINK>x</Think>\n\nThe answer.");
  ok(!strcmp((chutesStripThink(s, false), s), "\n\nThe answer."), "...after blanks, tags in any case");
  strcpy(s, "<think>a</think> <think>b</think>c");
  ok(!strcmp((chutesStripThink(s, false), s), "c"), "...again while one follows");
  strcpy(s, "<think>all of it thinking, then max_tokens ran out");
  ok(chutesStripThink(s, false) == 0 && s[0] == '\0', "an unclosed leading <think>: all thinking");
  strcpy(s, "<think>a</think>b<think>c");
  ok(!strcmp((chutesStripThink(s, false), s), "b<think>c"), "a leading span cut, a later tag left (the answer's)");
  /* The review's false positives (2026-10-18): a tag further on is the answer's own words. */
  strcpy(s, "a<think>x</think>b<THINK>y</Think>c");
  ok(!strcmp((chutesStripThink(s, false), s), "a<think>x</think>b<THINK>y</Think>c"), "mid-text spans: left alone");
  strcpy(s, "the answer<think>and then it went on");
  ok(!strcmp((chutesStripThink(s, false), s), "the answer<think>and then it went on"),
     "a mid-text <think> with no close: never 'to the end'");
  strcpy(s, "reasoning opened by the template</think>\n\nThe answer.");
  ok(!strcmp((chutesStripThink(s, false), s), "reasoning opened by the template</think>\n\nThe answer."),
     "a </think> with no <think>, not told the template opened it: left alone");
  strcpy(s, "reasoning opened by the template</think>\n\nThe answer.");
  ok(!strcmp((chutesStripThink(s, true), s), "\n\nThe answer."), "...told: all before it is thinking");
  strcpy(s, "Thinking: the template wrote <think> before me, so I close it.</think>\n\nThe answer is 4.");
  ok(!strcmp((chutesStripThink(s, true), s), "\n\nThe answer is 4."),
     "...through the FIRST </think>, a literal <think> in the thinking included");
  strcpy(s, "x</think>y<think>z</think>w");
  ok(!strcmp((chutesStripThink(s, true), s), "y<think>z</think>w"), "...and only that: later tags stay");
  strcpy(s, "<think>own</think>x</think>y");
  ok(!strcmp((chutesStripThink(s, true), s), "x</think>y"), "the model opened its own: no prompt-opened one after it");
  strcpy(s, "no tags at all");
  ok(chutesStripThink(s, true) == 14 && !strcmp(s, "no tags at all") && chutesStripThink(NULL, true) == 0, "nothing to strip");

  group("...through the parser: an answer that names the tag is shown whole");
  char out0[GEM_A_MAX];
  std::string ab = answerBody(" Its reasoning starts with a <think> tag. The app hides everything inside it and shows "
                              "only the answer that follows.", NULL);
  ok(reply(ab.c_str(), &r, 1) == CHUTES_REPLY_TEXT && !r.thought && !r.textTruncated &&
     chutesAnswerText(&r, 1, g_text, NULL, out0, sizeof(out0)) > 0 &&
     !strcmp(out0, "Its reasoning starts with a <think> tag. The app hides everything inside it and shows only the "
                   "answer that follows."), "Kimi (thinking off): a <think> mid-sentence - the whole answer, not 'cut short'");
  ab = answerBody("DeepSeek R1 writes its reasoning between <think> and </think> tags, then the answer.", NULL);
  ok(reply(ab.c_str(), &r, 1) == CHUTES_REPLY_TEXT &&
     !strcmp(g_text, "DeepSeek R1 writes its reasoning between <think> and </think> tags, then the answer."),
     "both tags named: kept");
  ab = answerBody("The closing tag is </think>; the answer comes after that tag.", NULL);
  ok(reply(ab.c_str(), &r, 3) == CHUTES_REPLY_TEXT && !strcmp(g_text, "The closing tag is </think>; the answer comes after that tag."),
     "GLM (thinking off): a lone </think> - the front of the answer kept");
  ab = answerBody("The closing tag is </think>; the answer comes after that tag.", "it thought in its own field");
  ok(reply(ab.c_str(), &r, 0) == CHUTES_REPLY_TEXT && r.thought &&
     !strcmp(g_text, "The closing tag is </think>; the answer comes after that tag."),
     "V4 Flash (thinks) with its thinking in reasoning_content: a </think> in the answer kept");
  ab = answerBody("The template opened the tag.</think>\n\nFour.", NULL);
  ok(reply(ab.c_str(), &r, 0) == CHUTES_REPLY_TEXT && r.thought && !strcmp(g_text, "\n\nFour."),
     "V4 Flash (thinks) with NO thinking field: the prompt-opened thinking cut");
  ok(reply(ab.c_str(), &r, 1) == CHUTES_REPLY_TEXT && !r.thought && strstr(g_text, "template") != NULL,
     "...the same content from Kimi with thinking off: kept (not its shape)");

  group("the answer as shown and kept (geminiAnswerText's rule)");
  char out[GEM_A_MAX];
  reply(R_KIMI, &r);
  ok(chutesAnswerText(&r, 1, g_text, NULL, out, sizeof(out)) > 0 && !strcmp(out, "The capital of Bolivia is Sucre."),
     "cleaned: the leading space gone");
  reply(R_GLM, &r);
  chutesAnswerText(&r, 3, g_text, "Wikipedia: Uruguay", out, sizeof(out));
  ok(!strcmp(out, "The capital of Uruguay is Montevideo.\n\nWikipedia: Uruguay"), "the sources line after a blank one");
  ChutesReply cut = r;
  strcpy(cut.finish, "length");
  chutesAnswerText(&cut, 3, "Partly answered", NULL, out, sizeof(out));
  ok(!strcmp(out, "Partly answered (cut short)"), "finish \"length\" with text: \" (cut short)\" (Gemini's MAX_TOKENS)");
  cut.finish[0] = '\0';
  cut.textTruncated = true;
  chutesAnswerText(&cut, 3, "Too long", NULL, out, sizeof(out));
  ok(!strcmp(out, "Too long (cut short)"), "a content cut to fit: \" (cut short)\"");
  ok(chutesAnswerText(&r, 3, "**Bold** \xE2\x80\x94 ok", NULL, out, sizeof(out)) > 0 && !strcmp(out, "Bold - ok"),
     "the screen's ASCII: markdown out, a dash folded");

  group("no answer under a 200");
  reply(R_KIMI_LENGTH, &r);
  chutesNoTextMessage(&r, 2, out, sizeof(out));
  ok(!strcmp(out, "Kimi K2.6 careful ran out of room before answering - ask for less"), "length: out of room");
  ChutesReply e;
  memset(&e, 0, sizeof(e));
  e.kind = CHUTES_REPLY_EMPTY;
  strcpy(e.finish, "content_filter");
  chutesNoTextMessage(&e, 3, out, sizeof(out));
  ok(!strcmp(out, "GLM 5.1 declined to answer"), "content_filter: declined");
  strcpy(e.finish, "stop");
  chutesNoTextMessage(&e, 0, out, sizeof(out));
  ok(!strcmp(out, "DeepSeek V4 Flash sent an empty answer"), "\"stop\" with nothing in it: empty");
  /* The WiPhone's review (2026-10-19, COVEY's rule): ONLY a model that said "stop" with nothing in
   * it "sent an empty answer". A failed finish or none said is the ladder's busy (never DONE); the
   * words, should a caller show them anyway, are chutesFailText's for a 200. */
  bool busyWords = true;
  for (const char* f : { "", "abort", "error" }) {
    strcpy(e.finish, f);
    chutesNoTextMessage(&e, 0, out, sizeof(out));
    if (strcmp(out, "Chutes is busy right now - try again in a minute") != 0) {
      printf("    finish \"%s\": \"%s\"\n", f, out);
      busyWords = false;
    }
  }
  ok(busyWords, "none said (null), \"abort\", \"error\": never \"sent an empty answer\" - the busy words");
  e.kind = CHUTES_REPLY_BAD;
  chutesNoTextMessage(&e, 0, out, sizeof(out));
  ok(!strcmp(out, "Could not read Chutes' answer") && (chutesNoTextMessage(NULL, 0, out, sizeof(out)), !strcmp(out, "Could not read Chutes' answer")),
     "a body that does not read");

  /* The X4's review (2026-10-19, COVEY's FINISHES): only stop, length and content_filter END an
   * answer. An "abort" (vLLM, SGLang) or "error" (newer vLLM) is a generation that FAILED: after text
   * it is cut short - never shown or kept (and sent back as context) as a whole answer. */
  group("a finish that does not end an answer (the X4's review, 2026-10-19)");
  ok(reply(finishBody("The three causes were: first, the", "abort").c_str(), &r, 0) == CHUTES_REPLY_TEXT &&
     chutesAnswerText(&r, 0, g_text, NULL, out, sizeof(out)) > 0 &&
     !strcmp(out, "The three causes were: first, the (cut short)"),
     "\"abort\" after text: \" (cut short)\" - never shown or kept as a whole answer");
  reply(finishBody("Partly said", "error").c_str(), &r, 1);
  chutesAnswerText(&r, 1, g_text, "Wikipedia: Uruguay", out, sizeof(out));
  ok(!strcmp(out, "Partly said (cut short)\n\nWikipedia: Uruguay"), "\"error\" after text: cut short too, the sources line after");
  reply(finishBody("Partly said", "tool_calls").c_str(), &r, 1);
  chutesAnswerText(&r, 1, g_text, NULL, out, sizeof(out));
  ok(!strcmp(out, "Partly said (cut short)"), "any finish not stop/length/content_filter: cut short (no tool is ever sent)");
  reply(finishBody("The first step is to", "content_filter").c_str(), &r, 3);
  chutesAnswerText(&r, 3, g_text, NULL, out, sizeof(out));
  ok(!strcmp(out, "The first step is to (stopped - GLM 5.1 declined to finish)"),
     "content_filter after text: COVEY's declined words, with the label of the preset that answered");
  chutesAnswerText(&r, 1, g_text, "Wikipedia: Uruguay", out, sizeof(out));
  ok(!strcmp(out, "The first step is to (stopped - Kimi K2.6 declined to finish)\n\nWikipedia: Uruguay"),
     "...before the sources line, as the cut note is - and the preset ANSWERING is named (a fallback's)");
  chutesAnswerText(&r, CHUTES_GEMINI, g_text, NULL, out, sizeof(out));
  ok(!strcmp(out, "The first step is to (stopped - the model declined to finish)"), "...no preset: \"the model\"");
  ChutesReply big;
  memset(&big, 0, sizeof(big));
  strcpy(big.finish, "content_filter");
  std::string longText(GEM_A_MAX * 2, 'a');
  const size_t nb = chutesAnswerText(&big, 0, longText.c_str(), "Wikipedia: Uruguay", out, sizeof(out));
  ok(nb > 0 && nb == strlen(out) && nb < sizeof(out) &&
     strstr(out, "(stopped - DeepSeek V4 Flash declined to finish)\n\nWikipedia: Uruguay") != NULL,
     "a content past the cap: the note and the sources line still fit, the text cut");
  char tiny[24];
  ok(chutesAnswerText(&big, 0, "x", NULL, tiny, sizeof(tiny)) == 0 && tiny[0] == '\0',
     "a buffer too small for the note: 0 and \"\" - never half a note, never past the end");
  reply(finishBody("Paris is the capital.", "stop").c_str(), &r, 1);
  chutesAnswerText(&r, 1, g_text, NULL, out, sizeof(out));
  ok(!strcmp(out, "Paris is the capital."), "stop: whole, no note");
  reply(finishBody("Paris is the capital.", NULL).c_str(), &r, 1);
  chutesAnswerText(&r, 1, g_text, NULL, out, sizeof(out));
  ok(!strcmp(out, "Paris is the capital."), "a null finish: whole, as before");
  ok(reply(finishBody(NULL, "abort").c_str(), &r, 0) == CHUTES_REPLY_EMPTY && !strcmp(r.finish, "abort"),
     "\"abort\" before any text: EMPTY (the ladder makes it busy - below)");

  /* The WiPhone's review (2026-10-19, COVEY's clean_text): the parser's TEXT is any byte that is not
   * a blank, and the screen drops an emoji, a control character, a markdown marker. Judged before
   * the ladder (ai_net.cpp's aiRunChutes; check_ai_glue contract 16), so an emoji alone with
   * "abort" is busy - it said "sent an empty answer" with no retry. */
  group("\"any text\" is what the screen shows (chutesAnswerShown)");
  bool none = true;
  for (const char* c : { "\\ud83d\\ude00", " \\ud83d\\ude00\\n", "\\u0007", "**", "```", "\\n# \\n> \\n", "\\u200b" }) {
    for (const char* src : { (const char*)NULL, "Wikipedia: Uruguay" }) {
      reply(finishBody(c, "abort").c_str(), &r, 0);
      const int parsed = r.kind;
      const size_t n = chutesAnswerShown(&r, 0, g_text, src, out, sizeof(out));
      if (parsed != CHUTES_REPLY_TEXT || n != 0 || out[0] != '\0' || r.kind != CHUTES_REPLY_EMPTY ||
          strcmp(r.finish, "abort") != 0) {
        printf("    content %s (sources %s): parsed %d, %u bytes, kind %d\n", c, src ? src : "none", parsed,
               (unsigned)n, r.kind);
        none = false;
      }
    }
  }
  ok(none, "an emoji, a control character, a zero-width space or markdown alone (a sources line or not): the "
           "parser's TEXT, the screen's nothing - EMPTY, its finish kept, \"\" out");
  reply(finishBody("Paris is the capital.", "stop").c_str(), &r, 1);
  char viaText[GEM_A_MAX];
  chutesAnswerText(&r, 1, g_text, "Wikipedia: France", viaText, sizeof(viaText));
  ok(chutesAnswerShown(&r, 1, g_text, "Wikipedia: France", out, sizeof(out)) == strlen(viaText) &&
     !strcmp(out, viaText) && r.kind == CHUTES_REPLY_TEXT, "a text the screen shows: chutesAnswerText's answer, TEXT");
  reply(finishBody("Paris \\ud83d\\ude00", "content_filter").c_str(), &r, 3);
  ok(chutesAnswerShown(&r, 3, g_text, NULL, out, sizeof(out)) > 0 && r.kind == CHUTES_REPLY_TEXT &&
     !strcmp(out, "Paris (stopped - GLM 5.1 declined to finish)"), "...an emoji beside words: the words, the notes kept");
  bool untouched = true;
  for (const char* b : { R_KIMI_LENGTH, R_503, R_429_HTML }) {
    reply(b, &r, 2);
    const int k = r.kind;
    strcpy(out, "stale");
    if (chutesAnswerShown(&r, 2, g_text, NULL, out, sizeof(out)) != 0 || out[0] != '\0' || r.kind != k) {
      untouched = false;
    }
  }
  strcpy(out, "stale");
  ok(untouched && chutesAnswerShown(NULL, 0, g_text, NULL, out, sizeof(out)) == 0 && out[0] == '\0',
     "EMPTY, ERROR, BAD (or none): \"\" out, the reply untouched - no stale answer");
  reply(finishBody("Paris is the capital.", "stop").c_str(), &r, 1);
  ok(chutesAnswerShown(&r, 1, g_text, NULL, NULL, 0) == 0 && r.kind == CHUTES_REPLY_TEXT,
     "no buffer to build it in: nothing judged - a TEXT stays TEXT");
}

// ── the ladder ───────────────────────────────────────────────────────────────────────────

static ChutesReply errReply(bool model) {
  ChutesReply r;
  memset(&r, 0, sizeof(r));
  r.kind = CHUTES_REPLY_ERROR;
  r.errModel = model;
  return r;
}

static void testLadder() {
  group("the ladder: rule 0, 1, 2 (an answer; the key; the plan)");
  ChutesLadder l;
  uint32_t d = 0;
  const char* st = NULL;
  ChutesReply ok200;
  reply(R_KIMI, &ok200);
  ok(chutesLadderStart(&l, 1, 0, true, 1000) == GEM_STEP_RETRY && l.cur == 1 && !l.onFallback && l.retryTimeouts,
     "start: the preset asked for, the retry setting's default on");
  ok(chutesLadderNext(&l, 200, GEM_NET_OK, &ok200, 2000, &d, &st) == GEM_STEP_DONE && l.attempts == 1, "200: DONE");
  ChutesReply bad;
  reply(R_429_HTML, &bad);
  chutesLadderStart(&l, 1, 0, true, 0);
  ok(chutesLadderNext(&l, 200, GEM_NET_OK, &bad, 1, &d, &st) == GEM_STEP_DONE, "200 that does not read: DONE (\"Could not read\")");
  reply(R_KIMI_LENGTH, &bad);
  chutesLadderStart(&l, 2, 0, true, 0);
  ok(chutesLadderNext(&l, 200, GEM_NET_OK, &bad, 1, &d, &st) == GEM_STEP_DONE, "200, all thinking: DONE - never retried blindly");
  ChutesReply er = errReply(false);
  for (int code : { 401, 403, 402 }) {
    chutesLadderStart(&l, 0, 0, true, 0);
    const int step = chutesLadderNext(&l, code, GEM_NET_OK, &er, 1, &d, &st);
    char what[80];
    snprintf(what, sizeof(what), "%d: FAIL at once - no retry, no fallback", code);
    ok(step == GEM_STEP_FAIL && !l.onFallback && l.cur == 0 && d == 0, what);
  }

  group("rule 3: a model that has left Chutes");
  ChutesReply gone = errReply(true);
  chutesLadderStart(&l, 0, 0, false, 0);
  ok(chutesLadderNext(&l, 404, GEM_NET_OK, &gone, 1, &d, &st) == GEM_STEP_RETRY && l.cur == 1 && l.onFallback &&
     d == 0 && (l.goneMask & 1u) && st && !strcmp(st, "DeepSeek V4 Flash is not offered any more - asking Kimi K2.6..."),
     "404: remembered gone, the chain's next at once, and said so");
  ok(chutesLadderNext(&l, 200, GEM_NET_OK, &ok200, 2, &d, &st) == GEM_STEP_DONE, "...which answers");
  char lab[GEM_LABEL_MAX];
  chutesAnswerLabel(&l, lab, sizeof(lab));
  ok(!strcmp(lab, "DeepSeek V4 Flash was not found - Kimi K2.6 answered"), "the label under the answer says what happened");
  const uint32_t mask = l.goneMask;
  chutesLadderStart(&l, 0, mask, false, 0);
  ok(l.cur == 1 && l.onFallback && l.fbWhy == CHUTES_FB_GONE && !strcmp(l.note, "DeepSeek V4 Flash is not offered any more - asking Kimi K2.6..."),
     "the next question: straight to the fallback (no request to a gone model)");
  ok(chutesLadderNext(&l, 503, GEM_NET_OK, &er, 1, &d, &st) == GEM_STEP_RETRY && d == CHUTES_RETRY_GAP_MS &&
     chutesLadderNext(&l, 503, GEM_NET_OK, &er, 3000, &d, &st) == GEM_STEP_FAIL,
     "...and that start WAS its one fallback: busy twice there, FAIL (no third preset)");
  chutesLadderStart(&l, 0, (1u << 0) | (1u << 1) | (1u << 3), false, 0);
  ok(l.cur == 0 && !l.onFallback, "every one of its chain gone too: asked all the same (it may be back)");
  chutesLadderStart(&l, 0, 0, false, 0);
  chutesLadderNext(&l, 404, GEM_NET_OK, &gone, 1, &d, &st);
  ok(l.cur == 1 && chutesLadderNext(&l, 404, GEM_NET_OK, &gone, 2, &d, &st) == GEM_STEP_FAIL && l.cur == 1 &&
     l.goneMask == 7u, "the fallback gone too: FAIL - never a third preset (glm is not asked), both remembered "
                       "(and kimi_think with kimi: one model)");
  /* Gone is the MODEL (review 2026-10-18): kimi and kimi_think send one id. */
  chutesLadderStart(&l, 1, 0, false, 0);
  ok(chutesLadderNext(&l, 404, GEM_NET_OK, &gone, 1, &d, &st) == GEM_STEP_RETRY && l.cur == 0 &&
     l.goneMask == ((1u << 1) | (1u << 2)), "kimi 404: kimi_think is marked gone with it");
  ChutesLadder twin;
  chutesLadderStart(&twin, 2, l.goneMask, false, 0);
  ok(twin.cur == 0 && twin.onFallback && twin.fbWhy == CHUTES_FB_GONE &&
     !strcmp(twin.note, "Kimi K2.6 careful is not offered any more - asking DeepSeek V4 Flash..."),
     "...so the next kimi_think question starts at V4 Flash - no request sure to 404");
  chutesLadderStart(&l, 2, 1u << 0, false, 0);
  ok(chutesLadderNext(&l, 404, GEM_NET_OK, &gone, 1, &d, &st) == GEM_STEP_FAIL && l.attempts == 1 && l.goneMask == 7u,
     "kimi_think 404 with V4 Flash gone: FAIL at once - never its twin kimi (the same id)");
  chutesLadderStart(&l, 3, 0, false, 0);
  ok(chutesLadderNext(&l, 400, GEM_NET_OK, &gone, 1, &d, &st) == GEM_STEP_RETRY && l.cur == 1,
     "400 whose detail names the model: gone too");
  chutesLadderStart(&l, 3, 0, false, 0);
  ok(chutesLadderNext(&l, 400, GEM_NET_OK, &er, 1, &d, &st) == GEM_STEP_FAIL, "400 for anything else: FAIL");
  /* The X4's review (2026-10-19): a model gone on the question's LAST attempt is remembered too - it
   * said "not offered any more" and forgot it, so the next question spent its one fallback there. */
  ChutesReply busyE = errReply(false);
  chutesLadderStart(&l, 0, 0, false, 0);
  ok(chutesLadderNext(&l, 503, GEM_NET_OK, &busyE, 100, &d, &st) == GEM_STEP_RETRY &&
     chutesLadderNext(&l, 503, GEM_NET_OK, &busyE, 2700, &d, &st) == GEM_STEP_RETRY && l.cur == 1 &&
     chutesLadderNext(&l, 503, GEM_NET_OK, &busyE, 3300, &d, &st) == GEM_STEP_RETRY && l.cur == 1,
     "v4flash 503, its retry 503, kimi 503: kimi's retry is the 4th attempt");
  ok(chutesLadderNext(&l, 404, GEM_NET_OK, &gone, 5900, &d, &st) == GEM_STEP_FAIL && l.attempts == CHUTES_MAX_ATTEMPTS,
     "kimi 404 on it: FAIL (the cap)");
  char gb[160];
  chutesFailText(404, GEM_NET_OK, &gone, &l, FAKE_KEY, gb, sizeof(gb));
  ok(!strcmp(gb, "Kimi K2.6 is not offered by Chutes any more") && l.goneMask == ((1u << 1) | (1u << 2)),
     "...says Kimi is not offered any more AND remembers it (kimi_think with it: one model)");
  ok(chutesFallbackOf(0, l.goneMask) == 3, "...so the next V4 Flash question's one fallback is GLM, not Kimi again");
  chutesLadderStart(&l, 3, 0, false, 0);
  l.attempts = CHUTES_MAX_ATTEMPTS - 1;
  ok(chutesLadderNext(&l, 400, GEM_NET_OK, &gone, 1, &d, &st) == GEM_STEP_FAIL && l.goneMask == (1u << 3),
     "a 400 naming the model on the last attempt: remembered the same");

  group("rules 4 + 5: busy - one retry inside the budget, then ONE fallback");
  ChutesReply e503;
  reply(R_503, &e503);
  chutesLadderStart(&l, 3, 0, false, 10000);
  ok(chutesLadderNext(&l, 503, GEM_NET_OK, &e503, 11000, &d, &st) == GEM_STEP_RETRY && l.cur == 3 &&
     d == CHUTES_RETRY_GAP_MS && st && !strcmp(st, "Chutes is busy - trying again..."), "503: the same preset, 2.5 s on");
  ok(chutesLadderNext(&l, 503, GEM_NET_OK, &e503, 14000, &d, &st) == GEM_STEP_RETRY && l.cur == 1 && l.onFallback &&
     d == GEM_LITE_DELAY_MS && !strcmp(st, "GLM 5.1 is busy - asking Kimi K2.6..."), "again: glm -> kimi");
  ok(chutesLadderNext(&l, 503, GEM_NET_OK, &e503, 15000, &d, &st) == GEM_STEP_RETRY && l.cur == 1 && d == CHUTES_RETRY_GAP_MS,
     "the fallback gets its own one retry");
  ok(chutesLadderNext(&l, 500, GEM_NET_OK, &e503, 18000, &d, &st) == GEM_STEP_FAIL && l.attempts == 4,
     "...then FAIL: never a second fallback (kimi's chain is not walked on)");
  chutesLadderStart(&l, 0, 0, false, 0);
  ok(chutesLadderNext(&l, 429, GEM_NET_OK, &bad, 27500, &d, &st) == GEM_STEP_RETRY && d == CHUTES_RETRY_GAP_MS && !l.onFallback,
     "429 at 27.5 s: the retry ends inside the 30 s budget - retried");
  chutesLadderStart(&l, 0, 0, false, 0);
  ok(chutesLadderNext(&l, 429, GEM_NET_OK, &bad, 27501, &d, &st) == GEM_STEP_RETRY && l.cur == 1 && l.onFallback,
     "429 at 27.501 s: it would end past the budget - no retry, the fallback");
  chutesLadderStart(&l, 2, 0, false, 0);
  ok(chutesLadderNext(&l, 502, GEM_NET_OK, &bad, 1, &d, &st) == GEM_STEP_RETRY &&
     chutesLadderNext(&l, 504, GEM_NET_OK, &bad, 2, &d, &st) == GEM_STEP_RETRY && l.cur == 0,
     "502/504 are busy too: kimi_think -> v4flash");
  ChutesReply mid = errReply(false);
  chutesLadderStart(&l, 1, 0, false, 0);
  ok(chutesLadderNext(&l, 200, GEM_NET_OK, &mid, 1, &d, &st) == GEM_STEP_RETRY && d == CHUTES_RETRY_GAP_MS,
     "a 200 that carries an error: busy, retried");
  /* The X4's review (2026-10-19, COVEY's FINISHES): a 200 with no text whose finish does not end an
   * answer ("abort": the engine gave up before a word) is a server failure - never "sent an empty
   * answer". */
  ChutesReply ab;
  reply(finishBody(NULL, "abort").c_str(), &ab, 0);
  chutesLadderStart(&l, 0, 0, true, 0);
  ok(chutesLadderNext(&l, 200, GEM_NET_OK, &ab, 1000, &d, &st) == GEM_STEP_RETRY && l.cur == 0 &&
     d == CHUTES_RETRY_GAP_MS && st && !strcmp(st, "Chutes is busy - trying again..."),
     "200, no text, \"abort\": the busy retry (rule 5), never DONE");
  ok(chutesLadderNext(&l, 200, GEM_NET_OK, &ab, 4000, &d, &st) == GEM_STEP_RETRY && l.cur == 1 && l.onFallback &&
     l.fbWhy == CHUTES_FB_BUSY, "...then the fallback");
  ok(chutesLadderNext(&l, 200, GEM_NET_OK, &ab, 5000, &d, &st) == GEM_STEP_RETRY &&
     chutesLadderNext(&l, 200, GEM_NET_OK, &ab, 8000, &d, &st) == GEM_STEP_FAIL, "...its own retry, then FAIL");
  char fb[160];
  chutesFailText(200, GEM_NET_OK, &ab, &l, FAKE_KEY, fb, sizeof(fb));
  ok(!strcmp(fb, "Chutes is busy right now - try again in a minute"), "...in the 5xx words");
  reply(finishBody("", "error").c_str(), &ab, 1);
  chutesLadderStart(&l, 1, 0, true, 0);
  ok(ab.kind == CHUTES_REPLY_EMPTY && chutesLadderNext(&l, 200, GEM_NET_OK, &ab, 1000, &d, &st) == GEM_STEP_RETRY,
     "\"error\" with \"\": the same");
  for (const char* f : { "stop", "length", "content_filter" }) {
    reply(finishBody(NULL, f).c_str(), &ab, 1);
    chutesLadderStart(&l, 1, 0, true, 0);
    char what[96];
    snprintf(what, sizeof(what), "no text, finish %s: DONE as before (it says why itself)", f);
    ok(chutesLadderNext(&l, 200, GEM_NET_OK, &ab, 1000, &d, &st) == GEM_STEP_DONE, what);
  }
  /* The WiPhone's review (2026-10-19, COVEY's rule): no text and NO finish said - null, no key, no
   * choice at all - is a server failure too. Only "stop" with nothing in it is an empty answer. */
  const std::string noFinish[] = {
    finishBody(NULL, NULL),
    "{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"\"}}]}",
    "{\"choices\":[]}",
  };
  const char* noFinishWhat[] = { "null", "no finish_reason key", "no choice" };
  for (int i = 0; i < 3; i++) {
    reply(noFinish[i].c_str(), &ab, 1);
    chutesLadderStart(&l, 1, 0, true, 0);
    const bool retried = ab.kind == CHUTES_REPLY_EMPTY && ab.finish[0] == '\0' &&
                         chutesLadderNext(&l, 200, GEM_NET_OK, &ab, 1000, &d, &st) == GEM_STEP_RETRY && l.cur == 1 &&
                         d == CHUTES_RETRY_GAP_MS && st && !strcmp(st, "Chutes is busy - trying again...");
    const bool fellBack = chutesLadderNext(&l, 200, GEM_NET_OK, &ab, 4000, &d, &st) == GEM_STEP_RETRY && l.cur == 0 &&
                          l.onFallback && l.fbWhy == CHUTES_FB_BUSY;
    chutesLadderNext(&l, 200, GEM_NET_OK, &ab, 5000, &d, &st);
    const bool failed = chutesLadderNext(&l, 200, GEM_NET_OK, &ab, 8000, &d, &st) == GEM_STEP_FAIL;
    chutesFailText(200, GEM_NET_OK, &ab, &l, FAKE_KEY, fb, sizeof(fb));
    char what[128];
    snprintf(what, sizeof(what), "no text, %s: the busy retry, then the fallback, then FAIL in the busy words - never "
                                 "\"sent an empty answer\"", noFinishWhat[i]);
    ok(retried && fellBack && failed && !strcmp(fb, "Chutes is busy right now - try again in a minute"), what);
  }
  /* ...and "no text" is what the screen shows: the parse, chutesAnswerShown, then the ladder -
   * aiRunChutes' order (check_ai_glue contract 16). */
  char shown[GEM_A_MAX];
  reply(finishBody("\\ud83d\\ude00", "abort").c_str(), &ab, 0);
  chutesAnswerShown(&ab, 0, g_text, NULL, shown, sizeof(shown));
  chutesLadderStart(&l, 0, 0, true, 0);
  ok(chutesLadderNext(&l, 200, GEM_NET_OK, &ab, 1000, &d, &st) == GEM_STEP_RETRY && l.cur == 0 &&
     d == CHUTES_RETRY_GAP_MS && st && !strcmp(st, "Chutes is busy - trying again..."),
     "an emoji alone with \"abort\": the busy retry - it said \"DeepSeek V4 Flash sent an empty answer\"");
  reply(finishBody("\\ud83d\\ude00", NULL).c_str(), &ab, 0);
  chutesAnswerShown(&ab, 0, g_text, NULL, shown, sizeof(shown));
  chutesLadderStart(&l, 0, 0, true, 0);
  ok(chutesLadderNext(&l, 200, GEM_NET_OK, &ab, 1000, &d, &st) == GEM_STEP_RETRY, "...with no finish said: the same");
  reply(finishBody("\\ud83d\\ude00", "stop").c_str(), &ab, 0);
  chutesAnswerShown(&ab, 0, g_text, NULL, shown, sizeof(shown));
  chutesLadderStart(&l, 0, 0, true, 0);
  ok(chutesLadderNext(&l, 200, GEM_NET_OK, &ab, 1000, &d, &st) == GEM_STEP_DONE &&
     (chutesNoTextMessage(&ab, l.cur, shown, sizeof(shown)), !strcmp(shown, "DeepSeek V4 Flash sent an empty answer")),
     "...with \"stop\": DONE, and that IS an empty answer (the model said it was done)");

  group("rule 6: nothing in time - the fallback at once, ONCE (\"Retry if it times out\")");
  chutesLadderStart(&l, 0, 0, false, 0);
  ok(chutesLadderNext(&l, 0, GEM_NET_TIMEOUT, NULL, 120000, &d, &st) == GEM_STEP_RETRY && l.cur == 1 && d == 0 &&
     !strcmp(st, "DeepSeek V4 Flash was slow - asking Kimi K2.6..."), "a timeout: no second wait on it, kimi at once");
  ok(chutesLadderNext(&l, 200, GEM_NET_OK, &ok200, 125000, &d, &st) == GEM_STEP_DONE, "kimi answers");
  chutesAnswerLabel(&l, lab, sizeof(lab));
  ok(!strcmp(lab, "DeepSeek V4 Flash was slow - Kimi K2.6 answered"), "\"<label> was slow - <next> answered\"");
  chutesLadderStart(&l, 3, 0, false, 0);
  chutesLadderNext(&l, 0, GEM_NET_TIMEOUT, NULL, 60000, &d, &st);
  ok(chutesLadderNext(&l, 0, GEM_NET_TIMEOUT, NULL, 120000, &d, &st) == GEM_STEP_FAIL && l.attempts == 2,
     "the fallback slow too: FAIL - never more than 2 presets a question");
  chutesLadderStart(&l, 1, 0, false, 0);
  ok(chutesLadderNext(&l, 0, GEM_NET_LOST, NULL, 5000, &d, &st) == GEM_STEP_RETRY && l.fbWhy == CHUTES_FB_DROPPED &&
     !strcmp(st, "Kimi K2.6 dropped out - asking DeepSeek V4 Flash..."), "a dropped connection: the fallback, said so");
  chutesLadderStart(&l, 1, 0, false, 0);
  l.retryTimeouts = false;
  ok(chutesLadderNext(&l, 0, GEM_NET_TIMEOUT, NULL, 60000, &d, &st) == GEM_STEP_FAIL, "the setting off: FAIL at once");
  bool allFail = true;
  for (int net : { GEM_NET_DNS, GEM_NET_CONNECT, GEM_NET_TLS, GEM_NET_TLS_OTHER, GEM_NET_NOMEM, GEM_NET_TOO_BIG, GEM_NET_BAD_BODY }) {
    chutesLadderStart(&l, 1, 0, false, 0);
    if (chutesLadderNext(&l, 0, net, NULL, 1, &d, &st) != GEM_STEP_FAIL || l.onFallback) {
      printf("    net %d retried\n", net);
      allFail = false;
    }
  }
  ok(allFail, "DNS, connect, TLS, memory, too big, a bad body: FAIL, never the fallback");
  chutesLadderStart(&l, 0, 0, false, 0);
  l.attempts = CHUTES_MAX_ATTEMPTS - 1;
  ok(chutesLadderNext(&l, 503, GEM_NET_OK, &e503, 1, &d, &st) == GEM_STEP_FAIL, "the attempt cap holds");

  group("every preset's label pair fits under an answer (GEM_LABEL_MAX)");
  bool fits = true;
  int longest = 0;
  for (int a = 0; a < CHUTES_PRESETS; a++) {
    for (int why = CHUTES_FB_SLOW; why <= CHUTES_FB_GONE; why++) {
      for (int b = 0; b < CHUTES_PRESETS; b++) {
        if (a == b) continue;
        ChutesLadder x;
        memset(&x, 0, sizeof(x));
        x.onFallback = true;
        x.fbFrom = a;
        x.cur = b;
        x.fbWhy = why;
        const size_t n = chutesAnswerLabel(&x, lab, sizeof(lab));
        if (n >= sizeof(lab)) fits = false;
        if ((int)n > longest) longest = (int)n;
      }
    }
  }
  printf("      the longest: %d of %d\n", longest, GEM_LABEL_MAX - 1);
  ok(fits, "\"Kimi K2.6 careful was not found - DeepSeek V4 Flash answered\" and every other");
}

// ── a topic never spans the two providers (the X4's review, 2026-10-19) ──────────────────

static void addEx(GeminiChat* c, int64_t t, bool topic, bool failed, const char* label) {
  geminiChatAdd(c, t, topic, failed, label, "a question", failed ? "why it failed" : "an answer");
}

static void testTopic() {
  group("who answered, from the label kept under it (chutesAnsweredBy)");
  bool all = true;
  char lab[GEM_LABEL_MAX];
  for (int a = 0; a < CHUTES_PRESETS; a++) {
    ChutesLadder x;
    memset(&x, 0, sizeof(x));
    x.cur = a;
    chutesAnswerLabel(&x, lab, sizeof(lab));
    if (chutesAnsweredBy(lab) != CHUTES_BY_CHUTES) {
      printf("    \"%s\" not Chutes'\n", lab);
      all = false;
    }
    for (int why = CHUTES_FB_SLOW; why <= CHUTES_FB_GONE; why++) {
      for (int b = 0; b < CHUTES_PRESETS; b++) {
        if (a == b) continue;
        x.onFallback = true;
        x.fbFrom = a;
        x.cur = b;
        x.fbWhy = why;
        chutesAnswerLabel(&x, lab, sizeof(lab));
        if (chutesAnsweredBy(lab) != CHUTES_BY_CHUTES) {
          printf("    \"%s\" not Chutes'\n", lab);
          all = false;
        }
      }
    }
  }
  ChutesLadder none;
  memset(&none, 0, sizeof(none));
  none.cur = -1;
  chutesAnswerLabel(&none, lab, sizeof(lab));
  ok(all && !strcmp(lab, "Chutes") && chutesAnsweredBy(lab) == CHUTES_BY_CHUTES,
     "every label chutesAnswerLabel writes - each preset, each fallback pair and reason, \"Chutes\": Chutes'");
  const char* models[] = { GEM_DEFAULT_MODEL, GEM_LITE_MODEL, "gemini-2.5-flash", "gemini-3.1-pro-preview",
                           "models/gemini-flash-latest", "gemma-3-27b-it", "kimi", "glm-5.1", "deepseek-v4-flash",
                           "a.b-c" };
  bool gem = true;
  for (const char* m : models) {
    geminiModelLabel(m, lab, sizeof(lab));
    if (chutesAnsweredBy(lab) != CHUTES_BY_GEMINI) {
      printf("    %s -> \"%s\" not Gemini's\n", m, lab);
      gem = false;
    }
  }
  ok(gem, "every geminiModelLabel - Flash, Flash-Lite, 2.5-Flash, and a gemini.txt model named kimi or glm-5.1 "
          "(\"Kimi\", \"Glm-5.1\": no preset's label): Gemini's");
  ok(chutesAnsweredBy("") == CHUTES_BY_UNKNOWN && chutesAnsweredBy(NULL) == CHUTES_BY_UNKNOWN &&
     chutesAnsweredBy("Flash-Lite - daily Flash limit reached") == CHUTES_BY_UNKNOWN &&
     chutesAnsweredBy("Kimi K3") == CHUTES_BY_UNKNOWN && chutesAnsweredBy("DeepSeek V4 Flash2") == CHUTES_BY_UNKNOWN &&
     chutesAnsweredBy("Flash!") == CHUTES_BY_UNKNOWN,
     "a failed exchange's \"\", an older firmware's Gemini label, a preset renamed since: neither's (sent to neither)");
  ok(chutesChoiceProvider(CHUTES_GEMINI) == CHUTES_BY_GEMINI && chutesChoiceProvider(0) == CHUTES_BY_CHUTES &&
     chutesChoiceProvider(3) == CHUTES_BY_CHUTES && chutesChoiceProvider(CHUTES_NOT_A_CHOICE) == CHUTES_BY_GEMINI,
     "a choice's provider: a preset is Chutes, Gemini is Gemini");

  group("a topic never spans Gemini and Chutes (chutesTopicDue)");
  GeminiChat* c = (GeminiChat*)calloc(1, sizeof(GeminiChat));
  GeminiChat* c2 = (GeminiChat*)calloc(1, sizeof(GeminiChat));
  const int64_t T = 1790000000;                 // 2026-09
  ok(chutesTopicDue(c, T, 0, CHUTES_BY_UNKNOWN) && chutesTopicDue(c, T, CHUTES_GEMINI, CHUTES_BY_GEMINI) &&
     chutesTopicDue(NULL, T, 0, CHUTES_BY_CHUTES), "an empty chat (or none): a topic, whoever is asked");
  addEx(c, T, true, false, "Flash");
  addEx(c, T + 60, false, false, "Flash-Lite");
  ok(!chutesTopicDue(c, T + 120, CHUTES_GEMINI, CHUTES_BY_GEMINI) &&
     !chutesTopicDue(c, T + 120, CHUTES_GEMINI, CHUTES_BY_UNKNOWN),
     "a Gemini topic, Gemini asked: it runs on (this boot, and after a restart: the labels say Gemini)");
  bool cross = true;
  for (int p = 0; p < CHUTES_PRESETS; p++) {
    cross = cross && chutesTopicDue(c, T + 120, p, CHUTES_BY_GEMINI) && chutesTopicDue(c, T + 120, p, CHUTES_BY_UNKNOWN);
  }
  ok(cross, "...any Chutes preset asked: a new topic - Google's answers are never sent to Chutes");
  geminiChatClear(c);
  addEx(c, T, true, false, "DeepSeek V4 Flash");
  addEx(c, T + 60, false, false, "GLM 5.1 was busy - Kimi K2.6 answered");
  ok(chutesTopicDue(c, T + 120, CHUTES_GEMINI, CHUTES_BY_CHUTES) && chutesTopicDue(c, T + 120, CHUTES_GEMINI, CHUTES_BY_UNKNOWN),
     "a Chutes topic, Gemini asked: a new topic - nothing Chutes was asked or answered goes to Google (a restart too)");
  ok(!chutesTopicDue(c, T + 120, 2, CHUTES_BY_CHUTES) && !chutesTopicDue(c, T + 120, 3, CHUTES_BY_UNKNOWN),
     "...another preset asked: the topic runs on (one provider, one key)");
  ok(!chutesTopicDue(c, T + 120, 0, CHUTES_BY_CHUTES), "...switched to Gemini and back before asking: it runs on "
                                                     "(nothing was asked of the other)");
  addEx(c, T + 90, true, true, "");               // this boot: a Gemini question that failed (it started a topic)
  ok(chutesTopicDue(c, T + 120, 0, CHUTES_BY_GEMINI),
     "this boot's last question went to Gemini and failed: Chutes asked now starts a topic (the boot's word)");
  ok(!chutesTopicDue(c, T + 120, 0, CHUTES_BY_UNKNOWN),
     "...after a restart a failed exchange's provider is not known - and nothing of it is ever sent: no topic forced");
  /* The other way (the WiPhone's review, 2026-10-19): a Gemini topic, a Chutes question that failed
   * (it started a topic), back to Gemini. Nothing would be sent - the labels cannot say it - so only
   * the boot's word gives Menu > AI its note and the next answer its "New topic". */
  geminiChatClear(c);
  addEx(c, T, true, false, "Flash");
  addEx(c, T + 60, false, false, "Flash-Lite");
  addEx(c, T + 90, true, true, "");               // this boot: a Chutes question that failed (it started a topic)
  ok(chutesTopicDue(c, T + 120, CHUTES_GEMINI, CHUTES_BY_CHUTES),
     "this boot's last question went to Chutes and failed: Gemini asked now starts a topic (the boot's word, "
     "the other way)");
  ok(!chutesTopicDue(c, T + 120, CHUTES_GEMINI, CHUTES_BY_UNKNOWN) &&
     !chutesTopicDue(c, T + 120, CHUTES_GEMINI, CHUTES_BY_GEMINI),
     "...after a restart (not known), or when it was Gemini's own question: no topic forced");
  geminiChatClear(c);
  addEx(c, T, true, false, "Kimi K2.6");
  addEx(c, T + 60, true, false, "Flash");
  ok(!chutesTopicDue(c, T + 120, CHUTES_GEMINI, CHUTES_BY_UNKNOWN),
     "only what would be sent is judged: Kimi's answer before this topic began is never sent");
  geminiChatClear(c);
  addEx(c, T, true, false, "Flash-Lite - daily Flash limit reached");
  ok(chutesTopicDue(c, T + 60, CHUTES_GEMINI, CHUTES_BY_UNKNOWN) && chutesTopicDue(c, T + 60, 0, CHUTES_BY_UNKNOWN),
     "an exchange whose label neither writes: a new topic for both - it is sent to neither");
  geminiChatClear(c);
  addEx(c, T, true, false, "Flash");
  ok(chutesTopicDue(c, T + GEM_TOPIC_GAP_S + 1, CHUTES_GEMINI, CHUTES_BY_GEMINI) &&
     !chutesTopicDue(c, T + GEM_TOPIC_GAP_S, CHUTES_GEMINI, CHUTES_BY_GEMINI),
     "geminiChatTopicDue's reasons kept: two hours on, a topic; not before");
  geminiChatNewTopic(c);
  ok(chutesTopicDue(c, T + 60, CHUTES_GEMINI, CHUTES_BY_GEMINI), "...New topic: a topic");
  geminiChatClear(c);
  addEx(c, 0, true, false, "GLM 5.1");
  ok(chutesTopicDue(c, 0, CHUTES_GEMINI, CHUTES_BY_UNKNOWN) && !chutesTopicDue(c, 0, 3, CHUTES_BY_UNKNOWN),
     "no trusted clock (no two-hour gap): the provider rule alone");
  /* The chat the 10-18 build left on both phones' cards: it ran ONE topic across a switch, so a topic
   * there can already hold both providers' answers. EVERY exchange that would be sent is judged, not
   * just the newest: judging the newest alone would send Kimi's answer to Google here (the backport
   * review's mutant, 2026-10-19). */
  geminiChatClear(c);
  addEx(c, T, true, false, "Kimi K2.6");
  addEx(c, T + 60, false, false, "Flash");
  ok(chutesTopicDue(c, T + 120, CHUTES_GEMINI, CHUTES_BY_UNKNOWN) &&
     chutesTopicDue(c, T + 120, CHUTES_GEMINI, CHUTES_BY_GEMINI),
     "a topic that already mixes them (Kimi, then Flash), Gemini asked: a new topic - Kimi's answer is never sent");
  geminiChatClear(c);
  addEx(c, T, true, false, "Flash");
  addEx(c, T + 60, false, false, "Kimi K2.6");
  ok(chutesTopicDue(c, T + 120, 1, CHUTES_BY_UNKNOWN) && chutesTopicDue(c, T + 120, 1, CHUTES_BY_CHUTES),
     "...and the mirror (Flash, then Kimi), Kimi asked: a new topic - Google's answer is never sent to Chutes");

  group("...across a restart and a cloned card: the chat as the card holds it");
  geminiChatClear(c);
  addEx(c, T, true, false, "Kimi K2.6 careful");
  addEx(c, T + 30, false, false, "Kimi K2.6 careful was slow - DeepSeek V4 Flash answered");
  std::vector<char> saved(geminiChatSave(c, NULL, 0) + 1);
  geminiChatSave(c, saved.data(), saved.size());
  ok(geminiChatLoad(saved.data(), saved.size() - 1, c2) && c2->n == 2, "the saved chat reads back");
  ok(chutesTopicDue(c2, T + 60, CHUTES_GEMINI, CHUTES_BY_UNKNOWN) && !chutesTopicDue(c2, T + 60, 1, CHUTES_BY_UNKNOWN),
     "a restart: the labels read back still say Chutes - Gemini starts a topic, a preset runs on");
  ok(chutesTopicDue(c2, T + 60, CHUTES_GEMINI, CHUTES_BY_GEMINI),
     "a card cloned from another phone (this boot asked Gemini, the card's topic is Chutes'): a topic all the same");
  free(c);
  free(c2);
}

// ── the words for a failure ───────────────────────────────────────────────────────────────

static std::string fail(int code, int net, const ChutesReply* r, const ChutesLadder* l) {
  char b[256];
  chutesFailText(code, net, r, l, FAKE_KEY, b, sizeof(b));
  return b;
}

static void testFailText() {
  group("a failed question, in the phone's own words");
  ChutesLadder l;
  chutesLadderStart(&l, 3, 0, false, 0);
  ChutesReply e = errReply(false);
  ok(fail(401, 0, &e, &l) == "Chutes refused the key - check chutes.txt" && fail(403, 0, &e, &l) == fail(401, 0, &e, &l),
     "401/403: the key");
  ok(fail(402, 0, &e, &l) == "Chutes: your plan's daily requests or balance are used up - check chutes.ai", "402: the plan");
  ChutesReply g = errReply(true);
  ok(fail(404, 0, &g, &l) == "GLM 5.1 is not offered by Chutes any more" && fail(400, 0, &g, &l) == fail(404, 0, &g, &l),
     "404 / a 400 naming the model: not offered any more");
  ok(fail(503, 0, &e, &l) == "Chutes is busy right now - try again in a minute" && fail(500, 0, &e, &l) == fail(503, 0, &e, &l),
     "5xx: busy");
  uint32_t d0;
  const char* st0;
  ChutesLadder later;
  chutesLadderStart(&later, 0, 0, false, 0);              // Chutes HAS answered this boot
  chutesLadderNext(&later, 429, GEM_NET_OK, NULL, 1, &d0, &st0);
  chutesLadderNext(&later, 429, GEM_NET_OK, NULL, 3000, &d0, &st0);
  chutesLadderNext(&later, 429, GEM_NET_OK, NULL, 4000, &d0, &st0);
  ok(chutesLadderNext(&later, 429, GEM_NET_OK, NULL, 7000, &d0, &st0) == GEM_STEP_FAIL && later.answers429 == 4 &&
     fail(429, 0, NULL, &later) == "Chutes is busy (too many requests) - try again in a minute",
     "four 429s after Chutes has answered this boot: busy - the key is known good");
  ChutesLadder first;
  chutesLadderStart(&first, 0, 0, true, 0);
  uint32_t d;
  const char* st;
  chutesLadderNext(&first, 429, GEM_NET_OK, NULL, 1, &d, &st);
  chutesLadderNext(&first, 429, GEM_NET_OK, NULL, 3000, &d, &st);
  chutesLadderNext(&first, 429, GEM_NET_OK, NULL, 4000, &d, &st);
  ok(chutesLadderNext(&first, 429, GEM_NET_OK, NULL, 7000, &d, &st) == GEM_STEP_FAIL &&
     fail(429, 0, NULL, &first) == "Chutes said too many requests - check the key in chutes.txt",
     "every answer a 429 before Chutes ever answered this boot: the anonymous path - check the key");
  ChutesLadder mixed;
  chutesLadderStart(&mixed, 0, 0, true, 0);
  chutesLadderNext(&mixed, 503, GEM_NET_OK, &e, 1, &d, &st);
  chutesLadderNext(&mixed, 429, GEM_NET_OK, NULL, 3000, &d, &st);
  ok(fail(429, 0, NULL, &mixed).find("check the key") == std::string::npos, "...a 503 among them: it is load, not the key");
  /* Review 2026-10-18: a timeout or a drop is a request Chutes HELD - a key it does not know is
   * answered at once (401, or the anonymous path's 429) - so it breaks the run of 429s. */
  ChutesLadder slow;
  chutesLadderStart(&slow, 0, 0, true, 0);
  chutesLadderNext(&slow, 0, GEM_NET_TIMEOUT, NULL, 120000, &d, &st);
  ok(chutesLadderNext(&slow, 429, GEM_NET_OK, NULL, 121000, &d, &st) == GEM_STEP_FAIL &&
     fail(429, 0, NULL, &slow) == "Chutes is busy (too many requests) - try again in a minute",
     "first boot: V4 Flash timed out, then Kimi's one 429 - busy, not 'check the key'");
  chutesLadderStart(&slow, 0, 0, true, 0);
  chutesLadderNext(&slow, 429, GEM_NET_OK, NULL, 1, &d, &st);
  chutesLadderNext(&slow, 0, GEM_NET_TIMEOUT, NULL, 120000, &d, &st);
  ok(chutesLadderNext(&slow, 429, GEM_NET_OK, NULL, 121000, &d, &st) == GEM_STEP_FAIL &&
     fail(429, 0, NULL, &slow).find("check the key") == std::string::npos,
     "first boot: 429, a timeout on its retry, the fallback's 429 - busy");
  chutesLadderStart(&slow, 1, 0, true, 0);
  chutesLadderNext(&slow, 0, GEM_NET_LOST, NULL, 5000, &d, &st);
  ok(chutesLadderNext(&slow, 429, GEM_NET_OK, NULL, 6000, &d, &st) == GEM_STEP_RETRY &&
     chutesLadderNext(&slow, 429, GEM_NET_OK, NULL, 9000, &d, &st) == GEM_STEP_FAIL &&
     fail(429, 0, NULL, &slow).find("check the key") == std::string::npos, "first boot: a drop, then 429s - busy");
  ok(fail(0, GEM_NET_TIMEOUT, NULL, &l) == "GLM 5.1 did not answer in time" &&
     fail(0, GEM_NET_DNS, NULL, &l) == "No internet - Chutes' name did not resolve" &&
     fail(0, GEM_NET_TLS, NULL, &l) == "Secure connection failed - Chutes' certificate did not verify" &&
     fail(0, GEM_NET_LOST, NULL, NULL) == "The connection dropped before the answer came", "no HTTP answer: the network's words");
  ok(fail(418, 0, &e, &l) == "Chutes error 418" && fail(301, 0, &e, NULL) == "Chutes error 301",
     "anything else: its code (a redirect is never followed - it is just a code)");

  group("🛑 no Chutes text ever, no key ever");
  const std::string b402 = r402();
  ChutesReply r;
  chutesParseReply(1, b402.c_str(), b402.size(), g_text, sizeof(g_text), &r);
  const std::string t = fail(402, 0, &r, &l);
  ok(t.find(FAKE_WALLET) == std::string::npos && t.find("tao") == std::string::npos && t.find("Quota") == std::string::npos &&
     strstr(g_text, FAKE_WALLET) == NULL, "402's wallet: in no text, in no buffer");
  bool clean = true;
  for (int code : { 0, 200, 400, 401, 402, 403, 404, 418, 429, 500, 503 }) {
    for (int net = GEM_NET_OK; net <= GEM_NET_BAD_BODY; net++) {
      if (!noKey(fail(code, net, &r, &l))) clean = false;
    }
  }
  ok(clean, "no failure text holds the key");
  /* A key that could only get in through a label would still be scrubbed: the scrub runs last. */
  char sc[200];
  snprintf(sc, sizeof(sc), "x %s y", FAKE_KEY);
  geminiScrub(sc, sizeof(sc), FAKE_KEY);
  ok(!strcmp(sc, "x [key] y"), "geminiScrub takes a Chutes key out like a Gemini one");
}

// ── the roots ────────────────────────────────────────────────────────────────────────────

static int b64v(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static std::vector<uint8_t> derOf(const char* pem, int nth) {
  std::vector<uint8_t> der;
  const char* p = pem;
  for (int i = 0; i <= nth; i++) {
    p = strstr(p, "-----BEGIN CERTIFICATE-----\n");
    if (!p) return der;
    p += strlen("-----BEGIN CERTIFICATE-----\n");
  }
  const char* end = strstr(p, "-----END CERTIFICATE-----");
  uint32_t acc = 0;
  int bits = 0;
  for (; p < end; p++) {
    const int v = b64v(*p);
    if (v < 0) continue;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      der.push_back((uint8_t)((acc >> bits) & 0xFF));
    }
  }
  return der;
}

static std::string sha256Hex(const std::vector<uint8_t>& d) {
  uint8_t h[32];
  bsSha256(d.data(), d.size(), h);
  std::string s;
  char b[4];
  for (int i = 0; i < 32; i++) {
    snprintf(b, sizeof(b), i ? ":%02X" : "%02X", h[i]);
    s += b;
  }
  return s;
}

static void testRoots() {
  group("the Chutes root SET, by SHA-256 (godaddy_roots.h, isrg_roots.h, gts_roots.h)");
  /* ai_net.cpp's AI_CHUTES_ROOTS_PEM is exactly these seven macros, in this order
   * (tests/check_ai_glue.py pins the define); this is their bytes. ISRG Root YR / YE since the
   * review (2026-10-18): a Let's Encrypt renewal served with the short Gen-Y chain anchors there. */
  static const char SET[] = GODADDY_ROOT_G2_PEM ISRG_ROOT_X1_PEM ISRG_ROOT_X2_PEM ISRG_ROOT_YR_PEM ISRG_ROOT_YE_PEM
                            GTS_ROOT_R1_PEM GTS_ROOT_R4_PEM;
  static const char* const WANT[] = {
    "45:14:0B:32:47:EB:9C:C8:C5:B4:F0:D7:B5:30:91:F7:32:92:08:9E:6E:5A:63:E2:74:9D:D3:AC:A9:19:8E:DA",  // GoDaddy G2
    "96:BC:EC:06:26:49:76:F3:74:60:77:9A:CF:28:C5:A7:CF:E8:A3:C0:AA:E1:1A:8F:FC:EE:05:C0:BD:DF:08:C6",  // ISRG X1
    "69:72:9B:8E:15:A8:6E:FC:17:7A:57:AF:B7:17:1D:FC:64:AD:D2:8C:2F:CA:8C:F1:50:7E:34:45:3C:CB:14:70",  // ISRG X2
    "E5:7B:7E:6F:15:0C:41:91:02:E8:D5:C0:55:72:9F:F9:67:B9:D1:A8:29:BF:00:CE:C8:9C:A6:04:EB:F4:A8:6F",  // ISRG Root YR
    "E1:4F:FC:AD:5B:00:25:73:10:06:CA:A4:3A:12:1A:22:D8:E9:70:0F:4F:B9:CF:85:2F:02:A7:08:AA:5D:56:66",  // ISRG Root YE
    "D9:47:43:2A:BD:E7:B7:FA:90:FC:2E:6B:59:10:1B:12:80:E0:E1:C7:E4:E4:0F:A3:C6:88:7F:FF:57:A7:F4:CF",  // GTS R1
    "34:9D:FA:40:58:C5:E2:63:12:3B:39:8A:E7:95:57:3C:4E:13:13:C8:3F:E6:8F:93:55:6C:D5:E8:03:1B:3C:7D",  // GTS R4
  };
  static const char* const NAMES[] = { "Go Daddy Root CA - G2", "ISRG Root X1", "ISRG Root X2", "ISRG Root YR",
                                       "ISRG Root YE", "GTS Root R1", "GTS Root R4" };
  for (int i = 0; i < 7; i++) {
    const std::string h = sha256Hex(derOf(SET, i));
    char what[96];
    snprintf(what, sizeof(what), "%s: SHA-256 %.8s...%s", NAMES[i], WANT[i], WANT[i] + strlen(WANT[i]) - 8);
    ok(h == WANT[i], what);
  }
  ok(derOf(SET, 7).empty(), "exactly seven certificates (GoDaddy TLS Root CA R1 is in no macOS store: not pinned)");
  ok(derOf(GODADDY_ROOT_G2_PEM, 0).size() == 969, "G2 alone: the 969-byte DER llm.chutes.ai sent (2026-10-18)");
  static const char GEMINI_PAIR[] = GTS_ROOT_R1_PEM GTS_ROOT_R4_PEM;
  ok(derOf(GEMINI_PAIR, 0) == derOf(SET, 5) && derOf(GEMINI_PAIR, 1) == derOf(SET, 6) && derOf(GEMINI_PAIR, 2).empty(),
     "Gemini's pin (AI_ROOTS_PEM = R1 + R4) is the same bytes, moved, not changed");
}

// ── the screen's rows, in the firmware's fonts ────────────────────────────────────────────

static void testScreen() {
  group("the picker (AI_MODELS: Akrobat Bold 20 rows, Akrobat Bold 16 notes, 232 px)");
  Font7 f20, f16, f18;
  fontLoad(&f20, Akrobat_Bold20);
  fontLoad(&f16, Akrobat_Bold16);
  fontLoad(&f18, Akrobat_Bold18);
  ok(f20.ok && f16.ok && f18.ok, "the firmware's glyph tables load");
  const int rowW = 240 - 8;                       // the row's allotted width: lcd width - the menu's leftOffset
  bool fit = true, glyphs = true;
  int scrolls = 0;
  for (int row = 0; row < CHUTES_CHOICES; row++) {
    const int c = chutesChoiceAt(row);
    char title[80];
    snprintf(title, sizeof(title), "%s (in use)", chutesChoiceLabel(c));
    const char* lines[] = { chutesChoiceLabel(c), title, chutesChoiceNeeds(c), chutesChoiceNote(c) };
    for (int k = 0; k < 4; k++) {
      const Font7* f = k < 2 ? &f20 : &f16;
      const int w = textWidth(f, lines[k]);
      if (w > rowW && k < 3) {
        printf("    %d px: \"%s\"\n", w, lines[k]);
        fit = false;
      } else if (w > rowW) {
        printf("      %d px, scrolls when selected (COVEY's words, kept): \"%s\"\n", w, lines[k]);
        scrolls++;
      }
      glyphs = glyphs && allGlyphs(f, lines[k]);
    }
  }
  ok(fit, "every label, \"<label> (in use)\" and \"needs chutes.txt\" fits its row: never cut");
  ok(scrolls <= 2, "COVEY's notes: all but two fit; those two scroll on the selected row (the firmware's marquee)");
  ok(glyphs, "...no hollow boxes");

  group("Menu > AI's rows and notes (Akrobat Bold 20: 220 px, a note at most 8 rows)");
  bool rows = true;
  char b[256];
  for (int row = 0; row < CHUTES_CHOICES; row++) {
    const int c = chutesChoiceAt(row);
    snprintf(b, sizeof(b), "Model: %s", chutesChoiceShort(c));
    if (textWidth(&f20, b) > 240 - 8 - 12) rows = false;
    if (chutesTopicNote(c, b, sizeof(b)) >= sizeof(b) || noteRows(&f20, b, 220) > 8 || !allGlyphs(&f20, b)) rows = false;
    chutesRetryNote(c, b, sizeof(b));
    if (noteRows(&f20, b, 220) > 8 || !allGlyphs(&f20, b)) rows = false;
  }
  ok(rows, "\"Model: <short>\" one row; the topic and retry notes wrap within 8 (and fit app_ai's 256-byte line)");
  chutesTopicNote(CHUTES_GEMINI, b, sizeof(b));
  ok(!strcmp(b, "New topic: Gemini forgets the earlier questions (they stay on screen). A topic also starts by "
                "itself two hours after the last question."), "Gemini's topic note: the words of before");
  chutesTopicNote(2, b, sizeof(b));
  ok(!strcmp(b, "New topic: Kimi careful forgets the earlier questions (they stay on screen). A topic also starts "
                "by itself two hours after the last question, and on a change to Gemini."),
     "a preset's: a change to Gemini starts one too (the X4's review, 2026-10-19)");
  printf("      the provider note: %d rows\n", noteRows(&f20, CHUTES_TOPIC_SWITCH, 220));
  ok(noteRows(&f20, CHUTES_TOPIC_SWITCH, 220) <= 3 && allGlyphs(&f20, CHUTES_TOPIC_SWITCH) &&
     strstr(CHUTES_TOPIC_SWITCH, "new topic") && strstr(CHUTES_TOPIC_SWITCH, "Gemini and Chutes"),
     "Menu > AI's \"The next question starts a new topic: ...\" note: 3 rows at most, no hollow boxes");
  chutesRetryNote(CHUTES_GEMINI, b, sizeof(b));
  ok(!strcmp(b, "Retry if it times out: when Gemini sends nothing in time, ask once more, then Flash-Lite once."),
     "Gemini's retry note: the words of before");
  chutesRetryNote(2, b, sizeof(b));
  ok(!strcmp(b, "Retry if it times out: when Kimi careful sends nothing in time, ask V4 Flash once."),
     "a preset's: its chain's next");
  bool ki = true;
  int n = 0;
  for (const char* const* p = CHUTES_KEYINFO_NOTES; *p; p++, n++) {
    const int r = noteRows(&f20, *p, 220);
    printf("      Key info note %d: %d rows\n", n + 1, r);
    if (r > 8 || !allGlyphs(&f20, *p)) ki = false;
  }
  ok(ki && n == 5, "Key info's five Chutes notes each fit addNoteWrapped's 8 rows");
  bool priv = false, file = false, google = false, topic = false;
  for (const char* const* p = CHUTES_KEYINFO_NOTES; *p; p++) {
    if (strstr(*p, "goes only to Chutes")) priv = true;
    if (strstr(*p, "chutes.txt") && strstr(*p, "API Keys")) file = true;
    if (strstr(*p, "never to Google")) google = true;
    if (strstr(*p, "never spans Gemini and Chutes") && strstr(*p, "starts a new one")) topic = true;
  }
  ok(priv && file, "...the file, its folder, and \"goes only to Chutes\"");
  ok(google && topic, "...\"never to Google\", and a topic never spanning the two (chutesTopicDue makes it true)");
  ok(textWidth(&f18, CHUTES_XFER_LINE) <= 240 - 12 && allGlyphs(&f18, CHUTES_XFER_LINE),
     "the uploader's \"Send gemini.txt or chutes.txt.\" (Akrobat 18, 228 px)");
  const char* xf[] = { chutesXferFound(0), chutesXferFound(3), chutesXferFound(CHUTES_GEMINI) };
  bool xfit = true;
  for (const char* s : xf) {
    printf("      %d px: \"%s\"\n", textWidth(&f18, s), s);
    if (textWidth(&f18, s) > 240 - 12 || !allGlyphs(&f18, s)) xfit = false;
  }
  ok(xfit && strstr(xf[0], "chutes.txt") && !strcmp(xf[0], xf[1]) && strstr(xf[2], "gemini.txt"),
     "the uploader's green line names the file of the model IN USE, and fits (Akrobat 18, 228 px)");

  group("the WAITING box (Akrobat Bold 16, 228 px, the stage in 3 rows)");
  bool st = true;
  int worst = 0;
  for (int a = 0; a < CHUTES_PRESETS; a++) {
    for (int why = CHUTES_FB_SLOW; why <= CHUTES_FB_GONE; why++) {
      // the ladder's own words, through a real fallback, for every preset and reason
      ChutesLadder x;
      chutesLadderStart(&x, a, 0, false, 0);
      uint32_t d;
      const char* s = NULL;
      if (why == CHUTES_FB_SLOW) chutesLadderNext(&x, 0, GEM_NET_TIMEOUT, NULL, 1, &d, &s);
      else if (why == CHUTES_FB_DROPPED) chutesLadderNext(&x, 0, GEM_NET_LOST, NULL, 1, &d, &s);
      else if (why == CHUTES_FB_GONE) chutesLadderNext(&x, 404, GEM_NET_OK, NULL, 1, &d, &s);
      else chutesLadderNext(&x, 503, GEM_NET_OK, NULL, 40000, &d, &s);
      if (!s || !x.onFallback) {
        st = false;
        continue;
      }
      const int r = wrapRows(&f16, s, 228);
      if (r > worst) worst = r;
      if (r > 3 || !allGlyphs(&f16, s)) {
        printf("    %d rows: \"%s\"\n", r, s);
        st = false;
      }
    }
  }
  const char* fixed[] = { "Chutes is busy - trying again...", "Looking up Wikipedia..." };
  for (const char* s : fixed) {
    if (wrapRows(&f16, s, 228) > 3) st = false;
  }
  printf("      the most rows a stage took: %d of 3\n", worst);
  ok(st, "every fallback stage (each preset, each reason) fits 3 rows");
  bool one = true;
  for (int p = 0; p < CHUTES_PRESETS; p++) {
    snprintf(b, sizeof(b), "%s - 150 s", chutesPreset(p)->label);
    if (textWidth(&f16, b) > 228) one = false;
    snprintf(b, sizeof(b), "%s - 150 s - again in 3 s", chutesPreset(p)->label);
    if (textWidth(&f16, b) > 228) {
      // AiApp::drawStatus falls back to the short name for the countdown form
      snprintf(b, sizeof(b), "%s - 150 s - again in 3 s", chutesPreset(p)->shortName);
      printf("      the countdown with the short name: \"%s\" %d px\n", b, textWidth(&f16, b));
      if (textWidth(&f16, b) > 228) one = false;
    }
  }
  ok(one, "\"<label> - N s\" one row for every preset; with \"- again in N s\" the label or its short name fits");
}

int main() {
  testPresets();
  testChoice();
  testConfig();
  testRequest();
  testReply();
  testLadder();
  testTopic();
  testFailText();
  testRoots();
  testScreen();
  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
