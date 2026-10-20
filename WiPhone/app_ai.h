/*
 * app_ai.h - Menu > AI: ask Google's Gemini a question from the keypad, on WiFi, with the
 * owner's own free key on the card (README "AI") - or, since D-175 (2026-10-18), one of four
 * Chutes models with the owner's own Chutes key (/API Keys/chutes.txt; off without it) - and, since
 * D-176 (2026-10-19), with ONE Tavily web search before a question about something recent (the
 * owner's own /API Keys/tavily.txt; off without it). The talking
 * is ai_net.cpp's (a worker of its own, TLS with each provider's roots pinned); the rules are
 * gemini.cpp's and chutes.cpp's (host-tested).
 *
 * SCREENS (one enterState() door, which also drops any claim on predictive text):
 *   TRANSCRIPT  the saved chat, paged like the Files viewer (bookLayoutPage + bookRenderRun over
 *               ONE PSRAM buffer): "> question" in blue, the answer in white, under it the model
 *               that answered ("Flash") in grey, a grey "New topic" line where a topic starts.
 *               Opens on the newest page. Up/Down page; OK asks; Select = the menu; Back leaves.
 *   MENU        Ask a question / [Ask with web search] / Model: <short> / Web search: <mode> / New
 *               topic / Clear chat... / Key info / Retry if it times out: On|Off (OK flips it; NVS -
 *               ai_net.h aiRetryTimeouts; on by default). D-176: "Web search: Off|Auto|Always" (OK
 *               cycles it; NVS ai_web; Auto by default) says "needs tavily.txt" without a usable key
 *               (OK opens Key info, nothing saved); "Ask with web search" (only with a key and not
 *               Off) opens COMPOSE for one question that searches whatever Auto would say; a last
 *               note counts today's searches.
 *               When the model in use is the other provider than the topic's (the X4's review,
 *               2026-10-19: a topic never spans Gemini and Chutes), a first note says the next
 *               question starts a new topic (ai_net.h aiTopicDue).
 *   MODELS      (D-175) the five choices - DeepSeek V4 Flash, Kimi K2.6, Kimi K2.6 careful, GLM 5.1,
 *               Gemini Flash - each with COVEY's one-line note, the one in use marked "(in use)" and
 *               selected. OK saves it (NVS ai_model, ai_net.h aiSetModelChoice) and goes back to the
 *               menu - OK on the row in use saves nothing (the default stays the default); a row
 *               whose key file is missing says "needs chutes.txt" (or gemini.txt) and OK on it opens
 *               Key info instead - nothing is saved.
 *   CONFIRM     Clear chat: "Yes, delete it" / "No".
 *   COMPOSE     titled "Ask <short>" ("Ask Gemini", "Ask V4 Flash"; "<short> + web" for a web
 *               question - D-176): the Meshtastic compose screen's
 *               rules: a MultilineTextWidget (500 characters),
 *               predictive text claimed AFTER setInputState, the counter strip. Send = CALL or the
 *               left soft key ("Ask"); END cancels; OK is a typing key and is swallowed; Back is
 *               backspace ("Clear"). A refused question stays typed, the reason in the strip.
 *   WAITING     the transcript with the question at the bottom and a status box: the stage
 *               ("Asking Gemini...", "Google is busy - trying again... 2 s"), the model, the
 *               seconds since it was asked. END cancels (the answer is thrown away when it lands);
 *               Back leaves the app and the question carries on - the answer is in the chat when
 *               the app is opened again.
 *   KEYINFO     where the key comes from and what the phone found: the file, "key: set (N chars)",
 *               the model - NEVER the key or any part of it (screenshots get shared). The first
 *               screen when the choice in use has no key; then, when the other provider's key is
 *               there, a "Model: <short>" row opens MODELS (review 2026-10-18: the way out). Back
 *               leaves the app only with no key at all and no chat. "Add key file over WiFi" starts
 *               the uploader into /API Keys (XFER). Then a Chutes section (D-175): chutes.txt's
 *               state, where a key comes from, the format, that it goes only to Chutes - never to
 *               Google - and that a change between Gemini and Chutes starts a new topic. Then (D-176)
 *               Tavily's: tavily.txt's state (its length), how to get a key, the setting, and the
 *               privacy words (websearch.h WEB_PRIVACY).
 *   XFER        the uploader's address, like Files' upload screen, with the warning that the
 *               phone's own hotspot is OPEN, and "chutes.txt found - Back to use it." (or gemini.txt)
 *               once the model in use has its key. Back stops it and reads the key again.
 *
 * MEMORY. This runs on the loop task, whose 8 KB stack a Books picture page already takes ~7.6 KB
 * of: no big locals here. The transcript, its styles, the page and the line being drawn are
 * members (the app object is PSRAM: WiPhoneApp's operator new).
 */
#ifndef APP_AI_H
#define APP_AI_H

#include "GUI.h"
#include "book_layout.h"
#include "app_gbc_xfer.h"
#include "ai_net.h"

class AiApp : public WindowedApp {
public:
  AiApp(LCD& disp, ControlState& state, HeaderWidget* header, FooterWidget* footer);
  virtual ~AiApp();

  ActionID_t getId() {
    return GUI_APP_AI;
  };
  appEventResult processEvent(EventType event);
  void redrawScreen(bool redrawAll = false);

protected:
  enum AiState { AI_TRANSCRIPT = 0, AI_MENU, AI_CONFIRM, AI_COMPOSE, AI_WAITING, AI_KEYINFO, AI_XFER, AI_MODELS };
  enum { SEG_QUESTION = 0, SEG_ANSWER, SEG_META };
  struct Seg {
    uint32_t start;           // byte offset where this style begins (sorted)
    uint8_t  style;           // SEG_*
  };
  static const int SEG_MAX = 4 * GEM_CHAT_MAX + 8;

  AiState  appState;
  MenuWidget*          menu;
  MultilineTextWidget* textArea;

  char*    text;              // the transcript (PSRAM)
  size_t   textLen, textCap;
  Seg      segs[SEG_MAX];
  int      nSegs;
  uint32_t page;              // where the shown page starts
  uint32_t newestStart;       // where the newest exchange starts (its divider, or its "> " line)
  uint32_t builtGen;          // the chat generation the transcript was built from
  bool     builtPending;      // ...and whether it had a question in flight at the bottom
  BookPage pg;                // the page being drawn / stepped (a member: ~500 bytes)
  char     line[256];         // one line through bookRenderRun

  const char* note;           // a refused question's reason (strip), or NULL
  char     noteBuf[96];
  AiProgress prog;            // the worker's progress, as last read
  uint32_t progGen;           // ...the record last drawn
  uint32_t progSec;           // ...and its seconds
  AiKeyInfo key;              // what the last read of gemini.txt found (never the key itself)
  AiChutesKey ckey;           // ...and of chutes.txt (D-175; never the key either)
  AiWebKey wkey;              // ...and of tavily.txt (D-176; never the key either)
  bool     askWeb;            // D-176: the COMPOSE open now is "Ask with web search"'s
  int      xferSeen;          // files the uploader had taken when the key was last read
  /* The header's title while it says the model ("Ask V4 Flash"): HeaderWidget::setTitle keeps the
   * POINTER, so the words live here. */
  char     title[32];

  void     enterState(AiState s);
  void     clearInputClaims();
  void     freeWidgets();
  MenuWidget* newMenu();
  void     buildMenu();
  void     buildConfirm();
  void     buildKeyInfo();
  void     buildModels();
  bool     choiceUsable();    // the choice in use has a key a question can use (ai_net.h)
  void     buildCompose();
  void     buildTranscript();
  void     segAdd(uint8_t style);
  void     put(const char* s);
  int      styleAt(uint32_t off) const;
  int      viewLines() const;
  int      viewWidth() const;
  int      viewBottom() const;    // where the text area ends (the strip/box under it)
  void     toLastPage();
  void     armTimer();
  bool     transcriptStale() const;
  void     ask();
  void     drawTranscript();
  void     drawStrip(const char* s, uint16_t color);
  int      drawWrapped(SmoothFont* f, const char* s, int x, int y, uint16_t w, int lh, int maxRows);
  void     drawStatus();
  void     drawComposeStrip();
  void     drawXfer();
};

#endif // APP_AI_H
