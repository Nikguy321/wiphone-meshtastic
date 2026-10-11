# NH Devices Companion (Android)

The companion app for devices on NH firmware: the WiPhone, the X4 e-reader (CrossPoint NH fork)
and others to come. Version 1.0 does one job. It **shares the phone's GPS with devices on the
phone's Wi-Fi hotspot**, as NMEA 0183 over TCP on port 10110, the port those devices look for.
File transfer and per-device management are planned.

The user guide is [docs/phone-location.md, section 3a](../docs/phone-location.md#3a-android-nh-devices-companion-recommended).
This page is for building and changing it.

- Package `io.github.nikguy321.nhcompanion`; it shows as **NH Companion** on the home screen.
- Android 8.0+ (minSdk 26), targetSdk 35. Java, framework APIs only: no AndroidX, no Google Play
  services, no network libraries. The release APK is about 29 KB.
- **It makes no outgoing connections.** It needs INTERNET only to listen on its port.

## How it works

| File | What it does |
|---|---|
| `GpsService.java` | The foreground service (type `location`) and its notification. It opens the port, turns the GPS on while a device is connected and for 5 min after the last one leaves, and holds a partial wake lock while the GPS is on. It sends the GPS chip's own sentences, or builds them from the fix. |
| `Hub.java` | The TCP port. Plain `java.net`, unit-tested on the computer. Each device gets a 64-line queue that drops the **oldest** lines when the device stops reading. A write stuck for 30 s means the device is gone. At most 8 devices. |
| `Policy.java` | Who may read. Refused:<br>• anything from the phone itself: loopback, and also an app connecting to the phone's own hotspot address, which Linux delivers locally;<br>• a connection that arrived on a network the phone *uses* (Wi-Fi it joined, mobile data, a VPN);<br>• Wi-Fi Direct and Wi-Fi Aware peers;<br>• a device outside the subnet of the interface it connected on.<br>Tethering interfaces (hotspot, USB, Bluetooth) are not networks the phone uses, so their devices are let in. |
| `Nmea.java` | Relays the chip's GGA/RMC/GSA/GST/VTG from `OnNmeaMessageListener`: split at every `$`, checksums checked, CRLF added, GSV and proprietary lines dropped. Builds GGA/RMC from a `Location` with integer arithmetic only: no locale commas, no E notation, minutes carried at 60. |
| `MainActivity.java` | The one screen. **Set up everything** walks through precise location, notifications, the battery exemption and "all the time" location, then starts sharing and opens the hotspot settings. |
| `BootReceiver.java` | Restarts sharing after a reboot or an app update, if it was on. Android lets a location service started from the background read the GPS only with "all the time" location. Without it, the app posts a "tap to start" notification instead. The same notification appears when Android restarts the service after killing the process and cannot give it the GPS. |

**Which sentences:**
- The chip's own NMEA when the phone gives it to apps (most do). Its GGA has real satellite
  counts and HDOP.
- Otherwise, 2.5 s without a chip GGA (and 3 s after the GPS starts), GGA + RMC are built from
  Android's fix:
  - satellites from `GnssStatus` (empty when Android reports 0 beside a fix);
  - HDOP = accuracy / 4 m;
  - altitude only when Android 14+ gives one above mean sea level.
- With nothing to send for 4 s, a "no fix" GGA, so the device knows the app is alive. The WiPhone
  treats 10 s of silence as a dead link. It is a GGA only, because a "V" RMC stamped with the same
  second as a real fix would make the WiPhone discard that fix.
- Positions from a mock-location app are never sent, and the screen says so.

## Build

You need JDK 17 and the Android SDK (platform 35, build-tools 35). Android Studio has both, or use
the command-line tools. Then:

```bash
cd android
./gradlew testDebugUnitTest
```

```bash
./gradlew assembleDebug
```

- The debug APK is `app/build/outputs/apk/debug/app-debug.apk`.
- `./gradlew assembleRelease lintRelease` builds the release. It is signed only when
  `~/.gradle/gradle.properties` names a keystore:

```properties
NHCOMPANION_STORE_FILE=/path/to/your.jks
NHCOMPANION_STORE_PASSWORD=...
NHCOMPANION_KEY_ALIAS=...
NHCOMPANION_KEY_PASSWORD=...
```

- The APK in `dist/` is signed with the maintainer's key, which is kept outside this repo.
  Android installs an update only when it is signed with the same key as the installed copy.
  So moving between that APK and your own build means uninstalling first.

## Test without a phone

The unit tests (`app/src/test`) cover the NMEA text, the network policy and the port: fan-out,
refusals, a device that stops reading, a busy port.

For the whole app, use the emulator (a `default` x86_64 or arm64 image). **Debug builds let
loopback in**, so `adb forward` reaches the port. In debug builds only, two extras on the launch
intent help:
- `--ez anynet true` also lets in connections from the emulator's own network (to test through
  Doze);
- `--ez made true` builds sentences from the fix even when the chip sends text.

1. Forward the port:

   ```bash
   adb forward tcp:10120 tcp:10110
   ```

2. Read the stream through the WiPhone's own gate, built from the firmware sources unchanged:

   ```bash
   c++ -std=c++11 -O1 -IWiPhone tools/phone_stream_check.cpp WiPhone/self_pos.cpp WiPhone/mesh_pos.cpp WiPhone/nmea.cpp -o /tmp/phone_stream_check
   ```

   ```bash
   /tmp/phone_stream_check 127.0.0.1 10120 60 --show
   ```

3. In another terminal, feed the emulator positions (lon lat alt sats). It holds a position
   only while you keep sending it, so repeat this once a second:

   ```bash
   adb emu geo fix -0.0015 51.4779 46 9
   ```

4. Screen off and deep Doze:

   ```bash
   adb shell dumpsys battery unplug
   ```

   ```bash
   adb shell input keyevent KEYCODE_SLEEP
   ```

   ```bash
   adb shell dumpsys deviceidle enable deep
   ```

   ```bash
   adb shell dumpsys deviceidle force-idle deep
   ```

`phone_stream_check` exits 0 when the WiPhone would have kept a fix and the link never went
silent. It prints coordinates only with `--show`, so leave that off when reading a real phone.

**What the emulator cannot show:**
- The chip-NMEA path. The emulator turns `geo nmea` into a fix and never passes the text on.
- A phone maker's own battery management (Samsung's app sleeping, for example).

Both need a real phone.

**Tested 2026-10-11 on an Android 15 emulator:**
- setup, start at boot, restart after a crash;
- a 90 s stream with the screen off, and another in forced deep Doze over a non-loopback
  connection: 77 of 78 fixes kept;
- refusal of the mobile-data side, of an app connecting to the phone's own address, and, in the
  release build, of loopback;
- when Android restarts the service without "all the time" location, the "paused, tap to share"
  notification appears, and one tap brings sharing back.
