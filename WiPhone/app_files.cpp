/*
 * app_files.cpp — see app_files.h for what this is and what it deliberately is not.
 *
 * Memory discipline (the whole design fits it):
 *   - the entry table is ONE PSRAM block, capped at FILES_MAX_ENTRIES — a folder with ten
 *     thousand files lists the first two hundred and SAYS it truncated;
 *   - the viewer buffer is ONE PSRAM block, capped at FILES_VIEW_MAX — a bigger file shows
 *     its head and says so;
 *   - menu widgets are PSRAM-backed already (AbstractWidget::operator new).
 *   Nothing here allocates from internal heap in a loop.
 */
#include "app_files.h"
#include "app_books.h"     // bookRenderRun + fontMeasure, shared with the e-reader
#include "config.h"

#include <Arduino.h>
#include <SD.h>
#include <stdarg.h>

static const int    FILES_MAX_ENTRIES = 200;
static const size_t FILES_VIEW_MAX    = 60 * 1024;
static const int    FILES_MARGIN      = 6;

// Menu keys. 0 is never used (readChosen's "nothing" value).
static const MenuOption::keyType ROW_UPLOAD    = 1;
static const MenuOption::keyType ROW_UP        = 2;
static const MenuOption::keyType ROW_PASTE     = 3;
static const MenuOption::keyType ROW_CLIPCLEAR = 4;
static const MenuOption::keyType ROW_FIRST     = 10;

// Options-menu keys.
static const MenuOption::keyType OPT_OPEN   = 1;
static const MenuOption::keyType OPT_COPY   = 2;
static const MenuOption::keyType OPT_MOVE   = 3;
static const MenuOption::keyType OPT_DELETE = 4;
static const MenuOption::keyType OPT_CANCEL = 5;
static const MenuOption::keyType DEL_NO     = 1;
static const MenuOption::keyType DEL_YES    = 2;

/* THE CLIPBOARD — file-scope statics, deliberately: "a persistent paste that stays until
 * I've pasted" should survive backing out of the app entirely. It is only a remembered
 * PATH: marking a file for Copy or Move touches NOTHING on the card. A Move becomes real
 * only when the paste succeeds — rename first (atomic on FAT), else copy + verify + only
 * then delete — so a mark that is never pasted can never break the file. */
static char    s_clipSrc[208] = {0};
static char    s_clipName[64] = {0};
static uint8_t s_clipMove = 0;         // 0 = nothing held; 1 = copy; 2 = move

/* File extensions the text viewer renders. Everything else opens as an info page rather
 * than a garbage dump — and a text file with a weird extension still gets sniffed in. */
static bool textishName(const char* name) {
  const char* dot = strrchr(name, '.');
  if (!dot) {
    return false;
  }
  static const char* exts[] = { ".txt", ".log", ".ini", ".md", ".cfg", ".conf", ".json",
                                ".csv", ".xml", ".cue", ".m3u", ".since", ".nfo" };
  for (unsigned i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
    if (!strcasecmp(dot, exts[i])) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------- lifecycle

FilesApp::FilesApp(LCD& disp, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(disp, state, header, footer) {
  log_d("create FilesApp");
  strcpy(curPath, "/");
  note[0] = '\0';
  entries = (FEntry*)ps_malloc(sizeof(FEntry) * FILES_MAX_ENTRIES);
  if (!entries) {
    log_e("FILES: no PSRAM for the entry table");
  }
  header->setTitle("Files");
  footer->setButtons("Select", "Back");
  scanDir();
  buildBrowse();
}

FilesApp::~FilesApp() {
  log_d("destroy FilesApp");
  if (appState == FILES_XFER) {
    xferStop();                    // never leave the server up with no screen owning it
  }
  closeView();
  delete menu;
  free(entries);
}

// ---------------------------------------------------------------- browsing

void FilesApp::scanDir() {
  entryCount = 0;
  truncated = false;
  if (!entries) {
    return;
  }
  File dir = SD.open(curPath);
  if (!dir || !dir.isDirectory()) {
    log_e("FILES: cannot open %s", curPath);
    if (dir) {
      dir.close();
    }
    return;
  }
  File f;
  while ((f = dir.openNextFile())) {
    // ⚠ On this core File::name() returns the FULL path — basename it ourselves.
    const char* n = f.name();
    const char* base = strrchr(n, '/');
    base = base ? base + 1 : n;
    if (base[0] && base[0] != '.') {           // hidden entries stay hidden
      if (entryCount >= FILES_MAX_ENTRIES) {
        truncated = true;
        f.close();
        break;
      }
      FEntry& e = entries[entryCount++];
      snprintf(e.name, sizeof(e.name), "%s", base);
      e.isDir = f.isDirectory();
      e.size = e.isDir ? 0 : (uint32_t)f.size();
    }
    f.close();
  }
  dir.close();

  // Dirs first, then case-insensitive alphabetical — so the listing is stable and a
  // folder never reshuffles under a repeat visit. Insertion sort: 200 entries max.
  for (int i = 1; i < entryCount; i++) {
    FEntry key = entries[i];
    int j = i - 1;
    while (j >= 0 && (( !entries[j].isDir && key.isDir) ||
                      (entries[j].isDir == key.isDir &&
                       strcasecmp(entries[j].name, key.name) > 0))) {
      entries[j + 1] = entries[j];
      j--;
    }
    entries[j + 1] = key;
  }
}

void FilesApp::buildBrowse() {
  delete menu;
  menu = new MenuWidget(0, header->height(), lcd.width(),
                        lcd.height() - header->height() - footer->height(),
                        "(empty folder)", fonts[AKROBAT_BOLD_18], 9, 8);
  menu->setStyle(MenuWidget::DEFAULT_STYLE, WHITE, BLACK, BLACK, GREEN);

  snprintf(headerTitle, sizeof(headerTitle), "%s", curPath);
  header->setTitle(headerTitle);

  if (note[0]) {
    menu->addOption(note, 0, 1);       // one-line result of the last operation; key 0 = inert
  }
  if (s_clipSrc[0]) {
    char prow[96];
    snprintf(prow, sizeof(prow), "[ Paste \"%s\" here ]", s_clipName);
    menu->addOption(prow, ROW_PASTE, 1);
    menu->addOption("[ Cancel paste ]", ROW_CLIPCLEAR, 1);
  }
  menu->addOption("[ Upload into this folder ]", ROW_UPLOAD, 1);
  if (strcmp(curPath, "/") != 0) {
    menu->addOption("[..]", ROW_UP, 1);
  }
  char label[80];
  for (int i = 0; i < entryCount; i++) {
    if (entries[i].isDir) {
      snprintf(label, sizeof(label), "%s/", entries[i].name);
    } else if (entries[i].size >= 10240) {
      snprintf(label, sizeof(label), "%s  %luK", entries[i].name,
               (unsigned long)(entries[i].size / 1024));
    } else {
      snprintf(label, sizeof(label), "%s  %luB", entries[i].name,
               (unsigned long)entries[i].size);
    }
    menu->addOption(label, (MenuOption::keyType)(ROW_FIRST + i), 1);
  }
  if (truncated) {
    menu->addOption("(more files not listed)", 0, 1);
  }
}

void FilesApp::fullPathOf(int idx, char* out, size_t cap) const {
  snprintf(out, cap, "%s%s%s", curPath,
           (strcmp(curPath, "/") == 0) ? "" : "/", entries[idx].name);
}

void FilesApp::setNote(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(note, sizeof(note), fmt, ap);
  va_end(ap);
}

void FilesApp::buildOptions() {
  delete menu;
  menu = new MenuWidget(0, header->height(), lcd.width(),
                        lcd.height() - header->height() - footer->height(),
                        NULL, fonts[AKROBAT_BOLD_18], 9, 8);
  menu->setStyle(MenuWidget::DEFAULT_STYLE, WHITE, BLACK, BLACK, GREEN);
  snprintf(headerTitle, sizeof(headerTitle), "%s", entries[selIdx].name);
  header->setTitle(headerTitle);
  menu->addOption("Open", OPT_OPEN, 1);
  menu->addOption("Copy", OPT_COPY, 1);
  menu->addOption("Move", OPT_MOVE, 1);
  menu->addOption("Delete...", OPT_DELETE, 1);
  menu->addOption("Cancel", OPT_CANCEL, 1);
}

void FilesApp::buildConfirmDel() {
  delete menu;
  menu = new MenuWidget(0, header->height(), lcd.width(),
                        lcd.height() - header->height() - footer->height(),
                        NULL, fonts[AKROBAT_BOLD_18], 9, 8);
  menu->setStyle(MenuWidget::DEFAULT_STYLE, WHITE, BLACK, BLACK, GREEN);
  snprintf(headerTitle, sizeof(headerTitle), "Delete %s?", entries[selIdx].name);
  header->setTitle(headerTitle);
  menu->addOption("Cancel", DEL_NO, 1);        // first, so a hasty OK is harmless
  menu->addOption("Yes, delete this file", DEL_YES, 1);
}

void FilesApp::markClipboard(int idx, bool move) {
  fullPathOf(idx, s_clipSrc, sizeof(s_clipSrc));
  snprintf(s_clipName, sizeof(s_clipName), "%s", entries[idx].name);
  s_clipMove = move ? 2 : 1;
  setNote("%s marked - open a folder and Paste", move ? "Move" : "Copy");
}

void FilesApp::pasteHere() {
  if (!s_clipSrc[0]) {
    return;
  }
  char dst[208];
  snprintf(dst, sizeof(dst), "%s%s%s", curPath,
           (strcmp(curPath, "/") == 0) ? "" : "/", s_clipName);

  if (!strcmp(dst, s_clipSrc)) {
    setNote("Already here");
    return;
  }
  if (SD.exists(dst)) {
    // Refuse rather than overwrite: clobbering a same-named file is how data dies.
    setNote("\"%s\" already exists here", s_clipName);
    return;
  }
  File src = SD.open(s_clipSrc, FILE_READ);
  if (!src) {
    // The source is gone (deleted, moved off-device). Nothing to hold anymore.
    setNote("Source is gone - paste cancelled");
    s_clipSrc[0] = '\0';
    s_clipMove = 0;
    return;
  }
  const uint32_t srcSize = (uint32_t)src.size();

  if (s_clipMove == 2) {
    // Try the atomic path first: FAT rename moves across folders on the same card.
    src.close();
    if (SD.rename(s_clipSrc, dst)) {
      setNote("Moved \"%s\"", s_clipName);
      s_clipSrc[0] = '\0';
      s_clipMove = 0;
      scanDir();
      return;
    }
    src = SD.open(s_clipSrc, FILE_READ);      // rename refused: fall back to copy+delete
    if (!src) {
      setNote("Move failed - file untouched");
      return;
    }
  }

  // Stream copy. On ANY failure the partial destination is removed and the source is
  // left exactly as it was — the mark-never-breaks-the-file rule.
  File out = SD.open(dst, FILE_WRITE);
  if (!out) {
    src.close();
    setNote("Cannot write here");
    return;
  }
  const size_t CHUNK = 8192;
  uint8_t* buf = (uint8_t*)ps_malloc(CHUNK);
  bool ok = (buf != NULL);
  uint32_t done = 0;
  int breather = 0;
  while (ok && done < srcSize) {
    size_t got = src.read(buf, CHUNK);
    if (!got) {
      ok = false;
      break;
    }
    if (out.write(buf, got) != got) {
      ok = false;
      break;
    }
    done += got;
    if ((++breather & 7) == 0) {
      vTaskDelay(1);                 // let WiFi/SIP breathe during a long copy
    }
  }
  free(buf);
  out.close();
  src.close();
  if (!ok || done != srcSize) {
    SD.remove(dst);                  // never leave a half-file lying around
    setNote("Copy failed - original untouched");
    return;
  }

  if (s_clipMove == 2) {
    // The copy is verified complete (byte count matched); only NOW may the source go.
    if (!SD.remove(s_clipSrc)) {
      setNote("Copied; could not remove original");
    } else {
      setNote("Moved \"%s\"", s_clipName);
    }
  } else {
    setNote("Pasted \"%s\"", s_clipName);
  }
  s_clipSrc[0] = '\0';
  s_clipMove = 0;
  scanDir();
}

void FilesApp::deleteEntry(int idx) {
  char path[208];
  fullPathOf(idx, path, sizeof(path));
  if (SD.remove(path)) {
    setNote("Deleted \"%s\"", entries[idx].name);
    if (!strcmp(path, s_clipSrc)) {
      s_clipSrc[0] = '\0';          // the clipboard cannot point at a ghost
      s_clipMove = 0;
    }
  } else {
    setNote("Could not delete \"%s\"", entries[idx].name);
  }
  scanDir();
}

void FilesApp::enterDir(const char* name) {
  size_t cur = strlen(curPath);
  // "/" + name, or path + "/" + name — bounded, and refuse rather than truncate a path:
  // a truncated path silently browses the WRONG directory.
  if (cur + 1 + strlen(name) + 1 >= sizeof(curPath)) {
    log_e("FILES: path too deep, not entering %s", name);
    return;
  }
  if (cur > 1) {
    strcat(curPath, "/");
  }
  strcat(curPath, name);
  note[0] = '\0';
  scanDir();
  buildBrowse();
}

void FilesApp::upDir() {
  char* slash = strrchr(curPath, '/');
  if (slash && slash != curPath) {
    *slash = '\0';
  } else {
    strcpy(curPath, "/");
  }
  note[0] = '\0';
  scanDir();
  buildBrowse();
}

// ---------------------------------------------------------------- viewing

void FilesApp::closeView() {
  if (viewText) {
    free(viewText);
    viewText = NULL;
  }
  viewLen = 0;
  viewPage = 0;
}

void FilesApp::openEntry(int idx) {
  if (idx < 0 || idx >= entryCount || entries[idx].isDir || !entries[idx].name[0]) {
    return;
  }
  closeView();
  snprintf(viewName, sizeof(viewName), "%s", entries[idx].name);

  char path[200];
  snprintf(path, sizeof(path), "%s%s%s", curPath,
           (strcmp(curPath, "/") == 0) ? "" : "/", entries[idx].name);

  const uint32_t size = entries[idx].size;
  const bool wantText = textishName(viewName);
  bool loaded = false;
  bool clipped = false;

  if (wantText) {
    File f = SD.open(path, FILE_READ);
    if (f) {
      size_t take = size;
      if (take > FILES_VIEW_MAX) {
        take = FILES_VIEW_MAX;
        clipped = true;
      }
      viewText = (char*)ps_malloc(take + 96);
      if (viewText) {
        size_t got = f.read((uint8_t*)viewText, take);
        // Strip CR in place: the layouter speaks '\n' and a CR would render as a glyph.
        size_t w = 0;
        bool binary = false;
        int suspicious = 0;
        for (size_t r = 0; r < got; r++) {
          char c = viewText[r];
          if (c == '\r') {
            continue;
          }
          if ((unsigned char)c == 0) {
            binary = true;
            break;
          }
          if ((unsigned char)c < 9) {
            if (++suspicious > 16) {       // a handful can be junk; a lot means binary
              binary = true;
              break;
            }
          }
          viewText[w++] = c;
        }
        if (binary) {
          free(viewText);
          viewText = NULL;
        } else {
          if (clipped) {
            w += snprintf(viewText + w, 90, "\n\n[showing the first %u KB of %lu KB]",
                          (unsigned)(FILES_VIEW_MAX / 1024), (unsigned long)(size / 1024));
          }
          viewLen = w;
          loaded = true;
        }
      }
      f.close();
    }
  }

  if (!loaded) {
    // The info page: every file opens to SOMETHING that says what it is and why there is
    // no more to see. Far better than a dead OK button or a screen of glyph soup.
    viewText = (char*)ps_malloc(256);
    if (!viewText) {
      return;
    }
    viewLen = snprintf(viewText, 256,
                       "%s\n\n%lu bytes\n\n%s",
                       viewName, (unsigned long)size,
                       wantText ? "This file could not be read as text."
                                : "No viewer for this file type.\n\nText files "
                                  "(.txt .log .ini .md .json ...) open as pages.");
  }
  viewPage = 0;
  appState = FILES_VIEW;
  snprintf(headerTitle, sizeof(headerTitle), "%s", viewName);
  header->setTitle(headerTitle);
  footer->setButtons("", "Back");
}

int FilesApp::viewLines() const {
  SmoothFont* f = fonts[AKROBAT_BOLD_18];
  const int top = header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  return (bottom - top - 4) / (f->height() + 2);
}

int FilesApp::viewWidth() const {
  return (int)lcd.width() - 2 * FILES_MARGIN;
}

// ---------------------------------------------------------------- uploading

void FilesApp::startUpload() {
  snprintf(xferDir, sizeof(xferDir), "%s", curPath);
  snprintf(xferHeading, sizeof(xferHeading), "Upload to %s", curPath);
  xferCfg.dir = xferDir;
  xferCfg.heading = xferHeading;
  xferCfg.accept = "";               // any file: this is the GENERAL uploader
  xferCfg.nounPlural = "files";
  xferCfg.defaultName = "download.bin";
  xferCfg.apName = "WiPhone-Files";
  xferStart(&xferCfg);
  appState = FILES_XFER;
  footer->setButtons("", "Stop");
}

// ---------------------------------------------------------------- events

appEventResult FilesApp::processEvent(EventType event) {
  if (appState == FILES_BROWSE) {
    if (LOGIC_BUTTON_BACK(event)) {
      if (strcmp(curPath, "/") == 0) {
        return EXIT_APP;
      }
      upDir();
      return REDRAW_ALL;
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      MenuOption::keyType sel = menu->readChosen();
      if (sel == ROW_UPLOAD) {
        startUpload();
        return REDRAW_ALL;
      } else if (sel == ROW_UP) {
        upDir();
        return REDRAW_ALL;
      } else if (sel == ROW_PASTE) {
        pasteHere();
        buildBrowse();
        return REDRAW_ALL;
      } else if (sel == ROW_CLIPCLEAR) {
        s_clipSrc[0] = '\0';
        s_clipMove = 0;
        setNote("Paste cancelled - nothing was changed");
        buildBrowse();
        return REDRAW_ALL;
      } else if (sel >= ROW_FIRST) {
        int idx = (int)(sel - ROW_FIRST);
        if (idx < entryCount) {
          if (entries[idx].isDir) {
            enterDir(entries[idx].name);
          } else {
            // A file asks what you want ("click a file and it asks"): Open / Copy /
            // Move / Delete. Folders still enter on one press - browsing stays fast.
            selIdx = idx;
            buildOptions();
            appState = FILES_OPTIONS;
          }
          return REDRAW_ALL;
        }
      }
    }
    return REDRAW_SCREEN;

  } else if (appState == FILES_OPTIONS) {
    if (LOGIC_BUTTON_BACK(event)) {
      appState = FILES_BROWSE;
      buildBrowse();
      return REDRAW_ALL;
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      MenuOption::keyType sel = menu->readChosen();
      if (sel == OPT_OPEN) {
        openEntry(selIdx);
        return REDRAW_ALL;
      } else if (sel == OPT_COPY || sel == OPT_MOVE) {
        markClipboard(selIdx, sel == OPT_MOVE);
        appState = FILES_BROWSE;
        buildBrowse();
        return REDRAW_ALL;
      } else if (sel == OPT_DELETE) {
        buildConfirmDel();
        appState = FILES_CONFIRM_DEL;
        return REDRAW_ALL;
      } else if (sel == OPT_CANCEL) {
        appState = FILES_BROWSE;
        buildBrowse();
        return REDRAW_ALL;
      }
    }
    return REDRAW_SCREEN;

  } else if (appState == FILES_CONFIRM_DEL) {
    if (LOGIC_BUTTON_BACK(event)) {
      appState = FILES_BROWSE;
      buildBrowse();
      return REDRAW_ALL;
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      MenuOption::keyType sel = menu->readChosen();
      if (sel == DEL_YES) {
        deleteEntry(selIdx);
      }
      appState = FILES_BROWSE;
      buildBrowse();
      return REDRAW_ALL;
    }
    return REDRAW_SCREEN;

  } else if (appState == FILES_VIEW) {
    if (LOGIC_BUTTON_BACK(event)) {
      closeView();
      appState = FILES_BROWSE;
      buildBrowse();
      footer->setButtons("Select", "Back");
      return REDRAW_ALL;
    }
    if (viewText && (event == WIPHONE_KEY_DOWN || event == WIPHONE_KEY_OK)) {
      BookMeasure m = { fonts[AKROBAT_BOLD_18], fontMeasure };
      BookPage pg;
      bookLayoutPage(viewText, viewLen, viewPage, viewWidth(), viewLines(), &m, &pg);
      if (pg.next < viewLen) {
        viewPage = pg.next;
      }
      return REDRAW_SCREEN;
    }
    if (viewText && event == WIPHONE_KEY_UP && viewPage > 0) {
      BookMeasure m = { fonts[AKROBAT_BOLD_18], fontMeasure };
      viewPage = bookLayoutPrevPage(viewText, viewLen, viewPage, viewWidth(), viewLines(), &m);
      return REDRAW_SCREEN;
    }
    return DO_NOTHING;

  } else {                            // FILES_XFER
    if (LOGIC_BUTTON_BACK(event) || LOGIC_BUTTON_OK(event)) {
      xferStop();
      appState = FILES_BROWSE;
      note[0] = '\0';
      scanDir();                      // whatever was uploaded should be visible NOW
      buildBrowse();
      footer->setButtons("Select", "Back");
      return REDRAW_ALL;
    }
    return REDRAW_SCREEN;             // any keypress refreshes the files-added count
  }
}

// ---------------------------------------------------------------- drawing

void FilesApp::redrawScreen(bool redrawAll) {
  if (appState == FILES_BROWSE || appState == FILES_OPTIONS || appState == FILES_CONFIRM_DEL) {
    drawBrowse(redrawAll);
  } else if (appState == FILES_VIEW) {
    drawView();
  } else {
    drawXfer();
  }
}

void FilesApp::drawBrowse(bool all) {
  if (menu) {
    ((GUIWidget*)menu)->redraw(lcd);
  }
}

void FilesApp::drawView() {
  SmoothFont* f = fonts[AKROBAT_BOLD_18];
  const int top = header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  const int lh = f->height() + 2;

  lcd.fillRect(0, top, lcd.width(), bottom - top, BLACK);
  if (!viewText) {
    return;
  }

  BookMeasure m = { f, fontMeasure };
  BookPage pg;
  bookLayoutPage(viewText, viewLen, viewPage, viewWidth(), viewLines(), &m, &pg);

  lcd.setTextFont(f);
  lcd.setTextDatum(TL_DATUM);
  lcd.setTextColor(WHITE, BLACK);
  char line[448];
  int y = top + 2;
  for (int i = 0; i < pg.nLines; i++) {
    if (!pg.lines[i].blank) {
      bookRenderRun(f, viewText + pg.lines[i].off, pg.lines[i].len, line, sizeof(line));
      lcd.drawString(line, FILES_MARGIN, y);
    }
    y += lh;
  }
}

void FilesApp::drawXfer() {
  SmoothFont* f = fonts[AKROBAT_BOLD_18];
  const int top = header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  const int lh = f->height() + 2;

  lcd.fillRect(0, top, lcd.width(), bottom - top, BLACK);
  lcd.setTextFont(f);
  lcd.setTextDatum(TL_DATUM);
  lcd.setTextColor(WHITE, BLACK);
  int y = top + 4;
  char line[96];
  snprintf(line, sizeof(line), "Uploading into %s", curPath);
  lcd.drawString(line, FILES_MARGIN, y); y += lh + 4;
  if (xferOn()) {
    lcd.drawString("On your computer, open:", FILES_MARGIN, y); y += lh;
    snprintf(line, sizeof(line), "  http://%s/", xferAddr());
    lcd.drawString(line, FILES_MARGIN, y); y += lh;
    lcd.drawString("  (or http://wiphone.local)", FILES_MARGIN, y); y += lh + 4;
    if (xferUsingAP()) {
      snprintf(line, sizeof(line), "Join hotspot: %s", xferApName());
      lcd.drawString(line, FILES_MARGIN, y); y += lh;
    }
    snprintf(line, sizeof(line), "Files added: %d", xferFilesAdded());
    lcd.drawString(line, FILES_MARGIN, y); y += lh + 4;
    lcd.setTextColor(GRAY_50, BLACK);
    lcd.drawString("Any key refreshes the count.", FILES_MARGIN, y); y += lh;
    lcd.drawString("Back stops the server.", FILES_MARGIN, y);
  } else {
    lcd.drawString("Server did not start.", FILES_MARGIN, y); y += lh;
    lcd.drawString("Check WiFi and try again.", FILES_MARGIN, y);
  }
}
