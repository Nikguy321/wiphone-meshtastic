# Phone location: your phone's GPS on a WiPhone, over the phone's hotspot

A WiPhone without the GPS backplate can still know where it is. Join it to your phone's
WiFi hotspot and run a small app on the phone that shares the phone's GPS. The WiPhone finds
the app by itself and uses that position:

- for **"me (phone)" on the map**, distances and bearings, the Almanac and the weather;
- for **position reports to the mesh**, if you turned those on (My node → Position) and the fix
  is good enough. See [what gets sent](#what-gets-sent-to-the-mesh).

No Bluetooth is involved. The WiPhone only reads: it never sends anything to the app, and it
never uses the phone's data to set its clock.

**Which source wins:**
1. The WiPhone's own GPS backplate, when it has a fix.
2. The phone over the hotspot.
3. Nothing.

The WiPhone never guesses a location from your internet address. On a phone hotspot that
guess lands wherever your carrier's network leaves the internet, often hundreds of kilometres
away.

**Supported apps:**

| Phone | App | Where | Cost | Port | Sent to the mesh? |
|---|---|---|---|---|---|
| Android | **gpsdRelay** (recommended) | F-Droid | free, open source | 10110 | yes |
| Android | Share GPS | Google Play | free (ads) | 10110 (set it) | yes |
| iPhone | GPS 2 IP | App Store | $7.99 (free Lite trial) | 11123 (its default) | **no**, see below |

The WiPhone asks the hotspot's gateway, which is your phone, on port 10110 and then on 11123.
You can add one more port under My node, or with `phonepos port <n>` on the serial console.

---

## 1. On the WiPhone (once)

1. Firmware **0.9.82-NH or newer**.
2. **Meshtastic → My node → Phone location: auto**. This is the default. Tapping the row
   switches it between auto and off.
3. Optional, to report your position to the mesh:
   - **My node → Position: every 5 / 15 / 30 min**;
   - **Send to:** a *private* channel your group uses. A public channel (readable by any radio
     in range) has to be confirmed on the WiPhone's screen. The serial console can never choose
     one.
4. Join the phone's hotspot like any WiFi network (Settings → WiFi). Save it so the WiPhone
   rejoins by itself.

The small line under **Phone location** tells you what it found. See
[Troubleshooting](#troubleshooting).

## 2. The phone's hotspot

The WiPhone's radio is 2.4 GHz only and older than WPA3.

**Android (Samsung shown; others are similar).** Settings → Connections → Mobile Hotspot and
Tethering → Mobile Hotspot → Configure:
- **Band:** 2.4 GHz
- **Security:** WPA2-Personal (not WPA3, not "WPA2/WPA3")
- **Advanced:**
  - turn **Protected Management Frames** off;
  - set **Turn off hotspot when no devices are connected** to *Never*, so it does not vanish
    while the WiPhone sleeps.

**iPhone.** Settings → Personal Hotspot → **Allow Others to Join**, and turn on **Maximize
Compatibility** (this makes it 2.4 GHz). Keep the Personal Hotspot screen in mind: iOS can
turn the hotspot off when nothing has been connected for a while.

---

## 3a. Android: gpsdRelay (recommended)

gpsdRelay is not on Google Play. It is on **F-Droid**, a store for free open-source apps.

**Install**
1. On the phone, open **f-droid.org** in Chrome and tap **Download F-Droid**. Open the
   downloaded file. Android asks once whether Chrome may install apps: allow it.
2. Open F-Droid, search **gpsdRelay** and install it (version 2.2 or newer).
   - You can instead install just the APK from the
     [gpsdRelay page on F-Droid](https://f-droid.org/packages/io.github.project_kaat.gpsdrelay/)
     or its [GitHub releases](https://github.com/project-kaat/gpsdRelay/releases/latest).
     You then won't get updates automatically.

**Set it up**
1. Open gpsdRelay. When it asks for location, choose **Allow all the time** and keep
   **Precise** on. Allow notifications.
2. Tap the **gear** (Settings):
   - turn **Monitor default network state** **off**. Left on, gpsdRelay stops sharing
     whenever the phone loses cell signal, which is exactly when you are out in the woods;
   - optional: turn on **Start service when Android boots**;
   - leave **NMEA generation interval** at 1000 ms;
   - tap **Apply**.
3. Tap **+** → **TCP Server**:
   - **IPv4 address:** `0.0.0.0` (the default)
   - **Port:** `10110`
   - tick **NMEA relaying**: the phone's real GPS sentences, with real satellite counts;
   - untick **NMEA generation**. It is ticked by default. Its sentences carry no satellite
     count, and gpsdRelay can misprint a generated coordinate. The WiPhone checks for both, but
     relaying is better.
   - Tap **Add**. The list shows `T:0.0.0.0:10110`.
4. Tap the **play** button at the top. A notification says **NMEA relay is running**.
5. Keep Android from stopping it: Settings → Apps → gpsdRelay → **Battery → Unrestricted**.
   On Samsung also: Settings → Battery → Background usage limits → add gpsdRelay to **Never
   sleeping apps**.

gpsdRelay reads the phone's real GPS only. Expect a fix outdoors or by a window, not deep
indoors. Without a fix the WiPhone says **no fix (indoors?)** and reports nothing.

When you are done, tap the stop button. Anything on a network the phone has joined can read
the position while gpsdRelay runs, including a hotel's WiFi.

## 3b. Android: Share GPS (Google Play)

Share GPS is older (last updated around 2016) and may not appear in the Play Store on newer
phones. If it installs, it works the same way.

1. Install **Share GPS** from Google Play. Allow location (**Allow all the time**, Precise).
2. Open **Connections** and add a new connection: type **NMEA**, method **TCP/IP**.
3. Tick **TCP/IP server** (the phone listens) and set the port to **10110**.
4. Start the connection. Its status says **Listening**.
5. Settings → Apps → Share GPS → Battery → **Unrestricted**. Share GPS's own help says
   battery optimisation makes the WiFi sleep in the background.

## 3c. iPhone: GPS 2 IP

1. Install **GPS 2 IP** from the App Store ($7.99, or try **GPS2IP Lite** first).
2. Settings (in the app):
   - **Connection Method: Socket**. The iPhone listens and the WiPhone connects.
   - **Network → port 11123**, the default.
   - **Protocol Type: NMEA**, with **GGA** and **RMC** turned on.
   - **Time between sending positions:** 1 s (or *No delay*).
   - **Disable when Static: off**. When it is on, GPS 2 IP goes quiet while you stand still,
     and the WiPhone takes the silence for a lost link.
   - **GPS Accuracy: The very best**.
   - **Operate in background mode: on**.
3. iPhone Settings → Privacy & Security → Location Services → GPS 2 IP → **Always** and
   **Precise Location: on**. With Precise off, iOS moves the position by kilometres on purpose.
4. Turn GPS 2 IP **on** on its main screen.

**What the WiPhone does with an iPhone's position:** it is shown on the WiPhone's map, used
for distances, the Almanac and the weather, and **not sent to the mesh**. iOS gives apps no
satellite count and no accuracy. GPS 2 IP's documentation says it always writes 8 satellites
and HDOP 0.9, so an indoor WiFi-based position looks exactly like a good GPS fix. The WiPhone
recognises those stand-in numbers and won't broadcast a position it can't grade. The status
line says **iPhone app gives no accuracy: shown, not sent**.

---

## What gets sent to the mesh

A position goes out only when all of these hold:
- you turned reporting on (Position + a Send to channel chosen on the screen);
- it comes from your phone over the hotspot (or the WiPhone's own GPS);
- it is fresh (under 30 s old);
- it has a real rating: at least 4 satellites and HDOP 10 or better.

It is marked **External** (from a phone) and full precision, the same way the official
Meshtastic apps mark a phone's GPS. The WiPhone's own backplate GPS is marked **Internal**.

Never sent:
- an iPhone fix;
- a weak fix;
- anything older than 30 s;
- anything from the serial bench tools.

**Who it trusts.** Whatever answers on the joined network's gateway. On your own hotspot
that is your phone. On someone else's WiFi (a hotel, a campground, a boat's NMEA gateway) a
device serving NMEA on those ports could set where the WiPhone thinks it is. Turn Phone
location **off** on networks you don't control if that matters to you. The backplate GPS
always wins, and the clock is never touched.

## Troubleshooting

The line under **Meshtastic → My node → Phone location**:

| It says | What it means / what to do |
|---|---|
| `this network: fix, 9 sats` | Working. |
| `no app found on this network` | Nothing answered on 10110 / 11123. Is the app running, on the right port, and is the WiPhone on *this phone's* hotspot? It asks again every few minutes. |
| `looking for a location app` | Asking now. |
| `connected, waiting for data` | The app accepted the connection but has sent nothing yet (gpsdRelay: is the service started?). |
| `old data dropped, waiting` | gpsdRelay replays old lines when the WiPhone connects. They are skipped on purpose. Wait a few seconds. |
| `fix found, checking it` | A fix needs a second, agreeing fix before it is used. One more second. |
| `no fix (indoors?)` | The phone has no GPS fix. Go outside or near a window. |
| `weak fix: not sent` | Fewer than 4 satellites or HDOP above 10. Shown, not reported. |
| `iPhone app gives no accuracy: shown, not sent` | GPS 2 IP. See [3c](#3c-iphone-gps-2-ip). |
| `fix, accuracy unknown: not sent` | The app sends no satellite count or HDOP. |
| `estimate only, not a GPS fix` | The app marked the position as an estimate (dead reckoning). Not used. |
| `fixes jump about: not used` | Two fixes in a row disagree. Often gpsdRelay with **NMEA generation** on: untick it, tick **NMEA relaying**. |
| `the app sends no GGA` | gpsdRelay: tick **NMEA relaying**. GPS 2 IP: turn on **GGA**. |
| `unreadable data from the app` | Not NMEA on that port, or the app sends something else (GPS 2 IP: **Protocol Type: NMEA**). |
| `not on WiFi` / `off` | Join the hotspot / tap the row to turn Phone location on. |

The serial console's `phonepos` command prints the same state with counters: bytes,
sentences, bad checksums, stale lines dropped, reconnects. It never prints a coordinate or an
address.

## For developers: testing without a phone

`tools/nmea_phone_stub.py` is a stand-in for the phone apps (standard library only):

```bash
python3 tools/nmea_phone_stub.py --port 10111 --bind <the Mac's address>
```

Then on the WiPhone's serial console: `phonepos at <the Mac's address>:10111`. This is RAM
only, and its fixes are bench fixes that are never broadcast. Flags reproduce each app's
quirks:
- `--burst 30`: gpsdRelay's replayed backlog;
- `--generated --acc 8`: gpsdRelay's generated sentences;
- `--ios`: no sats/HDOP;
- `--sats 8 --hdop 0.9` on port 11123: GPS 2 IP's stand-in numbers;
- `--sats 3`, `--hdop 12`, `--status V --quality 0`, `--stall 15`, `--no-crlf`.

The rules themselves live in `WiPhone/self_pos.{h,cpp}` (host-tested by
`tests/test_selfpos.cpp`). The socket is in `WiPhone/phone_pos.cpp`.

The X4 Pro e-reader fork ([crosspoint-reader](https://github.com/Nikguy321/crosspoint-reader))
reads the same apps the same way for its **Locate Me**.
