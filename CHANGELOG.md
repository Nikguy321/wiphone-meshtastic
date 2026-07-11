# Changelog

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
