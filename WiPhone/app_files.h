/*
 * app_files.h — a general, lightweight SD file browser + viewer + uploader.
 *
 * Three screens in one app, deliberately minimal:
 *   BROWSE  folders and files, dirs first, bounded listing (no unbounded heap on a phone
 *           that fights for every contiguous kilobyte).
 *   VIEW    text-ish files, paged with the SAME layout engine and advance-corrected
 *           measure the e-reader uses — so wrapping is correct by construction. A file the
 *           viewer will not render (binary, or huge) still opens: it gets an info page
 *           (name, size, why) instead of a garbage dump.
 *   XFER    the one parameterised upload server (app_gbc_xfer), pointed at the folder you
 *           are standing in. This is the piece that makes the browser more than a toy:
 *           until now /roms and /books were the only folders reachable over WiFi at all.
 *
 * ⚠ NO DELETE in this version, on purpose. A file-delete bug is how a working device
 * loses data; deletion can arrive later behind a confirm flow like the phonebook's.
 */
#ifndef APP_FILES_H
#define APP_FILES_H

#include "GUI.h"
#include "book_layout.h"
#include "app_gbc_xfer.h"    // XferConfig: the one parameterised upload server

class FilesApp : public WindowedApp {
public:
  FilesApp(LCD& disp, ControlState& state, HeaderWidget* header, FooterWidget* footer);
  virtual ~FilesApp();
  ActionID_t getId() {
    return GUI_APP_FILES;
  }
  appEventResult processEvent(EventType event);
  void redrawScreen(bool redrawAll = false);

protected:
  typedef enum {
    FILES_BROWSE,
    FILES_OPTIONS,      // OK on a file: Open / Copy / Move / Delete / Cancel
    FILES_CONFIRM_DEL,  // Cancel (default) / Yes, delete
    FILES_VIEW,
    FILES_XFER,
  } FilesState_t;

  struct FEntry {
    char     name[64];
    uint32_t size;
    bool     isDir;
  };

  FilesState_t appState = FILES_BROWSE;

  // ---- browsing
  char        curPath[128];
  char        headerTitle[64];      // HeaderWidget keeps the POINTER — must outlive the call
  MenuWidget* menu = NULL;
  FEntry*     entries = NULL;       // PSRAM
  int         entryCount = 0;
  bool        truncated = false;

  // ---- viewing
  char*    viewText = NULL;         // PSRAM
  size_t   viewLen = 0;
  uint32_t viewPage = 0;
  char     viewName[64];

  // ---- uploading
  XferConfig xferCfg;
  char       xferDir[128];
  char       xferHeading[80];

  int  selIdx = -1;                 // the entry the options/confirm menus are about
  char note[96];                    // one-line result message shown atop the browse list

  void scanDir();
  void buildBrowse();
  void buildOptions();
  void buildConfirmDel();
  void enterDir(const char* name);
  void upDir();
  void fullPathOf(int idx, char* out, size_t cap) const;
  void openEntry(int idx);
  void closeView();
  void startUpload();
  void markClipboard(int idx, bool move);
  void pasteHere();
  void deleteEntry(int idx);
  void setNote(const char* fmt, ...);
  void drawBrowse(bool all);
  void drawView();
  void drawXfer();
  int  viewLines() const;
  int  viewWidth() const;
};

#endif // APP_FILES_H
