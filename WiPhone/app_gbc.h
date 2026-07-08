/*
 * app_gbc.h — WiPhone Game Boy Color emulator
 *
 * On launch the app shows a ROM picker (the built-in game plus any .gb/.gbc on
 * the SD card). Selecting one enters "gaming mode" (WiFi/mesh off) and runs the
 * gnuboy core across both cores. Hang-up (END) opens an in-game pause menu.
 *
 * Input map:
 *   D-pad -> D-pad     F4 (bottom-right key) -> A     F3 (above it) -> B
 *   BACK (top-right) -> Start            SELECT (top-left) -> Select
 *   END (hang-up) -> pause menu / picker back
 */

#ifndef APP_GBC_H
#define APP_GBC_H

#include "GUI.h"

#define GBC_SCREEN_W 160
#define GBC_SCREEN_H 144
#define GBC_MAX_ROMS 48

class GbcApp : public ThreadedApp {
public:
  GbcApp(LCD& disp, ControlState& state);
  virtual ~GbcApp();

  ActionID_t getId() {
    return GUI_APP_GBC;
  };
  appEventResult processEvent(EventType event);
  void redrawScreen(bool redrawAll=false);

protected:
  static void emuThread(void* pvParam);   // emulate + render into the back buffer
  static void blitTask(void* pvParam);    // push the finished buffer to the screen

  // A selectable ROM: either the built-in game or a file on the SD card.
  struct Rom {
    char name[40];
    char path[100];
    bool embedded;
  };

  void scanRoms();               // build the picker list (built-in + SD /roms and root)
  void drawPicker();
  void drawPickerRow();          // repaint just the selected row (marquee tick)
  void drawXfer();               // "Transfer ROMs" screen (WiFi upload server)
  void drawHelp();               // scrollable help/instructions screen
  void startGame();              // enter gaming mode and launch the selected ROM

  void reclaimInternalRam();     // free the (unused) BT controller RAM for the emulator
  bool setupEmulator(const Rom& rom);   // gnuboy init + load the chosen ROM
  int  readPad();                // map the held keypad state to a gb_pad bitmask
  void blitBuffer(int idx);      // push framebuffer[idx] to the screen, centered

  // Pause menu (hang-up during play): Resume / Save / Load / Screen / Quit.
  void drawPauseMenu();
  void buildStatePath(char* out, size_t n);

  // Picker state (main thread only). The list shows the ROMs plus two action
  // rows appended after them: "Transfer ROMs" and "Help".
  enum UiMode { UI_PICKER, UI_XFER, UI_HELP };
  UiMode uiMode = UI_PICKER;
  int  helpTop = 0;              // first visible help line (scrolling)
  uint32_t xferDrawMs = 0;       // last live-refresh of the transfer screen
  bool xferClean = false;        // transfer screen background already drawn
                                 // (live refreshes skip the black fill: no flicker)
  Rom  roms[GBC_MAX_ROMS];
  int  romCount = 0;
  int  romSel = 0;
  int  romTop = 0;               // first visible row (scrolling)
  int  selScroll = 0;            // marquee offset (chars) for a long selected name
  bool confirmDelete = false;    // picker is asking to confirm a delete
  bool playing = false;          // false = picker on screen, true = game running
  bool enteredGaming = false;    // did we turn WiFi/mesh off? (restore on exit)
  volatile bool soundOn = false; // did we start the audio codec? (feed I2S + restore)
  int  audioStarve = 0;          // consecutive under-written I2S frames (emu thread only)

  void adjustVolume(int delta);  // F1/F2 in-game volume up/down

  int  initErr = 0;              // 0 = ok, <0 = setup error code
  char romName[40] = {0};
  // Diagnostics captured at an allocation failure, shown on the error screen.
  size_t dbgFreeInt = 0;         // free internal RAM
  size_t dbgLargest = 0;         // largest free internal block
  int    dbgWhere = 0;           // 1 = setupEmulator allocs, 2 = task stacks

  // Shared between processEvent and the emulator tasks.
  volatile bool paused = false;
  volatile int  menuSel = 0;
  volatile bool menuDirty = false;
  volatile int  pendingAction = 0;   // 0 none, 1 save, 2 load
  volatile bool scaled = false;      // false = crisp 1:1, true = 1.5x fill-width
  volatile bool needsClear = false;  // clear the screen once (after a mode switch)
  volatile int  speedPct = 0;        // measured game speed, % of real time (100 = full)
  char statusMsg[24] = {0};
};

#endif // APP_GBC_H
