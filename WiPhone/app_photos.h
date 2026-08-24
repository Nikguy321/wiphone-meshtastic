/*
 * app_photos.h — a photo viewer for the SD card, and the one place wallpaper is chosen.
 *
 * Three screens:
 *   LIST     everything in /photos this phone can actually decode, newest name order, with
 *            size and a lock marker. Non-decodable files are NOT listed — a viewer that
 *            offers you a file and then shows a grey rectangle is worse than one that does
 *            not offer it.
 *   VIEW     one photo, scaled to fit, LEFT/RIGHT to walk the folder (wrapping).
 *   OPTIONS  OK on a photo: wallpaper / rename / lock / delete.
 *
 * ── FORMATS, AND WHY THIS LIST AND NOT A LONGER ONE ──────────────────────────────────────
 *   .jpg .jpeg   baseline colour, via the ESP32's ROM TJpgDec (what drawImage already uses)
 *                ...and baseline GREYSCALE, via jpeg_grey.cpp, which exists because the ROM
 *                decoder flatly refuses greyscale — the e-reader hit that first.
 *   .bmp         24- and 32-bit uncompressed, decoded here. Cheap to support and it is what
 *                screenshots and simple exports tend to be.
 *
 * ⚠ **PNG IS DELIBERATELY NOT SUPPORTED**, and it is the one people will ask about. There is
 * no PNG decoder in this firmware or in the ESP32 ROM; adding one means vendoring a library
 * plus a line buffer's worth of RAM on a phone that fights for every contiguous kilobyte, and
 * it would buy no colour a JPEG cannot already show. If it is ever added, `photoDraw()` is
 * the single function that needs to learn about it.
 *
 * ── THE DEFAULT WALLPAPER CANNOT BE LOST, BY CONSTRUCTION ────────────────────────────────
 * It is not a file. GUI's loader tries /background.jpg on the SD card, then the same path in
 * SPIFFS, and falls back to `image_i256` — an array COMPILED INTO THE FIRMWARE. So "restore
 * the default" is not a copy operation that could fail: it deletes the override and lets the
 * fallback surface. Nothing the user can reach can destroy it.
 */
#ifndef APP_PHOTOS_H
#define APP_PHOTOS_H

#include "GUI.h"

class PhotosApp : public WindowedApp {
public:
  PhotosApp(LCD& disp, ControlState& state, HeaderWidget* header, FooterWidget* footer);
  virtual ~PhotosApp();
  ActionID_t getId() {
    return GUI_APP_PHOTOS;
  }
  appEventResult processEvent(EventType event);
  void redrawScreen(bool redrawAll = false);

protected:
  typedef enum {
    PHOTOS_LIST,
    PHOTOS_VIEW,
    PHOTOS_OPTIONS,
    PHOTOS_CONFIRM_DEL,
    PHOTOS_RENAME,
  } PhotosState_t;

  struct PEntry {
    char     name[64];
    uint32_t size;
    bool     locked;
  };

  PhotosState_t appState = PHOTOS_LIST;

  PEntry*     entries = NULL;        // PSRAM: 200 x ~72 B is not internal-RAM money
  int         entryCount = 0;
  bool        truncated = false;
  int         sel = 0;               // index into entries[], shared by list and viewer

  MenuWidget* menu = NULL;
  MultilineTextWidget* textArea = NULL;   // rename
  char        headerTitle[64];       // HeaderWidget keeps the POINTER - must outlive the call
  char        note[80];              // one-line result of the last action, shown in the list

  void  scanDir();
  void  buildList();
  void  buildOptions();
  void  buildConfirmDelete();
  void  buildRename();
  void  enterState(PhotosState_t st);

  bool  drawCurrentPhoto();          // decode + blit the selected photo, scaled to fit
  void  step(int delta);             // walk the folder, wrapping

  bool  setAsWallpaper();
  bool  restoreDefaultWallpaper();
  bool  doRename(const char* newName);
  bool  doDelete();
  void  toggleLock();

  // Lock list: a sidecar file, because a lock is the user's intent and nothing can rederive it
  void  loadLocks();
  void  saveLocks();
  bool  isLocked(const char* name) const;
};

#endif // APP_PHOTOS_H
