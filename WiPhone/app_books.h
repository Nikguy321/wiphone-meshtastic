/*
 * app_books.h — the WiPhone e-reader.
 *
 * The screen on top of the machinery that was already built and proven: epub_parse (what a
 * book says), book_layout (where the pages break), bookstore (where you were), booksync (how
 * COVEY hears about it). This file owns only what a person sees and presses.
 *
 * Library -> open -> paged, word-wrapped text. Position is (spine, offset) and is written to
 * the SD card, so it survives the phone dying with no warning — which it can, at any instant,
 * because the power rail is held by a software latch.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * WHY THE SCREEN TIMEOUT IS CHANGED WHILE A BOOK IS OPEN
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * The phone dims after 20 s and sleeps after 30 s of no key press. A page takes longer than
 * that to read, so the default settings turn the screen off mid-sentence, every page. While a
 * book is open the reader stretches both timeouts and restores the user's own values on the
 * way out (including from the destructor, so an unexpected teardown cannot leave the phone
 * with a five-minute screen timeout).
 */

#ifndef APP_BOOKS_H
#define APP_BOOKS_H

#include "GUI.h"
#include "SD.h"
#include "epub_parse.h"
#include "book_layout.h"
#include "bookstore.h"
#include "booksync_inbox.h"

#define BOOKS_MAX        48       // books listed from the SD card
#define BOOKS_HIST       48       // remembered page starts: exact back-paging while reading
#define BOOKS_DIR        "/books"
#define BOOKS_POS_FILE   "/books/positions.cbs"

class BooksApp : public WindowedApp {
public:
  BooksApp(LCD& disp, ControlState& state, HeaderWidget* header, FooterWidget* footer);
  virtual ~BooksApp();

  ActionID_t getId() {
    return GUI_APP_BOOKS;
  };
  appEventResult processEvent(EventType event);
  void redrawScreen(bool redrawAll=false);

protected:
  typedef enum {
    BOOKS_LIB,        // the library list
    BOOKS_MANAGE,     // delete a book you have finished with
    BOOKS_XFER,       // "Add books over WiFi" (the shared transfer server)
    BOOKS_HELP,
    BOOKS_READ,       // a page of the book
    BOOKS_MENU,       // the menu over a page: chapters, text size, info, close
    BOOKS_TOC,        // chapter list
    BOOKS_INFO,       // title / ids / fingerprint — what sync matches on
    BOOKS_PICTURE,    // one picture, full screen
    BOOKS_SYNCCARD,   // "COVEY is at 43% — go there?" — never taken automatically
    BOOKS_SYNCSET,    // the shared passcode and this device's name
    BOOKS_SYNCEDIT,   // typing one of those two
  } BooksState_t;

  // A file the library found. Metadata is NOT read here: opening 90 spine items to draw a
  // list would make the library take seconds to appear.
  struct BookFile {
    char name[64];               // basename, shown in the list
    char path[104];              // full SD path
    bool isTxt;
  };

  BooksState_t appState;
  MenuWidget*  menu;             // the active list, when a state has one
  MultilineTextWidget* textArea; // the editor, when typing a passcode or device name
  bool         editingPass;      // which of the two BOOKS_SYNCEDIT is editing

  /* ---- library
   *
   * ⚠ In PSRAM, not inline in the object. `new BooksApp` comes off the INTERNAL heap, and on
   * this phone that heap runs at about 16 KB free with WiFi and SIP up — measured, not
   * guessed. An 8 KB array of filenames sitting in it took the margin below what the WiFi
   * PHY needs to allocate its RF calibration data on a periodic re-scan, and phy_init calls
   * abort() when that fails: the phone rebooted a minute or two after a book was opened,
   * pointing at nothing in particular. Anything here that is more than a few hundred bytes
   * belongs in PSRAM, where there are megabytes spare. */
  BookFile* books;
  int      bookCount;
  int      pendingDelete;        // index of the book a second OK would delete, or -1
  bool     xferClean;            // transfer screen already painted (live refresh skips the fill)
  int      helpTop;
  char     libNote[96];          // a message shown as a row in the library (e.g. open failed)
  /* ⚠ HeaderWidget::setTitle KEEPS THE POINTER — it does not copy the string. So the book's
   * title has to live somewhere that outlives the call; a local buffer leaves the header
   * pointing at a dead stack frame. */
  char     headerTitle[64];

  // ---- the open book
  bool      isOpen;
  File      file;
  EpubSource src;
  EpubBook  book;
  int       openIdx;             // index into books[]
  char*     chapText;            // PSRAM: the current chapter, flattened to text
  size_t    chapLen;
  int       chapCap;             // what we actually managed to allocate
  int       spine;

  /* Pictures in the current chapter. Sizes come from the file HEADERS at chapter load (a few
   * KB each, not the whole image), because the layout must know how many rows to reserve
   * before anything is drawn. The image DATA is only read while it is being drawn — a 1 MB
   * cover does not get to sit in RAM all session. */
  EpubImage*    images;                      // PSRAM, for the reason above
  BookImageBox* imgBoxes;                    // the same, in the units the layout wants
  int          nImages;
  bool         imgGrey[EPUB_MAX_IMAGES];     // greyscale JPEG: the ROM decoder refuses these
  int          viewImage;                    // index shown full-screen, or -1
  uint8_t      pageImageKey[EPUB_MAX_IMAGES];  // number key that opens image i, 0 = not on this page
  uint32_t  pageStart;
  uint32_t  hist[BOOKS_HIST];    // page starts behind us, for exact back-paging
  int       histN;
  int       fontIdx;             // index into BODY_FONTS
  int       turnsSinceSave;

  // ---- position store
  BookStore*  store;
  BookStoreIo storeIo;
  char        ids[BOOKSYNC_MAX_IDS][BOOKSYNC_ID_MAX];
  int         nIds;

  /* What was on the SD card when this book was opened. Captured at open, before anything is
   * written, so it is literally the stored position — which is what makes "did my place
   * survive the reboot?" answerable on the device instead of on faith. Shown in Book info. */
  bool     openedFromSaved;
  uint32_t openedSpine;
  int      openedPct;

  /* ---- sync
   *
   * Two secrets, not one: the `booksync` Meshtastic CHANNEL carries the packet (its PSK is
   * the transport, imported like any other channel), and the PASSCODE below is the HMAC key
   * that says the position is really from you. Both must match COVEY's or nothing happens —
   * silently, on both devices, by design. */
  char     syncPass[24];         // the shared booksync passcode, in NVS
  char     syncDev[20];          // what this device calls itself in a position
  BookSyncRecord pending;        // a parked position that matches the open book
  uint32_t pendingFrom;          // which node sent it
  int      pendingIdx;           // its slot in the inbox, or -1 for none
  bool     pendingClock;         // its clock looks wrong (see bookSyncSuspectClock)
  char     syncNote[64];         // result of the last send, shown in the menu

  // ---- screen timeout, borrowed while reading
  bool     timeoutsHeld;
  uint32_t savedDimMs;
  uint32_t savedSleepMs;
  void holdScreenAwake(bool hold);

  void freeWidgets();
  void enterState(BooksState_t state);
  void setHeaderToBookTitle();   // substituted and cut to fit around the clock and icons
  MenuWidget* newMenu(const char* emptyMessage, SmoothFont* font, uint8_t perScreen);

  void scanBooks();
  bool isStray(int i) const;     // a book living outside /books (e.g. via the ROM uploader)
  void buildLibrary();
  void buildManage();
  void buildMenu();
  void buildToc();
  void buildSyncSettings();
  void buildSyncEdit();

  bool openBook(int idx);
  void closeBook(bool save);
  bool loadChapter(int i);       // pull spine item i into chapText
  void savePosition(bool flush);
  double fractionHere() const;

  // Paging. nextPage()/prevPage() step chapters at the ends of one.
  int  pageLines() const;        // rows of body text that fit
  int  pageWidth() const;
  void nextPage();
  void prevPage();
  void gotoOffset(uint32_t off, bool clearHistory);

  void drawPage();
  void drawHelp();
  void drawXfer();
  void drawInfo();
  void drawPicture();            // the full-screen view
  void drawSyncCard();           // the confirm card for an incoming position

  void loadSyncSettings();
  void saveSyncSettings();
  bool sendMyPlace();            // broadcast where I am on the booksync channel
  void checkForPending();        // is a parked position about the open book?
  void applyPending();           // take the jump, once confirmed

  void loadChapterImages();      // sizes and row heights for the current chapter
  int  imageRows(int i) const;   // rows of text height picture i occupies inline
  bool drawOneImage(int i, int x, int y, uint16_t boxW, uint16_t boxH);
};

#endif // APP_BOOKS_H
