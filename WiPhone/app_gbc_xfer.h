/*
 * app_gbc_xfer.h — the phone's file-transfer web server (core).
 *
 * Turns the phone into a little upload server so you can put files on the SD card from a
 * computer without typing on the phone. Open http://wiphone.local (or the shown IP) in a
 * browser and drag-and-drop, or paste a direct download URL. Falls back to a WiFi hotspot
 * when the phone isn't joined to a network.
 *
 * ONE server, PARAMETERISED — not one per app. It began as the Game Boy ROM uploader and is
 * now also the book uploader. Copying it was the obvious move and the wrong one: the two
 * things that make it survive on hardware (feeding the task watchdog inside a long POST, and
 * one file per request) are exactly the kind of lesson that rots in a copy. There is also
 * only one port 80, so a second server object would be a bug waiting for someone to open
 * both screens.
 *
 * Each app supplies an XferConfig and owns its own screen; this module owns only the server.
 */

#ifndef APP_GBC_XFER_H
#define APP_GBC_XFER_H

struct XferConfig {
  const char* dir;          // where uploads land, e.g. "/books" (created if missing)
  const char* heading;      // page title, e.g. "Add books"
  const char* accept;       // browser file-picker filter, e.g. ".epub,.txt"
  const char* nounPlural;   // "ROMs" / "books" — used in the page text and the count
  const char* defaultName;  // filename for a URL fetch that has no usable one
  const char* apName;       // hotspot SSID when the phone has no network
};

// Pumped from the main loop every iteration (no-op unless the server is running).
// Running the web server in the main loop context — rather than a task — keeps SD writes and
// the rest of the firmware's SPI use from colliding.
void gbcXferHandleClient();

void        xferStart(const XferConfig* cfg);   // bring it up (join WiFi or make an AP)
void        xferStop();                         // stop it and drop the AP if we made one
bool        gbcXferOn();                        // named for the extern in WiPhone.ino
bool        xferUsingAP();                      // true if we had to bring up our own hotspot
const char* xferAddr();                         // IP address to show next to wiphone.local
const char* xferApName();                       // SSID of the hotspot, when we made one
int         xferFilesAdded();                   // files added this session (uploads + fetches)

// The Game Boy app's call sites, unchanged.
void gbcXferStart();
inline void gbcXferStop()         { xferStop(); }
inline bool xferOn()              { return gbcXferOn(); }
inline bool gbcXferUsingAP()      { return xferUsingAP(); }
inline const char* gbcXferAddr()  { return xferAddr(); }
inline int  gbcXferRomsAdded()    { return xferFilesAdded(); }

#endif // APP_GBC_XFER_H
