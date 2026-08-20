/*
 * app_music.cpp — the music player's screen. See app_music.h.
 */

#include "app_music.h"
#include "app_gbc_xfer.h"
#include "Arduino.h"
#include "SD.h"

#define MUSIC_MARGIN       6

#define MUSIC_ROW_NOW      1
#define MUSIC_ROW_ADD      2
#define MUSIC_ROW_FIRST   10        // track i has key MUSIC_ROW_FIRST + i

#define MUSIC_MENU_SHUFFLE 1
#define MUSIC_MENU_REPEAT  2
#define MUSIC_MENU_STOP    3
#define MUSIC_MENU_RESCAN  4

/* The fourth uploader, and still one server. Adding a config is the whole job — see the
 * note at the top of app_gbc_xfer.h about why copying the server was the wrong move.
 *
 * ⚠ `accept` is only a browser hint, not a filter. The server takes whatever is dropped
 * on it, which is exactly why musicIsPlayable() decides what the library shows and
 * wav_reader/mp3_stream both parse their input as untrusted. */
static const XferConfig MUSIC_XFER_CFG = {
  MUSIC_DIR, "Add music", ".mp3,.wav", "tracks", "download.mp3", "WiPhone-Music"
};

// ---------------------------------------------------------------- construction

MusicApp::MusicApp(LCD& disp, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(disp, state, header, footer) {
  log_d("create MusicApp");
  appState = MUSIC_LIB;
  menu = NULL;
  xferClean = false;
  nowClean = false;
  headerTitle[0] = '\0';

  /* The library lives in the player, not here, so opening this screen while a track is
   * playing shows the library that track came from rather than re-reading the card. */
  musicPlayerBegin();
  enterState(MUSIC_LIB);
}

MusicApp::~MusicApp() {
  log_d("destroy MusicApp");
  xferStop();              // in case the app dies with the transfer screen up
  freeWidgets();
  /* Deliberately does NOT stop the music. That is the entire reason the player is a
   * separate module — see music_player.h. */
}

void MusicApp::freeWidgets() {
  if (menu) {
    delete menu;
    menu = NULL;
  }
}

MenuWidget* MusicApp::newMenu(const char* emptyMessage, SmoothFont* font, uint8_t perScreen) {
  MenuWidget* m = new MenuWidget(0, header->height(), lcd.width(),
                                 lcd.height() - header->height() - footer->height(),
                                 emptyMessage, font, perScreen, 8);
  m->setStyle(MenuWidget::DEFAULT_STYLE, WHITE, BLACK, BLACK, GREEN);
  return m;
}

// ---------------------------------------------------------------- lists

void MusicApp::buildLibrary() {
  menu = newMenu(NULL, fonts[AKROBAT_BOLD_18], 9);

  if (musicPlayerCurrent() >= 0) {
    menu->addOption("Now playing...", MUSIC_ROW_NOW, 1);
  }
  menu->addOption("Add music over WiFi...", MUSIC_ROW_ADD, 1);

  const int n = musicPlayerCount();
  for (int i = 0; i < n; i++) {
    const MusicTrack* t = musicPlayerTrack(i);
    if (t) {
      menu->addOption(t->name, (MenuOption::keyType)(MUSIC_ROW_FIRST + i), 1);
    }
  }
  if (n == 0) {
    menu->addOption("(no music yet - add some)", 0, 1);
  }
  const char* err = musicPlayerError();
  if (err) {
    menu->addOption(err, 0, 1);     // e.g. a track that would not decode
  }
}

void MusicApp::buildMenu() {
  menu = newMenu(NULL, fonts[AKROBAT_BOLD_18], 8);

  char line[40];
  snprintf(line, sizeof(line), "Shuffle: %s", musicPlayerShuffle() ? "on" : "off");
  menu->addOption(line, MUSIC_MENU_SHUFFLE, 1);

  const MusicRepeat r = musicPlayerRepeat();
  snprintf(line, sizeof(line), "Repeat: %s",
           r == MUSIC_REPEAT_ONE ? "one" : (r == MUSIC_REPEAT_ALL ? "all" : "off"));
  menu->addOption(line, MUSIC_MENU_REPEAT, 1);

  menu->addOption("Rescan the card", MUSIC_MENU_RESCAN, 1);
  menu->addOption("Stop playing", MUSIC_MENU_STOP, 1);
}

// ---------------------------------------------------------------- drawing

void MusicApp::drawNowPlaying() {
  SmoothFont* font = fonts[AKROBAT_BOLD_18];
  const int top = header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  const int lh = font->height() + 2;

  if (!nowClean) {
    lcd.fillRect(0, top, lcd.width(), bottom - top, BLACK);
    nowClean = true;
  }
  lcd.setTextFont(font);
  lcd.setTextDatum(TL_DATUM);
  int y = top + 4;

  const int cur = musicPlayerCurrent();
  const MusicTrack* t = cur >= 0 ? musicPlayerTrack(cur) : NULL;

  if (!t) {
    lcd.setTextColor(WHITE, BLACK);
    lcd.drawString("Nothing playing", MUSIC_MARGIN, y);
    return;
  }

  /* The title over two lines. Track names are long and the header would run under the
   * clock — the same trap the reader hit, and the reason HeaderWidget is not used here. */
  lcd.setTextColor(TFT_YELLOW, BLACK);
  char l1[32], l2[32];
  const char* nm = t->name;
  const size_t len = strlen(nm);
  const size_t cut = 24;
  if (len <= cut) {
    snprintf(l1, sizeof(l1), "%s", nm);
    l2[0] = '\0';
  } else {
    /* Break on a space if there is one near the cut, so a title splits between words
     * rather than mid-syllable. */
    size_t br = cut;
    for (size_t i = cut; i > cut / 2; i--) {
      if (nm[i] == ' ') {
        br = i;
        break;
      }
    }
    snprintf(l1, sizeof(l1), "%.*s", (int)br, nm);
    snprintf(l2, sizeof(l2), "%.*s", (int)(len - br > 24 ? 24 : len - br), nm + br);
  }
  /* Ellipsized, not silently cut: the two 24-char halves dropped everything
   * past ~48 characters of a track name with no sign anything was missing —
   * and 24 chars of this font already overruns the 240px panel. */
  const uint16_t trackW = lcd.width() - 2 * MUSIC_MARGIN;
  guiDrawEllipsized(lcd, l1, trackW, MUSIC_MARGIN, y);
  y += lh;
  if (l2[0]) {
    guiDrawEllipsized(lcd, l2, trackW, MUSIC_MARGIN, y);
  }
  y += lh + 6;

  /* ⚠ Every line below CHANGES and is redrawn once a second over the top of itself, so
   * each one clears its own strip first.
   *
   * Padding with trailing spaces does not work here and looked like corrupted glyphs on
   * screen: the face is proportional, so a shorter string does not cover the pixels of
   * the longer one it replaces, and a space only advances the cursor rather than
   * painting background. "0:47" over "0:46" left fragments of the old digits standing,
   * and the volume bar — which changes length as well as content — turned into a smear. */
  lcd.setTextColor(WHITE, BLACK);
  char line[48];
  const int clearW = (int)lcd.width() - MUSIC_MARGIN;

  const uint32_t secs = musicPlayerElapsed();
  snprintf(line, sizeof(line), "%s  %lu:%02lu",
           musicPlayerIsPlaying() ? "Playing" : "Paused",
           (unsigned long)(secs / 60), (unsigned long)(secs % 60));
  lcd.fillRect(MUSIC_MARGIN, y, clearW, lh, BLACK);
  lcd.drawString(line, MUSIC_MARGIN, y);
  y += lh + 2;

  snprintf(line, sizeof(line), "Track %d of %d", cur + 1, musicPlayerCount());
  lcd.fillRect(MUSIC_MARGIN, y, clearW, lh, BLACK);
  lcd.drawString(line, MUSIC_MARGIN, y);
  y += lh + 2;

  /* Volume as a bar plus the dB: dB alone means nothing to most people, and a bar alone
   * cannot say how much further it goes. */
  {
    const int v = musicPlayerVolume();
    const int span = MUSIC_VOL_MAX_DB - MUSIC_VOL_MIN_DB;
    int filled = span > 0 ? ((v - MUSIC_VOL_MIN_DB) * 10 + span / 2) / span : 0;
    if (filled < 0) {
      filled = 0;
    }
    if (filled > 10) {
      filled = 10;
    }
    char bar[12];
    for (int k = 0; k < 10; k++) {
      bar[k] = k < filled ? '=' : '-';
    }
    bar[10] = '\0';
    snprintf(line, sizeof(line), "Vol [%s] %d dB", bar, v);
    lcd.fillRect(MUSIC_MARGIN, y, clearW, lh, BLACK);
    lcd.drawString(line, MUSIC_MARGIN, y);
  }
  y += lh + 2;

  {
    const MusicRepeat r = musicPlayerRepeat();
    const uint32_t dry = musicPlayerUnderruns();
    snprintf(line, sizeof(line), "%s%s%s%lu",
             musicPlayerShuffle() ? "shuffle " : "",
             r == MUSIC_REPEAT_ONE ? "repeat one " : (r == MUSIC_REPEAT_ALL ? "repeat all " : ""),
             "gaps:", (unsigned long)dry);
    lcd.setTextColor(TFT_DARKGREY, BLACK);
    lcd.fillRect(MUSIC_MARGIN, y, clearW, lh, BLACK);   // cleared even when empty
    lcd.drawString(line, MUSIC_MARGIN, y);
  }
  y += lh + 8;

  lcd.setTextColor(TFT_DARKGREY, BLACK);
  lcd.drawString("Side buttons, top down:", MUSIC_MARGIN, y);
  y += lh;
  lcd.drawString(" play/pause | next", MUSIC_MARGIN, y);
  y += lh;
  lcd.drawString(" (hold=prev) | vol +/-", MUSIC_MARGIN, y);
  y += lh + 2;
  lcd.drawString("Back: keeps playing", MUSIC_MARGIN, y);
}

void MusicApp::drawXfer() {
  SmoothFont* font = fonts[AKROBAT_BOLD_18];
  const int top = header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  const int lh = font->height() + 2;

  if (!xferClean) {                 // full clear on entry only; the 1 Hz refresh overdraws
    lcd.fillRect(0, top, lcd.width(), bottom - top, BLACK);
    xferClean = true;
  }
  lcd.setTextFont(font);
  lcd.setTextDatum(TL_DATUM);
  int y = top + 2;

  if (!xferOn()) {
    lcd.setTextColor(WHITE, BLACK);
    lcd.drawString("Put .mp3 or .wav files", MUSIC_MARGIN, y); y += lh;
    lcd.drawString("on the SD card from a", MUSIC_MARGIN, y); y += lh;
    lcd.drawString("computer:", MUSIC_MARGIN, y); y += lh + 4;
    lcd.drawString("1. Join the same WiFi", MUSIC_MARGIN, y); y += lh;
    lcd.drawString("2. Press OK to start", MUSIC_MARGIN, y); y += lh;
    lcd.drawString("3. Open in a browser:", MUSIC_MARGIN, y); y += lh;
    lcd.setTextColor(TFT_YELLOW, BLACK);
    lcd.drawString("   wiphone.local", MUSIC_MARGIN, y); y += lh;
    lcd.setTextColor(WHITE, BLACK);
    lcd.drawString("4. Drag music in. Done.", MUSIC_MARGIN, y); y += lh;
  } else {
    lcd.setTextColor(TFT_GREEN, BLACK);
    lcd.drawString("Server ON", MUSIC_MARGIN, y); y += lh + 4;
    lcd.setTextColor(WHITE, BLACK);
    lcd.drawString("On your computer, open:", MUSIC_MARGIN, y); y += lh;
    lcd.setTextColor(TFT_YELLOW, BLACK);
    lcd.drawString("  wiphone.local", MUSIC_MARGIN, y); y += lh;
    char l[48];
    snprintf(l, sizeof(l), "  or http://%s", xferAddr());
    lcd.drawString(l, MUSIC_MARGIN, y); y += lh + 4;
    lcd.setTextColor(WHITE, BLACK);
    if (xferUsingAP()) {
      snprintf(l, sizeof(l), "(join WiFi '%s'", xferApName());
      lcd.drawString(l, MUSIC_MARGIN, y); y += lh;
      lcd.drawString(" first, no password)", MUSIC_MARGIN, y); y += lh + 4;
    } else {
      lcd.drawString("(same WiFi as the phone)", MUSIC_MARGIN, y); y += lh + 4;
    }
    snprintf(l, sizeof(l), "Tracks added: %d   ", xferFilesAdded());   // pad: drawn in place
    lcd.drawString(l, MUSIC_MARGIN, y); y += lh;
    lcd.setTextColor(TFT_DARKGREY, BLACK);
    lcd.drawString("Back: stop and re-scan", MUSIC_MARGIN, y);
  }
}

// ---------------------------------------------------------------- states

void MusicApp::enterState(MusicState_t state) {
  appState = state;
  freeWidgets();

  switch (state) {
  case MUSIC_LIB:
    controlState.msAppTimerEventPeriod = 0;
    header->setTitle("Music");
    footer->setButtons("Play", "Back");
    buildLibrary();
    break;
  case MUSIC_NOW:
    nowClean = false;
    controlState.msAppTimerEventPeriod = 1000;    // the elapsed time ticks
    /* ⚠ Force Numeric. ControlState::inputType is a MODE that persists across screens,
     * and the Meshtastic app leaves it on AlphaNum after composing — in which case 4 and
     * 6 would arrive as 'g' and 'm' and the transport keys would work or not depending
     * on which app you had opened earlier. Same trap the reader documents. */
    controlState.setInputState(InputType::Numeric);
    header->setTitle("Now playing");
    footer->setButtons("Menu", "Back");
    break;
  case MUSIC_XFER:
    xferClean = false;
    controlState.msAppTimerEventPeriod = 1000;    // live "tracks added" count
    header->setTitle("Add music");
    footer->setButtons(xferOn() ? "Stop" : "Start", "Back");
    break;
  case MUSIC_MENU:
    controlState.msAppTimerEventPeriod = 0;
    header->setTitle("Music options");
    footer->setButtons("Select", "Back");
    buildMenu();
    break;
  }
}

// ---------------------------------------------------------------- events

appEventResult MusicApp::processEvent(EventType event) {
  switch (appState) {

  case MUSIC_LIB:
    if (LOGIC_BUTTON_BACK(event)) {
      return EXIT_APP;
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      MenuOption::keyType sel = menu->currentKey();
      if (sel == MUSIC_ROW_NOW) {
        enterState(MUSIC_NOW);
        return REDRAW_ALL;
      }
      if (sel == MUSIC_ROW_ADD) {
        enterState(MUSIC_XFER);
        return REDRAW_ALL;
      }
      if (sel >= MUSIC_ROW_FIRST) {
        int idx = (int)sel - MUSIC_ROW_FIRST;
        if (musicPlayerPlay(idx)) {
          enterState(MUSIC_NOW);
        } else {
          /* Say so in the list. A track that does nothing when you press OK is the worst
           * failure here, and it has real causes: a truncated upload, a format the
           * decoder refuses, no PSRAM free. */
          freeWidgets();
          buildLibrary();
          menu->select(sel);
        }
        return REDRAW_ALL;
      }
    }
    return REDRAW_SCREEN;

  case MUSIC_NOW:
    if (LOGIC_BUTTON_BACK(event)) {
      controlState.msAppTimerEventPeriod = 0;
      enterState(MUSIC_LIB);
      return REDRAW_ALL;
    }
    if (LOGIC_BUTTON_OK(event)) {
      enterState(MUSIC_MENU);
      return REDRAW_ALL;
    }
    if (event == WIPHONE_KEY_SELECT) {
      musicPlayerTogglePause();
      nowClean = false;
      return REDRAW_SCREEN;
    }
    if (event == '6' || event == WIPHONE_KEY_RIGHT) {
      musicPlayerNext();
      nowClean = false;
      return REDRAW_SCREEN;
    }
    if (event == '4' || event == WIPHONE_KEY_LEFT) {
      musicPlayerPrev();
      nowClean = false;
      return REDRAW_SCREEN;
    }
    if (event == APP_TIMER_EVENT) {
      return REDRAW_SCREEN;
    }
    return DO_NOTHING;

  case MUSIC_XFER:
    if (LOGIC_BUTTON_BACK(event)) {
      xferStop();
      controlState.msAppTimerEventPeriod = 0;
      musicPlayerScan();               // anything that arrived is in the list now
      enterState(MUSIC_LIB);
      return REDRAW_ALL;
    }
    if (LOGIC_BUTTON_OK(event)) {
      if (xferOn()) {
        xferStop();
      } else {
        /* ⚠ Stop the music first. The uploader may bring up an access point, which drops
         * the phone off its network, and writing a 5 MB file to the card while the
         * decoder is reading from it makes both slow. Playing while uploading is not
         * worth what it costs. */
        musicPlayerPause();
        xferStart(&MUSIC_XFER_CFG);
      }
      xferClean = false;
      footer->setButtons(xferOn() ? "Stop" : "Start", "Back");
      return REDRAW_ALL;
    }
    if (event == APP_TIMER_EVENT) {
      return xferOn() ? REDRAW_SCREEN : DO_NOTHING;
    }
    return DO_NOTHING;

  case MUSIC_MENU:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(musicPlayerCurrent() >= 0 ? MUSIC_NOW : MUSIC_LIB);
      return REDRAW_ALL;
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      MenuOption::keyType sel = menu->currentKey();
      if (sel == MUSIC_MENU_SHUFFLE) {
        musicPlayerSetShuffle(!musicPlayerShuffle());
      } else if (sel == MUSIC_MENU_REPEAT) {
        const MusicRepeat r = musicPlayerRepeat();
        musicPlayerSetRepeat(r == MUSIC_REPEAT_OFF ? MUSIC_REPEAT_ALL
                             : (r == MUSIC_REPEAT_ALL ? MUSIC_REPEAT_ONE : MUSIC_REPEAT_OFF));
      } else if (sel == MUSIC_MENU_RESCAN) {
        musicPlayerScan();
        enterState(MUSIC_LIB);
        return REDRAW_ALL;
      } else if (sel == MUSIC_MENU_STOP) {
        musicPlayerStop();
        enterState(MUSIC_LIB);
        return REDRAW_ALL;
      }
      // Rebuild in place so the toggled value is visible immediately.
      MenuOption::keyType keep = sel;
      freeWidgets();
      buildMenu();
      menu->select(keep);
      return REDRAW_ALL;
    }
    return REDRAW_SCREEN;
  }
  return DO_NOTHING;
}

void MusicApp::redrawScreen(bool redrawAll) {
  switch (appState) {
  case MUSIC_NOW:
    drawNowPlaying();
    break;
  case MUSIC_XFER:
    drawXfer();
    break;
  default:
    if (menu) {
      ((GUIWidget*)menu)->redraw(lcd);
    }
    break;
  }
}
