# WiPhone Meshtastic + Game Boy Firmware

Custom firmware for the **[WiPhone](https://www.wiphone.io/)** (an open-source
ESP32 cell phone) that adds:

- a two-way **[Meshtastic](https://meshtastic.org/)** radio node (the WiPhone's
  built-in LoRa radio speaks Meshtastic's on-air protocol — encrypted channel
  text **and encrypted direct messages with modern Meshtastic devices**, no
  phone, app, or internet required — plus shared map pins, distances to your
  people, and sunrise/sunset for wherever you are),
- a full-speed **Game Boy / Game Boy Color emulator** with sound, save states,
  and drag-and-drop ROM upload over WiFi,
- a **music player** (MP3 and WAV, stereo, hardware transport buttons, plays on
  while you use the rest of the phone),
- an **e-reader** (EPUB and text, pictures inline, your place saved, and
  position sync to another device over LoRa),
- and phone quality-of-life fixes (real battery life, WiFi auto-switching,
  input reliability).

All of the WiPhone's normal phone / SIP / menu features stay intact.
Built with **PlatformIO** (the stock WiPhone firmware is Arduino-IDE only).

---

## 🔌 Install it from your browser — no tools needed

### ➡ **[nikguy321.github.io/wiphone-meshtastic](https://nikguy321.github.io/wiphone-meshtastic/)** ⬅

Plug the WiPhone into a computer with a USB cable, open the link in **Chrome or
Edge**, click **Install**, and pick the serial port that appears. About a
minute; the phone reboots itself when done. Nothing to install on the computer
— no Python, no command line. (Prefer doing it by hand? The PlatformIO route
below still works, and the phone's own **Settings → Firmware update** screen
carries both sets of instructions.)

---

## What's new

Full detail in **[CHANGELOG.md](CHANGELOG.md)** — every release, including the
bug fixes and why each one happened. Recent highlights:

- **0.9.7** — the phone knows where everyone is: **Places** (shared map pins),
  live **distances and bearings** in the node list, **Sun & legal light**, and
  every list that used to run off the screen now wraps or ellipsizes.
- **0.9.6** — **direct messages work again with modern Meshtastic** (2.5+
  encrypts DMs with public-key crypto and silently drops the old form; the
  phone now speaks it natively).
- **0.9.5** — install from a browser, a **Files** app, and type-just-the-number
  texting (the server part of the address fills itself in).
- **Fixed since 0.9.7** (in the repo, riding the next release): a WiFi
  auto-switch deadlock that could leave the phone sitting next to a saved
  hotspot without joining it; GPS/woods-backplate support (dormant until the
  hardware exists); Sun and the GPS toggle moved into the menus — a feature
  that only exists as a serial command is not a feature.

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
  it from any computer, **or paste a download link** and the phone fetches the
  file itself. Multiple files at once, per-file progress, and a failed upload
  retries itself a few times before complaining. Falls back to hosting its own
  hotspot (`WiPhone-ROMs`) when not on WiFi.
- **Big carts work** — 4 MB ROMs stream from the SD card on demand.
- **Comes with a game** — uCity (public-domain) is built into the firmware, so
  there is something to play before any SD card or upload.
- **Housekeeping in the list** — Back on a ROM offers to delete it (with a
  confirm); long names scroll so you can read them.
- **Two screen modes** — crisp 1:1 or 1.5× Fill, toggled from the pause menu,
  which also shows the measured game speed %.
- **In-app help** — the **Help...** row documents the controls and everything
  else a new user needs.
- Heads-up: WiFi and calls are off while a game runs (they come back when you
  quit) — the radio's RAM is the price of full speed.

Controls: D-pad moves, bottom-right side key = **A**, the key above it = **B**,
Back = **Start**, Select = **Select**, End (hang-up) = **pause menu**.

---

## Music player

- **MP3 and WAV straight from the SD card** — nothing to convert. Menu > Music.
  (Format is detected from the file's content, so a mislabelled file still
  plays; a track that will not decode is skipped with the reason shown in the
  list, instead of ending the album.)
- **Stereo** through the headphone jack, loudspeaker otherwise, and it follows
  the jack live: unplug mid-song and it carries on out of the speaker from the
  same spot.
- **The four side buttons are the transport**, top to bottom: play/pause,
  next (**hold** = previous), volume up, volume down. Hold-for-previous does the
  usual thing — back a track within 3 seconds, restart the current one after.
  On the Now Playing screen, keypad **4/6** and D-pad left/right also skip.
- **Keeps playing when you leave the screen.** Read a book or check the mesh
  with music going; tracks advance on their own. A phone call always wins the
  speaker: music pauses for it and deliberately never barges back in on its own.
- Shuffle, repeat (off / all / one), and its own WiFi uploader (same
  `wiphone.local` page; hosts its own hotspot when off WiFi).
- Volume starts low on every boot and is separate from the call volume, so a
  quiet album can never leave you unable to hear the next call.

Decoding is the **Helix MP3 decoder** (RealNetworks, RPSL — vendored under
`WiPhone/src/audio/helix-mp3/`), with its ~29 KB of working memory placed in PSRAM, because the
internal heap on this phone has nothing like that spare. Measured on the device:
about a quarter of realtime at 48 kHz stereo, so there is plenty of headroom.

## E-reader

- **EPUB and plain text** from the card, with **pictures inline**. Menu > Books.
  Numbered pictures enlarge to full screen when you press that number key.
- **Books arrive over WiFi too** — the library's top row starts the same
  drag-and-drop upload page the emulator uses.
- **Three text sizes**, switched from the reader menu; your place is preserved
  across the change, and a **chapter list** jumps anywhere in the book.
- **Your place is saved** and survives the phone losing power without warning.
- Position is **(chapter, character offset)**, never a page number — so it still
  means something after you change the font size, and on a different device.
- **Greyscale JPEGs decode**, which the ESP32's built-in decoder cannot do at
  all (it handles 3-component colour only, and most book art is 1-component).
- **Book sync over LoRa** — share your reading position with another device on a
  private channel. Jumps are confirmed, never taken silently — and an offer
  whose clock looks wrong says so instead of being trusted. Diagnostics live in
  Books → menu → Sync settings.

## Meshtastic features

- **Two-way Meshtastic text messaging** on the default LongFast channel (US),
  AES-encrypted and interoperable with regular Meshtastic nodes.
- **Encrypted direct messages that modern Meshtastic accepts.** Meshtastic 2.5+
  requires public-key crypto for DMs and silently drops the legacy form — this
  phone speaks the real thing (X25519 + AES-256-CCM, keys learned automatically
  from NodeInfo, incoming DMs acknowledged). Proven against stock 2.7 hardware,
  both directions.
- **Multiple channels** — import custom channels from a Meshtastic share link
  (decoded on-device, no internet). Each channel shows up as its own chat.
- **Places** — waypoints shared on the mesh (camp, the truck, a stand) appear
  in **Meshtastic → Places**, with expiry and owner-locked edits. Pick one as
  your reference and the **Nodes list shows live distance and bearing** to
  everyone — "3.2km E of camp · 4 min ago".
- **"I'm here (announce)"** — declare yourself at a waypoint; the phone
  broadcasts one position so other devices' maps show it (preferring a private
  channel over public LongFast, and admitting it honestly when the send failed).
- **Sun & legal light** — dawn / sunrise / sunset / dusk and a countdown
  ("LEGAL LIGHT: 13h 34m left") for the reference place, computed offline.
- **GPS-ready** — a receiver on the expansion header (build guide coming once
  the reference build is finished) gives the phone its own live position; the
  toggle is in My node, and everything above works without it.
- **Node list** with friendly names (learned from NodeInfo), an **editable node
  name** (long and 4-character short), and a **configurable hop limit**.
- **Mesh client role** — relays/rebroadcasts other nodes' packets to extend the
  mesh's range (flood routing).
- **New-message notifications** — a status-bar icon (with unread counts per chat),
  a brief on-screen popup, plus a quiet "pop" sound and a short vibration
  (designed to be unobtrusive — e.g. usable as a communicator while hunting).
  Meshtastic has its own entry in the sound settings: ring+vibrate, vibrate
  only, or silent.
- **Persistent** message history, channels, places and keys across reboots.
- **Low-power green/black UI theme** for the Meshtastic app, and a Meshtastic
  icon on the main menu.
- **Handy extras** — hold **Select + Back** (the top-left and top-right corner
  keys) together for two seconds to turn the screen off.

The normal WiPhone experience (phone calls, SIP, contacts, games, settings) is
untouched.

---

## Texting

- **Conversations, not an inbox and an outbox.** Messages opens on a list of
  people, newest first, with unread counts on the row. Open one to see the whole
  exchange in order, oldest at the top, your messages headed `You · 2 min ago`.
  Reply is one press and already knows the address; each message also offers
  Reply/Delete when opened.
- **One person is one conversation**, however the number is written — with the
  country code, without it, punctuated, or as a full SIP address.
- **Type just the number.** A bare number is completed to
  `number@your-server` automatically, from your active SIP account — in the
  composer and when saving a phonebook contact. (It needs a SIP account to be
  active; without one, sends fail and the serial `sip` command says why.)
- ⚠ The list covers your **recent** conversations, not the entire history. Someone
  you have not texted in a long time may not be shown, and a long thread shows
  its newest messages with an honest "… N older messages not shown" line.

### Sharing a number with a second device

If another device uses the same phone number, the texts it sends can be mirrored
here so the two do not drift apart. Built for COVEY, a Raspberry Pi handheld, but
the wire format is plain text and easy to speak.

- **Over WiFi** you get the full history, including anything that arrived while the
  phone was off. **Over LoRa** you get new texts anywhere in radio range with no
  WiFi at all — as channel messages or encrypted DMs. Run both — a text already
  held is recognised and not shown twice.
- **Setup:** an `smsmirror.txt` on the SD card — the other device's address,
  then its shared token (optional third line: the port; default 8087). Upload it
  with the usual WiFi upload page (the phone also looks in `/roms`, which is
  where that page writes). The file is re-read every 30 seconds, so changes take
  effect without a reboot. No file means the feature is off, and the phone says
  so on serial instead of failing quietly.
- **Notifications behave**: texts you sent from the other device arrive silently,
  and catch-up history older than ten minutes never buzzes — only a genuinely
  new incoming text does.
- If the mirror ever seems stuck, delete `/roms/smsmirror.since` on the card —
  the phone refetches everything and deduplicates.
- ⚠ **Use a dedicated mesh channel.** An invite link contains the channel key, so
  sharing it gives away everything else on that channel.
- ⚠ **The WiFi side is cleartext** — this phone cannot do HTTPS at all
  ([why](#why-there-is-no-over-the-air-update)). The token stops a stray browser, not
  someone watching the network. Turn it off on networks you do not trust; the LoRa
  side is encrypted by the channel key.

---

## Serial console

Plug in USB, open a terminal at **500000 baud**, type `?`:

| command | does |
|---|---|
| `up on` / `up off` / `up` | start / stop the WiFi upload page; show its address |
| `sync` / `mirror` | fetch mirrored texts now; mirror status |
| `sip` | SIP account state — loaded, registered, WiFi — in one line |
| `chan <url>` / `chans` | apply a Meshtastic channel invite link; list channels |
| `dm <!node> <text>` | send a direct message (encrypted when the key is known) |
| `announce` / `pki` | broadcast NodeInfo now; DM crypto state |
| `pos` | the whole positions picture: places, node fixes, pin, reference |
| `sun` | legal light at the reference place (or `sun <lat>,<lon>`) |
| `gps` / `gps on\|off` | GPS receiver state / route the user UART to it |
| `unread` / `unread clear` | recount + repair unread counters; mark all read |
| `bookpage` | dump the open reader page's layout (debugging) |

For the things that otherwise need the phone in your hand — handy when it is on
a bench instead. **No authentication:** whoever holds the cable holds the phone.

---

## Phone improvements

- **WiFi auto-switch** — the phone quietly scans in the background and hops to
  the strongest *saved* network (with hysteresis, so it doesn't ping-pong);
  waking the screen with no connection triggers an immediate scan+connect, and
  out-of-range scanning backs off to save battery, recovering the moment
  anything connects. Toggle under **Settings → WiFi auto-switch**.
- The **status bar shows the connected WiFi network's name**.
- Fixed the **"Edit current network"** screen freezing on input, and the
  multi-second menu freezes caused by DNS lookups (answers are cached now,
  including the "that name does not resolve" answer that used to freeze the
  phone over and over on restrictive networks).
- **Keypad reliability** — fixes for missed taps, stuck buttons, and held keys
  releasing at random (I2C error retry + a 40 ms hardware key heartbeat).
- **Real battery life.** The main loop used to spin flat out at 240 MHz forever
  with the screen off; it now sleeps between passes, drops the CPU to 80 MHz
  when nothing is happening, and lets the WiFi receiver park between beacons.
  Full speed returns instantly for the screen, music, a call, the emulator or an
  upload.
- **The phone records why it restarted** — crash, watchdog, brownout or a normal
  power-on — plus a health line every minute (memory, battery, CPU speed) to
  `/health.log` on the card. The log trims itself (newest ~4 hours kept), and is
  readable over WiFi at `http://wiphone.local/log` with an upload screen open,
  so it survives being unplugged and rebooted.
- **File transfers work from a phone.** The upload page had been built around
  drag-and-drop; it now has a proper file button and a paste-a-link fetch, stops
  wedging after one page load, retries a failed upload, raises the screen
  timeouts while the page is open (dim 5 min / sleep 10) so a long transfer
  doesn't go dark, and no longer knocks the phone off WiFi when it opens.

---

## Firmware updates

**Updates are installed over USB.** See [Flashing](#flashing-beginner-friendly)
below — it is the same `pio run -t upload` you used the first time, and it keeps
your settings, books, ROMs and messages.

**Settings > Firmware update** on the phone shows those steps on the screen, so
you can read them with the phone in your hand.

### Why there is no over-the-air update

The phone cannot open an HTTPS connection, so it can never download an update by
itself. This is a memory limit, not a broken link or an expired certificate:

| | |
|---|---|
| mbedTLS needs (`CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN` 16384, in **and** out buffers) | **~33 KB** |
| may any of it come from PSRAM? (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`) | **no, not set** |
| free internal heap on this phone | **~19 KB** |
| largest contiguous internal block, fresh boot | **~14.9 KB** |

The TLS handshake fails in `client->connect()` before a single byte of HTTP.
It is not close, and it is not fragmentation — there is not enough internal heap
even on a perfectly clean boot. The OTA controls have been removed rather than
left to report an error every time, and the boot-time update check is compiled
out entirely (`OTA_TRANSPORT_AVAILABLE` in `WiPhone/ota.h`) so the phone no
longer spends part of every startup on a connection that cannot open.

Changing this means changing the **transport** — a plain-HTTP mirror, or a TLS
stack that can allocate from PSRAM — not the URL. The code is still there and
still builds; flip `OTA_TRANSPORT_AVAILABLE` to `1` once the transport is real.

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
with `pio device list` and add `--upload-port /dev/<your-port>`. If the upload
starts and then fails partway ("Invalid head of packet" is the classic), your
USB-serial doesn't like full speed — set `upload_speed = 230400` in
`platformio.ini` and try again.

**6. First time only — also flash the filesystem** (config files, the ringtone,
the wallpaper):
```bash
pio run -t uploadfs
```

That's it — the WiPhone reboots into the firmware. Open the main menu and look for
**Meshtastic**.

> Tip: to watch the phone's logs, run `pio device monitor` — the project pins the
> monitor to the WiPhone's **500000 baud** console, so it works as-is.

---

## Credits & license

Based on the official [WiPhone firmware](https://github.com/HackEDA/wiphone-firmware)
by HackEDA, Inc., distributed under the **WiPhone Public License v1.0** (see
`LICENSE`). Meshtastic is a trademark of Meshtastic LLC; this is an independent,
community project, not affiliated with or endorsed by Meshtastic.
