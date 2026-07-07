/*
 * app_gbc_xfer.h — WiPhone "Transfer ROMs" screen.
 *
 * Turns the phone into a little upload server so you can put Game Boy ROMs on
 * the SD card from a computer without typing on the phone. Toggle the server on,
 * then open http://wiphone.local (or the shown IP) in a browser and drag-and-drop
 * a .gb/.gbc file, or paste a direct download URL. Falls back to a WiFi hotspot
 * ("WiPhone-ROMs") when the phone isn't joined to a network. ROMs land in /roms
 * and then appear in the Game Boy picker.
 */

#ifndef APP_GBC_XFER_H
#define APP_GBC_XFER_H

#include "GUI.h"

// Pumped from the main loop every iteration (no-op unless the server is running).
// Running the web server in the main loop context — rather than a task — keeps
// SD writes and the rest of the firmware's SPI use from colliding.
void gbcXferHandleClient();

class GbcXferApp : public WiPhoneApp {
public:
  GbcXferApp(LCD& disp, ControlState& state);
  virtual ~GbcXferApp();

  ActionID_t getId() {
    return GUI_APP_GBC_XFER;
  };
  appEventResult processEvent(EventType event);
  void redrawScreen(bool redrawAll=false);

protected:
  void startServer();
  void stopServer();

  bool serverOn = false;
  bool usingAP  = false;      // true if we had to bring up our own hotspot
  char addr[40] = {0};        // shown address (wiphone.local / IP / AP)
};

#endif // APP_GBC_XFER_H
