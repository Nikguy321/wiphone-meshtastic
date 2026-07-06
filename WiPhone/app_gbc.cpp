/*
 * app_gbc.cpp — WiPhone Game Boy Color emulator (Phase 3: playable, single-task)
 *
 * One task (core 0) emulates and blits each frame. Entering the app turns off
 * WiFi and stops the main loop's mesh/LoRa polling ("gaming mode") so the
 * emulator has the CPU and the shared SPI bus to itself; exiting restores them.
 *
 * (A double-buffered two-core overlap for higher framerate is planned, but is
 * kept out until it's proven — this single-task path is the reliable baseline.)
 */

#include "app_gbc.h"
#include "esp_heap_caps.h"
#include "esp_bt.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "WiFi.h"
#include "SD.h"
#include "gnuboy/gnuboy.h"
#include "gbc_test_rom.h"   // embedded public-domain ROM (flash) used when no SD ROM

// Global held-button mask, updated by the main loop's keypad scanner.
extern uint32_t keypadState;
// When true, the main loop stops polling the mesh/LoRa radio (frees the SPI bus).
extern volatile bool gGbcActive;
// Sticky press mask so fast taps between the emulator's frame-rate polls survive.
extern volatile uint32_t gGbcKeyLatch;

#define GBC_EMU_SAMPLERATE   32768
#define GBC_EMU_AUDIO_LEN    2048   // int16 samples in the (unused) audio sink buffer
#define GBC_FRAMESKIP        2      // emulate every frame, draw every Nth

// Pause-menu items and pending actions.
#define GBC_MENU_RESUME  0
#define GBC_MENU_SAVE    1
#define GBC_MENU_LOAD    2
#define GBC_MENU_SCREEN  3
#define GBC_MENU_QUIT    4
#define GBC_MENU_COUNT   5
#define GBC_ACT_NONE     0
#define GBC_ACT_SAVE     1
#define GBC_ACT_LOAD     2

// Fill mode: nearest-neighbor 1.5x upscale of 160x144 -> 240x216 (fills the
// screen width, keeps aspect ratio, black bars top/bottom).
#define GBC_FILL_W  240
#define GBC_FILL_H  216

// setup() error codes (negative), surfaced on the error screen.
#define GBC_ERR_ALLOC   -2
#define GBC_ERR_INIT    -3
#define GBC_ERR_LOAD    -4
#define GBC_ERR_TASK    -9

// gnuboy uses global state and gb_hw_init() has no matching free, so the core
// and framebuffer are set up once per boot and reused across launches.
static bool      s_gnuboyInited = false;
static uint16_t* s_emuFb    = NULL;   // 160x144 RGB565 big-endian framebuffer (PSRAM)
static uint16_t* s_scaledFb = NULL;   // 240x216 upscale target (PSRAM), for Fill mode
static uint16_t  s_xmap[GBC_FILL_W];  // precomputed nearest-neighbor column map
static int16_t*  s_emuAudio = NULL;
static uint8_t*  s_emuRom   = NULL;

GbcApp::GbcApp(LCD& disp, ControlState& state) : ThreadedApp(disp, state) {
  log_d("create GbcApp");

  // Enter "gaming mode": stop mesh/LoRa polling (frees the shared SPI bus) and
  // turn WiFi off (frees the high-priority WiFi tasks). BT is released in setup.
  gGbcActive = true;
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);

  if (!setupEmulator()) {
    return;   // initErr set; redrawScreen() shows the error, END/Back exits
  }
  BaseType_t st = xTaskCreatePinnedToCore(&GbcApp::emuThread, "gbc", 8192, this,
                                          tskIDLE_PRIORITY + 2, &xHandle, 0);
  if (st != pdPASS) {
    xHandle = NULL;
    initErr = GBC_ERR_TASK;
  }
}

GbcApp::~GbcApp() {
  log_d("destroy GbcApp");
  // ~ThreadedApp() deletes the task.
  lcd.setSwapBytes(false);   // leave the UI's expected default

  // Leave gaming mode: resume mesh polling and bring WiFi back up.
  gGbcActive = false;
  WiFi.mode(WIFI_STA);
  WiFi.reconnect();
}

// Free the internal RAM the Bluetooth controller reserves (~60KB; BT is unused).
void GbcApp::reclaimInternalRam() {
  esp_bt_controller_status_t st = esp_bt_controller_get_status();
  if (st == ESP_BT_CONTROLLER_STATUS_ENABLED) {
    esp_bt_controller_disable();
  }
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
    esp_bt_controller_deinit();
  }
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
}

// Case-insensitive check for a .gb / .gbc extension.
static bool hasRomExt(const char* name) {
  const char* dot = strrchr(name, '.');
  if (!dot) {
    return false;
  }
  char ext[5] = {0};
  for (int i = 0; i < 4 && dot[i]; i++) {
    char c = dot[i];
    ext[i] = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
  }
  return (strcmp(ext, ".gb") == 0 || strcmp(ext, ".gbc") == 0);
}

bool GbcApp::findRom(char* pathOut, size_t pathLen, char* nameOut, size_t nameLen) {
  const char* dirs[] = { "/roms", "/" };
  for (int d = 0; d < 2; d++) {
    File dir = SD.open(dirs[d]);
    if (!dir) {
      continue;
    }
    if (!dir.isDirectory()) {
      dir.close();
      continue;
    }
    File f;
    while ((f = dir.openNextFile())) {
      if (!f.isDirectory()) {
        const char* nm = f.name();
        const char* slash = strrchr(nm, '/');
        const char* base = slash ? slash + 1 : nm;
        if (hasRomExt(base)) {
          snprintf(pathOut, pathLen, "%s", nm);
          snprintf(nameOut, nameLen, "%s", base);
          f.close();
          dir.close();
          return true;
        }
      }
      f.close();
    }
    dir.close();
  }
  return false;
}

bool GbcApp::setupEmulator() {
  if (!s_gnuboyInited) {
    reclaimInternalRam();
  }

  // Pick a ROM: an SD-card .gb/.gbc if present, else the embedded game.
  const uint8_t* romData = NULL;
  size_t romSize = 0;
  char path[96];
  if (findRom(path, sizeof(path), romName, sizeof(romName))) {
    File f = SD.open(path);
    if (f) {
      romSize = f.size();
      if (s_emuRom) {
        free(s_emuRom);
        s_emuRom = NULL;
      }
      s_emuRom = (uint8_t*)ps_malloc(romSize);
      if (s_emuRom) {
        size_t rd = 0;
        while (rd < romSize) {
          int n = f.read(s_emuRom + rd, romSize - rd);
          if (n <= 0) {
            break;
          }
          rd += n;
        }
        if (rd == romSize) {
          romData = s_emuRom;
        }
      }
      f.close();
    }
  }
  if (!romData) {
    romData = gbc_test_rom;
    romSize = gbc_test_rom_len;
    snprintf(romName, sizeof(romName), "uCity(flash)");
  }

  // One-time gnuboy setup + framebuffer allocation.
  if (!s_gnuboyInited) {
    s_emuAudio = (int16_t*)malloc(GBC_EMU_AUDIO_LEN * sizeof(int16_t));
    size_t fbBytes = (size_t)GBC_SCREEN_W * GBC_SCREEN_H * sizeof(uint16_t);
    s_emuFb = (uint16_t*)ps_malloc(fbBytes);
    s_scaledFb = (uint16_t*)ps_malloc((size_t)GBC_FILL_W * GBC_FILL_H * sizeof(uint16_t));
    if (!s_emuFb || !s_scaledFb || !s_emuAudio) {
      initErr = GBC_ERR_ALLOC;
      return false;
    }
    for (int ox = 0; ox < GBC_FILL_W; ox++) {   // nearest-neighbor column map
      s_xmap[ox] = (uint16_t)((ox * GBC_SCREEN_W) / GBC_FILL_W);
    }
    // RGB565 big-endian matches the panel byte order (no per-pixel swap on blit).
    if (gnuboy_init(GBC_EMU_SAMPLERATE, GB_AUDIO_STEREO_S16, GB_PIXEL_565_BE, NULL, NULL) < 0) {
      initErr = GBC_ERR_INIT;
      return false;
    }
    gnuboy_set_framebuffer(s_emuFb);
    gnuboy_set_soundbuffer(s_emuAudio, GBC_EMU_AUDIO_LEN);
    s_gnuboyInited = true;
  } else {
    gnuboy_free_rom();
  }

  if (gnuboy_load_rom(romData, romSize) < 0) {
    initErr = GBC_ERR_LOAD;
    return false;
  }
  gnuboy_reset(true);
  return true;
}

int GbcApp::readPad() {
  // Held state OR any press latched since the last poll (then clear the latch),
  // so a quick tap that came and went between frames still registers for a frame.
  uint32_t ks = keypadState | gGbcKeyLatch;
  gGbcKeyLatch = 0;
  int pad = 0;
  if (ks & WIPHONE_KEY_MASK_UP)     pad |= GB_PAD_UP;
  if (ks & WIPHONE_KEY_MASK_DOWN)   pad |= GB_PAD_DOWN;
  if (ks & WIPHONE_KEY_MASK_LEFT)   pad |= GB_PAD_LEFT;
  if (ks & WIPHONE_KEY_MASK_RIFHT)  pad |= GB_PAD_RIGHT;   // (typo'd mask name in Hardware.h)
  if (ks & WIPHONE_KEY_MASK_F4)     pad |= GB_PAD_A;        // bottom-right user key
  if (ks & WIPHONE_KEY_MASK_F3)     pad |= GB_PAD_B;        // user key above A
  if (ks & WIPHONE_KEY_MASK_BACK)   pad |= GB_PAD_START;    // top-right
  if (ks & WIPHONE_KEY_MASK_SELECT) pad |= GB_PAD_SELECT;   // top-left
  return pad;
}

void GbcApp::blitBuffer(int idx) {
  if (!scaled) {
    const int xoff = (lcd.width()  - GBC_SCREEN_W) / 2;
    const int yoff = (lcd.height() - GBC_SCREEN_H) / 2;
    lcd.pushImage(xoff, yoff, GBC_SCREEN_W, GBC_SCREEN_H, s_emuFb);
    return;
  }
  // Fill mode: nearest-neighbor upscale into s_scaledFb, then one push.
  for (int oy = 0; oy < GBC_FILL_H; oy++) {
    const uint16_t* src = s_emuFb + ((oy * GBC_SCREEN_H) / GBC_FILL_H) * GBC_SCREEN_W;
    uint16_t* dst = s_scaledFb + oy * GBC_FILL_W;
    for (int ox = 0; ox < GBC_FILL_W; ox++) {
      dst[ox] = src[s_xmap[ox]];
    }
  }
  const int xoff = (lcd.width()  - GBC_FILL_W) / 2;
  const int yoff = (lcd.height() - GBC_FILL_H) / 2;
  lcd.pushImage(xoff, yoff, GBC_FILL_W, GBC_FILL_H, s_scaledFb);
}

void GbcApp::blitTask(void* pvParam) {
  (void)pvParam;   // unused in the single-task baseline
}

// Build "/sd/gbc/<rom>.state" from a sanitized ROM name.
void GbcApp::buildStatePath(char* out, size_t n) {
  char clean[40];
  int j = 0;
  for (int i = 0; romName[i] && j < (int)sizeof(clean) - 1; i++) {
    char c = romName[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      clean[j++] = c;
    }
  }
  clean[j] = 0;
  snprintf(out, n, "/sd/gbc/%s.state", clean);
}

void GbcApp::drawPauseMenu() {
  SmoothFont* font = fonts[OPENSANS_COND_BOLD_20];
  const int bw = 176, bh = 176;
  const int bx = (lcd.width() - bw) / 2;
  const int by = (lcd.height() - bh) / 2;
  lcd.fillRect(bx, by, bw, bh, TFT_DARKGREY);
  lcd.drawRect(bx, by, bw, bh, TFT_WHITE);
  lcd.setTextFont(font);
  lcd.setTextDatum(TL_DATUM);
  lcd.setTextColor(TFT_WHITE, TFT_DARKGREY);
  lcd.drawString("PAUSED", bx + 14, by + 10);

  const char* items[GBC_MENU_COUNT] = {
    "Resume", "Save state", "Load state", scaled ? "Screen: Fill" : "Screen: 1:1", "Quit"
  };
  int y = by + 46;
  for (int i = 0; i < GBC_MENU_COUNT; i++) {
    bool sel = (i == menuSel);
    lcd.setTextColor(sel ? TFT_YELLOW : TFT_WHITE, TFT_DARKGREY);
    lcd.drawString(sel ? ">" : " ", bx + 12, y);
    lcd.drawString(items[i], bx + 34, y);
    y += font->height() + 6;
  }
  if (statusMsg[0]) {
    lcd.setTextColor(TFT_GREENYELLOW, TFT_DARKGREY);
    lcd.drawString(statusMsg, bx + 14, by + bh - 26);
  }
}

void GbcApp::emuThread(void* pvParam) {
  GbcApp* app = (GbcApp*)pvParam;
  app->lcd.fillScreen(BLACK);
  app->lcd.setSwapBytes(false);   // framebuffer is already big-endian
  uint32_t frame = 0;
  while (1) {                     // stopped by vTaskDelete() from ~ThreadedApp
    if (app->paused) {
      // Perform any requested save/load here (same task as the LCD, so SD and
      // TFT never hit the shared SPI bus at the same time).
      if (app->pendingAction == GBC_ACT_SAVE) {
        SD.mkdir("/gbc");
        char path[80];
        app->buildStatePath(path, sizeof(path));
        int r = gnuboy_save_state(path);
        snprintf(app->statusMsg, sizeof(app->statusMsg), r == 0 ? "Saved" : "Save failed");
        app->pendingAction = GBC_ACT_NONE;
        app->menuDirty = true;
      } else if (app->pendingAction == GBC_ACT_LOAD) {
        char path[80];
        app->buildStatePath(path, sizeof(path));
        int r = gnuboy_load_state(path);
        app->pendingAction = GBC_ACT_NONE;
        if (r == 0) {
          app->paused = false;    // resume into the loaded state
        } else {
          snprintf(app->statusMsg, sizeof(app->statusMsg), "No save found");
          app->menuDirty = true;
        }
      }
      if (app->menuDirty) {
        app->drawPauseMenu();
        app->menuDirty = false;
      }
      gGbcKeyLatch = 0;   // don't let menu-nav presses spill into the game on resume
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    if (app->needsClear) {        // a Screen mode switch: wipe leftover borders
      app->lcd.fillScreen(BLACK);
      app->needsClear = false;
    }
    gnuboy_set_pad(app->readPad());
    frame++;
    bool draw = (frame % GBC_FRAMESKIP) == 0;
    gnuboy_run(draw);
    if (draw) {
      app->blitBuffer(0);
    }
    vTaskDelay(1);                // yield so core 0's watchdog/idle and IPC run
  }
}

appEventResult GbcApp::processEvent(EventType event) {
  if (initErr) {
    return LOGIC_BUTTON_BACK(event) ? EXIT_APP : DO_NOTHING;  // error screen
  }

  if (!paused) {
    if (event == WIPHONE_KEY_END) {     // hang-up opens the pause menu
      statusMsg[0] = 0;
      menuSel = GBC_MENU_RESUME;
      menuDirty = true;
      paused = true;
    }
    return DO_NOTHING;                  // all other keys are read as gamepad input
  }

  // Paused: drive the menu.
  switch (event) {
    case WIPHONE_KEY_UP:
      menuSel = (menuSel + GBC_MENU_COUNT - 1) % GBC_MENU_COUNT;
      menuDirty = true;
      break;
    case WIPHONE_KEY_DOWN:
      menuSel = (menuSel + 1) % GBC_MENU_COUNT;
      menuDirty = true;
      break;
    case WIPHONE_KEY_END:               // hang-up again = resume
      paused = false;
      break;
    case WIPHONE_KEY_OK:
      switch (menuSel) {
        case GBC_MENU_RESUME: paused = false;               break;
        case GBC_MENU_SAVE:   pendingAction = GBC_ACT_SAVE;  break;
        case GBC_MENU_LOAD:   pendingAction = GBC_ACT_LOAD;  break;
        case GBC_MENU_SCREEN: scaled = !scaled;             // toggle, stay in menu
                              needsClear = true;
                              menuDirty = true;              break;
        case GBC_MENU_QUIT:   return EXIT_APP;
      }
      break;
    default:
      break;
  }
  return DO_NOTHING;
}

void GbcApp::redrawScreen(bool redrawAll) {
  if (!initErr) {
    return;                            // the emulator task owns the screen
  }
  SmoothFont* font = fonts[OPENSANS_COND_BOLD_20];
  lcd.fillScreen(BLACK);
  lcd.setTextColor(RED, BLACK);
  lcd.setTextFont(font);
  lcd.setTextDatum(TL_DATUM);

  const char* msg = "emulator setup failed";
  if (initErr == GBC_ERR_ALLOC) {
    msg = "out of memory";
  } else if (initErr == GBC_ERR_INIT) {
    msg = "gnuboy init failed";
  } else if (initErr == GBC_ERR_LOAD) {
    msg = "bad ROM";
  } else if (initErr == GBC_ERR_TASK) {
    msg = "could not start task";
  }
  lcd.drawString(msg, 6, 40);
  lcd.setTextColor(WHITE, BLACK);
  lcd.drawString("Back/hang-up to exit", 6, 40 + font->height() + 6);
}
