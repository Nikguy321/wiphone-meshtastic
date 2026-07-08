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
#include "esp_system.h"     // esp_reset_reason (transfer-screen diagnostics)
#include "esp_bt.h"
#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "WiFi.h"
#include "SD.h"
#include "driver/i2s.h"
#include "Audio.h"
#include "gnuboy/gnuboy.h"
#include "gbc_test_rom.h"   // embedded public-domain ROM (flash) used when no SD ROM
#include "app_gbc_xfer.h"   // ROM transfer web server (UI lives on our Transfer screen)

// Global held-button mask, updated by the main loop's keypad scanner.
extern uint32_t keypadState;
// The phone's shared audio subsystem (I2S + codec + amp). We borrow it during play.
extern Audio* audio;
// When true, the main loop stops polling the mesh/LoRa radio (frees the SPI bus).
extern volatile bool gGbcActive;
// Sticky press mask so fast taps between the emulator's frame-rate polls survive.
extern volatile uint32_t gGbcKeyLatch;

// The I2S/codec runs at 32000. The emulator's APU rate is chosen slightly HIGHER
// on purpose: gnuboy rounds its internal step to (1<<21)/rate = 65, producing
// 32264 samples per emulated second — a ~0.8% surplus over the DAC. That keeps
// the DMA buffer full, so the blocking i2s_write paces the game to the DAC
// (continuous audio, ~99% speed) instead of slowly draining into underruns.
#define GBC_I2S_RATE         32000
#define GBC_EMU_SAMPLERATE   32264
#define GBC_EMU_AUDIO_LEN    2048   // int16 samples in the emulator's audio buffer
// In-game volume step (F1/F2). Codec volume is in dB-ish units (see Audio.h).
#define GBC_VOL_STEP         3

// Picker rows 0..GBC_PICKER_ACTIONS-1 are actions (Transfer, Help); ROMs follow.
#define GBC_PICKER_ACTIONS 2
// Names longer than this scroll (marquee) while selected. ~What fits from x=28
// in the condensed bold-20 font on the 240px screen.
#define GBC_PICKER_VIS_CHARS 18

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
#define GBC_ERR_ROMBIG  -5   // ROM larger than the largest free PSRAM block
#define GBC_ERR_READ    -6   // SD open/read failed
#define GBC_ERR_TASK    -9

// gnuboy uses global state and gb_hw_init() has no matching free, so the core and
// framebuffers are set up once per boot and reused across launches.
static bool      s_gnuboyInited = false;
static uint16_t* s_emuFb[2] = { NULL, NULL };  // double buffer, RGB565 big-endian (PSRAM)
static uint16_t  s_xmap[GBC_FILL_W];           // precomputed nearest-neighbor column map
static int16_t*  s_emuAudio = NULL;
static uint8_t*  s_emuRom   = NULL;
static bool      s_romLoaded = false;   // gnuboy currently holds a loaded ROM

// Release the current ROM: gnuboy's references first, then the PSRAM buffer they
// point into. Idempotent, so it's safe on exit and before loading a new ROM.
static void gbcUnloadRom() {
  if (s_romLoaded) {
    gnuboy_free_rom();
    s_romLoaded = false;
  }
  if (s_emuRom) {
    free(s_emuRom);
    s_emuRom = NULL;
  }
}

// Free everything the emulator keeps resident (gnuboy WRAM/VRAM, task stacks,
// audio buffer, framebuffers, loaded ROM). Once a game has run, the phone idles
// at ~10KB free internal RAM — the WiFi/web stack then panics (OOM) trying to
// serve the ROM transfer page, so the transfer screen calls this first. The
// next game launch re-allocates and re-inits everything from scratch.
static void gbcReleaseEmulator();

// Per-launch task handoff.
static SemaphoreHandle_t s_blitGo   = NULL;   // emu -> blit: buffer s_blitIdx is ready
static TaskHandle_t      s_blitTask = NULL;
static volatile int      s_blitIdx = 0;
static volatile bool     s_blitBusy = false;  // true while the blit task is pushing a frame
static volatile bool     s_running = false;
static volatile bool     s_blitExited = false;
static volatile bool     s_emuExited = false;

// Task stacks/TCBs allocated once and reused every game (xTaskCreateStatic), so
// launching a game never allocates — deleting an unpinned task defers its stack
// free to the (starved) idle task, which otherwise runs the phone out of RAM
// after one game ("could not start task").
// NOTE: StackType_t is 1 byte on ESP-IDF FreeRTOS, so these are sizes in BYTES.
// After gnuboy_init claims WRAM/VRAM, free internal RAM is ~32KB with a largest
// contiguous block of ~9.6KB (measured on-device) — so only ONE 8KB block exists.
// The blit task gets it: it runs the deep fopen->FATFS->SD save/load path that
// overflowed the old 2KB stacks and rebooted the phone. The emulator loop is
// shallow (it ran indefinitely on 2KB without crashing), so 4KB is a 2x margin.
#define GBC_EMU_STACK_BYTES   4096
#define GBC_BLIT_STACK_BYTES  8192
static StaticTask_t s_emuTcb;
static StaticTask_t s_blitTcb;
// Stacks live in internal RAM but are allocated from the heap on the FIRST launch
// (after gaming mode has freed WiFi + the ~60KB BT RAM) and reused forever, never
// freed. Keeping them out of BSS matters: ~20KB of permanent static internal RAM
// starves the boot-time WiFi/BT allocations and boot-loops this RAM-tight board.
static StackType_t* s_emuStack  = NULL;
static StackType_t* s_blitStack = NULL;

static void gbcReleaseEmulator() {
  gbcUnloadRom();
  if (s_gnuboyInited) {
    gnuboy_deinit();
    s_gnuboyInited = false;
  }
  if (s_emuAudio)  { free(s_emuAudio);  s_emuAudio  = NULL; }
  if (s_emuFb[0])  { free(s_emuFb[0]);  s_emuFb[0]  = NULL; }
  if (s_emuFb[1])  { free(s_emuFb[1]);  s_emuFb[1]  = NULL; }
  if (s_emuStack)  { free(s_emuStack);  s_emuStack  = NULL; }
  if (s_blitStack) { free(s_blitStack); s_blitStack = NULL; }
}

GbcApp::GbcApp(LCD& disp, ControlState& state) : ThreadedApp(disp, state) {
  log_d("create GbcApp");
  scanRoms();               // build the picker list; the game starts on selection
  romSel = GBC_PICKER_ACTIONS;   // land on the first game, not the action rows
  // ~4Hz APP_TIMER_EVENT drives the picker's marquee for long ROM names (the
  // GUI saves/restores the previous period around this app's lifetime).
  controlState.msAppTimerEventPeriod = 250;
}

// Enter gaming mode and launch the selected ROM on the two emulator tasks.
void GbcApp::startGame() {
  // Gaming mode: stop mesh/LoRa polling, turn WiFi off (frees CPU + the SPI bus),
  // and disable the core watchdogs (the two tasks saturate both cores).
  gGbcActive = true;
  enteredGaming = true;
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  disableCore0WDT();
  disableCore1WDT();

  if (!setupEmulator(roms[romSel - GBC_PICKER_ACTIONS])) {
    return;   // initErr set; redrawScreen() shows the error
  }

  // Borrow the phone's audio path for the emulator's APU output. Best-effort:
  // if it won't start, the game just runs silent (never block gameplay on it).
  if (audio) {
    audio->setSampleRate(GBC_I2S_RATE);
    soundOn = audio->start();
    audioStarve = 0;
  }

  s_blitGo     = xSemaphoreCreateBinary();
  s_running    = true;
  s_blitBusy   = false;
  s_blitExited = false;
  s_emuExited  = false;
  if (!s_blitGo) {
    initErr = GBC_ERR_ALLOC;
    return;
  }
  // Allocate the task stacks once, now that gaming mode has freed the RAM (WiFi off,
  // BT reclaimed). Reused every game and never freed, so this runs only once.
  // Blit stack FIRST: it needs the single largest free block (see above), and
  // carving the emu stack out first could split it.
  if (!s_blitStack) {
    s_blitStack = (StackType_t*)heap_caps_malloc(GBC_BLIT_STACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!s_emuStack) {
    s_emuStack = (StackType_t*)heap_caps_malloc(GBC_EMU_STACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!s_emuStack || !s_blitStack) {
    dbgWhere = 2;
    dbgFreeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    dbgLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    initErr = GBC_ERR_ALLOC;
    return;
  }
  // Static stacks (reused every game) and UNPINNED (like DigitalRainApp): the SMP
  // scheduler runs the two ready high-priority tasks on the two cores.
  xHandle = xTaskCreateStatic(&GbcApp::emuThread, "gbc", GBC_EMU_STACK_BYTES, this,
                              tskIDLE_PRIORITY + 2, s_emuStack, &s_emuTcb);
  s_blitTask = xTaskCreateStatic(&GbcApp::blitTask, "gbcblit", GBC_BLIT_STACK_BYTES, this,
                                 tskIDLE_PRIORITY + 2, s_blitStack, &s_blitTcb);
  if (xHandle && s_blitTask) {
    playing = true;
    return;
  }
  // Shouldn't happen with static allocation, but stay safe.
  s_running = false;
  if (s_blitGo) {
    xSemaphoreGive(s_blitGo);
  }
  vTaskDelay(pdMS_TO_TICKS(40));
  if (xHandle)    { vTaskDelete(xHandle); }
  if (s_blitTask) { vTaskDelete(s_blitTask); }
  xHandle = NULL;
  s_blitTask = NULL;
  initErr = GBC_ERR_TASK;
}

GbcApp::~GbcApp() {
  log_d("destroy GbcApp");
  if (playing) {
    s_running = false;
    if (s_blitGo) {
      xSemaphoreGive(s_blitGo);   // wake the blit task if it's waiting
    }
    // Wait for both loops to exit, then a bit more so each task actually reaches
    // its parked vTaskDelay() (blocked). Deleting a *blocked* task frees its stack
    // immediately; deleting a still-running one only defers the free to idle,
    // which would leave no RAM for the next game ("could not start task").
    for (int i = 0; i < 60 && !(s_emuExited && s_blitExited); i++) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    if (xHandle)    { vTaskDelete(xHandle);    xHandle = NULL; }
    if (s_blitTask) { vTaskDelete(s_blitTask); s_blitTask = NULL; }
    lcd.setSwapBytes(false);
    vTaskDelay(pdMS_TO_TICKS(30));   // let the idle task reclaim any deferred frees
  }
  // Always release these (also covers a failed startGame that created the
  // semaphore / loaded a ROM before bailing).
  if (s_blitGo) { vSemaphoreDelete(s_blitGo); s_blitGo = NULL; }
  gbcUnloadRom();

  if (soundOn && audio) {   // hand the audio path back to the phone
    audio->shutdown();
    soundOn = false;
  }
  gbcXferStop();            // in case the app dies while the transfer screen is up

  if (enteredGaming) {   // restore what gaming mode turned off
    enableCore0WDT();
    enableCore1WDT();
    gGbcActive = false;
    WiFi.mode(WIFI_STA);
    WiFi.reconnect();
  }
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

void GbcApp::scanRoms() {
  romCount = 0;
  // The built-in game is always available (works with no SD card).
  snprintf(roms[romCount].name, sizeof(roms[0].name), "uCity (built-in)");
  roms[romCount].path[0] = 0;
  roms[romCount].embedded = true;
  romCount++;

  const char* dirs[] = { "/roms", "/" };
  for (int d = 0; d < 2 && romCount < GBC_MAX_ROMS; d++) {
    File dir = SD.open(dirs[d]);
    if (!dir) {
      continue;
    }
    if (!dir.isDirectory()) {
      dir.close();
      continue;
    }
    File f;
    while (romCount < GBC_MAX_ROMS && (f = dir.openNextFile())) {
      if (!f.isDirectory()) {
        const char* nm = f.name();
        const char* slash = strrchr(nm, '/');
        const char* base = slash ? slash + 1 : nm;
        if (hasRomExt(base)) {
          snprintf(roms[romCount].name, sizeof(roms[0].name), "%s", base);
          snprintf(roms[romCount].path, sizeof(roms[0].path), "%s", nm);
          roms[romCount].embedded = false;
          romCount++;
        }
      }
      f.close();
    }
    dir.close();
  }
}

bool GbcApp::setupEmulator(const Rom& rom) {
  gbcUnloadRom();                    // free whatever was loaded before (clean slate)

  if (!s_gnuboyInited) {
    reclaimInternalRam();
  }

  // Load the ROM. Failures are REAL errors now — this used to silently fall
  // back to the built-in game, so a too-big ROM (e.g. 4MB Tomb Raider, which
  // can never fit a contiguous PSRAM block) just launched uCity with no clue.
  const uint8_t* romData = NULL;
  size_t romSize = 0;
  if (!rom.embedded) {
    snprintf(romName, sizeof(romName), "%s", rom.name);
    File f = SD.open(rom.path);
    if (!f) {
      initErr = GBC_ERR_READ;
      return false;
    }
    romSize = f.size();
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (romSize + 65536 > largest) {           // keep some PSRAM headroom
      f.close();
      dbgWhere = 3;                            // error screen shows both sizes
      dbgFreeInt = romSize;
      dbgLargest = largest;
      initErr = GBC_ERR_ROMBIG;
      return false;
    }
    s_emuRom = (uint8_t*)ps_malloc(romSize);
    if (!s_emuRom) {
      f.close();
      initErr = GBC_ERR_ALLOC;
      return false;
    }
    size_t rd = 0;
    while (rd < romSize) {
      int n = f.read(s_emuRom + rd, romSize - rd);
      if (n <= 0) {
        break;
      }
      rd += n;
    }
    f.close();
    if (rd != romSize) {                       // truncated read -> discard
      free(s_emuRom);
      s_emuRom = NULL;
      initErr = GBC_ERR_READ;
      return false;
    }
    romData = s_emuRom;
  } else {
    romData = gbc_test_rom;
    romSize = gbc_test_rom_len;
    snprintf(romName, sizeof(romName), "uCity");
  }

  if (!s_gnuboyInited) {
    s_emuAudio = (int16_t*)malloc(GBC_EMU_AUDIO_LEN * sizeof(int16_t));
    size_t fbBytes = (size_t)GBC_SCREEN_W * GBC_SCREEN_H * sizeof(uint16_t);
    s_emuFb[0] = (uint16_t*)ps_malloc(fbBytes);
    s_emuFb[1] = (uint16_t*)ps_malloc(fbBytes);
    if (!s_emuFb[0] || !s_emuFb[1] || !s_emuAudio) {
      dbgWhere = 1;
      dbgFreeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      dbgLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
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
  }

  if (gnuboy_load_rom(romData, romSize) < 0) {
    initErr = GBC_ERR_LOAD;
    return false;
  }
  s_romLoaded = true;
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
    // 1:1, but bounce rows through internal RAM in 6-row chunks: pushImage
    // straight from the PSRAM framebuffer CPU-feeds the SPI with per-pixel PSRAM
    // reads that thrash the cache BOTH cores share (measured: direct-PSRAM 1:1
    // ran at 75% game speed while the row-buffered Fill ran at 96%).
    uint16_t rowbuf[GBC_SCREEN_W * 6];   // 1.9KB on the blit task's stack
    const int xoff = (lcd.width()  - GBC_SCREEN_W) / 2;
    const int yoff = (lcd.height() - GBC_SCREEN_H) / 2;
    for (int y = 0; y < GBC_SCREEN_H; y += 6) {   // 144 rows = 24 whole chunks
      memcpy(rowbuf, fb + y * GBC_SCREEN_W, sizeof(rowbuf));
      lcd.pushImage(xoff, yoff + y, GBC_SCREEN_W, 6, rowbuf);
    }
    return;
  }
  // Scale row-by-row through a small buffer on this task's stack (internal RAM)
  // instead of a full-frame PSRAM scale buffer. The old triple PSRAM round-trip
  // (read fb, write 101KB, read it back for SPI) thrashed the cache BOTH cores
  // share and audibly slowed emulation on the other core. The 1.5x row map
  // repeats every 3rd output row, so a third of the rows reuse the previous scale.
  uint16_t rowbuf[GBC_FILL_W];   // 480 bytes
  const int xoff = (lcd.width()  - GBC_FILL_W) / 2;
  const int yoff = (lcd.height() - GBC_FILL_H) / 2;
  int prevSy = -1;
  for (int oy = 0; oy < GBC_FILL_H; oy++) {
    int sy = (oy * GBC_SCREEN_H) / GBC_FILL_H;
    if (sy != prevSy) {
      const uint16_t* src = fb + sy * GBC_SCREEN_W;
      for (int ox = 0; ox < GBC_FILL_W; ox++) {
        rowbuf[ox] = src[s_xmap[ox]];
      }
      prevSy = sy;
    }
    lcd.pushImage(xoff, yoff + oy, GBC_FILL_W, 1, rowbuf);
  }
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

// F1/F2 during play: nudge the codec volume. setVolumes() clamps each output to
// its own valid range, so stepping all three together is safe (the codec applies
// whichever one is active: loudspeaker / earpiece / headphones).
void GbcApp::adjustVolume(int delta) {
  if (!audio) {
    return;
  }
  int8_t ear, hp, loud;
  audio->getVolumes(ear, hp, loud);
  audio->setVolumes(ear + delta, hp + delta, loud + delta);
}

// Fill out[0..outLen-2] with the marquee window of name at scroll: the name
// cycles with a 3-space gap ("LONG NAME   LONG NAME..."), one char per tick.
static void gbcMarqueeWindow(const char* name, int scroll, char* out, int outLen) {
  int n = (int)strlen(name);
  int cycle = n + 3;
  int pos = scroll % cycle;
  for (int i = 0; i < outLen - 1; i++) {
    int p = (pos + i) % cycle;
    out[i] = (p < n) ? name[p] : ' ';
  }
  out[outLen - 1] = 0;
}

// Repaint only the selected row's text: called on each APP_TIMER_EVENT tick to
// advance the marquee without a full drawPicker (a full redraw flashes black).
void GbcApp::drawPickerRow() {
  SmoothFont* font = fonts[OPENSANS_COND_BOLD_20];
  const int lh = font->height() + 8;
  const int y = 40 + (romSel - romTop) * lh;
  char line[GBC_PICKER_VIS_CHARS + 1];
  gbcMarqueeWindow(roms[romSel - GBC_PICKER_ACTIONS].name, selScroll, line, sizeof(line));
  lcd.fillRect(28, y, lcd.width() - 28, font->height(), BLACK);
  lcd.setTextFont(font);
  lcd.setTextDatum(TL_DATUM);
  lcd.setTextColor(TFT_YELLOW, BLACK);
  lcd.drawString(line, 28, y);
}

void GbcApp::drawPicker() {
  SmoothFont* font = fonts[OPENSANS_COND_BOLD_20];
  lcd.fillScreen(BLACK);
  lcd.setTextFont(font);
  lcd.setTextDatum(TL_DATUM);
  lcd.setTextColor(TFT_GREENYELLOW, BLACK);
  lcd.drawString("Select a game", 8, 8);

  // Rows 0 and 1 are the "Transfer ROMs" / "Help" actions (always on top, no
  // scrolling past a long game list to reach them); ROM i is at row i + 2.
  const int total = romCount + 2;
  const int lh = font->height() + 8;
  const int top = 40;
  int visible = (lcd.height() - top - 26) / lh;
  if (visible < 1) {
    visible = 1;
  }
  if (romSel < romTop) {
    romTop = romSel;
  }
  if (romSel >= romTop + visible) {
    romTop = romSel - visible + 1;
  }

  int y = top;
  for (int i = romTop; i < total && i < romTop + visible; i++) {
    bool sel = (i == romSel);
    lcd.setTextColor(sel ? TFT_YELLOW : TFT_WHITE, BLACK);
    lcd.drawString(sel ? ">" : " ", 8, y);
    if (i >= GBC_PICKER_ACTIONS) {
      const char* nm = roms[i - GBC_PICKER_ACTIONS].name;
      char line[GBC_PICKER_VIS_CHARS + 1];
      if (sel && (int)strlen(nm) > GBC_PICKER_VIS_CHARS) {
        gbcMarqueeWindow(nm, selScroll, line, sizeof(line));   // scrolling window
      } else {
        snprintf(line, sizeof(line), "%s", nm);                // truncate to fit
      }
      lcd.drawString(line, 28, y);
    } else {                       // action rows, tinted to stand apart from games
      lcd.setTextColor(sel ? TFT_YELLOW : TFT_CYAN, BLACK);
      lcd.drawString(i == 0 ? "Transfer ROMs..." : "Help...", 28, y);
    }
    y += lh;
  }

  if (confirmDelete) {
    lcd.setTextColor(TFT_RED, BLACK);
    lcd.drawString("Delete this ROM?", 8, lcd.height() - 46);
    lcd.setTextColor(TFT_DARKGREY, BLACK);
    lcd.drawString("OK: delete   any: cancel", 8, lcd.height() - 22);
  } else {
    lcd.setTextColor(TFT_DARKGREY, BLACK);
    lcd.drawString("OK play  Back del  End exit", 8, lcd.height() - 22);
  }
}

// "Transfer ROMs" screen: instructions when the server is off, address +
// live "ROMs added" count while it's on. The server itself is app_gbc_xfer.cpp.
void GbcApp::drawXfer() {
  SmoothFont* font = fonts[OPENSANS_COND_BOLD_20];
  if (!xferClean) {              // full clear only on entry/layout change; the
    lcd.fillScreen(BLACK);       // 1Hz live refresh just overdraws the text
    xferClean = true;            // in place (a full clear flashed black visibly)
  }
  lcd.setTextFont(font);
  lcd.setTextDatum(TL_DATUM);
  int lh = font->height() + 4;
  int y = 8;

  lcd.setTextColor(TFT_GREENYELLOW, BLACK);
  lcd.drawString("Transfer ROMs", 8, y);
  y += lh + 4;

  lcd.setTextColor(TFT_WHITE, BLACK);
  if (!gbcXferOn()) {
    lcd.drawString("Put Game Boy ROMs on the", 8, y); y += lh;
    lcd.drawString("SD card from a computer:", 8, y); y += lh + 4;
    lcd.drawString("1. Connect phone to WiFi", 8, y); y += lh;
    lcd.drawString("2. Press OK to start", 8, y); y += lh;
    lcd.drawString("3. On your computer open", 8, y); y += lh;
    lcd.setTextColor(TFT_YELLOW, BLACK);
    lcd.drawString("   wiphone.local", 8, y); y += lh;
    lcd.setTextColor(TFT_WHITE, BLACK);
    lcd.drawString("4. Drag ROMs in or paste", 8, y); y += lh;
    lcd.drawString("   a link. Done!", 8, y); y += lh + 8;
    lcd.setTextColor(TFT_DARKGREY, BLACK);
    lcd.drawString("OK: start   Back: games", 8, lcd.height() - 24);
  } else {
    lcd.setTextColor(TFT_GREEN, BLACK);
    lcd.drawString("Server ON", 8, y); y += lh + 4;
    lcd.setTextColor(TFT_WHITE, BLACK);
    lcd.drawString("On your computer, open:", 8, y); y += lh;
    lcd.setTextColor(TFT_YELLOW, BLACK);
    lcd.drawString("  wiphone.local", 8, y); y += lh;
    char line[40];
    snprintf(line, sizeof(line), "  or http://%s", gbcXferAddr());
    lcd.drawString(line, 8, y); y += lh + 4;
    lcd.setTextColor(TFT_WHITE, BLACK);
    if (gbcXferUsingAP()) {
      lcd.drawString("(join WiFi 'WiPhone-ROMs'", 8, y); y += lh;
      lcd.drawString(" first, no password)", 8, y); y += lh + 4;
    } else {
      lcd.drawString("(same WiFi as the phone)", 8, y); y += lh + 4;
    }
    snprintf(line, sizeof(line), "ROMs added: %d   ", gbcXferRomsAdded());   // pad: drawn in place
    lcd.drawString(line, 8, y); y += lh;

    // Diagnostics (refreshed ~1Hz): free heap, its low-water mark, last reset
    // reason (6=task WDT, 4=panic, 9=brownout, 1=power on), WiFi status. Drawn
    // in the text flow — a fixed bottom position collided with the key hints.
    char diag[56];
    snprintf(diag, sizeof(diag), "mem %u min %u rst %d wf %d   ",   // pad: drawn in place
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
             (int)esp_reset_reason(), (int)WiFi.status());
    lcd.setTextColor(TFT_DARKGREY, BLACK);
    lcd.drawString(diag, 8, y);

    lcd.setTextColor(TFT_DARKGREY, BLACK);
    lcd.drawString("OK: stop   Back: games", 8, lcd.height() - 24);
  }
}

// Help lines. NULL-string entries render as section headers (green).
static const char* const s_helpLines[] = {
  "@CONTROLS (in game)",
  "D-pad: move",
  "Bottom right key: A",
  "Key above it: B",
  "Back (top right): Start",
  "Select (top left): Select",
  "Top 2 right keys: volume",
  "End (hang up): pause",
  "@PAUSE MENU",
  "Save state: bookmark the",
  " game exactly as it is",
  "Load state: jump back to",
  " the saved bookmark",
  "Screen 1:1 or Fill: size",
  "Number = game speed %",
  "Quit: back to the phone",
  "@ADDING GAMES",
  "Pick 'Transfer ROMs...'",
  "in the game list, then",
  "follow the steps shown.",
  "Files ending .gb / .gbc",
  "land in the game list.",
  "Back key deletes a game",
  "(asks first).",
  "@GOOD TO KNOW",
  "WiFi + calls are off",
  "while a game runs; quit",
  "the game to reconnect.",
  "Save states live on the",
  "SD card, one per game.",
  "Built-in game: uCity by",
  "AntonioND (GPL).",
};
static const int s_helpCount = sizeof(s_helpLines) / sizeof(s_helpLines[0]);

void GbcApp::drawHelp() {
  SmoothFont* font = fonts[OPENSANS_COND_BOLD_20];
  lcd.fillScreen(BLACK);
  lcd.setTextFont(font);
  lcd.setTextDatum(TL_DATUM);
  const int lh = font->height() + 4;
  const int top = 40;
  const int visible = (lcd.height() - top - 26) / lh;

  lcd.setTextColor(TFT_GREENYELLOW, BLACK);
  lcd.drawString("Game Boy help", 8, 8);
  if (helpTop > s_helpCount - visible) {
    helpTop = s_helpCount - visible;
  }
  if (helpTop < 0) {
    helpTop = 0;
  }
  int y = top;
  for (int i = helpTop; i < s_helpCount && i < helpTop + visible; i++) {
    const char* ln = s_helpLines[i];
    if (ln[0] == '@') {           // section header
      lcd.setTextColor(TFT_GREEN, BLACK);
      lcd.drawString(ln + 1, 8, y);
    } else {
      lcd.setTextColor(TFT_WHITE, BLACK);
      lcd.drawString(ln, 8, y);
    }
    y += lh;
  }
  lcd.setTextColor(TFT_DARKGREY, BLACK);
  lcd.drawString("Up/Down scroll   Back: games", 8, lcd.height() - 22);
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
  if (speedPct > 0) {              // measured game speed (100 = full speed)
    char spd[12];
    snprintf(spd, sizeof(spd), "%d%%", speedPct);
    lcd.setTextColor(speedPct >= 97 ? TFT_GREENYELLOW : TFT_ORANGE, TFT_DARKGREY);
    lcd.drawString(spd, bx + bw - 52, by + 10);
  }

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
        errno = 0;
        int r = gnuboy_save_state(path);
        if (r == 0) {
          snprintf(app->statusMsg, sizeof(app->statusMsg), "Saved");
        } else {   // r=-1 file open/write failed (errno says why), r=-2 buffer alloc failed
          snprintf(app->statusMsg, sizeof(app->statusMsg), "Save fail %d e%d", r, errno);
        }
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
  uint32_t winStart = start;      // rolling 1s window for the speed readout
  uint32_t winFrames = 0;
  int skip = 1;                   // adaptive frameskip: render every skip-th frame
  int skipPhase = 0;              // (heavy games: drop DISPLAY frames, not game speed)
  while (s_running) {
    if (app->paused) {
      vTaskDelay(pdMS_TO_TICKS(20));
      start = millis();           // reset the pacing baseline for resume
      frames = 0;
      winStart = start;
      winFrames = 0;
      continue;
    }
    gnuboy_set_pad(app->readPad());
    // Render only when the blit is free AND the frameskip phase comes up.
    // Scanline rendering runs on this core, so skipping it is the single
    // biggest relief for heavy games — the game logic stays at full speed
    // and only the display rate drops.
    bool willDraw = !s_blitBusy && (++skipPhase >= skip);
    if (willDraw) {
      skipPhase = 0;
    }
    gnuboy_set_framebuffer(s_emuFb[emuBuf]);
    gnuboy_run(willDraw);
    frames++;
    winFrames++;
    uint32_t winMs = millis() - winStart;
    if (winMs >= 1000) {          // GB runs 59.73 fps; ~60 emulated frames/s = 100%
      app->speedPct = (int)(winFrames * 100 * 1000 / 60 / winMs);
      winFrames = 0;
      winStart = millis();
      // Adapt the frameskip to hold 100% game speed: falling behind -> skip
      // more display frames (up to 2/3 dropped); comfortably at speed -> claw
      // display smoothness back.
      if (app->speedPct < 96 && skip < 3) {
        skip++;
      } else if (app->speedPct >= 99 && skip > 1) {
        skip--;
      }
    }
    if (willDraw) {
      s_blitIdx = emuBuf;
      s_blitBusy = true;
      xSemaphoreGive(s_blitGo);
      emuBuf = 1 - emuBuf;        // render the next frame into the other buffer
    }
    // Audio: push this frame's APU samples. Bounded wait: with the DMA full
    // (game ahead of the DAC) the write blocks ~one frame and becomes the de
    // facto clock, locked to the DAC with zero drift. If I2S misbehaves (wrong
    // rate after some phone sound reconfigured it, etc.) the timeout caps the
    // damage and the starve counter turns sound off entirely — silence beats
    // slow-motion (audio was throttling games to 13% before this guard).
    if (app->soundOn) {
      size_t n = gnuboy_get_audio_count();       // int16 count (stereo interleaved)
      if (n) {
        size_t want = n * sizeof(int16_t);
        size_t wrote = 0;
        i2s_write(I2S_NUM_0, s_emuAudio, want, &wrote, pdMS_TO_TICKS(25));
        if (wrote < want) {
          if (++app->audioStarve >= 60) {        // a full second of failed writes
            app->soundOn = false;
          }
        } else {
          app->audioStarve = 0;
        }
      }
    }
    // Wall-clock floor: never run faster than 60fps. When the blocking write
    // above already paced this frame, we're at/behind schedule and this is a
    // no-op; it always yields at least one tick for the main loop's keypad.
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

  if (!playing) {
    if (uiMode == UI_XFER) {
      // Transfer ROMs screen. OK toggles the server; Back returns to the list
      // (rescanning it so anything just uploaded appears immediately).
      if (LOGIC_BUTTON_OK(event)) {
        if (gbcXferOn()) {
          gbcXferStop();
        } else {
          gbcXferStart();
        }
        xferClean = false;        // layout changes: repaint from scratch
        return REDRAW_SCREEN;
      }
      if (LOGIC_BUTTON_BACK(event)) {
        gbcXferStop();
        scanRoms();
        if (romSel >= romCount + GBC_PICKER_ACTIONS) {
          romSel = romCount + GBC_PICKER_ACTIONS - 1;
        }
        uiMode = UI_PICKER;
        return REDRAW_SCREEN;
      }
      if (!IS_KEYBOARD(event) && gbcXferOn()) {
        // Live "ROMs added"/heap refresh — but at most once a second. Background
        // events can arrive much faster than 1Hz, and redrawing the whole screen
        // per event kept the main loop busy on SPI instead of pumping the web
        // server (the page then never loaded).
        uint32_t nowMs = millis();
        if (nowMs - xferDrawMs >= 1000) {
          xferDrawMs = nowMs;
          return REDRAW_SCREEN;
        }
      }
      return DO_NOTHING;
    }

    if (uiMode == UI_HELP) {
      switch (event) {
        case WIPHONE_KEY_UP:
          helpTop--;                            // drawHelp clamps
          return REDRAW_SCREEN;
        case WIPHONE_KEY_DOWN:
          helpTop++;
          return REDRAW_SCREEN;
        case WIPHONE_KEY_BACK:
        case WIPHONE_KEY_END:
          uiMode = UI_PICKER;
          return REDRAW_SCREEN;
        default:
          return DO_NOTHING;
      }
    }

    // ROM picker: rows 0/1 are Transfer/Help, ROM i sits at row i + 2.
    if (confirmDelete) {
      if (!IS_KEYBOARD(event)) {
        return DO_NOTHING;                      // ignore background ticks so the prompt stays up
      }
      if (event == WIPHONE_KEY_OK) {            // OK confirms; any other key cancels
        int r = romSel - GBC_PICKER_ACTIONS;
        if (r >= 0 && r < romCount && !roms[r].embedded) {
          SD.remove(roms[r].path);
        }
        scanRoms();
        if (romSel >= romCount + GBC_PICKER_ACTIONS) {
          romSel = romCount + GBC_PICKER_ACTIONS - 1;
        }
      }
      confirmDelete = false;
      return REDRAW_SCREEN;
    }
    if (!IS_KEYBOARD(event)) {
      // Advance the marquee if the selected game's name is too long to fit,
      // repainting only that row (full redraws flash black). Time-based, NOT
      // per-event: background events can arrive far faster than the 250ms app
      // timer (WiFi/SIP activity), which once spun the scroll unreadably fast.
      uint32_t nowMs = millis();
      if (nowMs - marqueeMs >= 250 &&
          romSel >= GBC_PICKER_ACTIONS &&
          (int)strlen(roms[romSel - GBC_PICKER_ACTIONS].name) > GBC_PICKER_VIS_CHARS) {
        marqueeMs = nowMs;
        selScroll++;
        drawPickerRow();
      }
      return DO_NOTHING;
    }
    switch (event) {
      case WIPHONE_KEY_UP:
        if (romSel > 0) { romSel--; }
        selScroll = 0;
        return REDRAW_SCREEN;
      case WIPHONE_KEY_DOWN:
        if (romSel < romCount + GBC_PICKER_ACTIONS - 1) { romSel++; }
        selScroll = 0;
        return REDRAW_SCREEN;
      case WIPHONE_KEY_OK:
        if (romSel == 0) {                      // "Transfer ROMs..."
          // Give the WiFi/web stack RAM to breathe: drop anything a previous
          // game left resident AND the unused BT controller's ~60KB reserve.
          // The phone idles at ~9KB free internal — the web server's TCP
          // buffers starve mid-page at that level (white half-loaded page).
          gbcReleaseEmulator();
          reclaimInternalRam();
          uiMode = UI_XFER;
          xferClean = false;                    // full screen draw on entry
          return REDRAW_SCREEN;
        }
        if (romSel == 1) {                      // "Help..."
          uiMode = UI_HELP;
          helpTop = 0;
          return REDRAW_SCREEN;
        }
        startGame();
        return initErr ? REDRAW_SCREEN : DO_NOTHING;   // tasks own the screen once playing
      case WIPHONE_KEY_BACK:                    // top-right button: delete an SD ROM
        if (romSel >= GBC_PICKER_ACTIONS && !roms[romSel - GBC_PICKER_ACTIONS].embedded) {
          confirmDelete = true;
          return REDRAW_SCREEN;
        }
        return DO_NOTHING;
      case WIPHONE_KEY_END:
        return EXIT_APP;
      default:
        return DO_NOTHING;
    }
  }

  if (!paused) {
    if (event == WIPHONE_KEY_END) {
      statusMsg[0] = 0;
      menuSel = GBC_MENU_RESUME;
      menuDirty = true;
      paused = true;
    } else if (event == WIPHONE_KEY_F1) {   // top user button: volume up
      adjustVolume(+GBC_VOL_STEP);
    } else if (event == WIPHONE_KEY_F2) {   // second user button: volume down
      adjustVolume(-GBC_VOL_STEP);
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
    if (!playing) {                    // picker/transfer/help until a game starts
      if (uiMode == UI_XFER) {
        drawXfer();
      } else if (uiMode == UI_HELP) {
        drawHelp();
      } else {
        drawPicker();
      }
    }
    return;                            // once playing, the emulator tasks own the screen
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
  } else if (initErr == GBC_ERR_ROMBIG) {
    msg = "ROM too big for RAM";
  } else if (initErr == GBC_ERR_READ) {
    msg = "SD read failed";
  } else if (initErr == GBC_ERR_TASK) {
    msg = "could not start task";
  }
  lcd.drawString(msg, 6, 40);
  lcd.setTextColor(WHITE, BLACK);
  int y = 40 + font->height() + 6;
  if ((initErr == GBC_ERR_ALLOC || initErr == GBC_ERR_ROMBIG) && dbgWhere) {
    char line[48];
    snprintf(line, sizeof(line), "where:%d free:%u", dbgWhere, (unsigned)dbgFreeInt);
    lcd.drawString(line, 6, y);
    y += font->height() + 6;
    snprintf(line, sizeof(line), "largest:%u", (unsigned)dbgLargest);
    lcd.drawString(line, 6, y);
    y += font->height() + 6;
  }
  lcd.drawString("Back/hang-up to exit", 6, y);
}
