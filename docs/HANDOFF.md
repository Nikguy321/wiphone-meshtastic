# WiPhone — session handoff

**Last updated:** 2026-08-17 · **`main` pushed and clean at `17b43e0`. Phone flashed with it and
booted clean. Tests 841/841.** Version still reports **0.9.2**; the CHANGELOG's top section is
**"Unreleased"** — bump `FIRMWARE_VERSION` when you release.

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

0. 🆕 **COVEY NOW TEXTS FROM THE SAME NUMBER, AND THIS PHONE CANNOT SEE WHAT IT SENDS.**
   Added 2026-08-17 from the COVEY side, at Nick's request; he will implement here on his
   next usage reset.

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
| PSK | `f2b880151f9b560c6a43068cbef9edb4` — 16 bytes, **byte-identical to the stored invite** |
| passcode | `1111` on **both** devices (`booksync_pw` in COVEY's `/root/.covey/prefs.json`) |
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
