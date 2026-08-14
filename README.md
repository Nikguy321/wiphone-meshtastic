# WiPhone Meshtastic + Game Boy Firmware

Custom firmware for the **[WiPhone](https://www.wiphone.io/)** (an open-source
ESP32 cell phone) that adds:

- a two-way **[Meshtastic](https://meshtastic.org/)** radio node (the WiPhone's
  built-in LoRa radio speaks Meshtastic's on-air protocol — encrypted text with
  standard Meshtastic devices, no phone, app, or internet required),
- a full-speed **Game Boy / Game Boy Color emulator** with sound, save states,
  and drag-and-drop ROM upload over WiFi,
- a **music player** (MP3 and WAV, stereo, hardware transport buttons, plays on
  while you use the rest of the phone),
- an **e-reader** (EPUB and text, pictures inline, your place saved, and
  position sync to another device over LoRa),
- **over-the-air firmware updates** from this repo,
- and phone quality-of-life fixes (real battery life, WiFi auto-switching,
  input reliability).

All of the WiPhone's normal phone / SIP / menu features stay intact.
Built with **PlatformIO** (the stock WiPhone firmware is Arduino-IDE only).
See [CHANGELOG.md](CHANGELOG.md) for what's new.

---

## Game Boy Color emulator

Main menu → **Games → Game Boy**. Based on the retro-go fork of gnuboy, tuned
until real Game Boy Color games run at full speed on the phone.

- **Full speed with sound** — even heavy GBC titles. Sound plays through the
  phone's speaker; the **top two side keys** are volume up/down in-game.
- **Save states** — bookmark any game exactly where you are (pause → Save
  state), one slot per game, stored on the SD card.
- **Get games in over WiFi** — pick **Transfer ROMs...** in the game list: the
  phone becomes a tiny website (`wiphone.local`); drag `.gb`/`.gbc` files onto
  it from any computer. Multiple files at once, per-file progress. Falls back
  to hosting its own hotspot when not on WiFi.
- **Big carts work** — 4 MB ROMs stream from the SD card on demand.
- **Two screen modes** — crisp 1:1 or 1.5× Fill, toggled from the pause menu,
  which also shows the measured game speed %.
- **In-app help** — the **Help...** row documents the controls and everything
  else a new user needs.

Controls: D-pad moves, bottom-right side key = **A**, the key above it = **B**,
Back = **Start**, Select = **Select**, End (hang-up) = **pause menu**.

---

## Music player

- **MP3 and WAV straight from the SD card** — nothing to convert. Menu > Music.
- **Stereo** through the headphone jack, loudspeaker otherwise, and it follows
  the jack live: unplug mid-song and it carries on out of the speaker from the
  same spot.
- **The four side buttons are the transport**, top to bottom: play/pause,
  next (**hold** = previous), volume up, volume down. Hold-for-previous does the
  usual thing — back a track within 3 seconds, restart the current one after.
- **Keeps playing when you leave the screen.** Read a book or check the mesh
  with music going; tracks advance on their own.
- Shuffle, repeat (off / all / one), and its own WiFi uploader.
- Volume starts low and is separate from the call volume, so a quiet album can
  never leave you unable to hear the next call.

Decoding is the **Helix MP3 decoder** (RealNetworks, RPSL — vendored under
`WiPhone/src/audio/helix-mp3/`), with its ~29 KB of working memory placed in PSRAM, because the
internal heap on this phone has nothing like that spare. Measured on the device:
about a quarter of realtime at 48 kHz stereo, so there is plenty of headroom.

## E-reader

- **EPUB and plain text** from the card, with **pictures inline**. Menu > Books.
- **Your place is saved** and survives the phone losing power without warning.
- Position is **(chapter, character offset)**, never a page number — so it still
  means something after you change the font size, and on a different device.
- **Greyscale JPEGs decode**, which the ESP32's built-in decoder cannot do at
  all (it handles 3-component colour only, and most book art is 1-component).
- **Book sync over LoRa** — share your reading position with another device on a
  private channel. Jumps are confirmed, never taken silently.

## Meshtastic features

- **Two-way Meshtastic text messaging** on the default LongFast channel (US),
  AES-encrypted and interoperable with regular Meshtastic nodes.
- **Multiple channels** — import custom channels from a Meshtastic share link
  (decoded on-device, no internet). Each channel shows up as its own chat.
- **Direct messages** to specific nodes.
- **Node list** with friendly names (learned from NodeInfo), an **editable node
  name**, and a **configurable hop limit**.
- **Mesh client role** — relays/rebroadcasts other nodes' packets to extend the
  mesh's range (flood routing).
- **New-message notifications** — a status-bar icon (with unread counts per chat),
  a brief on-screen popup, plus a quiet "pop" sound and a short vibration
  (designed to be unobtrusive — e.g. usable as a communicator while hunting).
- **Persistent** message history and channels across reboots.
- **Low-power green/black UI theme** for the Meshtastic app, and a Meshtastic
  icon on the main menu.
- **Handy extras** — triple-tap the top-right button to turn the screen off.

The normal WiPhone experience (phone calls, SIP, contacts, games, settings) is
untouched.

---

## Phone improvements

- **WiFi auto-switch** — the phone quietly scans in the background and hops to
  the strongest *saved* network (with hysteresis, so it doesn't ping-pong);
  waking the screen with no connection triggers an immediate scan+connect.
  Toggle under **Settings → WiFi auto-switch**.
- The **status bar shows the connected WiFi network's name**.
- Fixed the **"Edit current network"** screen freezing on input.
- **Keypad reliability** — fixes for missed taps, stuck buttons, and held keys
  releasing at random (I2C error retry + a 40 ms hardware key heartbeat).
- **Real battery life.** The main loop used to spin flat out at 240 MHz forever
  with the screen off; it now sleeps between passes, drops the CPU to 80 MHz
  when nothing is happening, and lets the WiFi receiver park between beacons.
  Full speed returns instantly for the screen, music, a call, the emulator or an
  upload.
- **The phone records why it restarted** — crash, watchdog, brownout or a normal
  power-on — plus a health line every minute (memory, battery, CPU speed) to
  `/health.log` on the card. Readable over WiFi at `http://wiphone.local/log`
  with an upload screen open, so it survives being unplugged and rebooted.
- **File transfers work from a phone.** The upload page had been built around
  drag-and-drop; it now has a proper file button, stops wedging after one page
  load, retries a failed upload, keeps the screen awake while transferring, and
  no longer knocks the phone off WiFi when it opens.

---

## Firmware updates over the air

**Settings > Firmware settings** checks this repo and installs a newer build.

The phone reads `ota/wiphone-ota.ini` from `main`, compares its `version` against
the running firmware, and offers the update only if it is higher.

To publish one:

```bash
tools/publish_ota.sh 0.9.1     # builds, stages ota/, bumps the version
git add -A && git commit -m "Release 0.9.1" && git push
```

For a public repo, pushing *is* releasing — the phone reads straight from `main`.

## Applying a channel setup link

Meshtastic shares channel setups as a link that looks like:

```
https://meshtastic.org/e/?add=true#CgcSAQE6AgggCjISIA-e2sF5bAtEulPtEtsB…
```

**Only the part after the `#` matters.** Everything before it
(`https://meshtastic.org/e/…`) is just a web redirect and contains no channel
data — the base64 text *after the `#`* is the entire channel configuration.

**To add channels to the WiPhone:**

1. On any Meshtastic device or the Meshtastic app, open your channel's **share /
   QR link** and copy it.
2. Send that link to the WiPhone **as a Meshtastic direct message**. You can send
   the whole URL, or — if it's too long to fit in one message — just the part
   **after the `#`** (that fragment is usually short enough on its own).
3. On the WiPhone: open **Meshtastic → Chats**, open the direct-message thread
   from that sender, **select the message** containing the link, and press
   **OK ("Apply link")**.
4. The custom channels are imported and appear as their own chats. They persist
   across reboot.

Applying a link **adds/merges** channels (it keeps your existing ones, including
LongFast). Decoding happens entirely on the device — **no internet needed.**

---

## Hardware

Made for **WiPhone** hardware:
- ESP32-WROVER (240 MHz, PSRAM, 16 MB flash)
- Semtech SX1276 LoRa radio (RFM95W), US 915 MHz / LongFast
- ST7789 display, CP2104 USB-to-serial

---

## Flashing (beginner-friendly)

You'll build and flash the firmware with **PlatformIO**. On a Mac / Linux /
Windows machine:

**1. Install Python 3** (if you don't have it): <https://www.python.org/downloads/>

**2. Install PlatformIO Core:**
```bash
pip3 install -U platformio
```

**3. Get this code** — clone or download this repository (use the green **Code**
button at the top of the page), then open a terminal in the project folder.

**4. Plug in the WiPhone** via USB. (On some systems you may need the Silicon Labs
CP210x USB-to-UART driver; macOS usually has it built in.)

**5. Build and flash the firmware:**
```bash
pio run -t upload
```
PlatformIO auto-detects the WiPhone's serial port. If it can't find it, list ports
with `pio device list` and add `--upload-port /dev/<your-port>`.

**6. First time only — also flash the filesystem** (fonts, config, the
notification sound, etc.):
```bash
pio run -t uploadfs
```

That's it — the WiPhone reboots into the firmware. Open the main menu and look for
**Meshtastic**.

> Tip: to watch debug logs, run `pio device monitor` — note the WiPhone's serial
> runs at **500000 baud**.

---

## Credits & license

Based on the official [WiPhone firmware](https://github.com/HackEDA/wiphone-firmware)
by HackEDA, Inc., distributed under the **WiPhone Public License v1.0** (see
`LICENSE`). Meshtastic is a trademark of Meshtastic LLC; this is an independent,
community project, not affiliated with or endorsed by Meshtastic.
