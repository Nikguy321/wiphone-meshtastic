# WiPhone Meshtastic + Game Boy Firmware

Custom firmware for the **[WiPhone](https://www.wiphone.io/)** (an open-source
ESP32 cell phone) that adds:

- a two-way **[Meshtastic](https://meshtastic.org/)** radio node (the WiPhone's
  built-in LoRa radio speaks Meshtastic's on-air protocol — encrypted text with
  standard Meshtastic devices, no phone, app, or internet required),
- a full-speed **Game Boy / Game Boy Color emulator** with sound, save states,
  and drag-and-drop ROM upload over WiFi,
- and phone quality-of-life fixes (WiFi auto-switching, input reliability).

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

---

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
