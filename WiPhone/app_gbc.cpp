/*
 * app_gbc.cpp — WiPhone Game Boy Color emulator (Phase 6: dual-core overlap, diagnostic)
 *
 * Two tasks + two framebuffers overlap emulation with the screen push:
 *   - emuThread (core 1): poll keypad, emulate+render into the back buffer, hand
 *     it to the blit task, switch buffers. Never touches the LCD or SD.
 *   - blitTask: owns ALL LCD and SD. Pushes finished frames while the emulator
 *     renders the next; when paused, runs the menu + save/load here too so the two
 *     cores never hit the shared SPI bus at the same time.
 *
 * Both tasks are left unpinned (pinning to core 1 starved them) and the core
 * watchdogs are disabled while playing (the two tasks saturate both cores). The
 * emulator paces to 60fps and only renders frames the blit can show, so game
 * speed and input stay correct (and authentic) even when the display can't keep up.
 */

#include "app_gbc.h"
#include "esp_heap_caps.h"
#include "esp_bt.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
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

// Fill mode: nearest-neighbor 1.5x upscale of 160x144 -> 240x216.
#define GBC_FILL_W  240
#define GBC_FILL_H  216

// setup() error codes (negative), surfaced on the error screen.
#define GBC_ERR_ALLOC   -2
#define GBC_ERR_INIT    -3
#define GBC_ERR_LOAD    -4
#define GBC_ERR_TASK    -9

// gnuboy uses global state and gb_hw_init() has no matching free, so the core and
// framebuffers are set up once per boot and reused across launches.
static bool      s_gnuboyInited = false;
static uint16_t* s_emuFb[2] = { NULL, NULL };  // double buffer, RGB565 big-endian (PSRAM)
static uint16_t* s_scaledFb = NULL;            // 240x216 upscale target (PSRAM), for Fill
static uint16_t  s_xmap[GBC_FILL_W];           // precomputed nearest-neighbor column map
static int16_t*  s_emuAudio = NULL;
static uint8_t*  s_emuRom   = NULL;

// Per-launch task handoff.
static SemaphoreHandle_t s_blitGo   = NULL;   // emu -> blit: buffer s_blitIdx is ready
static TaskHandle_t      s_blitTask = NULL;
static volatile int      s_blitIdx = 0;
static volatile bool     s_blitBusy = false;  // true while the blit task is pushing a frame
static volatile bool     s_running = false;
static volatile bool     s_blitExited = false;
static volatile bool     s_emuExited = false;

GbcApp::GbcApp(LCD& disp, ControlState& state) : ThreadedApp(disp, state) {
  log_d("create GbcApp");

  // Enter "gaming mode": stop mesh/LoRa polling and turn WiFi off. BT in setup.
  gGbcActive = true;
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);

  // The two emulator tasks saturate both cores, so the idle tasks can't pet the
  // task watchdog -> disable the core WDTs while playing (re-enabled on exit).
  disableCore0WDT();
  disableCore1WDT();

  if (!setupEmulator()) {
    return;   // initErr set; redrawScreen() shows the error, END/Back exits
  }

  s_blitGo   = xSemaphoreCreateBinary();
  s_running    = true;
  s_blitBusy   = false;
  s_blitExited = false;
  s_emuExited  = false;
  if (!s_blitGo) {
    initErr = GBC_ERR_ALLOC;
    return;
  }
  // Leave both tasks UNPINNED (like DigitalRainApp, which blits fine): the SMP
  // scheduler runs the two ready high-priority tasks on the two cores, giving the
  // overlap, without the core-1 starvation seen when pinning there.
  BaseType_t e = xTaskCreate(&GbcApp::emuThread, "gbc", 8192, this,
                             tskIDLE_PRIORITY + 2, &xHandle);
  BaseType_t b = xTaskCreate(&GbcApp::blitTask, "gbcblit", 8192, this,
                             tskIDLE_PRIORITY + 2, &s_blitTask);
  if (b != pdPASS || e != pdPASS) {
    xHandle = NULL;
    initErr = GBC_ERR_TASK;
  }
}

GbcApp::~GbcApp() {
  log_d("destroy GbcApp");
  s_running = false;
  if (s_blitGo) {
    xSemaphoreGive(s_blitGo);   // wake the blit task if it's waiting
  }
  // Wait for both loops to exit (they then park in a delay loop), so neither is
  // mid-SPI/SD when we delete it, then free both explicitly.
  for (int i = 0; i < 60 && !(s_emuExited && s_blitExited); i++) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (xHandle)    { vTaskDelete(xHandle);    xHandle = NULL; }
  if (s_blitTask) { vTaskDelete(s_blitTask); s_blitTask = NULL; }
  if (s_blitGo)   { vSemaphoreDelete(s_blitGo); s_blitGo = NULL; }

  lcd.setSwapBytes(false);

  enableCore0WDT();
  enableCore1WDT();

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

  if (!s_gnuboyInited) {
    s_emuAudio = (int16_t*)malloc(GBC_EMU_AUDIO_LEN * sizeof(int16_t));
    size_t fbBytes = (size_t)GBC_SCREEN_W * GBC_SCREEN_H * sizeof(uint16_t);
    s_emuFb[0] = (uint16_t*)ps_malloc(fbBytes);
    s_emuFb[1] = (uint16_t*)ps_malloc(fbBytes);
    s_scaledFb = (uint16_t*)ps_malloc((size_t)GBC_FILL_W * GBC_FILL_H * sizeof(uint16_t));
    if (!s_emuFb[0] || !s_emuFb[1] || !s_scaledFb || !s_emuAudio) {
      initErr = GBC_ERR_ALLOC;
      return false;
    }
    for (int ox = 0; ox < GBC_FILL_W; ox++) {
      s_xmap[ox] = (uint16_t)((ox * GBC_SCREEN_W) / GBC_FILL_W);
    }
    if (gnuboy_init(GBC_EMU_SAMPLERATE, GB_AUDIO_STEREO_S16, GB_PIXEL_565_BE, NULL, NULL) < 0) {
      initErr = GBC_ERR_INIT;
      return false;
    }
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
  const uint16_t* fb = s_emuFb[idx];
  if (!scaled) {
    const int xoff = (lcd.width()  - GBC_SCREEN_W) / 2;
    const int yoff = (lcd.height() - GBC_SCREEN_H) / 2;
    lcd.pushImage(xoff, yoff, GBC_SCREEN_W, GBC_SCREEN_H, (uint16_t*)fb);
    return;
  }
  for (int oy = 0; oy < GBC_FILL_H; oy++) {
    const uint16_t* src = fb + ((oy * GBC_SCREEN_H) / GBC_FILL_H) * GBC_SCREEN_W;
    uint16_t* dst = s_scaledFb + oy * GBC_FILL_W;
    for (int ox = 0; ox < GBC_FILL_W; ox++) {
      dst[ox] = src[s_xmap[ox]];
    }
  }
  const int xoff = (lcd.width()  - GBC_FILL_W) / 2;
  const int yoff = (lcd.height() - GBC_FILL_H) / 2;
  lcd.pushImage(xoff, yoff, GBC_FILL_W, GBC_FILL_H, s_scaledFb);
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

// Core 0: owns the LCD and SD. Pushes finished frames; when paused, runs the
// menu and save/load here (so SD and LCD never contend on the SPI bus).
void GbcApp::blitTask(void* pvParam) {
  GbcApp* app = (GbcApp*)pvParam;
  app->lcd.setSwapBytes(false);
  app->lcd.fillScreen(BLACK);

  while (s_running) {
    if (app->paused) {
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
          app->paused = false;
        } else {
          snprintf(app->statusMsg, sizeof(app->statusMsg), "No save found");
          app->menuDirty = true;
        }
      }
      if (app->menuDirty) {
        app->drawPauseMenu();
        app->menuDirty = false;
      }
      s_blitBusy = false;
      gGbcKeyLatch = 0;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    // Running: push the latest rendered frame the emulator handed us.
    if (xSemaphoreTake(s_blitGo, pdMS_TO_TICKS(30)) == pdTRUE) {
      if (!s_running) {
        break;
      }
      if (app->needsClear) {
        app->lcd.fillScreen(BLACK);
        app->needsClear = false;
      }
      app->blitBuffer(s_blitIdx);
      s_blitBusy = false;
    }
  }
  s_blitExited = true;
  for (;;) {
    vTaskDelay(portMAX_DELAY);   // park until the destructor deletes us
  }
}

// Emulate at a steady ~60fps (correct game speed + responsive input). Render only
// the frames the blit task can actually show; when the blit is busy, keep
// emulating (no render) so the game never slows down — display frames just drop.
void GbcApp::emuThread(void* pvParam) {
  GbcApp* app = (GbcApp*)pvParam;
  int emuBuf = 0;
  uint32_t start = millis();
  uint32_t frames = 0;
  while (s_running) {
    if (app->paused) {
      vTaskDelay(pdMS_TO_TICKS(20));
      start = millis();           // reset the pacing baseline for resume
      frames = 0;
      continue;
    }
    gnuboy_set_pad(app->readPad());
    bool willDraw = !s_blitBusy;                 // only render if the blit is free
    gnuboy_set_framebuffer(s_emuFb[emuBuf]);
    gnuboy_run(willDraw);
    frames++;
    if (willDraw) {
      s_blitIdx = emuBuf;
      s_blitBusy = true;
      xSemaphoreGive(s_blitGo);
      emuBuf = 1 - emuBuf;        // render the next frame into the other buffer
    }
    // Pace to 60 emulated fps; always yield a little for the main loop's keypad.
    uint32_t target = start + (uint32_t)((uint64_t)frames * 1000 / 60);
    int32_t ahead = (int32_t)(target - millis());
    vTaskDelay(ahead > 1 ? pdMS_TO_TICKS(ahead) : 1);
  }
  s_emuExited = true;
  for (;;) {
    vTaskDelay(portMAX_DELAY);
  }
}

appEventResult GbcApp::processEvent(EventType event) {
  if (initErr) {
    return LOGIC_BUTTON_BACK(event) ? EXIT_APP : DO_NOTHING;
  }

  if (!paused) {
    if (event == WIPHONE_KEY_END) {
      statusMsg[0] = 0;
      menuSel = GBC_MENU_RESUME;
      menuDirty = true;
      paused = true;
    }
    return DO_NOTHING;
  }

  switch (event) {
    case WIPHONE_KEY_UP:
      menuSel = (menuSel + GBC_MENU_COUNT - 1) % GBC_MENU_COUNT;
      menuDirty = true;
      break;
    case WIPHONE_KEY_DOWN:
      menuSel = (menuSel + 1) % GBC_MENU_COUNT;
      menuDirty = true;
      break;
    case WIPHONE_KEY_END:
      paused = false;
      break;
    case WIPHONE_KEY_OK:
      switch (menuSel) {
        case GBC_MENU_RESUME: paused = false;               break;
        case GBC_MENU_SAVE:   pendingAction = GBC_ACT_SAVE;  break;
        case GBC_MENU_LOAD:   pendingAction = GBC_ACT_LOAD;  break;
        case GBC_MENU_SCREEN: scaled = !scaled;
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
    return;                            // the emulator tasks own the screen
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
