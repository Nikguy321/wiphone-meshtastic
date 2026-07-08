/*
 * app_gbc_xfer.h — Game Boy ROM transfer web server (core).
 *
 * Turns the phone into a little upload server so you can put Game Boy ROMs on
 * the SD card from a computer without typing on the phone. Open
 * http://wiphone.local (or the shown IP) in a browser and drag-and-drop
 * .gb/.gbc files, or paste a direct download URL. Falls back to a WiFi hotspot
 * ("WiPhone-ROMs") when the phone isn't joined to a network. ROMs land in
 * /roms and then appear in the Game Boy picker.
 *
 * The UI lives inside the Game Boy app (GbcApp's "Transfer ROMs" screen);
 * this module only owns the server.
 */

#ifndef APP_GBC_XFER_H
#define APP_GBC_XFER_H

// Pumped from the main loop every iteration (no-op unless the server is running).
// Running the web server in the main loop context — rather than a task — keeps
// SD writes and the rest of the firmware's SPI use from colliding.
void gbcXferHandleClient();

void        gbcXferStart();       // bring the server up (join WiFi or make an AP)
void        gbcXferStop();        // stop it and drop the AP if we made one
bool        gbcXferOn();
bool        gbcXferUsingAP();     // true if we had to bring up our own hotspot
const char* gbcXferAddr();        // IP address to show next to wiphone.local
int         gbcXferRomsAdded();   // files added this session (uploads + fetches)

#endif // APP_GBC_XFER_H
