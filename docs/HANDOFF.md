# WiPhone — session handoff

## ▶▶ NEXT SESSION — TASK LIST (rewritten 2026-08-24, evening; phone-2 section added 08-25)

Read this first; everything below it is narrative. Repo clean at **0.9.19**.
**Phone 1 is flashed with everything here and verified as far as the cable allows.**
✅ **Phone 2 was flashed to HEAD (`6c50d7d`) on 2026-08-25** — P5 is done, and it immediately
turned up the bug below.

### ✅ ANSWERED 2026-08-26 — THE SIP FLAP IS FIXED, AND TWO STALE SENTENCES SAID OTHERWISE (0.9.21)

Nick remembered it being fixed. **He was right, and it is now MEASURED.** 330 s of console on
phone 1 (the phone that actually holds the account), WiFi up throughout: **8 REGISTERs, ONE
`SIP REGISTRATION -> REGISTERED`, zero "lost"** — seven refreshes in a row all logging
`registered=1`. Before `3d9f329` it read REGISTERED → lost → REGISTERED once a minute.

🛑 **WHAT COST THE TIME WAS NOT THE CODE, IT WAS TWO STALE ENGLISH SENTENCES.** `tinySIP.h:624`
still said *"THE REAL FIX, deliberately NOT shipped tonight"* — directly above a paragraph
describing that same fix as in place — and MEMORY.md said it was "left for a session with Nick
present". Both were written 08-22 and obsolete by 08-24. **Both corrected.** ⚠ When a fix lands,
grep for the comment that said it had not.

🛑 **`rtpSilent()` WAS SETTING `registered = true`.** It is reached when the OTHER PARTY's RTP
goes quiet (`WiPhone.ino:3186`) and has no business touching registration — it reads like a copy
of `wifiTerminateCall()` above it with the boolean flipped. **Cost:** `sipRegistered` gates
CallApp's END/BACK (`GUI.cpp:5575`, `:5592`), so after an RTP timeout on an UNregistered phone,
END stopped being the one press that leaves the call screen. Removed.

🔋 **AND A BATTERY BUG FOUND WHILE WATCHING THAT CONSOLE — the WiFi scan backoff was defeated by
its own retry line.** An empty scan set `_msLastScan = now - AUTO_SCAN_PERIOD_MS + RETRY`, which
means "570 s ago" — correct only while CONNECTED, because disconnected the due-check compares
against 120 s. A stamp older than the period is due IMMEDIATELY.
**MEASURED with the AP gone: 114 scans in 280 s, one every ~2.5 s, against a design of one every
two minutes.** ⚠ **An empty scan IS the out-of-range case**, so the one situation the backoff
existed for was the one it never applied to. Fixed by splitting `n == 0` (out of range → normal
backoff) from `n < 0` (aborted → retry soon), with `currentDiscPeriod()` as the single
definition both the due-check and the retry scheduler use.

- [ ] ⚠ **VERIFY THE SCAN FIX — it is the one thing here NOT proven on hardware.** The AP came
      back before the fixed build could be watched under the same conditions, and a phone that
      can see an AP rejoins rather than staying disconnected. **Attach the cable, turn the
      hotspot OFF, watch 5 minutes: expect 2-3 `scan started` lines, not ~110.**
- [ ] 🔎 **Phone 1's WiFi returned 0 networks on 112 consecutive scans** while the Mac was
      associated to a 2.4 GHz AP on channel 6 in the same room. It recovered on its own later.
      Worth one look if it recurs — a scan that sees literally nothing is not the same as
      being out of range.

---

### ✅ CLOSED 2026-08-26 — THE OTHER MENUS WERE DROPPING THEIR ROWS TOO (0.9.20)

0.9.18 fixed `addOption(text, 0, …)` in Photos and **nobody swept for it.** Eight more sites in
Books, Files and Music were still passing 0, so those rows had never once appeared: "(no books
yet)", why a book would not open, what the last booksync send did, **"Channel 'booksync':
MISSING"**, "Parked positions: N", the result of the last file operation, "(more files not
listed)", "(no music yet)", and why a track would not decode.

⚠ **`app_files.cpp:156` still carried the comment `key 0 = inert`** — the exact false belief
0.9.18 was written to kill, sitting one file over from the fix. **That is what an unswept fix
looks like, and it is the reason this entry exists.**

**FIXED** with `MenuWidget::addNote()`, so there is one correct way to write a display-only row.
The widget's refusal now names it: `menu option key is 0 - row DROPPED, not added. For a
display-only row use addNote()`. 🛑 **`MENU_ROW_NOTE` is `0xFFFFFF01` and a range test will EAT
it** — `(int)(sel - ROW_FIRST)` underflows to a NEGATIVE index that sails through
`idx < entryCount` and reads off the front of the array, so Files, Books and Music each reject
it explicitly *before* their range test. A `tests/run_tests.sh` source guard now fails the suite
on any new `addOption(..., 0, ...)` — a grep, not a unit test, because the wrong spelling
compiles, links, runs and shows nothing.

✅ **VERIFIED ON THE HANDSET:** marking a file for copy in Files shows **"Copy marked - open a
folder an.."** as its first row — a line that had never rendered. OK pressed on that row three
times: nothing, no crash, screen unchanged.

✅ **AND THE CHAT-HISTORY ROUND TRIP THAT 0.9.19 OWED IS NOW DONE**, with Nick's go-ahead for
one transmission: phone 1 `send 2 persist-check 0826` → `MESH RECEIPT: … -> in mesh`, msgs
26→27, card file 9272→9680 B; **phone 1 rebooted and the message is still in the thread**; and
**phone 2 received it over the air and it survived phone 2's reboot too** (`loaded from SD at
boot`, 1 msg, 1704→2032 B). The incoming direction is the one that matters — phone 2 is the
phone that had been losing everything.

✅ **BOTH PHONES ARE ON 0.9.21**, `built Aug 26 2026 08:03:20`, read back with `ver` on each
port rather than assumed from a successful upload.

- [ ] ⚠ **The audit that found the unswept sites lost one of its 14 readers** to an API error,
      so about a quarter of the 08-25 evening transcript was never read. Treat that sweep's
      findings as a FLOOR. The re-runnable script is
      `.claude/…/workflows/scripts/handoff-truth-audit-wf_df33ea35-806.js`.

---

### ✅ CLOSED 2026-08-26 — THE CHAT HISTORY WENT TO THE CARD AND WAS READ BACK FROM FLASH (0.9.19)

Nick: *"after rebooting I lose the chat history in meshtastic chats on the wiphone's."*

🔑 **`meshFs()` ANSWERED "NO CARD" FOR THE WHOLE OF `setup()`.** It reads `s_meshCardIn`, which
was assigned in exactly one place — `MeshtasticService::loop()` — and `loadDb()` /
`loadFavourites()` run in `setup()`, before any loop pass. So the load always got the initial
`false` and read **SPIFFS**, while every save from the first loop pass on wrote the **SD card**.
⚠ And `loadDb()`'s "fallback" was `meshFs()` then `SPIFFS` — **two spellings of the same
filesystem**, so the card's database was unreachable at boot whatever was in it.

**MEASURED over the cable before anything was changed:** `/meshdb.bin` was **9272 bytes on
phone 1's card** and **6352 on phone 2's** (~26 and ~14 messages), while phone 2's chat list
came up with **no messages in any of its five conversations**. Two file sizes side by side were
the whole diagnosis.

🛑 **AND EACH REBOOT ATE MORE OF IT.** The save writes RAM over the card unconditionally, so a
phone that loaded nothing saved nothing over a good file. **Phone 2's stored history is GONE** —
three diagnostic boots took its card database from 6352 bytes to 1704 (20 nodes, zero messages)
before the cause was found. Phone 1 survived only because its SPIFFS copy happened to be nearly
as fresh as its card copy. Not recoverable; recorded so nobody hunts for it.

**FIXED:** `setCardPresent()` is the single writer of that flag and `WiPhone.ino` calls it
**before** `meshService.setup()`; both load paths now name SD and SPIFFS explicitly.
**Retention is now a decision rather than an accident**, at Nick's request: `MESH_PERSIST_*` —
**40 per chat / 200 total on SD, 12 / 60 on SPIFFS** (the split is the measured 50x speed
difference; a save blocks keypad, screen and WiFi together). In-session scrollback is unchanged
at 150. The rule lives in `mesh_retain.cpp`, host-tested by `tests/test_retain.cpp` (15 checks).

🛠 **NEW INSTRUMENT — `meshdb` on the console.** Which filesystem is in use, which one the boot
actually read, what is in RAM, what the next save keeps, and **the size of `/meshdb.bin` on both
filesystems**. ⚠ The old load line was `log_i`, which this build's log level DROPS — so the one
line that would have shown the wrong file being read never reached the cable anyone was
watching. It is `log_e` now and names the filesystem.

✅ **VERIFIED ON THE HANDSET (both flashed 2026-08-26):** phone 1 boots `loaded from SD at boot`
with 26 msgs where the old build had 3 in LongFast; a node starred over the cable **survived a
reboot** (the favourites file has the same fix); `meshdb` reports the same 26 across successive
reboots.

- [ ] ⚠ **STILL OWED: the round trip with a NEW message.** Everything above proves the *load*
      path and proves a *star* surviving. Watching a freshly received text survive a reboot
      needs one transmission on a private channel — ask Nick before putting anything on the
      air, then: `send <i> persist-check`, wait 15 s, reopen the port, look at the thread.
- [ ] **Phone 2 has an empty history to refill.** Its chats will stay blank until it hears
      something; that is now expected, not a fault.

---

### 🔴 P0 — PHONE 2 AND THE WOODS-PLATE GPS: ONE BUG FOUND AND FIXED, A SECOND STILL OPEN

✅ **BUG 1 — THE BOOT-LOOP PANIC — IS ROOT-CAUSED AND FIXED.** 🔑 **The plate's GPS starts
transmitting the moment it has POWER; it does not wait for the firmware.** So when `setup()`
called `userSerial.begin(115200, ...)` it was attaching the UART RX interrupt to an ALREADY
SATURATED line, and the ISR fired inside the attach's own critical section:

```
Guru Meditation Error: Core 1 panic'ed (Interrupt wdt timeout on CPU1)
setup() WiPhone.ino:1289 -> HardwareSerial::begin -> uartBegin -> uartAttachRx
   -> uartEnableInterrupt -> esp_intr_alloc -> vTaskExitCritical -> _xt_lowint1 -> _uart_isr
```

It then rebooted into `setup()` and did it again — 🛑 **a BOOT LOOP, which presents as a dark
screen and dead buttons, i.e. exactly "it won't respond to button presses".** Reproduced on
**2 of 3 boots** with a byte-identical backtrace. ⚠ **The second backtrace in that dump
(`esp_pm_impl_waiti` / `prvIdleTask`, `EXCCAUSE 0x6`) is the OTHER core's idle task and is
MEANINGLESS** — it cost an hour and nearly sent this hunt after a divide-by-zero that does not
exist. On an INT_WDT dump, read the FIRST backtrace.

**The fix (committed):** open the port at `USER_SERIAL_BAUD` always, and retune to `gGpsBaud`
from `loop()`'s first pass. The line is still live at 9600, but interrupts arrive ~12x less
often and that attach has always survived — it is what every `gpsen=false` boot already did.
The retune calls `updateBaudRate()`, which never re-enters `esp_intr_alloc`, so it cannot
reproduce the fault. 🔑 **That is also the mechanism behind the asymmetry that led here:
toggling `gps on` by hand always worked, booting with it on did not.** The old comment at that
call said opening at USER_SERIAL_BAUD and retuning "would work too" but dirtied the reader's
counters — `gGpsReader.reset()` at the retune keeps them clean, so nothing is lost.
**VERIFIED:** with the fix flashed, boots are clean, no Guru, `retuned from loop()` in the log,
and a GPS fix arrives normally.

✅ **BUG 2 — THE IN-USE WEDGE — IS ALSO ROOT-CAUSED AND FIXED (2026-08-25).**

🔑 **IT IS A DEADLOCK IN THE ARDUINO CORE, REACHED BY ARITHMETIC.** Every `HardwareSerial`
registers `uart_on_apb_change()` (`esp32-hal-uart.c:225`). On `APB_BEFORE_CHANGE` that callback
switches the UART RX interrupt OFF and drains the hardware FIFO into the 256-byte RX queue using
`xQueueSend(..., 1)` — a **one-tick block**, and `CONFIG_FREERTOS_HZ` is 1000, so **1 ms per
byte** once that queue is full. The queue's only consumer is the main loop, which is at that
moment *inside the callback*. Nothing can free a slot.

```
115200 -> 11.52 bytes arrive per ms, 1 drains per ms -> +10.5/ms, exit condition UNREACHABLE
  9600 ->  0.96 bytes arrive per ms, 1 drains per ms -> converges, exits, always has
```
🛑 **So ANY `setCpuFrequencyMhz()` call parks the loop task forever while the plate's GPS is
streaming.** ⚠ And `esp32-hal-cpu.c:190-196` fires that callback on ANY frequency change, even
80<->240 where APB does not actually move — the guard there covers only the register write.

**MEASURED, and it is what finally identified it:** subscribing the loop task to the task
watchdog (it never was — see below) reported `loopTask (CPU 1)` stalled with **`CPU 1: IDLE1`**.
The loop **BLOCKS, it does not spin** — which killed every "the UART storm starves the loop"
theory at once. No watchdog had ever fired because only core 0's idle task is checked.

⚠ **Why it read as "mostly on screen unlock":** the wake REPAINT (~30.7 ms of SPI) runs BEFORE
the clock is raised, so ~354 bytes pile into a 256-byte queue — over threshold every time.
Screen-OFF is preceded by a backlight write, not a repaint, so that pass is short and usually
survives. That is why the downclock kept surviving in tests and sent the hunt after the wrong
transition.

🔑 **THE PREDICTION THAT CONFIRMED IT, testable against logs already captured:** the wedge lands
*between* `setCpuFrequencyMhz()` and the `CPU %luMHz` log line that follows it, so **a wedged log
never contains that line.** Checked against 5 wedge captures: **5 of 5 have no `CPU nnnMHz` line
at all**, while every surviving run prints one.

**THE FIX:** `gGpsNmea` is now a term in the `busy` (and `hardBusy`) predicate, so the clock is
pinned at 240 MHz whenever the GPS owns the UART and the transition never happens. Costs idle
power while GPS is on; buys a phone that answers its buttons. **VERIFIED: 380 s with GPS enabled
at boot, healthy throughout, where the same build died at ~40 s before.**

⚠ **The SD/smsmirror lines were a RED HERRING** — they are simply what logs shortly before the
clock gate evaluates. Good thing that was hedged rather than fixed.

✅ **ALSO ADDED, AND WORTH KEEPING: the loop task is now subscribed to the task watchdog**
(20 s, print-not-panic). It never was, which is exactly why a permanent stall was silent — and
note the existing `LOOP STALL` detector CANNOT see this class of fault, because it measures the
gap between two passes and so only reports a stall the loop RECOVERED from.

---

#### (superseded) Bug 2 as it stood before the cause was found With bug 1 fixed the phone boots and
runs, then **silently stops ~30-40 s in, with GPS on.** ⚠ **It is a DIFFERENT SHAPE from bug 1:
no panic, no backtrace, no reboot — the loop task simply stops** while other tasks keep
printing. Not "interrupts off too long"; something blocks and never returns.

Where it stops, on 3 of 3 wedges, is the last two lines logged:
```
[E][vfs_api.cpp:64] open(): /sd/smsmirror.txt does not exist
[E][vfs_api.cpp:64] open(): /sd/roms/smsmirror.txt does not exist
```
That is `smsMirrorLoadConfig()`'s 30-second retry (`sms_mirror_poll.cpp:102`) re-reading a file
that never exists. ⚠ **BE CAREFUL WITH THIS CLUE:** those lines come from the VFS layer, not
from the poller (which logs only on its FIRST miss), and the loop plainly continues past them —
so this is *where logging stops*, NOT proof that `SD.open()` is what hangs. It is always the
SECOND retry, never the first. GPS off survives these same polls indefinitely.

Next step is instrumentation, not more staring: a breadcrumb of where the loop last was that
survives the hang (the existing `LOOP STALL` detector at `WiPhone.ino:2105` only fires if the
loop RESUMES, so it cannot see a permanent stop), and subscribing the loop task to the task
watchdog so a stall REBOOTS with a decodable reason instead of hanging silently.

---

#### Original P0 notes (kept — the symptom description and the ruled-out theories)

Nick, 2026-08-25, from work: *"was working good but after I turned on the gps it wanted to lock
up every so often mostly on screen unlock… now its screen is off and it won't respond to button
presses."*

🔑 **WHAT THE WEDGE ACTUALLY IS, MEASURED — the main loop stops while the phone stays powered.**
It is NOT a crash-reboot and NOT a dead chip. In the wedged state:
  * **zero `HEALTH` / `KEYS` / `DROP` lines** — those come from `loop()`, and `loop()` has stopped;
  * `[E][Networks.cpp:262] resolveDomain()` errors **keep arriving**, because that is a separate
    FreeRTOS task and it is still running — 🛑 **so "the phone is still printing" does NOT mean
    the phone is alive**, and this is the trap that will fool the next person;
  * serial commands are typed but never answered (the reader is in `loop()`);
  * `esptool` talks to it perfectly — ⚠ **and that proves nothing**, because it resets into the
    ROM bootloader. `ESP32-D0WDQ6-V3 rev 3, MAC 4c:eb:d6:44:93:34` was read out of a phone that
    was, as far as the user was concerned, dead.
  * ⚠ The `/dev/cu.usbserial-*` node proves nothing either — that is the **CP2104**, which is
    USB-powered and enumerates whether or not the ESP32 is running.

⚠ **One boot came up `reset_reason=5` = INT_WDT**, so at least one wedge ended in an interrupt
watchdog reset — "something blocked with interrupts off". The panic dump that came with it
decodes to `esp_pm_impl_waiti` / `prvIdleTask` and its `EXCCAUSE 0x6` is **meaningless**: an
INT_WDT dump shows the *interrupted* context, which was the idle task. 🛑 **Do not go hunting a
divide-by-zero. There isn't one.** (Both divisions near the GPS fix log are guarded, and
`sats=-1 hdop=-1.0` is cosmetic — `gpsSats` only updates when a GGA supplies a count.)

✅ **WORKAROUND, APPLIED AND PERSISTED: `gps off`.** Sent over serial; it writes
`wpmesh/gpsen=false`, so it survives a reboot — confirmed by rebooting and seeing no
`NMEA reader ON` line. The phone then ran healthily through the screen-off transition and the
240→80 MHz downclock, `up=3min` and counting, `heap=24300 largest=21076` flat.

🛑 **THE CAUSE IS NOT FOUND, AND ONE PLAUSIBLE STORY WAS TESTED AND KILLED.** The obvious
suspect was the PLL switch: `setCpuFrequencyMhz()` 240→80 at screen-off, with GPS holding the
shared user UART at **115200** where the GUI path uses 9600 — and `WiPhone.ino:3715` already
records that switch breaking the *input* path on 2026-08-22 (*"The level was never the problem;
the SWITCHING was"*). **It does not reproduce.** With GPS turned on AFTER boot, the phone sailed
through `CPU 80MHz (idle)` at t=40.8 s and stayed healthy for 135 s. So the downclock alone is
not it.

What is left, in the order worth testing:
  * ⚠ **GPS on AT BOOT vs toggled later** — every wedge so far booted with `gpsen=true`
    (`userSerial.begin(gGpsBaud…)` in `setup()`); the run that survived called
    `updateBaudRate()` on an already-begun UART. That asymmetry is real and untested.
  * ⚠ **Plain intermittency** — Nick said "every so often". 135 s of survival proves little.
  * ⚠ **Screen UNLOCK specifically**, which is where he saw it and which serial cannot exercise.
    This needs a human with a thumb.

⚠ **`panicwatch` was suspected and CLEARED**: the wedge began minutes after it attached, but a
control run with panicwatch attached and GPS off gave 6 `HEALTH` lines in 100 s. The instrument
is not the disease.

- [ ] **Leave GPS off until this is understood.** Turning it back on is one serial command, and
      it is how you reproduce the bug — do that deliberately, with `tools/panicwatch.py 025A3F65`
      already attached, not by accident.
- [ ] **Next capture wants the `Guru Meditation` HEADER line**, which was filtered out of the
      first capture and is the one thing that would name the panic class outright.
- [ ] ⚠ **Unrelated but visible throughout: `resolveDomain(): errno=210` for `pool.ntp.org` on
      every boot.** WiFi associates (`wifi=1`) and DNS does not resolve. Not the wedge, but it is
      the same shape as the fault Jake documented on his handheld, and the clock depends on it.

### ✅ CLOSED 2026-08-25 — THE WALLPAPER, AND THE INSTRUMENT THAT FOUND IT (0.9.13)

Nick: *"I tried to apply a background from a photo but nothing changed."* **Fixed, flashed to
BOTH phones, and verified by screenshot.**

🛑 **`gui.init()` READ THE WALLPAPER ~50 LINES BEFORE `SD.begin()` MOUNTED THE CARD.** Photos
writes `/background.jpg` to the SD card; the loader asked an unmounted filesystem and got
`false`, on every boot since the feature existed. The fingerprint was in every boot log —
`[E][vfs_api.cpp:72] exists(): File system is not mounted`. 🔑 **Phone 2's card still had
`/background.jpg` at 31,440 bytes — the exact size of `BT09.JPG`.** Nick's copy had always
worked; nothing ever read it. ⚠ **Do NOT "fix" this by moving `SD.begin()` earlier** — it
shares SPI with the screen and that order is deliberate. `GUI::loadWallpaper()` is called
again *after* the mount instead.

🔑 **WHY IT LOOKED LIKE NOTHING AT ALL:** a rejected override falls into the SAME path as "none
chosen", and this phone ships a `/background.jpg` **in SPIFFS** — the dark texture everyone
reads as the default. Something always loaded. Just never the user's picture.

Also closed with it: **two different size ceilings on one photo** (viewer 2 MB, loader 1 MB —
a photo in between viewed, said "Wallpaper set", and vanished at the next boot; both are 2 MB
now); **setAsWallpaper() now verifies** by asking the loader and rolling back on refusal;
**wallpapers FILL the screen** (TJpgDec only halves, so every 480x270 photo on the card was a
240x135 band across the top — now resampled to cover, cropped to centre).

🔬 **NEW INSTRUMENT, AND IT IS THE POINT: `shot` + `tools/shot.py`** — the live frame as a PNG
on the host. **The reason Photos shipped broken is that a cable cannot press keys**, so no GUI
claim in this repo was ever checked. Now it can be:

```bash
tools/shot.py /dev/cu.usbserial-025A3F65 out.png --wait 16 --cmd "wallpaper set BT01.JPG"
```

Plus `wallpaper` on the console (what the loader found, from where, at what pixel size, and why
it refused) and `wallpaper reload | list | clear | set <name>` — `set` runs **the same
`photosSetWallpaper()` the menu runs**, so the path is provable over the cable.

- [ ] ⚠ **`/photos/20260823_093939.jpg` on phone 2 is 0 bytes** — a failed upload. Photos lists
      it and cannot open it. Small, and its own job.

### ✅ CLOSED 2026-08-25 — "PHONE 2 NEVER LOCKS" (0.9.16). IT WAS LOCKING; THE FIRST KEY UNDID IT

Nick: *"whiphone two doesn't like its screen to lock ever… I just don't want to be accidentally
pushing buttons while it is in a bag."* **Root-caused, fixed, both phones flashed, before/after
proven with the same script.**

🔑 **`GUI::inCall()` said "yes" for TEARDOWN.** It tested `not NotInited/Idle/Error`, which is
true for `HangUp`, `HangingUp`, `HungUp` and `Decline`. Its one caller is the unlock path, which
reads that as "a call is coming in, let any key answer it" and clears `locked`.

🛑 **One press of END unlocks a locked phone — on BOTH phones.** END sets `HangUp` from anywhere
with no call needed, *before* the same press reaches the unlock check. 🛑 **And with a SIP
account, EVERY key unlocks for as long as SIP sits in teardown** — measured in this repo at 19
min and SIX HOURS at `sip=6` when the proxy is unreachable. **That is the difference between the two
phones: PHONE 1 has the account (registered to voip.ms), PHONE 2 has none.**
⚠ **CORRECTED 2026-08-26 — this sentence had the phones the wrong way round, and line ~908 of
this same file always had it right.** Measured over the cable, `sip` on each:
`025A3EAF` (phone 1) → `account LOADED (sip:565611_nikguy@seattle1.voip.ms)`;
`025A3F65` (phone 2) → `account NOT LOADED`. The mechanism above is unchanged; only the label
on the phone was wrong.

Fixed by making `inCall()` mean live-or-ringing, using the SAME set `sipNeedsFullSpeed()` and
the WiFi auto-switch gate already use — ⚠ **the third guard here to need this correction**.
Answering a ringing phone on any key still works (`BeingInvited` is in the set).

⚠ **`lock` on the console is the new diagnostic** and it prints the SLEEP gate on purpose: the
lock is not its own timer, it fires inside `SCREEN_SLEEP_EVENT`, so a phone that dims but never
sleeps never locks either — two causes, one symptom.

- [ ] ⚠ **Loaded, not fired:** `lock_keyboard` defaults to **0** when a `[lock]` section exists
      without the key, while a missing SECTION defaults to 1 (`WiPhone.ino`). Same disagreement
      that shipped a phone which never dimmed or slept (2026-08-22 audit). Both phones read 1
      today, so it was not this bug — but it is a live landmine.
- [ ] 🔎 **Not verified by machine: answering a REAL incoming call from the lock screen.** The
      set is the same one two other guards use and `BeingInvited` is in it, but no call has been
      placed to a locked phone. Fold this into the existing "ring the phone" task below.

### ✅ CLOSED 2026-08-25 — MENU CONTRAST OVER A WALLPAPER (0.9.14)

Nick, right after the wallpaper fix: *"the menus have no contrast… can the menu items have a
translucent grey background so I can see the words no matter what background I pick?"*
**Done, both phones flashed, before/after screenshotted from one build.**

🔑 **It got worse the moment the wallpaper started working.** Every phone had been showing the
same near-black SPIFFS texture, and white text sits on that perfectly. The main menu is the ONE
menu built non-opaque (the `false` ending its `MenuWidget` constructor, GUI.cpp) so the picture
shows through — and a real photograph behind it made four of five rows unreadable.

Fixed with a **scrim**: `guiDrawScrim()` lays a translucent grey plate under each non-selected
row via the sprite's existing `pushTransparent()`. ⚠ **Checked as a number, not by eye** — grey
56 at alpha 190 puts the plate between 41.7 (black photo) and 106.7 (white photo), so white
text measures **5.35:1 at worst**, above the 4.5:1 floor for *any* wallpaper. Tune it live with
`scrim <alpha> [hex]` (RAM only; `scrim 0` is the old look).

🔬 **NEW INSTRUMENT: `key` — the cable can press buttons.** With `shot` (0.9.13) this closes the
loop: any screen can now be reached AND seen without a thumb.

```bash
tools/shot.py /dev/cu.usbserial-025A3F65 menu.png --wait 16 --cmd "key menu" --cmd "key down down"
```

⚠ It injects into `keypadBuff`, so it is a REAL press — the wake, drain loop and each app's
`processEvent` run unchanged. Do not add a second dispatch path.

- [x] ✅ **THE CLOCK FACE TOO — ASKED, AND NICK CHOSE THE SCRIM (0.9.15).** `00:00`, the
      date/network line, the missed-call line and the softkey label each get a plate sized to
      the WORDS, not a panel across the screen. 🛑 **One union plate for the clock and the line
      under it** — their bands overlap, and two translucent plates over the same pixel blend
      TWICE and paint a dark seam right where the eye lands. Measured after the fact by reading
      the screenshot back: smooth from y=68 to y=182, median plate 105 → **5.47:1** behind the
      network line, 79 → **8.21:1** behind the softkey.
- [ ] ⚠ **Cosmetic, pre-existing, NOT fixed:** the softkey label's font is whatever the
      missed-call block left set — 20pt with a missed call showing, 24pt without. The scrim
      measures from the current font so it follows either way, but the label itself changes
      size. Its own small job.

### ✅ CLOSED 2026-08-25 — GPS BEACONING: THE CHAIN WAS FINE, THE SLOT WAS BEING WASTED (0.9.17)

Nick: *"covey isn't seeing wiphone 2 on the map."* 🔑 **Nothing was broken.** The RAK had a
real, full-precision position from `WiPhone-Nick2` `!00449334` matching Nick's RAK and Meteor
Pro to four decimals — **7.7 hours old**, while the node itself was heard every minute. The
phone was indoors: `sats in view: 0`, raw NMEA showing satellites with elevation/azimuth and a
**blank SNR** (knows where to look, hears nothing). Config was correct throughout
(`reporting every 300s to 'hunt-group'`).

Two real improvements came out of it:

- 🔑 **A SLOT IS NOW OWED, NOT SPENT.** Interval 300 s vs `MESH_POS_TX_FRESH_MS` 30 s: the old
  code sent-or-lost in one instant, so under canopy — fix coming and going — a phone that had a
  good fix 40 s ago stayed silent for another five minutes. `posDue` holds the slot open and
  fires on the next fresh fix. ⚠ **Freshness is NOT relaxed**; nothing older than 30 s ever goes
  out. ⚠ One slot max — a flag, not a counter. **Proven on the handset:** nothing owed at t+0,
  `A SLOT IS OWED` after one interval with no fix.
- 🛑 **FIX-QUALITY GATE, and it is measured.** `sats=3 hdop=6.4` gave a position **~20 km** from
  the phone's real location and nothing refused it. Three satellites is a **2D fix** — it assumes
  an altitude and puts the error sideways. `MESH_POS_MIN_SATS=4` (the arithmetic minimum for 3D,
  not a strict bar), `MESH_POS_MAX_HDOP_X10=100` as a backstop. ⚠ −1 = unknown = allowed;
  refusing on silence would break receivers that emit no GGA. Pure `meshPosFixUsable()` in
  `mesh_pos.cpp` with host tests, including the 20 km fix as a named check.

⚠ **METHOD NOTE THAT COST A WRONG "GREEN" TODAY:** grepping `run_tests.sh` output for
`passed|failed` matched a DIFFERENT suite's summary line while `test_pos` was reporting
`1 FAILURE(S)`. **Check the exit code.** `bash tests/run_tests.sh >/dev/null 2>&1; echo $?`

- [x] ✅ **DONE — both phones are on 0.9.20** (`built Aug 26 2026 08:03:20`), read back with
      `ver` on each port, so this is not an assumption about what was flashed. ⚠ The 0-byte
      photo this item mentions no longer exists — it was deleted through the Photos UI on
      2026-08-25. Original wording kept below.
      ~~WiPhone 1 is still on 0.9.16 — it was unplugged for this build.~~
- [ ] 🔎 **The beacon has never been watched end-to-end with a real fix.** Take phone 2 outside,
      then read COVEY's `pos.time` for `!00449334` and confirm it moves.

### 🔴 P1 — TWO THINGS NEED A HUMAN, AND ONE OF THEM IS UNTESTED CODE

⚠ **The wallpaper half of this is now DONE and machine-verified** — see the block above. What
is left below still needs a thumb, but `shot` means you can now SEE the result of every press.

- [x] ✅ **THE PHOTOS APP HAS NOW BEEN RUN — 2026-08-25, over the cable, and it works.** `key` +
      `shot` drove it end to end on phone 1 (Menu > Tools > Photos) and every screenshot is
      clean: the **LIST** (14 files with sizes and the restore-wallpaper row), the **VIEWER**
      (BT01.JPG renders scaled to fit, 2/14), RIGHT walking the folder (⚠ only `key right` was ever sent — LEFT is a separate branch and is still unpressed), and the **OPTIONS**
      menu (Set as wallpaper / Lock / Rename / Delete / Cancel). The empty-file message added in
      0.9.17 was verified on the real 0-byte photo: *"This file is empty (0 bytes) / the upload
      did not finish"*, with Options still reachable so it can still be deleted. **This closes
      the "nobody has ever run it" half of the item.**
### 🛑 CLOSED 2026-08-25 — THE PHOTOS APP COULD NEVER SHOW A MESSAGE (0.9.18)

`ROW_INERT = 0`, and **`MenuWidget::addOption()` refuses a key of 0** — it logs
`menu option key is 0` and adds no row. So the list's result line has **never once appeared**:
"Deleted", "Renamed to X", "Wallpaper set", "Not set - <reason>", "X is locked", the truncation
warning. The constant said `key 0 = not selectable` and did the opposite.

⚠ **IT QUIETLY UNDID THE SAME DAY'S 0.9.13 WALLPAPER WORK** — `setAsWallpaper()` had just been
rewritten to report what the loader actually said, through this exact line. It was only ever
visible over the cable, **which is why the gap survived being "verified". A fix checked through
one channel is not checked.** Fixed with real keys (`ROW_NOTE`/`ROW_TRUNCATED`); the widget is
untouched because other menus may rely on it rejecting 0.

- [x] ✅ **DELETE CONFIRMATION EXERCISED** (2026-08-25, deleting the 0-byte photo at Nick's
      request). `Cancel` is first and selected by default, so a mis-timed OK destroys nothing.
      Before/after listing: exactly one file gone, the other thirteen byte-identical.
- [ ] ⚠ **STILL NOT EXERCISED: the RENAME text-entry screen.** Both
      Rename needs typed input through a MultilineTextWidget and nothing has driven that path.
      🔑 It CAN now be driven with `key`/`shot`.
- [ ] ~~🔑 **OPEN THE PHOTOS APP (Menu > Tools > Photos). NOBODY HAS EVER RUN IT.**~~ It is new
      today: it builds, registers, boots clean and passes the duplicate-menu-id check — but
      **every screen in it needs a key press and serial cannot press keys.** The starring
      feature shipped with exactly this gap the same morning and it had a real bug (a
      filesystem write on the key path, `reset_reason=4`). Assume this one has something too.
      First open also creates `/photos`; put a JPEG in there (Files > /photos >
      "[ Upload into this folder ]") and try view / rename / lock / delete / set-as-wallpaper.
- [ ] **Ring the phone and confirm an INBOUND call lands.** The SIP fix is measured (zero flaps
      in four minutes, self-recovered from a real WiFi drop) but "a call actually arrives" is
      the one property the cable cannot prove. It is the last thing between that fix and done.

### ✅ Closed 2026-08-24 — do not redo these

- **The scrolling freeze is FIXED, and it was never the database.** `WiFi.disconnect(true)`
  blocks **5007 ms** (`begin()` next to it: 30 ms) — the argument is `wifioff`, so it stops the
  whole radio to reassociate. One task, so it froze keypad, screen and WiFi together. That is
  why it was rare (needs the hotspot to blip) and why it always came "with WiFi dropping" —
  **the drop was the trigger, not the symptom.** Fix: `WiFi.disconnect(false)`.
- **The mesh database lives on the SD card** (`meshFs()`, SPIFFS fallback per call). Benchmarked:
  SPIFFS 2599–2845 ms for an 8 KB save-shaped write against SD's 48–57 ms.
- **SIP registration no longer flaps**, and the ping starvation that made the 08-22 attempt fail
  is fixed too. **`chg=` is settled** — it was inverted as well as reading the wrong GPIO.
- **Node list 32 → 200 in PSRAM; nodes can be starred** (`*` on the Nodes list; starred sort to
  the top and are evicted last; the star list is its own file).
- **NEW: Photos app** — `/photos` on SD, with lock / rename / delete / set-as-wallpaper and a
  restore-default. ⚠ **CORRECTED 2026-08-26: baseline colour JPEG ONLY — not BMP, and not
  greyscale.** `TFT_eSPI::drawImage()` sniffs `RLE3`, `I256` and `FF D8 FF` and has never had a
  BMP path, and the ROM TJpgDec refuses greyscale and progressive JPEGs. The app has since been
  run on the handset; see the P1 block.

### 🛠 INSTRUMENTS THAT NOW EXIST — reach for these before theorising

- **`LOOP STALL`** — any superloop pass over 250 ms, logged at `log_e` **and into `/health.log`**
  (rate-limited to one a minute) with `scr` / `cpu` / `wifi` state. Persisting it is what cracked
  the freeze: it separated `scr=0 cpu=80` (idle, the database saves behaving) from
  `scr=65 cpu=240 wifi=6` (screen on, phone active, WiFi down) and pointed away from storage.
- **`TIME_STEP(name, call)`** in WiPhone.ino — times one named step and says so past 150 ms.
  Three successive splits with it walked 5296 ms down to the single guilty argument.
- **`bench`** (serial) — SPIFFS vs SD, 8 KB, the same shape a real save has. Re-measure rather
  than trusting these numbers on a different card.
- **`wifi drop`** (serial) — disconnect WITHOUT marking the radio user-disabled, so the field
  retry path runs on demand. A bug needing someone else's access point to misbehave cannot be
  measured otherwise.
- **`star`**, **`send <chan> <text>`**, **`health`** — the other cable-only exercisers. Anything
  reachable only by a thumb cannot be proven from here; that keeps being the lesson.
- Boot lines carry **`build=<compiler timestamp>`** so a run of samples can be attributed to a
  firmware. Without it the `chg=` question was unanswerable from four hours of data.

### ⚠ THREE WRONG ANSWERS FROM TODAY, RECORDED SO THEY ARE NOT RE-RUN

1. *"The freeze is the database save."* It was blocking ~1.5 s and it really is better on SD —
   **but it was not what Nick was feeling.** The symptom named a component and the component was
   innocent.
2. *"Batch the ~55 small writes."* No change. The write was never the cost; SPIFFS `open` is.
3. *"Chunk the write across loop passes."* **Worse — 31 stalls against 12.** SPIFFS is quick
   until a write crosses a block boundary and forces an erase, and an erase is atomic however
   small the write that triggered it. **A stall the filesystem takes in one indivisible piece
   cannot be chunked around.**

### 🖼 COVEY — ✅ SHIPPED, and this heading was stale for two days

- [x] ✅ **DONE — COVEY's photo viewer shipped (D-115…D-122), is deployed, and has been driven
      on the panel twice.** It even grew past the WiPhone's version: an oversize JPEG is
      rescued out of process by `gdk-pixbuf-thumbnailer` and labelled `shown downscaled`.
      **Read the COVEY handoff, not this heading.** What is genuinely still open there is
      Rename/Delete by finger, one real upload over port 8085, and the `.webp`/`.tif` listing
      question. Original wording kept:
      ~~A photo viewer for COVEY, same idea as the WiPhone's… only the WiPhone half is built.~~

### P2 — ⚠ WATCH FOR THIS RECURRING: your own devices getting evicted

**DMs from phone 1 to COVEY had been failing silently, for a while.** The 32-node table evicted
strictly oldest-heard, and the slot wipe takes `pubKey`/`pkiFlags` with it — so on LongFast, a
public channel full of nodes we will never DM, COVEY lost its slot to the 32nd stranger and came
back **keyless**. Keyless = DMs fall back to the pre-2.5 legacy form, which COVEY refuses
(`err=6`). The message appeared in the thread and looked sent. **The delivery receipt is what
made it visible, hours after shipping.**

Fixed: a node whose public key we know is evicted **last**. But the cap is still 32 on a public
mesh, so:

- [ ] **If DMs to COVEY ever go quiet again, run `pki` FIRST.** `NO KEY` for `!62b8d2fd` is the
      whole diagnosis.
- [ ] 🔑 **To make COVEY re-announce its key: `ssh covey` → `sudo systemctl restart covey-ui`.**
      It emits NodeInfo on start and phone 1 logs `MESH PKI: learned key ... DMs unlocked`.
      ⚠ **The phone's own `announce` is NOT reliable for this** — it drew NodeInfo from two
      strangers across three minutes and never from COVEY, because `nbr` shows **0 direct
      neighbours**: COVEY is multi-hop, and its NodeInfo replies do not survive the trip even
      though routed NAKs do.
- [ ] Consider whether 32 is the right cap now that this phone sits on LongFast.

### ✅ `chg=` IS SETTLED (2026-08-24) — it was inverted as well as mis-read

The charger IC's STAT is open-drain: LOW on the charger, released high off it. The read was
`== HIGH`, so the flag was **inverted for the life of the project**, on top of reading the wrong
GPIO until the same morning — which is what hid it. Measured, one boot, one build stamp:

```
up=20  v=4.20  chg=0     on the charger
up=21  v=4.15  chg=1     <- unplugged; voltage starts falling
up=26  v=4.18  chg=0     <- replugged; voltage jumps back
```

⚠ **`chg=1` now means "ON THE CHARGER". It does NOT yet mean "current is flowing"** — plugged in
at soc=100%/4.19 V it still reads 1, which is either a charger topping off or a pin that tracks
USB presence. Settling that needs a run left plugged until charging genuinely terminates.

⚠ **The old warning about mining historical `chg=` still stands** for anything written before the
build stamp existed: pre-fix the flag was reading GPIO 32 (`USER_SERIAL_TX`, idles high), so old
runs record whether the UART was up. Attribute samples by `build=` or do not use them.

### ✅ The SIP flap is FIXED (2026-08-24) — and the 08-22 revert was never its fault

Three changes; the important one is not the obvious one.

1. 🔑 **PING STARVATION — this is why registration could go `lost` and never come back.** The
   caller is an if/else chain: `if (ping due) ping(); else if (register due) registration();`.
   `ping()` wrote `msLastPing` only INSIDE its `connected()` branch, so with the connection down
   the stamp never advanced, the ping branch was taken on *every pass*, and **`registration()`
   was never reached again.** Only a reboot recovered it. ⚠ **That is the "20 minutes later the
   phone went `lost` and never came back" of 08-22 — and it is reachable entirely on its own.
   It has been there since the initial commit.** `ping()` stamps the ATTEMPT now.
2. **The flap:** `requestRegister()` cleared `registered` on every REGISTER, so the phone
   declared itself unregistered for one round trip per refresh, by construction.
3. **`REGISTER_PERIOD_MS` 60000 → 45000.** ⚠ **Neither 2 nor 3 works alone** — with period equal
   to expiry, the expiry check tripped at the instant the refresh fell due, which is exactly why
   the 45 s attempt on its own only moved the flap. The on-air `Expires` header is untouched, so
   a dead phone's binding still clears at the registrar within 60 s.

Unplanned live proof of (1): WiFi genuinely dropped after a reset during testing and
**registration recovered by itself** — precisely what used to require a reboot.

### P4 — hardware, parts in hand

- [ ] **Build plate v2.** ⚠ ORDER: bench ONE TPS63020 off the plate first (set its **3V3 jumper**,
      tie **PS LOW**); meter header **VBAT → GND** (must read the phone cell and TRACK it); meter
      **D1/D2 band orientation, cathodes toward VIN** (D2 backwards = 4.9 V back-charging the cell).
- [ ] **Publish the woods backplate build guide** (gated on assembly).
      📐 The sheet was geometry-audited 2026-08-23 and there is now a **v1→v2 change sheet**
      (`docs/woods-backplate-v1-to-v2.svg`) so the built plate need not be re-checked wire by wire.
      The shared artifact carries both; its source is `docs/woods-backplate-artifact.html`.

### P5 — small and unblocked

- [ ] **Tell people the `*` key exists.** Starring a node on the Nodes list is `*`, and it
      toggles both ways — but the footer there reads "Select / Back" and nothing on screen
      mentions it. The feature is fine; it is simply undiscoverable to anyone who did not build
      it. A footer hint is the whole fix.

### ❌ DECIDED AGAINST (2026-08-24): giving COVEY its own node superset

COVEY has no node store of its own — `node_list()` reads `iface.nodes` straight from the RAK,
so the list size is the radio firmware's compile-time cap and covey-ui cannot raise it. The only
way to show more would be a persistent superset: every node ever seen, merged with the live list.

**Nick's call, and it is the right one: leave it.** A node the radio has forgotten carries
FROZEN data — its position, battery and hop count are from whenever it was last heard — and
presenting that beside live nodes makes the list *less* trustworthy, not more. "The mesh you can
see right now" is a more honest list than "everything ever heard, some of it fiction". Starring
(shipped the same day) already solved the actual problem, which was finding your own devices in
the noise of a public channel. **Do not re-propose this without a concrete staleness design.**

### P6 — setup, unblocked, quick

- [ ] **Phone 2: flash it to match phone 1**, then set booksync passcode → `2222`.
- [ ] **Phone 2 SIP**: a second free VoIP.ms sub-account + a ring group, so both phones ring.
      ⚠ Never leave both phones primary on the SAME sub-account.
- [ ] **The 2-minute Android-browser upload test**, owed since the upload redesign.

### COVEY, for whoever picks it up

- [ ] **Play-test DEEPFIELD from the Games menu** (not over SSH — different settings file, and the
      SDL focus trap only shows on the launcher path). It is installed, rotated (`screenTurn=90`,
      `controlTurn=0`, `detail=2`), and its d-pad and crosshair bugs are fixed. Confirm the ship
      steers and sound reaches the speaker.
- 📖 **`docs/37_LOVE_Games_On_COVEY.md` in the COVEY repo is the playbook** for any LÖVE game.
      A kit for Jake is in `incoming/deepfield-covey-kit/`.

### Traps worth re-reading before touching anything

- ⚠ **Flashing needs panicwatch STOPPED** — `pkill -9 -f panicwatch.py` FIRST. And beware `&&`
  chains: `grep -c` returning 0 exits non-zero and skips the pkill.
- ⚠ **`cd` into the repo explicitly.** A build "succeeded" in the wrong directory today and esptool
  then flashed nothing, because the shell cwd had been reset between calls.
- ⚠ **Only `log_e` is compiled in.** A `log_d` diagnostic makes a working feature and a broken one
  look identical.
- ⚠ **A scripted multi-edit must write per-edit or not at all.**
- ⚠ **`digitalRead`/`digitalWrite`/`pinMode` DO NOT REACH THE GPIO EXTENDER.** Half this board's
  status pins are `EXTENDER_PIN(n)` == `n + 0x40` on the SX1509, which is out of the ESP32's
  GPIO 0–39 range — and the Arduino core does not reject an out-of-range pin, it silently reads
  a DIFFERENT one (`gpio_get_level()` masks its shift to 5 bits, so pin 64 reads GPIO 32).
  **Always `allDigitalRead` / `allDigitalWrite` / `allPinMode`.** This cost two dead status flags
  for the life of the project. Audited clean 2026-08-24; keep it that way.
- ⚠ **Menu IDs: count UP (48+ is free), never fill the 8/25 gaps.** A duplicate is SILENT;
  `GUI::init()`'s boot check is the only thing that catches it, and it has shipped twice.
- ⚠ **Never restate a constant in a second file.** The `health` trailer did, and was stale within
  the hour. Only the file that owns a constant may print it.
- ⚠ **Reading `/health.log` over HTTP is the dangerous route** — it needs the uploader up, and heap
  has been measured at `largest=10560`, below where an 11 KB allocation has already rebooted this
  phone. Use `health` / `health all` on serial.



## 🔋 2026-08-23: THE FIRST REAL ON-BATTERY RUN — ~6.4 h, NOT ~10 h

Nick was out with the phone all day and asked whether the drain logs were worth reading, warning
that he had charged it in the car. **They were, and the car charge does not spoil them** — it
splits the day into segments, and the discharge segments can be measured on their own. What it
does mean is that `chg=` cannot be used to find the charging periods; they were inferred from the
voltage curve instead.

### THE NUMBER

**3.00 h on battery: soc 99 % → 52 %, v 4.13 → 3.80. That is 15.7 %/h, or ~6.4 h from full to
empty.** The handoff has said **~10 h** since the power work; this is the first measurement taken
**off** USB.

⚠ **"Treat 10 h as unproven from here on" — written here on 08-23 — WAS AN OVERCORRECTION, and
it is this 15.7 %/h run that is the outlier.** The ~10 h figure was recorded (§ "Battery: ~10
hours measured") as *steady state with the first 30 minutes discarded (surface charge)* =
10.0 %/h. That is the same relaxation correction rediscovered from scratch on 08-23 evening,
when a WiFi-off idle hour came in at **8.6 %/h** — within 1.4 %/h of the old number. This 3.0 h
run discarded **nothing** and had the radio hunting for 86 % of it, which inflates it twice over.
**Quote a rate with its conditions attached; stop converting either into an hours figure** until
a run starts an hour after unplugging and continues into the flat part of the curve. No run has
ever watched a pack go full to empty.

**What it was NOT** — both ruled out by the same samples:
- **Not the screen.** Screen was off for **177 of 181** samples.
- **Not the CPU governor.** **177 of 181** samples were at **80 MHz**; only 4 at 240.

So the draw is the radios plus baseline, with the phone sitting in a pocket doing nothing.

### ⚠ LEADING SUSPECT, EXPLICITLY NOT PROVEN

The phone spent **155 of 181 samples in `wifi=1` (WL_NO_SSID_AVAIL)** — i.e. away from any known
network, hunting. `Networks.cpp` scans **every 2 min while disconnected against every 10 min while
connected**, so being away from home is a 5× scan rate all day.

**I could not separate its cost from the curve, and did not pretend to.** Voltage slope is not
linear in SOC, so comparing a hunting block at 3.9 V against a connected block at 4.1 V measures
the battery's chemistry, not the radio. Only one contiguous same-state block was long enough to
fit (105 min, `wifi=1`, 80 mV/h) and there is nothing to compare it against.

▶ **TO SETTLE IT, and it is a cheap experiment:** two runs of an hour each on battery, screen off,
one with WiFi off and one hunting, starting from the same SOC. If hunting is the cause the
difference will be obvious; if it is not, the baseline draw is the finding and the scan rate is
exonerated. **Do not change the scan interval before measuring** — that is how the DFS experiment
went wrong.

### ✅ `chg=` WAS DEAD BECAUSE IT READ THE WRONG CHIP — root-caused and fixed 2026-08-24

It read **0 in all 807 samples**, including two unmistakable charging periods (soc climbing
89 % → 100 %, and 53 % → 86 % during the drive home, voltage rising through both). This document
called it "its own small mystery", then a measured fact. **The cause turned out to be a one-line
bug, found while chasing something else entirely.**

`battCharged` used plain `digitalRead(BATTERY_CHARGING_STATUS_PIN)` — but that pin is
`EXTENDER_PIN(0)` == **64** on the SX1509, and the ESP32 has GPIO 0–39. `digitalRead()` does not
reject an out-of-range pin: `gpio_get_level()` evaluates `(in1.data >> (pin - 32)) & 1` and the
Xtensa shift masks its amount to 5 bits, so `>> 32` became `>> 0`. **It was reading GPIO 32**
(USER_SERIAL_TX / MotorEN), which idles low — hence 0, forever, through every charge.
`cardPresent` had the identical bug and was reading **GPIO 33**, the I2S word-select clock.
Both go through `allDigitalRead()` now.

⚠ **The flag is still not trustworthy, for a different reason.** After the fix it *still* read 0
on USB at soc=96 % with voltage rising. Either the charger had legitimately tapered near full, or
the polarity is inverted (these STAT pins are typically open-drain, **active LOW while charging**,
which makes `== HIGH ? true : false` backwards). **Settle it from a low pack — see P2 — and do
not flip it on the theory.** Until then the voltage curve remains the only honest source.

### 🛠 HOW THE LOG WAS READ, AND WHY NOT OVER WIFI

New serial commands: **`health`** (last 24 KB) and **`health all`**. They stream `/health.log`
straight to UART from a 256-byte stack buffer — no WiFi, no sockets, no heap.

⚠ **The obvious route, `http://wiphone.local/log`, is the dangerous one.** It needs the WiFi
uploader up, and at the moment of reading, internal heap was **free=11448, largest=10560,
min-ever=196** — *below* the ~16 KB baseline at which an 11 KB allocation has already aborted the
WiFi PHY and rebooted this phone once. **The log exists to explain restarts; fetching it must not
cause one.**

### ⚠ THE LOG WAS ONE BOOT FROM BEING HALF-EATEN

It measured **98,453 bytes against a 96 KB cap**. The next boot line would have trimmed it to the
newest 32 KB (~4 h) and taken the start of the run with it — silently. The cap was raised **before**
flashing, which is the only reason the whole run survived.

**`HEALTH_LOG_MAX` 96 K → 256 K, `HEALTH_LOG_KEEP` 32 K → 128 K (~16 h).** The cost is nothing: a
line is ~130 bytes once a minute, so the cap is ~33 h and ~2 MB a month, on a card whose other
occupant is a 5 MB book. The old caution was about a resource that is not scarce, and it had
already cost the reset_reason line for a restart on 2026-08-15.

⚠ **And a self-inflicted one worth remembering:** the first version of the `health` trailer restated
the cap as literals in `serial_cmd.cpp`, and they were **stale within the hour** — it confidently
reported the old 96 K/32 K after the constants had been raised. The trailer is now printed by the
file that owns the constants. A diagnostic that misreports the thing it exists to report is worse
than none.

## ⌨️ 2026-08-22 (late): THE MISSED KEYPRESSES — ROOT CAUSE FOUND BY RAW TRACE

Nick, same evening: *"menu scrolling still misses some inputs but not much"* and — new, and true
**since the beginning** — *"tentap typing, sometimes it will miss a tap and I have to tap again
while scrolling through letter choices."*

### 🔑 THE CAUSE: THE SN7326 DOES NOT SAY WHICH KEY WAS RELEASED

Measured with a raw event trace added for the purpose (`keys raw` on the serial console):

```
+0ms    0x41 P    DOWN pressed
+141ms  0x00 r    ...and the release comes back as key code ZERO
+1047ms 0x54 P    OK pressed
+115ms  0x00 r    ...zero again
+455ms  0x60 P    ASTERISK pressed
+109ms  0x00 r    ...zero again
```

**Key code 0 is CALL in this keymap** (`WIPHONE_KEYBOARD` IS defined — Hardware.h:34 — so the
`#else` MZJ map is what compiles; the stock map in the `#ifndef` branch is dead code and its
code 0 means "1", which is a trap when reading this switch). So **every release the phone has
ever seen was applied to CALL.** The key actually pressed never had its bit cleared, stayed
"held", and was therefore DEAF to its own next press until the 350 ms stale sweep let go.
A person re-taps in 150–250 ms — inside that window. That is both reported symptoms, exactly.

⚠ **THE VENDOR CODE HALF-KNEW.** `newState < keypadState` is commented *"Some buttons were
released silently"* — a workaround for this very behaviour. It re-armed `keypadState` only, so
it worked for as long as `keypadState` was what gated a UI keypress, and stopped mattering the
moment anything consulted `uiKeyDown` instead.

⚠ **IT ALSO CAUSED THE DOUBLE PRESSES** seen mid-session: with `keyLastUpMs` stamped for CALL
instead of the real key, the contact bounce visible in that same trace — **a re-press 6 ms after
a release** — met a bounce filter that was watching the wrong key and sailed through as a second
menu step.

**THE FIX:** a release carrying code 0 is attributed to whatever key is actually down. That
clears the right bit AND arms the right bounce window. ⚠ **Interrupt-driven reads only** — an
empty FIFO reads back as 0x00 as well, and the only thing separating "nothing to report" from
"something was released" is that an interrupt fires only when an event really happened.

### 🛑 TWO THEORIES TRIED AND RETIRED BY MEASUREMENT — do not re-implement them

- **A 40 ms UI poll of the FIFO.** Added on the theory that the chip's 10 ms auto-clearing INT
  pulse was stranding events with no edge left to announce them. **The trace refuted it flatly:
  `drained` stayed at 0 through whole sessions of menu use while `empty` climbed into the
  hundreds.** It recovered nothing, spent I2C every 40 ms doing it, and made the 0x00 byte
  ambiguous — which is the one thing the real fix cannot afford. Retired.
- **A same-batch bounce exemption** (a release and re-press drained together escaping the time
  filter). Written for a FIFO backlog that measurement says never happens; its counter never
  moved. It is precisely what would let that 6 ms bounce through. Now counts only, never exempts.

⚠ **ALSO MEASURED: THE HELD-KEY HEARTBEAT IS ~109 ms, NOT THE 40 ms THE CODE ASSUMES.**
`SN7326.h`'s `LONGPRESS_DELAY(1)` comment claims 40 ms; the trace says otherwise:

```
+567ms 0x60 P    * pressed
+109ms 0x60 P    * pressed AGAIN, no release in between   <- a heartbeat, not a keypress
```

Short taps (~110-140 ms) usually end before the first re-report, which is why an early trace
looked as though there were no heartbeats at all — **that reading was wrong and is corrected here.**

🛑 **THIS COST A REAL BUG.** A "stale hold" rescue keyed on 100 ms — chosen from the 40 ms claim —
sat BELOW the true interval, so every heartbeat of a slightly-long press was promoted into a second
keypress. Nick found it instantly: *"push OK then push the star key to unlock, it immediately
thinks I'm trying to type the star key in the dialer"* — the unlock swallowed the real press and
the heartbeat 109 ms later went through to the dialer. **The rescue now counts and does nothing.**
It had nothing left to save anyway (`swept` = 0 once releases are attributed properly) and one
clear way to do harm. ⚠ **Anything keyed on a held-key gap must sit well above 109 ms with room for
jitter, and below the 350 ms sweep to be worth having. Re-measure before trusting any number here.**

### ⚠ ONE REGRESSION THE FIX ITSELF CREATED — fixed, but know the shape of it

The first cut of the code-0 attribution set `mask = held`, i.e. it blamed the release on EVERY
key believed down. Release one of two held keys and **both** were marked up, so the key still
under a finger left `uiKeyDown` and its next ~109 ms heartbeat read as a brand-new press — a
**phantom keypress**. It reached the Select+Back sleep chord (lift one finger early and the other
key fires; a spurious Back navigates if the chord had not yet completed), rolling two-key typing,
and the Game Boy (releasing A while holding RIGHT drops RIGHT for ~109 ms — `app_gbc.cpp` reads
`keypadState` directly and the `newState < keypadState` repair is skipped in game mode).

**Attribution now requires EXACTLY ONE key down** (`(held & (held - 1)) == 0`). With two or more
down the chip has said something was released and not what, and there is no honest way to pick —
so it does not: the byte stays decoded as CALL as it always was, and the sweep tidies up. Counted
as `relamb`.

🔑 **Worth remembering how it was caught: an adversarial source review, not the trace.** It needs
TWO KEYS AT ONCE, and every hardware trace taken that night was single-key. The trace found what
reading could not; the review found what tracing could not. ⚠ That same review refuted 19 of its
own 24 findings, several because they cited line numbers from a tree being edited underneath them
— **do not run a source review against a tree you are actively changing.**

### ⚠ KEY_BOUNCE_MS WAS NEVER ACTUALLY IN THE PATH UNTIL NOW

While every release went to CALL, the bounce window was armed on the wrong key — so the 40 ms it
had carried since it was written had **never been tested against real typing**. Attributing
releases correctly armed it for the first time, putting an untested 40 ms directly in the path of
fast same-key tapping, which is exactly the tentap case. **Now 25 ms**: measured bounces are 6 ms
and 13 ms, and no human re-taps inside 80 ms.

### Still standing, and worth keeping

- The UI press edge is `uiKeyDown` ALONE. It used to be nested inside the `keypadState` edge,
  which made `keypadState` a second undocumented veto and made the comment claiming otherwise
  false. The stale sweep now clears both masks, and skips a pass that arrives late (it runs
  BEFORE the drain, so its premise "we would have seen a heartbeat" needs us to have been looking).
- `SN7326.h::readReg()` no longer discards a byte it successfully read. It returned the status of
  a **trailing zero-length write** left over from a begin/end pair wrapped around `requestFrom()`;
  any NAK there reported failure for a key already off the wire, and the caller dropped it.
- A press dropped for a full `keypadBuff` is no longer latched into `uiKeyDown`, so nothing is
  needed to retry it.

📊 **THE INSTRUMENT: `keys` and `keys raw`** on the serial console, plus a `KEYS` line into
`/health.log` on the minute tick **only when a counter moves**. `relfix` counts re-attributed
releases and should climb steadily in normal use; `killed` counts bounces correctly filtered;
`gapfix`/`swept` should now stay near zero. **`keys raw` is the tool that solved this** — the
counters said what was wrong, only the raw bytes said why.

📶 🛑 **SIP: THE DEEPER FIX WORKED AND WAS STILL REVERTED — READ BEFORE RE-TRYING.**
Shipped as both halves (drop `registered = false` from `registration()`; refresh at 45 s not 60 s,
because the refresh counts from `msLastRegisterRequest` when we ASKED and expiry from
`msLastRegistered` when the proxy ANSWERED, so a 60 s period put the 200 OK on the exact instant
the expiry term tripped). **It did what it was meant to: 53 flaps in 31 min → 0.**

⚠ **THEN THE PHONE WENT `lost` AND NEVER CAME BACK** — ~20 minutes unregistered on the phone that
owns Nick's number, ended only by reflashing. Reverted; phone 1 is back on vendor behaviour and
verified `registered: yes`. 🔑 **THE LESSON: THE FLAP WAS LOAD-BEARING.** Clearing and re-setting
`registered` every 60 s meant a run of REGISTER refreshes that never got a 200 OK was INVISIBLE —
the phone re-declared itself REGISTERED the moment any one of them worked. Remove the flap and
the same run shows as a permanent `lost`. **The flap was hiding whether refreshes succeed.**

**RULED OUT, do not re-investigate:** `wifiTerminateCall()` (tinySIP.cpp:1666) does clear the flag
but needs a WiFi drop AND `isBusy()` — neither happened (`wifi=3`, no call); `terminateCall()`
returns at its `!currentCall` guard before touching any flag.
**LEADING SUSPECT, UNPROVEN:** `ping()` (tinySIP.cpp:1567) writes `msLastPing` only **inside**
`if (tcpProxy->connected())`. A failing ping leaves the ping condition permanently true, and
`checkCall()`'s chain is `if (ping due) ping(); else if (register due) registration();` — so
**a failing ping starves registration() forever.**

▶ **NEXT ATTEMPT NEEDS INSTRUMENTATION FIRST, NOT A RETRY.** Log at `log_e`: registration()
actually being CALLED, and the class of each REGISTER response. Until those exist there is no way
to tell "never asked" from "asked and ignored". Do it on **phone 2**, not the phone with the number.

**STATE:** keypad fixes FLASHED to phone 1 (025A3EAF); SIP reverted and re-registered. 1167 host
assertions green. Uncommitted. ⚠ **Phone 2 untouched.**

## ✅ 2026-08-22 (night): EVERYTHING FLASHED AND VERIFIED — plus one change BACKED OUT

**Both phones run commit 1833c4d.** Phone 1: 3+ min clean, zero panics, SIP REGISTERED.

✅ **DUPLICATE TEXTS FIXED AND ROOT-CAUSED ON AIR.** A live text produced two acks with the
IDENTICAL Call-ID and CSeq, and the second was dropped: `SIP MESSAGE retransmit dropped
(Call-ID 0ad4f98c… CSeq 102) - acked, not stored`. Identical Call-ID = retransmission of ONE
transaction, so the guard is the COMPLETE answer and the RFC 3261 18.2.2 "wrong box" hypothesis
is RETIRED — the ack goes to `208.100.60.42:5060`, the same box we register to. It was always
the network (Android hotspot, symmetric NAT).

🛑 **THE 80 MHz MENU EXPERIMENT IS BACKED OUT — `UI_IDLE_DOWNCLOCK 0`.** Nick on hardware: "the
menu is a bit laggy and doesn't pick up every button push." Missed input is a BREAK, not a tuning
problem, so it was NOT dialled to 160 as he offered. 🔑 **The evidence contradicted the obvious
reading:** the new "screen idle" state NEVER FIRED — zero occurrences — so menus were never
running at 80 MHz and the lag was not the low clock. It was `cpuRaiseForUi()` calling
`setCpuFrequencyMhz()` from INSIDE THE KEY-DRAIN LOOP; a PLL switch mid-keypad-read is how a
keypress goes missing. **The level was never the problem, the SWITCHING was — 160 switches just as
hard.** If revisited: raise the clock somewhere that is NOT the input path, and MEASURE the saving
first (never quantified; the backlight likely dominates screen-on draw).

⚠ **SIP REGISTRATION FLAPS ONCE PER REFRESH — CAUSE FOUND, FIX NOT SHIPPED.** `REGISTERED -> lost
-> REGISTERED` every cycle, since the initial commit, invisible until the line was promoted to
log_e. 🔑 NOT the period: `registration()` clears `registered` on EVERY refresh (tinySIP.cpp:1288)
and restores it when the 200 OK lands, so the phone declares itself unregistered for one round
trip, by construction. My first fix (refresh at 45 s) was WRONG and is REVERTED — it just moved
the flap to every 45 s. **The real fix** is to not clear the flag while merely REFRESHING (the
expiry check in `registrationInvalid()` already covers a failed refresh) — deliberately left for a
session with Nick present, because it is core registration state on the phone that owns his number.

✅ Also shipped and verified tonight: uploader idle auto-stop (the battery answer), NTP backoff,
the reference-counted screen-timeout holder, `dimming=0`/`sleeping=0` shipped-config landmine,
the ringtone surviving a WiFi drop, xferStart failing cleanly, and the mesh retry backoff.
⚠ **Ring-init trap, worth remembering:** the patch that should have initialised the retransmit ring
ABORTED on a failed assertion before writing the file, and reported success from the half that
applied — shipping uninitialised members. Saved only because `TinySIP sip` is a global (BSS-zeroed);
`Test.cpp:731` builds one on the stack. Now member-initialised in the class. **A scripted
multi-edit must write per-edit or not at all.**

## 📨 2026-08-22 (evening, Nick out of town): DUPLICATE TEXTS — FIXED IN SOURCE, ⚠ NOT FLASHED

**Symptom:** every inbound text arriving twice or more on the WiPhone.
🔑 **COVEY was POWERED OFF, which rules the SMS mirror out entirely** — both copies came in
over SIP. And the SIP receive path had **no de-duplication of any kind**: `tinySIP.cpp` called
`textMessages.add()` for whatever arrived.

**Mechanism:** a UDP MESSAGE whose 200 OK does not reach the server is RETRANSMITTED by that
server (RFC 3261 timer E/F, ~7 times over ~32 s). We *do* ack correctly per RFC 3428 — the ack
most likely goes UNMATCHED because a symmetric NAT on an unfamiliar network maps the reply to a
different port than the request arrived on. Network property; the phone must tolerate it.

**Fix (commit d3462a5):** de-dup on **Call-ID + CSeq** — the RFC identity of a retransmission,
and exactly what a genuine second text does NOT share (send "ok" twice → two Call-IDs), so it
can never merge two real messages. 8-entry ring. **Still acks every copy** — going quiet on the
second guarantees five more.

⚠ **NOT FLASHED — the affected phone (phone 1, adapter `025A3EAF`) was out of town and only
phone 2 was on the cable.** Flash phone 1 and the duplicates should stop.
⚠ **The fix is also an instrument.** If duplicates SURVIVE it, the new log line names the
dropped Call-ID: duplicates carrying DIFFERENT Call-IDs mean VoIP.ms is re-pushing the queued
text as a FRESH transaction after each REGISTER (we re-register every 60 s), which is a
different fault needing a content+time-window guard instead of a transaction-identity one.
🔑 **TRIPLICATE MEASURED (Android → the SIP number): 3 copies on the WiPhone, 1 on COVEY.**
The COUNT is the evidence: a fixed 2 would mean two delivery PATHS, but a varying 2–3 is the
signature of RETRANSMISSION (VoIP.ms retries at ~500 ms, 1 s, 2 s…), with the phone storing
however many landed. Combined with COVEY's single copy, the diagnosis is settled: one message
upstream, delivered repeatedly over SIP, stored every time.

✅ **PHONE 2 EXONERATED** (Nick asked directly, four independent grounds):
1. **The WiPhone NEVER transmits mirror records** — `smsMirrorPack()` is called nowhere in
   firmware (only its own .cpp/.h and the host test). The SMS mirror is one-way, COVEY → phone.
2. Phone 2 has **no SIP account** (measured: `account NOT LOADED, registered: no`).
3. It was 30 miles away — far outside handheld LoRa, no relay chain.
4. COVEY was OFF during the doubling, so no mesh path was live at all.
📎 **The two-way sync Nick remembered is real but is TWO features carrying DIFFERENT traffic:**
SMS mirror (COVEY → phone, CSM1 on `smsmirror`) carries **SIP texts**; mesh history replay
(phone → COVEY, `RPL?` on `booksync`, D-112) carries **mesh channel texts**. Neither can
duplicate a SIP text.

✅ **Phone 2 FLASHED with the fix 2026-08-22** (it has no SIP account yet, but will).
⚠ **Phone 1 STILL UNFLASHED** — it is out of town with Nick; flash when he is home.

✅ **COVEY shows ONE copy of each text (measured same evening, COVEY powered back on).** It reads
the account history over the REST API, so VoIP.ms holds exactly ONE stored message — which rules
out every upstream explanation (DID delivering to two destinations, `getSMS` window overlap,
account-level duplication) and pins the fault to repeated SIP DELIVERY of that single message.
✅ **WORKAROUND, now that COVEY is on:** phone → SIP Accounts → **Unmake primary**. The phone
stops registering (no SIP delivery, no duplicates) and still receives, because COVEY mirrors to
it over the radio. Cost: no outbound calls/texts from the phone until it is made primary again.
⚠ Turning COVEY on ALONE does not fix it — a mirror record adopts ONE matching copy, so with two
already stored you still see two.
⚠ **No workaround while COVEY is off** — un-registering SIP would mean no texts at
all. With COVEY ON, un-registering the phone (SIP Accounts → Unmake primary) would give single
copies via the mesh mirror.

## 📐 v2 DESIGNED 2026-08-22 (not yet built) — DUAL-SOURCED RAIL, PARTS NOT ORDERED

**Nick's idea, and it is right:** power the plate from the phone's own VBAT so the radio and GPS
survive a dead external pack. v1 rejected VBAT, but **that objection was about the PART**: a
TLV62569 is a BUCK with a 3.4 V input floor and a 1S cell ends at 3.0 V. A **buck-boost** has no
floor, so the objection dissolves with the regulator swap.

**Shipped design (wiring sheet rev v2, commit 4879c5f):** D1 and D2 Schottky-OR the PowerBoost's
5.2 V and the header's VBAT into one buck-boost. Pack alive → 5.2 V wins, phone's cell untouched
(v1 behaviour kept). Pack dead → VBAT takes over. No logic, no firmware. New wire **W20**
(header VBAT → D2 → the OR node); **W6 gains D1**; W7/W8/W10/W12 keep their nets and just land on
the new module. **EN gate carries over unchanged** — a TPS63020 module and the Pololu S9V11E2F3
both behave like the TLV there, and the S9V11E2F3's 100 kΩ EN pull-up matches the TLV's 99.9 kΩ
exactly, so R1 (4.7 k) + W12 keep their measured margin.

🔑 **v2 REMOVES NO CHARGING PATH — Nick asked, and the answer is written into the sheet.** The
PowerBoost stays: it still charges the phone through W4, still charges its own pack over its own
microUSB, and phone-USB still reaches the plate through D1 (measured today). Deleting the
PowerBoost would delete the external pack; that is NOT what v2 does.

**PARTS — ✅ ORDERED 2026-08-22. Nick has the diodes.**
- ✅ **TPS63020 module, Amazon B0H3KQ1VXJ** (DWEII 6-pack). 26 × 18 mm, 2–5.5 V in.
  Pads per the board photos: **VIN · GND · PS · EN** one edge, **OUT · GND** the other,
  **3V3 / 4V2 / 5V solder-selectable**.
  ⚠ **SET THE 3V3 JUMPER** before wiring — it does not ship set for this job.
  ⚠ **PS is power-save select — tie it LOW** (pulse-skipping at idle; the plate idles ~45 mA).
    Do not leave it floating. Check what the module ties it to before assuming.
  ⚠ **EN thresholds are tighter than the TLV's** (V_IL 0.4 V, V_IH 1.2 V vs the Pololu's 0.7 V),
    so checklist item 4 — phone off, EN must read < 0.4 V — is now load-bearing. Our measured
    0.22 V divider still passes, with less headroom than v1 had.
  ⚠ 6 pieces: bench one (3V3 jumper + EN behaviour) BEFORE it goes on the plate.
  ⚠ **26 × 18 mm and ~4 mm tall** — bigger than the TLV breakout. Case impact.
- Alternative if height/size ever bites: **Pololu S9V11E2F3** (#5712, $4.95, 10.9 × 16.5 × 4.1 mm,
  <0.2 mA quiescent, 100 kΩ EN pull-up matching the old TLV's).
- D1/D2: 1N5819 / SS14 / BAT54 (Nick has these).

**⚠ TWO CHECKS OWED BEFORE THE REWORK:**
1. **Meter header VBAT → GND.** It must read the phone's own cell (3.7–4.2 V) and TRACK it as the
   phone discharges. **That pin has never carried current on this plate** — v1 left it empty.
2. **Meter D1 and D2 band orientation — cathodes toward VIN.** D2 backwards = 4.9 V back-charging
   the phone's cell.

**Known tradeoff, accepted:** energy reaching the plate via the phone (pack → boost → charger →
cell → buck-boost) lands ~66% efficient vs ~83% direct, and with the pack dead the plate draws
~45 mA continuous off the phone's own cell. That is the price of the radio not dying.

## ✅ BENCH STATE, 2026-08-22 (final) — REWORK DONE, WHOLE FEATURE PROVEN ON AIR

**R3–R7 are SOLDERED and every close passed the same day:**
- **Phantom power is dead**: pack out + phone alive → no GPS LED (the rail that floated at
  2.54 V now collapses; the meter probe at C2 is optional — the LED and the Error behaviour
  below are the evidence that matters).
- **The screen tells the truth**: both supplies out → mesh screen **Error** within 5 s
  (witnessed by Nick); power back → **`MESH RADIO RECOVERED — reconfigured, announcing`** in
  the serial log within 10 s, and the boot-style announce went out on air (captured). Radio,
  GPS (8-sat fix through R6), SPI through R3–R5, health check 0 false alarms — all verified
  post-rework, firmware 2d3f85c FLASHED.
- 🔑 **DISCOVERY: USB is a second legitimate supply for the plate.** The phone's 5 V node
  (~4.6–4.7 V on USB) reaches the header 5 V terminal, and the TLV runs from 3.4 V — so a USB
  power bank keeps radio + GPS fully alive with NO woods pack. Ran for hours in exactly this
  state (pack out, USB in) with zero drama — which also soaks the review's
  PowerBoost-back-feed worry (formal current probe into the PB 5 V pad remains optional).
  Consequence: the LOST/RECOVERED test CANNOT run over USB serial — both supplies must be out.

**PLATE SWAP RULES (stock LoRa plate ↔ woods plate), both directions safe anytime:**
1. **Swap COLD** — phone fully off, always (driven SPI into an unpowered radio otherwise).
2. **BT2.0 out whenever the woods plate is off the phone** — the PowerBoost keeps boosting and
   the loose 5 V terminal is hot; the 3.3 V rail is safe (EN pulled low by R1) but 5 V is not.
3. Optional: `gps off` before living on the stock plate (spares a future bench session the
   "bytes=0 = wiring?" confusion). Firmware needs NO change — radio config rebuilds at boot,
   mesh identity lives in the phone, and the health check simply never fires on a plate whose
   rail cannot die independently.

**Next: Nick designs the shell/case.** Constraints already known: BT2.0 reachable (it IS the
off switch) · PowerBoost micro-USB cutout (the pack's charger) · case should leave NO room to
populate the JST (paralleled packs, July 2026) · pigtail strain relief where it crosses the
pack + u.FL clicked-not-resting · antenna/SMA bulkhead placement · GPS antenna sky view ·
PowerBoost LED visible if possible (it is a useful power-state tell).

## 🔧 (superseded 2026-08-22 evening — rework was owed, now done; kept for the record)

**Physical state right now:** the plate is **MOUNTED and fully proven on air** — phone charges
from the pack, plate rail gates correctly, **GPS has a live fix (9 sats, HDOP 1.1, 115200 — NOT
9600, see below)**, and the radio TXed an announce and learned a PKI key back through the new
coax/SMA path. The phone runs the health-check firmware (2d3f85c).

### ⚠ OWED: the R3–R7 resistor rework (parts chosen, sheet updated, NOT yet soldered)
**Why (measured):** pack disconnected + phone alive = the phone's driven pins back-feed the plate
through ESD clamps. Plate 3.3 V floated at **2.54 V** (one diode under 3.3), the 5 V node at
**2.18 V** (backwards through the TLV's high-side body diode). An RFM95W runs at 2.5 V: it
answered REG_VERSION, the mesh screen said **READY**, RX worked, **TX was silently impossible**,
and the GPS LED blinked on a plate with no battery. Nick reproduced the READY lie on screen,
including across a reboot.
**The fix (wiring sheet rev 2026-08-22 has every position):**
- **R3–R6, 4× 1 kΩ series** in the phone-driven lines only: W14 MOSI, W15 SCK, W16 NSS,
  W18 GPS-RX. W13 MISO / W17 GPS-TX stay plain wire (plate-driven; EN gate covers them).
- **R7, 10 kΩ, RFM NSS pad 5 → PLATE 3.3 V — REQUIRED** (adversarial-review find): GPIO 27
  floats during every ESP32 reset; NSS drifting low lets the radio drive MISO = **GPIO 12 =
  MTDI, the flash-voltage boot strap** → phone fails to boot. The stock v2.2 plate fits this
  exact pull-up (its R2 100 k). Referenced to the PLATE rail so it adds no phantom feed.
**After soldering, two closes:** (1) repeat the pack-out probe at C2 — expect **well under 1 V**
(the review rates the collapse threshold computed-not-proven; this measurement closes it);
(2) with the pack out ≥5 s the phone's mesh screen must now say **Error**, and ≤10 s after
plugging back in it must say Ready again (announce fires — that's the firmware below).
**Also owed from review (UNCERTAIN, 2 min):** pack out, phone on USB, plate mounted — measure
current into the PowerBoost 5 V pad / voltage at its BAT pad. Expect ~0 (TPS61090 output body
diode blocks); sustained current = back-feed into the boost, wants a Schottky in W4.

### ✅ Firmware: the radio must keep proving it exists (2d3f85c, flash OWED — port was unplugged)
`MeshPhy::healthCheck()` = REG_VERSION **plus** REG_OP_MODE (version alone is a liar twice over:
phantom power, and a reconnected pack answering 0x12 from POR defaults — present, deaf, mute).
Service checks every 5 s → `MESH_RADIO_ERROR` loudly; every 10 s in ERROR it re-runs the FULL
register config (`begin()` is now a shell over `reinit()`) so a swapped pack rejoins in ≤10 s
with a boot-style announce, no reboot. Boot probe hardened with a SLEEP-readback second opinion
(floating MISO can't fake two registers). ⚠ Detection is only honest once R3–R6 are in.

### ✅ GPS: **115200, NOT 9600** — and COVEY's repo knew (its gps.py, D-062)
The M100 was alive all along; 9600 was COVEY's D-033 PLAN number, never amended after the
implementation moved. Scanned on hardware: only 115200 produces sentences. New serial tools:
**`gps baud <n>`** (persists, wpmesh/gpsbaud, retunes live) and **`gps raw`** (hex+ASCII — tells
wrong-baud from UBX-binary). Default now 115200 (`GPS_SERIAL_BAUD_DEFAULT`). Both repos'
docs corrected; the lesson is written into Hardware.h: **when two repos share a part, the working
code is the authority, not the decision that proposed it.**

### If the pack dies in the field (until the rework is soldered)
The phone itself is fine (5 V pin goes to 0 = the shipped state), but the plate half-lives on
phantom power and the screen LIES about Ready. Field rule for now: pack dead → `gps off` + power
the phone off, or pull the plate. After the rework: the screen tells the truth by itself.

*(Superseded, kept for the reasoning:)* the first-power sequence below exists because a reversed
cell or a solder bridge is unrecoverable on the PowerBoost, and both are cheap to rule out while
the plate is loose.

### As-built deviations from the BOM (all deliberate, all recorded)
- **F1 = GF300** (16 V, 3 A radial PPTC) in the **cell POSITIVE** leg, not the 1 A the BOM
  originally specified. The 1 A was undersized: that leg carries boost INPUT current, ~1.6 A at
  a fresh cell and ~1.75 A once it sags to 3.3 V, and PPTC hold derates ~0.85× at 40 °C.
- **BT2.0 pigtail into the PowerBoost's BAT/GND PADS**, not the JST. The pads are wired in
  parallel with the JST, so this is electrically identical. ⚠ **THE JST MUST STAY EMPTY** —
  a pack on both inputs is paralleled cells, which is exactly what destroyed both packs'
  protection FETs in July 2026.
- Battery leg is 20–22 AWG (BT2.0 stock). The 26–30 AWG in the BOM is for signal/3.3 V runs
  only; it is NOT adequate for the cell leg.

### ✅ Everything measured today (the build guide's owed numbers — all pass)
| measurement | result | meaning |
|---|---|---|
| 5 V header fed at 5.0 V / 200 mA limit, no USB | **phone charges** | the pack-drives-phone-through-the-header topology is now MEASURED, not inferred |
| header 3.3 V pin | **3.3 V phone ON / 0 V OFF** | a true power-state signal, so the TLV EN gating works; also proves `ENABLE_DAUGHTER_33V` does not gate this rail |
| TLV62569 EN→VIN, unpowered | **99.9 kΩ** | the internal pull-up, measured directly (within 0.1% of the assumed 100 k) |
| EN powered, no pull-down | **4.95 V** | without gating the plate's rail would be permanently ON |
| EN with the 4.7 kΩ fitted | **0.22 V** | ✅ PASS vs the 0.4 V guaranteed-off limit; 0.234 V at the PowerBoost's 5.2 V |

### ✅ The stock LoRa plate's resistors — CLOSED, and the plate is a v2.2 (not v2.0)
Vendor design files found and saved to **`~/Downloads/wiphone-lora-v2.1/`** (schematic PDF +
rendered PNG, BOM .xlsx, Eagle .sch/.brd). Published on wiphone.io, NOT GitHub. Authoritative:
- **R1 = R2 = 100 kΩ** pull-ups (RESET and NSS to 3.3 V). Nick measured 98.9 k and 52 k — the
  52 k is an in-circuit parallel-path artifact, not a different part.
- **R6 = 0 Ω**, the series element of an otherwise-unpopulated pi network (**C2, C3 = DNP**).
  Netlist: `ANT = {R6.2, C3.2, U1.ANT}`, `N$9 = {R6.1, C2.2, J1.SIGNAL}`.
  ⇒ **the stock RF path is module ANT → 0 Ω link → U.FL. No matching, no filter, no PA.**
  So soldering coax straight from `ANA` to the SMA pigtail is what the factory does minus one
  joint — the "one genuinely open RF question" is closed.
- **R7 and R11 DO NOT EXIST** (nor R3/R4/R5 — the numbering simply has gaps).

### ✅ FIRST POWER DONE — steps 1–4 PASSED, 2026-08-22 (bench supply AND LiPo)

| measurement | result | meaning |
|---|---|---|
| 5 V rail (PowerBoost out) | **5.15 V** | boost running; PowerBoost-typical, in spec |
| header 3.3 V pin → GND, no phone | **0.22 V** | 🔑 this pin IS the EN node (W12). The divider predicts 5.15 × 4.7 k/(4.7 k + 99.9 k) = **0.231 V**, so 0.22 V is a three-way pass at once: W12 continuous, R1 fitted and correct (missing R1 → ~5.15 V and a permanently-live plate), internal pull-up intact. Well under the 0.4 V guaranteed-off limit. **Far easier to probe as built than the TLV EN pad — use this pin.** |
| plate 3.3 V rail (across C2 and C4) | **0 V** | ✅ THE key check — the EN gate really does collapse the plate rail with no phone attached |
| repeat on the LiPo (step 4) | **same three readings** | no bench-supply artifact |

⚠ **The PowerBoost's LED is on continuously and that is CORRECT** — the 1000C's green
power-good LED lights whenever the boost is enabled (the yellow/red charge LEDs only mean
anything when its own micro-USB is fed). It is already inside the measured few-mA quiescent.

### ▶ MOUNTING (step 5) — mate it COLD, and here is why
**Phone fully shut down (not asleep — the header 3.3 V pin reading 0 V is the proof) AND the
pack unplugged at the BT2.0 before the plate touches the header.** Then: seat squarely and fully
home → plug the pack back in (phone should begin charging while still off) → power the phone on.
Two independent reasons, both about the mate itself:
- **The mesh is live**, so the ESP32 is actively driving SPI (GPIO 12/13/14/27) at 3.3 V. Hot-mated,
  those lines land on an RFM95W whose rail is still down (EN does not rise until the header's 3.3 V
  pin makes contact) and the drive current goes through the RFM's ESD clamps to find its rail.
- **Ten contacts mate in an arbitrary order** by hand. With the pack live, W4 presents 5.2 V at the
  header's 5 V pin, which must not make before GND.

🔋 **The BT2.0 unplug is the real off switch, not the phone's power button.** The EN gate stops the
PLATE RAIL draining the pack when the phone is off, but the 5 V boost keeps feeding the phone's
charger regardless — that is the intended power-bank behaviour, and it is the overnight drain path.

### ▶ FIRST-POWER SEQUENCE — do these in order, plate still OFF the phone
1. **Meter BT2.0 polarity with a DMM. Do not trust wire colour.** Confirm pack + reaches the
   PowerBoost BAT pad. Reverse-connecting a LiPo here is not recoverable.
2. **Confirm the JST is empty.**
3. 🔑 **First power from a BENCH SUPPLY, not the cell** — 3.8 V, **300 mA limit**, into
   BAT+/GND (the PowerBoost cannot tell it from a cell, and a LiPo cannot be current-limited).
   Expect: quiescent draw only; **5 V rail ≈ 5.2 V**; and — the interesting one —
   **the plate's 3.3 V rail should read 0 V**, because EN is gated off the phone's 3.3 V pin
   and no phone is attached. A live 3.3 V rail here means the EN gate is miswired.
4. Only when 3 is clean: swap the bench supply for the LiPo, re-check the same three readings.
5. Then mount on the phone and verify, in order: phone charges · plate 3.3 V comes up with the
   phone · `gps on` then `gps` shows bytes+sentences (bytes rising with sentences 0 = wrong
   baud) · radio still transmits (serial `nbr now`, or watch a mesh send).

⚠ **The PowerBoost will not run at all without a cell (or bench supply) on BAT** — a dark board
with nothing attached is expected, not a fault.

## ✅ 2026-08-21 (afternoon): NEIGHBOUR INFO — who hears whom, on both devices

Nick's ask, for building a mesh map while hunting. **Phone side:** a RAM table of
nodes heard with NO relay (hop_start == hop_limit; hop_start absent = unknown,
never counted — a relayed node is not a neighbour and an edge to it would be a
road that does not exist), announced as a real Meshtastic NeighborInfo packet
(portnum 71) on the PRIVATE channels only — never the public primary, and never
booksync/smsmirror (machine channels, nobody reading). **My node > Neighbor
info** cycles off / 1h / 4h; serial `nbr` lists the table, `nbr on|4h|off|now`
drives it. The encoder is hand-rolled protobuf pinned BYTE-FOR-BYTE to the real
runtime's output (tools/gen_neighbor_vectors.py → tests/test_neighbor.cpp);
proto3 default-elision is part of that contract (emitting `snr=0` where the
reference emits nothing cost 5 bytes and byte-equality).
🔑 **MEASURED, and it decides the whole feature: the RAK forwards port-71
packets to its clients ONLY while its own NeighborInfo module is enabled.**
Module off → the phone announced twice and COVEY saw nothing; module on →
`[nbr] !00449040 hears 1 node(s): !62b8d2fd` immediately. So COVEY's toggle is
not about its own broadcasting, it is the switch that lets COVEY (and the
Android app) see anyone's neighbours at all.
⚠ The RAK's own module has a documented 4-hour floor and will not transmit over
LoRa on a default-key channel (COVEY's primary is stock LongFast) — so COVEY
contributes its local view, and the PHONE is what actually puts neighbour data
on the air. That inverts Nick's expectation ("covey more often") and is worth
saying out loud.

## ✅ 2026-08-21 (afternoon): MESH HISTORY REPLAY — the phone is COVEY's memory now

**docs/replay-spec.md is the contract; CHANGELOG has the narrative.** The phone
rings the last 64 channel texts it hears/sends (PSRAM ~13 KB, internal RAM
untouched); COVEY asks `RPL?` on booksync after a blind window (lent radio /
powered off) and gets records back — original UTC timestamps, oldest first, one
packet per 3 s, honest gap marker. **Proven live in the exact bag scenario:**
two messages sent through COVEY's own radio while covey-ui slept came back on
wake AS OUTGOING in its Howe-group history, timestamps within seconds of truth.
Wire format pinned both ways by generated vectors (tools/gen_replay_vectors.py
→ tests/vectors_replay.h; regenerate whenever covey_ui/replay.py changes).
Serial `replay` = ring/pending/last-served. Machine-prefix rule extended: RPL*
never reaches chat on either device (unknown-prefix fallthrough IS chat).
🔑 Clock traps now encoded in the spec: `getExactUtcTime()` not
`getExactUnixTime()` (local-shifted epoch — cost 7 h on the first live run),
and the asker pads t2 by +600 s (a tight window served 0 records with both
sitting in the ring; the no-RTC Pi and the phone WILL disagree).
COVEY's half: covey_ui/replay.py (reference) + replay_glue.py + the
_replay_tick in app.py; its HANDOFF carries D-112.
v2 lines live in the spec: GPS-true time, LAN transport, SD-persisted ring.
✅ **The adversarial review RAN (19 agents) and its two confirmed findings are
FIXED + flashed + verified on air:**
1. 🔒 **PRIVACY (high): legacy channel-encrypted DMs were entering the ring** —
   this phone deliberately accepts that DM form, and it decrypts with the
   CHANNEL key, so replay would have BROADCAST a private message to every
   booksync member. The spec's "DMs are safe, they're PKC" assumption was
   simply false for the legacy form. Ring capture is now gated on
   `toInternal == MESH_BROADCAST_ADDR` — that check IS the DM exclusion.
2. **Gap honesty (medium): a reboot hid its own losses.** Coverage was proxied
   by ring FULLNESS, and the ring is empty after every reboot — the exact case
   the flag exists for. Now a `replayCoverFrom` stamp (first packet after the
   clock locks) is compared against t1. Verified on air: an all-time ask now
   answers GAP where it used to claim completeness.
⚠ Three verify agents were cut by a usage limit; their findings (skewed-window
handling, ASK_MAX-vs-ring interplay) are UNJUDGED — the workflow can be re-run
from the session's workflows/scripts/replay-review-wf_963fee0d-85c.js.
⚠ Capture needs a KNOWN CLOCK: for the first minute or two after a reflash the
phone hears traffic and rings NOTHING (correct — an unstamped record can serve
no window, and it cost a confusing bench minute today).

## ✅ 2026-08-21 (day): the redesign LANDED — chunked protocol + raw transport

**What shipped (see CHANGELOG for the narrative):** chunk_proto.h (verdicts, name
safety, CRC32 — host-tested), `/chunk` on the WebServer (multipart fallback, 4 KB
pieces), **the raw transport on port 8081** (one TCP connection per batch, raw
text/plain bodies read incrementally by a non-blocking state machine in the main
loop, CORS-simple so the page fetches cross-port, 16 KB pieces), the page JS
(probes 8081 → falls back; batch survives per-file failure; native form kept for
fetch-less browsers), durable-bytes-in-RAM (File::size() lies on open files),
chunkPersist-not-finalize on breaker pauses, no breaker trips at all mid-batch
(cycling inside tight heap LEAKED — floors stair-stepped 3328→616 until removed),
serial `heap`, per-file floor telemetry, tools/chunk_push.py (both transports +
fault injection).

**Acceptance vs the brief's bar:**
- ✅ 3 consecutive 4-book batches over the fast link *(the phone-to-phone hotspot —
  the original crash-day link)*, all 12 files byte-verified, **zero** breaker
  events, **zero** panics; 66–97 KB/s, ROUND-TRIP-BOUND on that link (home LAN
  should be several× faster; piece size is the lever, in-flight is window-bound).
- ⚠ **"floor never below 10 KB free" is UNMET AS WRITTEN and appears unmeetable on
  this stack**: worst sampled per-file floor 1,384 free (allocator min-ever 144 B).
  These are sub-millisecond WiFi-task burst allocations during ANY sustained RX —
  invisible to sampling, non-fatal by design (failed RX alloc = dropped frame =
  TCP retransmit), and ~90 MB of hardware transfers today crossed them without a
  single crash. Nick decides whether the bar moves or more mitigation is wanted.
- ⏳ **Real-mobile-browser run still owed**: Nick's Android on NickH-wifi opening
  http://<phone-ip>/ and uploading any file (a photo works) — 2 minutes, closes
  the bar's last clause. The page JS itself is proven in a real engine against a
  faithful mock (15 % injected failures, CRC corruption, resume) and both
  transports are proven against the real phone via chunk_push.py.

**Traps for whoever touches this next:**
- ⚠ Flashing REQUIRES panicwatch STOPPED (it eats esptool sync; one attempt left
  the phone in download mode). `pkill -f panicwatch` → `pio run -t upload` →
  restart it.
- ⚠ `pieces=` in the done-line telemetry is per-SEGMENT (resets when the batch
  file reopens after a connection drop) — not per-file. The floors are per-segment
  too. sent==size in chunk_push output is the honest per-file check.
- ⚠ The legacy whole-file `/upload` can still wedge the main loop if its client
  vanishes mid-POST (framework parser + flooded RX window). Chunked pieces dodge
  this by being smaller than the TCP window. `/upload` stays for curl + no-JS.
- The `.bin` probe clutter in /books from graded testing is invisible to the
  Books list; harmless.

## 📻 COVEY BT investigation (same day, for the COVEY repo's record too)

Nick: RAK invisible to the Android Meshtastic app. Radio config verified
bluetooth.enabled=True, mode 1, pin 123456 (covey-ui stopped/queried/restarted).
Mac holds a stale `AdaDFU` pairing (DFU-mode identity from the failed BLE-OTA era;
cannot explain app-mode invisibility; blueutil not installed, removal parked).
Android bonds wiped by Nick + Meshtastic app device list cleared — STILL invisible
⇒ the RAK is likely NOT ADVERTISING. Leading theory: covey-ui holds the serial
phone-API 24/7 and nRF52 Meshtastic suppresses BLE while the serial API is in use
(would also date "when it used to work" to before the always-on UI / the 08-14
2.7.26+bootloader reflash, which also wiped bonds). **Decisive 30 s test ready:
stop covey-ui, Nick scans from the app, restart.** Related: outgoing messages sent
from the Android app never appeared in COVEY's UI — investigated, mechanism
narrowed (meshtastic lib discards own-node carbon copies with from==0 BEFORE
publish — but COVEY's journal shows ZERO such drops, so more likely the firmware
never cc'd the serial session at all); definitive answer needs the phone connected,
i.e. blocked on the BT fix. Full agent report should be re-run if lost; key files:
covey_ui/mesh_meshtastic.py:720 (_on_receive), :1004 (send_text local echo),
meshtastic lib mesh_interface.py:1574 (the own-packet drop).

## 🌅 2026-08-21: THE UPLOAD REDESIGN — where it stands, exactly

**The phone RUNS the chunked build** (safe daily firmware: upload paths are inert
until `up on`; serial `heap` command in; WiFi/SIP/mesh untouched). **NOT accepted,
NOT done** — read this before continuing.

**Done + proven:**
- `chunk_proto.h` (verdicts/name-sanitize/hex/CRC32, host-tested), `/chunk` GET+POST
  on the transfer server, page JS chunked sender (4 KB stop-and-wait, CRC32, resync,
  honest retries), `tools/chunk_push.py` (bench pusher: --corrupt/--restart-at/
  --resume/--gap all proven against the mock), serial `heap`, per-request + per-file
  heap telemetry, chunk-aware breaker (8 s non-escalating pauses for chunk-paced
  traffic; resume line 7168 = backpressure line).
- **Mock-proven byte-perfect in a real browser** (scratchpad mock_phone.py): 3-file
  batch through 15% injected failures, CRC-corrupt piece 422+recovery, mid-file
  abandon + true resume (tail-only resend). JS crc32 == zlib == chunk_proto.h.
- Measured (via `heap` + graded probes): request machinery transient ~10 KB; 4 KB
  body costs nothing more, 8 KB dips 4 KB, 16 KB grinds largest to 636 B; each
  request fragments largest ~1.6–2 KB; server recycle heals it; boot largest is a
  stable ~19 K (the "9–10 K boots" were UPTIME fragmentation, mystery closed).

**The hardware verdict that redirects the work:** two real runs (unpaced, and paced
150 ms/piece) both died the same way — hundreds of per-piece CONNECTIONS accumulate
TIME_WAIT pcbs (120 s tail, heap-backed) + per-request String churn, largest pinned
~5 K, breaker flapping, client gives up. **Pacing does not save the WebServer path;
its per-request cost is the killer.** ▶ NEXT: a raw single-connection chunk endpoint
(one TCP connection per file/session, raw bodies read incrementally at OUR pace into
the PSRAM block — in-flight bounded by TCP window 5744 + stop-and-wait, so the flood
is impossible by construction; CORS-simple so the page can use it cross-port;
WebServer `/chunk` stays as the fallback). Sketch: non-blocking state machine pumped
from the main loop, 2 s dead-client deadline, piece cap 32 K.

**Review findings OPEN (a 14-agent adversarial review ran; its verify stage was cut
by a usage limit — findings are triaged, unfixed unless noted):**
1. 🔑 **CONFIRMED from vfs_api.cpp: `File::size()` on an OPEN written file stats the
   PATH — FatFS dir entry updates only on sync/close — so held arithmetic undercounts
   between flushes.** Fix: track durable bytes in RAM; trust size() only at (re)open
   and on closed files. (Applies to the raw server design too.)
2. `/fetch` can TRUNCATE the open batch file via a second FIL (name collision incl.
   the `download.epub` default) → add `chunkFinalize()` at top of handleFetch.
3. Telemetry floors/pieces init only at off==0 → resumed files log garbage floors;
   pieces double-count on 507 (increment precedes flush verdict).
4. Page JS: (a) give-up on one file ABANDONS the rest of the batch (fail() never
   advances); (b) permanent 4xx retried 24×; (c) fetch-less JS browsers get a dead
   page (unconditional preventDefault kills the native-form fallback); (d) SD-fail
   loop: acks between 507s reset tries so a dying card cycles forever (needs a
   no-progress stall counter); (e) zero-byte files skipped but counted as delivered.
5. Fixed already: stale s_chunkSawPiece verdict after an aborted POST.

**Traps (cost real time):**
- ⚠ **Flashing REQUIRES panicwatch STOPPED** — it eats esptool's sync replies; one
  attempt left the phone sitting in download mode. pkill panicwatch → pio upload →
  restart panicwatch.
- ⚠ A vanished client mid-POST on the LEGACY /upload path can wedge the main loop
  INDEFINITELY (flooded RX window keeps the FIN out; parser spins on connected()).
  Chunked pieces are smaller than the TCP window precisely so this cannot happen.
- ⚠ piece_*.bin probe clutter sits in /books (Books list won't show .bin; harmless).
- Book 2 was left TRUNCATED by the failed runs and was re-pulled + verified 3060655
  on 2026-08-21 morning. The corpus books live on COVEY /home/covey/books/.

**Acceptance bar unchanged** (docs/upload-redesign-brief.md): three consecutive
4-book batches over the fast LAN at full speed, zero breaker trips, internal-heap
floor never below 10 KB, then a REAL mobile browser. Then update README's upload
claims (add `heap` to its serial table too) + CHANGELOG.

## 🌙 2026-08-20 (evening) — the upload saga: fast networks were the killer all along

> **▶ NEXT SESSION STARTS HERE: `docs/upload-redesign-brief.md` — Nick approved the
> token spend for the chunked stop-and-wait redesign. The brief is the cold-start
> document: measured anatomy, agreed design, scouted traps, test corpus, acceptance bar.**

**The bug Nick reported as "books upload locked up + panic" unraveled into a 5-act play,
every act measured on hardware.** (1) The STA reconnect gate stranded WiFi during server
sessions — fixed (softAP-only gates). (2) lwIP starvation killed the listener then the
whole stack — circuit breaker added. (3) Breakers v1/v2 both died of resume-threshold
catch-22s (a paused-but-allocated server pins largest below ANY threshold) — v3 frees the
server outright on pause + timer-backed resume. (4) THE REAL ANATOMY: fast links (home
LAN 3 ms RTT; the crash-day phone-to-phone hotspot link) let TCP flood internal heap with
radio RX buffers faster than SD drains — heap measured 14 KB → 868 B in seconds; the slow
work-hotspot uplink had been accidental flow control all along. v4 = mid-upload
BACKPRESSURE (stop consuming ⇒ window closes ⇒ sender stalls) + breaker defers mid-file
(trips only < 3 KB during transfers). Push now SURVIVES but crawls on fast LANs.
(5) **THE ANSWER IS PULL**: /fetch ("paste a download link") lets the phone read at its
own pace — 5 MB books at FULL SPEED, heap intact. Serial `up on books` added so the bench
can start the Books uploader hands-free; delivery ran: Mac python http.server + POST
/fetch per book, straight into /books.

⚠ Bench follow-ups queued: per-request heap anatomy of the push path (why transients run
7-15 KB); breaker v5 lessons — a timer-resume into sub-threshold heap flaps in 3 s
slivers, and a pause landing right after handleFetch can eat the HTTP response after the
download already succeeded (harmless but reads as failure); WiPhone power button does NOT
reboot (screen-off only — a "restart" over serial is esptool --after hard_reset read_mac).
⚠ smsmirror.txt shows as a "book" in the Books list (config file in the .txt-books net) —
filter queued.

## ☀️ 2026-08-20 (day, Nick at work) — GPS half written, wiring sheet, rf_check's first run

- **🔧 THE WIFI AUTO-SWITCH DEADLOCK, found live and fixed (9ad7a36, FLASHED + PROVEN):**
  the switcher's call-guard blocked on CallState::HangUp — a teardown state the END key
  enters from ANY screen (ino:1857) that STICKS with no proxy (BYE can never send). Leave
  WiFi coverage + one END press = the network-restorer blocked by a state that needs the
  network to end. Measured: sip=6 for SIX HOURS, 7,549 EHOSTUNREACH errors, zero scans,
  the saved hotspot in range. Guard now blocks only the six audio-delicate states AND only
  while CONNECTED (no WiFi ⇒ no audio to glitch ⇒ never block). Proven on hardware: fresh
  boot scanned at 2 min, joined 'NickH-wifi' by itself; END presses leave it connected.
  ⚠ HEALTH's wifi= field is RAW WiFi.status() (1=NO_SSID_AVAIL, 3=CONNECTED,
  6=DISCONNECTED) — it was misread as a boolean once today; don't repeat that.
  ⚠ Follow-up question, deliberately not changed: should END enter HangUp from screens
  with no call? It plausibly backs call-reject; the guard fix removes its teeth here.
  ⚠ The phone now runs the post-0.9.7 build (GPS half dormant + Sun screen + GPS toggle
  + this fix) — TONIGHT'S BENCH FLASH IS ALREADY DONE; after assembly just flip
  My node > GPS receiver.

- **The woods backplate is GO**: parts in hand, bench checks tonight. The build sheet is
  `docs/woods-backplate-wiring.svg` — leg-by-leg, every pad named, W1–W19 run list, the
  before-first-power checklist (5V-accepts-charge test, TLV EN pull-up measure via
  R1-divider: R_pu = 4.7k×(VIN−V_EN)/V_EN, pass = V_EN < 0.4 V). RFM95W pads verified
  against the HopeRF DS Figure 2 itself. Docs reconciled: the buck feeds from the
  PowerBoost's 5.2 V, NOT VBAT (BOM superseded the .md; both now agree).
- **UI-first rule adopted (Nick):** features ship with a UI surface; serial = diagnostics.
  Applied same-day: **Meshtastic > Sun & legal light** (countdown-first screen) and
  **My node > GPS receiver** toggle (off / on (no fix yet) / on (fix), same NVS pref).
- **The GPS firmware half is IN and dormant** (commit fe96b66, NOT flashed — soak): nmea
  reader on the stock USER_SERIAL UART2 (GPIO 38/32 @ 9600 = the plate's exact wiring),
  fix < 2 min old slots into resolveReference between chosen waypoint and pin. After
  assembly: `gps on` once over serial, then `gps` for bytes/sentences/fix (bytes up +
  sentences 0 = wrong baud). **No auto-broadcast, on purpose** — announcing stays manual.
- **Morning incident closed**: dropping a pin crashed covey-ui AND the service stayed dead
  (xinit exits 0 → Restart=on-failure blind). Fixed + regression-proven + unit now
  Restart=always, crash-restart proven live. COVEY repo D-110. 'test pin' heard by the
  phone on air; **'vashon' still needs a re-save on COVEY.** `sun` proven live at the pin.
- **rf_check.py ran on hardware for the first time** (remote, covey-ui auto-restored):
  config 8/8 vs baseline (drift RETIRED), and it was counting the RAK's OWN packets as
  receptions — one printed DIRECT — now filtered (COVEY commit 03f9bc9). Zero external
  packets corroborated by the phone: the mesh was genuinely quiet. Side-by-side vs the
  spare RAK remains the September test.
- **Serial bridge hardened**: panicwatch now repo-tracked (`tools/panicwatch.py`) + types a
  wake newline first — first bytes after DFS idle arrive mangled (`pos` arrived as 40
  chars of junk). Plain `printf 'cmd\n' > /tmp/wiphone.cmd` is reliable now.
- Diagnostic truth-telling: `pos` no longer says "no waypoints heard" above a listed
  waypoint (says "none CHOSEN"); reference stays a deliberate choice (Places > pick).
- **📋 QUEUED for the GPS firmware release (Nick, 2026-08-20): publish the complete woods
  backplate build guide on GitHub** — the repo is PUBLIC (verified: private=false, Pages
  live), so the docs are already visible; what's owed is the FINISHED guide: Nick's
  measured values folded in (TLV EN pull-up, the 5V charge-in result, terminal count),
  the wiring sheet + BOM + firmware setup (`gps on` / My node toggle) linked from the
  README as a build-this page. **Timing: AFTER Nick meters and assembles** — measured
  numbers, not predictions, go in the public guide.

## 🌙 The overnight block (Nick asleep; everything below flashed + verified + pushed)

- **Waypoint DELETE parity was BROKEN and is fixed**: COVEY's delete is a Waypoint with
  an id and NO position (`waypoints.is_delete` — absence of position is the marker,
  because expire==0 means "never"); the phone's parser rejected exactly that form. Now:
  id-only packet ⇒ delete (owner-checked); local once-a-minute expiry sweep too. ⚠ v3's
  waypoint tail had NO size field in the DB header — DB is now **v4** (every struct size
  recorded; v1/v2/v3 migrate, v3's places re-learn on air). Nick must RE-SAVE 'vashon'.
- **`unread clear` survives reboots now** — Messages' part1/part2 caches were storing
  stale "u" flags back over the direct file writes (`dropPartitionCaches()` guards every
  such write; a kept log line separates failed-write from re-marked). ⚠ My first two
  diagnoses (cache theory applied too narrowly, then a LOG-READING error — awk matched
  the FIRST occurrence in a 38k-line log) were wrong before the fenced reboot test
  proved the fix; fence your greps.
- **Text overflow: 16 sweep findings + 7 review follow-ups, all fixed.** guiDrawEllipsized
  everywhere (UTF-8-safe, returns drawn width); header draws title LAST sized to what
  icons+clock leave; ⚠ MenuOptionIconnedTimed (chats list) has ITS OWN redraw — fixing
  the other two option classes does not touch it. sipThreadIdentity() is the ONE
  grouping rule (letters-only hash tail — hex would feed smsMirrorDigits and break
  idempotency); sipDisplayLabel() the ONE display rule.
- **`sun`** (serial): NOAA solar math in sun_times.{h,cpp}, 21 host checks incl. almanac
  anchors; verified on device (Seattle 08-20: 05:39/06:12/20:13/20:47). ⚠ Times are UTC
  minutes 0..1439 — evening events WRAP to the adjacent UTC date; unwrap for spans.
  Clock-face UI deliberately left for Nick's daylight taste.
- **COVEY: position broadcasts can ride a chosen channel** (Node & Location row, suite
  41, deployed, covey-ui active). Default UNCHANGED (still public LongFast) until Nick
  flips it — the row says "every radio can read this" so the flip is informed. The
  on/off half already existed (Auto-broadcast off/1m/5m/15m).
- COVEY map ruler (RUL) from earlier tonight: suite 40, deployed.

**▶ MORNING CHECKLIST FOR NICK:** (1) re-save the camp waypoint on COVEY (v4 migration
dropped it — expect it in Places in seconds); (2) flip Position channel to a private one
if you want (Settings > Node & Location); (3) `sun` works from serial now, and lights up
on the phone once a waypoint/pin exists; (4) the overnight soak monitor logged only my
own reflashes. Wishlist still open: Files icon/main-menu, clock-face legal light,
wallpapers, flasher publish after soak.

## 📍 0.9.7 (2026-08-19 evening): positions, places, and the unread-icon truth

**What it does:** the phone (no GPS) learns node positions (port 3) and waypoints
(port 8) off the air; Meshtastic > Places lists them; Nodes shows "1.4km NE" from a
chosen reference; "I'm here (tell the mesh)" pins the phone at a waypoint and announces
one Position (on a PRIVATE channel when one exists). Serial: `pos`, `unread`. DB v3
(v1/v2 migrate in place — verified live: "migrated 32 node(s) from v2", keys kept).
Crypto/geometry host-proven (test_pos, python-checked vectors).

**A 36-agent adversarial review ran BEFORE flash and confirmed 9 findings + resurrected
3 whose verifiers died on a network/limit outage (⚠ lesson: a finding whose verifier
never ran is NOT refuted — read the workflow failures list). All 12 fixed pre-release;
the CHANGELOG's review section lists them. Standouts: protobuf tags are varints (stock
precision_bits broke the one-byte reader), locked-waypoint ownership, node-slot
recycling leaked position+pubkey, serial help() truncated at 192B for releases.**

**Verified on hardware tonight:** boot clean, v2→v3 migration kept 32 nodes + PKC key,
`unread` found and named the white-icon cause (31 real unread from the re-mirrored
history, oldest in the 14257604281 thread — buried past the old 120-message read-marking
cap; thread-open now scans full depth while unread exist), live PKC key-learns from two
more nodes.

**⚠ AWAITING (Nick's move):** he'll add/re-save a waypoint on COVEY's map — expect
`MESH WAYPOINT: '<name>'` on serial, the place in Places, `pos` showing it. Then pick
reference / "I'm here" and see the phone on COVEY's map. COVEY's position broadcasts
need its GPS to have a fix before Nodes shows "COVEY 1.4km NE".

**🛑 PRIVACY FINDING FOR COVEY (not fixed, Nick to decide):** the RAK's PRIMARY channel
is the stock default LongFast (hash 0x08, well-known key — proven by ACK interop), and
covey-ui's `sendPosition` uses channel index 0 ⇒ **COVEY broadcasts Nick's GPS position
every 5 min readable by ANY Meshtastic radio in range.** Waypoints are safe (mapview
picks channels deliberately). Fix = covey-ui sends positions on a private channel
(+ the Settings toggle Nick asked about). The phone's own pin announce already prefers
a private channel.

## 🔐 0.9.6 (2026-08-19): Meshtastic PKC — DMs can work again. What was DONE and what is NOT YET PROVEN.

**Why:** Meshtastic 2.5+ requires public-key crypto for DMs BOTH WAYS — the RAK refuses
to send to a keyless phone (`PKI_SEND_FAIL_PUBLIC_KEY`, the 2026-08-19 finding), and —
new finding, from reading 2.7's Router.cpp — it also silently DROPS the legacy
channel-encrypted text DMs this phone was SENDING ("Rejecting legacy DM"). So 0.9.5 DMs
were dead in both directions and only the RAK→phone direction was known.

**The scheme** (every fact verified against meshtastic/firmware master source
2026-08-19, files in CryptoEngine.cpp / Router.cpp / mesh.proto — not from docs):
- session key = SHA-256(X25519(my priv, peer pub)); AES-256-CCM (L=2, M=8, no AAD).
- 13-byte nonce: packetId u32 LE | extraNonce | fromNode | 0x00. Frame:
  ciphertext | tag(8) | extraNonce(4). On-air channel-hash byte = 0x00.
- Keys travel in NodeInfo (User field 8), TOFU. **Stock NEVER overwrites a stored
  key** — hence the phone's keypair is PERMANENT (NVS `wpmesh/pkipriv`): if it is ever
  lost (chip erase!), every peer must delete/re-learn this node or DMs die SILENTLY.
- ACKs (Routing{error_reason} + request_id) are CHANNEL-encrypted on the primary even
  for PKI DMs — stock excludes ROUTING from PKC. The phone now ACKs DMs it accepts and
  re-ACKs retransmissions (dupes of ids in `recentAckIds`).

**Where the code lives:** `mesh_pki.{h,cpp}` (self-contained crypto; vendored
curve25519-donna + tiny-AES-c under `WiPhone/src/crypto/` — provenance headers in each
file), integration in meshtastic_service.{h,cpp}, serial commands in serial_cmd.cpp.
`tests/test_pki.cpp` + `tests/vectors_pki.h` (regen: `tools/gen_pki_vectors.py`, needs
`pip install cryptography`) prove byte-identical frames against Python both directions.

**RAM (Nick asked):** +1,688 B static measured (84,020 → 85,708), ZERO heap. The X25519
derive costs ~3 KB transient STACK, so it only ever runs at superloop depth: RX
key-learn warms a 2-entry session-key cache; a GUI-depth DM with a cold cache is QUEUED
one loop tick (`pendingDm`), never derived inline. On-hardware floor after boot (which
includes one derive): 3,020 B free (`pki` prints it live).

**Verified on hardware tonight:** boots clean, heap unchanged, node DB migrated v1→v2
(names kept, `MESH DB: 8 nodes`), keypair generated + persisted, announce carries the
key (`MESH ANNOUNCE SENT ... pki=in packet`), `pki` command works.

**🎉 PROVEN ON AIR, BOTH DIRECTIONS (2026-08-19 ~12:00, real hardware, no simulation):**
1. The RAK's key arrived with its periodic NodeInfo (~90 min after flash): phone shows
   `!62b8d2fd 'Nick H New Device' key HepOEI6R...`. ⚠ Lesson: the RAK does NOT answer
   `announce` want_response requests (stock suppresses replies to recently-heard nodes) —
   the ≤3 h periodic broadcast is the realistic key-exchange path after a fresh flash.
2. **Phone→RAK:** serial `dm !62b8d2fd <text>` was decrypted by the 2.7 RAK, displayed
   in COVEY's Messages, and persisted (verified in `/root/.covey/messages.json` over
   SSH — note covey-ui runs as ROOT, so its state is under /root/.covey, not ~covey).
3. **RAK→phone:** Nick saw it pop and replied "works!" FROM COVEY — the phone logged
   `MESH PKI DM from !62b8d2fd id=0x07b69a66 (6B) ACKed`, stored it in Chats, and its
   ACK closes the loop on COVEY's side. "Meshtastic 2.7 refuses ALL DMs" is CLOSED.
4. Follow-up flashed the same hour: phone-originated DMs now set want_ack, so the RAK's
   `MESH DM ACK ... err=0` line is per-message delivery proof for OUR sends too (the
   first test send got no ACK purely because want_ack was 0 — by design, now changed).

**Still open:** after soak, rebuild the webflasher (`tools/make_webflasher.sh` +
publish) so the page serves 0.9.6.

**Honest limitations, on purpose:** phone-originated DMs don't set want_ack (no
retransmit machinery — ACKs are logged, not shown in UI); DM to a keyless node still
goes legacy (works for pre-2.5 peers, dropped by 2.5+, log_e says so); a >220-char DM is
refused (PKI overhead + the 255 B LoRa frame — stock refuses the same length). A
mismatch-flagged key needs "Clear nodes" (Meshtastic app) or nothing changes.

## ✅ 0.9.5 (2026-08-19): the erase-recovery release
The web flasher is live and DATA-SAFE (esptool-js direct; erase only as an explicit
option — see docs/webflasher-plan.md for the incident that forced that). Files app,
number completion (sipCompleteAddress — sees through sip:/+ prefixes; needs the ACTIVE
account), serial toolbox (chan/chans/sip/bookpage), reader margin fix, booksync PSK
ROTATED (passcode 2222 both sides; the leaked key is dead), book sync re-verified
end-to-end by Nick, missed calls + phonebook confirm. 🔑 Field lesson worth keeping:
"texting dead + completion dead + mirror refusing" = ONE cause, no active SIP account —
the `sip` serial command prints it in one line.

## ✅ 2026-08-19 (day): reader fix proven, Files app, web flasher, courtesies

- **The e-reader margin bug is FIXED and confirmed by Nick** ("works well"). Mechanism:
  SmoothFont::textWidth bills the LAST glyph ink-only; the layouter measured one char at a
  time so EVERY char was "last" → lines packed 1-2 chars over budget → the draw clamped
  them left, beheading the first character. `fontMeasure` (app_books.cpp) now bills every
  glyph its advance via a sentinel space. ⚠ Any NEW measure must reuse it (exported in
  app_books.h) — a second naive measure reintroduces the bug.
- **Files app** (Menu > Tools > Files): browse/view/upload-anywhere + clipboard Copy/Move
  with mark-never-breaks-the-file semantics (rename-else-copy-verify-then-delete, refuse
  overwrite, partial dst removed on failure), delete behind Cancel-first confirm. The
  `bookpage` serial probe cornered the reader bug on its first use.
- **Web flasher**: `webflasher/` prototype (ESP Web Tools, the t-ui pattern) + staging
  script + `docs/webflasher-plan.md`. 🛑 NOT published yet — go-live steps in the plan.
  🔑 Update discovery: the PAGE is the check; the phone-side idea from Nick's notes cannot
  be a GitHub poll (TLS wall) and must NOT lean on COVEY (only Nick has one — his call,
  2026-08-19). Planned instead: an offline build-age nudge once the URL is live.
- **Phonebook Delete asks first** (confirm menu REBUILT each entry — a reused widget
  remembered its highlight and would have recreated the one-press erase; review catch).
- **Missed calls** show amber on the clock, observed purely inside setSipState (ring arms;
  Call/Accept/DECLINE disarm — declined-counts-as-missed was a review catch too); cleared
  by opening the dialer or phonebook.
- Still open on the wishlist: Files icon + main-menu promotion, COVEY wallpaper, the
  build-age nudge + Firmware-update screen text at flasher go-live.

## ✅ 0.9.4 — THE AUDIT RELEASE (2026-08-19). What changed and what to know.

A 38-agent read-only audit produced 30 verified findings (the full list with change specs
is committed at `docs/audit-2026-08-18-confirmed.json`); the high-value low-risk set is
implemented, adversarially re-reviewed (which caught a major defect in the first draft of
the WiFi backoff — see below), built, flashed, and boot-verified. The CHANGELOG's 0.9.4
section is the user-facing summary. Engineer's notes:

- **tinySIP hardening**: vsnprintf in Connection::print/printf; header-count cap. ⚠ A
  >100-header message is REJECTED WHOLE (the comment says so honestly now).
- **findMessage**: `IniFile*&` out-param — the old `IniFile&` "reseating" shallow-copied
  part2 over part1 (aliased Sections, latent double free). Three call sites bind
  `IniFile& ini = *part` inside the found-guard.
- **WiFi out-of-range backoff** (WiPhone.ino, reconnect block): 5×20 s then 180 s, radio
  quiesced 30 s after each eased attempt, instant retry on screen wake (10 s spacing).
  🔑 **The quiesce consults `lastWifiConnectAttemptMs()`** (stamped inside connectToWiFi,
  i.e. by EVERY join path) — the first draft disconnected joins the auto-switcher or the
  user had just started; the review caught it. Do not "simplify" that check away.
- **Mesh SPI poll** rate-limited to 10 ms (RX_DONE latches; packets are >100 ms on air).
- **Idle tick**: vTaskDelay(5) when the CPU-gate predicate says idle AND
  `smsMirrorPollBusy()` is false (a mid-transfer poll keeps the 1 ms tick).
- **QoL**: Reboot is a confirm submenu (menu[42] — the count must match the rows);
  redial via `controlState.lastDialed` (OK on empty dialer prefills, second OK calls);
  battery % on the clock (transparency-bracketed — opaque smooth-font cells paint black
  boxes over the wallpaper); `#` on the clock opens Messages when the envelope shows
  (`ENTER_MESSAGES_APP` 0x20); Reply returns to the THREAD via `returnState`.

## 🔎 OPEN: the e-reader "characters cut off at margins" report (photo, 2026-08-18)

Nick's photo shows page 16/90 of the BattleTech book with the first characters of two
continued-paragraph lines missing ("**c**alled", "his **h**ead"). **The layout engine is
exonerated**: a host probe ran the real epub_parse + book_layout over the real book — all
90 chapters, proportional measure — and every non-whitespace byte lands in exactly one
line (structurally, the only byte nextStart can skip is a space). The source bytes are
clean plain ASCII. So the loss happens ON DEVICE between layout and pixels — real font
metrics or the render path, where host tests cannot reach.

**▶ The instrument is already flashed: serial command `bookpage`** dumps the open reader
page — per line: text offset, byte length, measured px, first raw byte, rendered string.
**Next time the reader is open on a damaged page** (the position is saved, so opening the
book lands right there), run it via the watcher: `echo bookpage > /tmp/wiphone.cmd`, read
the serial log. If a line's `off` skips the missing char → device-side layout/measure; if
`off` is right but the rendered string lacks it → bookRenderRun/drawString. One paste
settles it. (Probe plumbing: `booksDebugDumpPage()` in app_books.cpp via a live-instance
pointer; prints a hint if no book is open.)

🎉 **ALL FOUR OPEN FAULTS ARE FIXED AND THE FIX IS ON THE PHONE.** The evening session
below closed everything the morning audit left open — read "WHAT 0.9.3 CHANGED" first.
The morning's fault write-ups are kept beneath it because their reasoning is still the
best map of these subsystems.

## ✅ WHAT 0.9.3 CHANGED (2026-08-18 evening) — all deployed, most verified on hardware

**Fault 1 (COVEY receives nothing) — fixed, and the real mechanism was NEITHER suspect.**
The 45 s timeout was deployed and the poller STILL saw nothing new. 🔑 **`getSMS` applies
a DEFAULT `limit` of 50 and keeps the OLDEST 50 of the date window** — the day the window
outgrew 50 messages, polls "succeeded" with the newest silently absent. Plus the server
stamps dates in the ACCOUNT's timezone (~3 h ahead), so an evening text is dated tomorrow
and a same-day `to=` missed it. Fixed in COVEY's `siptext.py` (`limit=9999`, `to`+1 day).
**Proven end to end: Nick's mobile → VoIP.ms → COVEY poller → LAN mirror → this phone's
store (`poll ok: 2 new`, `since=110050232`).**

**Fault 2 (texts land mid-thread) — fixed at both layers.**
- **A sentinel-repair pass in `Messages::load()`**: when the clock is known, partitions
  whose index `t2 == ffffffff` get their unknown-time messages stamped with distinct
  ascending times ending at the sync moment (arrival order preserved — `reorderLast`
  files equal keys in arrival order), then the partition re-sorts and t1/t2 recompute.
  ✅ **Fired on hardware first boot: "MSG: repaired 6 unknown-time message(s) in
  partition 2"** — six real messages un-pinned from "newest forever".
- **`msgCompare` never returns 0 now**: ties break on the VoIP.ms id when both carry one
  (which also puts COVEY's clamp-stamped catch-up bursts in true order), then on
  insertion sequence. `threadCompare` ties break on peer. Two openings of one thread can
  no longer disagree.
- `getAckTime()` reads `"a"` (was `"t"`).

**Fault 3 (no vibration on incoming text) — fixed structurally.**
`smsMirrorIngestLine()` latches news flags; the MAIN LOOP is the single announcer for
BOTH transports (`smsMirrorTakeNews()`): immediate buzz for an inbound arrival (Text
messages setting), NEW_MESSAGE_EVENT coalesced to ≥700 ms so a burst is a handful of
snapshot rebuilds, not one per record. The old notify inside meshService.loop() is gone
(it would double-buzz). The motor now fires BEFORE the pop (kills the I2C-disturbance
theory outright), and a latched-`ringing` suppression logs itself.
⚠ **The buzz is the one thing NOT yet verified on hardware** — it needs a genuinely
inbound text arriving via the mirror. Ask Nick to text the number and listen.

**Fault 4 (reboots opening/scrolling conversations) — every ranked cause closed.**
- 🔑 **NanoIni `Section`/`KeyValue` objects now allocate in PSRAM** (operator new, the
  AbstractWidget pattern; MessageData inherits it). The whole-partition parse — ~700
  internal blocks per file — now costs internal heap nothing. This was confirmed live
  first: the LAN mirror's catch-up burst aborted the phone mid-ingest
  (`NanoIni::parse → loadPartition → preload → ingestMirrored`, reset_reason=4, decoded
  by the watcher) minutes after COVEY's fix made the mirror flow again.
- **`preload()` break fix**: a cached part1 no longer drags part2 into RAM for a window
  that fits in one partition (the break sat inside the reload branch); part2's partition
  number is read off part2 (was part1).
- **`MultilineTextWidget::redraw()` allocates nothing** — the two unchecked per-redraw
  strdups (up to 13×/scroll keypress) are in-place char swaps now.
- **`setText()` checks `allocateMore()`** and truncates instead of writing 4 bytes past
  the row-pointer block on OOM; `appendText()` checks its malloc; thread rows and the
  row-pointer array live in PSRAM (`extStrndup`/`extRealloc`).
- **The LAN poller persists its `since=` high-water mark** to `/roms/smsmirror.since`,
  so a reboot no longer re-fetches and re-ingests the entire account history into the
  most fragile minutes of boot. Delete the file to force a full resync on purpose.

**Housekeeping:** ChoiceWidget's per-keypress `log_e` spam (incl. "MESUT") deleted;
`FIRMWARE_VERSION` 0.9.2 → 0.9.3; CHANGELOG released section written.

**The adversarial review round (same evening) — a 20-agent multi-lens review of the diff
confirmed 14 findings, and three were regressions in the fixes themselves. All fixed,
rebuilt, reflashed (`260e39b`). The ones worth remembering:**
- 🛑 **Persisting the since-id would have made refused records PERMANENT silent loss** —
  the mark advanced even when ingest returned -1 (no SIP account, unbuildable URI, db
  load failure), and the RAM-only mark's reboot-refetch was the thing quietly healing
  that. The mark now advances only past accepted records. **The general lesson: before
  persisting any high-water mark, ask what the old full-rescan was silently repairing.**
- The sentinel repair now lets **the index follow the file**: a failed partition store no
  longer clears the ffffffff flag that makes the repair run; a stale flag (power loss
  between the two stores) heals on the next load.
- `msgCompare`'s both-have-vids tiebreak was **intransitive** across mixed vid/no-vid
  equal-time blocks — UB for qsort, the exact nondeterminism it was written to remove.
  Now strictly lexicographic on (has-vid, vid, seq).
- The buzz latch is gated on the record being **recent** (10 min, clock known) so a
  full resync of a fresh store does not run the motor for the whole catch-up.
- The mirror poll's read deadline is a **stall** timeout (re-armed on progress, 120 s
  absolute cap): the first-ever catch-up processed 61 records slower than the old 8 s
  whole-response budget and was being declared "timed out" while healthy.

**Verification state:** clean boot, mesh radio + all 5 channels up, SIP registered,
sentinel repair fired, heap `largest` ~24 KB and recovering post-boot. Still to watch:
the buzz on a real inbound, thread order by eye, and a long soak (the honest window is
hours — see "do not characterise heap behaviour from anything under an hour").

---

## 🗂 THE MORNING AUDIT (kept for the reasoning; every fault below is FIXED as of 0.9.3)

---

## ☎️ THE PHONE IS A PHONE NOW (2026-08-17)

**Calls and texts work, both directions, on a real number.** Proven on hardware against Nick's
own mobile.

| | |
|---|---|
| **Number** | **425-320-0782** (Everett WA), VoIP.ms |
| **Sub-account** | `565611_nikguy` · POP **`seattle1.voip.ms`** · UDP-SIP · G.711U only |
| **Outbound call** | ✅ works, audio described as "ok" |
| **Outbound text** | ✅ arrives with the right caller ID |
| **Inbound call** | ✅ rings (and vibrates) |
| **Inbound text** | ✅ arrives |

🔑 **WHAT MADE INBOUND WORK — it was never the firmware.** The DID was still routed to
`[main account]` while the phone registers as the SUB-account, so inbound calls hit an account
with nothing registered on it (**busy signal**) and inbound SMS had nowhere to go (**silence**).
Fixed entirely in the portal: **DID Numbers → Manage DIDs → edit the DID → route SIP/IAX to
`565611_nikguy`, set POP to Seattle 1, enable SMS and point its delivery at the same
sub-account.** Outbound worked all along because it uses the sub-account's own registration.

⚠ **Phonebook entry that works for BOTH calling and texting:** put the FULL URI in the SIP URI
field — `14257604281@seattle1.voip.ms`. Calling auto-appends the server for a bare number
(`ControlState::setRemoteNameUri`), but **`TinySIP::sendMessage()` uses the address verbatim**,
so a bare number calls fine and silently fails to text.

⚠ **Read the POP hostname off the portal, never infer it.** "Seattle 1" is literally
`seattle1.voip.ms`, NOT the unnumbered `seattle.voip.ms`. That guess cost a correction.

⚠ **Do not add G.722** to the sub-account's codecs. VoIP.ms does not support it between
different POPs, and the firmware only implements PCMU / PCMA / G.722 anyway — G.729a would
connect and give you silence.

### Also shipped 2026-08-17

| | |
|---|---|
| **The 5-second freezes** | Blocking DNS on the main loop — see the section below. Fixed. |
| **Ringer mode** | **Settings > Audio** now has Ring + vibrate / **Vibrate only** / Silent, persisted and loaded at boot. Asked for after a very loud ring at work. |
| **Message icons** | Status bar now says WHICH kind of message waits: **white = SIP**, **green = Meshtastic**, overlapping pair = both. Collapses as you read each kind. ⚠ The white/green mapping was ASSUMED, not specified — swapping is two lines in `GUI::drawMessageIcon()`. |

---

## ▶ PICK UP HERE (2026-08-18) — FOUR OPEN FAULTS, AUDITED

Reported after a day of real use. **Nothing here has been reproduced on a bench.** Every claim
below was read out of the code and then attacked by a second reader; **7 of 12 candidate causes
were REFUTED and are not listed.** What follows survived that. Order matters: fault 1 gates the
others, and several fixes interact.

⚠ **Do the MEASUREMENTS before the fixes.** Each fault below names a check that takes under a
minute and needs no reflash. Four theories were wrong before the last backtrace arrived.

---

### 1. ✅ COVEY RECEIVES NOTHING — CONFIRMED and the fix is STAGED (2026-08-18)
Nick ran **Test Connection from the device**: *"VoIP.ms did not answer in time"* — the read
path timing out at its 12 s default while sends succeed at 45 s. Fix is committed on COVEY's
`main` (read timeout 45 s, an always-visible poll-status strip, a `busy` watchdog), **not yet
deployed** — deploy steps and verify order are in COVEY's `notes/HANDOFF.md`.
⚠ **When it deploys, the mirror starts flowing again — expect fault 4 to fire harder under
the inbound burst, and expect ordering strangeness from the catch-up poll (the `_parse_ts`
clamp stamps old messages with the fetch moment). Neither is new; do not re-diagnose.**

---

### 2. 🟠 A NEW TEXT LANDS MID-THREAD — cause found, and it is NOT what it looks like

🔑 **`saveMessage` stores `0xFFFFFFFF` as the "unknown time" sentinel** (`Storage.cpp:710-712`,
`if (!time) { time--; }`). That is the **MAXIMUM uint32**, so a message with unknown time sorts
as **the newest message in the thread forever**, and every text that arrives afterwards is
placed *above* it — i.e. in the middle. The sentinel was chosen so partition sorting stayed
correct; it is actively wrong as a display order.

⚠ **The phone stores unknown time far more often than you would think.** `everUpdated` is a RAM
flag, so **time is unknown after EVERY reboot until NTP answers** — and stays unknown for a
whole session on any network where NTP is blocked. **Fault 4 manufactures those windows**, and
VoIP.ms pushes queued SMS immediately after REGISTER, i.e. precisely into them.

**MEASUREMENT — free, no reflash, no serial:**
- **Status bar has no `HH:MM`** → the clock is unknown → everything stored this session is
  getting the sentinel. Confirmed on the spot.
- **In the thread, a message drawn with only a name and NO ` · <time>`** → its stored time is in
  the future, i.e. it is a sentinel. `Clock::dateTimeAgo` writes an empty string for any
  future timestamp (`clock.cpp:373-375`), and the header omits the separator when `ago` is
  empty — so the bug hides the very value that identifies it.

**Also true, and worth fixing in the same pass:**
- **`msgCompare` returns 0 for equal timestamps and `qsort` is not stable** — so any block of
  messages sharing a timestamp (the common case) comes out in **arbitrary order that can differ
  between two openings of the same thread**. Needs a deterministic tiebreaker.
- The material for one is already stored and free: `saveMessage` writes the VoIP.ms id under
  `"v"`, but `SipThreadMsg` does not carry it and `addMsg` never reads it.
- 🛑 **Do NOT switch to an id-only sort.** A text received over SIP that COVEY has not yet
  mirrored has **no id at all**, as does every locally-composed send until adoption — an
  id-only key dumps all of them to one end. Time-with-id-tiebreak, or fix the sentinel.
- ⚠ **`buildThread`'s character budget walks backward BY SORT ORDER**, so once the order is
  wrong the genuinely newest text can be pushed behind "*… N older messages not shown*" and
  **vanish from the screen entirely.** Expect that as a second symptom; it is a consequence,
  not a separate bug.

---

### 3. 🟠 NO VIBRATION ON AN INCOMING TEXT — a whole transport is silent

🔑 **The LAN (HTTP) mirror stores an incoming text and announces NOTHING** — no vibration, no
pop, and no `NEW_MESSAGE_EVENT`. It is the only delivery path in the firmware with no
notification at all. The LoRa mirror got one (`meshtastic_service.cpp:682-684`); the LAN poller
never did.

🔑 **And it poisons the other path:** ingest dedups by VoIP.ms id, so **whichever transport
lands a text first wins** — and because LAN is the silent one, a LAN-first delivery
**permanently suppresses the LoRa copy's notification.** That fits the report exactly.

**Fix:** in `sms_mirror_poll.cpp::ingestLine()`, take `bool inbound` from
`smsMirrorIngestLine(line, &inbound)` and call `smsMirrorNotifyArrival()` when `r > 0 &&
inbound`, mirroring the mesh path. Raise `NEW_MESSAGE_EVENT` there too.

⚠ **Confirm from the log you already have before changing anything** — both lines are `log_e`:
`SMSMIRROR poll ok: N new of M` with **N > 0** and **no `NOTIFY: buzz` in the same second** is
the proof.

⚠ **The motor block is gated on `!gui.state.ringing`, and the one log line that would prove it
ran is INSIDE that gate.** Any path leaving `ringing` latched kills every later notification
with no trace. Add an `else` that logs `NOTIFY: suppressed (ringing)`.

⚠ **`NOTIFY: buzz` proves the DECISION, never the motor.** The write goes through the I2C
extender and cannot report failure. **A/B that settles it:** set Text messages to *Vibrate
only* and send one. Buzzes there but not in *Sound + vibrate* → the pop's I2C traffic is
disturbing the extender; fix by driving the motor **before** `playPop`.

---

### 4. 🟠 REBOOTS WHILE SCROLLING / OPENING CONVERSATIONS — my paging fix missed the bulk

🛑 **`SCAN_PAGE = 10` bounds only the `preloaded` DEEP COPIES. The partition FILES those copies
are read out of are still loaded WHOLE**, and every INI section and key-value in them is a
separate `operator new` from the **internal** heap. At `PARTITION_SIZE = 100` that is roughly
**700 live internal blocks per partition**. Paging bounded the wrong thing.

Ranked, all read-and-certain unless noted:

1. **Whole-partition parse** (above). Measure first — the existing `appHeapProbe()` already
   spans it: it runs at the end of `enterApp()`, and `MessagesApp`'s constructor does
   `buildChats()` → `sipThreadsBuild()` → `preload()` → `loadPartition()`. **Reboot, open
   Messages, read `largest` either side in `health.log`.** No code change needed to measure.
2. **`preload()` parses a SECOND partition it never reads**, on every page after the first: the
   `break` for "only one partition needed" sits *inside* the branch that loads part1, so a
   cached part1 falls through and loads part2 unconditionally. **Doubles item 1 for every page.**
   (`Storage.cpp:674` also reads the partition number off the wrong object while you are there.)
3. 🔑 **`MultilineTextWidget::redraw()` calls `strdup()` twice unchecked and dereferences the
   result on the next line** (`GUI.cpp:11416`, `:11430`). In the thread view nearly every row
   ends in `\n`, so this runs **up to 13 times per redraw — on every scroll keypress.** This is
   **the only finding whose trigger is literally "while scrolling"**, which is what Nick
   reported. Guard both, or drop the allocations (one wants to omit a trailing character, the
   other wants a prefix width — a length-limited `textWidth` does both).
4. **`setText()` ignores `allocateMore()`'s return value** and writes `rowsDyn[cursRow]`
   regardless; when the pointer realloc fails, `cursRow == maxRows` and the store goes **4 bytes
   past the end of the block.** An OOM becomes **silent heap corruption** — which is exactly why
   such a reboot would produce no decodable backtrace.
5. *(inference)* The per-row `strndup` load: ~90–115 small internal allocations per thread open
   plus a 10→20→40→80→160 realloc ladder. ⚠ **If item 1 shows the partitions are the bulk, LEAVE
   `THREAD_TEXT_BUDGET` ALONE** — it was already cut once on a wrong diagnosis and restored, and
   cutting it again would repeat that mistake. The right fix is a **PSRAM row allocator**.
6. *(inference)* An inbound burst multiplies all of the above: each `NEW_MESSAGE_EVENT` rebuilds
   both the chats menu and the thread widget, each rebuild re-runs four 120-deep scans. **Fixing
   fault 1 will make fault 4 fire harder.** Coalesce with a dirty flag.

**Ruled out**, so nobody spends a fix there: `getText()`'s large malloc and `appendText()`'s
unchecked malloc are real defects but unreachable from opening or scrolling a conversation.

---

### Found in passing — small, unrelated, worth doing
- 🔊 **`ChoiceWidget` logs at `log_e` on every left/right keypress** (`GUI.cpp:10776/10778/
  10788/10794`, including one that just prints `MESUT`). `log_e` is the only level compiled in,
  so leftover debug spam is flooding the field log. **Delete.**
- **`MessageData::getAckTime()` reads `"t"` instead of `"a"`** — returns the message time, not
  the ack time. Dormant (nothing calls it), one character to fix, a live trap for whoever adds
  delivery receipts.
- **Neither the LAN poll nor the LoRa mirror raises `NEW_MESSAGE_EVENT`**, so a mirrored text
  arriving while a thread is open is invisible until you back out and re-enter — and then it
  appears at whatever position the sort key gives it, which **reads as "it appeared in the
  middle"**. This muddies fault 2's report; fix it with fault 3.

### The tool for 3 and 4
```bash
python3 tools/panicwatch.py       # logs every byte, decodes any Backtrace via addr2line
```
(now repo-tracked; the `/tmp/panicwatch.py` copy this doc used to point at survives no reboot)
⚠ **It owns the serial port. Do not open a second monitor** — that killed the previous version
twice, taking the dump with it. Send console commands by writing a line to `/tmp/wiphone.cmd`
(`up on`, `up off`, `sync`, `mirror`).
⚠ **The FIRST bytes of a write after idle get mangled** — with the phone idling (DFS, 80 MHz)
`pos` once arrived as 40 chars of junk while a command 3 s later was clean (2026-08-20). The
watcher now types a sacrificial newline + 250 ms pause before every command, so `printf
'cmd\n' > /tmp/wiphone.cmd` is reliable as-is (the old leading-`\n` trick in the command file
never survived the watcher's `strip()`).

---

## 🛑 THE CONVERSATION-VIEW ABORT — FOUND, FIXED, AND WORTH READING FIRST

**Opening a conversation rebooted the phone.** Four rounds of theorising got it wrong; the
decoded backtrace got it in two minutes. **Read the crash before theorising about the crash**
— this doc already said that, and it was right again.

```
operator new -> __cxa_allocate_exception -> std::terminate -> abort   (reset_reason=4)
  NanoIni::Section::deepCopy / addKeyValue
  MessageData::MessageData
  Messages::preload
  <caller>
```

🔑 **`Messages::preload(dir, offset, count)` DEEP-COPIES each message's INI section into
INTERNAL RAM.** Asking for a large window means that many live section copies, and the
allocation behind them is `operator new`, which **THROWS** on failure. Nothing catches it, so
it becomes `terminate()` → `abort()`. **Same bug class as the `WiFiUDP::parsePacket` abort
written up further down this file.**

⚠ **THE WHOLE-WINDOW FORM IS A TRAP: it works on a small store and aborts on a big one**, so
it survives every test and dies in use. That is why it presented as "one specific conversation
crashes" — it depends on how much is stored in that DIRECTION, not on the thread.

✅ **All three callers are paged now** — 10 sections live at a time with `clearPreloaded()`
between pages, same depth covered:
`sip_threads.cpp` `scanDirection()` + `collect()`, and `GUI.cpp` `MessagesApp::markThreadRead()`.
**If you add a fourth `preload()` caller, PAGE IT.** `grep -n "\.preload(" WiPhone/*.cpp`.

⚠ **The cost is a visible pause opening a conversation** — 12 passes instead of 1, three times
over (chats list, thread, mark-read). `SCAN_PAGE` could rise, but **measure `largest` at a few
page sizes before picking a number**; 120 aborted and 10 is safe, and nobody has found the
edge between them.

⚠ **A wrong diagnosis shipped on the way** and has been reverted: `THREAD_TEXT_BUDGET` was cut
1800 → 600 blaming `MultilineTextWidget`'s per-row internal allocations. `largest` really was
collapsing, but the cause was the deep copies above. **It is back at 1800.** If thread
rendering ever does look like the culprit, the app-open probe in `GUI::enterApp()` prints
`largest` either side of opening the screen — measure, do not assume.

🔑 **THE TOOL THAT ENDED IT.** A serial watcher that logs every byte and runs `addr2line`
against `firmware.elf` the instant `Backtrace:` appears. ⚠ **The first version DIED whenever
anything else opened the port — and took the dump with it, twice.** It reconnects on any error
now, and commands go through a file (`/tmp/wiphone.cmd`) so nothing ever opens a second handle.

---

## 📨 SMS MIRRORING FROM COVEY (2026-08-17) — COVEY'S HALF IS LIVE, THE PHONE'S IS HALF-BUILT

**The plan in item 0 below could not be built as written, and the reason is worth reading
before anything else here.**

### 🛑 THE PHONE CANNOT TALK TO VOIP.MS. NOT A URL PROBLEM — THE OTA MEMORY WALL AGAIN.

"Let the WiPhone poll the same `getSMS` and merge" is impossible on this build. Re-verified
2026-08-17 from the framework's own headers, not from memory:

| | |
|---|---|
| `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN` | **16384** — a ~16 KB IN buffer **and** a ~16 KB OUT buffer |
| `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` | **not set** — so none of it may come from PSRAM |
| free internal heap, whole phone | **~31 KB**, largest block **~20.5 KB** on the plateau |

~33 KB of internal heap does not exist here. ⚠ **And the obvious escape hatch is not
available either: mbedtls is 2.16.7**, which predates `MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH`,
so RFC 6066 max-fragment negotiation would **not** shrink those allocations — in 2.16 the
buffers are sized from `MBEDTLS_SSL_MAX_CONTENT_LEN` regardless of what gets negotiated.
Nor is there a cleartext way round it: **`http://voip.ms/api/v1/rest.php` answers 301 to
https** (measured). **Do not re-plan around the phone reaching VoIP.ms directly.**

### ✅ SO COVEY RELAYS — and that turns out to be the better arrangement
`getSMS` returns the **account's** history, so COVEY already holds every text this phone
sent *and* received. Syncing the phone **from COVEY** therefore does more than close the
original one-way gap: it **back-fills texts that arrived while the phone was off**.

**One record, two transports.** `CSM1 <id> <peer> <dir> <ts> <text...>` — see
[`sms_mirror.h`](../WiPhone/sms_mirror.h) for the full reasoning. `id` is the VoIP.ms message
id and is THE dedup key. `text` is last so it holds spaces with no quoting. Worst realistic
case is **198 bytes against MESH_TEXT_LEN 234**, and a single SMS can never exceed 160
characters, so **one record is always one packet — no chunking, ever.**
⚠ **The LAN transport serves these same lines**, not JSON, deliberately: JSON would mean a
JSON parser on a phone with 20 KB of contiguous heap, and two parsers to keep in step.
⚠ **The text is escaped** (`\\` `\n` `\r`) because one raw newline in a last-position field
splits a record in half on a line-oriented transport and takes the next record down with it.

### What is DONE and PROVEN
- **[`sms_mirror.cpp`](../WiPhone/sms_mirror.cpp) + 185 host assertions**, including
  **interop vectors generated by running COVEY's own `smsmirror.py`**
  (`tools/gen_smsmirror_vectors.py`) — the booksync precedent, so a format drift cannot go
  silent. **Tests are now 1,026 across nine suites.**
- **COVEY's LAN endpoint is LIVE and tested against the real store** — 19 real records
  served, `403` with no token and with a wrong token, `404` on a wrong path, `since=` filter
  correct, a garbage `since` does not 500.
- **The mesh channel exists.** A **new** channel `smsmirror` at **index 4** with a fresh
  random 256-bit PSK. Booksync at index 3 verified **byte-identical afterwards**. Radio
  config backed up to `firmware/meshtastic/backups/` in the COVEY repo.
- **The phone's RX hook and ingest are written and compile** (+8 bytes static RAM).

### 🔑 WHY A NEW CHANNEL AND NOT `booksync` — this is a security finding, not a preference
🛑 **The `booksync` channel's PSK is published on the public internet.**
this doc committed the booksync channel's PSK **and** its passcode in plaintext, in
`Nikguy321/wiphone-meshtastic`, **which is public** (confirmed: an unauthenticated GitHub
API request returns 200). ✅ **Both are redacted as of 2026-08-17** — but they are still in
the git HISTORY, so the old key must be treated as compromised until it is rotated. Anyone who reads the repo can decrypt that
channel. Tolerable for reading positions; **not** somewhere to put the content of texts.
**That PSK and passcode should be rotated and scrubbed regardless of this feature.**

The new channel also cost no firmware work — the mesh layer is already generic
(`importChannelUrl()`, `sendChannelMessage(hash, text)`, 8 slots).

### ▶ WHAT IS LEFT, in the order that gets it working
1. **IMPORT THE `smsmirror` CHANNEL ON THE PHONE.** Nothing arrives until this is done.
   🛑 **THE INVITE URL IS NOT WRITTEN DOWN HERE, AND MUST NOT BE.** A Meshtastic invite
   carries the channel PSK inside its base64 fragment, so pasting one into this file would
   publish the key — which is precisely the mistake that makes the `booksync` channel
   worthless (see above). **It travels device to device instead**, and COVEY gained the
   button for it on 2026-08-17:

   | on | do |
   |---|---|
   | **COVEY** | open the **`smsmirror`** chat → the **dots** → **Share channel link** → tap this phone in the node list. It sends the invite as a **DM** (never a broadcast — the link *is* the key). |
   | **this phone** | **Menu > Meshtastic**, open that message, press **OK** — `app_meshtastic.cpp` `MESH_VIEWMSG` calls `applyChannelUrl()`. |

   ✅ **The pass is the phone answering `Applied: 1 channel(s) added.`** That message is the
   only confirmation you get, and it is worth waiting for: it distinguishes an invite that
   was **APPLIED TO THE RADIO** from one merely sitting in a chat thread. That exact
   distinction is what made book sync look broken for a whole session.
2. **The LAN poller does not exist yet**, and ⚠ **it must NOT be a blocking `HTTPClient`
   GET.** The UI is one task; a 2.5 s call every 60 s is the 5-second-freeze bug rebuilt on
   purpose. It needs a **state machine over `WiFiClient` driven from the main loop** — a
   FreeRTOS task is the other option but its stack cannot live in PSRAM, and internal heap
   is the scarce thing. The ingest entry point it needs already exists and is transport-
   neutral: `smsMirrorIngestLine()` in [`sms_mirror_rx.h`](../WiPhone/sms_mirror_rx.h).
   COVEY's token is in its `prefs.json` under `smsmirror_token`; the endpoint is
   `http://<covey>:8087/sms?since=<id>` with header `X-Covey-Token`.
3. **NOTHING HAS CROSSED BETWEEN THE TWO DEVICES YET.** The wire format is proven against
   COVEY's real encoder on the Mac, and COVEY's server is proven against real data, but no
   mirrored text has reached the phone by either path. **Treat the whole ingest path as
   unproven until one does.**

⚠ **THE LAN PATH IS CLEARTEXT** — it must be, the phone has no TLS. The token stops a port
scanner, not somebody sniffing the wifi you are both on. `smsmirror_lan` in COVEY's prefs
turns it off; the LoRa path *is* encrypted, and the two dedup by id so nothing is lost by
running only one. `cryptography` 43 is on the Pi and the phone has AES-CTR in `mesh_crypto`,
so encrypting the LAN body under the channel key is a real option if it is ever wanted.

⚠ **`ingestMirrored()`'s scan is BOUNDED** to the newest 60 messages in the record's own
direction (an unbounded scan means loading every partition). If a mirrored text pairs with a
copy older than that window it is **stored twice**. That needs a burst of >60 messages
between COVEY sending and the phone hearing — outside normal use, but real.

---

## 🧊 THE 5-SECOND UI FREEZES WERE BLOCKING DNS

**A freeze that recovers is not a crash** — a crash reboots. This was the main loop blocked, and
since the whole UI is one task, blocked means frozen. `resolveDomain()` in `Networks.cpp` is
reached from SIP connect, from **RTP setup during a call**, and from NTP, and it had two blocking
calls in series:

1. **A 500 ms mDNS query run for EVERY name**, including public ones. mDNS only answers for
   `.local`, so that half second was guaranteed waste. Every `X not found on local network` line
   in an old log IS that wasted half second.
2. **`lwip_gethostbyname()`**, which retries internally and blocks for SECONDS when DNS is slow
   or unreachable — and **NTP retries every 500 ms** (`TIME_UPDATE_RETRY_DELAY_MS`). Measured:
   `pool.ntp.org` failed to resolve **seven times in two minutes** on a work WiFi.

Fixed by asking mDNS only about names it could answer for, and caching results — **including
failures**, which is the half that matters, since a negative entry makes an unresolvable name
free for `NEG_TTL_MS` no matter how eagerly the caller retries. Measured after: **7 failures per
2 min → 1 per 3 min**, zero wasted mDNS lookups. ⚠ `.local` answers are deliberately NOT cached
(the `wiphone.local` staleness trap). Cleared while there: the WiFi auto-switch scan is already
async and was innocent.

---

## 🛑 THE RESTART MYSTERY IS SOLVED. READ THIS BEFORE ANYTHING ELSE.

**`WiFiUDP::parsePacket()` was both the abort AND the fragmentation.** One function, two
separate failure modes, and between them they account for everything chased for weeks.

```cpp
char * buf = new char[1460];                    // EVERY call, before it knows if data exists
if(!buf){ return 0; }                           // DEAD CODE: new[] throws, never returns null
if ((len = recvfrom(..., MSG_DONTWAIT, ...)) == -1) { delete[] buf; return 0; }
```

**Fault 1 — the abort.** `new[]` throws `std::bad_alloc` on failure. Nothing catches it, so it
becomes `std::terminate()` → `abort()` → **`reset_reason=4`**. The framework's own null-check
never fires because it was written with `malloc` semantics. Proven by decoding the panic
backtrace against `firmware.elf`:

```
loop -> TinySIP::checkCall -> UDP_SIPConnection::available -> WiFiUDP::parsePacket
     -> operator new[] -> __cxa_throw -> std::terminate -> abort
```

**Fault 2 — the ratchet.** It allocates 1,460 bytes *before* checking whether a packet is
waiting — the overwhelmingly common case — then frees it. `TinySIP::checkCall()` drove that
from the main loop **thousands of times a second**. An allocator running flat out, doing
nothing, chopping the heap to pieces. This is why `largest` slid all day and never recovered
while FREE heap sat perfectly still.

**Both fixed.** `udpParsePacketSafe()` in `helpers.h` fronts all six call sites (guard + catch);
`UDP_SIPConnection::available()` polls at **50 ms**. Measured either side, idle, screen off:

| | before | after |
|---|---|---|
| shape | **continuous** ratchet, step after step | two early steps, then a **plateau** |
| 2 min | 22,492 and still falling | 27,836 |
| 28 min | **3,168**, then panic | 20,528 |
| free heap | 31,056 (never moved) | ~31,000 (never moves) |
| `reset_reason=4` | **every 1–3 min** | **none** |

### ✅ PROVEN OVERNIGHT, 2026-08-16→17 — 5.4 HOURS, ZERO PANICS

Idle, screen off, SIP registered, on USB power. `largest` sampled from the health log:

```
up=  0min  27,988
up= 24min  20,528
up= 78min  20,528
up=160min  20,528
up=241min  20,528
up=295min  20,528     <- four and a half hours without moving
up=322min  20,084
```

**It PLATEAUS.** Two steps in the first half hour, then it settles and stays. This was the open
question — whether the remaining drift was a slow slide toward death — and the answer is no.
**1,330 heap dips over the night, `reset_reason=4` count: ZERO.**

The dip histogram is the mechanism, confirmed: **991 of the 1,330 drops are exactly −1,712** —
the `parsePacket` buffer taken and handed straight back, every one recovering in full. That is
the same churn as before, now happening at 20 Hz instead of ~20 kHz.

⚠ **Two corrections this section has already needed, kept as a warning against short windows.**
It once claimed "the ratchet is gone" (from a 4-minute sample) and later "one discrete step,
then flat" (from 11 minutes). The truth needed six hours: **two** early steps, then a genuine
plateau. **Do not characterise heap behaviour on this phone from anything under an hour.**

🔎 **STILL OPEN, LOW PRIORITY — the two early steps.** ~5 KB then ~2 KB, both inside the first
half hour, never recurring. Free heap barely moves across them, so they still look like
fragmentation rather than plain allocation. Not urgent: the plateau at ~20,500 sits against an
abort threshold of 1,460–3,168. The watchpoint prints app / sip / wifi / screen / uptime at the
instant of each drop, so whoever picks this up starts with the context already logged.

⚠ **NOT claimed:** that any of this explains restarts recorded *before* SIP existed. Nothing else
polls UDP continuously — `USE_VIRTUAL_KEYBOARD` is commented out, NTP polls only inside a
request window — so those had a different cause and remain unexplained.

### 🔑 THE TOOL THAT FOUND IT — use it before theorising
A **ratchet watchpoint** at the top of `loop()` in `WiPhone.ino` samples `largest` every 250 ms
and logs ONLY real drops, with context:

```
DROP largest 27884->22492 (-5392) app=16388 sip=1 wifi=3 scr=0 cpu=80MHz up=109s
```

It found the cause on its first run. The 15 s HEALTH line was far too coarse — it says memory
vanished sometime in the last quarter minute, never during what. **Silent on an idle phone.**
Costs a free-list walk 4×/second; delete it or raise `DROP_THRESH` once this is well trusted.

### 💡 THE LESSON, recorded because it cost hours
**The ESP32 prints a backtrace on panic and I spent half a day on heap arithmetic before
reading it.** `addr2line` against `.pio/build/wiphone/firmware.elf` named the exact function in
two minutes. **Read the crash before theorising about the crash.**

```bash
xtensa-esp32-elf-addr2line -pfiaC -e .pio/build/wiphone/firmware.elf 0x... 0x...
```

---

## ▶ WHERE TO PICK UP (2026-08-16)

**Nick's stated priorities, in his words: STABILITY and BATTERY LIFE on the WiPhone.** The
audio crackle is explicitly **dropped** — *"Covey does music, so worst case it's just not great
on the wiphone and I live with it."* Do not spend time on it.

### 🧠 THE MEMORY PICTURE — where the internal heap actually went

Free internal heap went from **~9,964 to ~31,300** in one day, and `largest` from ~8,136 to
~27,700. Three finds, all located with `xtensa-esp32-elf-nm --size-sort -S firmware.elf`
(addresses starting `3ffc…` are internal DRAM; `3f4…` is flash and irrelevant):

| what | recovered | how |
|---|---|---|
| **A phantom second `Audio` object** | **12,616 B** | `DiagnosticsApp::processEvent()` held a `static Audio` — 12,604 B of BSS — as a workaround for `DIAGNOSTICS_ONLY` builds. `config.h` defines `WIPHONE_PRODUCTION`, so that mode is **off** and the branch can never run, but `static` reserves the memory unconditionally. Now `#ifdef`-guarded. It also reassigned the **global** `audio` pointer, so opening Diagnostics in production would have swapped the phone's audio device. |
| **All widgets → PSRAM** | ~4,200 B per screen | `operator new` on `AbstractWidget`, the root of the hierarchy, covers all ~163 widget allocations in one place. Settings > SIP accounts went **−4,236 → +0** per open. ⚠ Moving just the app OBJECT (the BooksApp trick) recovered only 460 of those bytes — the object was never the cost, its dozen widgets were. |
| **The UDP poll churn** | the ratchet itself | see the section above |

**What is left in internal DRAM, and why it stays:** `setup()::audio_local` (12,604 B — the real
one), the global `sip` (5,364 B), WiFi's `g_cnxMgr`, `meshService`. ⚠ **The `Audio` object should
NOT be moved to PSRAM** — it is I2S/DMA-adjacent and timing-critical.

⚠ **The Game Boy owns 16 KB of internal RAM that CANNOT be moved:** an 8,192-byte blit task
stack, a 4,096-byte emulator stack, and a 4,096-byte audio buffer. **FreeRTOS stacks physically
cannot run from PSRAM on the ESP32.** The ROM and both framebuffers are already correctly in
PSRAM. This is why **`sipMayPoll()` stops SIP polling while the emulator is on screen** — they
compete for memory neither can give up. A live call outranks the emulator.

### ⚠ A DEFECT LEFT DELIBERATELY UNFIXED
`TFT_eSprite::esp32Calloc()` tries **`calloc()` first and PSRAM only as a fallback** — backwards
on a device with 3.6 MB of PSRAM and ~28 KB of internal heap. It is real, and it is **not**
fixed, because a 14-minute soak with the screen forced permanently awake (`SCREEN_ALWAYS_ON_TEST`
in `config.h`, left in place but off) showed **screen-on alone does not ratchet the heap**:
`largest` drifted −1,792 and then sat flat. The sprite sites are the Ackman game and the design
demo, neither of which gets used. Flipping the priority would slow every sprite write and cost
the emulator frame rate to fix something that is not happening. **If evidence ever points back
here, use a size threshold — big allocations to PSRAM, small hot ones internal — not a blanket
flip.**

### ✅ Shipped 2026-08-16 — typing, honest labels, and OTA switched off at source

**Nothing here is queued work; it is all done, flashed and pushed.** Listed because three of
the four are behaviour changes you will notice in the hand.

| | |
|---|---|
| **Typing** | **The D-pad centre now commits the highlighted multi-tap letter and is consumed doing it** ([`GUI.cpp`](../WiPhone/GUI.cpp), `alphanumericInputEvent`). `AAA` is tap-OK-tap-OK instead of two 2 s waits. The timeout is UNCHANGED — OK is an extra way to commit, not a replacement. Only `WIPHONE_KEY_OK` is taken, so **CALL and the top-left Send still commit AND send in one press**. ⚠ On mesh compose, sending after typing is now two OK presses (commit, then send) or one CALL. |
| **Labels** | Four screens said **"Cancel"** on a key that is **backspace** (mesh compose, edit name, short name, book sync editor). Now **"Clear"**, matching what GUI.cpp already used everywhere. Cancel is **END**, the key below Back. |
| **OTA screen** | **Settings > Firmware update** is now a scrollable USB how-to, not a form. Up/down = line, left/right = page, Back exits. Text is string literals drawn from flash: **zero heap**, deliberately — `MultilineTextWidget` would `strdup` a row per line on every open. **896 B less static RAM, 2.5 KB less flash** than the form it replaced. |
| **OTA at boot** | **The every-boot TLS attempt is gone.** Gated on **`OTA_TRANSPORT_AVAILABLE` (0) in `WiPhone/ota.h`** — flip to 1 only when the TRANSPORT changes. `start_ssl_client: -1` no longer appears in the boot log. |

⚠ **Two traps worth keeping, found while doing this:**
- **`ota.updateExists()` is the NETWORK call** and it sat to the **LEFT** of the cheap local
  predicates, so `&&` short-circuiting never protected it. Fixed underneath the gate for
  whenever OTA is real again.
- **`autoUpdateEnabled()` DEFAULTS TO TRUE** — it returns false only if the ini literally says
  `"no"`. So "just turn auto-update off" was never a fix, and is now impossible anyway because
  the screen that set it is gone. That is why the gate is a compile-time switch, not a setting.

📊 **Measured either side of the OTA-at-boot change:** fresh-boot `largest` went **14,388 →
15,860**. ⚠ **One sample each and NOT under identical conditions** (the second was screen-on at
240 MHz, which normally makes `largest` worse) — the direction is trustworthy, the exact number
is not.

### ▶ THE ACTUAL OPEN ITEMS

0. 🔄 **SUPERSEDED 2026-08-17 — see "SMS MIRRORING FROM COVEY" near the top of this doc.**
   The plan below ("the fix is for the WiPhone to poll the same `getSMS` and merge") **cannot
   be built**: the phone has no working TLS and cannot reach the VoIP.ms API at all. COVEY
   relays instead, over the LAN and over LoRa. COVEY's half is live; the phone's RX hook and
   ingest are written; **the LAN poller and the channel import are still to do.** The
   measured VoIP.ms facts below are all still true and still worth not re-deriving.

   **What COVEY does, and why it does not touch this phone.** COVEY does **NOT** register
   SIP. It uses the VoIP.ms **REST API** (`https://voip.ms/api/v1/rest.php`, `getSMS` /
   `sendSMS`) with the account email + an API password, IP allow-list opened to `0.0.0.0`.
   That was deliberate: VoIP.ms overwrites a registration when the same sub-account is used
   twice, so registering COVEY as `565611_nikguy` would have **taken the registration off
   this phone** and killed its inbound calls and texts. Nothing in the portal changed —
   the DID still routes SMS to this sub-account, and this phone's SIP is untouched.

   **The gap to close here.** `getSMS` returns the ACCOUNT's history, so COVEY sees both
   directions including everything this phone sends and receives. Nothing pushes COVEY's
   outbound to this phone, so **a text sent from COVEY never appears in this phone's
   threads.** The fix is for the WiPhone to poll the same `getSMS` and merge, which pairs
   naturally with item 1 below — the conversation-style rewrite has to touch the storage
   layer anyway, and merging a second source is much cheaper to design in now than to
   retrofit afterwards.

   ⚠ **THREE THINGS MEASURED ON COVEY THAT WILL BITE HERE TOO — do not re-derive them:**
   - **`type=0` is SENT and `type=1` is RECEIVED**, the opposite of what the name suggests.
     Established against messages whose origin was known independently (the "test fr" /
     "om covey 2" pair typed on COVEY, and a text sent from this phone, are both type=0;
     every reply from the mobile is type=1). Getting it backwards makes the unread count
     permanently zero and draws received texts as if you sent them.
   - **`limit` returns the OLDEST messages, not the newest.** A limit-only query silently
     stops returning new messages once history outgrows it, while still answering
     `"status":"success"`. Bound the query with a `from`/`to` date window instead.
   - **VoIP.ms answers HTTP 403 to a request with no User-Agent header** (Python's default
     `Python-urllib/3.x` is refused outright), before the API is reached, so there is no
     status string to explain it. Send some User-Agent.
   - Timestamps did not reconcile to any single offset on COVEY; it orders by message id
     and clamps display times so nothing shows in the future. Expect the same.

1. 🔜 **NEXT UP, AGREED WITH NICK 2026-08-17: make the SIP texting app CONVERSATION-STYLE**,
   the way the Meshtastic app already is, instead of the current inbox/outbox split.

   **Scouted, so do not re-derive it:**
   - **`MessagesApp` (GUI.h) is built around the split**: states `MAIN / INBOX / OUTBOX /
     COMPOSING`, separate `inboxMenu` and `sentMenu`, and separate `inboxOffset/inboxSelected`
     and `sentOffset/sentSelected`. Nine methods in GUI.cpp, plus `ViewMessageApp` (5) and
     `CreateMessageApp` (11) hanging off it.
   - **`MeshtasticApp` is the working template to copy** — `MESH_CHATS` lists conversations,
     `MESH_THREAD` shows one correspondent with both directions interleaved, `MESH_COMPOSE`
     writes into it. Same widget vocabulary, same app skeleton.
   - ⚠ **The storage layer is the real work, not the UI.** `flash.messages` is queried as two
     flat lists (incoming / sent). Threading needs a grouping pass by correspondent, and the
     SIP URI is the natural key — but check how `Storage::messages` stores and sorts before
     assuming it can group cheaply on a phone with ~20 KB of internal heap.
   - Widgets are all PSRAM-backed now (`AbstractWidget::operator new`), so building a few more
     menus costs contiguous internal RAM roughly nothing — the app-open probe will confirm.

2. 👀 **WATCH FOR CRASHES IN NORMAL USE.** This is the standing task and the only real proof.
   ✅ **Overnight 2026-08-16→17: 5.4 hours idle, SIP registered, ZERO `reset_reason=4`.** But
   that was idle. A day of real use — menus, Books, the Game Boy, calls — is the test that has
   not been run. ⚠ **Check the reason before assuming:** `4` is the abort that was fixed;
   `1` is power-on class and a completely different investigation.

3. 🔎 **ONE UNEXPLAINED PANIC, 2026-08-17.** The phone panicked once while STARTING an outbound
   call, then the next attempt worked and it has not recurred. Strong suspicion, unproven: a
   call changes the sample rate, which makes `configureI2S()` reinstall the driver, and that
   reallocates **~16 KB of internal DMA-capable RAM** (`dma_buf_count(4) x dma_buf_len(1024) x
   2ch x 2B`) against a plateau of ~20 KB. **No backtrace was captured**, so this is a theory.
   If it happens again, get the backtrace before touching anything — see the lesson below.

4. **Node names should fill in on their own** now the node table evicts instead of freezing.
   Stock firmware beacons every 3 h, so give it hours, not minutes.

5. ✅ **CLOSED, all confirmed by hand on the device.** OK-commits-a-letter typing (*"typing
   seems to work a lot better"*), the **Firmware update** USB how-to page, the four **Clear**
   labels, the **visible cursor**, the **ringer mode**, and **calls and texts in both
   directions** on a real number.

   ⚠ **TWO CORRECTIONS WORTH KEEPING, because both came from trusting a short look.**
   - This doc once said *"`MultilineTextWidget::processEvent` has ZERO references to
     `WIPHONE_KEY_LEFT`/`RIGHT`"* and queued writing cursor movement. **Wrong.** It handles
     both and always has — the scouting grep used a 160-line window and the handler sits ~190
     lines into the function. The real bug was the cursor being drawn in hardcoded
     `WP_COLOR_0` (black) on screens whose theme sets a BLACK background, so movement worked
     all along and was simply invisible. **Bound your grep window; confirm "missing" before
     building on it.**
   - The POP hostname was inferred as `seattle.voip.ms` from VoIP.ms running seattle/2/3.
     It is `seattle1.voip.ms`. **Read it off the portal.**

5. **Nothing else is queued.** Everything below is reference.

### 🔑 THE ONE TOOL THAT MADE TODAY WORK — use it before theorising
`GUI::enterApp()` carries an **app-open heap probe** that writes to `health.log` *and* serial:
```
APP id=16398 heap 15772->12492 largest 12284->9216 (-3068)     <- Books, before the fix
APP id=16398 heap 15732->15316 largest 12208->12068 (-140)     <- after
```
It named the culprit on the first crash after being added, then **validated its own fix in the
same log file** with starting conditions 76 bytes apart. App ids are `GUI_BASE_APP (16384) +
ordinal`: **16388** CLOCK · **16389** SPLASH · **16392** PHONEBOOK · **16394** MESSAGES ·
**16397** MESHTASTIC · **16398** BOOKS · **16399** MUSIC · **16406** DIAGNOSTICS ·
**16409** NETWORKS · **16410** AUDIO_CONFIG · **16426** GBC.

⚠ **`largest` is the number that predicts the crash.** `free` and `min` disagree with it and
neither predicts anything. `min` is flat while idle and steps down only on events.

### ⚠ THE BUG CLASS THAT PRODUCED FOUR OF TODAY'S SEVEN FAULTS
**Component A writes shared state; component B reads it without setting it, so B's behaviour
depends on what A did last.** The menu ID collision, the Game Boy's I2S channel format, the
notification pop's device config, and the CPU gate's shared SIP predicate were all this.
**Every consumer must set what it needs. Anything borrowing the audio device for a one-shot
must `preserve()`/`restore()` around it.** When fixing one caller of a shared predicate, add a
new predicate rather than editing the shared one.

### ⚠ WHERE I WAS WRONG TODAY — recorded so it is not re-trusted
- **OTA was never "ready but for a version bump".** The transport cannot work at all — see the
  memory-wall section. I said otherwise before testing it.
- **The WiFi scan screen was NOT a missing `scanDelete()`.** The framework frees results
  itself. Right fix (5 s tick), wrong reason.
- **`gaps:0` does NOT rule out audio starvation** — only *severe* starvation. A 5 ms dropout is
  invisible to that counter and plainly audible.
- **The mesh frequency was NOT mismatched.** `djb2("LongFast") % 104 = slot 19 = 906.875 MHz`,
  exactly what is hardcoded. I had a tidy theory involving COVEY's 2.7.26 upgrade; it was wrong.

⚠ **`wiphone.local` mDNS goes stale when the phone changes network** (seen at
`192.168.158.33` on the hotspot, then `192.168.1.57` on SmithWifi). It pings but port 80
refuses. **Resolve first** — `dscacheutil -q host -a name wiphone.local` — and use the raw IP
if in doubt.

---

## ✅ WHAT SHIPPED 2026-08-15 (all measured, all on hardware)

| | |
|---|---|
| **Restarts** | WiFi event handler re-registered on **every** connect retry — ~430 copies per car journey, each multiplying a 1,460-byte UDP churn on reconnect. Registered once now. |
| **Restarts** | **Opening Books cost ~3 KB of contiguous internal heap, every time.** `BooksApp::operator new` → PSRAM. −3,068 → −140, validated by the probe. |
| **Restarts** | Notification pops permanently reconfigured the audio device (6 parameters, none restored). `Audio::preserve()`/`restore()` implemented — they had been declared under a bare `// TODO` since the beginning. |
| **Restarts** | `configureI2S()` reinstalled the driver — and its ~16 KB of internal DMA buffers — to change nothing. |
| **Restarts** | The WiFi scan screen rebuilt its whole menu every second (and asked for 750 ms **per channel**, so a scan could never finish). Now 5 s. |
| **Battery** | **CPU pinned at 240 MHz for 19+ min, screen off, out of range**, because `sipCallActive()` counts `HangUp` — a teardown state the phone can rest in forever with no proxy. Separate `sipNeedsFullSpeed()` for the CPU gate. |
| **Game Boy** | Ran at **50%** because it set the I2S sample rate but **inherited** the channel format. Paced by a blocking `i2s_write`, a mono sink drains at half rate. Arithmetic matched to the decimal. |
| **Menus** | Settings > WiFi auto-switch showed the Music icon and **opened Music** — duplicate menu ID. Now checked at boot. |
| **Diagnostics** | `/log?tail=N`; the health log **keeps its newest 32 KB** instead of deleting everything; the app-open probe. |
| **Book sync** | **Proved on air, both directions.** |
| **Mesh** | Periodic NodeInfo, replies to `want_response`, editable short name, node table evicts instead of freezing. |

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

### 🎯 THE SECOND CAUSE, FOUND AND FIXED 2026-08-15: **opening Books cost ~3 KB, every time**
**Nick's hypothesis was that the crashes were WiFi retrying while off-network. The data says
no.** Second run of the car session: **`wifi=1` (out of range) for 16 straight minutes**, retrying
every 20 s and auto-scanning every 2 min, with `largest` flat at ~15,000 — oscillating a few
hundred bytes and never ratcheting. **The off-network path is clean; that is the WiFi handler fix
holding.**

**The app-open probe named the culprit on its first crash.** Every other app measured `+0`;
Books measured, on two separate runs that each panicked within two minutes of it:
```
APP id=16398 (BOOKS)  largest  8,596 ->  5,732  (-2,864)   ... then 4,692, 3,084, PANIC
APP id=16398 (BOOKS)  largest 12,284 ->  9,216  (-3,068)
```
**Cause:** `new BooksApp` is a single ~3 KB **internal** allocation. `EpubBook book` alone is
**1,584 bytes by value** (measured on the host), plus `BookSyncRecord pending` ~250,
`ids[3][72]` 216, `hist[48]` 192, the sync strings 108, `libNote`+`headerTitle` 160, and the base
classes — landing right on the measured figures.

⚠ **AN EARLIER FIX MOVED THIS APP'S ARRAYS TO PSRAM (22,520 bytes) AND STOPPED THERE, LEAVING
THE OBJECT ITSELF INTERNAL.** That is why the phone kept panicking after opening a book even
though the arrays were "already fixed". Do not read "Books was fixed" as meaning all of it was.

**Fix:** `BooksApp::operator new` → `ps_malloc`, moving the whole object in one step instead of
converting a dozen members to pointers. Falls back to internal if PSRAM is exhausted; `free()`
is region-agnostic on ESP-IDF so one delete serves both.

✅ **VALIDATED BY THE SAME PROBE, before and after in one log file:**
```
before:  APP id=16398  largest 12,284 -> 9,216   (-3,068)
after:   APP id=16398  largest 12,208 -> 12,068    (-140)
```
Starting conditions **within 76 bytes**, so it is like-for-like. **95% reduction.** The residual
−140 is the widgets, which are separate `new`s registered in `registeredWidgets`.

⚠ **STILL OPEN, smaller:** `largest` fell **17,480 → 13,312 while the NETWORKS screen was open**
— the manual *Scan WiFi networks* page, which is NOT the background auto-switch scan (the flat
16 minutes above exonerates that one). Likely a missing `WiFi.scanDelete()`. Chase it after
confirming Books.

### ✅ FIRST REAL EVIDENCE THE FIX WORKS (2026-08-15, Nick out of the house)
An 85-minute run, **`wifi=1` (WL_NO_SSID_AVAIL — out of range) for essentially all of it**,
which is the exact state that killed it in the car. Out of range, `WiPhone.ino:1667` retries
every `WIFI_RETRY_PERIOD_MS` (20 s), so that window is **roughly 250 connect attempts** — each
of which used to append a handler that was never removed, with the vector doubling as it grew.

| up | largest | wifi |
|---|---|---|
| 29 min | 24,632 | 1 |
| 45 min | 24,632 | 1 |
| 61 min | 24,632 | 1 |
| 77 min | 24,632 | 1 |
| 85 min | **24,632** | 6 |

**Not one byte of movement in 85 minutes, at nearly 3× the headroom the car run had** (which sat
pinned at 8,848 and panicked at 143 min). ⚠ **HONEST LIMIT: the phone never ASSOCIATED in that
window.** This validates the half where registrations accumulate on retries. It does **not** yet
exercise the amplification half — one `GOT_IP` running the handler N times and churning the
1,460-byte UDP buffers — which needs a successful rejoin after a long time out of range.

**Where to look:** `/health.log` on the card. A line a minute; it survives both the reboot and
being unplugged, so the reset reason of the run that died sits at the top of the next run's
entries.

⚠ **THE LOG USED TO DESTROY ITS OWN EVIDENCE, AND IT COST US ONE (fixed 2026-08-15).** At the
96 KB cap it called `SD.remove()` on the whole file. On 2026-08-15 the phone rebooted while Nick
was out, the log hit the cap 29 minutes later, and **the `BOOT reset_reason=` line for that
reboot was deleted before anyone could read it** — the one number the investigation turns on.
`healthLogTrim()` now copies the last **32 KB** (~4 hours) forward and swaps the file in, so
there is always recent history. Streamed in a 512-byte stack buffer — a 32 KB allocation here
would be self-defeating on this phone. Falls back to deleting if the copy fails, because an
unbounded log is worse than a lost one. Also fixed the ordering: the size check now runs
**before** the boot line is written, so a boot line can no longer be destroyed by the very call
that wrote it. ⚠ **The trim path does not execute until the log next reaches 96 KB (~12 h at a
line a minute), so it has NOT been exercised on hardware yet.**

```bash
curl "http://wiphone.local/log?tail=6000"     # with any upload screen open
```
⚠ **Use `?tail=`.** A whole-file fetch truncates — with modem sleep on and the CPU at 80 MHz
the link is slow — and truncation costs you the END of the file, which is exactly where the
reset reason lives. Retrying instead is what made the reading of the log a cause of the crash.

### 🔋 THE CPU STUCK AT 240 MHz IN A DEAD SIP STATE — found and fixed 2026-08-15
**Measured from the car log:** the phone sat at **`sip=6` (`CallState::HangUp`)** with the
**screen off** and **no network (`wifi=1`)** for **19+ consecutive minutes**, holding the CPU at
**240 MHz** the whole time. At 80 MHz the core draws roughly half as much, so this was about
**double the idle current — out of range, in a car, on battery.**

`sipCallActive()` counts `HangUp` as a call. But `HangUp` and `HangingUp` are **teardown**:
HangUp's own comment says it "can be triggered from any other state", and HangingUp is "waiting
for confirmation of BYE/CANCEL, resending" — **with no proxy reachable that confirmation never
arrives and the phone rests there indefinitely.** Neither has an audio session to protect.

⚠ **THIS IS THE SAME TRAP THE REPO ALREADY HIT AND WROTE UP.** The earlier one was the phone
resting in `CallState::Error` (12) forever while a "not idle" test called it active; the fix
replaced exclusion with a **positive list of eight states**. Correct as far as it went — **but
two of those eight can also stick.** A positive list is not automatically safe; ask of each
member "can the phone rest here forever?"

**Fix:** a separate `sipNeedsFullSpeed()` for the CPU gate — the six states with a live or
imminent audio session. ⚠ **Deliberately NOT an edit to `sipCallActive()`**, which also gates
music-pauses-for-a-call; quietly changing a shared predicate to fix one caller is the exact bug
class that produced three separate faults in this codebase on the same day. Verified on
hardware: `CPU 80MHz (idle)` with the screen off.

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

### ✅ MEASURED BY NICK 2026-08-14 — two facts, and they point the same way
1. **`gaps` never left ZERO**, through general crackle on headphones.
2. 🔑 **"when I let the screen sleep the music cleared up almost perfectly."**

⚠ **FIRST, A CORRECTION: `gaps:0` IS MUCH WEAKER EVIDENCE THAN THIS DOC USED TO CLAIM.** Read
what it actually counts (`Audio.cpp`, the `starvedNow` logic): it only increments if the loop
arrives with the decode buffer empty **and** the DMA then accepts 12 consecutive frames without
filling. The DMA holds ~4,096 frames — about 3.5 MP3 frames — so it fills after ~4 iterations
and clears the flag. **It therefore only ever catches a drought of roughly 313 ms.** A DMA that
empties for 5 ms is invisible to it, and 5 ms of silence is an audible click. **So `gaps:0`
rules out catastrophic starvation, NOT starvation.** The old "if it stays at 0 the cause is
elsewhere" was wrong and cost nothing only because Nick reported the screen fact too.

**▶ LEADING HYPOTHESIS: TFT/SD SPI BUS CONTENTION.** Verified from the pin definitions, not
guessed:

| | pins | speed |
|---|---|---|
| TFT (`src/TFT_eSPI/User_Setup.h`) | SCLK 18, MOSI 23, MISO 19, **CS 5** | 40 MHz |
| SD (`WiPhone.ino:760`, `Hardware.h:110`) | **same bus**, **CS 2** | 15 MHz |

`SD.begin(SD_CARD_CS_PIN, SPI, ...)` puts the card on the very `SPI` object the display uses —
those are the ESP32 VSPI defaults. `SUPPORT_TRANSACTIONS` is force-defined on ESP32
(`TFT_eSPI.cpp:36`), so the bus is properly mutexed: **no corruption, but strict serialisation —
while the screen draws, an SD read waits.** And the now-playing screen sets
`msAppTimerEventPeriod = 1000` (`app_music.cpp:305`), so it wakes and redraws every second.
Screen asleep → no TFT traffic → the card is uncontended → clean. That is Nick's observation
exactly, and it explains `gaps:0` at the same time, because these stalls are milliseconds.

**▶ THE ONE-PRESS TEST THAT SETTLES IT (do this first, tomorrow).**
Play a track, then **press Back to the Music library list and leave the screen LIT.** Playback
continues by design (the player outlives its app).
- **Crackle CLEARS with the screen still on** → contention, and it is software-fixable. This
  also rules out the backlight electrically **and** rules out CPU frequency, since both are
  unchanged on a static lit screen. That is what makes one button press decisive.
- **Crackle PERSISTS** → electrical coupling from the display/backlight into the audio path,
  and no amount of code will help.

If it is contention, the fix direction is **shrinking what the once-a-second redraw does**, or
topping up the I2S buffer around it — **not** a ring buffer and **not** bigger DMA buffers (see
the red warning below). Do not add a ring buffer on a hunch; decode has ~74% headroom (measured:
5103 µs/frame against a 24000 µs budget).

🔴 **AND THE OBVIOUS FIX WOULD MAKE THE RESTART BUG WORSE — check this before reaching for it.**
I2S is `dma_buf_count = 4`, `dma_buf_len = 1024` ≈ **93 ms** at 44.1 kHz, and the decode-ahead
loop can produce up to 12 frames (~313 ms) in one pass, so it can already refill that buffer
several times over. Those buffers are also roughly **16 KB of internal, DMA-capable RAM — the
exact resource that is running out and panicking the phone** (§1). **Enlarging them trades the
crackle for the restart.** The two open bugs pull in opposite directions; that is why the
`gaps` reading has to come first.

### 🎮 4. Game Boy ran at 50% — FIXED, needs one look to confirm
Nick, 2026-08-14: *"my Metroid II game (probably all the games) is running at 50%"*, and he
remembered older work that shut off WiFi and mesh during play. **Those optimisations are all
still present and were not the problem** (`app_gbc.cpp:187-188` kills WiFi; the main loop gates
`lora.loop()` and `meshService.loop()` on `!gGbcActive`). **It was not CPU speed either** —
`gGbcActive` is in the 240 MHz "busy" list, and `CONFIG_FREERTOS_HZ` is 1000 so the main loop's
`vTaskDelay(1)` is 1 ms, not 10.

**🔑 THE EMULATOR IS PACED BY ITS AUDIO, SO THE I2S CHANNEL FORMAT IS A TIMING PARAMETER.**
The design header says it outright: the blocking `i2s_write` is *"the de facto clock, locked to
the DAC with zero drift"*. `gnuboy_get_audio_count()` returns a **stereo interleaved** int16
count.

`app_gbc.cpp` set the RATE and inherited the FORMAT — `setSampleRate()` only calls
`i2s_set_sample_rates()` and never touches `channel_format`, which `configureI2S()` derives from
`Audio::monoOut`. `monoOut` defaults to false, so for a long time it was stereo **by luck**.
Then the music player began following the headphone jack (stereo on headphones, mono to the
loudspeaker) and started leaving `monoOut = true`. The arithmetic:

| | bytes/second |
|---|---|
| emulator produces | 32264 × 2ch × 2B = **129,056** |
| DAC consumes, **stereo** | 32000 × 2 × 2 = 128,000 (the documented 0.8% surplus) |
| DAC consumes, **mono** | 32000 × 1 × 2 = **64,000** |

Half the sink, so the write blocks twice as long: **50.4%**. Nick measured 50%.

**Fix:** `audio->setMonoOutput(false)` alongside the rate, so it is set rather than inherited.
Flashed 2026-08-14. ⚠ **Confirm two things in-game:** the pause menu's speed readout should be
**green ≥97%** (it is drawn orange below that, `app_gbc.cpp:811-814`), **and game sound should
still work** — forcing stereo also flips `codec.setAudioPath(!mono)`, which is the known-good
historical state but has not been heard by ear since the change.

⚠ **THE TRAP THAT MAKES THIS LOOK LIKE A CPU PROBLEM:** the adaptive frameskip
(`app_gbc.cpp:942`) only drops **display** frames. It cannot speed up a game paced by audio, so
at 50% it simply pins itself at maximum skip and stays there. **A maxed-out skip plus an orange
speed readout reads as "the CPU is too slow" and here it meant the opposite.** The comment at
`app_gbc.cpp:927` already warned about *"wrong rate after some phone sound reconfigured it"* —
audio had throttled games to 13% once before.

### 🎉 5. BOOK SYNC WORKS ON AIR — PROVED 2026-08-15. **Nick: *"works! thanks"***
**The oldest open item in this feature is CLOSED.** A reading position crossed from the WiPhone
to COVEY over LoRa, between two real devices, for the first time. Everything before this was
host tests against generated vectors; this is the real thing.

**🔑 THE TRAP THAT MADE IT LOOK BROKEN — and it will catch the next person too:**
Nick tapped **Sync my place** and reported *"the covey got nothing"*. **Nothing was supposed to
happen.** A LoRa round trip does not fit in the 15 s sync window, so the receiver **parks** the
record and offers it **only when you next OPEN THAT BOOK**. There is no notification, no toast,
no badge — the receiving device looks completely inert until you open the book. Opening it
produced the confirm card immediately, and it had almost certainly worked the first time.
**If sync "does nothing", OPEN THE BOOK ON THE RECEIVER before debugging anything.**

⚠ **The parked record is RAM-ONLY.** A `covey-ui` restart discards it silently — which happened
during this very debug, when the radio check restarted the service. Re-send after any restart
before concluding it failed.

### ✅ BOTH DIRECTIONS PROVED. **You do NOT need to tap Sync on both devices.**
COVEY → WiPhone was confirmed straight after (Nick: *"it does work! thanks"*), so it is
bidirectional on real hardware.

⚠ **The "tap Sync on the other device" prompt is NOT a requirement, and it reads like one.**
There is no request/response in this protocol (D-089): tapping *Sync my place* **broadcasts your
own position**, and the receiver parks it with **no tap at all**. Tapping on both is only for the
two-way case where you want each device to learn the other's and converge. **For a one-way push,
only the sender taps** — Nick reasonably read the prompt as "you must tap both".

**The receiver is always listening, whatever app is open:** `meshtastic_service.cpp:507` calls
`bookSyncInboxPush()` from the radio receive path, and `app_books.cpp:930` calls
`bookSyncInboxFindFor()` when a book is opened. Nothing needs to be on screen for a packet to be
kept.

### 🔎 THE DIAGNOSTIC SCREEN — **Books > menu > Sync settings**
Built for exactly this and worth reaching for FIRST, because it answers both failure modes:
```
Channel 'booksync': found        <- or MISSING (app_books.cpp:1571)
Parked positions: N              <- N>0 means a record arrived and is waiting (app_books.cpp:1573)
```
⚠ **`Parked positions` is the ONLY visibility you get.** The line logged when a sync packet
arrives (`meshtastic_service.cpp:509`) is a **`log_i`, and only `log_e` is compiled into this
build** — so a packet landing prints *nothing at all* on serial. Do not read serial silence as
"it never arrived"; read the counter.

**The verified-good configuration, for reference when it next misbehaves:**

| | |
|---|---|
| channel | `booksync`, **index 3** on COVEY's radio |
| PSK | **[redacted — this repo is public]** 16 bytes, verified **byte-identical to the stored invite**. Read it off the radio: `meshtastic --port /dev/ttyACM1 --info`, channel index 3. |
| passcode | **[redacted]** — the same on **both** devices; read it from `booksync_pw` in COVEY's `/root/.covey/prefs.json`. It is short and was never rotated after being published; low stakes (it guards page positions only), but do not reuse it for anything else. |
| book | `Ghosts_of_Timkovichi.epub`, **5,059,833 bytes on both** — same size, so the ids agree |

⚠ **An invite STORED in prefs and an invite APPLIED to the radio are different states.** Checking
the radio needs `covey-ui` stopped to free the serial port:
`sudo systemctl stop covey-ui; meshtastic --port /dev/ttyACM1 --info; sudo systemctl start covey-ui`
— **always restart it**, it is easy to leave the device UI-less.

---

## 🔊 THE SHARED-AUDIO-STATE AUDIT (2026-08-15) — 12 findings, 1 fixed, 9 latent

Nick asked for an audit after two bugs in one day turned out to share a root cause. **18
candidates, 12 survived independent adversarial refutation.** The pattern hunted was:
*component A writes shared state, component B reads it without setting it, so B's behaviour
depends on what A did last.*

### 🎯 The headline is a CORRECTION: the music player is not the main offender
**`Audio::playPop()` is, and it predates music.** It runs on every Meshtastic notification and
sets **six** parameters — `setDataChannels(1)`, `setBitsPerSample(16)`, `setSampleRate(8000)`,
`setMonoOutput(true)`, `setHeadphones(false)`, `chooseSpeaker(true)`,
`setVolumes(Max, Max, MaxLoudspeaker)` — and the teardown at `WiPhone.ino` called only
`ceasePlayback()`, **restoring none of them.** One mesh message left the phone at 8 kHz, mono,
loudspeaker-forced, full volume, permanently.

⚠ **That means a mesh notification was almost certainly the REAL cause of the Game Boy 50%
bug**, not the music player — `monoOut = true` is the whole mechanism, and a mesh message is far
more frequent than a music session. The `app_gbc.cpp` fix immunises the emulator; this fixes the
source.

✅ **FIXED 2026-08-15 by implementing `Audio::preserve()` / `Audio::restore()`** — which were
**declared in `Audio.h` from the very beginning under a bare `// TODO` and never implemented.**
The original authors saw this need and every one-shot sound since has leaked device state.
⚠ **`restore()` reinstalls the I2S driver AT MOST ONCE and only if something differs** —
`configureI2S()` reallocates ~16 KB of internal DMA buffers, and fragmenting internal heap is
what causes the panics, so putting values back through the individual setters would have cost
several reinstalls per notification and been worse than the bug.
⚠ **`playback` is deliberately NOT restored** (ceasePlayback closes the file). **A pop still
stops the current track — that is a separate, unfixed issue.**

### ⏸ NINE FINDINGS ARE REAL BUT UNREACHABLE — do not spend time until SIP works
**Every one of them needs a completed SIP call, and this phone has never made one.** Measured:
**`sip=12` (`CallState::Error`) in 49 of 50 health samples** — with no proxy reachable the phone
rests in Error forever. Fix these *before* SIP is ever made to work:

| | what |
|---|---|
| 🔴 **Live mic RTP leak** | Call teardown never clears `microphoneOn` / `microphoneStreamOut` / `rtpRemoteIP` / `rtpRemotePort`, and `audioOn` is the **only** gate. Anything that turns audio back on — music, **or a mesh pop, which needs no user action at all** — resumes encoding the live microphone and sending RTP to whoever you last called. `microphoneOn` is written exactly ONCE in the whole tree (`= true`); there is no `= false` anywhere and no `rtp.stop()`. |
| 🔴 **`micEnc[1600]` overflow** | `packetSizeSamples()` is computed from `sampleRate`/`dataChannels`. With music's 44.1 kHz stereo left behind, a G.711 call overruns the buffer by ~164 bytes, landing on the `WiFiUDP rtp` object's socket fd and tx_buffer pointer. |
| 🟠 **Ringtone at music volume** (×4 findings, one root cause) | `startRingtone()` takes `Audio::playback` away from music **before** `musicPlayerPause()` checks `audio->musicPlaying()`, so `restoreCallVolume()` is skipped. The phone rings 18–45 dB quiet and the call stays there. |
| 🟠 **F1 during a call** | The transport keys were guarded for the emulator and for Books but **not for a live call**. F1 starts a track, steals `Audio::playback` from `RtpStream`, and reinstalls I2S at 44.1 kHz underneath the call. |
| 🟡 **Settings > Audio corruption** (×2) | Opening that screen while music plays persists the **music** volume into `/configs.ini` as the call volume, surviving reboot; and saving a volume there while music plays is silently reverted by music's stale snapshot. |

### 🟢 Cosmetic, worth knowing
**Diagnostics keypad self-test can never register F1/F2** (and F3/F4 while playing) once a track
is loaded — the transport consumes them first, so the four side-button squares stay grey and
read as **dead hardware**. `WiPhone.ino:1495-1515` vs `GUI.cpp:8473-8567`.

### The general lesson
**`Audio` is a device-wide singleton with no ownership discipline.** Every consumer should set
what it needs rather than inherit, and anything that borrows the device for a one-shot should
`preserve()` / `restore()` around it. The `app_gbc.cpp` fix and this one are the same fix applied
at two different levels.

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

## 🛑 OTA CANNOT WORK ON THIS BUILD — IT IS A MEMORY WALL, NOT A URL OR A CERTIFICATE
**Established 2026-08-15 by actually trying it.** Nick pressed **Check** on a correctly
published 0.9.2 and got:
```
Dev: 0.9.1   Srv: 0
Error: -301 - Can't connect to ser[ver]
```
`-301` is set at **`ota.cpp:585`** when **`client->connect(hname, 443)`** fails. That is the
**TLS handshake**, before a single byte of HTTP. The URL was right, the manifest was live, and
the binary served was byte-identical to the build — all verified from the Mac.

**The arithmetic, from the framework's own `sdkconfig.h`:**

| | |
|---|---|
| `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN` | **16384** — a ~16 KB IN buffer **and** a ~16 KB OUT buffer, so **~33 KB** |
| `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` | **not set** — so none of it may come from PSRAM |
| phone's free internal heap | **~19 KB total** |
| phone's largest contiguous block, **fresh boot** | **~14,900 bytes** |

**It is not close, and it is not a fragmentation problem — there is not enough internal heap
even when perfectly clean.** The handshake cannot succeed on this build, ever. That `[E]
WiFiClientSecure.cpp: start_ssl_client: -1` line in every boot log is this, and it has been
there all along.

⚠ **THIS CORRECTS THIS DOCUMENT.** The section below says the update page "works again" after
the expired-pinned-cert fix. **That fix was real and necessary but it was never the whole
story** — the phone still cannot open the connection. So "**OTA has never actually installed
anything**" was never about caution or bad luck: **it has never been able to.** Do not re-plan
around OTA until the transport is changed.

✅ **ACTED ON 2026-08-16.** The UI is gone (Settings > Firmware update is now a USB how-to) and
the boot-time attempt is **compiled out** behind `OTA_TRANSPORT_AVAILABLE` in `WiPhone/ota.h`.
`start_ssl_client: -1` no longer appears in the boot log. **Flip that macro to 1 only when the
transport itself changes** — the download code is intact and still builds, so it is one line
when there is something for it to talk to. ⚠ Two things learned doing it: `ota.updateExists()`
is the network call and it sat to the **LEFT** of the cheap predicates so `&&` never guarded it
(fixed underneath the macro), and **`autoUpdateEnabled()` defaults to TRUE**, so turning
auto-update "off" was never going to stop the attempt.

**Options, ranked:**
1. **USB.** 55 s, hash-verified, ~10 times in one day without a hitch. This is the working path.
2. **Add plain-HTTP support** and serve `firmware.bin` off the LAN — avoids TLS entirely. Port
   **443 is hardcoded at `ota.cpp:585`** and `https://` is assumed in four places
   (`ota.cpp:187, 207, 241, 260`). A real change, but the only one that could work without
   touching the framework.
3. **Rebuild arduino-esp32** with a smaller `MBEDTLS_SSL_MAX_CONTENT_LEN` or
   `MBEDTLS_EXTERNAL_MEM_ALLOC`. Correct in principle; PlatformIO ships precompiled libs, so
   this is a serious undertaking.

✅ **0.9.2 is published and verified live regardless** — manifest and binary both served from
`raw.githubusercontent.com`, binary byte-identical to the build. If the transport is ever fixed,
the release is already sitting there. **0.9.2 was installed over USB.**

⚠ `app1` has still never been written on this phone.

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

## The reader is DONE and confirmed, and sync is now PROVED ON AIR (2026-08-15)

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

### ✅ SYNC IS PROVED ON AIR (2026-08-15) — built 2026-08-11, commit 595e838
A position crossed from the WiPhone to COVEY over LoRa on 2026-08-15. Nick: ***"works!"***
Before that, what was proven was 34 host assertions against real COVEY-generated packets:
parking, matching, newest-wins, and that every wrong-passcode and tampered vector is diverted
out of Chats and never matches. **Those held up on real hardware, first time, with no code
changes** — the only thing that ever went wrong was knowing where to look for the result.
See the trap in **PICK UP HERE §5**: the receiver shows nothing until you open the book.

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
