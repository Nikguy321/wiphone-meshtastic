# Phone location: your phone's GPS on a WiPhone, over the phone's hotspot

A WiPhone without the GPS backplate can still know where it is. Join it to your phone's
WiFi hotspot and run a small app on the phone that shares the phone's GPS. The WiPhone finds
the app by itself and uses that position:

- for **"me (phone)" on the map**, distances and bearings, the Almanac and the weather;
- for **position reports to the mesh**, if you turned those on (My node → Position) and the fix
  is good enough. See [what gets sent](#what-gets-sent-to-the-mesh);
- for the **time zone** when Settings → Time zone is on Auto (0.9.82): the zone where the phone's
  fix is, after two looks two minutes apart, never near a zone line. A phone's fix counts only
  when no other clock is within 10 km, whatever app sends it: even a fix with good satellite
  numbers can be kilometres off (gpsdRelay with NMEA generation on, GPS 2 IP on another port).
  The backplate GPS only has to be clear of the line itself.

No Bluetooth is involved. The WiPhone only reads: it never sends anything to the app, and it
never uses the phone's data to set its clock's time (UTC) - only, on Auto, which zone to show
it in.

**Which source wins:**
1. The WiPhone's own GPS backplate, when it has a fix.
2. The phone over the hotspot.
3. A [Wi-Fi location](#wi-fi-location) from the access points around it, only when it is
   genuinely local and accurate. It is off unless you turn it on.
4. Nothing. A wrong location is worse than none.

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

**A Wi-Fi location** is sent only with **Wi-Fi location: map + mesh**, and only when:
- two scans at least 60 s apart have confirmed it (a fix taken while moving never goes out);
- a scan finished within the 60 s before the send confirmed it again. When a report is due
  and this is all that is missing, the WiPhone asks for a scan and sends on its confirmation;
- no GPS or phone report went out in the last hour within the Wi-Fi fix's accuracy + 50 m of it
  (a receiver keeps the last position it heard: a coarse one must not replace a precise one).

It goes out as a ~182 m cell (Meshtastic `precision_bits` 17, the point moved to the middle of the
cell), with no `location_source` (it is not a measurement), and its time is the time of the scan
that confirmed it. The WiPhone's own map and COVEY draw a ring of that size. While a new fix lies
within the accuracy of the one that produced the last cell, the same cell is sent again, so a
fix near a cell edge does not flip between neighbouring cells.

**Who it trusts.** Whatever answers on the joined network's gateway. On your own hotspot
that is your phone. On someone else's WiFi (a hotel, a campground, a boat's NMEA gateway) a
device serving NMEA on those ports could set where the WiPhone thinks it is. Turn Phone
location **off** on networks you don't control if that matters to you (or pick a named zone in
Settings → Time zone, so the clock's zone cannot follow it). The backplate GPS always wins, and
the clock's time is never touched.

## Wi-Fi location

With no GPS fix and no phone over the hotspot, the WiPhone can work out roughly where it is from
the WiFi access points it hears. It asks **beaconDB** (a free, open, volunteer-run database of
where access points are) and believes the answer only when two separate questions agree.
**Meshtastic → My node → Wi-Fi location** has three settings; tapping the row moves to the next:

| Setting | What happens |
|---|---|
| **off** (the default) | Nothing is scanned for it and nothing is sent anywhere. |
| **map only** | The map shows **me ±60 m Wi-Fi** (written `me +/-60 m Wi-Fi`) with a dotted ring that size; distances, the Almanac and its weather use it. It is never sent to the mesh. |
| **map + mesh** | The same, and position reports may send it, under the stricter rules [below](#what-gets-sent-to-the-mesh). |

The serial console can choose only `netloc off` or `netloc map`. **map + mesh** puts your
position on other people's maps, so it is chosen on the WiPhone's screen, and it starts only when
you **leave the My node screen** with it chosen (the line under the row says
`mesh starts when you leave here`). The row goes off → map only → map + mesh → off, so the way
from map only to off passes through map + mesh; that tap never sends anything.

Switching it **off** stops a lookup that is still waiting or half done: nothing more is sent, and
nothing from it is kept.

**What it sends, and to whom.** When a position is wanted, the WiPhone sends beaconDB
(`https://api.beacondb.net/v1/geolocate`) the hardware addresses (BSSIDs) and signal strengths of
up to 20 nearby access points in each of two requests. Nothing else is sent: no network names, no
details of this phone, a generic `WiPhone-NH` User-Agent with no version. The request turns
beaconDB's own fallback to your internet address off twice (`"considerIp":false` and
`"fallbacks":{"ipf":false}`), and any answer that still says it came from a fallback is thrown
away. Nothing is ever submitted back. beaconDB's privacy page says it keeps its web server logs
(your internet address, the time, the URL and the User-Agent) for 28 days and does not record the
request bodies. The line under the row says this while it is off:
`if on: sends Wi-Fi IDs to beaconDB`.

These access points are never sent:
- hidden networks, and any network whose name ends in `_nomap` or `_optout` (the owner asked);
- addresses that travel with someone: randomised (locally administered) addresses, which most
  phone hotspots use, group addresses, and VRRP virtual routers;
- the network the WiPhone has joined, when it is a phone's hotspot (a location app answered on
  it under Phone location at any time since joining, even if the app has stopped since) or its
  address is a locally administered one. This is checked again when a look starts, and a look
  waits 15 seconds after joining a network so the phone's app can answer first;
- anything weaker than -85 dBm.

**When it asks.** All of these must hold:
- the setting is not off, and the WiPhone is on someone else's WiFi (not its own hotspot);
- there is no live GPS or phone fix (a measured position always wins);
- a position is wanted: Maps or the Almanac was on screen in the last 10 minutes, position
  reports are armed with **map + mesh**, or `netloc now` on the console;
- something changed: it has not looked yet on this network, the access points it heard last
  time are mostly gone, or the last fix ran out;
- at least 10 minutes since the last look (2 for `netloc now`), and at most 3 looks an hour (a look
  is up to two requests);
- no map download, uploader, book sync, call, game or Files job is running, and no music is
  playing (`netloc now` pauses the music, as the weather's Refresh does).

It needs a WiFi scan of this network from the last 2 minutes. When there is none, it asks the
WiFi auto-switcher for one, at most every 2 minutes. That scan obeys every rule the
auto-switcher's own scans obey (enough free memory, not during a call or a game), and waits while
an AI question, a weather fetch or a lookup is connecting (a scan and a secure connection at the
same moment can run the phone out of memory). It is only a scan: it never hops to another network,
never disconnects, and never reloads the saved-network list (with auto-switch on, the
auto-switcher's own 10-minute scan still does all of that on time). While a phone's GPS stream is
live on the hotspot, auto-switch does not hop away from that hotspot either. A lookup never starts
while a scan is running.

**The agreement rule.** beaconDB's answer is an average of whichever access points it knows, and
its accuracy figure says how far each one is usually heard, not whether they agree. One access
point that has moved (a household that moved, a travel router) can drag the answer kilometres away
while it still reads "±50 m". So:
1. At least 4 separate devices must be heard at -85 dBm or stronger. Several addresses of one
   router (2.4 GHz, 5 GHz, guest) count as one device.
2. The devices are dealt into two halves by signal strength (1st, 3rd, 5th... and 2nd, 4th,
   6th...), and each half is asked on its own.
3. Both answers must be HTTP 200 with no fallback, each must claim 1 to 100 m, and they must lie
   within the larger of the two claims of each other.
4. The WiPhone uses the midpoint, at an accuracy of the largest of 50 m, either claim, and the
   distance between them. It never claims better than 50 m.

Two halves cannot catch access points that moved **together** (a household's router, extender
and travel router, all mapped at the old address). So two more checks:
- **The jump check.** If this phone had a GPS or phone fix in the last 6 hours, the Wi-Fi answer
  must be reachable from it at 300 m/s or less (beyond the answer's own accuracy and 50 m).
  Otherwise it is refused: `far from your last fix: not used`.
- **A scan during the lookup.** A lookup can wait up to a minute behind a weather fetch. If a WiFi
  scan finishes meanwhile and no longer hears 2/3 of the devices asked about, the answer is
  refused (`moved during the lookup`) and it looks again later.

**How long it lasts.** A Wi-Fi location is where some access points are, so it stays true only
while the WiPhone still hears them. Every later scan is checked: if at least 2/3 of those devices
are still heard, the fix is kept; if not, it is dropped **at once**, before any new look. While
Maps or the Almanac is open, a fix is re-checked by a scan every 2 minutes, so a fix carried off
on a phone hotspot that stays connected is dropped within about 2 minutes. When the WiFi drops or
the WiPhone joins another network, the fix comes off the map at once and comes back only if the
next scan still hears its access points. Shown for 12 minutes after the last scan that confirmed
it, then gone. There is no grey "last Wi-Fi" marker.

Everything measured from a Wi-Fi fix carries its size: distances read "from Wi-Fi +/-60 m", and the
map's height line reads "35m below Wi-Fi +/-60 m" rather than "below you".

**Why it often says "not enough known Wi-Fi here".** beaconDB knows only access points that
volunteers have passed with its app. A home, a cabin or a campground has often never been seen,
and the halves must both be known and agree. That is by design: it says nothing far more often
than it says something wrong. The time zone never uses a Wi-Fi location either.

The line under **Wi-Fi location**:

| It says | What it means |
|---|---|
| `if on: sends Wi-Fi IDs to beaconDB` | Off. |
| `+/-60 m, 3 min ago, map only` | Working: the size, and when a scan last confirmed it. |
| `used while Maps or Almanac is open` | On, waiting until a position is wanted. |
| `waiting for a Wi-Fi scan` | It asked the auto-switcher for a scan. |
| `too few Wi-Fi networks (2 of 4)` | Not enough separate devices at -85 dBm or stronger. Nothing was sent. |
| `not enough known Wi-Fi here` | beaconDB did not know enough of them: a half got HTTP 404, or an answer that was not a Wi-Fi fix. |
| `too vague (+/-350 m): not used` | A half claimed more than 100 m. |
| `lookups disagree: not used` | The two halves were further apart than they claimed. One access point has probably moved. |
| `far from your last fix: not used` | The halves agreed, but on a place this phone could not have reached since its last GPS or phone fix. The access points have probably moved together. |
| `moved during the lookup` | A scan that finished while the lookup was out no longer heard the access points. It looks again later. |
| `WiFi changed: waiting for a scan` | The WiFi dropped or changed. The fix is off the map until a scan shows the access points are still there. |
| `mesh starts when you leave here` | map + mesh is chosen. Nothing goes to the mesh until you leave this screen. |
| `no fix now (moved or expired)` | The access points behind the fix are gone, or it ran out. It looks again when allowed. |
| `beaconDB unreachable: will retry` / `low memory: will retry` | Tries again after 10 minutes. |
| `not needed: GPS/phone fix in hand` / `waits while music plays` / `not on WiFi` | Self-explanatory. |

`netloc` on the serial console prints the setting, the last scan (how many access points, how
many usable, how many devices), the last look's verdict and accuracy, the fix's state and
whether it may be sent, and each half's HTTP code and handshake time. It never prints an address
or a coordinate. `netloc now` asks for a look; `netloc answer <A> <B>` feeds two canned answers
through the same agreement rule for the bench (each `{...}` as beaconDB's JSON, a 3-digit HTTP
status, or `-` for "not asked"). The result is a bench fix: shown, never sent by the schedule.

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
