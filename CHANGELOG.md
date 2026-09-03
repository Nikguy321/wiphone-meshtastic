# Changelog

## 0.9.56 (2026-09-03) - the battery percentage comes from the voltage, off phone 1's measured curve

Phone 1 ran itself flat overnight on purpose — 9h16m from a genuinely full cell, WiFi off,
screen off, 80 MHz, the cleanest load this project has logged — and the run showed the one real
gauge error we have ever measured: **the CW2015 reported `soc=0%` at 3.54 V while the cell ran on
to 3.30 V, 1.3 hours of runtime reported as empty.** Its battery profile (BATINFO) is never
programmed, so it runs a generic model whose floor is ~240 mV too high for this cell, and which
reads ~7 points optimistic through the middle. Mean error 5.7 points against time-linear truth,
worst 16.4.

Off the charger, the display no longer uses it. `battery_curve.{h,cpp}` maps cell voltage to SOC
through an 18-breakpoint table derived from that run (10 mV bins of time-linear SOC, 50 mV means),
and the battery tick sets `battSoc` from it whenever the phone is not charging. Against the run it
was built from: mean error 0.54 points, worst 2. The chip's own number is still read and still
logged as `soc=` in the health line, with the estimate beside it as `est=`, so the next discharge
can score both exactly as this one was scored.

**On the charger the chip's number is shown instead**, and the first flash of this build is why:
45 minutes into a charge from empty at 3.89 V, the curve said **61 %** and the chip said **20 %**.
Charge current lifts the terminal voltage ~100 mV, and a discharge curve read against it
over-reports by 30-40 points — optimistic, the wrong way for a battery display to be wrong. The
chip is the only charge-aware estimate we have. ⚠ On phone 2 `battCharged` reads 0 even on USB
(the backplate holds the rail), so phone 2 shows the curve on the cable — where its internal cell
is genuinely full and the curve's 100 % is right anyway. Phone 1's `health.log` is now recording a
complete charge (chip vs curve vs voltage, once a minute); a charge-side correction can come from
it later.

⚠ **This is phone 1's curve, and Nick's decision is that it is the firmware's standard** —
phone 2 and other people's phones use it as the best guess available. It is load-dependent
(voltage sags under load, so a lit screen or a hunting radio makes it UNDER-report — the
conservative direction), and it is fitted to one run and tested against that run, which proves it
reproduces the curve rather than that the curve holds on another day.

⚠ **The old `if (soc > 0.0)` guard on the chip's value made the display FREEZE at its last
non-zero reading once the chip hit 0 %** — so the phone showed "1 %" for its last 1.3 hours. Gone.

🛑 **Correction to the 0.9.54/55 notes: phone 2's 4.6 h at `soc=100%` was its woods BACKPLATE
feeding the internal cell, not a gauge dead zone.** Phone 2 wears the backplate unless Nick says
otherwise; its cell holds the internal one at ~4.2 V for hours, so `soc=100%` after unplugging is
real, `chg=0` on the USB cable is normal, and "12.5 h" is two cells in sequence. **Battery
experiments happen on phone 1.** Recorded in the handoff and in memory so it stops being
re-learned.

`tests/test_battery.cpp` (20 checks) loads the actual recorded run from
`tests/fixtures/p1_discharge_2026-09-03.tsv`, scores the table AND the chip against it, and pins
the specific fault: 3.54 V must still read ≥ 12 %. Mutation-tested — a breakpoint zeroed to the
chip's cliff, two breakpoints transposed, interpolation removed, the top clamp moved a band — all
caught.

## 0.9.55 (2026-09-02) - Position, Waypoint and the channel hash are pinned too; every varint reader hardened

The rest of the mesh surface gets the treatment 0.9.54 gave the Data and User protobufs.
Nick was hands-off for this one, so the work went in as limit-safe checkpoints (`8b93e72`,
then the pinning commit, then this release).

**Position and Waypoint are now pinned against upstream's own encoder.** `mesh_pos.cpp`
hand-rolls both, and `test_pos.cpp` had only ever checked them against bytes written by hand
in the same file. `tools/gen_pos_vectors.py` → `tests/vectors_pos.h` now carries what the real
Meshtastic runtime emits: Position encode byte for byte; Position decode including a
**stock-shaped broadcast with every field a RAK actually sends** (two-byte tags for
`sats_in_view`/`precision_bits` included) — which is the packet COVEY puts on the air every five
minutes; Waypoint decode for the full form, the id-only deletion marker, a truncated long name,
UTF-8, unknown fields 7/8, and no-id refused. This is the interop surface the hunting trip
depends on, and it was the one still resting on the author's own reading of the spec.

**The channel hash is pinned.** It lived in `mesh_crypto.cpp` beside AES, which needs mbedtls
and will not build on a Mac, so it could never join the suite — 0.9.54 listed it as a known
gap. `meshXorHash`/`meshChannelHash`/the default key move verbatim into **`mesh_hash.{h,cpp}`**
(`mesh_crypto.h` re-includes it, no caller changes), and `test_wire.cpp` asserts seven
(name, key) pairs against `meshtastic.util.generate_channel_hash`, plus
`meshDefaultChannelHash()` == the LongFast vector == `0x08`, the byte the boot line has always
printed. Of the two gaps documented in 0.9.54, only the 16-byte on-air header remains, and that
one is unpinnable from python by nature.

**UBSan found a real defect the moment the new vectors ran.** The stock-shaped Position carries
`timestamp_millis_adjust = -250`, and protobuf encodes a negative `int32` sign-extended to 64
bits — a **ten-byte varint**. `rdVarint` in `mesh_pos.cpp` shifted a `uint32_t` by 35 and then
63: undefined behaviour, which Xtensa masks to a 5-bit count. Nothing was ever misread (the walk
follows the continuation bit, and every field that file reads is `fixed32`), but `altitude` is
`int32` too and a fix below sea level is a real thing. `mesh_wire.cpp` had the same pattern in
three readers. All four now discard bits past 32; a below-sea-level Position vector and a
hand-crafted ten-byte-varint Data case pin it, and removing the guard makes UBSan complain again.

**The CallApp in-call volume handler can no longer wipe `configs.ini`.** Recorded as a known
hazard by the 0.9.52 review: it called `load()` (never `restore()`, unlike every other writer of
that file) and then `store()` unconditionally, so a failed load became a three-key file with
every other setting gone. It now tries `restore()` like its siblings and, if the file still will
not load, applies the volume to the driver and persists nothing, loudly. ⚠ Needs a completed SIP
call to reach, so it has never fired and **cannot be verified on hardware**; it is strictly
safer than what it replaces.

Every new assertion was mutation-tested — swapped tags, wrong fields, the old single-byte tag
read, XOR→OR in the hash — all caught. Host suite green under ASan+UBSan with zero runtime
errors. `test_wire` 128 → 139 assertions.

## 0.9.54 (2026-09-02) - the Meshtastic wire format is pinned against upstream's own protobufs

Nick asked whether this phone can keep up if Meshtastic changes their core files. It can — this
is a reimplementation rather than a fork, with the Data and User protobufs hand-rolled as literal
tag bytes, and every parser here skips fields it does not recognise, so upstream adding fields
(the overwhelming majority of what they do) steps straight past us. What was missing was any way
to notice the changes that *would* hurt: an existing field renumbered or re-meant.

`meshBuildData`, `meshBuildUser`, `meshFillHeader`, `meshParseData`, `meshParseUserName` and the
protobuf primitives move out of `meshtastic_service.cpp` into **`mesh_wire.cpp`**, byte for byte.
They were file-statics in a translation unit that drags in the ESP32 SDK, so nothing could ever
test them. `mesh_wire.cpp` is Arduino-free, and **`tests/test_wire.cpp` (84 assertions)** now
asserts its bytes against vectors generated by the real Meshtastic python runtime
(`tools/gen_wire_vectors.py`, meshtastic 2.7.11): Data encode and decode, User/NODEINFO encode
and decode, the `"!%08x"` id spelling, all six PortNums against `portnums.proto`, and — on
purpose — messages carrying fields this firmware does not know, so forward compatibility is a
test rather than a comment. `s_hopLimit` stops being shared mutable state read from inside
`meshFillHeader` and becomes a parameter.

The new test found a real divergence on the day it was written: **`meshBuildData()` always
emitted Data field 2, even for an empty payload** — `12 00` where upstream emits nothing, because
proto3 omits a field holding its default. It never misread a packet (an absent `bytes` field
decodes as empty) and it is unreachable in shipping code, since `meshTxText()` rejects empty
text. Fixed anyway: two bytes of LoRa airtime, and a permanent diff against every other node's
encoding.

The new suite was then pointed at itself by an adversarial review, which found four real
defects in code this change had just introduced or moved, all now fixed and all now covered:

* **`portnum` was written as one raw byte, not a varint.** Every portnum this firmware speaks
  is under 128, so nothing on air was ever wrong — but Meshtastic's enum runs to 511, and
  portnum 256 (`PRIVATE_APP`) encoded as `08 00`, a Data addressed to portnum **zero**. Silent,
  and it would have been inherited by whoever added the next portnum.
* **`meshBuildUser` emitted empty `long_name`/`short_name` as `12 00` / `1a 00`** where proto3
  omits them — the same defect as the empty payload, one field over.
* **`meshParseUserName` selected a present-but-empty `long_name` over a perfectly good
  `short_name`**, so a peer sending that form (which this firmware itself did until now)
  rendered as a bare node number with its name sitting unread two bytes away.
* **The generator emitted greedy C hex escapes.** `esc()` wrote bare `\xNN`, and a C lexer
  swallows a following hex digit — `"Caf\xc3\xa9a"` lexes `\xa9a` as one escape. The committed
  cases happened to be safe, which is precisely how it would have shipped broken later.

Coverage gaps the review found were closed too: decode vectors now include a multi-byte length
varint, an absent field 2, and unknown fields *before* a field the test asserts (the original
unknown-field vector put them all after, so a broken skip could not have changed the result).
Hand-written cases cover the two shapes upstream's runtime cannot emit — a `fixed64` unknown
field and a present-but-empty `long_name`. **Every fix was mutation-tested**: reverting each one
fails the suite, and the first version of the short-public-key test was found vacuous that way
and rewritten. 84 assertions → **128**.

⚠ Two surfaces are deliberately **not** pinned, and the handoff says so at length. The 16-byte
on-air PacketHeader is firmware-internal to Meshtastic and absent from the protobufs, so it gets
a structural test (size, field offsets, flag packing) that catches our drift but not theirs. The
channel hash *is* exposed upstream and matches ours exactly, but lives in `mesh_crypto.cpp`,
which needs mbedtls and will not build on a host; splitting the hash out would close it.

Rehearsed across a real version gap: the generator was run on COVEY (meshtastic 2.7.10) and its
output diffed against the committed vectors from 2.7.11 — byte-identical everywhere.

## 0.9.53 (2026-09-02) - the Buzz value is clamped on the way in

Follow-up to 0.9.52 from its adversarial review, which found **no defect** in the slider
itself and one hardening worth doing. `notify_vibro_ms` was stored at boot and on screen-open
straight from the ini with no range check, so a hand-edited `-1` became **65,535 ms — a
65-second motor run per notification** — and 0 became a sub-millisecond twitch that reads as a
dead motor. Worse, `audioDeviceBusy()`'s vibro term held the audio device awake for the whole
65 s. Both load sites now `constrain(v, 50, 650)`, the slider's own range. Consistent with how
the neighbouring `notify_*` fields load, so not a regression, but a bad file no longer becomes
a bad phone. Two stale comments still describing the pulse as a fixed 180 ms are reworded.

⚠ The review also surfaced a **pre-existing, unrelated** hazard, recorded in the handoff and
NOT fixed here: the CallApp in-call volume handler can rewrite `configs.ini` with only its
three volume keys if its own `load()` fails, dropping every other setting. It needs a completed
SIP call to reach.

## 0.9.52 (2026-09-02) - a Buzz slider, because a pocket is not a desk

Nick: *"can you add a 'vibration duration' slider to the notifications settings menu? I'd like
to customize what feels best when the phone is in a pocket."*

### ✅ Settings > Notifications > Buzz

50-650 ms in twelve 50 ms steps, default **200 ms** (the nearest on-grid value to the 180 that
had been hard-coded as `MESH_VIBRO_MS`). Stored as `ControlState::notifyVibroMs`, persisted as
`[audio] notify_vibro_ms`, loaded at boot in `GUI::loadSettings()` alongside the other
notification settings. The two places that time the pulse - the buzz in `notifyMessageArrived()`
and the release in `loop()` - read it through the same name they always did:

    #define MESH_VIBRO_MS (gui.state.notifyVibroMs)

⚠ **This is the NOTIFICATION buzz only.** The call ring pattern (`vibroOnPeriodMs` /
`vibroOffPeriodMs`, loaded from the ringtone config) is a separate mechanism and is untouched.
A burst of arrivals still re-arms the timer rather than being skipped, so several texts in a
row become one longer buzz at the chosen length.

### 🔧 The screen was full, so both sliders went inline

Header 30 + three 64 px label/choice rows + one 50 px stacked label-and-slider = 277 of the
280 px above the footer. A fifth stacked row did not fit. Both sliders now take the inline
form Screen config uses - label left, slider right, 25 px each, `labelWidth` 110 so the two
settings screens line up - which lands the whole screen back at 277. `FocusableApp(4)` → `(5)`.

**Verified on phone 2:** screen renders with both inline rows clear of the footer; four
`right` presses stepped 200 → 400 ms; **Save, reboot, reopen → 400 ms.**

## 0.9.51 (2026-09-02) - the join gate now asks the last scan, not the last 20 minutes

**0.9.50's scan-gated join was tested on a real 50-minute commute and both halves worked.** The
same log showed the gate engaging 25 minutes into the drive instead of 5, so half the trip still
burned blind joins.

### ✅ What the commute proved

```
up745   wifi=1   joins=12/0    lost WiFi setting off
up760   wifi=1   joins=18/0    last blind attempt
up770   wifi=1   joins=18/1    FIRST SKIP - gate engaged
up778   wifi=1   joins=18/2
up788   chg=1    joins=18/3    plugged in at work
up789   wifi=3   joins=19/3    REJOINED
```

`tried` frozen at 18 for **28 minutes** with zero join attempts, then one attempt the moment a
scan saw a saved network, and it connected. Drain over the gated stretch, least-squares on the
voltage trace: **~38 mV/h against 106 mV/h measured hunting on 0.9.46/47.**
⚠ **That magnitude is soft** — a 27-minute window, 10 mV gauge quantisation (±20 mV/h over that
span) and R²=0.54. The direction is strong; the number is not pinned.

### 🐛 The 20-minute lookback was buying nothing and costing twenty minutes

`worthAttemptingJoin()` compared `_savedSeenMs` against `WIFI_SCAN_EVIDENCE_MS` (20 min), so a
network seen just before setting off counted as "recent evidence" for twenty minutes after it was
gone. Scans run every **5** minutes in a dry spell, so the lookback could only ever delay the gate.

Replaced with `_savedSeenLastScan` — **did the MOST RECENT completed scan see a saved network?**
A boolean about the latest evidence, which is the question actually being asked. Cleared *before*
`autoSwitchEvaluate()` and set inside it, so the flag always describes the scan that just
finished rather than leaving a previous answer standing through an early return.

Expected effect on the same commute: the gate engages ~5-6 min after losing WiFi rather than ~25,
roughly doubling the gated portion of a 50-minute drive.

⚠ **The staleness check stays** (`_scanDoneMs` vs `WIFI_SCAN_EVIDENCE_MS`): no scan in a long
time still fails open. Only the *positive* evidence test changed.
⏸ **The safety valve is still untested** — it fires on the 4th consecutive skip and the drive only
reached 3. A longer trip would exercise it.

## 0.9.49 (2026-09-01) - one WiFi switch that actually means it

Nick: *"in settings let's just make a global Wi-Fi off toggle... it can shut everything Wi-Fi
related off with one button."* Plus, on the retry cadence: *"5 mins of fast reconnect then after
that period, slower retries every 10 minutes?"* — which is what 0.9.48 shipped, so the threshold
moves from 10 minutes to his 5.

### 🛑 "WiFi: off" was not off, and did not stay off

The toggle really did stop the PHY. Three other paths turned it back on without clearing it, and
boot never applied a stored OFF at all:

- **The WiFi list screen's 5 s rescan** (GUI.cpp) — `scanNetworks()` opens with
  `enableSTA(true)` → `esp_wifi_start()`, so standing on that screen put the radio back on air.
- **The ROM/file uploader's stop path** (app_gbc_xfer.cpp) — unconditional
  `WiFi.mode(WIFI_STA)` + `begin()`.
- **The Game Boy's exit** (app_gbc.cpp) — unconditional `mode(WIFI_STA)` + `reconnect()`.
- **Boot** — `Networks::init()` does `WiFi.mode(WIFI_STA)` and `setup()` only called `disable()`
  when there was NO saved SSID. Any naive "just persist the toggle" would have inherited this.
- **And the label could lie**: `wifiOn` was a bare global that the toggle set and those three
  paths did not.

### ✅ One switch, persisted, and honest

`Networks::setRadioOff()` flips it, persists it (Preferences, `wpwifi/radiooff`) and applies it in
one call. `Networks::radioOff()` is the single predicate all four paths above now consult.
`wifiOn` is **deleted** as a stored global and `#define`d to `!wifiState.radioOff()`, so the label
cannot drift from the radio.

⚠ **The original "NOT PERSISTED" comment gave a real reason** — *"a radio that stays off across a
power cycle is a setting you can forget you set, and the failure mode is a phone that silently
never connects again."* **That objection is answered, not ignored**: the menu row now carries a
subtitle (*"saves power - survives restarts"* / *"scanning and joining as usual"*), and boot
prints `WIFI: radio is OFF (your setting, kept across restarts)` at log_e.

**PROVED ON HARDWARE, both directions:** toggled off → row relabels, status bar clears; reboot →
`wifi=255` (station not started); toggled on → reboot → `wifi=3` associated.

### ⏱ The dry-spell threshold is now 5 minutes

Nick's number, and he argued it down from my 10 himself. He is right: the screen-wake escape hatch
forces an immediate scan **and** an immediate retry, so a long back-off costs far less
responsiveness than it looks like it should. Out of range: join retry 180 → 600 s, scan
300 → 900 s, radio-on time ~604 s/h → ~181 s/h (16.8 % → 5.0 % duty).

### 💡 Not done, deliberately — the next idea

The retry still calls `connectToPreferred()` blind, and a failed join holds the radio associating
for up to 30 s. **Scanning first (~350 ms) and only attempting a join when a known SSID is
actually present** would remove almost all of that. It is the biggest remaining win, and it is NOT
in this release because it cannot be tested from a desk — it needs a drive away and back, and
shipping it untested before a three-day gap risks a phone that will not rejoin. For next session.

## 0.9.48 (2026-09-01) - a phone with no WiFi to find stops hunting for it

Nick, after driving 50 minutes home with no WiFi and watching the battery: *"can we make it so
if it doesn't see a Wi-Fi signal in 10 minutes, the retry interval is a lot less?"*

Yes, and the measurement says he was pointed at the right thing. Field data from three runs on
phone 1 puts **WiFi searching at ~106 mV/h against ~60 mV/h associated** — about 75 % more —
and that held both across runs and within a single one.

### 🔑 The scans were never the expensive part

A scan lights the radio for a few hundred milliseconds. The cost is the **join retry**:
`WIFI_RETRY_PERIOD_MS` is 20 s, easing only to 180 s after five failures, and **every failed
attempt leaves the radio associating for up to 30 s** before the quiesce in `loop()` disconnects
it. Out of range that is ~20 attempts an hour x 30 s — an **~17 % duty cycle at full radio
power**, with the scans contributing about 4 seconds of it.

### ✅ One predicate, both cadences

`Networks::inLongDrySpell()` — ten minutes with no association — now eases both:

| | before | after |
|---|---|---|
| join retry | 180 s | **600 s** |
| auto-switch scan | 300 s | **900 s** |
| radio-on time per hour | ~604 s (16.8 %) | **~181 s (5.0 %)** |

**A 3.3x reduction** in radio-on time for a phone that is simply somewhere without WiFi.

⚠ **Both cadences read the SAME predicate on purpose.** This file already learned that lesson —
see the note on `scheduleScanRetry()`, where the due-check and the retry scheduler disagreeing
produced 114 scans in 280 seconds.
⚠ **The deaf-radio fast path is now guarded by it too.** That branch pulls the cadence back to
30 s to confirm-and-cure a genuinely deaf radio, which is right for deafness — but a phone that
has been away from any network for ten minutes is not deaf, and letting it fire would have undone
the easing completely.
✅ **Both escape hatches survive**: connecting clears the spell instantly, and a screen wake still
forces an immediate scan *and* an immediate retry, so picking the phone up is prompt however long
the spell has run.

### 📉 Why this, and not the audio leak

The run that prompted it had **`aud=0/0` in all 225 samples** — no audio leak — and **`ps=1`
throughout, including 83 samples spent searching and 8 with the radio down**. So neither the codec
nor lost modem sleep was involved. It was the disconnected radio, and only that.

⚠ The saving is **estimated from duty cycle, not yet measured on the battery.** `ps=` and the
health log will show it on the next run away from a network.

## 0.9.47 (2026-09-01) - two shared devices that nothing ever switched off

✅ **PROVEN ON HARDWARE THE SAME MORNING.** Nick played a track for six seconds and paused it:
`aud=1/1` (playing) → `aud=1/0` (paused: powered, nothing moving — **the leak**) → held while the
screen was lit → screen slept → **`AUDIO: device was left powered with nothing using it -
released after 30 s`** → `aud=0/0`. Both phones flashed and confirmed on 0.9.47.

Nick: *"yesterday I unplugged wiphone 1 at like 6am-ish and left it mostly idle. at 10am it was
at 12 percent."* That is ~22 %/h against a measured baseline of ~10. Four battery ideas were put
up for assessment; **three of them turned out not to matter, and the two things that did were
found by reading the shared devices rather than the feature list.**

### 🛑 The audio codec, the external amplifier and I2S are left powered, indefinitely

`Audio::shutdown()` is the ONLY function that calls `codec.shutDown()` (Audio.cpp:274),
`i2s_stop()` (:311) and `amplifierEnable(0)` (:308). Every one of its call sites is a SIP call
teardown or a debug easter egg. **There is no idle path**, and on a phone whose proxy never
answers the call teardowns never run at all.

So stopping a track leaves the WM8750 powered — DAC, output driver, VREF/VMID, the left ADC and
the **microphone bias** (WM8750.h `powerUp()` sets POWER1 = VMIDSEL|VREF|AINL|ADCL|MICB
regardless of the mask) — plus I2S running and, conditionally, the separate loudspeaker
amplifier IC. `music_player.cpp` only ever calls `Audio::stopMusic()` = `playbackFile.close()` +
`ceasePlayback()`, and `ceasePlayback()` (Audio.cpp:673-682) touches none of it.

⚠ **PAUSING IS ENOUGH, AND IT IS WHAT NICK DOES.** `MusicApp::~MusicApp()` deliberately does not
stop playback, so "start an album, pause it, pocket the phone" is the ordinary shape.
⚠ **The amplifier is worse than the codec because it is CONDITIONAL.**
`restoreCallVolume()` calls `chooseSpeaker(s_savedLoudspeaker)` — whatever the routing was when
music STARTED (music_player.cpp:43) — and `chooseSpeaker()` no-ops when the value does not change
(Audio.cpp:223). The call screen sets loudspeaker unconditionally (GUI.cpp:5983). **So whether
pausing music switches your amplifier off depends on whether you had opened the dialer since
boot.**

🔑 **AND IT IS INVISIBLE.** After `stopMusic()`, `musicPlayerIsPlaying()` reads false, so the CPU
governor drops to 80 MHz and every instrument the project owns reports a perfectly idle phone.

### 🛑 WiFi modem sleep — one setter, six bypass paths (⚠ but see the correction below)

⚠ **DISPROVEN SAME DAY FOR THE PATH THAT MATTERED.** `wifi bounce` on 0.9.47 held `ps=1` right
through a full radio cycle and never triggered the repair, so `esp_wifi_start()` appears to
PRESERVE power-save on this SDK — contrary to the comment at Networks.cpp:171-172. The code facts
below stand; the consequence drawn from them does not. The invariant and the `ps=` field stay
(they cost nothing and will catch it if it ever does happen), but **the audio leak is the only
PROVEN drain of the two.** The Settings toggle OFF→ON, the Game Boy exit and the uploader stop do
a fuller stop/start and remain untested.


`esp_wifi_set_ps(WIFI_PS_MIN_MODEM)` appears **exactly once in the tree** — Networks.cpp:174,
inside `connectToWiFi()`, which has exactly one caller. The comment there says why it sits after
`begin()`: *"the driver resets the power-save mode when the station starts, so setting it earlier
is silently undone."* Every other path that starts the station never goes near it, and the
comment above it prices that at *"tens of milliamps, forever"*:

- `Networks::bounceRadio()` (Networks.cpp:504) — ⚠ **the AUTOMATIC deaf-radio cure**, so an
  ordinary WiFi blip in a pocket silently doubles idle radio draw for the rest of the boot;
- the Settings WiFi toggle turning back **ON** (GUI.cpp:1839) and the edit screen (GUI.cpp:6632)
  — ⚠ the one control a user reaches for to SAVE power;
- the Game Boy on exit (app_gbc.cpp:328), the ROM uploader on stop (app_gbc_xfer.cpp:1766),
  and the blocking scan path (Networks.cpp:821).

### ✅ The fix, for the class rather than the callers

Both are the same bug: **a shared device whose mode one component sets and another silently
resets.** Fixing each caller fixes that caller; the next one breaks it again, as four previous
faults in this codebase already did. So:

- **An audio idle watchdog** in `loop()`: when nothing is entitled to the device for 30 s
  continuous, `shutdown()` — which clears codec, I2S and amplifier together. Safe because
  `shutdown()` uses `i2s_stop()` and never uninstalls the driver (see `Audio::configureI2S`), so
  release-and-reacquire costs no DMA reallocation.
- **A WiFi power-save invariant**, re-asserted every 5 s regardless of which path restarted the
  station.
- 🎁 The audio watchdog also closes the **hot-mic leak**: `shutdown()` is what clears
  `microphoneOn` / `microphoneStreamOut` / `rtpRemotePort`.
- ⚠ **Both log at `log_e` every time they fire.** A watchdog that cleans up silently is a
  bug-hider; these are instruments that name the leak each time, so the underlying caller can
  still be found.

### 🔎 And the instruments, because none of this was visible

- `aud=%d/%d` in the health line — powered / moving-samples. **`aud=1/0` is the leak signature**,
  and it is the state that reads as a perfectly idle phone in every other field on that line.
- `ps=%d` — 1 = modem sleep, 0 = receiver at full power, -1 = station not up.
- A serial `audio` command: who is entitled to the device, and for how long it has been idle.
  ⚠ Both report the firmware's SHADOW of the chip: the WM8750 is write-only (no `readReg`), so
  the registers cannot be read back. That is why the watchdog runs on a timer rather than
  trusting bookkeeping.

### 🐛 Two bugs the adversarial review found in the watchdog itself, before it ever ran

- **It could never have fired.** The first version used `sipCallActive()`, which counts
  `HangUp`/`HangingUp` — states measured stuck in **1,066 of 1,071** health samples on phone 2.
  Now `sipNeedsFullSpeed()`; a teardown with RTP genuinely flowing is held by `movingSamples()`.
- **It would have crashed the voice recorder.** `recordFromMic()` sets `microphoneRecord` and
  never touches `playback`, so `movingSamples()` read false during a live recording.
  `movingSamples()` now counts recording, and the predicate gained `screenBrightness > 0` as a
  catch-all for UI consumers with no non-sticky flag (the mic meters go through `turnMicOn()`,
  which sets only `microphoneOn` — cleared nowhere but `shutdown()`).

### 🐛 Pre-existing: `ceaseRecording()` freed the buffer and kept the write index

`freeNull(&recordRaw)` left `recordRawW` set, and `saveWavRecord()` does
`if (recordRawW > 0) write(recordRaw, recordRawW * 2)` — a read from address 0 on the next Save
after ANY `shutdown()` during a recording. Reachable from every teardown path; the watchdog would
merely have been the first thing to trigger it reliably. The buffer and its indices now die
together.

### 📋 Measured, and NOT worth doing — the three ideas that were assessed and rejected

- **A "do not relay other nodes" toggle saves nothing.** Measured `MARK mesh-relay`: 25 relays in
  20 h on phone 1, 9 in 18 h on phone 2 = 0.5-1.25/h. At ~0.5 s on air and ~90 mA at PA_BOOST
  +17 dBm that is ~0.016 mAh/h — about 0.01 % of the budget. Worth adding as mesh etiquette,
  never as a battery feature.
- **No Bluetooth is running.** Nothing in the tree calls BluetoothSerial/BLEDevice/NimBLE, and
  arduino-esp32 does not start the controller unless asked. (There IS RAM to reclaim:
  `esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)` is called only when the Game Boy starts.)
- **Turning the SIP account off saves nothing.** `sipNeedsFullSpeed()` excludes every non-call
  state and phone 1 idles at 80 MHz with `sip=1`; with no WiFi there is nothing to poll and
  nothing to re-REGISTER. The woods saving is the WiFi radio, not SIP.

## 0.9.46 (2026-08-29) - book sync stops being a stopwatch

Nick: *"If I sync my place from a device, the wiphone doesn't automatically have the new place.
I instead have to open the book on the receiving wiphone, then on the other device I have to
push sync at that point, then on the receiving wiphone I have to close the book, then reopen
it... I prefer how it works on covey where the 'jump to (device name)'s place?'"*

That sequence was not superstition. It was the **only** sequence that worked, and each of its
three steps was working around a different fault.

### 🛑 Opening the Books app threw away every parked position

`BooksApp`'s constructor called `bookSyncInboxInit()`. The app is `new`ed on entry to Books and
`delete`d on exit (GUI.cpp, `GUI_APP_BOOKS`), so **the parking spot was emptied at the exact
moment the reader was about to look in it.**

The parking spot exists precisely because a LoRa round trip does not fit inside the moment you
press Sync — a position is supposed to be able to arrive while the reader is shut and be waiting
when you open the book. Wiping it on entry inverted that: a position only survived if it landed
with Books *already open*, which is why the book had to be opened **first** and the send had to
come **after**. The inbox is a file-static array that the C runtime zeroes before `setup()`
runs; it never needed initialising, and it must outlive the app that reads it. The call is gone
and there is a 🛑 comment where it was, because it reads like tidy housekeeping.

### 🐛 A position arriving while you were reading was invisible until you closed the book

`checkForPending()` ran on book open and after a passcode edit, and nowhere else. Sitting on a
page while a position landed, nothing happened — hence the close-and-reopen. The reading screen
now watches the inbox on a one-second tick and raises the offer where you are.

The tick costs a **`uint32_t` comparison**: `bookSyncInboxSeq()` moves only when the inbox
actually changes, and the HMAC verify behind `bookSyncInboxFindFor()` runs only then. A mesh
**rebroadcast of a frame already parked deliberately does not move it** — flood routing means we
hear the same frame more than once, and re-raising the card for it would be a nag rather than a
sync. Pressing Sync again on the sender does move it, because that packet carries its own
`turnedAt` and nonce.

Removing an entry moves it too, so acting on one offer makes the reader look again: two devices
can each park a position for the same book, and the second must not stay invisible until the
next open.

### 🐛 Even when it WAS found, nothing said so

The only surfacing of a found position was a row inside `Menu` — "Go to X's place (47%)". A
position could arrive, park, verify, match the open book, and still be invisible to anyone who
did not think to press Menu. Opening a book with a position waiting now goes **straight to the
sync card**, which is COVEY's behaviour and what Nick asked for. Declining lands on the page;
the menu row stays for the passcode-edit path.

### ⚙ The tick, and the shared-state trap it walks past

`msAppTimerEventPeriod` is device-wide and apps routinely leave their own value in it. Rather
than add a thirteenth scattered assignment, `BooksApp::enterState()` now **defaults it to 0 at
the top** and lets the two screens that need a tick override it — so a state added later gets
"no timer" instead of whatever the previous screen wanted. Same bug class as the menu ID, the
I2S channel format and the notification pop's device config.

### What this means in use

Timing no longer matters in any direction:

| when the position arrives | before | now |
|---|---|---|
| Books app shut | discarded on entry | offered when you open the book |
| in the library, book shut | menu row only | offered when you open the book |
| book open, you are reading | invisible until close+reopen | offered within a second |

⚠ Still RAM-only: a reboot discards anything parked. And **you still do not need to tap Sync on
both devices** — *Sync my place* broadcasts, the receiver parks it with no tap.

### 🧪 Tests

`test_inbox` gains 17 assertions covering the change counter in both directions — the quiet
failure here is silent either way (the card never appears, or it re-appears on every
rebroadcast). Suite: **1,161 assertions, 0 failures** across eight suites.

## 0.9.45 (2026-08-29) - the instrument for the residual drift, and six audit fixes

### 📐 The measurement nobody has taken

0.9.43/0.9.44 removed the app-switching ratchet (47 app opens, net `largest` +0) but phone 1
still lost **2,292 bytes over 174 minutes in steps that fall BETWEEN app opens**. The app-open
probe cannot see that by construction and the DROP instrument says a step happened without
saying what caused it. So `heapEvent()` / `heapDelta()` now mark `largest` at the remaining
suspects, into `/health.log`:

- `MARK scan-pre` / `MARK scan-post` — brackets every auto-switch WiFi scan
- `MARK assoc` / `MARK disassoc` — every association change
- `MARK mesh-relay` — every flood rebroadcast, **logged only when `largest` moved ≥256 bytes**
  so a quiet TX costs nothing

⚠ **Loop task only.** `healthLogLine()` opens a file on the SD card, so association is detected
by polling `WiFi.status()` from the loop at 2 s rather than by hooking `processWiFiEvent()` —
that handler runs on the WiFi event task with a 4 KB stack and would race the loop's own SD
access. This is the same discipline that made the scan guard a *call-site* decision.

If the residual is stepwise and lands on these marks, we have it. If `largest` slides smoothly
between them, it is none of these and the next pass must instrument allocation *sites*.

### 🐛 Six from the audit — the ones that were cheap and safe

- 🛑 **Diagnostics ▸ Networks rebooted deterministically with WiFi down.** `nextToPing++` is
  unconditional but `pingedAll` was only set inside the `wifiRssi != 0` branch, so the sequence
  never terminated and `bbPings[i]->setText()` ran past the end of a two-element array. At
  `i==2` that calls setText() on a NULL `this`. One press of DOWN, ~3 s, every time. Index
  bounded; `buff` initialised (it was handed uninitialised to `extStrdup()` on that path).
- 🛑 **`ping_start()` could hang the phone until the battery came out.** The loop was bounded on
  `ping_seq_num`, which is only incremented *after* `mem_malloc()` succeeds — so one failed
  340-byte allocation made it an infinite `delay(1000)` loop with the watchdog set
  print-not-panic. **Contiguous-heap exhaustion arriving as a HANG rather than an abort: no
  reset_reason, no backtrace, nothing in the log.** Now bounded on attempts, and the trailing
  delay after the last ping is gone.
- 🐛 **ParcelApp read and copied the serial buffer after freeing it.** `free()` sat above the
  reads; `setText()` then did `strlen()` on the freed block and allocated *that* length from
  the internal heap. A use-after-free presenting as a random-sized allocation.
- 🐛 **`CriticalFile::backup()` leaked both buffers on every settings-screen exit** — it runs
  from the destructor of every settings app. PSRAM, so not a panic mechanism, but monotone and
  reclaimed only by reboot. `restore()` and `store()` always freed theirs.
- 🐛 **Three sprites leaked their pixel buffers on every open.** `TFT_eSprite` has no destructor
  anywhere in the vendored library. Digital Rain (98 B) and Ackman (340 B) are the stability
  problem, not Recorder's 32 KB: `esp32Calloc()` tries plain `calloc()` first, so the small ones
  come from the **internal** heap and are stranded mid-heap, splitting exactly what `largest`
  measures.
- 🐛 **UART Passthrough: a one-byte heap overwrite, an unchecked malloc, and a divide-by-zero.**
  `data[rxBytes] = 0` wrote `data[1024]` on a full read, one byte into the next block's header —
  silent, with the abort arriving later and elsewhere. The terminator was never needed. The
  buffer is checked and in PSRAM now. And an empty baud field gave `atoi("") == 0`, so
  `uart_set_baudrate()` computed `(UART_CLK_FREQ << 4) / 0` — an immediate Guru Meditation on
  the first OK press.

### Still open, deliberately

The Digital Rain two-writer SPI race (`~ThreadedApp`'s bare `vTaskDelete` plus `showMeshPopup`
drawing from the loop while the demo's task pushes sprites) needs an exit handshake and
hardware to verify. Not attempted blind.

## 0.9.44 (2026-08-29) - four more of the one bug, and 0.9.43 confirmed in real use

### 📏 First: 0.9.43 was measured on the handsets, and the ratchet is gone

Nick used both phones for 1-3 h. Read back from `/health.log`, which survives unplugging:

| | phone 1 (never restarted, 174 min) | phone 2 (2 reboots, a case swap) |
|---|---|---|
| app opens recorded | 26 | 21 |
| **net `largest` across all of them** | **+0 bytes** | **+0 bytes** |
| panics | **0** | **0** (both boots `reset_reason=1`) |
| `largest` start → end | 20,940 → 18,648 | 19,200 → **20,816, ended higher** |
| scan-guard false refusals | 0 (12 scans completed) | 0 |

On 0.9.42 the Dialer cost **−2,308 bytes of `largest` on first open, permanently** (−2,416 /
−2,524 / −2,416 / −1,876). Now it takes −1,712 and **gives every byte back on the next probe**,
and 45 of 47 opens are exactly `+0`.

⚠ **A RESIDUAL DRIFT REMAINS AND IS NOT APP SWITCHING.** Phone 1 still lost 2,292 bytes over
174 min in two steps that fall BETWEEN app opens, which netted zero. Unexplained. ⚠ `min-ever`
free internal reached **4,076 bytes** — still one bad transient from trouble.

### 🛑 Two write-only UDP sockets, opened on the WiFi event task, deleted

`processWiFiEvent()` called `udp.begin()` and `udpRtcp.begin()` on every `STA_GOT_IP`. Each
costs a `new char[1460]` inside `WiFiUDP::begin()`, behind the same **dead null check** as
`parsePacket()` and `_scanDone()` — and it runs on the **WiFi event task**, so unlike
`udpParsePacketSafe()` there is nowhere to put a try/catch. Two 1,460-byte contiguous internal
requests, on the association that follows a scan that has just taken ~3.6 KB.

**Nothing ever read either socket.** Verified across the whole tree: no
read/parsePacket/write/beginPacket/available/stop on either, ever. RTP audio uses its own `rtp`
socket (Audio.cpp:1500); the only `udpRtcp` mentions are in a commented-out block. So this was
~2,920 bytes of internal heap held for the life of the boot, plus an uncatchable throw, in
exchange for nothing. Sockets, externs and the port constant are gone.

The `delay(100)` went with them — its own comment said it existed solely to stop those
`begin()` calls erroring, so it was blocking the shared event task for 100 ms on every
association for the sake of code that no longer exists.

### 🐛 The base64 message decode leaked, every time, forever

`Section::getValueBase64()` had **no `free()`** — the sibling `putValueBase64()` twenty lines
up always had one. Any message whose text is not `isSafeString()` is stored base64, which is
**any message containing a newline, an emoji, a curly quote or an accent**, so opening a
conversation lost a few hundred bytes to ~2 KB of internal heap that never came back. It also
ran unattended: `ingestMirrored()` decodes each newly arrived mirrored SMS. Now freed, checked
(it was an unchecked write through NULL), zero-initialised (the decoder only NUL-terminates
when it decoded > 0 bytes), and taken from PSRAM.

### 🐛 Mesh DB save could write ~248 KB past its buffer

`selectPersisted()` returned 0 without touching the caller's `keep` mask when its own key
allocation failed. `saveDb()` then sized its snapshot from that 0 but still tested `keep[i]` —
uninitialised `ps_malloc` bytes — copying a 248-byte message for every garbage non-zero byte,
into the PSRAM that holds the node table, the book text and every widget. Silent and unbounded.
One `memset`. Latent rather than imminent (PSRAM has ~3.6 MB spare) but it would have been
blamed on anything but this.

### 🐛 Two unchecked mallocs on the SIP receive path

`normalizeLinearSpaces()` and `parseQuotedString()` both allocated and wrote through the result
on the next line, with no NULL test, on every inbound message with a multi-word display name or
an escaped quoted string. Both now bail: the name stays un-normalised / the string stays
escaped, which is cosmetic. ⚠ Latent on these phones today (no proxy) and live the moment SIP
works.

## 0.9.43 (2026-08-29) - the random restart, root-caused and cut off at both ends

Nick asked for a dive on remaining instability. **Every panic ever captured on either phone
(33 MB of serial, two distinct events) is one root cause: contiguous INTERNAL heap running
out, killing whatever allocates next.** Two death shapes, both observed — `abort()` via
`std::terminate`, and `Guru Meditation (LoadProhibited, EXCVADDR 0x20)` when
`_Unwind_RaiseException` itself faults on a null unwind context. In this build a throwing
allocation is a device-killer either way.

Two changes: one removes the **cause**, the other removes the **trigger**.

### 🔑 The cause: font glyph metrics were eating the internal heap, permanently

`SmoothFont::loadFont()` did **seven plain `malloc()`s per font face** (TFT_eSPI.cpp, both
the SPIFFS and the array variant). Plain `malloc` is INTERNAL heap on this build — there is
no PSRAM diversion at any size. Eleven faces × 7 = **77 permanently-held scattered internal
blocks, ~11.5-15.5 KB**, on a phone with ~20 KB of contiguous internal heap in total.

- ⚠ **Loaded LAZILY, at DRAW time.** `FontCollection::operator[]` builds each face on first
  use and caches it for the life of the boot, and the first use is a widget drawing — i.e.
  *after* `enterApp()` returns. **That is why the app-open probe reads `+0` while `largest`
  falls: the probe is blind to font loading by construction.** Worth knowing before trusting
  that instrument again.
- It explains the shape exactly: `largest` ratchets while you visit new screens and then
  PLATEAUS, because there are only eleven faces. Measured 2026-08-29 on one binary: run 1
  lost 7,380 bytes in 11 minutes of navigation then sat flat for 130; run 2, never touched,
  held 15,500 for 8.9 hours.
- Now `fontMetricAlloc()` → PSRAM, internal fallback. The arrays are read during text layout
  on the loop task — no ISR, no DMA, no `IRAM_ATTR` — so external RAM is safe for them.
- 🐛 **And the seven were used UNCHECKED**, so a font loaded on an exhausted heap wrote
  through NULL — a LoadProhibited blamed on whatever screen happened to open. Checked now.

📏 **MEASURED, phone 1, same `heap` command on a young boot:**

| | 0.9.42 | 0.9.43 | |
|---|---|---|---|
| free | 19,388 | 23,216 | **+3,828** |
| largest | 19,208 | 20,940 | **+1,732** |
| min-ever | 10,312 | 15,948 | **+5,636** ← the worst moment of the run |
| blocks | 404/15 | 374/18 | **−30** ← the metric arrays, gone from internal |

⚠ **NOT YET PROVEN: that this removes the RATCHET.** That needs the phone driven through
screens, and both phones lock after 40 s, so serial key injection is swallowed
(`lock: locked=yes`). Two attempts to test it reported "apps opened: 0" — flat numbers there
mean the test never ran, not that the fix worked. **It needs a thumb.**

### 🎯 The trigger: the periodic WiFi scan was the allocation that actually killed it

`WiFiScanClass::_scanDone()` does `new wifi_ap_record_t[n]` behind `if(!_scanResult)` — the
same DEAD null check as the `new char[1460]` in `WiFiUDP::parsePacket()` that
`udpParsePacketSafe()` already guards. ⚠ **And this one cannot be caught at the call site:
`_scanDone()` runs on the WiFi event task. The only lever is deciding not to scan.**

📏 **MEASURED, phone 1, three scans off the DROP instrument: ~192 bytes of contiguous
internal heap per access point** (n=19 → −3,648; n=19 → −3,648; n=18 → −3,536). Deliberately
not `sizeof(wifi_ap_record_t)` (~80) — the driver allocates alongside the array, and the
number that predicts the crash is the total effect on `largest`.

**The crash, exactly:** the phone died with `largest = 3,072` and `[autosw] scan started` as
the last line in the log. 19 APs needed 3,648. There were 3,072.

`wifiScanMemoryOk()` now gates all four scan sites, sizing its estimate from the last scan's
AP count so it adapts to the actual air. Checked against every heap state seen in the wild:
no false refusals at 19,208 / 15,500 / 11,016 / 9,512; refuses at 3,072. ⚠ It is a
PREDICTION, not a guarantee — the count comes from the previous scan and the allocation
happens later on another task. It turns certain death into unlikely, which is the same trade
`udpParsePacketSafe()` makes and the best available.

### Also measured, not yet fixed

- **The phone freezes for the whole LoRa airtime, every transmit.** `MeshPhy::send()` polls
  TxDone with `delay(1)`. 205 loop stalls in 9.9 h idle, median 649 ms, max 2,285 ms (= the
  2,000 ms TX timeout). Causally demonstrated: **6/6 deliberate transmissions produced a
  stall, 1/6 matched idle windows did** (p≈0.008). Airtime theory predicts 575 ms for a
  51-byte SF11/250k packet. ⚠ Most of these are **unlogged flood REBROADCASTS** — only ~31 of
  the 154 long stalls have a matching announce line: the phone freezes its own UI to relay
  other people's packets.
- ⚠ **A third instance of the same throwing-allocation bug is still open:**
  `processWiFiEvent()` calls `udp.begin()` twice on `STA_GOT_IP`, and `WiFiUDP::begin()` does
  `tx_buffer = new char[1460]` with the same dead null check — on the WiFi event task, on
  every reconnect, moments after a scan has just taken 3.6 KB.

## 0.9.42 (2026-08-28) - conversations know who they are talking to

Two things reported from use, both of them the same complaint: the Messages app knew the
phone number and expected you to know the person.

- **Chats, threads and thread lines are labelled with the CONTACT NAME** where the phonebook
  has one, falling back to the address exactly as before where it does not. Nick: "I don't
  have each contact member number memorized, I have to keep clicking the conversation to see
  who is who."
  - ⚠ **Matched on the grouping IDENTITY, never on the string.** The same person reaches the
    message store as `14257604281@seattle1.voip.ms`, `+14257604281` or a bare `4257604281`
    depending on which way the message came in, and the phonebook holds a fourth spelling
    again. `sipThreadIdentity()` — the normaliser the threads are already grouped by — is the
    one rule, so a name can never attach to a row the identity says is somebody else. Both
    the `s` and `l` fields are checked, since a LoRa-only contact has no SIP address at all.
  - ⚠ **The phonebook is loaded ONCE, in the app's constructor**, beside the message store.
    A lazy load at the first label would have put a SPIFFS read inside a menu redraw, and
    SPIFFS on this board takes its stalls in one indivisible piece. An unloaded phonebook
    just means no names, which is what the old code showed anyway.
  - ⚠ The open thread's label is **copied** into `MessagesApp::threadLabel`, not pointed at:
    `HeaderWidget::setTitle()` keeps the pointer rather than the string, and the name it
    would point at lives in the phonebook INI that the picker is free to rewrite. It is also
    one phonebook scan instead of one per rendered message.
- **New Message → Choose → someone you have already texted now OPENS that conversation**
  instead of starting a second, parallel history with the same person. Nick: "think how phone
  texting apps should work."
  - The composer reads the thread snapshot `buildChats()` already built — no rebuild, no
    message-store scan — and exits handing the peer back only when a thread actually exists.
    A genuinely new correspondent is unchanged: the address is filled in and you type.
  - ⚠ **Only with an EMPTY message body.** Choose is reachable after typing (move focus back
    up to "To:" and press OK), and somebody who has already written the message meant to
    send it, not to go read history.
  - Back out of the thread lands on that person's row in the chats list, not at the top.
- 🐛 **And picking a recipient no longer bins a half-written message.** Found while writing
  the guard above: `CreateMessageApp::setupUI()` rebuilds every widget on the screen — that
  is how the pick fills "To:" in — and the fresh body widget came back empty. So "type the
  message, then choose who it goes to", which is a perfectly ordinary order to do it in,
  silently threw the message away. The body is now carried across.

## 0.9.41 (2026-08-28) - predictive text in the note page

- **Note Page predicts.** Prose, like a message body — the other place you write sentences
  rather than an address or a key.
- 🔑 **And predictive text can no longer outlive the app that claimed it.** `t9Field` was set
  by the field that wanted T9 and otherwise cleared only by `setInputState()`, which nothing
  calls on the way out of an app — so it stayed set on menus with no text field at all,
  swallowing digit keys and stopping UP/DOWN/BACK from navigating. Now cleared at the two
  doors every app leaves by, beside the existing `clearTextFocus()`. This is the general fix
  for a bug already patched twice in one app; an app that wants T9 re-claims it when it
  builds its field.
- ✅ Verified on hardware: the note page reports `opted in: yes` and predicts "Hello";
  leaving it reports `opted in: no`.

## 0.9.40 (2026-08-28) - predictive text in the SIP message body

Reported from use: the SIP composer had no T9.

- The message body now predicts; the **"To:" address deliberately does not** — it is a SIP
  URI, and a dictionary has no business in it.
- ⚠ **This screen is the first with two text fields wanting opposite things**, and `t9Field`
  is one flag on ControlState, so it has to follow FOCUS rather than be set once per screen.
  Synced by the app after the focus move settles, NOT pushed from the widget:
  `FocusableApp::setFocus()` calls `setFocus()` on every widget in array order and each one
  resets the input state, so a widget positioned after the target would clear a flag the
  target had just set. Doing it in the app is immune to that ordering; doing it in the widget
  would have worked here by luck and broken on the next screen with three fields.
- ✅ Verified on hardware: `To:` reports `opted in: no`, DOWN to the body reports
  `opted in: yes`, and 4-3-5-5-6 predicts "Hello".

## 0.9.39 (2026-08-28) - the rest of the review

Ten more findings from the same adversarial pass, triaged and fixed. Nothing here was
reported by using the phone; all of it came from reading.

**Input path**
- **Both hold gestures fired while the phone was LOCKED or the screen asleep.** `uiKeyDown`
  is set by the keypad scan whether or not the press reached the app, and neither hold was
  gated. Holding `0` on a locked draft queued a backspace and a `0` straight into the text
  field — the last character you really typed was deleted and replaced, invisibly, behind the
  lock screen. Both now gated the way the F2 music hold already was.
- **One `#` press destroyed a pending automatic capital.** `t9Commit` cleared `t9Caps`
  unconditionally, and `#` called it on every press — so looking at the modes and coming back
  gave "hello" rather than "Hello". Only a commit that actually committed something consumes
  the capital now.
- **A mode change silently ate a half-typed multi-tap letter.** `#` cleared `inputCurKey`
  without emitting it, while the comment three lines above claimed the opposite. Both this
  and the `*` drain now share one helper.
- **The `#` hold was a toggle, not an arm** — and in Abc/ABC it landed on a flag those modes
  never read, making it a 500 ms no-op that also stepped the mode back. It arms now, and maps
  onto `inputShift` where that is the flag in play.
- **A held digit never redrew the footer**, so the strip contradicted the engine until the
  next keypress.
- **`t9LiteralDigit` was the one emit path that never called `t9NoteChar`** — hold a digit,
  then type, and the next word came out capitalised mid-line.

**Dictionary**
- **Slurs were in the table at ranks 4,600-6,500** and would have been offered as
  predictions. Blocked, along with subtitle plumbing (`subtitles`, `sync`, `corrected`,
  `ripped`). Ordinary profanity stays — people type it and the phone is not a chaperone.
- **Contractions inherited their base word's rank and each family was inserted in reverse**,
  so `4-3` offered "I'd" before "he" and `4-6` offered "I'm" before "in". They have their own
  rank band now, ordered by their own frequency.
- **`y'all` and `o'clock` were being dropped entirely** — no usable base word, so the old
  fallback appended them past position 46,000 and the cut discarded both.
- **The documented SD path was not the path the firmware reads.** The usage line said
  `/t9-extra.txt`; the code opens `/t9/extra.txt`. Following the instructions produced zero
  extra words.

✅ Verified on hardware: cycling all four modes then typing gives "Hello"; `#` mid-letter now
emits the letter. `43` gives "he" first and `46` gives "in" first. Host test 71 checks.

⚠ Known and NOT fixed: common first names outrank ordinary words on some keys (`sam` over
`ran`, `jack` over `lack`, `mike` over `mile`), and they are stored lowercase so they are
wrong even when they are what you wanted. Removing names is worse than the problem;
demoting them properly needs a name list and a rank penalty, which is a change worth making
deliberately rather than in a batch like this one.

## 0.9.38 (2026-08-28) - a test that knows English, and the words the corpus was too old to have

The 0.9.37 review found "cat" missing from the dictionary while all 68 host checks passed.
This closes that gap and the vocabulary hole it exposed.

- 🔑 **The host test now asserts what the dictionary must CONTAIN**, not merely that it is
  self-consistent. 166 hand-written everyday words — the commonest hundred, a deliberate
  block of two- and three-letter words, things people text about, the contractions, and the
  phone vocabulary — each checked as present AND as actually typable. Hand-written on
  purpose: a list derived from the corpus that builds the table could never disagree with it.
- **Plus a blunt instrument that needs no English:** how many of the 512 three-digit keys
  match at least one word. 111 when cat was missing, 373 now, against a ceiling of 433 for
  the whole 50,000-word corpus. The floor is 350.
- ✅ **The test was proven against the bug it was written for.** Rebuilding the old dictionary
  and running it gives 3 failures naming cat, sat, mat, tip, gym, pop, pub, map and usb, and
  reports coverage collapsed to 111 of 512. It would have stopped this shipping.
- **Words a 2018 subtitle corpus is too old to rank.** wifi, bluetooth, usb, username, login,
  logout, browser, screenshot, emoji, smartphone, hotspot and friends were all absent from
  the table — film dialogue rarely says them. ⚠ They are PROMOTED, not added: most were in
  the corpus already, just far below the 25,000 cut ("wifi" sat at rank 32,756), so the first
  attempt skipped them as "already present" and changed nothing at all.
- Deliberately NOT added: meshtastic, lora, repeater. Project vocabulary belongs in the SD
  extra dictionary, which is exactly what that design is for — baking one project's jargon
  into everybody's firmware is the thing it exists to avoid.

## 0.9.37 (2026-08-28) - four T9 bugs found by review, three of them mine from today

An adversarial read of the finished feature, run while the phone was being used. Every one
of these was confirmed on hardware before it was touched.

- 🔑 **"cat" was not in the dictionary.** The rank cutoff that correctly removes two-letter
  initialisms (tv, dr, pm, cd) was applied at THREE letters as well, which left 125 of 1,115
  three-letter words and made **401 of the 512 three-digit keys match nothing at all**: cat,
  bat, cop, bus, cup, art, cry, sat, mat, tip, gym, pop, pub were all untypable. The
  generator's own worked example — `228 -> cat, bat, act` — described a table containing
  neither cat nor bat. `--short-len` now defaults to 2. Costs 1.1% of unambiguous keys and
  one on the worst run; the initialisms that come back (bff, omg, fbi, dna) are mostly things
  people type anyway.
- **Punctuation landed after the space it preceded.** Every T9 path returns before the
  multi-tap machine, which is the only code that commits an armed '*' selection — so typing
  a word, '*', then '0' gave "Hello ." instead of "Hello. ", and if the screen changed before
  the 2 s idle timeout the full stop was lost outright. It also left inputCurKey set, which
  blanks the prediction strip. The commit is now hoisted to where T9 can reach it, and the
  space moves to r2 so the symbol still goes first.
- **A held digit jumped ahead of the word it interrupted.** t9LiteralDigit popped the held
  digit off the pending word but never committed what remained, so 4-3-5-5-6 then holding 7
  produced "7hello". Anyone typing mp3, covid19 or route66 would have hit it.
- **Predictive text outlived the screen that owned it.** t9Field is set by buildCompose and
  cleared only by setInputState, which leaving compose does not call — so T9 stayed live on
  the thread list, swallowing digits and stopping UP/DOWN/BACK from navigating.
  MeshtasticApp::enterState now clears it; Compose re-claims it.
- ✅ All four verified on phone 1: `228` gives "Act 1/4" and DOWN gives "Cat 2/4"; a word then
  '*' then '0' gives "Hello. "; leaving Compose reports `opted in: no`.

⚠ Worth recording how these were found. The engine's 68 host checks passed throughout — they
test t9.cpp, and three of these four live in the input path above it. The one bug the host
test could have caught (the missing three-letter words) it did not, because it only asserts
that words IN the table are reachable, never that words a user expects are in it.

## 0.9.36 (2026-08-28) - bring your own T9 words

An optional second dictionary, read from `/t9/extra.txt` on the SD card at boot. Its words
are always offered AFTER the built-in English ones, which is exactly what you want from
jargon: reachable when you need it, never in the way.

- 🔑 **It is a file on the card, not part of the firmware.** One person's vocabulary has no
  business in a stranger's phone, and keeping it off the image means the licence of whatever
  was harvested is never a question for anybody else. It also means adding words costs a file
  copy rather than a reflash, and it generalises — callsigns, place names, a team roster.
- **PSRAM, read exactly once, at boot.** ~105 KB for 8,700 words (a 70 KB blob plus a 35 KB
  pointer array, two allocations for the whole dictionary — the words are never copied
  individually). Measured on hardware: internal heap unchanged at 18,736 free. A filesystem
  read per keystroke is the freeze this firmware has fought before, which is why the built-in
  table is in flash and this one is read once.
- **Every failure is silent and named.** No card, no file, a malformed file, no PSRAM: the
  table stays empty and predictive text works exactly as it does without it. `t9` on the
  console says which of those happened, because a dictionary that quietly failed to load
  looks identical to one with none of your words in it.
- **`up on t9`** starts the WiFi uploader pointed at `/t9`, and **`t9 reload`** picks up the
  new file without a reboot — a word list is a thing you iterate on.
- New tools: `fetch_sarna_titles.py` (page TITLES via the MediaWiki API — never article text;
  the prose is GNU FDL 1.2 and a vocabulary list is not a copy of it) and `gen_t9_extra.py`,
  which takes any one-per-line list, drops what the built-in dictionary already has, and
  sorts the rest for the phone's binary search.
- ⚠ The useful knob is `--min-titles`. Words appearing in exactly ONE title across a 93,000
  article wiki are where both the junk and the cost live: at 2 you get 8,753 words for 68 KB
  and the worst candidate run stays at 9, where the full tail takes it to 11.
- ✅ Proven on phone 1 end to end: 93,649 titles harvested, 8,753 words uploaded over WiFi in
  0.6 s, `Kerensky` predicted and auto-capitalised — while `4663` still gives `good` as
  candidate #1, now 1 of 8 instead of 1 of 7.

## 0.9.35 (2026-08-28) - T9 predictive text

Five keypresses for "hello" instead of thirteen. Type d-o-n-t and get "don't". Off by
default nowhere: it is on, with a Settings row to turn it off, and '#' steps out of it
mid-sentence without a trip to a menu.

- 🔑 **25,000 words in flash, ZERO bytes of heap.** The table is `static const` in
  memory-mapped `.rodata`, so a lookup is a plain pointer dereference and performs no
  allocation at all — which is the entire safety argument on a phone whose internal heap
  has been measured at 2,276 bytes free at its worst, and where a failed `new` throws with
  nothing to catch it. 293 KB of flash (8.7% of the free DROM window), +64 bytes of static
  RAM, and a worst-case lookup of 25 binary-search probes.
- **Sorted by keypad digit key, then by frequency.** Neither the key nor the frequency is
  stored: the key is recomputed from each word, and the rank IS the position in the run. So
  "most likely first" and "press DOWN for the next" both fall out of an index, for free.
  91.9% of digit keys match exactly one word; the worst collision is 9.
- **UP/DOWN cycle candidates, but ONLY while a word is pending** — with nothing pending they
  still move focus and the cursor, exactly as before. Same rule the OK key already followed:
  consume a key only in the state where it means something new. '*' and '#' were left alone
  for the same reason; '#' is the only way to type a capital.
- **'#' cycles T9 -> Abc -> ABC -> 123.** A superset of the shift toggle it used to be, so
  nothing was taken away. 123 exists because "testing 1 2 3" was otherwise untypable.
- **Hold a number key to type the number**, the gesture every keypad phone had. The short
  press has already happened when the hold fires — keys are edge-triggered, and holding
  every digit back until release would put half a second of lag on ordinary typing — so it
  is REVERSED instead: the digit comes back out of the pending word. Holding '0' also
  retracts the space it already typed.
- **Capitals both ways:** automatically for the first word and after `.` `!` `?` plus a
  space, and by hand with a long press on '#' for a name in the middle of a sentence.
- **The pending word lives in the FOOTER, not inline in the text field**, and is inserted
  only on commit. That single decision is why this touched no widget code and why the digit
  buffer cannot drift out of step with what is on screen.
- **Per-field, default OFF, and message bodies are the only field that opts in.** `InputType`
  cannot decide it — the WiFi password field is `InputType::AlphaNum` too — so the other
  twenty-odd text fields keep today's behaviour without anyone having to remember them.
- The dictionary took three rounds, each written into `tools/gen_t9_dict.py` so it is not
  re-learned: `/usr/share/dict/web2` is the wrong KIND of list (headwords only — it rejects
  every plural and does not contain "has", while it does contain "ba" and "aa"); the corpus
  tokenises on the apostrophe so every contraction had to be repaired; and 279 two-letter
  initialisms were removed by a rank cutoff rather than a curated list, because a real short
  word is by definition frequent.
- ✅ Both handsets. Verified end to end over the serial bridge and by hand: prediction,
  candidate cycling, mode switching, held digits, held '#', auto-capitals, the Settings row
  and its persistence across a reboot.
- Source: hermitdave/FrequencyWords (OpenSubtitles), MIT. Only an ordered list of ordinary
  English words is compiled in — no counts, no n-grams, no source text. The 612 KB corpus is
  not committed; `tools/t9-corpus/FETCH.md` has the URL and the hash the generated header
  records.

## 0.9.34 (2026-08-28) - two latent panics on the typing path

Found while scoping predictive text; none of it is T9, all of it is live today.

- `MAX_INPUT_SEQUENCE` was 18, making `inputSeq[]` 19 bytes, but the `*` symbols row is
  `.,!?@$/+-=%^ _:;'*#` - nineteen characters, so `strcpy` (GUI.cpp:1068) wrote twenty.
  Every press of `*` on a text screen overflowed by one, landing harmlessly in the padding
  before the next field. Harmless only by luck, and anything added beside `inputSeq` would
  have been silently zeroed on every press.
- `TextInputWidget::insertCharacter` and `PasswordInputWidget::insertCharacter` both called
  `allocateMore()`, ignored its bool, and then wrote `inputStringDyn[0]` - a NULL store, and
  a StoreProhibited panic, one failed realloc away on a heap measured at 2,276 bytes free at
  its worst. Both now refuse the character instead, which callers already handle.
- `TextInputBase::allocateMore` grew the text buffer with plain `realloc` on the internal
  heap, doubling each time, ceiling 64,000 bytes for some callers. Now `extRealloc`, matching
  `MultilineTextWidget::allocateMore`. `PasswordInputWidget` deliberately stays internal:
  those buffers are tiny, and a secret is better off out of external PSRAM.

## 0.9.33 (2026-08-28) — the Nodes screen fits in memory again

Opening **Nodes** rebooted phone 1. The node database was never the problem — it has
lived in PSRAM since it existed. **The menu rows did not.**

- 🔑 **`MenuOption` was the one hole in the PSRAM rule.** `AbstractWidget::operator new`
  routes every widget to PSRAM, but `MenuOption` does not derive from it, so each row was
  a plain `new` plus a `strdup`'d title and subtitle — **three internal-heap allocations
  per row**, on the ~20 KB heap. `newMenu()` in app_meshtastic.cpp had already written
  this down in passing; nothing had acted on it. `MenuOption` now has the same
  `operator new`/`operator delete` pair, and both strings use `extStrdup`. One definition
  covers `MenuOptionIconned`, `MenuOptionIconnedTimed` and `MenuOptionPhonebook`, so
  **every list screen** benefits, not just Nodes.
- **Why it started now:** the cost is data-driven. `sizeof(MenuOptionIconned)` is 36 B and
  a row measures **~124 B** across its three blocks. Phone 1's database has grown to
  **163 nodes** — 20.2 KB — requested from an internal heap the phone's own `heap` command
  reported at 14.5–17.4 KB free with a largest block of 7.0–13.8 KB. It could not fit, and
  `operator new` throws on failure with nothing catching it: `std::terminate` → `abort` →
  `reset_reason=4`. The database crossed the threshold (~125–135 rows) and the screen
  stopped opening.
- 🛑 **Do not "fix" this by catching `std::bad_alloc`.** This phone has a decoded panic
  where the throw itself was fatal — `_Unwind_RaiseException` faulted with `LoadProhibited`
  before any handler could run (the SIP/UDP abort, helpers.h). The allocation has to not
  fail.
- **`MenuWidget::deleteAll()` leaked every row it owned.** It called `options.clear()`,
  which frees only the `LinearArray` pointer block — the `MenuOption` objects, their
  strings and their two `IconRle3`s were never deleted. `~MenuWidget` had always done it
  correctly; `deleteAll()` simply never adopted it. Its one caller is the SIP accounts
  VIEWING screen, so this leaked three iconned rows per account opened, for the life of
  the boot.
- **Apps go to PSRAM at the root now.** `BooksApp` and `SipAccountsApp` had each grown
  their own `operator new` after panicking; the other ~40 apps allocated from the internal
  heap only because nobody had panicked on them yet. The pair moved to `WiPhoneApp` — the
  same "fix it at the root, not per-app" argument `AbstractWidget` makes. The two per-app
  copies are left in place, redundant but harmless, because each carries the measurement
  that justified it.
- **Widget-lifetime text follows its widget.** `ButtonWidget`, `LabelWidget`,
  `MultilineTextWidget` and `MenuWidget`'s own strings were still `strdup`'d internally
  while the objects holding them sat in PSRAM. Also finishes a **partial conversion**:
  `MultilineTextWidget` already used `extStrndup`/`extRealloc` for its wrapped `rowsDyn`
  rows, but the insert-character `realloc` and the split/merge `malloc`/`strdup` paths
  were missed and quietly pulled edited rows back onto the internal heap. `registeredWidgets`
  moved from `LA_INTERNAL_RAM` to `LA_EXTERNAL_RAM` for the same reason — every widget it
  indexes is already external.
- ✅ **Proven on phone 1 over the serial bridge, no hands.** Opening Nodes with 163 nodes:
  internal heap **11,384 → 12,512 bytes free** (it goes UP — the previous menu is freed),
  PSRAM **3,600,696 → 3,580,524** (−20,172 B, ~124 B/row). No panic, no reboot, list
  renders with starred nodes on top. Three further open/close cycles: internal recovers to
  17,856, PSRAM flat — no leak. Before the fix the same screen asked ~20 KB of a heap that
  had ~15 KB.
- ⚠ **Left deliberately undone.** `buildNodes`, `buildThread` (up to `MESH_MSG_CAP` = 1000
  rows), `PhonebookApp::createLoadMenu` and `SipAccountsApp::createLoadMenu` are still the
  only four uncapped list builds in the firmware — every other list caps and says so
  (Files 200, Photos 200, Books 48, Music 96, Chats 24, Places 8). Capping Nodes would hide
  nodes from the user, which is a product decision, not a memory one; with the rows in
  PSRAM it is no longer a crash. `IconRle3` is also still a plain `new` on the internal
  heap, two per iconned row, and it lives in vendored `src/TFT_eSPI/`.

## 0.9.30 (2026-08-27) — peeking at the WiFi list no longer kills auto-rejoin

The 0.9.29 session's P2 wart, closed the same day, with two relatives found in the walk:

- 🔑 **The peek wart:** `NetworksApp`'s constructor clears `reconnect` (so a background
  rejoin cannot race the user's choice while the list is open) — but leaving WITHOUT
  joining left it cleared, so merely LOOKING at the WiFi list silently killed auto-rejoin
  (and the radio, and the driver's remembered AP) until the next join or reboot. The
  destructor now restores it (`Networks::resumeReconnect()`), gated on `userDisabled()`.
- **`disable()` says so in the LIVE flag:** every `disable()` caller is an explicit user
  action (menu WIFI-OFF, Remove, Disconnect, edit-screen WIFI-OFF), but only
  `loadPreferred()` ever set `_userDisabled` — from the INI, typically at boot — so a
  mid-session Disconnect left the live flag stale-false. `disable()` sets it now;
  a deliberate `connectTo()` clears it (the join path already writes `disabled=false`
  to the INI in the same breath).
- **The WIFI-ON toggles hand control back:** both ON paths only called
  `esp_wifi_start()` and left `reconnect` false — an innocent OFF→ON cycle killed
  auto-rejoin until a join or reboot. Both now call `resumeReconnect()`.
- ✅ All three scenarios proven live on phone 1 over the serial bridge, no hands:
  peek & back out → `rec` 0→1, scan, `switching to 'NickH-wifi'`, `conn=1`, SIP
  re-registered; explicit OFF + peek + back out → stays off (wifi=255, 50 s silent);
  OFF→ON toggle → `ud=0 rec=1`, hands-free rejoin + SIP REGISTERED.

## 0.9.29 (2026-08-27) — the deaf-scan state is diagnosed, and the phone cures it itself

Phone 1 idle-dropped WiFi and would not rejoin until a manual rescan — and the whole
mechanism was caught live at the work desk with phone 2 as the on-air control:

- 🔑 **The deaf-scan state.** After hours of disconnected retry churn the ESP32 WiFi driver
  reaches a state where **every scan completes with zero results** — async and blocking,
  80 and 240 MHz, screen on or off — while the twin phone hears the same AP at −50 dBm.
  The auto-switcher believed each empty scan ("out of range") and backed off forever.
  Nothing recovers it except a **radio off/on** — which is why "open the WiFi screen and
  rescan" always fixed it: `NetworksApp`'s constructor calls `disconnect(true, true)`, an
  accidental radio bounce (a reboot cures it the same way: a fresh radio heard 6 networks
  and SIP registered inside a minute).
- **The fix:** the auto-switcher now counts consecutive empty disconnected scans and, at
  two, **restarts the radio itself before the next scan** (`_dryScans`, Networks.cpp) —
  logs `[autosw] N consecutive empty scans: restarting the radio`. Deliberately without
  `eraseap`: the driver's remembered AP is not the disease.
- **New bench command `wifi bounce`** (`Networks::bounceRadio()`): the same cure on demand,
  no reboot. ⚠ Its first cut called `WiFi.disconnect(true)` directly from an associated
  phone — radio-off swallowed the STA_DISCONNECTED event, `connected` stayed true, and
  both rescue loops sat gated: wedged half-down until a reflash. The shipped version
  delivers a plain disconnect first (and sets the flag explicitly), then cycles the radio,
  then makes the next auto-switch scan due immediately. Proven end to end: bounce →
  scan n=1 at −36 dBm → rejoined in 9 s → SIP re-registered, hands-free.
- ⚠ Honest limit: the `_dryScans` trigger itself cannot be fabricated at a desk with the
  AP present, so its first live firing will be the next natural idle-drop — watch the
  field log for the line above.

## 0.9.28 (2026-08-27) — the upload page stops eating the phone, and WiFi rejoins by itself

### 🔑 The uploader: the page moves to the raw server, and the browser stops losing

The chunked transfer engine (0.9.2x era: one TCP connection, CRC-checked pieces, stop-and-wait)
was already solid — **what was broken was everything around it.** The page, the favicon, and the
probe still rode the framework WebServer, whose request machinery costs a **~10 KB
internal-heap transient per connection** on a phone that idles at 12–15 KB largest with SIP up.
Measured: TWO loads of the 829-byte page drove the largest free block 12,688 → 3,180 and
tripped the low-heap breaker, which frees the server with escalating backoff (90 → 600 s) —
so nearly every browser VISIT read "site cannot be loaded". No pacing fixes a per-request cost
that starts below the danger line.

So the raw server is now the front door: it listens on **:80** (and :8081 for the existing
tools) and serves the page itself for ~zero heap, keep-alive. The WebServer moved to **:8080**
as the legacy path — multipart `/upload` for `curl -F` and no-JS browsers, `/fetch`'s HTML
replies, `/log` (`http://<ip>/log` 302-redirects there; use `curl -L`). Its breaker was
reworked to match: instant trip below 3 KB stays (that is PHY-abort altitude), the 6 KB line
now requires the pressure to HOLD for 1.5 s (a single request's transient decays in under a
second and is not a flood), raw traffic inhibits trips entirely, and resume is
**recovery-only at ≥ 10 KB** — the timer-resume kept re-planting a ~6 KB server into a heap
that could not hold it, which is the measured allocate/free churn leak. mDNS now begins once
per boot and never ends: this core's `MDNS.end()` leaks a WiFi-event-callback entry per cycle,
and the breaker used to cycle it on every pause.

**Measured after:** page loads went from 2-of-10 answered to 19–20 of 20 under deliberate
hammering; three consecutive 4-book batches (~52 MB) landed 12-for-12 byte-verified at
48–69 KB/s with zero breaker events, zero panics, zero wedges. New: `tools/wiphone_send.py` —
one command from a computer, resumes partial files, retries transient SD 507s, byte-verifies
every file. ⚠ Honest note: the test card threw three 507 write errors across twelve files
(all recovered by retry) — watch that card.

### 🔑 The first REAL-browser run caught a bug every bench tool was blind to

The acceptance bar always ended with "then a real browser", and this is why: the raw-served
page set its piece size but left `BASE=''`, so the sender's `if(BASE)` routed it into the
MULTIPART branch — which the raw server refuses (413: multipart overhead pushes a 16 KB piece
past the cap). Every bench pusher speaks the raw protocol directly and structurally could not
see it. Found in the first minute of driving the page in a real Chrome over an Android
hotspot; fixed (`RAW = BASE || window.RAWPAGE`); re-proven: 2.8 MB in three files through the
page's own JS, byte-verified, an empty file skipped and said so.

### 🛑 WiFi auto-rejoin was a ONE-WAY FLAG, and the WiFi settings screen was the finger on it

`Networks::reconnect` — the flag that permits any automatic rejoining — was set true in the
constructor, false in `disconnect()`/`disable()`, and restored **nowhere**. The settings
screen's SAVE path calls `disconnect()` before rejoining, and that call is
`WiFi.disconnect(true, true)`: radio OFF and the driver's stored network ERASED. So **editing
or saving any WiFi network left that phone unable to auto-rejoin until reboot** — which is why
phone 1 (the phone whose networks actually get edited) spent months "losing WiFi" while an
identical phone 2 never did, and why it once sat 78 minutes next to a hotspot it could join in
two seconds by hand. Fix: a deliberate join (`connectTo()` — the settings screen, the
preferred-network retry, and the auto-switch hop all flow through it) restores the flag.
Proven the same hour: fresh boot associated hands-free, and `wifi drop` rejoined itself in
~2 s — the exact test that had just failed. ⚠ Two related warts seen and deliberately left:
removing a network and the Disconnect button both call `disable()` (radio full-off), heavier
than either action means.

### 🛑 RETRACTED, 30 minutes after being written: "phone 1's radio is hardware-dead"

The deaf readings were real — `wifi scan` 0/0/-2 on fresh boots while a neighbouring phone
heard the hotspot at -60 dBm — but the verdict was wrong, and the flaw was the CONTROL: the
"healthy" phone differed in WiFi STATE (associated), not just in radio, and scans issued while
the connect-retry machinery is mid-attempt (or the radio is off — see the flag above) read
empty or fail outright. Nick joined the hotspot from the phone's own UI in seconds; the same
serial scan then listed six networks. The wrong verdict stands struck-through in the handoff
as a specimen. Kept from the hunt, both useful: **`wifi calreset`** (erase the RF calibration
in NVS, reboot, recalibrate) and **`wifi restore`** (`esp_wifi_restore()` — factory-reset the
WiFi driver's stored state; ⚠ forgets the last-used network).

## 0.9.27 (2026-08-26) — `wifi scan`, and a retraction

- 🛑 **RETRACTED: "the idle downclock is what kills the WiFi association" (commit `3535a85`).**
  It rested on a 2-vs-2 A/B of an intermittent fault. With the full data set the claim does not
  survive: on a **standard router the same phone is 9 for 9 clean at 80 MHz** (eight 180-second
  boot-and-watch trials plus one 30-minute run), the downclock firing every time and
  `empty_scans=0` throughout. **Both failures happened on an iPhone personal hotspot**, which
  stops advertising when it is not actively serving a client — so `scan done: n=0` meant the
  SSID was not on the air, and the phone was right. **No CPU frequency change is being made.**
- 🛠 **NEW: `wifi scan` on the console** — a blocking scan listing what the radio can actually
  hear, with the RSSI and channel of each network. `n=0` has two completely different causes —
  a deaf radio or an absent AP — and nothing on the phone could tell them apart. That ambiguity
  is what sent a whole afternoon after the wrong suspect, so the fix is an instrument, not a
  guess. **Verified on the handset:** `wifi scan` -> `20 network(s)`, with `SmithWifi` at
  -60 dBm at the top of the list.
- ⚠ **Recorded in the handoff: three times today the INSTRUMENT was the fault, not the phone.**
  `grep` silently suppressing matches in a binary-ish capture; every serial port open RESETTING
  the phone and restarting the very association race being measured; and the Mac entering Idle
  Sleep mid-capture (confirmed in `pmset -g log`), which read exactly like a 16-minute firmware
  wedge. Long captures now run under `caffeinate -i -s`.

## 0.9.26 (2026-08-26) — two more that had never run, from the same sweep

### 🛑 The SIP re-init after a network-loss teardown has never once run

`WiPhone.ino:3202` was `gui.state.sipState == CallState::NotInited;` — **a comparison whose
result is discarded, where an assignment was meant.** The `if` block's only effect was clearing
the flag on the next line, so the re-init test below it fired only when `sipState` happened to
be `NotInited` already, or the account had changed. Present since the initial commit;
`platformio.ini` strips `-Werror`, so GCC's unused-value warning could never fail the build.

Written through `setSipState()` rather than as a raw `=`: every other transition in this
firmware goes through it, and it owns the ring-armed and missed-call bookkeeping, which a raw
assignment would silently bypass.

⚠ **THIS TURNS ON A PATH WITH NO HISTORY, AND IT IS THE ONE CHANGE HERE THAT IS NOT PROVEN.**
Bench it: with a real account, drop WiFi mid-call, restore it, and watch for `SIP is going to
init` followed by registration returning rather than flapping. If it misbehaves, that one line
is the whole change.

### Music never paused for a second call

The pause was edge-triggered — `if (callBusy && !wasCallBusy)` over a static. But
`sipCallActive()` counts `HangUp` and `HangingUp` as a call, and this repo has **measured 19+
consecutive minutes parked at `sip=6`** with the proxy unreachable. While parked there the latch
stays true, so an incoming INVITE moves `HangUp -> BeingInvited` — also true — and **there is no
rising edge.** The codec/I2S single-occupancy that the pause exists for was never honoured.

⚠ Fixed at the caller, NOT in `sipCallActive()`: that predicate is shared, and narrowing a shared
predicate to suit one caller is the bug class that produced four of the seven faults in the
2026-08-15 audit. It is idempotent now — `if (sipCallActive() && musicPlayerIsPlaying())` — so it
cannot latch and cannot repeat, and nothing resumes afterwards, as before.

## 0.9.25 (2026-08-26) — a sweep for unswept fixes, and it found five

A multi-agent hunt for the two shapes that cost a working day earlier today: **a fix applied
where it was noticed and never swept**, and **a comment that outlived its change**. Every
finding below was re-verified by hand against the source before anything was touched.

### 🛑 1. Every timestamp this phone put on the air was eight hours wrong

`clock.h:77` — `getExactUnixTime()` is `getExactUtcTime() + timeOffsetSeconds`, and
`WiPhone.ino:1479` loads `[time] zone` at boot and applies it (shipped config: `zone=-8`).
So that call returns the **LOCAL-shifted** epoch.

That was swept in exactly one place. `meshtastic_service.cpp` already carried the note proving
somebody measured it — *"getExactUtcTime, emphatically not getExactUnixTime … stamping it here
put every replayed record 7 hours off"* — on the replay path. **Seven sibling sites still used
the shifted clock, every one of them stamping or comparing a value that meets COVEY's real UTC:**

| site | what it broke |
|---|---|
| the **GPS position beacon** | COVEY reads the Position's `time` field verbatim and renders `time.time() - ts`. **Every WiPhone position on COVEY's map read eight hours stale.** In the woods that is "he moved twenty minutes ago" versus "that fix is from this morning." |
| booksync `turnedAt` (×3) | `bookSyncSuspectClock()` is `turnedAt > now + 300`. Against the shifted clock, **anything COVEY turned in the last ~7.9 h was painted "(their clock looks wrong)"** — the warning was about our own arithmetic. And this phone's page looked 8 h old to COVEY. |
| waypoint expiry (×2) | `expire` is COVEY's UTC. A pin set to die in a day died a day **and eight hours** later. |
| SMS-mirror freshness | the comment says "Ten minutes of slack"; the arithmetic made it **eight hours ten minutes**, so a full resync buzzed for everything under 8 h. |

⚠ **CORRECTED 2026-08-26, and the number matters.** The tables above said "eight hours" from
the shipped `data/configs.ini` (`zone=-8`). **MEASURED on the live phone** — `sun 47.6062,-122.3321`
→ `2026-08-26 at 'given' (local, UTC-7:00)` — the offset in force is **UTC−7 (PDT)**, so the real
shift on Nick's two phones is **SEVEN hours**. A freshly-flashed phone taking the shipped config
would see eight. Read "eight hours" above as "seven on these phones, eight on a stock one".

🔑 **AND THAT SETTLES THE "7.7-HOUR-OLD POSITION" of 2026-08-25.** At a 7.0 h shift, a position
that had *just* been taken would arrive reading 7.0 h old. It read **7.7 h** — so the fix really
was about 42 minutes stale, which is exactly what a phone indoors with `sats in view: 0` should
look like. **The earlier session's conclusion was right about the cause and wrong about the
magnitude: roughly seven of those 7.7 hours were this bug, and the remaining ~42 minutes were
the sky.** Both halves needed fixing and only one had been.
Left alone deliberately, all local-only: the SIP message store's load stamp, `myPinAtUnix`
(never transmitted), and the two places that compute the offset *on purpose*.

### 🛑 2. Use-after-free on two live SIP teardown paths

`wifiTerminateCall()` and `rtpSilent()` each `delete` every dialog, `dialogs.clear()`, and then
write `currentCall->terminated = 1;` — **through a pointer into what they just freed.**
`currentCall` is only ever assigned from a `findCreateDialog()` result, and that function puts
every dialog it returns into `dialogs`. It was **never set back to NULL anywhere in the file**,
and `sip` is a file-scope global — so `isBusy()` went on reading `->terminated`, `->confirmed`
and `->early` out of freed memory **on every superloop pass, for the rest of the session.**
Both callers are ordinary: a WiFi drop mid-call, and an RTP silence timeout.
The sibling `terminateCall()` opens with `if (!currentCall) … return TINY_SIP_ERR;` — it got it
right and was not copied. Now: marked terminated *before* the delete, pointer dropped after,
and the destructor drops it too.

### 🛑 3. A received message could walk off a 256-byte stack buffer

The channel-URL parser clamped with `(i + l <= (size_t)len)`. That is 32-bit arithmetic and it
**wraps**: a length varint of `0xFFFFFFFE` with `i == 8` makes `i + l == 6`, which passes, and
the walker then runs to `0xFFFFFFFE`. Every other protobuf walker in the repo clamps by
subtraction (`if (l > len - i)`), which cannot wrap; this one receive-side walker missed it —
and "Apply link" hands a received message's own text straight to it.
Fixed by clamping the safe way in both the outer and inner loops, and advancing the cursors by
the **clamped** length. **Verified on the handset**: the actual overflow payload
(`chan Cv7___8P`) now returns "0 channel(s) added", channels intact, no crash.

### 4. The GPS quality gate guarded the radio and not your own screen

`meshPosFixUsable()` had exactly one caller: the **transmit** path, under a measured note that a
`sats=3 hdop=6.4` fix was **20 km wrong**. `resolveReference()` never called it. So the phone
refused to put a bad fix on the air and then computed every distance and bearing **on your
screen** from it — against its own written argument that "a confident wrong answer, in the woods,
is worse than no answer". The gate is now on both reference branches; a bad fix falls through to
the manual pin.

### 5. Screen settings was a third reader of the lock/dim/sleep keys, with the old defaults

0.9.16/0.9.17 fixed the boot reader and the shipped config and called it "all three now agree".
**`GUI.cpp`'s Screen-settings app is a fourth party and was never swept** — seven sites falling
back to `0` (and `dim_level` to 100 against boot's 15). Save writes the widget values back, so
**opening Screen settings on a config missing a key and pressing Save persisted `dimming=0`,
`sleeping=0`, `lock_keyboard=0`** — and the lock rides on sleep, so that silently took the lock
with it. Also `data/configs.ini` still shipped `lock_keyboard=0`, so any `pio run -t uploadfs`
handed out a phone that never locks; `INTERNAL_FLASH.txt` documents it as `1`.

### 6. The guard written to stop the key-0 bug recurring missed three of its four spellings

The `git grep` added this morning would **not have caught the bug it was written for**. Checked
against the real historical lines: it misses a **named constant** (`ROW_INERT = 0`, which is
exactly what Photos had), a **wrapped call** (the `Channel 'booksync': MISSING` row had its
`, 0, 1);` on a continuation line), and the **two-argument form** (`style` defaults to 1).
Replaced with `tests/check_menu_keys.py`, which joins continuations, accepts both terminators,
and resolves an identifier key against enum/`#define`/`const` zeros in the same file. Proven
against all three previously-missed spellings, with no false positives, tree clean.

### 7. Two more copies of the SIP-flap myth, in the file 0.9.21 was written to purge

0.9.21 corrected `tinySIP.h:624` and MEMORY.md **by name and never grepped `docs/HANDOFF.md`** —
so that release is itself an instance of the bug it was fixing. Two copies survived, and the
worse one is headed **"READ BEFORE RE-TRYING"** and concludes *"THE LESSON: THE FLAP WAS
LOAD-BEARING"* — an instruction to a future session not to re-apply a fix that is shipped,
measured and working. Both marked superseded.

### Found and deliberately NOT shipped

- **`WiPhone.ino:3202` is `gui.state.sipState == CallState::NotInited;`** — a comparison whose
  result is discarded where an assignment was meant, present since the initial commit, so the
  SIP re-init after a network-loss teardown **has never once run**. One character. ⚠ Changing it
  turns on a path with no history at all, and that wants a real account and a real call to
  bench, with Nick present.
- **Music never pauses for a second call.** The pause is edge-triggered on `sipCallActive()`,
  which counts `HangUp` — measured at 19+ minutes in this repo — so while parked there an
  incoming INVITE produces no rising edge and the codec is never released.
## 0.9.22 (2026-08-26) — the rename field had no way to delete a character

The last screen in the Photos app that nobody had ever driven, driven at last — and it shipped
with a hole, exactly as 0.9.18's note said to assume.

- 🛑 **`PHOTOS_RENAME` tested `LOGIC_BUTTON_BACK(event)` BEFORE the event could reach the text
  widget**, and on the 21-button keyboard that macro is `BACK || END` (`Hardware.h:213`). So
  **Back never got to the field, and Back is backspace.** The screen opens prefilled with the
  current filename, so the only edit possible was to APPEND: you could turn `BT06.JPG` into
  `BT06.JPGxyz` and nothing else. Renaming a photo was, in practice, impossible.
- **MEASURED on the handset before the fix:** four Back presses walked four screens *out of the
  app* instead of deleting four characters, while `key 7` and `key 2 2` typed `p` and `b`
  perfectly well. The field worked; only the erase was missing.
- ⚠ **Every other text screen in this firmware already had it right and said so** —
  `app_books.cpp:1944`, `app_meshtastic.cpp:1372/:1396/:1419`, with the convention written out
  at `app_meshtastic.cpp:198`: *cancel is END, because Back is backspace in a text field.*
  **Photos was the single exception in the whole firmware.** Fixed to match them rather than to
  invent a fourth spelling — the rule at `Hardware.h:216` is that every consumer declares what
  it needs.

### The whole rename path, exercised end to end on the handset

Every step photographed, and the card finished byte-for-byte as it started:

| step | result |
|---|---|
| open Rename | field prefilled `BT06.JPG`, cursor at the end, Save/Back softkeys |
| type | `key 7` → `p`, `key 2 2` → `b`; multi-tap candidate strip renders (`tuv8`) |
| `#` | toggles case — strip switches to `ABC2` |
| `*` | opens the punctuation picker (`.!?@$/+-=%^_.,;'*#`) |
| 8× Back | erases all eight characters, placeholder "New name" returns, **screen does not exit** |
| Save with no extension | refused, and **says so**: "Keep a .jpg/.jpeg/.bmp ending" |
| rename to `BT06.JPEG` | **"Renamed to BT06.JPEG"**, list updates, 13 photos |
| rename back to `BT06.JPG` | **"Renamed to BT06.JPG"**, list restored |

🔑 **Both of those result lines are messages that could not render at all before 0.9.18** — they
go through the row that `addOption(..., 0, ...)` was silently dropping. This is the first time
anyone has seen a Photos rename report its own outcome, success or refusal.

## 0.9.21 (2026-08-26) — the SIP flap really was fixed; two things near it were not

Nick: *"I think the sip flap is fixed, I remember a session a bit back about it… but let's check."*
**He is right, and it is now measured rather than remembered.**

### The flap: measured, not inferred

330 s of console on phone 1 (the phone that actually holds the account), WiFi up throughout:

```
SIP REGISTER -> sending (registered=0 everRegistered=0)     <- the initial one
SIP REGISTRATION -> REGISTERED                              <- fired ONCE, at boot
SIP REGISTER -> sending (registered=1 everRegistered=1)     x7 refreshes, ~41 s apart
```

**8 REGISTERs, one state change, zero "lost".** Before 3d9f329 the sequence was
REGISTERED → lost → REGISTERED once per refresh, each one firing REGISTRATION_UPDATE_EVENT, a
redraw and a 240 MHz boost. Both halves of that fix are present and doing their job:
`requestRegister()` no longer clears the flag, and `REGISTER_PERIOD_MS` 45 s sits under the 60 s
expiry.

🛑 **What made this worth re-checking was not the code — it was two stale English sentences.**
`tinySIP.h:624` still said *"THE REAL FIX, deliberately NOT shipped tonight"*, sitting directly
above a paragraph describing that same fix as in place; and MEMORY.md said the real fix was
"left for a session with Nick present". Both were written on 08-22 and were obsolete by 08-24.
**They cost a session's work to disprove.** Both corrected. A comment that outlives its own
change is worse than no comment.

### 🛑 `rtpSilent()` was asserting SIP registration from a media timeout

`TinySIP::rtpSilent()` — reached when the OTHER PARTY'S RTP stream goes quiet
(`Audio.cpp` raises `RTP_SILENT_ON`, `WiPhone.ino:3186` calls it) — ended with
`TinySIP::registered = true;`. It reads like a copy of `wifiTerminateCall()` just above it with
the boolean flipped; that one sets `false` and has a reason to.

**What it cost:** `registered` drives `controlState.sipRegistered`, and CallApp's END/BACK
handler branches on it (`GUI.cpp:5575`, `:5592`) — not registered exits the app, registered
takes the hang-up path. So after an RTP silence timeout on an **un**registered phone, the flag
was forced true and END stopped being the one press that leaves the call screen. Bounded only
because `registrationInvalid()` also checks the expiry clock, so the lie corrected itself within
`REGISTER_EXPIRATION_S`. The assignment is gone; registration state is not this function's
business.

### 🔋 The WiFi scan backoff was defeated by its own retry line

Found while watching the console for the flap, and it is a battery bug.

`autoSwitchTick()` has a careful backoff — scan every 2 min while disconnected, easing to 5 min
"once it is clearly not a brief blip", written specifically so *"a phone carried out of range
all afternoon"* does not pay for scan after scan. An empty scan then set
`_msLastScan = now - AUTO_SCAN_PERIOD_MS + AUTO_SCAN_RETRY_MS`.

🔑 **That expression is only correct while CONNECTED.** It means "570 s ago", and the due-check
compares against `AUTO_SCAN_PERIOD_MS` *only when connected*; disconnected it compares against
120 s (or 300 s). A stamp 570 s in the past is already older than either, so the scan was due
**immediately** and the next tick started another one.

**MEASURED on phone 1 with the access point gone: 114 scans in 280 seconds — one every ~2.5 s,
against a design of one every two minutes.** Each scan lights the radio for a few hundred ms.
⚠ **And an empty scan IS the out-of-range case**, so the one situation the backoff was written
for was the one situation it never applied to.

Fixed by splitting the two outcomes, which the old code ran together:
- `n == 0` — a completed scan that found nothing. That is out-of-range; stamp it as an ordinary
  scan and let the 2/5-minute easing run.
- `n < 0` — the scan was **aborted** (a reconnect cycled WiFi under it). It learned nothing, so
  retry soon — via `scheduleScanRetry()`, which computes the stamp against whichever period the
  due-check will actually use. `currentDiscPeriod()` is now the single definition of that
  period, because the due-check and the retry scheduler disagreeing is the whole bug.

✅ **VERIFIED ON HARDWARE 2026-08-26**, same phone, same cable, hotspot switched off by hand:

| | scans while disconnected | cadence |
|---|---|---|
| before (0.9.20) | **114** in ~280 s | one every ~2.5 s |
| after (0.9.26) | **3** in 420 s | one every ~2 min — the designed rate |

`sinceScan` now reads `0s` after a scan completes where it read `570s` before, which is the
stamp the whole bug was about. **About a 38x cut in radio-on time when the phone is out of
range.** And it still comes back at once: with the hotspot restored the phone reported
`wifi: up` and `registered: yes` on the next boot, so the slower cadence costs nothing on
return — the reconnect loop is a separate path from the scan.

## 0.9.20 (2026-08-26) — the rest of the menus were dropping their rows too

0.9.18 fixed `addOption(text, 0, ...)` in Photos. **It was never swept.** Eight more call sites
were still passing 0 in Books, Files and Music, so those rows have never once appeared:

| where | what nobody has ever seen |
|---|---|
| `app_books.cpp:525,528` | "(no books yet - add some)", and *why* a book would not open |
| `app_books.cpp:1512` | what the last booksync send did, good or bad |
| `app_books.cpp:1657,1659` | "Channel 'booksync': **MISSING**", "Parked positions: N" |
| `app_files.cpp:156,182` | the result of the last file operation; "(more files not listed)" |
| `app_music.cpp:89,93` | "(no music yet)", and *why* a track would not decode |

⚠ `app_files.cpp:156` carried the comment **`key 0 = inert`** — the exact false belief 0.9.18
was written to kill, still sitting one file over. That is what an unswept fix looks like.

- **`MenuWidget::addNote()`** now exists so there is one correct way to add a display-only row
  and the wrong way has something to point at. The refusal message names it:
  `menu option key is 0 - row DROPPED, not added. For a display-only row use addNote()`.
- 🛑 **A NOTE ROW IS SELECTABLE, AND A RANGE TEST WILL EAT IT.** `MENU_ROW_NOTE` is
  `0xFFFFFF01`, and screens that test `sel >= ROW_FIRST` compute `(int)(sel - ROW_FIRST)` —
  which **underflows to a NEGATIVE index and sails straight through `idx < entryCount`**, then
  reads off the front of the array. Files, Books and Music each reject `MENU_ROW_NOTE`
  explicitly *before* their range test. This is why the note key is not simply "some number
  above the action base".
- **A source guard in `tests/run_tests.sh`** fails the suite on any new
  `addOption(..., 0, ...)`. A grep and not a unit test on purpose: the wrong spelling compiles,
  links, runs and silently shows nothing, so there is no return value to assert on.
- **Verified on the handset**: marking a file for copy in Files now shows
  **"Copy marked - open a folder an.."** as the first row — a line that had never rendered.
  OK pressed on it three times: nothing happens, no crash, screen unchanged.

### Chat history: the round trip that 0.9.19 still owed

With Nick's go-ahead, one text on the private hunt-group channel, end to end:

- phone 1 `send 2 persist-check 0826` -> `MESH RECEIPT: 'persist-check 0826' -> in mesh`;
  msgs 26 -> 27 and `/meshdb.bin` on the card 9272 -> 9680 bytes.
- **phone 1 rebooted: the message is still in the thread.**
- **phone 2 received it over the air and it survived phone 2's reboot too** — `loaded from SD
  at boot`, 1 msg, its card file 1704 -> 2032 bytes. That is the incoming direction, which is
  the one that matters: phone 2 is the phone that had been losing everything.

## 0.9.19 (2026-08-26) — the phone read the chat history from one filesystem and wrote it to the other

Nick: *"after rebooting I lose the chat history in meshtastic chats on the wiphone's."* He was
right, and the cause is not in the message code at all.

- 🛑 **`meshFs()` ANSWERED "NO CARD" FOR THE WHOLE OF `setup()`.** It reads a file-static
  (`s_meshCardIn`) that was assigned in exactly one place — `MeshtasticService::loop()` — and
  `loadDb()` / `loadFavourites()` run in `setup()`, before any loop pass. So at load time the
  answer was always the initial `false` and both read **SPIFFS**, while every save from the
  first loop pass onward wrote the **SD card**. The history went into a file nothing ever
  opened, and each boot then saved the stale RAM state back over the card.
- ⚠ **The fallback looked like a fallback and was the same file twice.** `loadDb()` opened
  `meshFs()` and then "fell back" to `SPIFFS` — two spellings of SPIFFS. The card's database
  was unreachable at boot no matter what was in it.
- **MEASURED ON BOTH HANDSETS 2026-08-26, over the cable, before anything was changed:**
  `/meshdb.bin` on phone 1's card was **9272 bytes** and on phone 2's **6352 bytes** — about
  26 and about 14 messages — while phone 2's chat list came up with **no messages in any of
  its five conversations** and phone 1 showed three. The two numbers side by side are the
  whole diagnosis, and neither was obtainable before `meshdb` existed.
- 🛑 **AND EACH REBOOT DESTROYED A LITTLE MORE.** The save is unconditional: it writes what is
  in RAM over the card. On phone 2, which loaded nothing, three diagnostic boots took the
  card's database from 6352 bytes to 1704 — **20 nodes and zero messages.** Phone 2's stored
  history is gone; it was overwritten before the cause was found. Phone 1 survived only
  because its SPIFFS copy happened to be nearly as fresh as its card copy.
- **Fixed** by making `setCardPresent()` the single writer of that flag and calling it from
  `WiPhone.ino` **before** `meshService.setup()`, and by naming SD and SPIFFS explicitly in
  both load paths so a future reordering cannot silently re-create the fault.
- **Also fixed the same way: the starred-node list.** `loadFavourites()` had the identical
  two-spellings-of-SPIFFS fallback, so a star set by hand was written to the card and read
  back from flash.

### How much history now survives, and why those numbers

Nick asked for "a certain amount of the most recent messages of each chat… your call how many".

- New `MESH_PERSIST_*` caps decide what is **written**, separately from what RAM holds:
  **40 per conversation and 200 overall on SD**, **12 and 60 on SPIFFS**. The split is not
  arbitrary — `bench` measures SD at 48–57 ms for an 8 KB image against SPIFFS's 2599–2845 ms,
  about fifty times slower, and a save blocks the keypad, the screen and the WiFi stack
  together. 200 messages is ~48 KB, a fraction of a second on a card.
- Scrollback **within a session** is unchanged (`MESH_MAX_PER_CHAT` is still 150). These caps
  are the floor on what a reboot gives back.
- The rule is "newest first, per conversation", in `mesh_retain.cpp` — pure arithmetic with no
  Arduino headers, so `tests/test_retain.cpp` exercises it on the host (15 checks, including
  that a quiet channel is not starved by a busy one, and that a DM with node 3 is not the same
  thread as channel 3).
- ⚠ **The conversation key here is NOT `chatKeyOf()`**, which folds every broadcast into 0.
  Using it would have put LongFast, hunt-group, booksync and smsmirror in one bucket sharing a
  single allowance.

### The instrument this needed, which did not exist

- **`meshdb` on the console** prints which filesystem is in use, which one `loadDb()` actually
  read at boot, what is in RAM, what the next save will keep, and **the size of `/meshdb.bin`
  on both filesystems**. The fault was invisible from every angle without it: the normal load
  path logged at `log_i`, which this build's log level drops, so the one line that would have
  shown the phone reading the wrong file never reached the cable people were watching. That
  line is now `log_e` and names the filesystem.
- **Verified on the handset**, not just in a test: phone 1 boots with `loaded from SD at boot`
  and 26 messages where the old build loaded 3 into LongFast; a node starred over the cable
  survived a reboot; and `meshdb` reports the same 26 across successive reboots.

## 0.9.18 (2026-08-25) — every message the Photos app ever tried to show was silently dropped

Found while deleting a file at Nick's request, which is the first time anyone had watched the
app react to an action.

- 🛑 **`ROW_INERT = 0`, AND `MenuWidget::addOption()` REFUSES A KEY OF 0.** It logs
  `menu option key is 0` and adds no row at all. So the Photos list's result line — the one
  thing that tells you what just happened — **has never once appeared on the screen**:
  "Deleted", "Renamed to X", "Wallpaper set", "Not set - <reason>", "X is locked - unlock it
  first", "Too big: N KB", and the "list truncated" warning. All of it written, none of it
  shown. The constant read perfectly (`key 0 = not selectable`) and did the opposite of what
  it said.
- **Proven three ways before it was touched:** a successful delete redrew the list with no
  message at all (screenshotted); the code path in `GUI.cpp:13253` rejects the key outright;
  and the handset logged exactly one `[E] addOption(): menu option key is 0` at the moment the
  list was rebuilt with a note set.
- ⚠ **THIS QUIETLY UNDID THIS MORNING'S 0.9.13 WALLPAPER WORK.** `setAsWallpaper()` was
  rewritten then to stop claiming success and instead report what the loader actually said —
  and the line it reports through is this one. It was only ever visible over the serial cable,
  **which is exactly why the gap survived being "verified"**. A fix checked through one channel
  is not checked.
- **Fixed** by giving the two display-only rows real keys (`ROW_NOTE`, `ROW_TRUNCATED`, both
  `ROW_ACTION + n`). Nothing acts on them: the list handler matches `ROW_RESTORE_WALL` or
  `k <= entryCount` (max 200), so OK on either does nothing — which is the behaviour "inert"
  was reaching for. The widget is untouched; other menus may rely on it rejecting 0.
  **Verified on the handset: "Wallpaper set" now renders as the first row.**
- **Also done, at Nick's request:** the 0-byte `/photos/20260823_093939.jpg` was deleted
  through the app's own UI — which incidentally exercised the delete confirmation for the first
  time (`Cancel` is first and selected by default, so a mis-timed OK cannot destroy anything).
  A before/after listing confirms exactly one file went and the other thirteen are byte-
  identical.

## 0.9.17 (2026-08-25) — a beacon slot is owed, not spent; and a fix has to be worth believing

Nick: *"I want to make sure the GPS locations are actually reporting and being sent over the
mesh."* They were — COVEY held a real, full-precision position from WiPhone 2 — but the last one
was **7.7 hours old**, because the phone was indoors with zero satellites in view. Two things
came out of looking at why.

- 🔑 **THE SLOT IS NOW OWED, NOT SPENT.** The beacon interval is 300 s and
  `MESH_POS_TX_FRESH_MS` is 30 s, and the old code sent-or-lost the slot in the same instant.
  With a solid fix that is fine — the fix refreshes every second, so every tick sends. But
  **under canopy, where the fix comes and goes, the odds it happens to be under 30 s old at the
  exact instant a 5-minute tick lands are poor** — so a phone that had a perfectly good fix
  forty seconds ago said nothing for another five minutes. That is the hunt scenario exactly.
  Now the slot stays owed and fires the moment a fresh fix arrives.
  ⚠ **The freshness rule is NOT relaxed** — nothing older than 30 s is ever transmitted, which
  was the entire point of it. "At most this often" replaces "only at these instants".
  ⚠ **One slot maximum.** `posDue` is a flag, not a counter, and the deadline keeps advancing
  while it is set, so an hour with no sky owes exactly one beacon rather than twelve.
  **Verified on the handset:** with no fix, `pos` reports nothing owed at t+0 and
  `A SLOT IS OWED - it will send the moment a fresh fix arrives` after one full interval.
- 🛑 **NEW GATE: A FIX HAS TO BE WORTH BELIEVING, and this one is measured.** On 2026-08-25 an
  indoor WiPhone reported `sats=3 hdop=6.4` at **47.33821,-122.16501** while the phone was in
  fact at **47.4965,-122.3749 — about 20 km away** — and nothing on the beacon path would have
  refused it. Had that landed on a tick, COVEY's map would have shown Nick 20 km from where he
  was, with no hint anything was wrong. **Three satellites is a 2D fix**: it solves lat/lon by
  *assuming* an altitude, and a wrong assumption pushes the error sideways, into the one number
  a hunting party reads. Four is the arithmetic minimum for a real 3D fix, so `MESH_POS_MIN_SATS
  = 4` is the standard bar and not a strict one — under canopy a working receiver tracks five to
  ten. `MESH_POS_MAX_HDOP_X10 = 100` is a backstop.
  ⚠ **Refuses only what it positively knows is bad**: both fields are −1 until a GGA supplies
  them, and unknown is *not* failure — refusing on silence would break any receiver that does
  not emit GGA, a different bug than the one being fixed.
  🔑 It lives in `mesh_pos.cpp` as the pure `meshPosFixUsable()`, beside the movement rule, so
  `tests/test_pos.cpp` proves it — including the measured 20 km fix as a named check.
- ⚠ **`lock_keyboard` now defaults to 1 when the key is absent**, matching the missing-*section*
  branch that always did. Two configs that both said nothing about locking used to give opposite
  answers. Same disagreement the 2026-08-22 battery audit found in `[screen]`, where it shipped
  a phone that never dimmed and never slept. Not the cause of the 0.9.16 lock bug — the landmine
  beside it.
- **An empty photo now says it is empty.** `/photos/20260823_093939.jpg` on phone 1 is 0 bytes —
  a died-half-way upload — and Photos said "Could not read this file", which sends the reader
  looking for a corrupt card. It is still listed and still deletable on purpose: COVEY's gallery
  learned the hard way that the pictures you cannot open are exactly the ones you cannot get rid
  of.

⚠ **Only WiPhone 2 has this build** — WiPhone 1 was unplugged and is still on 0.9.16. The GPS
half is irrelevant to it, but the lock default and the empty-photo message are not (the 0-byte
photo is on phone 1).

## 0.9.16 (2026-08-25) — the screen lock was being thrown away by the first button press

Nick: *"whiphone two doesn't like its screen to lock ever (even when set to do so in screen
config). phone 1 works like normal… I just don't want to be accidentally pushing buttons while
it is in a bag."*

- 🔑 **THE PHONE WAS LOCKING. THE FIRST KEY PRESS WAS THROWING THE LOCK AWAY.** `GUI::inCall()`
  answered "yes, we are in a call" for **any** SIP state that was not `NotInited`, `Idle` or
  `Error` — which includes `HangUp`, `HangingUp`, `HungUp` and `Decline`, none of which is a
  call. Its only caller is the unlock path, which reads it as *"a call is coming in, let any key
  answer it"* and clears `locked` on the spot.
- 🛑 **ONE PRESS OF END UNLOCKS A LOCKED PHONE — reproduced on BOTH phones.** END sets `HangUp`
  from anywhere with no call needed, and it does so **before the same keypress reaches the
  unlock check** — so the press that makes `inCall()` true is the press that gets the free
  unlock. END is the red key. A bag presses it.
- 🛑 **And on a phone with a SIP account, EVERY key unlocks for as long as SIP sits in
  teardown.** Those states *stick* when the proxy is unreachable, and this repo had already
  measured it twice: **19 minutes** (2026-08-15, car log) and **SIX HOURS** (2026-08-20) at
  `sip=6`. Reproduced here by dropping WiFi and pressing END. **That is the whole phone-2
  -versus-phone-1 difference: phone 2 has a SIP account loaded and registered, phone 1 has
  none** — so phone 1 only ever had the single-END-press window, and phone 2 spent real time
  with no working lock at all.
- **Fixed:** `inCall()` now means a genuinely live or ringing call — `InvitingCallee`,
  `InvitedCallee`, `RemoteRinging`, `Call`, `BeingInvited`, `Accept`. ⚠ **Answering a ringing
  phone on any key still works**, because `BeingInvited` is in the set; that is the case the
  shortcut existed for. What is gone is the free unlock for hanging up, having hung up, and
  declining.
- ⚠ **THIS IS THE THIRD GUARD IN THIS CODEBASE TO NEED THE SAME CORRECTION**, and the set used
  is deliberately identical to the two that were already fixed — `sipNeedsFullSpeed()` ("includes
  HangUp/HangingUp: teardown, and they stick") and the WiFi auto-switch gate. A fourth spelling
  of "in a call" is how they drift apart.
- **Proven before and after, on both phones, with the same script:** idle 80 s → `locked=yes`;
  press END → **was `locked=no`, is now `locked=yes`**; `OK` then `*` → `locked=no`. The
  legitimate two-key unlock is untouched.
- 🔬 **NEW: `lock` on the console** — the setting, the sleep gate it depends on, the SIP state,
  and what is actually on the card. It prints the sleep gate because **the lock is not its own
  timer**: it fires inside `SCREEN_SLEEP_EVENT`, so a phone that dims but never sleeps never
  locks either, and the dimming makes the timeout look like it is working. Two different causes,
  one symptom.
- ⚠ **Noticed while measuring, not fixed:** `lock_keyboard` defaults to **0** when a `[lock]`
  section exists without the key, while a missing *section* defaults to 1. That is the same
  disagreement that shipped a phone which never dimmed and never slept (2026-08-22 battery
  audit). Both phones here have `lock_keyboard=1`, so it was not the cause — but it is still
  loaded.

## 0.9.15 (2026-08-25) — the clock face gets the same treatment, and the phone can name its own build

- **The clock face now scrims its text**, at Nick's choice when asked. `00:00`, the date /
  network line, a missed-call line and the softkey label each get a plate sized to the words —
  **not a panel across the screen**, because covering the middle of the idle screen would
  defeat the point of choosing a picture at all.
- 🛑 **ONE PLATE FOR THE CLOCK AND THE LINE UNDER IT, NOT ONE EACH.** The date sits directly
  beneath the clock and their bands overlap by a pixel or two — and two translucent plates over
  the same pixel **blend twice**, painting a visibly darker stripe exactly where the eye lands.
  Both strings are measured first, one union rect is laid down, then the text is drawn.
  Verified by reading the screenshot back row by row: the plate is smooth from y=68 to y=182
  with no dark seam.
- **Measured on the photograph, not asserted:** median plate luminance behind the network line
  is **105 → 5.47:1** against white text, and behind the softkey label **79 → 8.21:1**. Both
  clear the 4.5:1 floor on the brightest wallpaper on the bench.
- Incidentally simplified: the known-time and unknown-time branches used to duplicate the
  drawing. They now build two strings and share one drawing path.
- 🔬 **NEW: `ver` on the console** — firmware version and build timestamp of the binary actually
  running. ⚠ It exists because **two different builds wore 0.9.14** during this session before
  anyone noticed; `d9ed477` warned about exactly that ("a different binary must not wear
  0.9.8's number") and the phone had no way to be asked. Now it does.

## 0.9.14 (2026-08-25) — menu text you can read on any wallpaper, and a cable that can press keys

Nick, straight after the wallpaper fix landed: *"the menus have no contrast and make it
difficult to see the text when a picture is behind them. can the menu items have a translucent
grey background so I can see the words no matter what background I pick?"*

- 🔑 **THIS GOT WORSE THE MOMENT THE WALLPAPER STARTED WORKING, and that is not a coincidence.**
  Until 0.9.13 every phone was showing the same near-black SPIFFS texture, which white text
  happens to sit on perfectly. The main menu is the one menu built non-opaque — it deliberately
  lets the wallpaper through — so the instant a real photograph got behind it, four of the five
  rows became unreadable.
- **Fixed with a scrim**, which is the only answer that holds for a picture the phone is not
  allowed to know anything about: a translucent grey plate between the photo and the words.
  A darker default wallpaper or a brighter font would both have been guesses about the image.
- ⚠ **Tuned by screenshot and then CHECKED AS A NUMBER**, because "looks fine to me" is not the
  property that was asked for. Grey 56 at alpha 190 lands the plate between **41.7** (over a
  black photo) and **106.7** (over a pure white one), so white text on it measures **14.4:1 at
  best and 5.35:1 at worst** — above the 4.5:1 accessibility floor for *every possible
  wallpaper*, which is exactly "no matter what background I pick". 140 was tried and is still
  washed out over a bright photo; 225 is legible but has all but deleted the picture.
- **Only the main menu changes.** Every other menu already paints a solid background, so none
  of them look any different.
- 🔬 **NEW: `key` on the serial console — the cable can now press buttons.** This is the other
  half of the gap `shot` opened in 0.9.13: a cable could already *see* the screen, but not
  change what was on it, so anything you had to navigate to was still unverifiable. The menu
  complaint above lives three key presses from the clock face.

  ⚠ **It injects into `keypadBuff`, the real keypad buffer**, so the wake, the drain loop, the
  easter-egg tracker and each app's own `processEvent` all run exactly as they do for a thumb.
  It is not a simulation of a press; it *is* one, from a different source. A second dispatch
  path would be a second thing to keep in step — and the class of bug that hides is precisely
  the one that put an untested Photos app in a user's hand.

  ```bash
  tools/shot.py /dev/cu.usbserial-025A3F65 menu.png --wait 16 --cmd "key menu" --cmd "key down down"
  ```
- **NEW: `scrim [<alpha> [hex]]`** — retune the plate against a real photograph and take a
  screenshot. ⚠ A bench knob, RAM only, deliberately not persisted: the shipped answer is
  `THEME_SCRIM_*` in `GUI.h`, and a value that exists only in RAM cannot quietly become the
  thing everyone is testing against. `scrim 0` is the pre-0.9.14 look, which is how the
  before/after above was taken from a single build.
- **Measured, not assumed:** nine key presses navigating the menu with the scrim on produced no
  `LOOP STALL` and no panic. The blend is ~55 k pixels per menu redraw and `pushTransparent()`
  caches its blend across runs of identical background pixels.
- ⚠ **The CLOCK face has the same complaint and was deliberately left alone here** — a grey
  box across the middle of the idle screen is an aesthetic call, not a bug fix, so it was put
  to Nick rather than assumed. He chose the scrim; **see 0.9.15 above.**

## 0.9.13 (2026-08-25) — "Set as wallpaper" wrote a good file that nothing ever read

Nick, from the handset: *"I tried to apply a background from a photo but nothing changed, so
there is something broken."* He was right twice over, and the second fault was hiding behind
the first.

- 🛑 **THE WALLPAPER WAS LOADED BEFORE THE SD CARD WAS MOUNTED.** `setup()` mounts SPIFFS,
  calls `gui.init()`, and only ~50 lines LATER calls `SD.begin()` — the card shares SPI with
  the screen and that order is deliberate. The loader lived inside `gui.init()`, so its
  `SD.exists("/background.jpg")` asked an unmounted filesystem and got `false`, every boot,
  forever. The Photos app writes the chosen wallpaper to the **SD card**. Nothing ever read it.

  The fingerprint had been in every boot log for as long as the feature existed:

  ```
  [E][vfs_api.cpp:72] exists(): File system is not mounted
  ```

  **Proven on the hardware, not argued:** phone 2's card was still carrying
  `/background.jpg` at **31,440 bytes — byte-for-byte the size of `BT09.JPG`**, the photo Nick
  had picked. His copy had worked perfectly. The firmware had simply never looked.
- 🔑 **IT WAS SILENT BECAUSE THE FALLBACK IS SHARED.** A rejected override lands in exactly
  the same path as "no wallpaper has ever been chosen", so the screen and the log were
  identical either way. Worse, this phone ships a `/background.jpg` **in SPIFFS** — the plain
  dark texture everyone reads as "the default". That file loaded fine, every time, which is
  what made "nothing changed" so convincing: something *was* being loaded, just never the
  user's choice.
- **Fixed:** `GUI::loadWallpaper()` is now a function with three call sites — `init()` (SPIFFS
  only, but the sprite must hold a picture before the next line of `setup()` redraws),
  **again after `SD.begin()`, which is the call that actually finds the file**, and from
  Photos when the user picks one, so it appears **at once** instead of "restart to see it".
- **`setAsWallpaper()` no longer trusts its own copy.** It asks the loader and repeats the
  answer verbatim, rolling the override back if the loader refuses. A greyscale JPEG still
  *views* in Photos (jpeg_grey.cpp decodes it; TJpgDec refuses greyscale outright) and still
  cannot be a wallpaper — and now it says so instead of claiming success.
- ⚠ **A second silent dropper, found on the way: two different size ceilings on one picture.**
  The viewer opened up to 2 MB; the wallpaper loader capped at 1 MB and fell back with no
  message. A photo between the two viewed perfectly, reported "Wallpaper set", and vanished at
  the next boot. Both are 2 MB now, and Photos refuses an over-limit photo *before* copying
  megabytes to the card.
- 🖼 **Wallpapers now FILL the screen.** TJpgDec only halves — 1, 1/2, 1/4, 1/8 and nothing in
  between — so every photo on the bench card (all 480x270) landed as a **240x135 band across
  the top** with the old background showing underneath, and a 4032x3024 phone photo cannot
  reduce past 504x378 and had its top-left corner cropped out instead. The loader now decodes
  into a temporary PSRAM sprite at the smallest halving that still covers the screen, then
  resamples to fill, cropping to centre. Screenshotted before and after.
- 🔬 **NEW: `shot` on the serial console, and `tools/shot.py`** — the live frame as base64,
  turned into a PNG on the host. **This is the instrument this repo did not have.** Every
  screen in Photos needs a key press, a cable cannot press keys, and that is exactly how a
  broken wallpaper reached a user untried. Everything claimed above about how the screen
  *looks* was checked with it.
- **NEW: `wallpaper` on the console** — what the loader found, from which filesystem, at what
  pixel size, and why it refused. Plus `wallpaper reload | list | clear` and
  `wallpaper set <name>`, which runs **the same `photosSetWallpaper()` the menu runs**, so the
  whole path can be proven over a cable. A test hook that re-implements the feature proves
  nothing; this one cannot drift.
- ⚠ **Noticed, not fixed:** `/photos/20260823_093939.jpg` on phone 2 is **0 bytes** — a failed
  upload that Photos still lists and cannot open.

## Unreleased (2026-08-24, late) — the scrolling freeze was never the database: it is `WiFi.disconnect(true)`

- 🔑 **`WiFi.disconnect(true)` BLOCKS FOR 5007 ms. `WiFi.begin()` next to it takes 30.** The
  argument is `wifioff`, so that call runs `esp_wifi_stop()` and tears the entire radio down
  purely in order to reassociate:

  ```
  SLOW WIFI: connectToWiFi [disconnect(true)=5007 begin=30 other=0]
  ```

  One task means those five seconds froze the keypad, the screen and the WiFi stack together.
- **This is the freeze Nick kept feeling while scrolling, and the database work never touched
  it.** It explains every part of the report that did not fit: why it was rare (it needs the
  hotspot to blip), why it always came with "and WiFi dropped" (**the drop was not a symptom of
  the freeze — it was the trigger for it**), and why it survived moving the save off the input
  path and then onto the SD card.
- **How it was found, because the guessing had to stop.** The stall detector was made to persist
  into `/health.log`, which showed two distinct populations: `scr=0 cpu=80` (idle — the database
  saves, landing exactly where they were meant to) and `scr=65 cpu=240 wifi=6` (screen on, phone
  active, WiFi already down). That second group pointed at the reconnect path rather than at
  storage. A `TIME_STEP()` macro then named the call, and three successive splits walked it down:

  | | |
  |---|---|
  | `connectToPreferred` | 5296 ms |
  | → `loadPreferred` 0 ms, `connectTo` | **5296 ms** |
  | → `loadNetworkSettings` 7 ms, `connectToWiFi` | **5037 ms** |
  | → `begin` 30 ms, `disconnect(true)` | **5007 ms** |

- **The fix is one argument: `WiFi.disconnect(false)`.** A plain disassociate is all a reconnect
  needs — `WiFi.begin()` sets the new config regardless, so the "delete old config" comment was
  describing work that `begin()` redoes anyway. ⚠ The genuine full cycle
  (`WiFi.disconnect(true, true)`) is untouched where it belongs, in the hard reset.
- **Verified both directions after the change:** a forced drop produces **no `SLOW WIFI`, no
  `SLOW STEP` and no `LOOP STALL`** — and the phone still reconnects, with SIP re-registering on
  its own (`registered: yes | wifi: up`). A fix that removed the freeze by breaking reconnection
  would have been worse than the bug.
- 🛠 **New serial command `wifi drop`** — disconnects WITHOUT marking the radio user-disabled, so
  the field retry path runs on demand. A bug that needs somebody else's access point to
  misbehave cannot be measured otherwise, and this one had already survived two rounds of
  fixing the wrong thing.
- ⚠ **A note on the two earlier rounds, kept honestly:** the database save really was blocking
  for ~1.5 s and really is better on the SD card. It simply was not what Nick was feeling. The
  lesson is the one the stall detector exists to enforce — **the symptom named a component, and
  the component was innocent.**

## Unreleased (2026-08-24, night) — the mesh database moves to the SD card, and the benchmark is why

- 🔑 **BENCHMARKED BEFORE COMMITTING, because the previous recommendation in this same session
  was wrong.** Nick asked me to look hard at whether SD was actually the right choice rather
  than assume it. A new serial command, `bench`, writes 8 KB the same shape a real save has —
  open, write, remove, rename — to both filesystems on this exact phone:

  | | total | open | write 8 KB | remove | rename |
  |---|---|---|---|---|---|
  | **SPIFFS** | 2599–2845 ms | **1645 ms** | 163–809 ms | 62–324 ms | 326–639 ms |
  | **SD** | **48–57 ms** | 10 ms | 11 ms | 15–24 ms | 12 ms |

  **About fifty times faster, and consistent** — no wear-levelling spikes across three passes,
  which was the specific risk that made SD worth measuring instead of assuming. Cheap cards are
  notorious for unpredictable multi-hundred-ms pauses and that would simply have moved the
  problem.
- ⚠ **AND IT SHOWS THE EARLIER DIAGNOSIS WAS AIMED AT THE WRONG OPERATION.** `open` alone costs
  **1.6 seconds** on SPIFFS. The write was never the bottleneck — which is exactly why batching
  the writes did nothing and why chunking them made it worse. **Creating a file is the
  pathological operation on this part**, and the temp-then-rename save hits it every save.
- **Measured end to end, same three-save test, same phone:**

  | | stalls over 250 ms |
  |---|---|
  | chunked write, SPIFFS | 31 |
  | single write, SPIFFS | 12 |
  | **single write, SD** | **5** |

  And the survivors are no longer the save: the largest is 5049 ms at `wifi=6`, i.e. WiFi
  association, which the stall detector will now keep catching on its own merits.
- ⚠ **SPIFFS REMAINS THE FALLBACK AND MUST KEEP WORKING.** `meshFs()` decides per call rather
  than caching a verdict at boot — a card can be removed, and `cardPresent` was itself lying for
  the life of the project until earlier today. `loadDb()`/`loadFavourites()` prefer the card but
  fall back to SPIFFS, so **a database written before this change is still found**, and the next
  save migrates it to the card. Verified on hardware: 34 nodes, 26 messages and 3 starred nodes
  all came back across the move.
- **Channels stay on SPIFFS** — a few hundred bytes, written when a channel is added, which is
  rare enough that the card being absent matters more than the speed.
- The service takes card presence from the main loop (`setCardPresent`) the same way it takes
  `uiIdle`, so it keeps no dependency on the UI.

## Unreleased (2026-08-24, evening) — the scrolling freeze, found and timed: it is the database save

- 🔑 **Nick: "sometimes when scrolling menus, the phone will freeze for a second or two, wifi
  will drop, then it will unfreeze and WiFi comes back up."** Reproduced and timed on the first
  attempt:

  ```
  MESH saveDb: 1712 ms BLOCKING for 5944 bytes (34 nodes, 13 msgs)
  LOOP STALL: 2196 ms in one pass - WiFi/keypad/screen were all frozen for this long
  ```

  **Everything in this firmware is one task**, so a blocking write stops the keypad, the screen
  and the WiFi stack together — which is why the symptom is a freeze *and* a WiFi drop, always
  together, always the same length. The loop's own sms-mirror comment already warned about this
  shape ("not slow, it is the 5-second freeze bug rebuilt on purpose").
- **Why it is only occasional:** `dbDirty` fires on real changes — a new node, a name, a learned
  key — not on every packet, so on a quiet mesh saves are rare. It is only when one lands
  mid-scroll that you feel all 1.5 s of it.
- ⚠ **TWO PLAUSIBLE CAUSES WERE MEASURED AND BOTH WERE WRONG.** Recorded so nobody re-runs them:
  - *"It is the ~55 tiny `f.write()` calls."* Batching them into one PSRAM-buffered writer moved
    1275 ms to about 1000 ms. Real but marginal — the call count was not the bottleneck.
  - *"SPIFFS is nearly full, so garbage collection is thrashing."* It is **2.6% full**
    (87 KB of 3.3 MB). Not fragmentation.

  What is left is the flash itself: **~6.9 KB takes ~1050 ms, about 6 KB/s.** The phase
  breakdown is `[rm_tmp=123 write=1053 rm_old=24 rename=125]`, so the temp-then-rename added for
  crash safety costs ~250 ms of it — real, but not the story.
- **The fix shipped is a deferral, and it is honest about being one: the save now waits until
  nobody is touching the phone** (three seconds after the last key). It does not make the write
  faster — **it makes it land where it cannot be felt.** The data still persists within seconds
  of the phone being put down, and the save is unchanged in every other respect.
- 🛠 **A permanent superloop stall detector ships with it.** Any pass over 250 ms now logs at the
  compiled-in level with the screen, CPU and WiFi state at the time. A freeze this rare cannot be
  caught by watching for it; it has to be caught by the phone. Costs one `millis()` compare per
  pass, and is silent until something real happens.
- ⚠ **I RECOMMENDED CHUNKING THE WRITE ACROSS LOOP PASSES, BUILT IT, MEASURED IT, AND IT WAS
  WORSE.** Recorded in full because the reasoning was plausible and someone will have it again.
  The per-byte model (~6 KB/s, so 128 B ≈ 20 ms a pass) is an **average, and the average is a
  lie here**: SPIFFS writes are quick until one crosses a block boundary and forces an ERASE,
  and an erase is atomic and blocking however small the write that triggered it. More, smaller
  writes means more boundary crossings. Same three-save test, same phone:

  | | stalls over 250 ms |
  |---|---|
  | chunked, 128 B per pass | **31** |
  | single write | **12** |

  **A stall the filesystem takes in one indivisible piece cannot be chunked around.** Reverted
  to a single write, which is at least *one* pause rather than many.
- **What did survive from that work, and is worth keeping:** the image is now snapshotted into
  PSRAM in one pass (memcpy, no I/O) and written from there. That makes the save atomic in a
  second sense — nodes and messages can no longer change halfway through and tear the file —
  and it removed the redundant `SPIFFS.remove()` that logged `/meshdb.tmp does not exists` on
  every single save.
- 🛠 **Stall records now also go to `/health.log`**, rate-limited to one a minute. The detector
  was serial-only, which meant it could only ever catch a freeze while somebody was watching a
  cable — and the freeze it exists to catch happens while the phone is being *used*. Same
  argument as the health line itself.
- [ ] 🔑 **THE REAL FIX IS TO STOP USING SPIFFS FOR THIS, and the evidence now points hard at
  it.** `/health.log` lives on the SD card, appends every minute, and has never once tripped the
  stall detector. Moving the database there is a deliberate piece of work — it needs a SPIFFS
  fallback for a missing card, which is exactly the case the `cardPresent` bug hid for years —
  **but it is the next thing to do, not another attempt to outsmart this filesystem.**
- [ ] **The root cause is untouched and worth returning to.** ~6 KB/s means any future
  full-database write blocks for over a second, and at the message cap that would be minutes.
  Two real options: chunk the write across loop passes (the "one bounded step per pass" idiom
  this codebase already uses for the SMS mirror), or move the database to the SD card, where
  `/health.log` already lives and writes without trouble. **Do not raise `MESH_MSG_CAP` before
  one of those is done.**

## Unreleased (2026-08-24, later) — `chg=` settled: the flag was inverted as well as mis-read

- ✅ **THE POLARITY QUESTION IS ANSWERED, AND IT TOOK FIVE MINUTES OFF THE CHARGER.** The charger
  IC's STAT output is open-drain — it pulls the line DOWN on the charger and releases it (pull-up
  → high) off it. The read was `== HIGH`, so the flag has been **inverted for the life of the
  project**, on top of reading the wrong GPIO entirely until earlier today, which is what hid it.
- **Measured, one continuous boot, one build stamp, no confound:**

  ```
  up=20  v=4.20  chg=0     on the charger
  up=21  v=4.15  chg=1     <- unplugged; voltage starts falling
  up=25  v=4.11  chg=1     still on battery
  up=26  v=4.18  chg=0     <- replugged; voltage jumps back
  ```

  `chg=1` tracked **not charging** throughout, and the voltage curve says so independently of the
  flag. This is exactly the run that could not be done overnight, and the build stamp added hours
  earlier is what makes it attributable rather than arguable.
- ⚠ **The overnight caution was right to hold, and so was refusing to act on it.** A 135-sample
  run last night looked like proof of this same inversion, and it could not be separated from the
  pre-fix confound where the flag was reading a UART TX line that idles high. It turns out to have
  been real — **but it was not knowable then**, and the correct call was to record both readings
  and go get a measurement that admits only one.
- ⚠ **WHAT IS STILL NOT ESTABLISHED:** whether the line reports *charging* or merely *charger
  present*. Post-fix, plugged in at soc=100% and 4.19 V, it still reads `chg=1` — which is either
  a charger still topping off, or a pin that tracks USB presence rather than charge current.
  **`chg=1` may safely be read as "on the charger". Do not yet read it as "current is flowing".**
  Settling that needs a run left plugged until charging genuinely terminates.
- **Also:** the Nodes screen title now reads `Nodes  *=star`. Starring worked from the day it
  shipped and was still undiscoverable — the footer's two slots are the OK action and Back, and
  `*` is neither, so nothing anywhere told you the key existed.

## Unreleased (2026-08-24, afternoon) — the SIP flap, the starvation under it, and two loose ends closed

- 🔑 **SIP registration no longer flaps, and the reason the last attempt failed was never the
  flap fix.** Three changes; the important one is not the obvious one.
  1. **Ping starvation.** The caller is an if/else chain — `if (ping due) ping(); else if
     (register due) registration();` — and `ping()` wrote `msLastPing` only INSIDE its
     `connected()` branch. With the connection down the stamp never advanced, the ping branch
     was taken on *every pass*, and **`registration()` was never reached again**. A phone that
     lost its connection could not re-register itself; only a reboot recovered it. ⚠ **That is
     the "went `lost` and never came back" of 2026-08-22, and it is reachable on its own — it
     has been there since the initial commit.** `ping()` stamps the ATTEMPT now.
  2. **The flap itself:** `requestRegister()` cleared `registered` on every REGISTER, so the
     phone declared itself unregistered for one round trip per refresh, by construction. A
     failed refresh is still covered — `registrationInvalid()` measures from `msLastRegistered`,
     which only a 200 OK advances.
  3. **`REGISTER_PERIOD_MS` 60000 → 45000.** ⚠ Neither 2 nor 3 works alone: with period equal
     to expiry the expiry check tripped at the same instant the refresh fell due, which is why
     the 45 s attempt on its own only moved the flap. The on-air `Expires` header is unchanged,
     so a dead phone's binding still clears at the registrar within 60 s.
- **Measured over 4 minutes on the hotspot: 6 REGISTERs, 1 REGISTERED transition, ZERO `lost`
  events** — against 53 flaps in 31 minutes before. And the starvation fix got an unplanned live
  test: WiFi genuinely dropped after a reset and **registration recovered on its own**, which is
  precisely what used to need a reboot.
  ⚠ **Still owed: confirmation that an INBOUND call lands.** That cannot be proven from the cable.
- ⭐ **The node order is now frozen between rebuilds, and that is a correctness fix, not a
  preference.** `getNode(index)` re-sorted on demand while `index` is a row number the UI got
  from a list it drew earlier — and the Nodes screen uses it for hit-testing as well as drawing.
  A node heard between the draw and the key press silently renumbered the rows, so `*` would
  star (or OK would DM) **the node next to the highlighted one**. COVEY had the identical bug in
  `_activate_at` and it was fixed the same day. Only a size change forces a rebuild now; the UI
  re-sorts explicitly in `buildNodes()`, which is the one moment the numbering may change.
- 🛠 **`/health.log` boot lines carry a build stamp** (`build=Aug 24 2026 07:51:57`). The log
  recorded only `up=` minutes — no wall clock, nothing saying which firmware wrote a sample — and
  that gap is the sole reason the `chg=` polarity question could not be settled from four hours
  of data: a long run of `chg=1` tracked discharging perfectly, and there was no way to tell
  whether those samples predated the fix, when the flag was reading a UART TX line that idles
  high. Every build is distinguishable now, so a run of samples can be attributed.

## Unreleased (2026-08-24, midday) — the node list grows 6x and learns to be starred

- **`MESH_MAX_NODES` 32 → 200, and the table moved to PSRAM.** It was a plain member array on a
  global, so it lived in BSS — **the internal RAM that actually panics this phone** — at 84 bytes
  a node. `ps_malloc`'d now, the same treatment `messages` already had. Measured on hardware:
  free internal heap at boot went **133,816 → 136,232**, so the bigger table costs *less* real
  RAM than the small one did.
- **Why 200 and not more:** PSRAM is not the constraint (3.6 MB free ≈ 44,000 nodes) and neither
  is the database — the message store can already take that file to 242 KB against the node
  table's 16 KB. The binding constraint is a person scrolling the list, which is what starring
  answers. Saves are debounced and fire only on a real change, so a bigger table is not
  proportionally more flash writes.
- ⭐ **Starred nodes.** Press `*` on the Nodes list. They sort to the top, and **they are the last
  thing evicted** — eviction now has three tiers (plain stranger → keyed peer → starred), each
  only touched when every lesser tier is empty. On a public LongFast mesh full of strangers,
  that second half is the one that matters: it is the same failure that silently killed DMs to
  COVEY, made impossible for the nodes Nick actually cares about.
- 🔑 **The starred IDs live in their OWN small file (`/meshfav.bin`), not just as a bit in the
  database.** Nick asked whether they could live somewhere a power cut cannot reach. The reason
  to do it is better than durability alone: **the node table is machine data — every entry is
  rediscoverable by listening to the mesh. The stars are the one thing in there that is not.**
  So they get their own file, written only when you press `*`, temp-then-renamed, instead of
  riding inside a 250 KB blob rewritten whenever any node is heard.
  It also makes a star **survive eviction**: the list is the truth and the flag on `MeshNode` is
  a cache re-applied whenever a node is created, so a starred node pushed out of a full table
  comes back starred — and immediately eviction-proof again — the moment it is heard.
  ⚠ **SPIFFS, deliberately not the SD card**: 4 bytes per star makes size irrelevant, and SPIFFS
  is always mounted whereas a card can be absent.
- **New serial command `star`** — bare it lists, with a node it toggles. Same reason `send`
  exists: a feature reachable only by a thumb on a screen cannot be proven from the cable.
- 🔴 **THIS SHIPPED A BOOT LOOP FIRST, AND NICK FOUND IT BEFORE I DID.** The constructor still had
  `memset(nodes, 0, sizeof(nodes))` from when `nodes` was a fixed array — where `sizeof` gave the
  whole 2,688 bytes. Against a pointer `sizeof(nodes)` is **4**, and at construction time it is
  still NULL, so this was `memset(NULL, 0, 4)`: **StoreProhibited inside a global constructor**,
  panicking before `setup()` ever ran. It compiles without a warning. Decoded from the backtrace
  with `addr2line`, which named the constructor in one pass after guessing had named nothing.
- ⚠ **The process failure is the more useful lesson: I flashed and moved on without reading the
  boot log.** The verification I *did* run filtered serial for expected strings, got nothing, and
  I read "no matches" instead of "no readable output at all" — which is itself the boot-loop
  signature, because the ROM bootloader talks at **115200** while that capture listened at
  500000. **A flash is not finished until something confirms `Booted` and the absence of `Guru`.**
  The check now does exactly that, and reports the readable fraction so garbage cannot look like
  silence.

## Unreleased (2026-08-24, overnight) — `saveDb()` truncated the database in place, and a night of resets proved it

- 🔴 **`saveDb()` OPENED THE REAL FILE WITH `"w"` — TRUNCATE, THEN REWRITE IN PLACE.** Anything
  that stopped the phone mid-write left a **short file**, and `loadDb()` `break`s out of its read
  loops on a short read and silently keeps whatever it got. Nodes are written first so they
  survive intact; messages come next and get cut off; waypoints are written **last** and go
  first. Nothing anywhere reports a problem — the file still has a valid magic and version.
- ⚠ **NOT THEORETICAL, AND I CAUSED IT.** A night of flashing and serial work took the message
  store from **55 messages to 4**, and the database still loaded cleanly with all 32 nodes.
  **Every serial port open resets this board**, and a save runs off a dirty flag, so the window
  is open far more often than "during a manual save". Traced by the `MESH DB: N nodes, M
  messages` boot line across nine captures: 43 → 43 → 49 → 54 → 54 → 54 → 55 → **4** → 5.
- **Fixed with the pattern this codebase already uses elsewhere:** write to `/meshdb.tmp`, then
  rename over `/meshdb.bin`. Rename on SPIFFS is atomic, so a reset now leaves either the old
  complete database or the new complete one, never half of either. `healthTrim()` has done this
  all along for the same reason; `saveDb()` simply never adopted it. If the rename fails the
  existing database is left untouched and it says so.
- **Verified by abusing it deliberately:** three sends across three reset cycles counted up
  6 → 7 → 8 → 9, then four bare reset cycles held at **9, 9, 9, 9**. Before the fix the same
  treatment is what collapsed it.
- ⚠ **The lost messages are lost** — roughly 50 of Nick's stored mesh messages. The node table
  (32, with keys) survived because of the write order. **Waypoints currently read as none**, and
  I cannot say whether any were set beforehand: I never captured a `pos` baseline, and COVEY has
  no `~/.covey/waypoints.json` either, so there is no second copy to compare against.
- ⚠ **A correction to my own entry above**, which said this class of loss would take *"Camp"*.
  There is no evidence Camp was ever set on this phone — that name comes from a **code comment**
  explaining why waypoints are persisted, and I wrote it up as though it were observed data. It
  is now phrased as "any stored waypoints", which is what is actually known.
- [ ] **Worth considering:** `loadDb()` treats a short read as "that is all there was" and
  carries on. That is what made this silent. A record count that does not match the header is a
  fact worth logging even when the file is otherwise usable.

## Unreleased (2026-08-24, later) — DMs to COVEY were silently failing, because a stranger evicted its encryption key

- 🔑 **THE NODE TABLE THREW AWAY COVEY'S PUBLIC KEY TO MAKE ROOM FOR A STRANGER, AND EVERY DM
  TO IT HAD BEEN FAILING SILENTLY EVER SINCE.** `upsertNode()` evicted strictly oldest-heard
  at the 32-node cap, and the slot wipe (rightly) takes `pubKey`/`pkiFlags` with it. On
  LongFast — a public channel full of nodes we will never DM — COVEY goes quiet, loses its
  slot to the 32nd stranger, and comes back keyless the next time it speaks. Keyless means
  DMs fall back to the pre-2.5 legacy form, which COVEY refuses to decrypt.
- **Measured, and the diagnosis is airtight:** the phone held COVEY as a bare `'!62b8d2fd'`
  with NO KEY, while the docs recorded it as `'Nick H New Device'` key `HepOEI6R…`. After
  recovery it reads `'Nick H New Device' key HepOEI6RPnBxEmLqSb8gezCVaVd0qO+3JYo1Ky2+fCM=` —
  and `HepOEI6R` base64-decodes to `1d ea 4e 10 …`, exactly the `learned key for !62b8d2fd
  (1dea4e10...)` the phone logged on relearning it. **The key never rotated. Eviction simply
  binned it.**
- **A node whose public key we know is now evicted LAST.** The scan prefers the oldest
  *keyless* node and only falls back to oldest-overall when every peer is keyed — which
  cannot deadlock and matches the old behaviour exactly on a table full of keyed peers. A
  peer we can do end-to-end crypto with is worth more than the 32nd stranger heard, always.
  Evicting a keyed peer now logs, because it should be rare enough to notice.
- ⚠ **This was found by the delivery receipt shipped hours earlier**, which is the entire
  argument for that feature. The failure was completely silent before: the message appeared
  in the thread and looked sent. The receipt said `failed: they could not decrypt it - key
  mismatch (err=6)` and pointed straight at it.
- **Recovery, and it is worth writing down: restarting `covey-ui` makes COVEY re-announce.**
  The phone's own `announce` (NodeInfo with want_response) drew replies from two strangers
  across three minutes but never from COVEY — `nbr` shows **0 direct neighbours**, so COVEY is
  multi-hop and its replies were not surviving the trip, even though routed NAKs were. A
  `sudo systemctl restart covey-ui` on the Pi triggered its NodeInfo immediately and the phone
  logged `MESH PKI: learned key for !62b8d2fd - DMs unlocked`. No config was written and no
  key was injected by hand; trust-on-first-use is intact.
- ✅ **BOTH RECEIPT BRANCHES ARE NOW PROVEN ON AIR.**

  ```
  dm: sent to !62b8d2fd (PKI)
  MESH DM ACK from !62b8d2fd for id=0x056414bc err=0
  MESH RECEIPT: 'hello from the wiphone' -> delivered
  ```

  together with the earlier failure case (`err=6` → `failed: they could not decrypt it`).
- ✅ **AND `in mesh` IS PROVEN TOO — all three states are now measured, none inferred.** A new
  serial command closed the gap: `send <chan-index> <text>` sends a channel text from the
  cable, which `dm` could not do, and which is why "in mesh" shipped unproven a few hours
  earlier.

  ```
  send: sent on [1] 'Howe group'
  MESH RECEIPT: 'goodnight from the wiphone' -> in mesh
  ```

  Because the acknowledgement is implicit, that line is also proof that something out there
  relayed the packet — the receipt and the reachability check are the same observation.
- **Also fixed, same family as the extender-pin bug:** `setup()` called plain `pinMode()` on
  `BATTERY_CHARGING_STATUS_PIN` (`EXTENDER_PIN(0)` == 64) in both board branches — configuring
  a pin the ESP32 does not have, so those lines did nothing. Benign, because the real
  configuration happens correctly through `allPinMode()` in the extender block earlier in
  setup — but **a line that looks like it sets up the charge-status input and does not is
  exactly how the sibling bug in the battery tick survived so long.** An audit of every raw
  `digitalRead`/`digitalWrite`/`pinMode` call against the extender-pin list found no others:
  the remaining ones are all genuine ESP32 GPIOs.

## Unreleased (2026-08-24) — delivery receipts on mesh messages, and the WiFi row moves into Settings

- **The WiFi toggle lives in Settings now**, leading the WiFi group, above "Edit current
  network" / "WiFi auto-switch" / "Scan WiFi networks" — the three rows that all assume a
  radio which is on. It spent a few hours on the main menu on 08-23 and Nick moved it. The
  change is one table line: both the runtime label builder and the dispatch key off `action`
  and never look at the parent, so the row can be re-homed freely. Verified after flashing —
  `GUI::init()`'s boot check reported zero duplicate menu IDs.
- 🔑 **Outgoing mesh messages now carry a delivery receipt**, shown on the line that already
  said "me" in a thread: **`me - sent`**, **`me - delivered`**, **`me - in mesh`**,
  **`me - failed`**.
- ⚠ **THEY ARE DELIVERY RECEIPTS, NOT READ RECEIPTS, AND THE WORDING IS DELIBERATE.**
  Meshtastic has no concept of a human having read anything — the strongest signal in the
  protocol is that a *radio* acknowledged the packet. COVEY's UI says the same thing for the
  same reason. Anything labelled "read" would be a claim the mesh cannot support.
- ⚠ **`in mesh` and `delivered` are not the same claim.** A DM is acknowledged by the
  destination node itself, so *delivered* means delivered. A broadcast is acknowledged
  **implicitly** — hearing your own packet rebroadcast is the acknowledgement — which proves
  the message entered the mesh and **nothing whatever about who received it**. Two words,
  because one would have been a lie half the time.
- **Almost none of this was new machinery.** The stack already set `want_ack` on PKI DMs,
  already parsed `request_id` off inbound ROUTING packets, and already logged
  `MESH DM ACK ... err=0` per message. `meshtastic_service.cpp:1570` even called it *"the
  only proof of delivery this phone gets"* — and then dropped it on the floor. This wires
  that existing signal to the message store and the UI.
- 🔑 **THE RECEIPT LIVES IN SPARE BITS OF `MeshMessage.flags`, AND THAT IS THE WHOLE DESIGN.**
  The obvious implementation adds a `uint32_t packetId` to `MeshMessage` — and `saveDb()`
  writes `sizeof(MeshMessage)` into the header while `loadDb()` **rejects the entire file** on
  a mismatch: nodes, messages and waypoints together. That would have silently binned the
  message history, the node list, and any stored waypoints on the first boot after the update. Measured on the real phone after
  flashing: **"MESH DB: 32 nodes, 49 messages stored"** — all still there. The packet-id
  association instead lives in an 8-entry RAM table for the seconds between sending and the
  ack; the resolved state persists in `flags`, exactly like `MESH_MSG_READ`.
- ⚠ **That table is keyed on `timeMs`, NOT on the message's index.** Indices are not stable —
  the store compacts on `removeMessageAt()`, so the per-chat and global caps can shift every
  message down while an ack is still in flight, and the receipt would land on a stranger's
  message. Pending entries do not survive a reboot, which is honest: an ack for a message
  sent before a restart can no longer be attributed to it.
- ⚠ **A DM MUST NOT TAKE THE IMPLICIT ACK, and this was caught before it shipped.** DMs flood
  too, so we hear our own DMs rebroadcast by neighbours exactly like broadcasts. Treating that
  as delivery would mark a DM "delivered" the moment *any* node relayed it — saying nothing
  about whether the person it was addressed to ever got it. The implicit path is gated on
  `hdr.dest == MESH_ADDR_BROADCAST_ONAIR`; a DM waits for the destination's own routing ACK.
- **Broadcasts deliberately do NOT ask for an ack, and this costs nothing on the air.** Flood
  routing rebroadcasts any packet with hops left whether or not `want_ack` is set, so the bit
  would add no information and risks stock answering with a routing packet nobody needed. The
  first implementation did set it; measuring what the implicit ack actually is removed the
  cost entirely. `want_ack` remains on DMs only, where it produces a real end-to-end reply.
- ⚠ **`meshTxData()` takes `wantAck` defaulted OFF and it must stay that way.** That function
  also carries NodeInfo, position, neighbour info and **our own outgoing routing ACKs** —
  asking for an ack on an ack is a loop. Only the text path opts in.
- **A queued DM that never reaches the air now says so.** DMs whose AES key is not cached are
  echoed into the thread at queue time and transmitted a tick later; both failure paths
  (`queued PKI send FAILED`, `key derive failed`) previously logged and left the message
  looking exactly like one that went out fine. They mark it failed now — which is why the
  pending-DM record carries its message's `timeMs` across the tick.
- **Words, not ticks, for a second reason:** the Akrobat/OpenSans faces are generated bitmap
  glyphs and **nothing anywhere in this firmware renders a non-ASCII character**. A checkmark
  would come out as a box, which is worse than no marker at all — COVEY hit exactly this and
  had to draw its ticks by hand.
- ✅ **PROVEN ON AIR — the failure path, twice, end to end.** A DM to COVEY produced the whole
  chain in the serial log:

  ```
  MESH DM to !62b8d2fd sent LEGACY (no key known) - a 2.5+ node will drop it
  MESH DM NAK from !62b8d2fd for id=0x9adaa67b err=6
  MESH RECEIPT: 'receipt test' -> failed: they could not decrypt it - key mismatch
                                          (legacy DM to a 2.5+ node?) (err=6)
  ```

  Send → packet id tracked → inbound ROUTING matched → receipt stamped on the right message.
  (The phone's `0 direct neighbours` reading beforehand was an empty table after a reboot, not
  silence — COVEY was there the whole time and answered.)
- ⚠ **The `delivered` and `in mesh` branches are NOT yet proven.** They are the same code path
  reached with `err == 0`, and the transport underneath them is exactly what the NAK above
  travelled on, so the risk is small — but "highly likely" is not "measured" and it should be
  watched rather than assumed. `announce` did not draw a NodeInfo out of COVEY inside 40 s, so
  its public key is still unknown and every DM keeps falling back to the legacy form.
- 🔑 **AND THE FEATURE IMMEDIATELY EARNED ITSELF: DMs from this phone are NOT REACHING COVEY
  AT ALL right now.** `pki` shows no key for `!62b8d2fd`, so they go out in the pre-2.5 legacy
  form, which COVEY refuses to decrypt (`err=6`). That has presumably been true for a while and
  was invisible — the message appeared in the thread and looked sent. **This is exactly the
  class of silent failure the receipt exists to surface**, found within minutes of it existing.
  The fix is hearing COVEY's NodeInfo; `pki` names who has keys.
- **A plain-English reason accompanies every failure in the log**, from the same Routing.Error
  table COVEY carries, with the raw code kept for searching. "err=6" tells nobody in a tree
  stand whether to move, wait, or re-send. The small screen keeps the short word (`me - failed`)
  and the log carries the sentence.
- **Built, flashed, boots clean, menu table verified (zero duplicate IDs), 1167 host
  assertions green.**

## Unreleased (2026-08-23, night) — two status flags were reading the wrong silicon, and one of them is the dead `chg=`

- 🔑 **`cardPresent` and `battCharged` were read with plain `digitalRead()` on GPIO-EXTENDER
  pins, which does not reach the extender at all.** Both live on the SX1509:
  `TF_CARD_DETECT_PIN` is `EXTENDER_PIN(1)` == **65** and `BATTERY_CHARGING_STATUS_PIN` is
  `EXTENDER_PIN(0)` == **64** (`EXTENDER_FLAG` is `0x40`). The ESP32 has GPIO 0–39, so both
  numbers are out of range — **and `digitalRead()` does not reject them.** It calls
  `gpio_get_level()`, which for `pin >= 32` evaluates `(in1.data >> (pin - 32)) & 1`; the
  Xtensa shift masks its amount to 5 bits, so `>> 32` becomes `>> 0` and `>> 33` becomes
  `>> 1`. The two flags were therefore reading:

  ```
  battCharged  <-  GPIO 32   (USER_SERIAL_TX / MotorEN)
  cardPresent  <-  GPIO 33   (I2S_WS_PIN, the I2S word-select clock)
  ```

- ✅ **THIS IS WHY `chg=` IS DEAD IN EVERY HEALTH SAMPLE.** It was filed as "its own small
  mystery" and then promoted to a measured fact — 0 across all 807 samples of the 3.0 h run,
  straight through two unmistakable charging periods. **The flag was never reading the
  charger. It was reading an idle-low pin on the wrong die.** `allDigitalRead()` — the
  accessor that honours `EXTENDER_FLAG` — has been sitting in `Hardware.cpp` the whole time,
  and every other extender read in the file already used it.
- ⚠ **`cardPresent` was reading the I2S word-select line, which is idle-low — so it reported
  TRUE whether or not a card was seated, and would FLICKER while audio plays**, since WS
  toggles at the sample rate and the tick samples it at an arbitrary phase. A card-dependent
  path could fail intermittently during music for reasons nothing else could explain. Not
  observed in the wild; recorded because it is now closed rather than waiting.
- **`cardPresent` is also seeded once in `setup()` beside `SD.begin()`.** It is declared
  `false` in `GUI.h` and was assigned in exactly one place — the battery tick — so for up to
  15 s after every boot the phone believed it had no card, and `healthDump()` bails on
  `!cardPresent`. That is what made `health` answer *"nothing to read - no card, or no
  /health.log yet"* **with a perfectly good card seated**, in exactly the window in which
  someone plugs a phone into the cable and asks it what happened.
- **Verified on hardware after flashing** (app partition only, 230400 baud, hash verified):
  `health` issued **11.3 s after boot** returned 25,745 bytes and 191 samples, where the same
  call before the fix returned the no-card message. That also proves `allDigitalRead()` on the
  extender works — a garbage read would have failed the dump.
- ⚠ **`chg=` STILL READS 0, AND THAT IS NOT YET RESOLVED.** With the phone on USB at soc=96%
  and terminal voltage *rising* (4.14 → 4.15 → 4.17 across three ticks), the flag stayed 0.
  Two readings fit and this session cannot separate them: the pack is nearly full and the
  charger has legitimately tapered off, **or the polarity is inverted** — charge-status
  outputs on this class of charger IC are typically open-drain and **active LOW while
  charging**, which would make `== HIGH ? true : false` exactly backwards. **Settle it on a
  run that starts from a low pack**, where "charging" is unambiguous, before trusting or
  re-inverting the flag. The read now reaches the right chip; the meaning of what it reads is
  the open half.
- Also fixed: a `log_d` that called `gpioExtender.digitalRead(POWER_CHECK)` without masking
  `EXTENDER_FLAG` (the working call at the top of `loop()` does mask it), so it queried
  extender pin 66 and logged nonsense. Cosmetic — `log_d` is not compiled into this build.

## Unreleased (2026-08-23, evening) — the idle floor is real, and the first 20 minutes off a charger are a lie

- **Idle drain, WiFi OFF, screen off, 80 MHz: ~8.6 %/h — about 1.8x better than the
  15.7 %/h mixed-use figure.** One hour on phone 1, 62 once-a-minute samples recovered
  from `/health.log` over the cable (`health all`). Off-charger window is unambiguous in
  the log: terminal voltage steps 4.19 → 4.14 at the unplug (up=32) and 4.02 → 4.10 at
  the replug (up=95), with `wifi=0 cpu=80 scr=0` on every sample in between.
- **⚠ THE ENDPOINT READ SAYS 15.74 %/h AND IT IS WRONG.** Taken start-to-finish the hour
  reads 100% → 84% = 15.74 %/h, which is *identical* to the mixed-use run and would have
  supported the flatly false headline "turning WiFi off saves nothing". Splitting the
  window into 20-minute blocks kills that reading:

  | block | SoC | rate | voltage slope |
  |---|---|---|---|
  | up 33–52 | 100 → 92% | **25.3 %/h** | −188 mV/h |
  | up 53–72 | 92 → 88% | 12.6 %/h | −41 mV/h |
  | up 73–94 | 87 → 84% | **8.6 %/h** | −40 mV/h |

  The first block is **post-charge surface-charge relaxation**, not current draw — a
  freshly-unplugged cell sheds its surface charge fast whatever the load. The last two
  blocks agree with each other (−41 and −40 mV/h) and that agreement is what makes them
  the real number. **Do not start an idle-drain measurement at the instant of unplugging;
  discard the first ~20 minutes.**
- The same artefact inflates the 3.0 h mixed-use run that produced ~6.4 h, though a
  20-minute contamination is diluted over three hours rather than dominating one. **~6.4 h
  is therefore mildly pessimistic**, and the two runs are less far apart than the raw
  numbers suggest. Neither is worth restating until a run starts an hour after unplugging.
- 🔑 **AND THE RETIRED ~10 h FIGURE WAS MEASURED CORRECTLY ALL ALONG — it is the 15.7 %/h that
  is the outlier.** `docs/HANDOFF.md:2142` records how it was taken: *"steady state with the
  first 30 minutes discarded (surface charge): 94% → 82% over 1.20 h ⇒ 10.0 %/h"*. **That is the
  same correction rediscovered from scratch tonight**, applied by whoever took that reading and
  then forgotten. Line the three up:

  | run | method | rate |
  |---|---|---|
  | original "~10 h" | steady state, first 30 min discarded | **10.0 %/h** |
  | tonight, WiFi OFF | steady state, first 20 min discarded | **8.6 %/h** |
  | 08-23 mixed use | endpoint-to-endpoint, **nothing discarded**, WiFi hunting 86% of it | 15.7 %/h |

  **The two corrected runs agree with each other to within 1.4 %/h across different builds and
  months; the uncorrected one is nearly double both.** So "10 h is unproven from here on, do not
  requote it" — written in this changelog on 08-23 and propagated into the memories — was an
  **overcorrection against the better-measured number**. The 15.7 %/h run is contaminated twice
  over: no relaxation discard, and the radio hunting for 86% of it.
- ⚠ **What that does NOT license:** the runs are not equivalent — different firmware, different
  months, different radio states, and one is 1.2 h against another's 21 min. **Neither is a
  full-discharge measurement, and nothing here has watched a pack go from full to empty.**
  Quote a rate with its conditions attached, and stop converting either into an hours figure
  until a run starts an hour after unplugging and continues into the flat part of the curve.
- **Confidence, stated honestly:** the gauge quantises to 1%, so three counts across a
  21-minute block carries roughly ±3 %/h; the voltage slope is the firmer half of the
  result. The 11.7 h that 8.6 %/h projects to is a *top-of-curve extrapolation* — the
  4.02–4.05 V band is nearly the flattest part of the Li-ion curve and the back half of a
  discharge will not be this kind. Quote the rate, not the projection.
- **⚠ Found while fetching this: `health` lies for the first minute after every boot.**
  `healthDump()` bails on `!gui.state.cardPresent` (WiPhone.ino:1786), but `cardPresent`
  is initialised `false` (GUI.h:318) and only ever assigned by the once-a-minute battery
  poll (WiPhone.ino:2727). Run `health` inside that first minute — exactly what you do
  after plugging in a phone — and it reports `nothing to read - no card, or no
  /health.log yet` **with a perfectly good card seated**. It reported precisely that here,
  and the log it was denying turned out to hold all 926 samples. Waiting 80 s and asking
  again returned the whole file. NOT FIXED: the card-detect read wants doing once in
  `setup()` beside `SD.begin()`, not only on the minute tick.

## Unreleased (2026-08-23) — the battery number gets measured instead of claimed, and the radio gets a switch

- **~6.4 h on battery, not the ~10 h the handoff had been claiming since the power
  work.** Measured over 3.00 h off USB: 99% → 52%, 4.13 → 3.80 V = 15.7 %/h. The first
  sample ever taken off the cable, and it is a third short of the figure it replaces.
  **10 h is unproven from here on.** Ruled out by the same samples: not the screen (off
  for 177 of 181) and not the CPU governor (177 of 181 at 80 MHz). The draw is radios
  plus baseline, in a pocket, doing nothing.
- ⚠ **Leading suspect, explicitly NOT proven:** 155 of 181 samples were `wifi=1`
  (WL_NO_SSID_AVAIL) — away from any known network, hunting, and the auto-switch scans
  every 2 min while disconnected against 10 min while connected. The cost could not be
  separated honestly: voltage slope is not linear in SoC, so comparing a hunting block at
  3.9 V against a connected block at 4.1 V measures chemistry, not radio. **Do not change
  the scan interval before measuring it.**
- ✅ **`chg=` is definitively dead** — 0 across all 807 samples, including two unmistakable
  charging periods (89→100%, and 53→86% on a drive home) with voltage rising through both.
  Previously filed as "its own small mystery"; now a measured fact. **Nothing may infer
  charge state from that flag.**
- **New: `health` / `health all`** stream `/health.log` to UART from a 256-byte stack
  buffer — no WiFi, no sockets, no heap. ⚠ The obvious route, `http://wiphone.local/log`,
  is the dangerous one: it needs the uploader up, and heap at the moment of reading was
  free=11448 largest=10560 min-ever=196 — below the ~16 KB at which an 11 KB allocation
  has already aborted the WiFi PHY and rebooted this phone. **The log exists to explain
  restarts; fetching it must not cause one.** ⚠ It was also one boot from being
  half-eaten — 98,453 bytes against a 96 KB cap, so the next boot line would have trimmed
  it to the newest 32 KB and silently taken the start of the run with it. Cap raised
  BEFORE flashing (`HEALTH_LOG_MAX` 96 K → 256 K, `KEEP` 32 K → 128 K, ~16 h) which is the
  only reason the run survived.
- **WiFi on/off is one press from the main menu, and the row says which it is.** The
  switch already existed and was real, but lived inside a *network's edit form* — menu,
  WiFi, pick a network, edit, find the widget — and told you nothing about the current
  state until you got there. Now the main menu reads `WiFi: on` / `WiFi: off` and one
  press flips it. This is a battery feature, not a convenience: the experiment that would
  settle the hunting question needs the radio switched off quickly and repeatably.
  ⚠ **Deliberately NOT persisted** — WiFi returns ON after a reboot, because a radio that
  stays off across a power cycle is a setting you can forget you set, and the failure mode
  is a phone that silently never connects again. ⚠ Menu ID 47 (0–46 taken; 8 and 25 are
  gaps and the rule is COUNT UP, not fill). A duplicate id is SILENT — `findMenu` matches
  on id alone, first hit wins — and has shipped twice; `GUI::init()`'s boot check is the
  only thing that catches it.
- **The wiring sheet is audited geometrically rather than by eye**, before being shared.
  🔑 The one Nick spotted: 143 px of W20 (VBAT) and W11 (GND) drawn on the same x=740
  lane, y 1072–1215 — GND is stroke-3 over VBAT's 2.6, so VBAT vanished underneath it and
  appeared to *stop* at the junction when it actually carries on to the OR node at y=1240.
  Four more collisions found the same way. The audit is three kept scripts that decompose
  every line into segments and report collinear overlap, with text boxes measured from a
  real renderer's `getBBox()` because estimating glyph widths was not accurate enough to
  trust. A v1→v2 change sheet ships with it so the plate does not have to be re-checked
  wire by wire.

## Unreleased (2026-08-22, late) — the missed keypresses, root-caused: every release the phone ever saw went to the wrong key

- **THE BUG WAS PRESENT SINCE THE FIRST COMMIT.** Nick: menu scrolling "misses some
  inputs but not much", and — new, and true all along — "tentap typing, sometimes it will
  miss a tap and I have to tap again while scrolling through letter choices."
- **Found by measurement, not by reading.** A raw event trace (`keys raw`, added for the
  purpose) said what the chip actually sends:

  ```
  +0ms    0x41 P    DOWN pressed
  +141ms  0x00 r    ...and the release comes back as key code ZERO
  +1047ms 0x54 P    OK pressed
  +115ms  0x00 r    ...zero again
  ```

  **Key code 0 is CALL in this keymap, so every release this phone has ever seen was
  applied to CALL.** The key actually pressed never had its bit cleared, stayed "held",
  and was therefore deaf to its own next press until the 350 ms stale sweep let go. A
  person re-taps in 150–250 ms — inside that window. It produced double presses too: with
  `keyLastUpMs` stamped for CALL instead of the real key, a real contact bounce (a
  re-press 6 ms after a release, visible in that same trace) met a bounce filter watching
  the wrong key. The vendor code half-knew — `newState < keypadState` is commented "Some
  buttons were released silently" — but that re-armed `keypadState` only.
- **A release carrying code 0 is now attributed to the key that is actually down.**
  Interrupt-driven reads only: an empty FIFO reads back as 0x00 as well, and an interrupt
  is the only thing proving an event happened at all. The UI press edge is `uiKeyDown`
  ALONE — it had been nested inside the `keypadState` edge, making `keypadState` a second
  undocumented veto, so a lost release left the key deaf even after a sweep that cleared
  only `uiKeyDown`. `SN7326::readReg()` also stops discarding a byte it successfully read
  (it was returning the status of a trailing zero-length write, so any NAK there reported
  failure for a key already off the wire).
- ⚠ **THE FIX SHIPPED A REGRESSION, AND A SOURCE REVIEW CAUGHT IT — NO TRACE COULD HAVE.**
  Attribution used `mask = held`, which may carry several bits, and the release branch
  handles a multi-bit mask — that was the bug. Release one of two held keys and BOTH were
  marked up, so the key still under a finger left `uiKeyDown` and its next ~109 ms
  heartbeat arrived looking like a **brand-new press nobody made**. Reachable three ways:
  the Select+Back sleep chord (lift one finger early and the other key fires — a spurious
  Back navigates if the chord had not yet reached SLEEP_CHORD_MS), rolling two-key typing
  (duplicate letter), and the Game Boy (releasing A while holding RIGHT drops RIGHT out of
  `keypadState` for ~109 ms, and `app_gbc.cpp` reads that mask directly). **Attribution now
  requires EXACTLY ONE key down** (`(held & (held - 1)) == 0`); otherwise the byte stays
  decoded as CALL exactly as it always was and the sweep tidies up — no worse than what it
  replaced, and far better than inventing a keypress. Counted as `relamb` so the ambiguous
  case is visible rather than assumed rare. **It needs two keys at once, and every trace so
  far was single-key.**
- **The held-key heartbeat is ~109 ms, not the 40 ms `SN7326.h` claims** — and a bug got
  built on the 40. Nick: "push okay then push the star key to unlock, it immediately thinks
  I'm trying to type the star key in the dialer." The stale-hold rescue used a 100 ms
  threshold taken from that 40 ms comment, *below* the real interval, so every heartbeat of
  a slightly-long press was promoted into a second keypress: the unlock swallowed the
  genuine `*` and the heartbeat went to the dialer. The rescue now counts and does nothing —
  with code-0 releases attributed correctly, releases stop going missing (`swept` sits at 0),
  so it had nothing left to save and one clear way to do harm. ⚠ **Anything keyed on a
  held-key gap must sit well above 109 ms with jitter room and below the 350 ms sweep.
  Re-measure before trusting any number here; the datasheet comment in the driver is wrong.**
- ⚠ **A wrong measurement of my own, corrected in the tree rather than quietly dropped:**
  earlier the same day I recorded that there are *no* held-key heartbeats at all, because an
  early trace showed nothing between a press and its release. Short taps (110–140 ms) simply
  end before the first re-report. There are heartbeats; they are just slower than the driver
  claims.
- **`KEY_BOUNCE_MS` 40 → 25.** This window had never actually been tested: while releases
  were going to CALL it was armed on the wrong key, so attributing them correctly armed it
  for the first time — an untested 40 ms sitting directly in the path of fast same-key
  tapping, which *is* the tentap case. Measured bounces are 6 ms and 13 ms and no human
  re-taps inside 80 ms, so 25 ms clears the real bounce with margin either side.
- **`alphanumericInputEvent()` is no longer called with `event == 0`.** `IS_KEYBOARD()` is
  `event <= 0x7f`, so 0 enters the keyboard block, and the unlock, the screen-off wake and
  the swallowed music transport keys all zero it deliberately. Zero is not the active key,
  so the "different button" branch ran and **committed the pending multi-tap letter** —
  nudge the volume mid-cycle and the letter lands early.
- **Tried and retired by measurement, recorded so they are not re-attempted:** a 40 ms UI
  FIFO poll (added believing the 10 ms INT pulse stranded events — `drained` stayed 0 through
  whole sessions while `empty` ran into the hundreds), and a same-batch bounce exemption (its
  counter never moved, and it is exactly what would let that 6 ms bounce through).
- Instrument: `keys` and `keys raw` on the console, plus a KEYS line into `/health.log` on
  the minute tick only when a counter moves. On hardware after the fix: `relfix` climbing,
  `swept=0`, `killed` catching real bounces. Nick: menu scrolling "feels great", typing "way
  way better", unlock "works it seems". 1167 host assertions green.

## Unreleased (2026-08-22, night) — the 80 MHz menu experiment is backed out, and the SIP flap is named but deliberately left alive

- ⚠ **THE 80 MHz MENU EXPERIMENT IS OFF, AND THE OBVIOUS READING OF WHY IS WRONG.**
  Nick, on hardware: "the menu is a bit laggy and doesn't pick up every button push."
  Missed input is a break, not a tuning problem, so it went OFF rather than being dialled
  down to 160 as offered — and 160 would not have helped. **The evidence contradicts the
  clock explanation: the new "screen idle" state NEVER FIRED**, zero occurrences across
  the whole session, only "busy" and "idle". The phone was never actually running its
  menus at 80 MHz. The lag came from the other half of the change: `cpuRaiseForUi()`
  calls `setCpuFrequencyMhz()` from **inside the key-drain loop**, and a PLL switch landing
  mid-keypad-read is exactly how a keypress goes missing. **The level was never the
  problem, the SWITCHING was — and 160 MHz switches just as hard.**
- `UI_IDLE_DOWNCLOCK 0` restores the previous behaviour exactly (the raise hook is compiled
  out too, so "off" is genuinely off), with six invariants asserted mechanically rather than
  eyeballed. The work and the reasoning stay in the tree for whoever revisits it, with the
  two conditions written down: **raise the clock somewhere that is NOT the input path, and
  MEASURE the saving first** — it was never quantified, and the backlight probably dominates
  screen-on draw anyway.
- ⚠ **SIP registration has flapped REGISTERED → lost → REGISTERED every 60 seconds since the
  initial commit, and it is STILL FLAPPING.** `registrationInvalid()` measures
  `REGISTER_EXPIRATION_S` (60 s) from `msLastRegistered` while `REGISTER_PERIOD_MS` refreshed
  at exactly 60000 — so the registration was declared dead at the precise moment the refresh
  fell due, every cycle. Nobody saw it because the only line reporting it was `log_d`, and
  `log_d` is not compiled into this build. It costs more than a flickering icon: each flap
  fires `REGISTRATION_UPDATE_EVENT`, which redraws, which took the clock to 240 MHz for the
  hold window — once a minute, indefinitely, for nothing, quietly undermining the battery
  work it shares a build with.
- ⚠ **The 45 s refresh that was supposed to fix it was REVERTED, because it was wrong.** It
  did not stop the flap, it moved it to every 45 s. The cause is not the period:
  `registration()` clears `registered` on EVERY refresh (`tinySIP.cpp:1288`) and sets it back
  when the 200 OK lands, so the phone declares itself unregistered for one round trip per
  refresh **by construction**. The real fix is to not clear it while merely refreshing, since
  the expiry check already covers a failed one — **deliberately not shipped**: it is core
  registration state on the phone that owns Nick's number, it needs him present to confirm
  inbound calls still land, and the same evening had already broken his input handling once
  by shipping something that could not be verified. An earlier, deeper attempt worked on its
  own terms (53 flaps in 31 min → 0) and was also reverted — 20 minutes later the phone went
  `lost` and never came back. **The flap is load-bearing: it hides whether refreshes actually
  succeed.**
- **The registration line is promoted `log_d` → `log_e`.** Only `log_e` is compiled into this
  build, so a phone that registers and one that never does looked IDENTICAL on serial — which
  cost real time chasing a registration that turned out to be merely slow on a fresh boot.
  It prints on both edges now. This promotion is the entire reason the once-a-minute flap was
  discovered at all.
- ⚠ **The retransmit ring members were NEVER INITIALISED.** The patch meant to do it aborted
  on a failed assertion *before* writing the file, and the run reported success from the half
  that did apply — so `seenMsgCallId`/`seenMsgCSeq`/`seenMsgNext` shipped with nothing setting
  them while `messageAlreadySeen()` indexes the array with `seenMsgNext`. It corrupted nothing
  **by luck**: `TinySIP sip` is a global, so it lands in BSS and the C runtime zeroes it. But
  `Test.cpp:731` constructs one ON THE STACK, where those members are garbage and the first
  inbound text would index out of bounds. Fixed with member initialisers in the class, which a
  later constructor cannot forget the way a line in the .cpp can. **The lesson worth keeping: a
  scripted multi-edit that asserts per-edit must write the file per-edit or not at all — a
  partial apply that reports success is worse than a clean failure.**

## Unreleased (2026-08-22, evening) — a five-domain battery audit: one real regression, and three bugs that had nothing to do with it

- **"Battery life is a bit worse than it was, recently."** The audit found one thing both
  recent and big enough to feel — and the phone's own health log had already recorded it
  happening.
- 🔑 **The upload server was pinning 240 MHz until someone remembered it.** The transfer
  server is a term in the DFS busy predicate, so while it is up the CPU is pinned at 240 MHz,
  the idle tick runs 5× faster, and the screen sleep timeout stretches to ten minutes —
  roughly **double idle current**. Nothing ever released it: no idle timeout, no client-gone
  timeout, no WiFi-loss timeout. Survivable while the only way to start it was a transfer
  *screen*, whose destructor always calls `xferStop()`; `up on` / `up on books` start it
  **headless**, with nothing to press Back on, and that path was two days old. **Measured, not
  inferred:** last file landed at up=14 min, then **328 consecutive health samples at
  cpu=240MHz with scr=0, from up=10 min to up=92 min** — 82 minutes at double idle draw with
  the screen off, ended only because `up off` was typed by hand. Every single
  240-MHz-with-screen-off sample in 4640 samples across 15 boots is inside that one window.
  It now stops itself after ten idle minutes, deferred by any client traffic on either
  transport, and the stamp is set at start so a server nobody visits still ages out.
- **NTP backs off when there is no clock to be had** — the vendor's own
  `// TODO: increase delay, if there is no Internet`, sitting above that loop since the
  initial commit, and Nick was standing in it on an Android hotspot that blocks UDP 123.
  Unbacked-off, a never-answering NTP woke the task **twice a second forever** and re-sent
  every 2.5 s: ~24 radio wake-and-transmits a minute, indefinitely, against SIP's ~2, and it
  never gives up because there is nothing to give up on. The fast 500 ms poll is still correct
  at FIRST — it is how the reply gets picked up promptly on a working network — so it keeps
  that for ~10 s, then doubles out to a one-minute ceiling. **A working network is completely
  unaffected; a dead one costs about one wake a minute instead of 120.** This is a longstanding
  cost, not the recent regression.
- ⚠ **Two independent save/restore holders over one pair of globals do not nest.** Reading a
  book and running the transfer server both stretched the screen timeouts by snapshotting
  `dimAfterMs`/`sleepAfterMs` into their own fields. Open a book, `up on books`, close the
  book, `up off` — and the last release writes back a value captured before the other holder
  moved it, leaving a **ten-minute screen timeout at full backlight for the rest of the
  session, silently, until a reboot**. Now `ControlState::holdScreenAwake()`, reference
  counted, one snapshot on the first hold, restored by the last release; an unbalanced release
  does nothing rather than restoring a snapshot nobody took.
- ⚠ **`data/configs.ini` shipped `dimming=0` and `sleeping=0` with `bright_level=100`**, and
  the missing-KEY fallbacks agreed with it — while the missing-SECTION branch twelve lines
  below defaults BOTH TRUE. **A phone that got that image ran full backlight until flat,
  silently**, and any `pio run -t uploadfs` would hand a device that state. All three now
  agree: ON.
- **The screen gate is relaxed from "lit" to "lit AND something was drawn in the last 2 s"**,
  so a static menu no longer pays full clock for being stared at; every redraw stamps that
  clock through one funnel in `GUI::redrawScreen` (twelve call sites), and anything with a
  deadline — emulator, transfer server, audio, live SIP — forces 240 regardless. ⚠ **The trap
  the obvious version falls into:** `busy` was ONE boolean feeding both the CPU frequency and
  the 5 ms idle tick; loosen it and a lit screen also gets the slow tick, dragging every pump
  in the loop. The predicates are separated and the tick keeps the ORIGINAL `busy` verbatim —
  **do not re-merge them.** ⚠ **Never target below 80 MHz:** `calculateApb()` returns a flat
  80 MHz for any CPU frequency ≥ 80, so an 80↔240 move reprograms no peripheral at all; below
  80 the source becomes the XTAL, APB follows, and every SPI divider goes wrong.
  *(The user-facing half of this was backed out the same night — see above.)*
- **Also ruled OUT with numbers rather than hand-waving, so nobody re-chases them:** the DFS
  gate itself (structurally leak-proof, one call site, level-triggered; 89% of samples idle
  correctly at 80 MHz), the mesh health-check retry on a plateless phone (~0.1%, and it now
  backs off from 10 s to 60 s after the first two minutes), the GPS UART left enabled with no
  GPS (0.8 mA hard ceiling), and the duplicate-SMS storm (~0.01 mA — three orders of magnitude
  below noticeable, correcting an earlier guess that it mattered).

## Unreleased (2026-08-22, midday) — every text arriving twice, and the SIP receive path had no de-duplication at all

- **Nick, out of town: every text arriving twice or more.** COVEY was POWERED OFF, which
  rules the SMS mirror out completely — both copies came in over SIP, and the SIP receive
  path called `saveMessage()` for whatever arrived, with no de-duplication of any kind.
- **A UDP MESSAGE whose 200 OK does not get back to the server is retransmitted by that
  server** (RFC 3261 timer E/F, up to seven times over ~32 s), and each copy became another
  message in the thread. The phone acks correctly per RFC 3428; the likely reason the ack goes
  unmatched is **a symmetric NAT on an unfamiliar network** mapping the reply to a different
  port than the request arrived on. That is a property of the network, so the phone has to
  tolerate it rather than fix it.
- **De-duplicated on Call-ID + CSeq** — the RFC identity of a retransmission, and exactly what
  a genuine second text does NOT share: send "ok" twice and you get two Call-IDs. **So this
  cannot merge two real messages**, which matters, because people do send the same word twice.
  Eight-entry ring, far more than one transaction's ~7 retries.
- ⚠ **Every copy is still acked, repeats included.** Going quiet on the second one is what
  would guarantee five more.
- ⚠ **It is also an instrument.** If duplicates survive it, the dropped-retransmit log line
  names the Call-ID — and duplicates carrying DIFFERENT Call-IDs would mean VoIP.ms is
  re-pushing the queued text as a fresh transaction after each REGISTER (we re-register every
  60 s) rather than retransmitting one. That is a different fault needing a
  content-plus-window guard instead.
- **The count is what proves it**: a measured triplicate, and phone 2 cleared on four
  independent grounds. COVEY seeing exactly one copy is the load-bearing clue — it puts the
  duplication in SIP delivery, not in the account.

## Unreleased (2026-08-22, morning) — the woods plate is finished, and a radio that lied about existing is made honest

- 🔑 **THE PLATE MADE A NEW FAILURE MODE POSSIBLE, AND IT WAS SILENT.** The radio lives on
  the external pack and the phone can outlive it by hours. Measured on the bench with the pack
  disconnected and the phone alive: **the phone's own driven pins back-fed the whole plate
  through ESD clamp diodes.** `3V3_PLATE` floated at 2.54 V (one diode below 3.3), the 5 V
  node at 2.18 V (backwards through the TLV's high-side body diode — the 0.36 V delta is the
  diode saying so). An RFM95W runs at 2.5 V: **it answered its version probe, the mesh screen
  said READY, RX worked, and TX was silently impossible.** The GPS LED blinked dimly on a plate
  with no battery. **Silent is worse than wedged.**
- **R3–R6 (4 × 1 kΩ, series) go in the four phone-driven lines** — W14 MOSI, W15 SCK, W16 NSS,
  W18 GPS-RX — capping each clamp path at ~2 mA so the rail collapses and **absence reads as
  absence**. W13 MISO and W17 GPS-TX stay plain wire: plate-driven, and the EN gate means the
  plate is never powered while the phone is off. Timing untouched (~15 µs/bit bit-bang against
  ~30 ns of added RC). R7 is the NSS pull-up.
- **`MeshPhy::healthCheck()` — REG_VERSION *plus* REG_OP_MODE**, deliberately not version
  alone: a radio whose pack comes BACK answers 0x12 from POR defaults (FSK standby, LoRa bit
  clear) — present, deaf and mute. Op-mode catches both that and the collapsed-rail case. The
  boot probe gets the same second opinion, a readback of the SLEEP+LoRa write, **so a floating
  MISO cannot fake a radio by producing one lucky 0x12**. Checked every 5 s while READY,
  dropping to `MESH_RADIO_ERROR` loudly; while ERROR it retries a full reinit every 10 s —
  `begin()` is now a thin shell over `reinit()`, so recovery re-runs the entire register config
  rather than trusting whatever survived. A swapped pack rejoins in ≤10 s with a boot-style
  announce, no reboot. Cost while healthy: two bit-banged register reads (~240 µs) every 5 s.
- **Proven on air, both halves witnessing the same hour:** Nick watched the mesh screen turn
  Error with both supplies out, and the serial log caught RECOVERED plus the announce when
  power returned. Plus the day's second discovery — **USB is a legitimate second supply for the
  whole plate**: a power bank runs radio and GPS with no woods pack.
- 🔑 **The woods GPS talks at 115200, not the 9600 written down — and COVEY knew all along.**
  The module was fitted, powered and talking, with bytes climbing and sentences stuck at zero,
  which is the documented wrong-baud signature. Measured by scanning: 9600 → 0 sentences,
  38400 → 0, 57600 → 1, 19200 → 0, 230400 → 0, **115200 → 991 sentences and a live fix**. The
  9600 was copied from COVEY's D-033, which recorded the *plan*; COVEY's own `covey_ui/gps.py`
  has run this module at 115200 since D-062 measured 8.6 KB/s of NMEA off it. **One grep of the
  sister repo would have replaced the whole scan**, and `Hardware.h` now says so where the next
  person will read it. Verified: 22,773 sentences, 9 sats, HDOP 1.3, zero overruns.
- **New: `gps baud <n>`** retunes the live port and persists, applied only while the reader owns
  the UART (the user-serial GUI path keeps `USER_SERIAL_BAUD`), with every owner-changing path
  now retuning — boot, `gps on|off`, and the My node toggle. **`gps raw`** dumps hex+ASCII of the
  last 64 bytes: readable ASCII means the baud is right and something else is wrong, `b5 62`
  means the module is speaking UBX binary, neither means the rate is still off. It ruled out UBX
  here in one command.
- **Wiring sheet v2: the rail is dual-sourced, so a dead pack no longer kills the radio.** D1
  and D2 OR the PowerBoost's 5.2 V and the phone's own VBAT into a single buck-boost — pack
  alive, 5.2 V wins and the phone's cell is untouched (v1 behaviour, kept); pack dead, VBAT
  takes over. **No logic, no firmware, two diodes.** The part has to change with it: v1's
  TLV62569 is a BUCK with a 3.4 V input floor and a 1S cell ends at 3.0 V, so it would drop out
  exactly at the bottom of the discharge — **which is why the original design rejected VBAT and
  recorded the rejection. That objection was about the part, not the idea.** Nothing removes a
  charging path.
- **TPS63020 ordered, and its PS pad gets a decision** rather than being left to chance: PS is
  the power-save select, LOW gives pulse-skipping at light load which is what a plate idling
  around 45 mA wants, **and the pin must not float**. ⚠ Two traps recorded while the part is
  fresh: the 3V3/4V2/5V jumper ships UNSET for our purpose, and this part's EN thresholds are
  tighter than the TLV62569's it replaces — V_IL 0.4 V against the Pololu alternative's 0.7 V.
  The measured 0.22 V EN divider still passes, **with less headroom than v1 had**, which
  promotes checklist item 4 from formality to load-bearing.
- **Two phones now**, so the port gets named rather than assumed.

## Unreleased (2026-08-21, evening) — first power on the plate, and a fuse that was sized for the wrong current

- **First power, steps 1–4 passed:** 5 V rail 5.15 V, plate 3.3 V rail 0 V across C2 and C4,
  identical on the bench supply and the LiPo. **The header 3.3 V pin measures 0.22 V with no
  phone attached, and that single reading is worth more than it looks** — it IS the EN node via
  W12, the divider predicts 0.231 V, and only a continuous W12 plus a correctly-valued R1 plus
  an intact internal pull-up can produce it. It is also far easier to reach with a probe than
  the TLV EN pad, so it is now the documented probe point.
- ⚠ **Mount it cold.** The mesh is live, so the ESP32 drives SPI into an unpowered RFM95W if
  the plate is mated hot, and ten contacts mate in an arbitrary order with 5.2 V sitting on the
  5 V pin. ⚠ **The BT2.0 unplug, not the phone's power button, is the real off switch** — the
  EN gate spares the plate rail, not the 5 V charge path.
- ⚠ **The battery-leg polyfuse was undersized: 2 A hold, not 1 A** — and then **3 A**, once the
  temperature derating was written down. Fitted as a GF300.
- **The last open RF question closes:** it is a v2.2, and R6 tunes a trace this build does not
  have. **R6 is 0 Ω, from WiPhone's own netlist — there is nothing in the antenna path.**

## Unreleased (2026-08-21, later still) — the phone tells the mesh who it hears

- **Neighbour info (portnum 71).** The phone keeps a RAM table of nodes heard with NO relay —
  `hop_start == hop_limit`; **a missing `hop_start` is UNKNOWN, not direct**, because claiming a
  relayed node as a neighbour would draw an edge that does not exist. Announced on the PRIVATE
  channels only: **never the public primary, where it would hand our topology to any stranger
  with a radio**, and never booksync/smsmirror, which carry machine traffic nobody reads.
- My node > Neighbor info cycles off / 1 h / 4 h (1 h is the useful one for a hunting day; 4 h
  matches what the stock module calls its floor). Serial `nbr` lists the table with signal and
  age; `nbr on|4h|off|now` drives it, because the bench cannot wait an hour to learn whether a
  packet is well formed.
- **The encoder is hand-rolled protobuf pinned byte-for-byte to the real runtime's output via
  generated vectors — which immediately earned it:** proto3 elides default-valued scalars, and
  emitting `snr=0` where the reference emits nothing made our packet 5 bytes longer and
  unreadable by comparison. ⚠ Multi-channel announces drip one per pass; three back-to-back
  LongFast sends would stall the superloop into the watchdog.
- Proven on air: COVEY logged `[nbr] !00449040 hears 1 node(s): !62b8d2fd`. 16 host suites green.

## Unreleased (2026-08-21, later) — the phone becomes COVEY's memory: mesh history replay

- **New: mesh history replay** (docs/replay-spec.md). The phone keeps a ring of the
  last 64 channel texts it hears or sends (PSRAM, ~13 KB — internal RAM untouched);
  when COVEY regains its radio after a blind window (lent to the phone app, or
  powered off in a bag) it asks `RPL?` on the booksync channel and the phone replays
  compacted records — original timestamps, oldest first, one packet per 3 s so a
  reply never storms the band, closing with an honest gap marker when history has
  already fallen off the ring. Proven live end-to-end: two messages sent through
  COVEY's own radio while covey-ui slept came back on wake and rendered as OUTGOING
  in its history, timestamps within seconds of truth.
- The wire format is pinned to COVEY's reference implementation by generated vectors
  (tools/gen_replay_vectors.py → tests/test_replay.cpp), down to whitespace grammar
  and uint32 bounds — 15 host suites green. Serial `replay` shows the ring, pending
  replies, and the last served request.
- **Adversarial review caught a privacy leak before the woods did**: legacy
  channel-encrypted DMs (a form this phone deliberately accepts, and which
  decrypts with the CHANNEL key) were entering the replay ring, so a private
  message could have been rebroadcast to every booksync member. Capture is now
  broadcast-only — that dest check is what actually enforces the spec's DM
  exclusion, since "DMs are PKC and unreadable" was never true of the legacy
  form. Also fixed: a reboot used to hide its own losses (coverage was proxied
  by ring fullness, and the ring is empty after exactly the event the gap flag
  exists for) — a coverage-start stamp replaces it, verified on air.
- **Clock lessons, encoded**: record timestamps are `getExactUtcTime()` —
  `getExactUnixTime()` is the LOCAL-shifted epoch and put the first live records
  7 hours off; and the asker pads its window generously (t2 + 600 s) because a
  no-RTC Pi and a drifting phone disagree — a tight window silently served 0
  records with both sitting in the ring. Dedup makes generosity free.

## Unreleased (2026-08-21) — uploads redesigned: chunked, checksummed, resumable, and finally fast-network-proof

- **The upload architecture is new.** Files travel as CRC32-checked pieces, one in
  flight at a time, each acknowledged before the next departs — so a fast network can
  no longer flood internal heap with radio RX buffers (the measured 14 KB → 868 B
  collapse), and a dropped connection, breaker pause, or reboot costs pieces, never
  files: the client asks `GET /chunk` how many bytes the card holds and continues from
  there. Two transports serve the same protocol: a **raw single-connection port
  (8081)** — one TCP connection per batch, bodies read incrementally at the phone's
  own pace, zero per-request heap cost — and the original web server's `/chunk` as
  the multipart fallback the page probes for automatically. Piece size is measured,
  not guessed (16 KB raw / 4 KB fallback; in-flight bytes stay bounded by the 5.7 KB
  TCP window either way). **Acceptance: three consecutive 4-book batches (~52 MB) over
  the fast phone-to-phone hotspot, 12/12 byte-verified, zero breaker events, zero
  panics, 66–97 KB/s (round-trip-bound on that link).**
- **The heap has an instrument now**: serial `heap` prints internal/DMA/PSRAM free,
  largest block, and the boot-long floor; the upload path logs per-file heap floors.
  First fruit: the "some boots have less memory" mystery was uptime fragmentation,
  not boot luck.
- **Truths this work surfaced, now encoded**: `File::size()` on an open FatFS file
  reads the stale directory entry (durable bytes are counted in RAM instead); the
  legacy whole-file POST parser can wedge the main loop if its client vanishes
  (chunked pieces are smaller than the TCP window precisely so that cannot happen);
  breaker teardown cycles inside tight heap leak (pauses no longer fire mid-batch,
  and they persist-without-closing the batch file); the page declares its charset
  (mojibake), keeps the native form fallback for fetch-less browsers, skips
  empty files honestly, and one failed file no longer sinks the rest of the batch.
- **Bench tools**: `tools/chunk_push.py` speaks the whole protocol (both transports,
  `--corrupt`, `--restart-at`, `--resume`, `--gap`, piece-size experiments);
  `tests/test_chunk.cpp` pins the protocol core (verdicts, name safety, CRC vectors).
  ⚠ Flashing now requires panicwatch stopped first — it eats esptool's sync replies.

## Unreleased (2026-08-20, day) — the phone grows a GPS ear, asleep until the plate exists

- **The upload heap saga, evening chapter — fast networks were the killer all along.**
  Push-mode uploads on a fast LAN (3 ms RTT) let TCP open wide and the radio flood
  internal heap with RX buffers faster than the SD drains: heap measured falling from
  14 KB to 868 BYTES inside seconds — while the sluggish phone-hotspot link had been
  masking the whole class by keeping bursts tiny. Defenses now in: mid-upload
  BACKPRESSURE (heap tight ⇒ stop consuming the socket ⇒ TCP window closes ⇒ sender
  stalls ⇒ radio buffers drain), and the breaker defers mid-file to it (trips only on
  catastrophe below 3 KB; between requests the 6 KB line stands). Push uploads on fast
  links now SURVIVE but crawl — the honest recommendation for bulk delivery is the
  page's "paste a download link" PULL path, which lets the phone read at its own pace
  and moved 5 MB books at full speed with heap intact, straight into /books.
- **`up on books`** — the Books uploader is serial-startable now (bench work needs to
  feed the reader with no hands on the phone); `?` lists it.

- **The upload server can no longer strangle the network stack** — the true anatomy of
  "the page locked up", measured live: an upload burst (worst with aborted connections)
  pins 12-15 KB of internal RAM in dead TCP control blocks for minutes, the largest
  free block collapses toward 3 KB, lwIP starts refusing connections, the listener
  dies, and finally the stack goes mute — not even ping answers — while WiFi still
  shows connected. A LOW-HEAP CIRCUIT BREAKER now pauses the server (listener closed,
  nothing lost, honest log line) when the largest internal block drops under 12 KB and
  resumes on its own at 20 KB. Proven on hardware: a stress burst tripped it cleanly —
  instant refusals instead of hangs, phone alive throughout. Per-upload heap telemetry
  (one log line per file) turns future leak-hunting into arithmetic: today's measured
  slope is ~350-400 B lost per upload plus the transient churn. The breaker's first cut
  carried two bugs its own first field trip exposed: a 20 KB resume threshold that
  steady-state fragmentation never reaches (one trip = paused forever), and pause state
  in a function-local static that outlived server restarts. Retuned to the measured
  band — trip 6 KB (uploads run happily at 7-8), resume 10 KB (idle recovers to 13-16)
  — and the state resets on every server start. The SECOND field trip (Nick's kitchen,
  the same evening) exposed the deeper truth: ANY resume threshold can be pinned
  unreachable by the paused server's own allocations, and the softAP fallback race
  (2 s for a flapping association to look connected) tears down the home-WiFi link
  each time it loses. Breaker v3 therefore FREES the server outright on pause
  (delete + MDNS.end — the memory actually comes back) and resumes on heap recovery
  OR a timer with re-trip backoff — a stranded pause is structurally impossible.
- **An upload no longer strands the phone off WiFi** — the reconnect machinery (and the
  auto-switcher) were disabled whenever the transfer server ran, guarding a real softAP
  crash. But the guard also fired in plain STA mode, where it protects nothing: a
  one-second hotspot blink mid-upload left the phone at "SSID not available" with the
  page dead and NO path back until the server was stopped — found live during a 4-book
  upload that ended in a panic-reboot. The gates now block only genuine softAP mode.
  (The panic itself did not reproduce once the stranding was fixed: four 3-5 MB EPUBs
  uploaded back-to-back over a phone hotspot, heap steady. Its habitat — a wedged,
  heap-starved server session — no longer exists; if it ever recurs on the bench cable,
  the watcher will catch the backtrace.)
- **The README tells the truth again** — a six-agent audit checked every public claim
  against the code: the phantom touchscreen, the "may require a couple tries" sleep
  gesture (it works every time; hold Select+Back 2 s), the 4-of-17 serial-command
  table, phonebook advice that 0.9.5's number completion had already made obsolete,
  and a `pio device monitor` tip that printed garbage (monitor_speed was pinned to
  115200 — now 500000, so the plain command works). The 0.9.6/0.9.7 features the
  README never mentioned — PKC DMs, Places, distances, Sun — are in it now, plus a
  What's-new section that surfaces this changelog. Firmware nits the audit caught:
  `sip` was missing from the serial `?` help, and the on-phone flashing help promised
  "fonts, sounds" from uploadfs (it carries config files, the ringtone, the wallpaper).
- **The WiFi auto-switch deadlock is fixed** — the reason the phone sat next to a saved
  hotspot ALL DAY without joining it. The switcher's call-guard blocked on HangUp — a
  TEARDOWN state the END key enters from anywhere, which STICKS when the proxy is
  unreachable (its BYE can never send). So: leave home WiFi, press END once, and the
  thing that restores the network is blocked by a state that needs the network to end.
  Measured live: sip=6 for six hours, 7,549 host-unreachable errors, zero scans, the
  hotspot in range and saved. The guard now blocks only the six genuinely audio-delicate
  states — and only WHILE CONNECTED, because with WiFi down there is no audio a scan
  could glitch: the deadlock is structurally impossible for any stuck state, ever.

- **Sun & legal light is a SCREEN now** (Meshtastic > Sun & legal light): countdown
  first — "LEGAL LIGHT: 13h 34m left" — then the four times, at the reference place;
  the same almanac-verified core the serial `sun` uses. When no place is known it
  says every way to get one, including the GPS toggle one screen away.
- **The GPS enable is a MENU TOGGLE** (Meshtastic > My node > GPS receiver:
  off / on (no fix yet) / on (fix)) — same flag and NVS pref as serial `gps on|off`;
  "fix" is only claimed while the fix is FRESH. Nick's standing rule, adopted today:
  features ship with a UI surface; serial commands are the diagnostics, not the feature.

- **The woods backplate's GPS half is in the firmware**: `nmea.{h,cpp}` reads RMC/GGA
  (checksum-strict, junk-tolerant — wake garbage before '$' is the expected steady
  state on this UART) into the mesh's own fixed-point coordinates, over the stock
  USER_SERIAL UART2 on GPIO 38/32 @ 9600 — the exact pins and baud the plate wires.
  A fix fresh within 2 min becomes the reference between a chosen waypoint and the
  manual pin, so `sun`, node distances and Places run from the phone's own position.
  **Dormant until serial `gps on`** (persists); `gps` prints the counters that
  diagnose a bench (bytes up + sentences 0 = wrong baud). No automatic position
  broadcasts — going on the air stays the manual I'm-here act. 39 new host checks
  against Python-computed vectors; +944 B static, zero heap. Not yet flashed.
- **`pos` names the true missing piece** when waypoints are heard but none chosen
  (it used to claim "no waypoints heard" directly above a listed waypoint).
- **panicwatch is repo-tracked** (`tools/panicwatch.py`) and types a wake newline
  before every command — the first bytes after DFS idle arrive mangled, which cost
  a garbled `pos` this morning; the old leading-`\n` ritual never survived strip().

## 0.9.7 (continued, overnight 08-19→20) — lists that wrap, text that fits, places that expire, and the sun

- **Places live and die like they do on the mesh**: deleting a pin on COVEY now removes
  it here too (the delete packet was being rejected as malformed — it never worked), and
  a place with an expiry ages out on its own clock, not just when a packet mentions it.
- **Long text wraps or says ".."** — the node list is two lines (name / distance+age),
  chats preview their newest message, and every menu, header, label, popup and caption
  in the firmware ellipsizes instead of running under the clock or off the screen
  (a 36-agent sweep found 16 offenders; all fixed, plus the review's follow-ups).
- **`sun`** — dawn / sunrise / sunset / dusk and a legal-light countdown for the
  reference place, offline (NOAA math, almanac-verified). Groundwork for a clock-face
  version.
- Position broadcasts on COVEY can now ride a **chosen channel** (Node & Location) —
  the public default is labeled with the truth: "LongFast — every radio can read this."
- Fixed: two SIP addresses sharing a long prefix no longer merge into one thread;
  `unread clear` survives reboots (a stale partition cache was resurrecting the flags).

## 0.9.7 — the phone knows where everyone is (no GPS required)

### 📍 Places and positions

COVEY broadcasts its GPS position every few minutes and shares waypoints (camp, the
truck, a stand) from its map. The phone now listens: **Meshtastic → Places** lists every
shared waypoint, and the **Nodes** list shows each node as *"Nick H New Device — 1.4km
NE"* measured from a reference you pick (any waypoint, or your own pin). Positions and
places survive reboots.

**"I'm here"** — open a place and declare yourself at it. The phone remembers (and
shows distances from you), and it announces the position to the mesh once, so COVEY's
map shows the phone. It's a statement, not a fix — the phone has no GPS; a future GPS
module would drop into the same slot (see docs/maps-plan.md for the map-app research).

### 📬 The white "new message" icon tells the truth again

Re-mirrored history (like the chip-erase recovery's) arrived unread and buried itself
deeper than the read-marking scan ever looked — the icon stayed lit with nothing visibly
new. Opening a conversation now clears its unread messages at ANY depth, and the new
serial `unread` command counts the truth from the records themselves, repairs the
counters when they disagree, and names a thread that still holds unread.

### 🔧 For the toolbox

- `pos` — every waypoint, every node fix with age and distance, your pin, the
  reference. The whole picture in one paste.
- `unread` — see above; `unread clear` marks EVERYTHING read, for flags orphaned by a
  deleted conversation (nothing left to open = nothing else could ever clear them).
- Fixed: serial `?` help was silently truncated at 192 bytes — every command added
  since `chan` was invisible in the one place a stranded user looks.

### 🥾 First field test, same day

The whole chain worked on air — waypoint received ("vashon"), reference set, COVEY
placed at "3.2km E of vashon" — but the UI hid the payoff: "Measure from here" now
jumps straight to the Nodes list where the distances live (the first field report was
"nothing happened anywhere?"), and the place-menu labels no longer run off the screen.

### 🛡 From the adversarial review (before this ever shipped)

Positions with modern fields (precision_bits) parse correctly; locked waypoints can't
be moved or deleted by anyone but their owner; a recycled node slot no longer inherits
the evicted node's position or crypto key; restored fixes say "old" instead of a
71,000-minute age; a full node table can't corrupt the waypoint store when RAM is
tight; the "I'm here" announce prefers a PRIVATE channel and says so loudly when only
the public default exists — and if the send fails, Status says "set (send FAILED)"
instead of letting silence read as safety.

## 0.9.6 — direct messages work again: the phone speaks Meshtastic's DM crypto

### 🔐 PKC — the fix for "the RAK refuses every DM to this phone"

Modern Meshtastic (2.5+) requires public-key cryptography for direct messages, in **both
directions**: a stock node refuses to *send* a DM to a phone with no known public key
(`PKI_SEND_FAIL_PUBLIC_KEY`), and silently *drops* the old channel-encrypted DM form on
receive ("Rejecting legacy DM"). Channels never noticed; DMs were dead both ways.

This release implements the whole scheme, verified line-by-line against the 2.7 firmware
source: an X25519 identity keypair (generated once, kept in NVS), the public key riding
in every NodeInfo broadcast, peers' keys learned from theirs (trust-on-first-use, like
stock — a changed key is flagged, never silently swapped), and DMs encrypted with
AES-256-CCM under the SHA-256 of the shared secret. Incoming DMs are ACKed — including
re-ACKing retransmissions — so the sender's app shows ✓ delivered instead of timing out.
DMs to a node whose key hasn't been heard yet still go out the legacy way (a pre-2.5
device will take them) with a loud serial note that a modern node won't.

The crypto is self-contained and host-tested against Python's `cryptography` playing the
stock-firmware side — byte-for-byte frames both directions (`tests/test_pki.cpp`), the
same philosophy as the book-sync hashes. RAM cost: +1.7 KB static, **zero heap**; the
X25519 math runs only at superloop depth where its 3 KB of stack is safe.

### 🔧 For the toolbox

- `pki` — our key (base64, comparable against COVEY's node list), every node's key
  state, mismatch flags, and the loop task's stack headroom.
- `announce` — broadcast NodeInfo now, asking hearers to answer with theirs (the fast
  way to swap keys with the RAK after this update).
- `dm <!node> <text>` — send a DM from the cable; the peer's ACK in the serial log is
  end-to-end proof its firmware decrypted us.
- Fixed while in there: a maximum-length message could build a 256-byte frame whose
  length wrapped to 0 in the radio call (sent nothing, claimed success). Now refused
  with a log line.

## 0.9.5 — install from a browser, browse your files, type just the number

### 🔌 Install and update from a browser

**[nikguy321.github.io/wiphone-meshtastic](https://nikguy321.github.io/wiphone-meshtastic/)** —
plug the phone into a computer, open the link in Chrome or Edge, click **Install**, pick
the port. About a minute; nothing to install on the computer. **Updating never touches
your data** — settings, contacts, messages and the SD card all stay; a full factory-reset
install exists only as a clearly-marked advanced option that asks first. The phone's own
**Settings → Firmware update** screen now leads with this route.

### 🗂 A file browser (Menu → Tools → Files)

Browse the whole SD card. Press OK on a file for **Open / Copy / Move / Delete** —
copy and move use a paste-where-you-land clipboard that survives leaving the app, never
overwrites, and can never damage the original (a move only completes after the copy is
verified). Text files open in a pager that wraps like the e-reader. **Upload into this
folder** turns the WiFi upload page toward wherever you're standing — the whole card is
now reachable over WiFi, not just ROMs and books.

### ☎️ Type just the number

New contact or new text: type `4257604281` and the phone completes it to a full SIP
address using **your own account's server** — so it adapts to any provider with nothing
to configure. Addresses with `@` or letters pass through untouched. Also understands a
leading `+` and entries older versions saved as bare numbers.

### 📖 Reader fix

Some lines lost their first character at the margin (a font measure/render disagreement).
Fixed at the root; wrapping is now exact.

### 🔧 For the toolbox

Serial console additions: `chan <url>` applies a Meshtastic channel invite over the cable
(the mesh can't deliver invites to this phone — modern Meshtastic requires public-key
crypto for DMs that this firmware doesn't implement), `chans` lists channels, `sip`
answers "why is texting dead" in one line, `bookpage` dumps the open reader page.

## 0.9.4 — the audit release: hardening, battery, and small courtesies

### 🛡 Hardening (from a 38-agent audit of the whole codebase)

- **Two remotely-reachable memory-safety holes in the phone stack are closed**: a SIP
  message could overflow a stack buffer via long names/URIs, and a peer sending more than
  100 headers wrote past an array. Both now bounded.
- **A latent double-free in message storage is defused** (partition cache switching
  shallow-copied one cache over another).
- The unread-messages counter no longer under-counts after reading a message when unread
  texts span storage partitions.
- In-call volume keys no longer start from uninitialized memory on a fresh flash, and
  what gets saved is what the hardware actually accepted.
- The mirror poll's stall timeout actually fires now (its re-arm condition was always
  true), and typing-screen redraws survive an out-of-memory strdup.

### 🔋 Battery, for days in the field

- **Out of Wi-Fi range, the radio finally rests.** The phone used to hard-cycle the Wi-Fi
  radio every 20 seconds all day when no network was in range — with the driver
  re-probing in between. After five quick retries it now eases to one attempt every
  3 minutes with the radio quiesced in between; picking the phone up or coming home
  reconnects immediately.
- The LoRa radio's status register is read every 10 ms instead of ~1000 times a second
  (it was bit-banged SPI each pass); nothing can be missed — packets take >100 ms on air.
- When truly idle (screen off, no call, no music, no transfer), the main loop ticks at
  5 ms instead of 1 ms — a fifth of the wakeups, imperceptible latency.

### 📱 Small courtesies

- **Reboot asks first** (it was a one-press menu item next to Settings).
- **Redial**: OK on an empty dialer brings back the last number you called (from the
  dialer or the phonebook); OK again calls it.
- **Battery percentage** next to the icon on the clock screen.
- **# on the clock opens Messages** when the envelope icon is showing.
- **Replying to a text returns you to that conversation**, not the conversations list.
- Serial console gains `bookpage` — dumps the open reader page's layout for diagnosing
  rendering reports.

## 0.9.3 — the day-of-real-use fixes: order, buzz, and the reboots

### 🧾 A conversation reads in the order it happened

- **Texts stored before the phone knew the time no longer pin themselves to the bottom
  forever.** A message arriving between power-on and the first clock sync used to be
  stamped "unknown", which sorted as *newest for all time* — so everything after it
  landed above it, i.e. in the middle of the thread. Now, the moment the clock syncs,
  those messages are stamped with the sync time and fall into place. Their shown time can
  be a little late (the phone has no clock chip to do better); their order is right.
- **Two openings of the same conversation now always show the same order.** Messages
  sharing a timestamp were previously ordered by coin toss. Ties now break on the
  provider's own message number when there is one, and deterministically otherwise —
  which also keeps a burst of catch-up texts from COVEY in their true order.

### 📳 Every arriving text announces itself

- **Texts arriving over WiFi from COVEY buzzed nothing and drew nothing** — they simply
  appeared in the store, and worse, they *claimed* the text so the radio copy stayed
  silent too. Both transports now announce through one shared path: buzz for a text
  somebody sent you, silence for mirrored copies of your own, and an open conversation
  refreshes on screen instead of waiting for you to back out and re-enter.
- The buzz now fires **before** the chirp, so if the chirp's wiring ever interferes with
  the vibration motor again, it loses that race instead of winning it.

### 🧯 The reboots while scrolling or opening conversations

All of the found causes are fixed, and the biggest is structural:

- **Parsed message files now live in the big memory bank (PSRAM), not the small one.**
  Opening a conversation parses message files into hundreds of little objects; they all
  came out of the ~20 KB pool the WiFi/phone stack needs to breathe, and a decoded crash
  backtrace caught exactly that running out. Those objects now go to the 3.6 MB bank.
- **A cached page no longer drags a second message file into memory for nothing**, and a
  long-standing accounting slip (the second file reporting the first file's number) is
  fixed with it.
- **Scrolling a conversation no longer allocates at all** — the redraw made up to 13
  little text copies per keypress, unchecked, and a failed one crashed the phone with the
  trigger "while scrolling". It now draws in place.
- **Running out of room while laying out text truncates the text** instead of writing
  past the end of a block — an out-of-memory moment used to become silent corruption and
  a reboot with no readable crash record.
- **After a reboot, the phone no longer re-downloads and re-checks the entire mirrored
  history from COVEY.** It remembers where it left off on the SD card. (Delete
  `/roms/smsmirror.since` to force a full resync on purpose.)

### 🔇 Housekeeping

- Deleted leftover debug spam that wrote to the field log on every settings keypress.
- Fixed a dormant accessor that returned a message's time when asked for its delivery
  time.

## Unreleased — texting looks like texting, and two devices share one number

### 💬 Messages are conversations now, not an inbox and an outbox

- **One row per person, newest first.** Open it and you see the whole exchange in order,
  oldest at the top, with `You:` on the ones you sent. Replying is one press — it
  already knows who you are replying to.
- The old **Inbox / Sent** split is gone. It was never how anyone thinks about texting;
  it was just how the messages happened to be filed on the card.
- **Unread counts sit on the row**, and opening a conversation clears it.
- The same person is one conversation however their number is written — with the country
  code, without it, punctuated, or as a full address.
- ⚠ The list shows your **recent** conversations, not every message you have ever had.
  Someone you have not texted in a very long time may not appear.

### 📨 Texts sent from another device show up here too

If you run a second device on the same phone number — this was built for COVEY, a
Raspberry Pi handheld — the texts it sends now appear on the phone as well, so the two
do not drift apart.

- **Two ways in, and they cover different gaps.** Over **WiFi** you get the full history,
  including texts that arrived while the phone was off. Over the **radio** (LoRa) you get
  new texts anywhere in range, no WiFi needed. Running both is fine — the phone
  recognises a text it already has and will not show it twice.
- **Setup:** put a two-line `smsmirror.txt` on the SD card — the other device's address
  on the first line, its shared token on the second. Upload it with the same WiFi upload
  page you use for ROMs and books. Nothing happens until that file exists, and the phone
  says so on the serial log rather than sitting there silently.
- ⚠ **Give it its own mesh channel** rather than reusing one. A channel invite contains
  the key, so sharing it hands over everything else on that channel too.
- ⚠ **The WiFi side is unencrypted**, because this phone cannot do HTTPS at all (see
  below). The token stops a stray browser, not somebody watching the same network. On a
  network you do not trust, turn the WiFi side off and let the radio carry it — the radio
  side *is* encrypted.

### 🔌 A serial console, for the things you can otherwise only reach by tapping the screen

Plug in USB, open a terminal at 500000 baud and type `?`. You can start and stop the WiFi
upload page, ask the phone to fetch texts right now, and check what it thinks its state
is — useful when the phone is on a bench and you are not holding it. No password:
whoever has the cable has the phone.

### ℹ️ Why the phone cannot fetch its own texts from the provider

The obvious design would be for the phone to talk to the provider directly. It cannot,
and it is worth writing down so nobody spends a day rediscovering it: a secure connection
wants about 33 KB of a particular kind of memory, and the whole phone has about 31 KB of
it. No setting changes that, and the usual workaround for small devices does not exist in
the version of the encryption library this build is stuck on. It is the same wall that
stops over-the-air firmware updates working.

## Unreleased — it makes phone calls now

### ☎️ Calls and texts work, on a real phone number

- **The phone can call and text ordinary phone numbers, both directions.** A VoIP.ms
  number, over WiFi. Calling out, texting out, ringing on the way in, and texts
  arriving — all confirmed on real hardware against a real mobile.
- ⚠ If inbound calls give a **busy signal** and inbound texts never arrive while
  outbound works fine, the number is routed to the wrong account at the provider,
  not broken on the phone. Point the DID at the sub-account the phone registers as.
- **Put the full address in the phonebook** — `15551234567@yourserver` — not just the
  number. Calling fills in the server for you; texting does not, so a bare number
  calls fine and silently fails to text.

### 🔕 A ringer you can silence

- **Settings > Audio → Ringer:** Ring + vibrate, **Vibrate only**, or Silent. The phone
  always vibrated on an incoming call; now you can keep the buzz and drop the noise.
  Remembered across reboots, and applied from the moment the phone starts rather than
  the first time you visit the settings screen.

### 📬 The status bar says what kind of message is waiting

- **A white envelope means a text; a green one means a Meshtastic message.** Both
  waiting shows them overlapping. Read one and the icon collapses to whichever kind is
  still unread, so it tells you what is left rather than just "something arrived".

### 🧊 The phone stopped freezing for seconds at a time

- **Those five-second freezes while clicking around were not crashes** — the phone was
  stuck waiting on a network name lookup, and everything on screen shares one thread.
  It was asking the local network about internet addresses it could never answer for,
  half a second each time, and retrying a failing lookup twice a second. It now skips
  the pointless question and remembers failures instead of repeating them.

## Unreleased — typing, and honest buttons

### ⌨️ Typing the same letter twice no longer means waiting

- **The middle of the D-pad now means "yes, that letter".** Typing `AZAZ` was
  always quick, because moving to a different key finishes the letter you were
  on. Typing `AAA` was not: pressing `2` again cycles `a → b → c`, so the only
  way to get a second `a` was to stop and wait two seconds. Now you press the
  key, press OK, and it is committed — as fast as your thumbs go, the way old
  phones worked.
- **The two-second wait still works exactly as before.** OK is an extra way to
  finish a letter, not a replacement, so nothing you already do stops working.
- **Sending is unchanged.** The top-left **Send** key and the green call key
  still finish the letter you are on *and* send, in one press. Only the middle
  of the D-pad changed, and only while a letter is waiting.

### 🏷️ The button under "Cancel" never cancelled

- **Four screens labelled a button "Cancel" when it actually deletes a
  character** — writing a message, editing your node name or short name, and the
  book sync settings. The key under that label is backspace; cancelling is the
  key *below* it. They now say **Clear**, which is what the rest of the phone
  already called it.

### 🔌 Firmware updates: honest instructions instead of a broken button

- **Settings > Firmware update** used to offer a Check button that could only
  ever fail. It now shows **step-by-step instructions for updating over USB**,
  on the phone's own screen, so you can read them while holding the phone —
  scroll with up/down, jump a page with left/right.
- **Why the old one could not work:** the phone cannot open a secure connection
  at all. It needs about 33 KB of fast memory to do so and has about 19 KB in
  total. That is a hardware limit, not an expired certificate or a stale link,
  and no amount of retrying was ever going to fix it.
- **The phone also stopped trying at every startup.** It had been attempting that
  same impossible connection on every single boot — quietly, with nothing on
  screen to say so — and the attempt costs both time and the scarce memory the
  phone then goes on to need. Measured on the device afterwards: the largest
  block of free fast memory at startup went from about 14.4 KB to about 15.9 KB,
  which is the number that decides whether the phone stays up.
- The new screen costs the phone **less** memory than the settings form it
  replaced, and does not allocate anything to display its text.

## 0.9.2 — book sync works, stability, and Game Boy speed

⚠ **Known limitation: over-the-air updates do not work on this phone**, and the
reason turns out to be memory rather than anything to do with the link. A secure
connection needs about 33 KB of the phone's scarce fast memory and there is only
about 19 KB of it, so the connection can never be opened. Install over USB. An
earlier note claiming the updater was working again was only half right — the
expired certificate it fixed was a real problem, but not the one stopping it.

### 📖 Reading position sync works between devices

- **Your place in a book now travels over the mesh, for real.** Read on the phone,
  tap *Sync my place*, and the other device offers to jump to where you got to.
  This had been built and tested against recorded data for months but had never
  been tried between two actual devices until now.
- **It works both ways** — phone to COVEY and COVEY to phone.
- **Only the device you are reading on needs the tap.** Tapping *Sync my place*
  sends your position out; the other device picks it up on its own, whatever it
  happens to be doing. You do not have to tap on both.
- ⚠ **Worth knowing, because it looks broken the first time:** the receiving
  device shows nothing at all when the position arrives. Radio messages are slow,
  so it is saved quietly and offered **the next time you open that book**. If sync
  seems to do nothing, open the book on the other device before assuming it failed.
- **Books > menu > Sync settings** tells you what is going on: whether the shared
  channel exists, and how many positions are waiting to be picked up.

## Stability, and Game Boy speed

A day spent on the phone restarting by itself. The cause is now identified and
fixed, a settings screen that had been quietly broken works again, the Game Boy
runs at full speed again, and the tools for reading what the phone was doing no
longer make the problem worse.

### 🔊 Notification sounds stop hijacking the speaker

- **A Meshtastic notification used to permanently change your audio settings.** The
  little "pop" reconfigures the speaker to play itself — mono, loudspeaker, full
  volume, low quality — and then never put any of it back. So one message left the
  phone that way for good: music afterwards played in mono out of the loudspeaker
  even with headphones plugged in, and the Game Boy dropped to half speed, because
  the emulator takes its timing from the speaker.
  Notification sounds now put everything back exactly as they found it. This was
  most likely the real cause of the half-speed games — a message arrives far more
  often than anyone opens the music player.
- ⚠ Still true: a notification stops whatever track is playing. That is a separate
  problem and is not fixed yet.

### 🎮 Game Boy back to full speed

- **Games were running at half speed.** The emulator deliberately lets the audio
  hardware set its pace — it hands over a frame of sound and waits for the
  speaker to be ready for more, which keeps the game locked to real time with no
  drift. But it only ever told the audio hardware *what rate* to play at, never
  whether the sound was stereo, and it quietly relied on whatever the last thing
  to use the speaker had left behind.
  Once the music player started switching between stereo and mono to follow the
  headphone jack, the Game Boy could inherit mono — and then the hardware drained
  its sound at exactly half the expected rate, so the game waited twice as long
  for every frame and ran at half speed. It now sets both, and never inherits.
- ⚠ Note for the future: the on-screen speed readout going orange, and the
  emulator dropping display frames to compensate, both look like "the processor
  is too slow". Neither was. Skipping frames cannot fix a game that is being
  paced by sound.

### 🔋 Battery

- **The phone could get stuck running flat out, on battery, with the screen off.**
  If a call ended while there was no signal, it waited forever for a confirmation
  that could never arrive — and while it waited, it treated itself as being on a
  call and never slowed the processor down. Caught in a car log: nineteen straight
  minutes at full speed, screen off, out of range. That is roughly double the
  idle drain, in exactly the situation where battery matters most.

### 🔁 The random restarts

- **Opening Books was quietly eating the phone's working memory.** Every time you
  opened it, about 3 KB of the phone's small pool of contiguous memory went and
  did not come back — and after enough opens, the next thing that needed a
  contiguous block could not get one and the phone restarted. It now lives in the
  large memory the phone has plenty of. Measured before and after: the cost of
  opening Books fell from about 3,000 bytes to 140.
- ⚠ Note for anyone reading older notes: Books' big *lists* were moved out of the
  scarce memory a while ago, but the app itself was left behind. "Books was fixed"
  was only half true, and the remaining half is what kept causing restarts.

- **The likely cause is fixed.** Every time the phone tried to join WiFi it
  registered another copy of its network event handler, and never removed any of
  them. Out of range it retries every 20 seconds, so a drive with no signal
  stacked up hundreds of copies — one measured journey was 143 minutes without
  service, which is around 430. The moment signal came back, **all** of them ran
  at once, each tearing down and rebuilding the same network buffers. That churn
  breaks the phone's small pool of working memory into fragments until something
  eventually cannot find the unbroken space it needs, and the phone reboots.
  It is registered once now, when the phone starts, instead of on every attempt.

- **The phone can now say why it restarted, and it has been answered.** Four
  restarts were captured and every one of them was a crash — not a flat battery,
  not a stall, not the power being interrupted. The battery never dropped below
  3.99 V across the whole record, which rules that out entirely.

- **First real evidence it worked.** An 85-minute stretch out of signal — the same
  condition that caused a restart in the car — with the phone's free-memory figure
  not moving by a single byte, at nearly three times the headroom the failing run
  had. Still not a complete proof: the phone never rejoined a network in that
  window, which is the other half of the problem.

- **The health log no longer throws away its own evidence.** It kept a fixed
  amount of history and, when full, deleted all of it and started again. That is
  exactly what happened during a restart nobody was watching: the record of *why*
  it restarted was erased half an hour later, before it could be read. It now
  keeps the most recent few hours instead.

### 🧭 Settings

- **Settings > WiFi auto-switch works again.** It was showing the Music icon, and
  choosing it opened the Music player instead of the toggle — so the screen was
  effectively unreachable. The two entries had been given the same internal ID and
  the menu picks the first match, so the WiFi row was standing in for Music.
  Renumbered.

- **The phone now checks its own menu for repeated IDs when it starts.** The same
  mistake had already shipped once before, as "Music opens Books", and was fixed
  in a way that moved the clash rather than removing it. Nothing was watching for
  it, so now something is.

### 🩺 Diagnostics

- **The health log can be read without disturbing what it measures.** Pulling the
  whole file kept truncating, and a truncated read loses the *end* — which is
  exactly where the reason for the last restart is written. Retrying was worse
  than the problem: around thirty fetches, each its own connection, on a phone
  whose scarcest resource is the memory all of that consumes. It can now hand back
  just the most recent part, so one request gets what matters.

## 2026-08-14 — Music player, e-reader, real battery life, and working firmware updates

The biggest release so far. Two whole new apps, a serious pass on power, and the
over-the-air updater brought back from the dead and pointed at this repo.

### 🎵 Music player (new)
- **Plays MP3 and WAV straight from the SD card** — no converting anything.
  Menu > Music. Stereo through the headphone jack, mono to the loudspeaker
  otherwise, and it follows the jack live: unplug mid-song and it carries on out
  of the speaker from the same place.
- **The four side buttons are the transport**, from the top: play/pause, next
  (**hold** for previous), volume up, volume down. Holding "previous" behaves
  like any music player — within 3 seconds it goes back a track, after that it
  restarts the current one.
- **Playback keeps going when you leave the screen.** Read a book or check the
  mesh with music playing; tracks advance on their own.
- Shuffle and repeat (off / all / one), a library scanned from `/music`,
  `/books`, `/roms` and the card root, and its **own WiFi uploader**.
- **Volume starts low** (−18 dB) and is remembered until the phone restarts. It
  is deliberately separate from the call volume, so a quiet album can never
  leave you unable to hear the next phone call.
- Pausing and playing **resumes where you were** rather than restarting.

### 📖 E-reader (new)
- **Reads EPUB and plain text from the card**, with pictures inline. Menu >
  Books.
- **Your place is saved** and survives the phone losing power at any instant.
  Position is stored as (chapter, character offset), not a page number, so it
  still means something after a font-size change — and on a different device.
- **Greyscale JPEGs decode**, which the ESP32's built-in decoder cannot do
  (it is 3-component only, and most book art is 1-component).
- **Book sync over LoRa** — share your reading position with another device on a
  private channel. Jumps are confirmed, never taken silently.

### 🔋 Battery life
- **The phone now idles.** The main loop used to spin flat out at 240 MHz
  forever, screen off, doing nothing. It now sleeps between passes.
- **The CPU drops to 80 MHz when nothing is happening** and returns to 240 the
  moment the screen comes on, music plays, a call starts, the emulator runs or
  an upload begins.
- **WiFi modem sleep is on.** The receiver used to stay powered continuously;
  it now parks between beacons. Nothing is missed — buffered traffic still
  arrives, a fraction of a second later.
- **Scanning backs off when there is nothing to join.** Out of range, it used to
  scan every 2 minutes forever; now it eases to 5 after a sustained absence, and
  snaps back the moment anything connects.

### 🛠 Diagnostics
- **The phone records why it restarted.** The reset reason (crash, watchdog,
  brownout, or a normal power-on) is logged at boot, which turns "it rebooted
  again" into a specific answer.
- **A health line every minute** — free memory, the lowest it has ever been,
  largest free block, battery, CPU speed — written to `/health.log` on the card
  so it survives both a reboot and being unplugged.
- **Read it over WiFi** at `http://wiphone.local/log` with an upload screen open.

### 📤 File transfers
- **The upload page works on a phone now.** It was built around drag-and-drop,
  which does not exist on a touch screen; there is a proper "Choose files"
  button, and the file picker no longer greys everything out.
- **It stops after one page load — fixed.** The server kept the single
  connection slot occupied, so nothing loaded again until it was restarted.
- **Opening an uploader no longer knocks the phone off WiFi.** If the radio was
  still associating it would tear the connection down and host its own network
  instead; and on the way out it never reconnected.
- **Uploads are faster and retry themselves** — a weak link no longer means
  starting over by hand.
- **The screen stays awake while a server is running**, so a long transfer does
  not go dark halfway and look like a crash.

### ⬆️ Firmware updates over the air
- **Settings > Firmware settings works again**, and now points at this repo.
- It was not a dead link: the certificate the phone pinned **expired in April
  2021**, so every check failed the TLS handshake regardless of the URL. It now
  trusts a proper root certificate (valid to 2035) instead of a pinned leaf.
- Releases are published from `ota/` in this repo; `tools/publish_ota.sh` builds
  the binary, writes the manifest and bumps the version together.

### Known issues
- **Slight crackle in music playback.** Much improved — the decoder used to be
  rate-limited so it could never catch up after any interruption — but not
  entirely gone. The now-playing screen shows a `gaps:` counter to help pin it
  down.
- **No seeking within a track**, and no track/artist tags: the library lists
  filenames.

## 2026-07-09 — Reliable menu input, forgiving screen-off, working WiFi auto-switch

### Input
- **Menus register one press per press.** Buttons no longer double-fire, and
  holding the d-pad no longer auto-scrolls the menu — UI key events are now
  strictly edge-triggered (one event per physical press, re-armed only by a
  real release). The Game Boy emulator's input path is untouched.
- **Triple-tap-to-sleep is easy now** — three taps of the top-right button
  with up to ½ second between them (was a near-impossible fixed 0.7 s window
  for all three).

### WiFi
- **Auto-switch actually works.** It reliably scans and connects to the
  strongest saved network now (previously it could sit disconnected
  indefinitely). It also scans more often while disconnected (every 2 minutes)
  so it finds a network without waiting.

## 2026-07-07 — Game Boy polish, big-ROM support, WiFi auto-switch, input reliability

### Game Boy emulator
- **Sound!** The emulated Game Boy APU plays through the phone's speaker
  (32 kHz stereo over I2S). The **top two side keys** adjust volume in-game.
  Audio can never slow the game down: if the audio path misbehaves, the game
  drops sound and keeps running at full speed.
- **Full speed on heavy games** — even demanding Game Boy Color titles
  (e.g. Resident Evil Gaiden: 44% → ~100%). How: removed a PSRAM silicon
  workaround the phone's rev-3 chip doesn't need, hot emulator paths moved to
  zero-wait IRAM, `-O2/-O3` on the emulator core, cache-friendly screen
  blitting, and adaptive frameskip (heavy scenes drop *display* frames, never
  game speed).
- **Big ROMs (4 MB carts) now playable** — ROMs too large to hold in RAM are
  streamed from the SD card in 16 KB banks on demand.
- **Save states fixed** — saving used to reboot the phone (a task stack was
  4× smaller than intended).
- **Transfer ROMs & Help moved into the Game Boy app** — top two rows of the
  game list. The help screen documents controls and everything else.
- **Multi-file ROM upload fixed** — dropping several ROMs at once used to
  reboot the phone (RAM starvation + watchdog); uploads now go one-per-request
  with per-file progress, and the transfer screen frees the emulator's memory
  and the unused Bluetooth reserve before serving.
- **Reliable buttons** — held directions no longer stutter or stick: fixed a
  keypad-handler bug that wiped held keys, added I2C error retry, periodic
  FIFO draining, and a 40 ms hardware "still pressed" heartbeat so a lost
  release un-sticks in a third of a second.
- Long ROM names marquee-scroll in the picker; honest error screens (a ROM
  that can't load no longer silently launches the built-in game).

### Phone
- **WiFi auto-switch** — the phone periodically scans (battery-friendly, async)
  and hops to the strongest *saved* network; if the screen wakes with no
  connection it scans immediately. Only switches when meaningfully stronger
  (10 dB hysteresis). Toggle under **Settings → WiFi auto-switch**.
- **Status bar shows the connected network name** (small text, auto-shortened).
- **"Edit current network" no longer freezes** — a toggle handler ran on every
  keystroke and blocked 5 s reconnecting each time.
- Pressing End during a game no longer triggers the phone's call/hang-up UI.

## 2026-07-05 — Meshtastic: channels, relaying, unread counts

- Multiple channels with AES-128/256; import channels from a Meshtastic share
  link fully offline (apply from a received DM).
- Mesh client role: relays other nodes' packets (flood routing) to extend range.
- Configurable hop limit; per-chat unread counts; status-bar unread icon.
- Game Boy Color emulator first milestones: playable → full-speed dual-core,
  ROM picker, WiFi ROM transfer, save states, pause menu, 1:1/Fill scaling.

## 2026-07-04 — Initial public release

- PlatformIO build of the stock WiPhone firmware (Arduino-IDE only upstream).
- Meshtastic on the WiPhone's SX1276 LoRa radio: two-way encrypted text with
  standard Meshtastic nodes on LongFast (US), chats/threads UI, direct
  messages, node names, persistent history, notifications (icon, popup, sound,
  vibration), editable node name, low-power green/black theme.
