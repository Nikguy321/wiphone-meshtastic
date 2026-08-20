#include "serial_cmd.h"
#include "app_gbc_xfer.h"
#include "sms_mirror_poll.h"
#include "app_books.h"       // booksDebugDumpPage, the `bookpage` command
#include "meshtastic_service.h"   // applyChannelUrl, the `chan` command
#include "mesh_pos.h"             // distance/bearing for the `pos` command
#include "sun_times.h"            // legal light, the `sun` command
#include "clock.h"                // ntpClock
#include "GUI.h"                  // gui.state, the `sip` command
#include <WiFi.h>

#include <Arduino.h>
#include <driver/uart.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>     // strtoul, the `dm` command's node number
#include <string.h>
#include <strings.h>

#ifdef USER_SERIAL      // (Hardware.h, via GUI.h) — the `gps` command's plumbing
#include "nmea.h"
#include <Preferences.h>
extern bool       gGpsNmea;      // WiPhone.ino: routes the user UART to the NMEA reader
extern NmeaReader gGpsReader;
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
    "  up off     stop the uploader",
    "  up         where to point a browser",
    "  sync       poll COVEY for mirrored texts now",
    "  mirror     mirror poller state",
    "  sip        SIP account state: loaded, registered, WiFi - one line",
    "  bookpage   dump the open reader page's layout + rendering",
    "  chan <url> apply a Meshtastic channel invite URL",
    "  chans      list the channels this phone has",
    "  pki        DM crypto state: our key, who has keys, stack headroom",
    "  announce   broadcast NodeInfo now, asking others to answer with theirs",
    "  dm <!node> <text>  send a direct message (PKI when the key is known)",
    "  pos        positions: waypoints, node fixes, our pin, the reference",
    "  gps        woods-plate GPS state: fix, sats, reader counters",
    "  gps on|off route the user UART (38/32) to the NMEA reader (persists)",
    "  sun        legal light at the reference place: dawn/sunrise/sunset/dusk",
    "  unread     recount unread texts, repair the counter, name the threads",
    "  unread clear  mark EVERYTHING read (orphaned threads included)",
    "",
  };
  for (size_t i = 0; i < sizeof(LINES) / sizeof(LINES[0]); i++) {
    say("%s\n", LINES[i]);
  }
}

/* `pos` — the whole positions picture in one paste: every waypoint heard, every
 * node with a fix (age in minutes), our own pin, and which reference distances
 * are measured from. The phone has no GPS — everything here was HEARD. */
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

static void reportUploader() {
  if (!gbcXferOn()) {
    say("uploader: off\n");
    return;
  }
  say("uploader: ON  http://%s/  (or http://wiphone.local/)%s\n",
      xferAddr(), xferUsingAP() ? "  [own hotspot]" : "");
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
  if (!strcasecmp(line, "up on")) {
    if (gbcXferOn()) {
      say("uploader already on\n");
    } else {
      gbcXferStart();
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
  if (!strcasecmp(line, "pos")) {
    reportPos();
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
      say("gps: NMEA reader %s (user UART rx=%d tx=%d @ %d)%s\n",
          gGpsNmea ? "ON" : "OFF", USER_SERIAL_RX, USER_SERIAL_TX,
          USER_SERIAL_BAUD, gGpsNmea ? "" : " - user-serial GUI path restored");
      return;
    }
    say("gps: reader %s  bytes=%u sentences=%u badck=%u overrun=%u\n",
        gGpsNmea ? "ON" : "OFF (gps on to start)",
        gGpsReader.bytes(), gGpsReader.sentences(),
        gGpsReader.badChecksum(), gGpsReader.overruns());
    if (gGpsNmea && gGpsReader.bytes() > 200 && gGpsReader.sentences() == 0) {
      say("gps: bytes flow but no sentences - almost certainly the WRONG BAUD\n");
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
  if (!strcasecmp(line, "chans")) {
    for (int i = 0; i < meshService.getChannelCount(); i++) {
      const MeshChannel* c = meshService.getChannel(i);
      if (c) {
        say("  [%d] '%s' keyLen=%d\n", i, c->name, (int)c->keyLen);
      }
    }
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
