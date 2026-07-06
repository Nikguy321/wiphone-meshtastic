/*
 * app_gbc.h — WiPhone Game Boy Color emulator (Phase 3: playable)
 *
 * Runs the gnuboy core in its own FreeRTOS task: each frame it polls the held
 * keypad state, emulates one Game Boy frame, and blits the 160x144 image
 * centered on the screen. The main loop keeps running (so the keypad keeps
 * being scanned) while the game plays. Hang-up (END) quits back to the menu.
 *
 * Current input map (tell me to change any of these):
 *   D-pad -> D-pad     F4 (bottom-right key) -> A     F3 (above it) -> B
 *   BACK (top-right) -> Start            SELECT (top-left) -> Select
 *   END (hang-up) -> quit
 */

#ifndef APP_GBC_H
#define APP_GBC_H

#include "GUI.h"

#define GBC_SCREEN_W 160
#define GBC_SCREEN_H 144

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
  static void emuThread(void* pvParam);   // core 0: emulate + render into the back buffer
  static void blitTask(void* pvParam);    // core 1: push the finished buffer to the screen

  bool findRom(char* pathOut, size_t pathLen, char* nameOut, size_t nameLen);
  void reclaimInternalRam();     // free the (unused) BT controller RAM for the emulator
  bool setupEmulator();          // reclaim + gnuboy init + load ROM; false on failure
  int  readPad();                // map the held keypad state to a gb_pad bitmask
  void blitBuffer(int idx);      // push framebuffer[idx] to the screen, centered

  // Pause menu (hang-up during play): Resume / Save / Load / Quit.
  void drawPauseMenu();
  void buildStatePath(char* out, size_t n);

  int  initErr = 0;              // 0 = ok, <0 = setup error code
  char romName[40] = {0};

  // Shared between processEvent (core 1) and emuThread (core 0).
  volatile bool paused = false;
  volatile int  menuSel = 0;
  volatile bool menuDirty = false;
  volatile int  pendingAction = 0;   // 0 none, 1 save, 2 load
  volatile bool scaled = false;      // false = crisp 1:1, true = 1.5x fill-width
  volatile bool needsClear = false;  // clear the screen once (after a mode switch)
  char statusMsg[24] = {0};
};

#endif // APP_GBC_H
