#include "serial_cmd.h"
#include "app_gbc_xfer.h"
#include "sms_mirror_poll.h"
#include "app_books.h"       // booksDebugDumpPage, the `bookpage` command
#include "app_photos.h"      // photosSetWallpaper, the `wallpaper set` command
#include "config.h"          // WIPHONE_KEY_*, the `key` command
#include "Storage.h"         // CriticalFile / ConfigsFile, the `lock` command

/* Defined in WiPhone.ino next to keypadBuff — see the note there on why this is a real press. */
extern bool uiInjectKey(char c);
#include "meshtastic_service.h"
#include "t9_extra.h"            // the `t9` command reports the extra dictionary   // applyChannelUrl, the `chan` command
#include "mesh_pos.h"             // distance/bearing for the `pos` command
#include "sun_times.h"            // legal light, the `sun` command
#include "clock.h"                // ntpClock
#include "GUI.h"                  // gui.state, the `sip` command
#include <SD.h>                   // the `wallpaper` command reads both filesystems
#include "mesh_phy.h"             // meshPhy.benchSleep, the `power lora` toggle
#include "Networks.h"             // wifiState.radioOff(), the `power sleep` gate
#include "Hardware.h"             // KEYBOARD_INTERRUPT_PIN, the `power sleep` wake pin
#include "Audio.h"                // audio->isOn(), the `power` guards
#include <driver/i2s.h>           // i2s_stop/i2s_start, the `power i2s` toggle
#include <esp_sleep.h>            // esp_light_sleep_start, the `power sleep` floor
#include <SPIFFS.h>
#include <WiFi.h>

#include <Arduino.h>
#include <driver/uart.h>
#include <esp_heap_caps.h>   // multi_heap_info_t, the `heap` command
#include <esp_phy_init.h>    // esp_phy_erase_cal_data_in_nvs, `wifi calreset`
#include <esp_wifi.h>        // esp_wifi_restore, `wifi restore`
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>     // strtoul, the `dm` command's node number
#include <string.h>
#include <strings.h>

#ifdef USER_SERIAL      // (Hardware.h, via GUI.h) — the `gps` command's plumbing
#include "nmea.h"
#include <Preferences.h>
extern bool       gGpsNmea;      // WiPhone.ino: routes the user UART to the NMEA reader
extern uint32_t   gGpsBaud;      // ...at THIS rate; USER_SERIAL_BAUD is the GUI path's
extern NmeaReader gGpsReader;
extern void       gpsApplyBaud(bool gpsOn);            // WiPhone.ino owns the UART object
extern int        gpsRawSnapshot(uint8_t* out, int cap);
#endif

/* ⚠ THE IDF UART API, NOT `Serial`. THIS IS THE WHOLE REASON THE FIRST VERSION READ NOTHING.
 *
 * `setup()` configures UART0 with `uart_param_config()` + `uart_driver_install(UART_NUM_0,
 * ...)` and NEVER CALLS `Serial.begin()`. So the Arduino `HardwareSerial` object is
 * uninitialised: `Serial.available()` always answers 0 and `Serial.read()` never returns a
 * byte, while log output still appears on the port perfectly — because the ESP log path
 * writes to UART0 through the IDF driver, not through `Serial`.
 *
 * That combination is nastily convincing: the port is plainly alive in one direction, so a
 * console built on `Serial` looks like "the phone is ignoring my commands" rather than "this
 * object was never started". Reading through the same driver that owns the port is correct
 * AND unambiguous. The driver is installed with a 2 KB RX ring, so typed input is buffered
 * for us between loop passes.
 */
static const uart_port_t PORT = UART_NUM_0;

/* 512, not 48: `chan <url>` carries a Meshtastic invite URL, and a complete four-channel
 * invite runs ~370 characters. Truncated input is not run (see below), so a small buffer
 * would make the one command this console exists to save you with silently impossible. */
static char     s_buf[512];
static uint16_t s_len = 0;

static void say(const char* fmt, ...) {
  char out[192];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(out, sizeof(out), fmt, ap);
  va_end(ap);
  if (n > 0) {
    uart_write_bytes(PORT, out, (size_t)(n < (int)sizeof(out) ? n : (int)sizeof(out) - 1));
  }
}

/* ⚠ One say() per line. say()'s buffer is 192 bytes and this text is ~750:
 * a single call TRUNCATED the help mid-list, so every command added after
 * `chan` was invisible to `?` — which is the one place a stranded user looks.
 * (Found by the adversarial review; it had been silently cut for releases.) */
static void help() {
  static const char* LINES[] = {
    "",
    "WiPhone serial commands:",
    "  ?          this help",
    "  up on      start the WiFi uploader (files land in /roms)",
    "  up on books|photos  same, into /books or /photos",
    "  up off     stop the uploader",
    "  up         where to point a browser",
    "  sync       poll COVEY for mirrored texts now",
    "  mirror     mirror poller state",
    "  sip        SIP account state: loaded, registered, WiFi - one line",
    "  bookpage   dump the open reader page's layout + rendering",
    "  keys       keypad health: why a press went missing (drained/rescued/swept)",
    "  health     dump /health.log over the CABLE (battery + restart black box)",
    "  health all dump the whole file, not just the last 24 KB",
    "  chan <url> apply a Meshtastic channel invite URL",
    "  chans      list the channels this phone has",
    "  meshdb     chat history: which filesystem, what loaded, what the next save keeps",
    "  wifi drop  simulate a hotspot blip, to measure the reconnect path",
    "  wifi scan  what the radio can actually hear - deaf radio vs absent AP",
    "  wifi bounce  radio off/on without a reboot - the deaf-scan cure, keeps saved APs",
    "  wifi calreset  erase the stored RF calibration and reboot - the deaf-radio probe",
    "  star [<!node>]  list starred nodes, or toggle one (top of list, evicted last)",
    "  send <i> <text>  send a channel text (index from `chans`) - proves the broadcast receipt",
    "  pki        DM crypto state: our key, who has keys, stack headroom",
    "  announce   broadcast NodeInfo now, asking others to answer with theirs",
    "  dm <!node> <text>  send a direct message (PKI when the key is known)",
    "  heap       memory truth: internal/DMA/PSRAM free+largest+floor",
    "  audio      audio device: powered? moving samples? who is entitled to it?",
    "  replay     history-replay state: ring occupancy, pending tx, last served",
    "  nbr        neighbours heard DIRECTLY + announce state (My node > Neighbor info)",
    "  nbr on|4h|off|now  set the announce cadence (1h/4h) or announce right now",
    "  pos        positions: waypoints, node fixes, our pin, the reference, our beacon",
    "  pos every <secs>|off  GPS position reporting cadence (persists; 300s floor)",
    "  pos now    force one beacon: obeys the channel/fix/spacing rules but NOT",
    "             the on/off switch - it sends with reporting OFF",
    "  gps        woods-plate GPS state: fix, sats, reader counters",
    "  gps on|off route the user UART (38/32) to the NMEA reader (persists)",
    "  gps baud <n>  GPS baud, persists (115200 = the M100 Mini, measured; not 9600)",
    "  gps raw    hex+ASCII of the last bytes off the wire - tells wrong-baud from binary",
    "  sun        legal light at the reference place: dawn/sunrise/sunset/dusk",
    "  lock       why the screen does or does not lock: setting, sleep gate, and the card",
    "  ver        firmware version and build time of the binary actually running",
    "  scrim [<alpha> [hex]]  the grey plate under menu text over a wallpaper (RAM only)",
    "  shot       dump the live screen as base64 (tools/shot.py turns it into a PNG)",
    "  ls [<dir>] list a card folder, HIDDEN entries included (marked *) - the one view",
    "             that shows dotfiles; the Files app and the pickers never do",
    "  rm </path> delete ONE file by full path (refuses folders and relative names)",
    "  power      the USB-power-meter bench: state of every switch below, and the method",
    "  power lcd sleep|wake   ST7789 DISPOFF+SLPIN / SLPOUT+DISPON (backlight untouched)",
    "  power i2s stop|start   the I2S peripheral+DMA that runs from boot whether or not",
    "             anything ever played",
    "  power lora sleep|rx    park the SX1276 in SLEEP (mesh DEAF) / back to RX-continuous",
    "  power sleep <secs>     light-sleep the ESP32 for N s (WiFi must be OFF): the floor",
    "  key <names>  press keys: select/menu back ok up down left right call end f1-f4,",
    "             or a single character. `key menu`, `key down down ok`. Real presses -",
    "             they go into the keypad buffer, so the whole UI path runs unchanged",
    "  wallpaper  what the background loader found, and why it did or did not use it",
    "  wallpaper reload|list|clear  re-read it / list /photos / drop the override",
    "  wallpaper set <name>  set /photos/<name> as the wallpaper - the SAME code the",
    "             Photos menu runs, so it can be proven over the cable",
    "  unread     recount unread texts, repair the counter, name the threads",
    "  unread clear  mark EVERYTHING read (orphaned threads included)",
    "",
  };
  for (size_t i = 0; i < sizeof(LINES) / sizeof(LINES[0]); i++) {
    say("%s\n", LINES[i]);
  }
}

/* `pos` — the whole positions picture in one paste: every waypoint heard, every
 * node with a fix (age in minutes), our own pin, which reference distances are
 * measured from, and — since the woods plate — what this phone is telling the
 * world about ITSELF. That last block is the one worth pasting into a bug
 * report: it answers "is my location going out, to whom, and when did it last
 * go" in five lines. */
static void reportPos() {
  int32_t refLat = 0, refLon = 0;
  char refName[20] = "";
  bool haveRef = meshService.resolveReference(&refLat, &refLon, refName, sizeof(refName));
  /* Name the TRUE missing piece: printing "no waypoints heard" directly above a listed
   * waypoint misleads — with waypoints in the DB the fix is CHOOSING one (2026-08-20). */
  say("pos: reference = %s\n",
      haveRef ? refName
      : meshService.getWaypointCount() > 0
          ? "NONE - waypoints heard but none chosen (Places > pick > set reference)"
          : "NONE (no waypoints heard, no pin set)");

  for (int i = 0; i < meshService.getWaypointCount(); i++) {
    const MeshWaypoint* w = meshService.getWaypoint(i);
    if (!w) continue;
    say("  wp '%s' id=%u  %d.%05d,%d.%05d%s\n", w->name, (unsigned)w->id,
        (int)(w->latI / 10000000), abs((int)((w->latI % 10000000) / 100)),
        (int)(w->lonI / 10000000), abs((int)((w->lonI % 10000000) / 100)),
        w->expire ? " (expires)" : "");
  }

  uint32_t nowMs = millis();
  for (int i = 0; i < meshService.getNodeCount(); i++) {
    const MeshNode* n = meshService.getNode(i);
    if (!n || n->posHeardMs == 0) continue;
    char dist[16] = "";
    char extra[40] = "";
    if (haveRef) {
      double m = meshPosDistanceM(refLat, refLon, n->latI, n->lonI);
      meshPosFmtDist(m, dist, sizeof(dist));
      snprintf(extra, sizeof(extra), "  %s %s of %s", dist,
               meshPosCompass8(meshPosBearingDeg(refLat, refLon, n->latI, n->lonI)), refName);
    }
    char age[16];
    if (n->posHeardMs == 1) {
      // The restored-from-flash sentinel: the fix is real, its age is unknown.
      snprintf(age, sizeof(age), "old");
    } else {
      snprintf(age, sizeof(age), "%um ago", (unsigned)((nowMs - n->posHeardMs) / 60000u));
    }
    say("  !%08x '%s'  %d.%05d,%d.%05d%s  (%s)\n", (unsigned)n->nodeNum, n->name,
        (int)(n->latI / 10000000), abs((int)((n->latI % 10000000) / 100)),
        (int)(n->lonI / 10000000), abs((int)((n->lonI % 10000000) / 100)),
        extra, age);
  }

  int32_t pinLat, pinLon;
  uint32_t pinAt;
  if (meshService.getMyPin(&pinLat, &pinLon, &pinAt)) {
    say("pos: our pin %d.%05d,%d.%05d (announced to the mesh)\n",
        (int)(pinLat / 10000000), abs((int)((pinLat % 10000000) / 100)),
        (int)(pinLon / 10000000), abs((int)((pinLon % 10000000) / 100)));
  } else {
    say("pos: no pin (Meshtastic > Places > pick one > I'm here)\n");
  }

  // ---- what this phone broadcasts about itself -------------------------------
  const uint32_t pi = meshService.getPosInterval();
  if (pi == 0) {
    say("pos: reporting OFF (My node > Report position)\n");
  } else {
    const char* chn = meshService.getPosChannelName();
    const MeshChannel* pc = meshService.getPosChannel();
    say("pos: reporting every %lus to '%s'%s%s\n", (unsigned long)pi,
        chn[0] ? chn : "(no channel)",
        !chn[0] ? "" : (pc ? "" : " [GONE from this phone]"),
        (pc && meshService.channelIsPublic(pc))
            ? (meshService.posChannelWasPublic() ? " [PUBLIC - confirmed]"
                                                 : " [PUBLIC - REFUSED]")
            : "");
    const char* why = meshService.posBlockedReason();
    say("pos: %s\n", why ? why : "armed and clear to send");
    /* ⚠ "A slot is owed" is a DIFFERENT state from "blocked", and the difference is the whole
     * point of the 2026-08-25 change: blocked says why nothing can go now, this says the phone
     * is holding a slot open and will send the instant a fix arrives. Without this line the
     * two are indistinguishable on the console. */
    if (meshService.posBeaconDue()) {
      say("pos: A SLOT IS OWED - it will send the moment a fresh fix arrives\n");
    }
    if (meshService.getPosLastTxMs() == 0) {
      say("pos: never beaconed (first send is a full interval after arming)\n");
    } else {
      say("pos: last beacon %lus ago, %s, %d slot(s) skipped since\n",
          (unsigned long)((millis() - meshService.getPosLastTxMs()) / 1000UL),
          meshService.posLastSendOk() ? "SENT" : "FAILED",
          meshService.getPosSkipRuns());
    }
  }
}

/* Base64 (std alphabet, padded) — how the Meshtastic apps display public keys,
 * so Nick can eyeball-compare against what COVEY shows for this phone. */
static void b64enc(const uint8_t* in, int len, char* out, int outCap) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int o = 0;
  for (int i = 0; i < len && o + 5 < outCap; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
    if (i + 2 < len) v |= in[i + 2];
    out[o++] = T[(v >> 18) & 63];
    out[o++] = T[(v >> 12) & 63];
    out[o++] = (i + 1 < len) ? T[(v >> 6) & 63] : '=';
    out[o++] = (i + 2 < len) ? T[v & 63] : '=';
  }
  out[o] = '\0';
}

/* `pki` — everything needed to see WHY a DM does or does not flow, in one paste:
 * our announced key (compare against COVEY's node list), each node's key state
 * (no key = DMs to them go legacy and 2.5+ nodes drop them), MISMATCH flags
 * (somebody re-keyed; Clear nodes to re-trust), and the loop task's stack
 * high-water mark (the X25519 derive is the deepest stack user we added). */
static void reportPki() {
  char b64[48];
  if (!meshService.pkiIsReady()) {
    say("pki: NOT READY - no keypair (NVS trouble?)\n");
    return;
  }
  b64enc(meshService.pkiPublicKey(), 32, b64, sizeof(b64));
  say("pki: our key %s\n", b64);
  int keyed = 0;
  for (int i = 0; i < meshService.getNodeCount(); i++) {
    const MeshNode* n = meshService.getNode(i);
    if (!n || n->nodeNum == meshService.getMyNodeNum()) {
      continue;
    }
    if (n->pkiFlags & MESH_NODE_HAS_KEY) {
      keyed++;
      b64enc(n->pubKey, 32, b64, sizeof(b64));
      say("  !%08x '%s' key %s%s\n", (unsigned)n->nodeNum, n->name, b64,
          (n->pkiFlags & MESH_NODE_KEY_MISMATCH) ? "  [MISMATCH SEEN - Clear nodes to re-trust]" : "");
    } else {
      say("  !%08x '%s' NO KEY - their DMs to us fail, ours to them go legacy\n",
          (unsigned)n->nodeNum, n->name);
    }
  }
  say("pki: %d node(s) with keys | loop-task stack floor %u bytes free\n",
      keyed, (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
}

/* `heap` — the memory instrument (upload-redesign brief: first deliverable).
 * Internal and PSRAM tell different stories on this phone: WiFi RX buffers,
 * lwIP, and every operator-new live INTERNAL-only, so "plenty of PSRAM free"
 * is never the number that saves you. min-ever is the low-water mark since
 * boot — if it sits far below current free, something already had a bad
 * moment this run. DMA is the subset the radio actually allocates from. */
static void reportHeap() {
  multi_heap_info_t h;
  heap_caps_get_info(&h, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  say("heap: internal free=%u largest=%u min-ever=%u  blocks used/free=%u/%u\n",
      (unsigned)h.total_free_bytes, (unsigned)h.largest_free_block,
      (unsigned)h.minimum_free_bytes,
      (unsigned)h.allocated_blocks, (unsigned)h.free_blocks);
  heap_caps_get_info(&h, MALLOC_CAP_DMA);
  say("heap: dma      free=%u largest=%u  (the pool WiFi RX buffers draw from)\n",
      (unsigned)h.total_free_bytes, (unsigned)h.largest_free_block);
  heap_caps_get_info(&h, MALLOC_CAP_SPIRAM);
  say("heap: psram    free=%u largest=%u min-ever=%u\n",
      (unsigned)h.total_free_bytes, (unsigned)h.largest_free_block,
      (unsigned)h.minimum_free_bytes);
  say("heap: loop-task stack floor %u bytes free\n",
      (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
}

static void reportUploader() {
  if (!gbcXferOn()) {
    say("uploader: off\n");
    return;
  }
  say("uploader: ON  http://%s/  (or http://wiphone.local/)%s\n",
      xferAddr(), xferUsingAP() ? "  [own hotspot]" : "");
  say("          legacy (no-JS / curl -F / log): http://%s:8080/\n", xferAddr());
  say("          hotspot SSID if used: %s   files added: %d\n",
      xferApName(), xferFilesAdded());
}

static void run(char* line) {
  while (*line == ' ') {
    line++;
  }
  if (!*line || !strcmp(line, "?") || !strcasecmp(line, "help")) {
    help();
    return;
  }

  if (!strcasecmp(line, "up")) {
    reportUploader();
    return;
  }
  if (!strcasecmp(line, "up on books")) {
    xferStart(xferBooksConfig());       // the Books uploader, no hands on the phone
    reportUploader();
    return;
  }
  if (!strcasecmp(line, "up on photos")) {
    xferStart(xferPhotosConfig());      // the gallery, same reason as books
    if (!gbcXferOn() && xferStartError()) {
      say("up: NOT started - %s\n", xferStartError());
    }
    reportUploader();
    return;
  }
  if (!strcasecmp(line, "up on t9")) {
    /* Upload a T9 word list, then `t9 reload` to pick it up without a reboot. */
    xferStart(xferT9Config());
    if (!gbcXferOn() && xferStartError()) {
      say("up: NOT started - %s\n", xferStartError());
    }
    reportUploader();
    return;
  }
  if (!strcasecmp(line, "up on")) {
    if (gbcXferOn()) {
      say("uploader already on\n");
    } else {
      gbcXferStart();
      /* A refusal is not a silent no-op: the bench needs the reason as much as
       * the screen does. See the heap guard in xferStart(). */
      if (!gbcXferOn() && xferStartError()) {
        say("up: NOT started - %s\n", xferStartError());
      }
    }
    reportUploader();
    return;
  }
  if (!strcasecmp(line, "up off")) {
    /* ⚠ Worth taking down rather than leaving up, and not only to save power: while the
     * uploader is on screen the Games app is running, and sipMayPoll() is false — so the
     * SMS mirror poller will not open a socket. Uploading its config and then leaving the
     * uploader up looks exactly like the poller being broken. That is what this was for. */
    if (!gbcXferOn()) {
      say("uploader already off\n");
    } else {
      xferStop();
      say("uploader: off\n");
    }
    return;
  }

  if (!strcasecmp(line, "sync")) {
    if (!smsMirrorPollConfigured()) {
      say("mirror: not configured (%s)\n", smsMirrorPollStatus());
      return;
    }
    smsMirrorPollRequestNow();
    say("mirror: poll requested; watch for a SMSMIRROR line\n");
    return;
  }
  if (!strcasecmp(line, "mirror")) {
    say("mirror: %s (%sconfigured)\n", smsMirrorPollStatus(),
        smsMirrorPollConfigured() ? "" : "not ");
    return;
  }
  if (!strcasecmp(line, "bookpage")) {
    booksDebugDumpPage();
    return;
  }

  /* `keys` — why a keypress went missing. A dropped press logs nothing and is
   * indistinguishable from a bad thumb, which is how the SN7326's 10ms INT pulse stayed
   * hidden behind "the menus miss the odd button" for two years. Each counter's meaning
   * is written above keypadHealth() in WiPhone.ino. */

  /* `health` — the battery/restart black box, over the cable rather than over WiFi.
   * See healthDump() in WiPhone.ino for why the HTTP route is the dangerous one. */
  if (!strcasecmp(line, "health") || !strcasecmp(line, "health all")) {
    extern int healthDump(uint32_t lastBytes);
    /* Exact matches, not a prefix test: a mistyped argument should reach the "unknown
     * command" reply rather than quietly dumping the whole file over the cable. */
    const bool all = !strcasecmp(line, "health all");
    say("--- health.log %s ---\n", all ? "whole file" : "last 24 KB");
    const int total = healthDump(all ? 0u : 24u * 1024u);
    if (total < 0) {
      say("health: nothing to read - no card, or no /health.log yet\n");
    }
    /* ⚠ The trailer (size, cap, trim point) is printed by healthDump() itself, NOT here.
     * The first version restated the cap as literals in this file and they were STALE the
     * same hour — the cap was raised in WiPhone.ino and this line went on confidently
     * reporting the old one. A diagnostic that misreports the thing it exists to report is
     * worse than no diagnostic. Only the file that owns the constants may print them. */
    return;
  }

  if (!strcasecmp(line, "keys")) {
    extern int keypadHealth(char* out, int cap);
    char buf[200];
    keypadHealth(buf, sizeof(buf));
    say("keys: %s\n", buf);
    return;
  }

  /* `audio` — is the codec powered, and is anything actually using it?
   *
   * ⚠ THIS REPORTS THE FIRMWARE'S SHADOW STATE, NOT THE CHIP'S. The WM8750 is addressed
   * write-only over its 2-wire interface (src/drivers/WM8750.h has setReg and no readReg), so
   * the registers cannot be read back to check. If the shadow and the chip ever disagree, this
   * command will say the comfortable thing — which is exactly why the idle watchdog in
   * WiPhone.ino releases the device on a timer rather than trusting anyone's bookkeeping.
   *
   * The line to look for is `powered=YES moving=no`: that is the leak. */
  if (!strcasecmp(line, "audio")) {
    /* Same shape as `keys` and `health`: the state lives in WiPhone.ino, where the SIP,
     * emulator, music and vibro state are all visible; this just prints what it hands back. */
    extern int audioStateDump(char* out, int cap);
    char buf[420];
    audioStateDump(buf, sizeof(buf));
    say("%s", buf);
    return;
  }
  /* `keys raw` — what the CHIP actually sent, oldest first. The counters say a release
   * went missing; only this says whether it was never emitted, mis-decoded, or stranded. */
  if (!strcasecmp(line, "keys raw")) {
    extern int keypadTrace(char* out, int cap);
    char buf[1400];
    keypadTrace(buf, sizeof(buf));
    say("keys raw (oldest first):\n%s\n", buf);
    return;
  }

  /* `chan <url>` — apply a channel invite straight over the cable. Born of a real jam:
   * a chip erase cost the phone its channels, and re-sharing them over the mesh was
   * impossible — the RAK's 2.7 firmware NAKs every DM to this phone with
   * PKI_SEND_FAIL_PUBLIC_KEY (the phone implements no PKC), and an invite broadcast on
   * the primary channel would hand the PSK to everyone in RF range. Serial has neither
   * problem: private by cable, works when the mesh is exactly the broken part. */
  if (!strncasecmp(line, "chan ", 5)) {
    const char* url = line + 5;
    while (*url == ' ') {
      url++;
    }
    int added = meshService.applyChannelUrl(url);
    say("chan: %d channel(s) added\n", added);
    return;
  }
  /* `sip` — is an account actually LOADED and registered? Added while chasing "typing a
   * number doesn't complete": the completion (and the mirror, and texting) all hang off
   * the ACTIVE account's URI, and an account that was typed into the list but never
   * SELECTED is invisible to all of them. This prints the truth in one line. */
  if (!strcasecmp(line, "sip")) {
    extern GUI gui;
    const char* uri = gui.state.fromUriDyn;
    say("sip: account %s%s%s | registered: %s | wifi: %s\n",
        (uri && uri[0]) ? "LOADED (" : "NOT LOADED",
        (uri && uri[0]) ? uri : "",
        (uri && uri[0]) ? ")" : " - open SIP accounts and SELECT one",
        gui.state.sipRegistered ? "yes" : "no",
        (WiFi.status() == WL_CONNECTED) ? "up" : "DOWN");
    return;
  }
  if (!strcasecmp(line, "pki")) {
    reportPki();
    return;
  }
  /* Predictive text on/off from the console. The Settings row does the same thing; this
   * exists because the feature has to be testable over the serial bridge without a finger
   * on the phone, and because turning it off in the field must not need a reflash. */
  if (!strncasecmp(line, "t9", 2) && (line[2] == '\0' || line[2] == ' ')) {
    extern GUI gui;
    const char* arg = line[2] ? line + 3 : "";
    while (*arg == ' ') {
      arg++;
    }
    if (!strcasecmp(arg, "on")) {
      gui.state.t9Enabled = true;
    } else if (!strcasecmp(arg, "off")) {
      gui.state.t9Enabled = false;
    } else if (!strcasecmp(arg, "reload")) {
      /* Re-read the card after an upload. The alternative is a reboot, which loses the
       * uploader session you just used to put the file there. */
      const int n = t9ExtraLoad(gui.state.cardPresent);
      gui.state.t9.setExtra(t9ExtraGet());
      say("t9: reloaded, %d extra words (%s)\n", n, t9ExtraStatus());
      return;
    } else if (*arg) {
      say("t9: say `t9 on`, `t9 off`, `t9 reload`, or `t9` to see the state\n");
      return;
    }
    say("t9: predictive text %s  (this field opted in: %s, word pending: %s)\n",
        gui.state.t9Enabled ? "ON" : "OFF",
        gui.state.t9Field ? "yes" : "no",
        gui.state.t9.pending() ? gui.state.t9.digits() : "no");
    /* The extra dictionary is invisible until it is wrong, so say what happened to it. A file
     * that failed to load looks exactly like a file with none of your words in it. */
    say("  extra words: %d from %s  (%s)\n", t9ExtraCount(), T9_EXTRA_PATH, t9ExtraStatus());
    return;
  }

  if (!strcasecmp(line, "heap")) {
    reportHeap();
    return;
  }
  /* `replay` — the mesh-history replay's whole state in one line (the feature
   * is otherwise invisible on this phone, which is the point: it exists so
   * COVEY can ask what it missed — docs/replay-spec.md). */
  /* `nbr` — who is in DIRECT earshot, with signal and age. The map-building
   * diagnostic: a node listed here is one hop away, and anything NOT listed we
   * only ever heard through a relay. */
  /* `nbr on|4h|off` mirrors the My node > Neighbor info row (same pref), and
   * `nbr now` forces one announce — the bench cannot wait an hour to find out
   * whether the packet is well formed. */
  if (!strncasecmp(line, "nbr ", 4)) {
    const char* arg = line + 4;
    while (*arg == ' ') arg++;
    if (!strcasecmp(arg, "on") || !strcasecmp(arg, "1h")) {
      meshService.setNeighborInterval(3600);
      say("nbr: announcing every 1h\n");
    } else if (!strcasecmp(arg, "4h")) {
      meshService.setNeighborInterval(14400);
      say("nbr: announcing every 4h\n");
    } else if (!strcasecmp(arg, "off")) {
      meshService.setNeighborInterval(0);
      say("nbr: announcing OFF\n");
    } else if (!strcasecmp(arg, "now")) {
      const int n = meshService.announceNeighborsNow();
      say("nbr: announced on %d private channel(s)%s\n", n,
          n ? "" : " - none configured, nothing sent");
    } else {
      say("nbr: usage nbr on|4h|off|now\n");
    }
    return;
  }
  if (!strcasecmp(line, "nbr")) {
    const uint32_t iv = meshService.getNeighborInterval();
    const char* chn = meshService.neighborChannelName();
    say("nbr: announce %s%s | channel %s | %d direct neighbour(s)\n",
        iv ? "every " : "OFF", iv ? (iv == 3600 ? "1h" : "4h") : "",
        chn ? chn : "NONE (primary is public - nothing sent)",
        meshService.getDirectNeighborCount());
    for (int i = 0; i < meshService.getDirectNeighborCount(); i++) {
      uint32_t node = 0, age = 0;
      int snr = 0;
      if (meshService.getDirectNeighbor(i, &node, &snr, &age)) {
        say("  !%08x  snr %d dB  heard %lum ago\n", (unsigned)node, snr,
            (unsigned long)(age / 60000UL));
      }
    }
    if (meshService.lastNeighborTxMs()) {
      say("nbr: last announce %lum ago, %d neighbour(s) in it\n",
          (unsigned long)((millis() - meshService.lastNeighborTxMs()) / 60000UL),
          meshService.lastNeighborTxCount());
    } else {
      say("nbr: never announced yet\n");
    }
    return;
  }
  if (!strcasecmp(line, "replay")) {
    if (meshService.replayLastServedMs()) {
      say("replay: ring %d heard | pending tx %d pkt(s) | last served %d rec(s) %lus ago\n",
          meshService.replayRingCount(), meshService.replayPendingPackets(),
          meshService.replayLastServedN(),
          (unsigned long)((millis() - meshService.replayLastServedMs()) / 1000));
    } else {
      say("replay: ring %d heard | pending tx %d pkt(s) | never served a request\n",
          meshService.replayRingCount(), meshService.replayPendingPackets());
    }
    return;
  }
  if (!strcasecmp(line, "pos")) {
    reportPos();
    return;
  }
  if (!strncasecmp(line, "pos ", 4)) {
    const char* arg = line + 4;
    while (*arg == ' ') {
      arg++;
    }
    if (!strncasecmp(arg, "every", 5)) {
      const char* n = arg + 5;
      while (*n == ' ') {
        n++;
      }
      if (!strcasecmp(n, "off")) {
        meshService.setPosInterval(0);
        say("pos: reporting OFF\n");
        return;
      }
      const uint32_t s = (uint32_t)strtoul(n, NULL, 10);
      /* 🔑 300 s floor, RAISED FROM 60 s: the bench must not be able to persist a
       * cadence the GUI cannot represent or undo. At 60 s this is 518 ms of air
       * every minute — 0.86% duty, ~15x the most aggressive GUI option (300 s,
       * 0.173%) and ~5x its default — on a band shared with COVEY and everything
       * else in range. Worse, it SURVIVES A REBOOT in NVS while the menu renders
       * it as "every 1 min" and collapses it to off on the next press, so the
       * user cannot see it or fix it from the phone. 300 s is the most aggressive
       * value the GUI itself offers; the bench does not get to invent a faster one. */
      if (s < 300 || s > 86400) {
        say("pos: interval out of range (300..86400 seconds, or off)\n");
        return;
      }
      meshService.setPosInterval(s);
      say("pos: reporting every %lus - first send is a full interval away\n",
          (unsigned long)s);
      return;
    }
    /* 🛑 `pos chan` IS REFUSED, DELIBERATELY. The consent that makes a location
     * beacon safe is the two-press confirm on the picker screen, where the word
     * PUBLIC is in the large font. A serial one-liner is not that consent, and
     * a bench command that can silently retarget somebody's live location
     * defeats every safeguard on the screen. Read-only here, on purpose. */
    if (!strncasecmp(arg, "chan", 4)) {
      say("pos: refused - choose the channel on the phone\n");
      say("pos: Meshtastic > My node > Send to (the public channel needs two presses)\n");
      return;
    }
    /* `pos now` — bench only: force one beacon regardless of the interval and
     * the movement gate. It still obeys every SAFETY rule (receiver on, channel
     * named, channel present, channel not silently public, fix fresh enough),
     * because those are what the feature is for.
     * ⚠ BLOCKS ~518 ms inside meshPhy.send(), like `nbr now` does. */
    if (!strcasecmp(arg, "now")) {
      const bool ok = meshService.sendGpsPositionNow();
      say("pos: forced beacon %s%s\n", ok ? "SENT" : "NOT SENT",
          ok ? " (37 B on air with the clock set - see the MESH POSITION log line)" : "");
      if (!ok) {
        const char* why = meshService.posBlockedReason();
        say("pos: %s\n", why ? why : "radio not ready, or no channels");
      }
      return;
    }
    say("pos: usage - pos | pos every <secs>|off | pos now  (channel: on the phone)\n");
    return;
  }
  /* `gps` — the woods backplate's GPS half, dormant until `gps on`. The status
   * line leads with the counters because they answer the first bench question:
   * bytes rising + sentences 0 = wrong baud; both 0 = wiring; sentences rising
   * with no fix = give the antenna sky. */
  if (!strcasecmp(line, "gps") || !strncasecmp(line, "gps ", 4)) {
#ifdef USER_SERIAL
    const char* arg = line[3] ? line + 4 : "";
    while (*arg == ' ') {
      arg++;
    }
    if (!strcasecmp(arg, "on") || !strcasecmp(arg, "off")) {
      gGpsNmea = !strcasecmp(arg, "on");
      Preferences p;
      p.begin("wpmesh", false);
      p.putBool("gpsen", gGpsNmea);
      p.end();
      gpsApplyBaud(gGpsNmea);      // the two consumers do NOT share a baud
      /* ...and tell the service, or resolveReference() keeps answering "GPS"
       * with coordinates from a receiver that just stopped. One bool, no I/O. */
      meshService.setGpsEnabled(gGpsNmea);
      gGpsReader.reset();          // counters answer "since when", so start clean
      say("gps: NMEA reader %s (user UART rx=%d tx=%d @ %u)%s\n",
          gGpsNmea ? "ON" : "OFF", USER_SERIAL_RX, USER_SERIAL_TX,
          (unsigned)(gGpsNmea ? gGpsBaud : USER_SERIAL_BAUD),
          gGpsNmea ? "" : " - user-serial GUI path restored");
      return;
    }
    /* `gps baud <n>` — the bench's answer to a wrong-rate module, without a
     * reflash per guess. Persisted, and applied to the live port only while the
     * reader owns it. */
    if (!strncasecmp(arg, "baud", 4)) {
      const char* n = arg + 4;
      while (*n == ' ') {
        n++;
      }
      if (!*n) {
        say("gps: baud %u (default %u)\n", (unsigned)gGpsBaud,
            (unsigned)GPS_SERIAL_BAUD_DEFAULT);
        return;
      }
      uint32_t b = strtoul(n, NULL, 10);
      if (b < 1200 || b > 921600) {
        say("gps: baud out of range (1200..921600)\n");
        return;
      }
      gGpsBaud = b;
      Preferences p;
      p.begin("wpmesh", false);
      p.putUInt("gpsbaud", gGpsBaud);
      p.end();
      if (gGpsNmea) {
        gpsApplyBaud(true);
        gGpsReader.reset();
      }
      say("gps: baud %u%s\n", (unsigned)gGpsBaud,
          gGpsNmea ? " - applied, counters reset" : " - saved (reader is off)");
      return;
    }
    /* `gps raw` — bytes climbing with sentences at 0 has TWO causes and the
     * status line cannot tell them apart. These bytes can: readable ASCII means
     * the baud is right and something else is wrong, `b5 62` means the module is
     * talking UBX binary, and unreadable non-UBX means the rate is still off. */
    if (!strcasecmp(arg, "raw")) {
      uint8_t buf[64];
      int n = gpsRawSnapshot(buf, sizeof(buf));
      if (!n) {
        say("gps: no bytes seen%s\n", gGpsNmea ? "" : " (reader is off)");
        return;
      }
      say("gps: last %d bytes @ %u baud\n", n, (unsigned)gGpsBaud);
      for (int i = 0; i < n; i += 16) {
        char hex[3 * 16 + 1], asc[17];
        int m = (n - i < 16) ? n - i : 16;
        for (int j = 0; j < m; j++) {
          snprintf(hex + 3 * j, 4, "%02x ", buf[i + j]);
          asc[j] = (buf[i + j] >= 0x20 && buf[i + j] < 0x7f) ? (char)buf[i + j] : '.';
        }
        hex[3 * m] = 0;
        asc[m] = 0;
        say("  %-48s |%s|\n", hex, asc);
      }
      return;
    }
    say("gps: reader %s @ %u  bytes=%u sentences=%u badck=%u overrun=%u\n",
        gGpsNmea ? "ON" : "OFF (gps on to start)",
        (unsigned)(gGpsNmea ? gGpsBaud : USER_SERIAL_BAUD),
        gGpsReader.bytes(), gGpsReader.sentences(),
        gGpsReader.badChecksum(), gGpsReader.overruns());
    if (gGpsNmea && gGpsReader.bytes() > 200 && gGpsReader.sentences() == 0) {
      say("gps: bytes flow, no sentences - wrong baud, or a module talking binary.\n");
      say("gps: `gps raw` decides it; `gps baud 9600|38400|57600|115200` retunes.\n");
    }
    int32_t la = 0, lo = 0;
    uint32_t age = 0;
    int sats = -1, hdop = -1;
    if (meshService.getGpsFix(&la, &lo, &age, &sats, &hdop)) {
      say("gps: fix %d.%05d,%d.%05d  age %lus  sats=%d hdop=%d.%d\n",
          (int)(la / 10000000), abs((int)((la % 10000000) / 100)),
          (int)(lo / 10000000), abs((int)((lo % 10000000) / 100)),
          (unsigned long)(age / 1000), sats,
          hdop >= 0 ? hdop / 10 : -1, hdop >= 0 ? hdop % 10 : 0);
    } else {
      say("gps: no fix yet (sats in view: %d)\n", sats);
    }
#else
    say("gps: USER_SERIAL not compiled into this build\n");
#endif
    return;
  }
  /* `sun` — legal light at the reference place, offline. The most-asked
   * question of a hunting day, answered from pure math + the coordinates the
   * mesh already delivered. Times print in LOCAL clock (the tz offset is
   * whatever the phone's clock is configured with). */
  if (!strcasecmp(line, "sun") || !strncasecmp(line, "sun ", 4)) {
    if (!ntpClock.isTimeKnown()) {
      say("sun: clock not set yet (needs one NTP sync)\n");
      return;
    }
    int32_t refLat, refLon;
    char refName[20];
    if (line[3] == ' ') {
      // `sun 47.6062,-122.3321` — explicit coordinates, for the bench.
      double la = 0, lo = 0;
      if (sscanf(line + 4, "%lf,%lf", &la, &lo) != 2) {
        say("sun: usage sun [lat,lon]\n");
        return;
      }
      refLat = (int32_t)(la * 1e7);
      refLon = (int32_t)(lo * 1e7);
      strlcpy(refName, "given", sizeof(refName));
    } else if (!meshService.resolveReference(&refLat, &refLon, refName, sizeof(refName))) {
      say("sun: no place to compute for - hear a waypoint or set a pin first\n");
      return;
    }
    uint32_t utc = ntpClock.getExactUtcTime();
    int tzMin = (int)(((int64_t)ntpClock.getExactUnixTime() - (int64_t)utc) / 60);
    int y, m, d;
    sunUnixToDate(utc, &y, &m, &d);
    SunTimes t;
    if (!sunTimesUtc(y, m, d, refLat * 1e-7, refLon * 1e-7, &t)) {
      say("sun: computation refused (bad coordinates?)\n");
      return;
    }
    say("sun: %04d-%02d-%02d at '%s' (local, UTC%+d:%02d)\n", y, m, d, refName,
        tzMin / 60, abs(tzMin % 60));
    struct Row { const char* label; int16_t utcMin; };
    const Row rows[4] = { { "first light", t.dawnMin }, { "sunrise", t.riseMin },
                          { "sunset", t.setMin }, { "last light", t.duskMin } };
    for (int i = 0; i < 4; i++) {
      if (rows[i].utcMin < 0) {
        say("  %-11s (does not occur today)\n", rows[i].label);
      } else {
        int loc = ((int)rows[i].utcMin + tzMin + 2880) % 1440;
        say("  %-11s %02d:%02d\n", rows[i].label, loc / 60, loc % 60);
      }
    }
    if (t.dawnMin >= 0 && t.duskMin >= 0) {
      /* Countdown on an unwrapped ladder from dawn: now, then dusk after it. */
      int nowU = (int)((utc % 86400u) / 60u);
      int dawn = t.dawnMin;
      int now2 = nowU + (nowU < dawn ? 1440 : 0);
      int dusk = t.duskMin + (t.duskMin < dawn ? 1440 : 0);
      if (now2 < dawn + 1) {
        int dm = dawn - now2;
        say("sun: first light in %dh %02dm\n", dm / 60, dm % 60);
      } else if (now2 < dusk) {
        int dm = dusk - now2;
        say("sun: LEGAL LIGHT NOW - ends in %dh %02dm\n", dm / 60, dm % 60);
      } else {
        say("sun: dark - tomorrow's times shift ~1-2 min\n");
      }
    }
    return;
  }
  /* `unread` — why is the white message icon lit? Counts the truth from the
   * message records themselves, repairs the three derived counters when they
   * disagree (drift keeps the icon lit with nothing to read), and NAMES a
   * thread that still holds unread so there is somewhere to go. */
  /* `unread clear` — mark EVERYTHING read. The one honest lever when unread
   * flags belong to a conversation that no longer exists on the phone (wiped
   * thread, remirrored history): there is no thread to open, so nothing else
   * can ever clear them. The user is asserting "I have seen everything". */
  if (!strcasecmp(line, "unread clear")) {
    extern GUI gui;
    int32_t n = gui.flash.messages.markAllRead();
    if (n < 0) {
      say("unread: message store not loaded\n");
      return;
    }
    gui.flash.messages.clearPreloaded();
    gui.state.unreadMessages = gui.flash.messages.hasUnread();
    say("unread: cleared %d - icon %s\n", (int)n,
        gui.state.unreadMessages ? "STILL LIT (report this)" : "off");
    return;
  }
  if (!strcasecmp(line, "unread")) {
    extern GUI gui;
    char from[64];
    int32_t n = gui.flash.messages.recountUnread(true, from, sizeof(from));
    if (n < 0) {
      say("unread: message store not loaded\n");
      return;
    }
    gui.state.unreadMessages = gui.flash.messages.hasUnread();
    say("unread: %d actually unread (counters repaired where they disagreed)\n", (int)n);
    if (n > 0) {
      say("unread: oldest unread is from %s - open that thread to clear it\n", from);
    } else {
      say("unread: icon %s\n", gui.state.unreadMessages ? "STILL LIT (report this)" : "now off");
    }
    return;
  }
  /* `announce` — one NodeInfo broadcast with want_response. The fast path for key
   * exchange: our packet carries our public key, and every hearer is asked to answer
   * with its own NodeInfo — which carries THEIR key. (Replies are damped by stock
   * firmware; give it a minute, then run `pki` to see what was learned.) */
  if (!strcasecmp(line, "announce")) {
    meshService.announceNodeInfo(true);
    say("announce: sent (watch for MESH ANNOUNCE above; `pki` in a minute to see keys)\n");
    return;
  }
  /* `dm !62b8d2fd hello` — send a direct message from the cable. Exists so PKC can be
   * proven end to end without touching the screen: the ACK the peer sends back (it only
   * ACKs what it DECODED) appears in this log as `MESH DM ACK ... err=0`. */
  if (!strncasecmp(line, "dm ", 3)) {
    const char* p = line + 3;
    while (*p == ' ') p++;
    if (*p == '!') p++;
    char* end = NULL;
    uint32_t node = (uint32_t)strtoul(p, &end, 16);
    if (node == 0 || !end || *end != ' ') {
      say("dm: usage dm <!nodehex> <text>\n");
      return;
    }
    while (*end == ' ') end++;
    if (!*end) {
      say("dm: empty message\n");
      return;
    }
    const MeshNode* peer = meshService.findNode(node);
    bool pki = peer && (peer->pkiFlags & MESH_NODE_HAS_KEY);
    bool ok = meshService.sendDirectMessage(node, end);
    say("dm: %s to !%08x (%s) - watch for 'MESH DM ACK ... err=0' = delivered\n",
        ok ? "sent" : "REFUSED", (unsigned)node,
        pki ? "PKI" : "LEGACY - no key, 2.5+ nodes drop it");
    return;
  }
  /* `send <idx> <text>` — a CHANNEL text from the cable. Born of a gap in the receipt work:
   * `dm` could prove the DM receipt end to end (`MESH DM ACK ... err=0` -> "delivered"), but
   * the BROADCAST receipt had no way to be exercised without typing on the handset, so
   * "in mesh" shipped as the one unproven state. A broadcast is acknowledged implicitly —
   * hearing our own packet rebroadcast is the ack — so watch for `MESH RECEIPT: ... -> in
   * mesh` a second or two after this, which also proves somebody out there relayed it.
   * Index, not hash: `chans` prints the indices and a hash is not something anyone can type. */
  if (!strncasecmp(line, "send ", 5)) {
    const char* p = line + 5;
    while (*p == ' ') {
      p++;
    }
    char* end = NULL;
    long idx = strtol(p, &end, 10);
    if (!end || end == p || *end != ' ') {
      say("send: usage send <chan-index> <text>   (see `chans`)\n");
      return;
    }
    while (*end == ' ') {
      end++;
    }
    if (!*end) {
      say("send: empty message\n");
      return;
    }
    const MeshChannel* c = meshService.getChannel((int)idx);
    if (!c) {
      say("send: no channel at index %ld (see `chans`)\n", idx);
      return;
    }
    bool ok = meshService.sendChannelMessage(c->hash, end);
    say("send: %s on [%ld] '%s' - watch for 'MESH RECEIPT: ... -> in mesh'\n",
        ok ? "sent" : "REFUSED", idx, c->name);
    return;
  }
  /* `star [<!nodehex>]` — bare, it lists what is starred; with a node, it toggles. Exists for
   * the same reason `send` does: the UI path is a key press on a screen, and a feature that
   * can only be exercised by a thumb cannot be proven from the cable. */
  if (!strncasecmp(line, "star", 4) && (line[4] == 0 || line[4] == ' ')) {
    const char* p = line + 4;
    while (*p == ' ') {
      p++;
    }
    if (!*p) {
      int shown = 0;
      for (int i = 0; i < meshService.getNodeCount(); i++) {
        const MeshNode* n = meshService.getNode(i);
        if (n && (n->pkiFlags & MESH_NODE_FAVOURITE)) {
          say("  * !%08x '%s'\n", (unsigned)n->nodeNum, n->name);
          shown++;
        }
      }
      say("star: %d starred (they sort to the top and are evicted last)\n", shown);
      return;
    }
    if (*p == '!') {
      p++;
    }
    uint32_t node = (uint32_t)strtoul(p, NULL, 16);
    if (!node) {
      say("star: usage star            (list)\n      star <!nodehex>  (toggle)\n");
      return;
    }
    bool on = meshService.toggleFavourite(node);
    say("star: !%08x is now %s\n", (unsigned)node, on ? "STARRED" : "not starred");
    return;
  }
  /* `bench` — time the SAME database-shaped workload on SPIFFS and on the SD card, on this
   * exact hardware, before deciding to move the database. The case for moving it rested on
   * "/health.log lives on SD and never stalls" — but that is ~130 bytes a minute, which says
   * nothing about an 8 KB write plus a create and a rename. And cheap SD cards run their own
   * wear levelling, which is a well-known source of unpredictable multi-hundred-ms pauses; SD
   * could easily just move the problem. Measure, do not reason. */
  if (!strcasecmp(line, "bench")) {
    extern GUI gui;
    const size_t N = 8192;
    uint8_t* buf = (uint8_t*)ps_malloc(N);
    if (!buf) {
      say("bench: no PSRAM\n");
      return;
    }
    memset(buf, 0xA5, N);
    for (int pass = 0; pass < 3; pass++) {
      // ---- SPIFFS ----
      uint32_t t0 = millis();
      File f = SPIFFS.open("/bench.tmp", "w");
      uint32_t tOpen = millis();
      if (f) {
        f.write(buf, N);
        uint32_t tWrite = millis();
        f.close();
        SPIFFS.remove("/bench.bin");
        uint32_t tRm = millis();
        SPIFFS.rename("/bench.tmp", "/bench.bin");
        say("bench spiffs[%d]: total=%ums open=%u write=%u rm=%u rename=%u\n", pass,
            (unsigned)(millis() - t0), (unsigned)(tOpen - t0), (unsigned)(tWrite - tOpen),
            (unsigned)(tRm - tWrite), (unsigned)(millis() - tRm));
      } else {
        say("bench spiffs[%d]: open FAILED\n", pass);
      }
      // ---- SD ----
      if (!gui.state.cardPresent) {
        say("bench sd[%d]: no card\n", pass);
        continue;
      }
      t0 = millis();
      File g = SD.open("/bench.tmp", FILE_WRITE);
      uint32_t gOpen = millis();
      if (g) {
        g.write(buf, N);
        uint32_t gWrite = millis();
        g.close();
        SD.remove("/bench.bin");
        uint32_t gRm = millis();
        SD.rename("/bench.tmp", "/bench.bin");
        say("bench sd[%d]:     total=%ums open=%u write=%u rm=%u rename=%u\n", pass,
            (unsigned)(millis() - t0), (unsigned)(gOpen - t0), (unsigned)(gWrite - gOpen),
            (unsigned)(gRm - gWrite), (unsigned)(millis() - gRm));
      } else {
        say("bench sd[%d]: open FAILED\n", pass);
      }
    }
    SPIFFS.remove("/bench.bin");
    if (gui.state.cardPresent) {
      SD.remove("/bench.bin");
    }
    free(buf);
    say("bench: done (8192 bytes per pass, the shape a real save has)\n");
    return;
  }
  /* `wifi drop` — simulate a hotspot blip: disconnect WITHOUT marking the radio user-disabled,
   * so the main loop's retry path engages exactly as it does in the field. Exists because the
   * 5-second freeze Nick feels while scrolling only happens on a reconnect, and a bug that
   * needs someone else's access point to misbehave cannot be measured on demand otherwise. */
  if (!strcasecmp(line, "wifi drop")) {
    WiFi.disconnect();
    say("wifi: dropped (not user-disabled) - the retry path will now run; watch for SLOW WIFI\n");
    return;
  }

  /* `wifi bounce` — radio OFF, then STA back on. THE cure for the deaf-scan state
   * (2026-08-27): after hours of disconnected retry churn the driver can reach a state
   * where every scan COMPLETES with 0 results while the AP is on air at -50 dBm (the twin
   * phone as witness) — screen wake, 240 MHz, quiesced windows, nothing helps except a
   * radio restart. "Open the WiFi screen and rescan brings it back" was this exact cycle
   * hiding in NetworksApp's constructor (disconnect(true,true)); this command is the same
   * medicine WITHOUT eraseap, so the driver's remembered AP survives. The retry loop and
   * auto-switcher take it from here — with the AP in range, expect a rejoin in seconds.
   * autoSwitchTick() now performs this on its own after 2 consecutive empty scans; this
   * command exists so a bench session can apply and PROVE the cure without a reboot. */
  if (!strcasecmp(line, "wifi bounce")) {
    wifiState.bounceRadio();
    say("wifi bounce: radio cycled off/on - retry/auto-switch take it from here\n");
    return;
  }

  /* `wifi scan` — a BLOCKING scan, listing what the radio can actually hear right now.
   *
   * 🔑 EXISTS BECAUSE ONE AFTERNOON WAS SPENT NOT KNOWING THIS. Phone 1 kept losing its
   * association and the auto-switch log showed `scan done: n=0` over and over. There are two
   * completely different faults behind that line — a radio that has gone deaf, or an access
   * point that has stopped advertising — and nothing on the phone could tell them apart. The
   * whole hunt went after the CPU downclock on that ambiguity, and the answer turned out to be
   * the access point (an iPhone hotspot, which stops broadcasting when it is not actively
   * serving anyone). ⚠ On a normal router the same phone is 9 for 9 clean.
   *
   * So: if this prints a list, the radio is fine and the SSID you want is simply not on the
   * air. If it prints 0 while other devices in the room are associated, THEN suspect the
   * phone. ⚠ Blocks for a couple of seconds - it is a bench command, not a loop call. */
  if (!strcasecmp(line, "wifi scan")) {
    /* Guarded like the rest — and this one says so out loud, because a bench command that
     * silently did nothing would be read as "the radio is dead" and send the next hour in
     * the wrong direction entirely. */
    if (!wifiScanMemoryOk("serial")) {
      say("wifi scan: REFUSED - not enough contiguous internal heap to survive the result.\n"
          "           This is the guard, not the radio. Run `heap` and look at `largest`.\n");
      return;
    }
    const int n = WiFi.scanNetworks(false);
    wifiScanNoteResult(n);
    if (n <= 0) {
      say("wifi scan: %d networks - the radio hears NOTHING. If other devices are associated\n"
          "           in this room, that points at the phone; otherwise the AP is not on air.\n", n);
    } else {
      say("wifi scan: %d network(s) - the radio is fine; check whether YOUR ssid is listed\n", n);
      for (int i = 0; i < n && i < 20; i++) {
        say("  %-32s %d dBm ch%d\n", WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i), (int)WiFi.channel(i));
      }
    }
    WiFi.scanDelete();
    return;
  }

  /* `wifi calreset` — erase the RF calibration data in NVS and reboot, forcing the PHY to
   * recalibrate from scratch on the way up. EXISTS BECAUSE OF 2026-08-27 MORNING: phone 1
   * booted DEAF — `wifi scan` 0/0/-2 (-2 = the scan API itself failing) on a FRESH boot
   * with largest=26 K, while phone 2 on the same desk heard NickH-wifi at -60 dBm and was
   * associated. Fresh heap rules out fragmentation; phone 2 rules out the air; a PCB
   * antenna rules out a knocked cable. What survives a reboot and steers the radio is the
   * stored calibration, so this is the probe — and if it cures a deaf radio, the cure is
   * now one serial command instead of a working theory. ⚠ Only the "phy" NVS namespace is
   * touched; WiFi credentials and everything else stay. Reboots when done. */
  if (!strcasecmp(line, "wifi calreset")) {
    const esp_err_t e = esp_phy_erase_cal_data_in_nvs();
    say("wifi calreset: erase %s - rebooting to recalibrate\n",
        e == ESP_OK ? "OK" : "FAILED (will still reboot; cal may simply be absent)");
    delay(300);
    ESP.restart();
    return;
  }

  /* `wifi restore` — esp_wifi_restore(): wipe the WiFi DRIVER's persisted state in NVS
   * (namespace nvs.net80211) and reboot. The step past calreset on the same 2026-08-27
   * deaf radio: with heap, air, firmware and calibration all ruled out, the one thing
   * left that survives reboots is the driver's own stored config. ⚠ Only the driver
   * namespace is touched — the app's Preferences (mesh keypair, book positions) and the
   * phone's own saved-network profiles are elsewhere and re-write driver config on the
   * next connect. */
  if (!strcasecmp(line, "wifi restore")) {
    const esp_err_t e = esp_wifi_restore();
    say("wifi restore: %s - rebooting\n", e == ESP_OK ? "OK" : "FAILED");
    delay(300);
    ESP.restart();
    return;
  }
  if (!strcasecmp(line, "chans")) {
    for (int i = 0; i < meshService.getChannelCount(); i++) {
      const MeshChannel* c = meshService.getChannel(i);
      if (c) {
        say("  [%d] '%s' keyLen=%d\n", i, c->name, (int)c->keyLen);
      }
    }
    return;
  }

  /* `meshdb` — where the chat history lives, what loaded, and what the next save will write.
   *
   * 🔑 EXISTS BECAUSE THE FAULT IT DIAGNOSES WAS INVISIBLE FROM EVERY ANGLE. The phone read
   * SPIFFS at boot and wrote the SD card from the first loop pass onward, so the chat list
   * came up empty while a perfectly good database sat on the card — and nothing on screen,
   * on the console or in the log said which file anything had touched. Two file sizes side by
   * side is the whole diagnosis, and neither number was obtainable before this.
   *
   * ⚠ The sizes are read live from BOTH filesystems every time, never cached: which one is
   * in use is the exact thing in question. */
  if (!strcasecmp(line, "meshdb")) {
    const bool card = meshService.dbOnCard();
    say("meshdb: in use %s   loaded from %s at boot\n",
        card ? "SD" : "SPIFFS", meshService.dbLoadedFrom());
    say("  in RAM: %d nodes, %d msgs, %d places\n",
        meshService.getNodeCount(), meshService.getMessageCount(),
        meshService.getWaypointCount());
    const int keep = meshService.persistedMessageCount();
    say("  next save writes %d of %d msgs (caps: %d per chat, %d total on %s)\n",
        keep, meshService.getMessageCount(),
        card ? MESH_PERSIST_PER_CHAT_SD : MESH_PERSIST_PER_CHAT_FLASH,
        card ? MESH_PERSIST_TOTAL_SD : MESH_PERSIST_TOTAL_FLASH,
        card ? "SD" : "SPIFFS");
    for (int i = 0; i < 2; i++) {
      fs::FS& f = i ? (fs::FS&)SPIFFS : (fs::FS&)SD;
      const char* nm = i ? "SPIFFS" : "SD    ";
      File h = f.open("/meshdb.bin", "r");
      if (h) {
        say("  %s /meshdb.bin  %u bytes%s\n", nm, (unsigned)h.size(),
            (!i && card) || (i && !card) ? "   <- the one in use" : "");
        h.close();
      } else {
        say("  %s /meshdb.bin  absent\n", nm);
      }
    }
    return;
  }

  /* `lock` — why the screen does or does not lock, in one paste.
   *
   * 🔑 THE LOCK IS NOT ITS OWN TIMER. It happens INSIDE the SCREEN_SLEEP_EVENT branch
   * (GUI.cpp, processEvent), so it needs BOTH `locking` and a screen that actually sleeps —
   * `doSleeping()` is `sleeping && 0 < sleepAfterMs <= 86400000`. A phone that dims but never
   * sleeps therefore never locks either, and the dimming makes it look like the timeout is
   * working. That is two different reasons for one symptom, so this prints both, plus what is
   * on the CARD as against what is in RAM — a setting that saved and a setting that loaded are
   * not the same claim.
   *
   * ⚠ `lock_keyboard` DEFAULTS TO 0 WHEN THE [lock] SECTION EXISTS but the key does not, while
   * a missing SECTION defaults to 1 (WiPhone.ino). The same disagreement in [screen] shipped a
   * phone that never dimmed and never slept — see the 2026-08-22 battery audit. */
  if (!strcasecmp(line, "lock")) {
    extern GUI gui;
    ControlState& st = gui.state;
    say("lock: locking=%s  locked=%s  (locking is the SETTING, locked is right now)\n",
        st.locking ? "ON" : "OFF", st.locked ? "yes" : "no");
    say("  sleep: sleeping=%s after=%lus -> doSleeping()=%s\n",
        st.sleeping ? "ON" : "OFF", (unsigned long)(st.sleepAfterMs / 1000),
        st.doSleeping() ? "YES" : "NO  <-- nothing can lock without this");
    say("  dim:   dimming=%s after=%lus level=%d  (dimming alone never locks)\n",
        st.dimming ? "ON" : "OFF", (unsigned long)(st.dimAfterMs / 1000), (int)st.dimLevel);
    /* ⚠ THE SIP STATE IS PART OF THE LOCK, and that is not obvious from anywhere else.
     * While inCall() is true the unlock path clears `locked` on ANY key — so a phone parked
     * in a teardown state has no working lock. See GUI::inCall(). */
    say("  sip:   state=%d  inCall()=%s%s\n", (int)st.sipState,
        gui.inCall() ? "YES" : "no",
        gui.inCall() ? "  <-- ANY key unlocks while this is YES" : "");

    CriticalFile ini(Storage::ConfigsFile);
    if (!ini.load() && !ini.restore()) {
      say("  card:  %s WILL NOT LOAD - the phone is running on built-in defaults\n",
          Storage::ConfigsFile);
      return;
    }
    if (!ini.hasSection("lock")) {
      say("  card:  [lock] section ABSENT -> boot defaults locking ON\n");
    } else {
      const int32_t v = ini["lock"].getIntValueSafe("lock_keyboard", -1);
      say("  card:  [lock] lock_keyboard = %s%s\n",
          v < 0 ? "(key MISSING)" : (v ? "1" : "0"),
          v < 0 ? "  <-- section present but key absent: boot reads this as OFF" : "");
    }
    if (!ini.hasSection("screen")) {
      say("  card:  [screen] section ABSENT -> boot defaults dim+sleep ON\n");
    } else {
      say("  card:  [screen] sleeping=%d sleep_after_s=%d dimming=%d dim_after_s=%d\n",
          (int)ini["screen"].getIntValueSafe("sleeping", -1),
          (int)ini["screen"].getIntValueSafe("sleep_after_s", -1),
          (int)ini["screen"].getIntValueSafe("dimming", -1),
          (int)ini["screen"].getIntValueSafe("dim_after_s", -1));
    }
    return;
  }

  /* `ver` — which binary is actually on this phone. Two builds wore 0.9.14 during the
   * 2026-08-25 session before this existed; asking the phone is cheaper than remembering. */
  if (!strcasecmp(line, "ver") || !strcasecmp(line, "version")) {
    say("firmware %s, built %s %s\n", FIRMWARE_VERSION, __DATE__, __TIME__);
    return;
  }

  /* `scrim` — the translucent plate under menu text, tunable against a real photo.
   * ⚠ RAM only. The shipped value is THEME_SCRIM_* in GUI.h; a reboot restores it. */
  if (!strncasecmp(line, "scrim", 5) && (line[5] == '\0' || line[5] == ' ')) {
    extern GUI gui;
    const char* a = line + 5;
    while (*a == ' ') {
      a++;
    }
    if (*a) {
      char* end = NULL;
      const unsigned long alpha = strtoul(a, &end, 10);
      if (end == a || alpha > 255) {
        say("scrim: alpha must be 0-255 (0 = off). `scrim 190`, `scrim 190 39C7`\n");
        return;
      }
      gScrimAlpha = (uint8_t)alpha;
      while (*end == ' ') {
        end++;
      }
      if (*end) {
        gScrimColor = (uint16_t)strtoul(end, NULL, 16);
      }
      gui.redrawScreen(true, true, true, true);
    }
    say("scrim: alpha=%u color=0x%04X (shipped default %u / 0x%04X; RAM only, a reboot resets it)\n",
        (unsigned)gScrimAlpha, (unsigned)gScrimColor,
        (unsigned)THEME_SCRIM_ALPHA, (unsigned)THEME_SCRIM_COLOR);
    return;
  }

  /* `key` — press keys on a phone nobody is holding.
   *
   * 🔑 THE COMPANION TO `shot`, AND THE OTHER HALF OF THE SAME GAP. A cable could already SEE
   * the screen; it could not change what was on it, so every claim about a screen you have to
   * navigate to was still unverifiable. Photos shipped to a user untested for exactly that
   * reason, and the menu-contrast complaint that followed lives three key presses from the
   * clock face.
   *
   * ⚠ It injects into keypadBuff — the real keypad buffer — so the wake, the drain loop, the
   * easter-egg tracker and the app's own processEvent all run as they do for a thumb. It is
   * not a simulation of a press; it IS one, from a different source. */
  if (!strncasecmp(line, "key", 3) && (line[3] == '\0' || line[3] == ' ')) {
    static const struct {
      const char* name;
      char code;
    } NAMES[] = {
      { "select", WIPHONE_KEY_SELECT }, { "back",  WIPHONE_KEY_BACK  },
      { "ok",     WIPHONE_KEY_OK     }, { "up",    WIPHONE_KEY_UP    },
      { "down",   WIPHONE_KEY_DOWN   }, { "left",  WIPHONE_KEY_LEFT  },
      { "right",  WIPHONE_KEY_RIGHT  }, { "call",  WIPHONE_KEY_CALL  },
      { "end",    WIPHONE_KEY_END    }, { "f1",    WIPHONE_KEY_F1    },
      { "f2",     WIPHONE_KEY_F2     }, { "f3",    WIPHONE_KEY_F3    },
      { "f4",     WIPHONE_KEY_F4     },
      /* The two softkeys under the screen, by what they DO rather than by their wiring —
       * "menu" is the word printed on the screen above the left one, and looking up which
       * hardware key that is should not be the reader's problem. */
      { "menu",   WIPHONE_KEY_SELECT }, { "sel",   WIPHONE_KEY_SELECT },
    };
    const char* p = line + 3;
    int sent = 0, refused = 0;
    char what[96] = "";
    while (*p) {
      while (*p == ' ') {
        p++;
      }
      if (!*p) {
        break;
      }
      char tok[16];
      size_t n = 0;
      while (*p && *p != ' ' && n < sizeof(tok) - 1) {
        tok[n++] = *p++;
      }
      tok[n] = '\0';
      char code = 0;
      for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (!strcasecmp(tok, NAMES[i].name)) {
          code = NAMES[i].code;
          break;
        }
      }
      /* A single character is itself — digits, * and # are what the keypad actually sends. */
      if (!code && n == 1) {
        code = tok[0];
      }
      if (!code) {
        say("key: don't know '%s' (names: select/menu back ok up down left right call end f1-f4, or one character)\n", tok);
        return;
      }
      if (uiInjectKey(code)) {
        sent++;
        snprintf(what + strlen(what), sizeof(what) - strlen(what), "%s%s", sent > 1 ? " " : "", tok);
      } else {
        refused++;
      }
    }
    if (!sent && !refused) {
      say("key: nothing to press. `key menu`, `key down down ok`, `key 5`\n");
      return;
    }
    /* ⚠ The presses are QUEUED, not applied — loop() drains keypadBuff on its next pass, and
     * a screenshot taken in the same breath would catch the OLD screen. Anything reading this
     * must let a pass happen before it looks. tools/shot.py --cmd does. */
    say("key: queued %d (%s)%s - drains on the next loop pass\n",
        sent, what, refused ? ", SOME DROPPED (buffer full)" : "");
    return;
  }

  /* `shot` — the live frame, base64, for tools/shot.py. See GUI::screenshotToSerial(). */
  if (!strcasecmp(line, "shot") || !strcasecmp(line, "screenshot")) {
    extern GUI gui;
    gui.screenshotToSerial();
    return;
  }

  /* `ls [dir]` — EVERY entry in a card folder, hidden ones included, with sizes. It exists
   * because the on-phone listers disagreed about dotfiles: the Files app hides any name that
   * starts with '.', while the Game Boy / Books / Music pickers listed everything with the
   * right extension — so a macOS AppleDouble sidecar ('._Game.gbc', written by Finder next to
   * every file it copies onto a FAT card) showed up as a duplicate ROM that the Files app
   * could not find (Nick, phone 2, 2026-09-03). The picker draws straight to the LCD, so
   * `shot` cannot show it either. This is the one view of the card as it actually is: hidden
   * entries are marked '*', folders end in '/'. */
  if (!strncasecmp(line, "ls", 2) && (line[2] == '\0' || line[2] == ' ')) {
    const char* arg = line + 2;
    while (*arg == ' ') {
      arg++;
    }
    const char* path = *arg ? arg : "/";
    File dir = SD.open(path);
    if (!dir) {
      say("ls: %s: cannot open (no card, or no such folder)\n", path);
      return;
    }
    if (!dir.isDirectory()) {
      say("ls: %s is a file, %u bytes\n", path, (unsigned)dir.size());
      dir.close();
      return;
    }
    File f;
    int n = 0, hidden = 0;
    while ((f = dir.openNextFile())) {
      /* ⚠ On this core File::name() returns the FULL path — basename it (same as app_files). */
      const char* nm = f.name();
      const char* base = strrchr(nm, '/');
      base = base ? base + 1 : nm;
      const bool hid = (base[0] == '.');
      if (hid) {
        hidden++;
      }
      if (f.isDirectory()) {
        say("  %s %s/\n", hid ? "*" : " ", base);
      } else {
        say("  %s %-40s %u\n", hid ? "*" : " ", base, (unsigned)f.size());
      }
      n++;
      f.close();
    }
    dir.close();
    say("ls: %d entries in %s, %d hidden (*)\n", n, path, hidden);
    return;
  }

  /* `rm </path/file>` — delete ONE file by full path; the pair to `ls`, for the entries the
   * Files app cannot show. Refuses folders and relative names so a typo cannot take out a
   * directory. Same SD.remove() the Files app's Delete and the picker's Back-del run. */
  if (!strncasecmp(line, "rm ", 3)) {
    const char* path = line + 3;
    while (*path == ' ') {
      path++;
    }
    if (path[0] != '/') {
      say("rm: give a full path starting with / (see `ls`)\n");
      return;
    }
    File f = SD.open(path);
    if (!f) {
      say("rm: %s: not found\n", path);
      return;
    }
    const bool isDir = f.isDirectory();
    f.close();
    if (isDir) {
      say("rm: %s is a folder - refused\n", path);
      return;
    }
    say("rm: %s: %s\n", path, SD.remove(path) ? "deleted" : "FAILED");
    return;
  }

  /* `power` — THE USB-POWER-METER INSTRUMENT (2026-09-03).
   *
   * Phone 1's full-to-empty run (9h07m on battery, WiFi off, screen off, 80 MHz) implies
   * ~80-100 mA of average draw, and a datasheet-typical sum of everything the firmware leaves
   * powered (ESP32 at 80 MHz never light-sleeping ~20-31, SX1276 RX-continuous ~11, ST7789
   * awake behind a dark backlight 1-8?, I2S+APLL running from boot 1-3?) reaches only
   * ~35-58 mA. The cell is soldered, so battery current cannot be metered, and a drain run
   * resolves 10 mA only after hours at matched voltage (10 mV log quantisation). Nick's USB
   * power meter resolves 1 mA in seconds — IF the phone is FULL (charger tapered: chg=1 and
   * v>=4.19 for 30+ min, so the meter reads the board and not the charge). Absolute readings
   * include the charger IC and the CP2104; only DELTAS mean anything. Each subcommand flips
   * exactly ONE thing so one reading answers one question:
   *   power lcd sleep     the panel controller's own draw (backlight is already off)
   *   power i2s stop      the I2S module + DMA (the APLL stays; see Audio.cpp:111-121)
   *   power lora sleep    the radio's RX-continuous draw (~11 mA typical: also calibrates the method)
   *   power sleep 20      everything EXCEPT the ESP32 core — the light-sleep floor, i.e. the
   *                       most the CPU lever could ever give (WiFi must be off: light sleep
   *                       with the station up drops the association)
   * Bench only — none of this is a power policy; the numbers it produces decide what becomes one. */
  if (!strncasecmp(line, "power", 5) && (line[5] == '\0' || line[5] == ' ')) {
    extern GUI gui;
    extern Audio* audio;
    extern volatile bool gGbcActive;
    static bool s_lcdAsleep = false;
    static bool s_i2sStopped = false;
    const char* arg = line + 5;
    while (*arg == ' ') {
      arg++;
    }
    const bool audioOn = audio && audio->isOn();
    if (!*arg) {
      say("power: cpu=%luMHz screen=%d wifi=%s lora=%s lcd=%s i2s=%s audio=%s gbc=%d\n",
          (unsigned long)getCpuFrequencyMhz(), (int)gui.state.screenBrightness,
          wifiState.radioOff() ? "OFF" : "on",
          !meshPhy.isReady() ? "absent" : (meshPhy.benchAsleep() ? "SLEEP(bench)" : "rx"),
          s_lcdAsleep ? "SLPIN(bench)" : "normal", s_i2sStopped ? "stopped(bench)" : "running",
          audioOn ? "ON" : "off", (int)gGbcActive);
      say("  method: cell FULL (chg=1, v>=4.19 for 30+ min), screen asleep, WiFi off. Read the\n");
      say("  meter; flip ONE switch; read again. Only deltas mean anything (charger+CP2104 ride along).\n");
      return;
    }
    if (!strcasecmp(arg, "lcd sleep") || !strcasecmp(arg, "lcd wake")) {
      const bool sleep = !strcasecmp(arg, "lcd sleep");
      if (!static_lcd || gGbcActive) {
        say("power lcd: the emulator owns the panel (or no LCD) - refused\n");
        return;
      }
      if (sleep) {
        static_lcd->writecommand(0x28);          // DISPOFF
        static_lcd->writecommand(0x10);          // SLPIN: oscillator, charge pumps, scan stop
      } else {
        static_lcd->writecommand(0x11);          // SLPOUT: the panel needs >=120 ms to settle
        delay(120);
        static_lcd->writecommand(0x29);          // DISPON
      }
      s_lcdAsleep = sleep;
      say("power lcd: %s (frame memory kept; the GUI repaints on the next screen wake)\n",
          sleep ? "DISPOFF+SLPIN" : "SLPOUT+DISPON");
      return;
    }
    if (!strcasecmp(arg, "i2s stop") || !strcasecmp(arg, "i2s start")) {
      const bool stop = !strcasecmp(arg, "i2s stop");
      if (stop && audioOn) {
        say("power i2s: audio is ON - stop it first\n");
        return;
      }
      const esp_err_t r = stop ? i2s_stop(I2S_NUM_0) : i2s_start(I2S_NUM_0);
      if (r == ESP_OK) {
        s_i2sStopped = stop;
      }
      say("power i2s: %s %s (Audio::start() calls i2s_start itself, so audio still works after a stop)\n",
          stop ? "stop" : "start", r == ESP_OK ? "ok" : "FAILED");
      return;
    }
    if (!strcasecmp(arg, "lora sleep") || !strcasecmp(arg, "lora rx")) {
      if (!meshPhy.isReady()) {
        say("power lora: no radio (not detected at boot)\n");
        return;
      }
      const bool sleep = !strcasecmp(arg, "lora sleep");
      meshPhy.benchSleep(sleep);
      say("power lora: %s%s\n", sleep ? "SLEEP - the mesh is DEAF until `power lora rx`" : "RX-continuous",
          sleep ? " (a send or a radio re-init also ends it)" : "");
      return;
    }
    if (!strncasecmp(arg, "sleep", 5)) {
      const int secs = atoi(arg + 5);
      if (secs < 1 || secs > 600) {
        say("power sleep <1..600 seconds>\n");
        return;
      }
      if (!wifiState.radioOff()) {
        say("power sleep: WiFi is ON - switch it off first (menu > WiFi: off); light sleep with the\n");
        say("  station up drops the association and the reading would include the reconnect\n");
        return;
      }
      if (audioOn || gGbcActive) {
        say("power sleep: audio or the emulator is running - refused\n");
        return;
      }
      say("power sleep: %d s. Keypad (GPIO%d low) wakes early; serial typed meanwhile is lost.\n",
          secs, KEYBOARD_INTERRUPT_PIN);
      uart_wait_tx_done(PORT, pdMS_TO_TICKS(200));           // let that line leave before the clocks stop
      /* Flash and PSRAM stay powered through light sleep on this IDF (3.3): sleep_modes.c only
       * powers VDD_SDIO down under `#ifndef CONFIG_SPIRAM_SUPPORT`, and this build has PSRAM
       * on, so the heap (which lives in PSRAM) survives. There is no ESP_PD_DOMAIN_VDDSDIO to
       * pin here (that arrived in IDF 4). Load-bearing on an SDK bump: light sleep must keep
       * VDD_SDIO ON or the phone wakes into garbage. */
      esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
      esp_sleep_enable_ext0_wakeup((gpio_num_t)KEYBOARD_INTERRUPT_PIN, 0);   // SN7326 INT idles high
      const uint32_t t0 = millis();
      const esp_err_t r = esp_light_sleep_start();
      const uint32_t dt = millis() - t0;
      const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
      say("power sleep: back after %lu ms (%s), woke by %s\n", (unsigned long)dt,
          r == ESP_OK ? "ok" : "esp_light_sleep_start FAILED",
          cause == ESP_SLEEP_WAKEUP_TIMER ? "timer" : cause == ESP_SLEEP_WAKEUP_EXT0 ? "keypad" : "something else");
      return;
    }
    say("power: unknown switch - `?` lists them\n");
    return;
  }

  /* `wallpaper` — the background loader, out loud, and the one way to drive Photos'
   * wallpaper path without a thumb. It exists because the failure it reports was INVISIBLE:
   * a rejected /background.jpg fell into the same fallback as "no wallpaper chosen", so the
   * screen and the log looked identical either way and "Set as wallpaper does nothing" had
   * no thread to pull (Nick, 2026-08-25). `set` runs the SAME function the menu runs — see
   * photosSetWallpaper() — so this cannot drift away from the feature it tests. */
  if (!strncasecmp(line, "wallpaper", 9) && (line[9] == '\0' || line[9] == ' ')) {
    extern GUI gui;
    const char* arg = line + 9;
    while (*arg == ' ') {
      arg++;
    }
    if (!strcasecmp(arg, "reload")) {
      gui.loadWallpaper();
    } else if (!strcasecmp(arg, "clear")) {
      /* The same operation as "[ Restore default wallpaper ]": delete the override and let
       * the compiled-in fallback surface. Nothing is copied in, so nothing can fail. */
      if (SD.exists(GUI::backgroundFile)) {
        say("wallpaper: removing %s from SD\n", GUI::backgroundFile);
        SD.remove(GUI::backgroundFile);
      } else {
        say("wallpaper: no override on SD to remove\n");
      }
      gui.loadWallpaper();
    } else if (!strcasecmp(arg, "list")) {
      File dir = SD.open("/photos");
      if (!dir || !dir.isDirectory()) {
        say("wallpaper: /photos is missing on the card\n");
      } else {
        File f;
        int n = 0;
        while ((f = dir.openNextFile())) {
          const char* nm = f.name();
          const char* base = strrchr(nm, '/');
          base = base ? base + 1 : nm;
          if (base[0] && base[0] != '.' && !f.isDirectory()) {
            say("  %-40s %u bytes\n", base, (unsigned)f.size());
            n++;
          }
          f.close();
        }
        dir.close();
        say("wallpaper: %d file(s) in /photos\n", n);
      }
      return;
    } else if (!strncasecmp(arg, "set ", 4)) {
      const char* name = arg + 4;
      while (*name == ' ') {
        name++;
      }
      char why[112] = "";
      const bool ok = photosSetWallpaper(name, why, sizeof(why));
      say("wallpaper set %s: %s - %s\n", name, ok ? "OK" : "REFUSED", why);
    } else if (*arg) {
      say("wallpaper: try `wallpaper`, `reload`, `list`, `set <name>`, `clear`\n");
      return;
    }

    /* ⚠ LOADING IT IS NOT SHOWING IT, and a screenshot taken without this line will quietly
     * show the PREVIOUS wallpaper and look like the load failed. ClockApp clones bgImage on
     * every redraw (GUI.cpp, ClockApp::redrawScreen) — but with no NTP the clock face has no
     * reason to redraw, so nothing repaints until something else happens. The Photos app does
     * not need this: it returns REDRAW_ALL. The console has to ask. */
    if (strcasecmp(arg, "list")) {
      gui.redrawScreen(true, true, true, true);
    }

    const bool onSd = SD.exists(GUI::backgroundFile);
    const bool onSpiffs = SPIFFS.exists(GUI::backgroundFile);
    say("wallpaper: %s\n", gui.getWallpaperNote());
    /* Name BOTH filesystems every time. SD is the one Photos writes and the one that was
     * unreadable at load time; SPIFFS is the one that always worked and therefore the one
     * that masked the fault. Printing only the winner hides exactly that asymmetry. */
    if (onSd) {
      File f = SD.open(GUI::backgroundFile, FILE_READ);
      say("  SD     %s  %u bytes\n", GUI::backgroundFile, f ? (unsigned)f.size() : 0u);
      if (f) {
        f.close();
      }
    } else {
      say("  SD     %s  absent\n", GUI::backgroundFile);
    }
    if (onSpiffs) {
      File f = SPIFFS.open(GUI::backgroundFile, FILE_READ);
      say("  SPIFFS %s  %u bytes  (SD wins when both exist)\n",
          GUI::backgroundFile, f ? (unsigned)f.size() : 0u);
      if (f) {
        f.close();
      }
    } else {
      say("  SPIFFS %s  absent\n", GUI::backgroundFile);
    }
    say("  limit  %u KB, baseline colour JPEG only (no progressive, no greyscale)\n",
        (unsigned)(GUI::backgroundFileMaxSize >> 10));
    return;
  }

  // %.40s: a pasted invite URL is ~370 chars and say()'s buffer is 192, so the
  // echo ate the "(try ?)" hint — the only useful half of this line.
  say("? unknown command: '%.40s'  (try ?)\n", line);
}

void serialCmdLoop() {
  uint8_t c;
  // 0 ticks = poll, never block. This runs every main-loop pass.
  while (uart_read_bytes(PORT, &c, 1, 0) == 1) {
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      s_buf[s_len] = '\0';
      if (s_len) {
        run(s_buf);
      }
      s_len = 0;
      continue;
    }
    if (s_len < sizeof(s_buf) - 1) {
      s_buf[s_len++] = (char)c;
    }
    // Over-long input is truncated rather than run in pieces: half a command is not a
    // command, and this is a debug console, not a protocol.
  }
}
