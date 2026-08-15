# WiPhone — session handoff

**Last updated:** 2026-08-14 · **`main` is pushed and clean at `d48e540`+.**
⚠ **The phone runs a DEV BUILD that reports 0.9.1** — flashed twice over USB without a version
bump, so the reported version no longer identifies what is installed. See below.
**Next up:** ONE measurement, then a release. The crackle needs a `gaps:N` reading before
anyone writes a line of code, and the plan Nick set is to **fold that fix into 0.9.2 and
install it over the air** — which would also be the first OTA this phone has ever done.

Read this first to resume. One command tells you the codebase is healthy:

```bash
./tests/run_tests.sh
```

Expect **841 assertions, 0 failures** across eight suites. It compiles the phone's own
sources — including the real MP3 decoder — with the host compiler under ASan, no PlatformIO,
no ESP32, no phone attached.
⚠ Two suites need gitignored fixtures or they skip themselves and say so:
`tools/gen_jpeg_fixtures.sh <book.epub>` and `tools/gen_mp3_fixtures.sh <file.mp3>`.

---

## ▶ PICK UP HERE

### 🔎 1. Why does the phone restart? — **CAUGHT. It is a PANIC, and the heap fragments.**
**2026-08-14: the first crash was captured in `/health.log`.** `BOOT reset_reason=4` — a
PANIC. That rules out the other three outright: **not brownout (9), not the watchdog (6),
not power (1).** The battery was never below 3.99 V and never charging across 173 samples,
so the tired-cell theory is dead too.

**Fragmentation is confirmed, and it is a RATCHET.** From the 112-minute run, which is the
cleanest evidence because nothing unusual was happening to it:

| up | free heap | largest block | |
|---|---|---|---|
| 0 min | 19,148 | **15,508** | idle, screen off |
| 9 min | 15,164 | 11,836 | screen on |
| 33 min | 11,836 | 7,216 | screen on |
| 34–101 min | ~11,700 | **7,216** | screen off — flat for 68 minutes |
| 104 min | 9,104 | 5,296 | screen on |
| 106 min | **16,996** | **6,260** | screen off |

**At 106 minutes free heap is back to 16,996 — near the 19,148 it started with — while
`largest` is 6,260 against a starting 15,508 and never recovers.** Free heap returning while
the largest block does not is the fingerprint of fragmentation rather than a leak, and it
steps down on every screen-on episode and holds. `min` heap slid 19,148 → 9,104 in that run.

**The run that panicked died at `largest=4512`**, having fallen 15,432 → 11,504 → 7,064 →
4,512 over its final three minutes.

⚠ **THE INSTRUMENT WAS PART OF WHAT IT MEASURED, and that is not a footnote.** The panic
happened while the log was being pulled over HTTP — ~30 whole-file fetches, each a fresh TCP
connection through the `WebServer`'s String parser, because a 20 KB response kept truncating.
`largest` oscillated 3,084 ↔ 6,660 throughout. **So the captured crash was probably provoked,
and it is NOT proof that Nick's occasional spontaneous restarts are the same event.** What it
does establish is the mechanism class — PANIC via internal-heap exhaustion — and the 112-minute
run above fragments the same way with nobody near it. This is the same trap as the
"Stop the instrument distorting the measurement it takes" commit. Fixed, below.

✅ **`/log?tail=N` now exists** so the next measurement costs ONE request instead of thirty:
```bash
curl "http://wiphone.local/log?tail=2000"     # with any upload screen open
```
It seeks to N bytes from the end and steps forward over the partial line it lands in, so the
response always starts on a whole line. ⚠ `Content-Length` is computed AFTER that resync, never
from `f.size()` — a length that disagrees with the body by one byte hangs the client. No `tail`
argument still sends the whole file. **`app_gbc_xfer.cpp` includes Arduino so it is not
host-testable; this was verified live against the phone instead** (`tail=2000` → header 1918,
body 1918, starting on a whole line).

⚠ **`tail` does not cure the truncation, it makes a SMALL read likely to succeed.** Measured on
the phone: ~1.5 KB succeeded 2/2, 3 KB 2/3, 6 KB 0/1, the whole 21 KB file 0/1. Failures land on
512-byte boundaries (1024, 15872), so it is the transfer dying, not the arithmetic.
**Ask for ~1500–3000 bytes, and check `Content-Length` equals the bytes you received** — that
comparison is the only reliable way to know you got it all.

### 💡 READING THE LOG IS THE BIGGEST SINGLE CONSUMER OF WHAT THE LOG MEASURES
Measured 2026-08-14 on a fresh boot, one minute per row:

| up | largest free block | |
|---|---|---|
| 1 min | 15,456 | idle, screen off |
| 2 min | 12,980 | idle |
| 3 min | **7,480** | the uploader screen opens — **−5,500** |
| 4 min | **4,808** | eight `?tail=` fetches — **−2,672** |

So the uploader app plus the `WebServer` costs ~5.5 KB of contiguous internal RAM, and **each
HTTP request permanently costs a further ~340 bytes that never comes back.** Thirty requests is
~10 KB, which is precisely what takes a healthy 15.5 KB start down to the 4,512 the phone
panicked at. ⚠ **One sample, so treat ~340 B/request as approximate** — but the shape is not in
doubt, and it means the honest way to read this log is: **one tail request, then close the
screen.** Do not leave the uploader open and poll it.

### 🎯 THE LIKELY ROOT CAUSE, FOUND AND FIXED 2026-08-14 — but PROOF IS TIME WITHOUT A PANIC
**`connectToWiFi()` re-registered the WiFi event handler on EVERY attempt.**
`WiFi.onEvent()` only ever does `cbEventList.push_back()` — **it does not deduplicate, and
`removeEvent()` is called nowhere in this firmware** (verified: zero matches). So the handler
list grew by one entry per connect attempt and never shrank except at reboot.

**That is the retry path.** `WiPhone.ino:1667` calls it every `WIFI_RETRY_PERIOD_MS` = **20 s**
for as long as there is no network. Nick's **car journey** shows exactly this: the log has
`wifi=1` (WL_NO_SSID_AVAIL) held for the whole run, **143 minutes out of range ≈ 430
registrations.**

Then one `SYSTEM_EVENT_STA_GOT_IP` runs `processWiFiEvent` **N times**, and that handler is not
free — each pass does `delay(100)` plus `udp.begin()` and `udpRtcp.begin()`, and
`WiFiUDP::begin()` does `stop()` (`delete[] tx_buffer`) then **`new char[1460]`**. At N=430 that
is ~43 s of blocking delay and **~1,720 alloc/free cycles of 1,460 internal bytes from a single
reconnect.** The `std::vector` also doubles as it grows, leaving a trail of holes.

⚠ **AND THERE IS NO malloc→PSRAM AUTO-DIVERSION IN THIS BUILD.** The framework `sdkconfig.h`
defines `CONFIG_SPIRAM_USE_CAPS_ALLOC` and **not** `CONFIG_SPIRAM_USE_MALLOC`, so plain
`new`/`malloc` is **internal at every size** — only `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` /
`ps_malloc` reach PSRAM. **`docs/MUSIC.md`'s "the threshold is 16 KB" is wrong for this build:
there is no threshold.** Anything that says otherwise should be corrected, not relied on.

**Why this fits the measured signature:** the growth is **monotonic and never recovers**, which
is the "free heap recovers but `largest` never does" fingerprint exactly; and it is driven by
**WiFi events, not uptime**, which is why a 55-minute idle run with stable WiFi held at
`largest=27,164` while the car run collapsed.

**The fix:** `WiFi.onEvent(processWiFiEvent)` moved out of `connectToWiFi()` into
`Networks::init()` behind a `static bool` latch, so it registers exactly once per boot.
`wifiState.init()` (WiPhone.ino:904) runs before the first `connectToPreferred()` (:916), so the
handler is in place in time. ✅ **Verified on hardware: `[autosw] conn=1` in the boot log, and
that flag is set ONLY from inside `processWiFiEvent` on GOT_IP — so the handler still fires.**

⚠ **CONFIDENCE: this is the best-supported candidate, NOT a proven cure.** Four panics were
captured (`largest` at death: 4,512 / 4,092 / 8,848 / 3,084 — note it is *not* a fixed
threshold, consistent with a probabilistic allocation failure). **The only proof is a long run
across several WiFi drops with no panic.** Check `/health.log` for `reset_reason=4`.

⚠ **A SMALLER INSTANCE OF THE SAME BUG IS LEFT IN ON PURPOSE.** `ESPmDNS`'s
`MDNSResponder::begin()` also calls `WiFi.onEvent()`, and `app_gbc_xfer.cpp:499`
`MDNS.begin("wiphone")` runs on every `xferStart()` while `MDNS.end()` does not remove the
handler — so one permanent entry per uploader session. It was **not** fixed because `MDNS.end()`
is paired with it and breaking that kills `wiphone.local`, which is how the log is read at all.
One per uploader session is negligible beside 430 per car journey.

**What is still open:** naming what takes the ~13 KB of *internal* heap when an app opens. The
22.6 KB of PSRAM that goes with it is already accounted for and is **healthy** — it is exactly
`BooksApp`'s four arrays (`books` 8,112 + `store` 11,912 + `images` 2,400 + `imgBoxes` 96 =
22,520, plus 21 bytes × 4 of allocator header = 22,604 observed). That is the earlier
ps_malloc fix working as designed; PSRAM has 3.6 MB spare and is not the problem.

---

**The reset-reason table, for reading any future log:**

| | |
|---|---|
| **1** POWERON | the switch, a flash, or the power was interrupted |
| **4** PANIC | a crash — null deref, `abort()`, assert |
| **6** TASK_WDT | the main loop stopped feeding the watchdog |
| **9** BROWNOUT | **the supply sagged** — not software at all |

⚠ **Read the number before theorising.** A tired cell under a WiFi transmit peak (9) looks
exactly like a crash from the outside, and chasing it as a memory leak would be chasing the
wrong thing.

**The mechanism, now measured rather than assumed.** The documented failure on this phone is
the WiFi PHY failing to get ~2 KB of *contiguous* internal RAM for RF calibration, at which
point `phy_init` calls `abort()` — and an `abort()` is **reset_reason=4**, which is what was
captured. Free heap looks fine while the largest block is what runs out.

⚠ **`min` being flat does NOT mean the heap is safe, and an earlier version of this doc leaned
on that.** It is flat *while idle* — 84 bytes over 67 idle minutes in the run above — and it
steps down hard every time an app opens. Watch **`largest`**, not `min` and not `free`. The
three disagree, and only `largest` predicts the crash.

**Where to look:** `/health.log` on the card. A line a minute; it survives both the reboot and
being unplugged, so the reset reason of the run that died sits at the top of the next run's
entries.

```bash
curl "http://wiphone.local/log?tail=6000"     # with any upload screen open
```
⚠ **Use `?tail=`.** A whole-file fetch truncates — with modem sleep on and the CPU at 80 MHz
the link is slow — and truncation costs you the END of the file, which is exactly where the
reset reason lives. Retrying instead is what made the reading of the log a cause of the crash.

### 🔋 2. Battery: ~10 hours measured. Is that good enough?
Measured on the device, steady state with the first 30 minutes discarded (surface charge):

```
94% -> 82% over 1.20 h  =>  10.0 %/h  =>  ~10 h from full
CPU at 80 MHz for 93% of the run, tracking screen state exactly (0 disagreements/103 samples)
```

⚠ **There is no before-measurement**, so the improvement factor is unknown. ~10 h is simply
the number to beat.

**Already done, and audited — do not redo these:** the main loop no longer spins (it was
`taskYIELD()` at 240 MHz forever), the CPU drops to 80 MHz when idle, WiFi modem sleep is on,
and disconnected WiFi scanning backs off. Bluetooth was already off, the keyboard backlight
already off, the screen already dims and sleeps.
**Left on purpose:** the LoRa radio sits in continuous RX (~10–12 mA). That is the cost of
hearing the mesh at all.

### 🎵 3. Music works and has been heard. One thing is still open.
Nick, on headphones: *"sounds alright... a little crackly, but ok"*, and after the fix
*"a bit better, but the songs don't quite skip, but almost."*

The big cause was found and fixed: `Audio::loop()` decoded exactly ONE frame per call — 24 ms
of audio — so any main-loop iteration slower than that drained the DMA with **no way to catch
up**, because the next pass still only produced 24 ms. Something rate-limited to realtime
cannot make up a gap. It now decodes ahead until the DMA refuses more.

⚠ **A residual glitch remains and is NOT diagnosed. THIS IS THE NEXT THING TO DO, and it is a
measurement, not a code change.** Nick, 2026-08-14: *"lets do this later"* — so it is queued,
with everything needed to act on it below.

**▶ THE READING THAT DECIDES IT.** Play a track on headphones; the now-playing screen shows
**`gaps:N`** in dark grey, on the line under the `Vol [====------]` bar. Note it, listen until
you hear the crackle, note it again.
- **`gaps` CLIMBS as you hear it** → real starvation. And since the DMA already holds ~93 ms
  (below), the cause is something blocking the main loop for longer than that — findable
  without touching buffer sizes.
- **`gaps` STAYS AT 0** → the buffer is not involved and **the ring buffer sketched in
  `mp3_stream.h` would be wasted work.** Look at the decoder resyncing, the SD read, or the
  output stage instead.

Do not add a ring buffer on a hunch; decode has ~74% headroom (measured: 5103 µs/frame against
a 24000 µs budget).

🔴 **AND THE OBVIOUS FIX WOULD MAKE THE RESTART BUG WORSE — check this before reaching for it.**
I2S is `dma_buf_count = 4`, `dma_buf_len = 1024` ≈ **93 ms** at 44.1 kHz, and the decode-ahead
loop can produce up to 12 frames (~313 ms) in one pass, so it can already refill that buffer
several times over. Those buffers are also roughly **16 KB of internal, DMA-capable RAM — the
exact resource that is running out and panicking the phone** (§1). **Enlarging them trades the
crackle for the restart.** The two open bugs pull in opposite directions; that is why the
`gaps` reading has to come first.

### 📖 4. Book sync has still never been on air
Unchanged, and still the only thing that needs COVEY. Everything in the reader section below
applies.

⚠ **Checked twice on 2026-08-14: `covey.local` does NOT resolve** — COVEY was not on the network
either time, while the WiPhone was (`192.168.158.33`, both on `NickH-wifi`). So this stayed
blocked all session. **Check it first next time; it is one `ping` and it decides whether this
item is even available.** ⚠ Note the COVEY side of the docs has carried a stale line saying the
WiPhone has no e-reader — **it has had a working one since 2026-08-11**, and COVEY's half is
finished and waiting, so the only genuinely missing piece is the two devices being powered on
the same network at the same time.

---

## ⬆️ Firmware updates now come from this repo

**Settings > Firmware settings.** The URL box fills itself in; nothing needs typing.

It never was a dead link — **the pinned certificate expired 2021-04-18**. `/wiphone.pem` in
the factory SPIFFS is the *leaf* for wiphone.io, not a root CA, and it beat everything else,
so every check failed the TLS handshake regardless of the URL. The built-in CA is now ISRG
Root X1 (valid to 2035) and `/wiphone.pem` is ignored; an explicit `/user.pem` still overrides.

**To publish a release:**
```bash
tools/publish_ota.sh 0.9.2      # builds, stages ota/, bumps FIRMWARE_VERSION together
git add -A && git commit -m "Release 0.9.2" && git push
```
For a public repo, **pushing is releasing** — the phone reads straight from `main`.

⚠ **raw.githubusercontent.com, never a github.com release URL.** The manifest is fetched by a
hand-rolled socket in `loadIniFile()` with **no redirect handling**; a release asset answers
302 and would look like an empty file. Cost: ~2 MB of repo growth per release.

⚠ A `serverIni` saved in `/user_ota.ini` BEATS the compiled-in default — that is what a user
override is for. A stored `wiphone.io` URL is now treated as "no preference" so the new
default wins; anything else you type will stick.

⚠ **The boot-time auto-check runs before WiFi has associated** and always fails with
`DNS Failed`. Pre-existing, harmless, and it did the same with wiphone.io. Use the **Check**
button once the phone is on WiFi. Fixing it means deferring that check until WL_CONNECTED.

⚠ **OTA has never actually installed anything.** `app1` has never been written on this phone.
The first over-the-air install is the risky one; recovery is the usual serial reflash.

⚠ **THE PHONE IS RUNNING A DEV BUILD THAT CALLS ITSELF 0.9.1 (2026-08-14).** Two USB flashes —
`/log?tail=` and the menu-ID fix — both without bumping `FIRMWARE_VERSION`, so the firmware
installed is NOT the `ota/firmware.bin` committed as 0.9.1. Nothing breaks (the version only
decides whether an update is offered) but **do not trust the reported version to identify what
is on the phone.**

### ▶ THE AGREED NEXT RELEASE — Nick's plan, 2026-08-14
Get the `gaps:N` reading → fix the crackle → **cut 0.9.2 and install it OVER THE AIR.** That
single release also clears the version skew above and would be **the first OTA this phone has
ever performed.**

⚠ **Nick asked whether he could OTA at the level he is already on. He cannot** — `publish_ota.sh`
bumps `FIRMWARE_VERSION` and stages `ota/` **together** precisely because a manifest equal to
the installed version is indistinguishable from a broken update check. The phone reports 0.9.1,
so the OTA must be **0.9.2 or higher** to be offered at all.

```bash
tools/publish_ota.sh 0.9.2
git add -A && git commit -m "Release 0.9.2" && git push     # for a public repo, pushing IS releasing
```
✅ **The risk is much lower than it was:** the USB adapter is connected
(`/dev/cu.usbserial-025A3EAF`), so recovery from a failed first OTA is a ~55 s reflash rather
than a problem. Do not attempt the first OTA with no cable to hand.

---

## The reader is DONE and confirmed; sync has never been on air

📖 **Nick read a real bought book on this phone on 2026-08-11 and confirmed it working**:
prose, chapter titles, pictures inline, and his place surviving a power cycle. Menu > Books.
Flash at 230400 (hash verified). That was the goal, and it is met.

**The one thing left needs the other device: proving book sync on air against COVEY.** Every
piece is written and host-tested; not one packet has crossed. The order to check things in is
in the sync section below — and every failure mode there is silent on both devices, so follow
it rather than guessing.

Nothing else is outstanding. Everything below this line is context for whoever picks it up.

### The test book is ALREADY ON THE SD CARD
`Ghosts_of_Timkovichi.epub` (**5,059,833 bytes** — the 5,060,061 written here before was
wrong; the FINGERPRINT is what proves a copy, and it feeds on the size so a match settles it)
was pushed over WiFi on 2026-08-11 and sits at
**`/roms/Ghosts_of_Timkovichi.epub`** — not `/books`. That is not a mistake to correct: the only
uploader the phone was running at the time was the Game Boy one, whose server has **no extension
filter at all** (the `accept=` attribute is a browser hint). The Books app scans `/roms` and the
card root as well as `/books` for exactly this reason, and **Manage** can move it.

**Proof it is the right file:** open it and check **Book info** shows `fp:2f6a8bc9d41ee898`. That
fingerprint is sha1(size + first 64 KB + last 64 KB), computed independently on the Mac, so a
match means the 5 MB arrived byte-for-byte.

### Confirmed by eye on 2026-08-11
Nick read a page of the real book. Two things it found, both fixed and flashed:
- **Akrobat has no typographic punctuation** and its `.notdef` is a narrow bar, so "Arc-Royal’s"
  read as "Arc-Royalls" — a missing glyph that looks like a letter, not like damage.
  `bookRenderRun()` substitutes only what the font cannot draw (it asks, via
  `getUnicodeIndex`) and feeds BOTH measuring and drawing. ⚠ It must never touch `chapText`.
- **`HeaderWidget::setTitle` keeps the pointer and `redraw` applies no width limit**, then
  paints the clock and icons over the title's right-hand end. Every other app gets away with
  it by having a short title; a book title ran straight under the clock.

### ⚠ THE INTERNAL HEAP IS ~16 KB, AND THAT IS THE WHOLE FIRMWARE'S MARGIN
Opening a book rebooted the phone a minute or two later. The symptom named the wrong culprit:
`phy_init: failed to allocate memory for RF calibration data` + `abort()` — the WiFi PHY, on
its periodic auto-switch re-scan, failing to get ~2 KB of **internal** RAM. Nothing in the
backtrace pointed at the reader, and the obvious theory (the new picture decoding) was wrong.

Measured with the heap probe in `app_books.cpp` (compiled out behind `BOOKS_HEAP_DEBUG` —
uncomment it before theorising about memory here):

| | free | largest block | psram |
|---|---|---|---|
| before | 5,112 | 3,084 | 3.6 MB |
| after | 15,436 | 12,252 | 3.6 MB |

`new BooksApp` was taking 11 KB of internal heap, 8 KB of it a plain array of 48 filenames.
PSRAM was never the constraint. **Anything in an app that is more than a few hundred bytes
belongs in `ps_malloc`.** Watch `largest` as much as `free`: this was a fragmentation failure
as much as a volume one.

### Pictures (2026-08-11) — including greyscale, via our own decoder
Inline, captioned `[1] press 1 to enlarge`, and full-screen on that number key. Read the two
bugs in commit fab0502 before touching the layout: a picture too tall for the space left on a
page was silently LOST, and a picture on a one-row page could stop the page advancing at all.
Colour goes through the ROM decoder; greyscale through `jpeg_grey` (see below), which is most
of them. Still out of reach and honestly reported on the picture: progressive JPEG, and PNG.

### If something regresses, check in this order
1. **Menu > Books lists the book.** If the library is empty, the SD scan or the card is the
   problem, not the reader.
2. **Open it.** Give it a few seconds: opening reads the zip directory of 230 entries, the OPF,
   the NCX, and fingerprints 128 KB. Expect the Title Page — spine 0 is a coverless-text cover
   and is skipped deliberately. 90 chapters, real titles from the NCX ("Prologue",
   "1. Monkeys with ’Mechs"), ~11 lines a page, ~34 characters a line at the small size.
3. **Page down and back up.** Nothing should be skipped or repeated at a boundary — that was a
   real bug, found by paging a real chapter backwards on the host, and it is now pinned.
4. **Close the book, reopen it.** It must land on the same page. That is the whole feature.
5. **Leave it sitting on a page for two minutes.** The screen must not sleep.

### ⚠ SYNC IS BUILT BUT HAS NEVER BEEN ON AIR (2026-08-11, commit 595e838)
COVEY was at home, case open and unpowered, so not one packet has crossed between the two
devices. What IS proven is 34 host assertions against real COVEY-generated packets: parking,
matching, newest-wins, and that every wrong-passcode and tampered vector is diverted out of
Chats and never matches.

**To test it, in this order:**
1. Put the SAME book file on COVEY (`/home/covey/books`). Byte-identical — that makes all
   three ids agree at once. `Ghosts_of_Timkovichi.epub` is on the WiPhone only.
2. Change COVEY's `booksync_pw` off its `"nnnn"` placeholder and type the same into
   **Books > menu > Sync settings > Passcode**. That screen also says whether the `booksync`
   CHANNEL exists — two separate secrets, both silent when wrong.
3. Import COVEY's `booksync` channel invite (DM the link, then **Apply link** in Meshtastic).
4. Read on one, "Sync my place", open the book on the other. The confirm card should offer
   the other device's position.

⚠ Every failure here is silent on both devices. If nothing happens, check in this order:
channel present → passcode identical → the two books share an id (Book info shows them).

### Then: the rest of sync
All four of the pieces below are BUILT (mesh divert, send by channel name, the confirm card,
the parking spot, passcode/device name in NVS) — see the note above for what is still unproven.
What is genuinely left:

- **Sending on a schedule** rather than only on demand, if that turns out to be wanted.
- **A second device to test any of it against.**

### Greyscale pictures — DONE (2026-08-11, commit dc910cf), unseen on screen
`jpeg_grey.{h,cpp}` is our own baseline greyscale decoder. It exists because the ESP32's
TJpgDec is in ROM, does 3-component YCbCr only, and **33 of the 45 pictures in this book are
1-component**. Proven on the phone first (`load_jpg_at=0`), then written, then checked pixel
for pixel against KNOWN pixels (a quality-100 round trip of a checkerboard: mean error 0.001)
and cross-checked against macOS `sips` on the book's own art.

⚠ If a book image ever disagrees with sips by a mean of ~1 with outliers around 33, that is
NOT a bug — it is sips smoothing a low-quality JPEG, which the ground-truth fixture exists to
prove. Check truth.jpg before suspecting the decoder.

What is still unsupported and now genuinely out of reach: progressive JPEG and PNG.

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
| `epub_parse.{h,cpp}` | zip, inflate, OPF, spine, XHTML→text, ids, fraction/locate, nav/NCX titles, pictures — **167** |
| `bookstore.{h,cpp}` | reading positions — **57** |
| `book_layout.{h,cpp}` | pages, wrapping, page-back, picture rows — **113** |
| `html_entities.h` | 2125 entities, generated |
| `app_books.{h,cpp}` | the reader. Compiles and runs nowhere yet — see PICK UP HERE |
| `app_gbc_xfer.{h,cpp}` | the shared upload server, one `XferConfig` per app — now four |
| `music_lib.{h,cpp}` | track list + play order — **96 assertions** with wav_reader |
| `wav_reader.{h,cpp}` | WAV headers, downmix, resample | ″ |
| `mp3_stream.{h,cpp}` | ID3 skip, frame sync, feeding helix — **32**, decodes a real track |
| `src/audio/helix-mp3/` | vendored MP3 decoder (RPSL) + our PSRAM allocator |
| `music_player.{h,cpp}` | library, queue, volume — outlives its app on purpose |
| `app_music.{h,cpp}` | the screen. See `docs/MUSIC.md` |

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

- **Read a chapter.** It is flashed; nobody has looked at the screen.
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

## Recently fixed — don't re-break these

### 🔁 DUPLICATE MENU IDs ARE SILENT, AND HAVE NOW SHIPPED TWICE (2026-08-14)
Nick: Settings > **WiFi auto-switch** was drawn with the **Music icon**, and selecting it
**opened the Music player**. Both symptoms, one cause: `"Music"` (parent 1) and
`"WiFi auto-switch"` (parent 5) both had **ID 42**.

**`findMenu()` matches on ID ALONE — it never looks at `parent`** — and returns the FIRST row
it finds. Music sits earlier in `menu[]`, so it shadowed the WiFi row completely: `enterMenu(5)`
built the Settings row and asked `findMenuIcons(42)`, which gave Music's icon, and on OK
`findMenu(42)` walked from the top and returned Music, so `enterApp(GUI_APP_MUSIC)` ran.

⚠ **This is the SECOND time.** Commit `3629566` is literally titled *"Fix Music opening Books"* —
the same failure — and it was fixed by moving Music to 42, **which `7abb47f` had already given to
WiFi auto-switch.** The fix relocated the collision instead of removing it. A comment reading
*"⚠ ID must be UNIQUE, not just the action"* was added at that time and did not prevent the
recurrence, so **there is now a real check**: `GUI::init()` walks the table at boot and logs
`MENU: DUPLICATE ID <n> - "A" shadows "B"` at `log_e`. Silence there is the pass condition.

WiFi auto-switch is now **43**. Music keeps 42 because `menuIcons[]` is keyed to it. **The only
duplicate in the whole table was 42** — verified by extracting the real table and running the
same O(n²) pass over it, which reproduces the bug on the committed HEAD and reports clean after.
⚠ **Free IDs are 25 and 44+.** 25 is a gap inside a used range and may have been retired; prefer
counting up from the top. Nothing persists a menu ID (`curMenuId` is runtime-only), so
renumbering needs no migration.

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
