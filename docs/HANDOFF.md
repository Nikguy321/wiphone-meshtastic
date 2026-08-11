# WiPhone — session handoff

**Last updated:** 2026-08-11 · **Next up:** flash it, read a chapter, then wire up sync.

Read this first to resume. One command tells you the codebase is healthy:

```bash
./tests/run_tests.sh
```

Expect **608 assertions, 0 failures** across four suites. It compiles the phone's own sources
with the host compiler under ASan+UBSan — no PlatformIO, no ESP32, no phone attached.

---

## ▶ PICK UP HERE — the reader is built and UNFLASHED

**There is a Books entry in the main menu now** and everything under it works as far as a host
compiler can tell. What has never happened: any of it running on the phone. Nobody has seen a
page of text on that screen.

### The one blocking step: flash it
`pio run` is green (**RAM 27.4%, flash 31.3%**) but the build has not been written to the phone.
That needs the USB cable — as of 2026-08-11 no serial device was present on the Mac
(`ls /dev/cu.usbserial-*` empty). See [[wiphone-flashing]]; ⚠ this unit's adapter **fails above
230400 baud**, so drive esptool directly rather than `pio run -t upload`.

### The test book is ALREADY ON THE SD CARD
`Ghosts_of_Timkovichi.epub` (5,060,061 bytes) was pushed over WiFi on 2026-08-11 and sits at
**`/roms/Ghosts_of_Timkovichi.epub`** — not `/books`. That is not a mistake to correct: the only
uploader the phone was running at the time was the Game Boy one, whose server has **no extension
filter at all** (the `accept=` attribute is a browser hint). The Books app scans `/roms` and the
card root as well as `/books` for exactly this reason, and **Manage** can move it.

**Proof it is the right file:** open it and check **Book info** shows `fp:2f6a8bc9d41ee898`. That
fingerprint is sha1(size + first 64 KB + last 64 KB), computed independently on the Mac, so a
match means the 5 MB arrived byte-for-byte.

### What to check first, in this order
1. **Menu > Books lists the book.** If the library is empty, the SD scan or the card is the
   problem, not the reader.
2. **Open it.** Expect the title page. 90 chapters, real titles from the NCX ("Prologue",
   "1. Monkeys with ’Mechs"), ~11 lines a page, ~34 characters a line at the small size.
3. **Page down and back up.** Nothing should be skipped or repeated at a boundary — that was a
   real bug, found by paging a real chapter backwards on the host, and it is now pinned.
4. **Close the book, reopen it.** It must land on the same page. That is the whole feature.
5. **Leave it sitting on a page for two minutes.** The screen must not sleep.

### Then: sync
- **Mesh wiring** — divert `CBS1 ` before it reaches Chats using `bookSyncIsSyncText()` (checked
  BEFORE the mac, deliberately: a packet signed with someone else's passcode is still not a chat
  message). Find the `booksync` channel **by name** and send with `sendChannelMessage(hash, text)`.
  ⚠ **Never fall back to the primary channel** — a position broadcast on LongFast is readable by
  every node in range.
- **A confirm card, never an automatic jump** (D-089): clock skew makes "newest wins" dangerous.
  `bookSyncSuspectClock()` is there to flag it.
- **Park late packets** for the next time that book is opened. COVEY does this because a LoRa round
  trip does not fit inside its 15 s sync window, and it listens continuously. That is what makes
  "sync now, pick it up later" work.
- **Passcode + device name storage** — planned for NVS under the existing `wpmesh` namespace (same
  place as the mesh node name), with an edit screen under **My node**. Not yet built.

---

## What the reader does, and the three things worth knowing before changing it

`app_books.{h,cpp}` is a `WindowedApp`: library → reader → menu (chapters / text size / book info
/ close), plus **Add books over WiFi** and **Manage** (delete, asks first). The library scans
`/books`, `/roms` and `/`.

**1. Pagination is a separate, testable module.** `book_layout.{h,cpp}` takes the text and a
width-measuring CALLBACK, so wrapping, paragraph gaps, UTF-8 and page-back are all exercised on a
Mac with a fixed-width stub. The phone supplies `SmoothFont::textWidth`. Keep it that way: the
only thing left in the app is drawing the lines it hands back.

⚠ **What page-back promises is CONTINUITY, not identity.** Greedy wrapping restarted at a
different offset can stay permanently out of phase, so a cold page-back may reflow the lines.
What it guarantees is that the page ENDS where the current one begins and is a full page. Exact
back-paging through pages you just read comes from the app's own history stack, not from
re-deriving them. Do not "fix" this by asserting identity — the algorithm cannot keep that
promise, and an earlier version of the test asserted it and passed only by luck.

**2. Position is (spine, byte offset) and text size does not move it.** Changing the font
reflows the page and keeps your place, which is the same property that lets COVEY understand a
position from here. ⚠ The offset is a BYTE offset into the extracted text; COVEY counts
CHARACTERS. They agree for ASCII and drift on curly quotes and em dashes — pre-existing,
documented in `epub_parse.h`, and absorbed by the whole-book fraction. It is one more reason a
synced jump is confirmed rather than taken silently.

**3. The transfer server is SHARED, not copied.** One `XferConfig` per app, one port 80, one copy
of the two lessons that keep it alive on hardware. `xferStart()` stops and restarts if another
app asks for a different folder. If you add a third uploader, add a config — do not add a server.

### Chapter titles are now in the vectors too
`gen_epub_vectors.py` emits `chapter_title()` per spine item, so the nav/NCX reading is checked
against COVEY's output rather than a second reading of the fixture — `epub3-nav.epub` covers the
EPUB3 form and `epub2-subdir.epub` the EPUB2 NCX. ⚠ Regenerating the vectors also REBUILDS the
fixture zips, so their `fp:` fingerprints change; that is expected, and the vectors and fixtures
must be committed together.

One difference from COVEY is deliberate: for a nested NCX this takes the entry's own label where
COVEY concatenates its children's too. They agree on a flat table of contents, which is what the
fixtures and the test book have. Titles never travel — only the spine INDEX does.

---

## What is done

| Module | State |
|---|---|
| `book_hash.{h,cpp}` | SHA-256, SHA-1, HMAC, base32, UTF-8 truncation |
| `booksync.{h,cpp}` | full wire protocol — **320 assertions** |
| `epub_parse.{h,cpp}` | zip, inflate, OPF, spine, XHTML→text, ids, fraction/locate, nav/NCX titles — **141** |
| `bookstore.{h,cpp}` | reading positions — **57** |
| `book_layout.{h,cpp}` | pages, wrapping, page-back — **90** |
| `html_entities.h` | 2125 entities, generated |
| `app_books.{h,cpp}` | the reader. Compiles and runs nowhere yet — see PICK UP HERE |
| `app_gbc_xfer.{h,cpp}` | the shared upload server, one `XferConfig` per app |

Crypto is self-contained rather than mbedtls **on purpose**: the host tests have to exercise the
code that actually ships, and an mbedtls backend would mean testing one implementation and
shipping another. ~2 KB of flash.

Interop vectors are **generated by running COVEY's real Python** (`tools/gen_*.py`), so they are
ground truth, not a second reading of the spec. Re-run them if COVEY's `booksync.py` / `epub.py`
ever change — the C tests then fail loudly instead of the two devices drifting apart in silence.

---

## ⚠ Traps — every failure mode in this feature is SILENT

COVEY drops a bad MAC without a word (telling an unauthenticated peer why it failed builds an
oracle), so a wrong implementation and a wrong passcode look identical: nothing logged, nothing
sent back, sync simply does not happen.

1. **The mesh mac is computed over the REDUCED record**, not the full one — first id only, device
   name cut to 12 **bytes**, fraction quantised to 1/65535ths.
2. **`make_record` stores `round(fraction, 6)` and that rounding is not cosmetic** — it feeds the
   quantisation. `int(round(1/3,6)*65535)` is 21844; unrounded it is 21845.
3. **Entities and whitespace are an OFFSET problem.** A position is a character offset, so an
   entity decoded to a different length than COVEY's shifts every offset after it and keeps growing
   to the end of the chapter — too small to trip the "offset past the end" check. Python's
   `str.split()` also treats **U+00A0** (what `&nbsp;` decodes to) as whitespace.
4. **`HEX` is `#define HEX 16` in Arduino's `Print.h`** — fine in a file that does not include
   `Arduino.h`, fatal in one that does.
5. **The href trap:** manifest hrefs resolve against the OPF's directory, not the zip root. Get it
   wrong and you get a book with zero chapters and *no error*.
6. **`_norm` returns the href unchanged when there is no base** — it does not normalise.

## How books are matched — not by filename

`id:<dc:identifier>`, `ta:<title|author slug>`, `fp:<content fingerprint>`, best first; ANY overlap
means the same book. **Only the FIRST id travels over LoRa.** Byte-identical copies on both devices
is the happy path — it makes all three agree at once. The filename only decides for a `.txt` (or an
EPUB with no `dc:title`), where the title comes from the filename stem.

**Two secrets, not one:** the `booksync` Meshtastic channel PSK (transport — COVEY has it at
channel idx 3 and can share it as an invite link the WiPhone can already import via **Apply link**)
AND the booksync passcode, which is the HMAC key.

⚠ **This repo is PUBLIC.** Test vectors embed a passcode *and* its derived key, which is the whole
secret — that is why `tools/gen_booksync_vectors.py` uses invented values. Never put a real
passcode there.

---

## Waiting on Nick

- **Flash the build and read a chapter.** Nothing above the host tests has ever run.
- Put the same book files on both devices — WiPhone SD `/books`, COVEY `/home/covey/books`.
  Byte-identical copies. `Ghosts_of_Timkovichi.epub` is on the WiPhone (in `/roms`); COVEY does
  not have it yet, and sync needs both.
- Import COVEY's `booksync` channel invite onto the WiPhone (DM the link, then **Apply link**).
  Never broadcast it — it carries the channel PSK.
- Change COVEY's booksync passcode from its `"nnnn"` placeholder, and match it on the WiPhone.

## The phone itself

New unit, flashed 2026-08-10. Port **`/dev/cu.usbserial-025A3EAF`** (per-unit — derive it from
`ls /dev/cu.usbserial-*`, never hardcode). ESP32-D0WDQ6-V3 **rev 3**, 16 MB.

🔴 **Its CP2104 fails above 230400** (`Invalid head of packet` at 460800 and 921600). `platformio.ini`
still says `upload_speed = 921600` and `pio run` has no override flag, so drive esptool directly.
Full command in the `wiphone-flashing` memory. Factory backup at
`backups/newphone-factory-full-16MB.bin`.

**Reading the boot log is inverted:** capture at **500000** baud, only `log_e` is compiled in, and
`MeshPhy: SX1276 detected` is a `log_i` that is compiled out. **Silence about the radio means the
radio is fine**; only failure prints.

---

## Recently fixed — don't re-break this one

**Triple-tap-to-sleep is suppressed while editing text**, because the top-right Back key is
BACKSPACE inside a field and three quick corrections were sleeping the phone mid-message.

⚠ The rule is **"the focused text field is NON-EMPTY"**, not "a text field has focus". The
first attempt used focus alone and made triple-tap *near-impossible*: `FocusableApp::setFocus()`
focuses whatever widget a screen lands on, and plenty of screens land on a text field, so every
Back press there reset the counter. **Focused is not typing.** An empty field has nothing to
backspace, so the tap can only have been meant as a triple-tap and is let through.

Also worth knowing for any future work in this area:
- `setFocus` is **pure virtual in `AbstractWidget`** and `TextInputAbstract` overrides it. A hook
  added to `FocusableWidget::setFocus` alone will never run for text widgets — that cost a
  build-and-flash cycle. `TextInputAbstract::setFocus` now delegates to the base instead of
  duplicating its two assignments.
- The tracker is a **pointer to the live widget**, cleared by `~FocusableWidget`, not a bool. A
  flag stuck true would disable triple-tap for the whole session.
- `ControlState::inputType` is unusable as a "typing" signal — a MODE, set ad-hoc, persists
  across screens.

Timing (`BACK_TAP_GAP_MS`, 500 ms between consecutive taps) was deliberately left alone so the
rule change could be judged on its own. Confirmed good on hardware 2026-08-10.

## Also open

- **Stretch goal:** [`woods-backplate.md`](woods-backplate.md) — RFM95W + GPS + 915 MHz whip +
  expanded battery on a screw-terminal plate. Design notes only, nothing built.
- `stash@{0}` holds DIAG battery/gauge instrumentation from the July 2026 power repair. Not needed
  on the new phone; kept in case the old one is revived.
