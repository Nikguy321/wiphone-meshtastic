#include "sms_mirror_poll.h"
#include "sms_mirror.h"
#include "sms_mirror_rx.h"
#include "config.h"
#include "helpers.h"
#include "Networks.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <SD.h>

#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------- tuning
static const uint32_t POLL_INTERVAL_MS   = 120000;   // 2 min: LoRa carries anything urgent
static const uint32_t RETRY_INTERVAL_MS  = 300000;   // 5 min after a failure, so a COVEY that
                                                     // is simply off does not cost a
                                                     // connect() attempt every two minutes
static const uint32_t CONNECT_TIMEOUT_MS = 600;      // the ONLY call here that can block
static const uint32_t READ_TIMEOUT_MS    = 8000;     // whole-response budget once connected
static const uint32_t HOST_CACHE_MS      = 600000;   // 10 min; re-resolved sooner on failure
static const int      READ_BUDGET        = 512;      // bytes consumed per main-loop pass

/* Both, and `/roms` is the one that matters in practice: the WiFi uploader writes THERE, not
 * to the card's root (app_gbc_xfer.cpp), so `/roms/smsmirror.txt` is the only one of these
 * that can be put on the phone without pulling the SD card out. The root path is kept first
 * because it is where someone editing the card on a computer would naturally put it. */
static const char* CONFIG_FILES[] = { "/smsmirror.txt", "/roms/smsmirror.txt" };
static const int   CONFIG_FILE_N  = 2;

// ---------------------------------------------------------------- state
enum PollState {
  PS_IDLE = 0,
  PS_CONNECT,
  PS_HEADERS,
  PS_BODY,
};

static bool        s_loaded    = false;      // config read and usable
static uint32_t    s_triedLoad = 0;          // millis() of the last attempt; 0 = never
static char        s_host[64]  = {0};
static char        s_token[96] = {0};
static uint16_t    s_port      = 8087;

static PollState   s_state     = PS_IDLE;
static WiFiClient  s_client;
static uint32_t    s_nextPollMs   = 0;
static uint32_t    s_deadlineMs   = 0;
static uint32_t    s_hostResolvedAt = 0;
static IPAddress   s_hostIp((uint32_t)0);
static int64_t     s_sinceId   = 0;          // high-water VoIP.ms id
static char        s_line[SMS_MIRROR_LINE_MAX + 8];
static uint16_t    s_lineLen   = 0;
static bool        s_lineOverflow = false;
static const char* s_status    = "idle";
static bool        s_forceNow  = false;

// ---------------------------------------------------------------- config
static void trimEol(char* s) {
  size_t n = strlen(s);
  while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
    s[--n] = '\0';
  }
}

static bool loadConfig() {
  if (s_loaded) {
    return true;
  }
  /* ⚠ RE-CHECKED PERIODICALLY, not once at boot, and that matters more than it looks.
   * The intended way to install this file is the WiFi uploader — i.e. WHILE THE PHONE IS
   * RUNNING. Giving up permanently on the first miss would mean uploading the config and
   * watching nothing happen, with no message anywhere, until someone thought to reboot.
   * That is the exact silent-failure shape this feature already has too much of.
   * Re-reading a small file every 30 s costs nothing next to that. */
  const uint32_t now = millis() | 1u;
  if (s_triedLoad && (uint32_t)(now - s_triedLoad) < 30000u) {
    return false;
  }
  const bool first = (s_triedLoad == 0);
  s_triedLoad = now;

  File f;
  for (int i = 0; i < CONFIG_FILE_N && !f; i++) {
    f = SD.open(CONFIG_FILES[i], FILE_READ);
  }
  if (!f) {
    s_status = "off (no smsmirror.txt)";
    if (first) {
      // Once, at boot. Says the feature exists and is waiting, rather than saying nothing.
      log_e("SMSMIRROR poll: no smsmirror.txt on SD (/ or /roms) - LAN sync off");
    }
    return false;
  }
  char buf[3][128] = {{0}, {0}, {0}};
  for (int i = 0; i < 3 && f.available(); i++) {
    size_t n = f.readBytesUntil('\n', buf[i], sizeof(buf[i]) - 1);
    buf[i][n] = '\0';
    trimEol(buf[i]);
  }
  f.close();

  snprintf(s_host, sizeof(s_host), "%s", buf[0]);
  snprintf(s_token, sizeof(s_token), "%s", buf[1]);
  if (buf[2][0]) {
    int p = atoi(buf[2]);
    if (p > 0 && p < 65536) {
      s_port = (uint16_t)p;
    }
  }
  if (!s_host[0] || !s_token[0]) {
    s_status = "off (host or token missing)";
    return false;
  }
  s_loaded = true;
  s_status = "idle";
  /* ⚠ The token is never logged, here or anywhere. It is the only thing standing between the
   * LAN and a readable copy of every text on the account. */
  log_e("SMSMIRROR poll configured: %s:%u", s_host, (unsigned)s_port);
  return true;
}

bool smsMirrorPollConfigured() {
  return loadConfig();
}

void smsMirrorPollRequestNow() {
  s_forceNow = true;
}

const char* smsMirrorPollStatus() {
  return s_status ? s_status : "idle";
}

// ---------------------------------------------------------------- helpers
static void finish(const char* why, bool ok) {
  s_client.stop();
  s_state = PS_IDLE;
  s_status = why;
  s_nextPollMs = millis() + (ok ? POLL_INTERVAL_MS : RETRY_INTERVAL_MS);
  if (!ok) {
    /* Re-resolve next time. A connection that failed is the best evidence available that a
     * cached address has gone stale — COVEY moves between SmithWifi and NickH-wifi and comes
     * back on a different subnet. */
    s_hostResolvedAt = 0;
  }
}

/* Resolve, with our own cache on top of resolveDomain()'s.
 *
 * ⚠ resolveDomain() deliberately does NOT cache `.local` answers, so calling it every poll
 * with `covey.local` would be a 500 ms BLOCKING mDNS query every two minutes — small, but
 * exactly the class of thing that added up to the 5-second freezes. Caching here costs one
 * stale connect() after COVEY changes address, which finish() already handles. */
static bool resolveHost() {
  const uint32_t now = millis() | 1u;
  if (s_hostResolvedAt && (uint32_t)(now - s_hostResolvedAt) < HOST_CACHE_MS &&
      (uint32_t)s_hostIp != 0) {
    return true;
  }
  IPAddress ip;
  if (ip.fromString(s_host)) {          // a literal address costs no lookup at all
    s_hostIp = ip;
    s_hostResolvedAt = now;
    return true;
  }
  ip = resolveDomain(s_host);
  if ((uint32_t)ip == 0) {
    return false;
  }
  s_hostIp = ip;
  s_hostResolvedAt = now;
  return true;
}

// One CSM1 line -> the store. Returns 1 if it was new.
static int ingestLine() {
  s_line[s_lineLen] = '\0';
  int added = 0;
  if (s_lineOverflow) {
    /* A line longer than any record we can build. Dropped rather than parsed from its
     * middle, and SAID SO: a silently skipped record is a text that never appears, with
     * nothing anywhere to explain it. */
    log_e("SMSMIRROR poll: over-long line dropped (%u bytes)", (unsigned)s_lineLen);
  } else if (smsMirrorIsMirrorLine(s_line)) {
    int r = smsMirrorIngestLine(s_line);
    if (r > 0) {
      added = 1;
    }
    SmsMirrorRecord rec;
    if (smsMirrorUnpack(s_line, &rec) && rec.id > s_sinceId) {
      s_sinceId = rec.id;
    }
  }
  s_lineLen = 0;
  s_lineOverflow = false;
  return added;
}

// ---------------------------------------------------------------- the step
int smsMirrorPollLoop() {
  if (!loadConfig()) {
    return 0;
  }
  const uint32_t now = millis();

  if (s_state == PS_IDLE) {
    /* Signed difference, not `now < s_nextPollMs`: millis() wraps every ~49 days and an
     * unsigned compare across the wrap would park the poller for another 49. */
    if (!s_forceNow && (int32_t)(now - s_nextPollMs) < 0) {
      return 0;
    }
    if (WiFi.status() != WL_CONNECTED) {
      s_status = "waiting for wifi";
      s_nextPollMs = now + POLL_INTERVAL_MS;
      return 0;
    }
    s_forceNow = false;
    if (!resolveHost()) {
      finish("host not found", false);
      return 0;
    }
    s_state = PS_CONNECT;
    s_status = "connecting";
    return 0;                       // connect on the NEXT pass, so this one stays cheap
  }

  if (s_state == PS_CONNECT) {
    /* The one call here that can block, bounded to CONNECT_TIMEOUT_MS. On a LAN a real
     * COVEY answers in single-digit milliseconds; the timeout only bites when it is off. */
    if (!s_client.connect(s_hostIp, s_port, CONNECT_TIMEOUT_MS)) {
      finish("no answer from COVEY", false);
      return 0;
    }
    char req[320];
    int w = snprintf(req, sizeof(req),
                     "GET /sms?since=%lld HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "%s: %s\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     (long long)s_sinceId, s_host, "X-Covey-Token", s_token);
    if (w <= 0 || (size_t)w >= sizeof(req)) {
      finish("request too long", false);
      return 0;
    }
    s_client.print(req);
    s_state = PS_HEADERS;
    s_status = "reading";
    s_deadlineMs = now + READ_TIMEOUT_MS;
    s_lineLen = 0;
    s_lineOverflow = false;
    return 0;
  }

  // --- PS_HEADERS / PS_BODY: consume at most READ_BUDGET bytes this pass.
  if ((int32_t)(now - s_deadlineMs) >= 0) {
    finish("timed out", false);
    return 0;
  }

  int added = 0;
  int budget = READ_BUDGET;
  while (budget-- > 0 && s_client.available()) {
    int c = s_client.read();
    if (c < 0) {
      break;
    }
    if (c == '\r') {
      continue;                    // CRLF: the LF does the work
    }
    if (c == '\n') {
      if (s_state == PS_HEADERS) {
        if (s_lineLen == 0) {
          s_state = PS_BODY;       // blank line ends the headers
        } else if (s_lineLen >= 12 && !strncmp(s_line, "HTTP/1.", 7)) {
          s_line[s_lineLen] = '\0';
          if (strstr(s_line, " 200") == NULL) {
            /* 403 is the token being wrong, and it is worth saying plainly: every other
             * failure mode here looks the same from the outside. */
            log_e("SMSMIRROR poll: %s", s_line);
            finish(strstr(s_line, " 403") ? "rejected: bad token" : "COVEY refused", false);
            return added;
          }
        }
        s_lineLen = 0;
        s_lineOverflow = false;
      } else {
        added += ingestLine();
      }
      continue;
    }
    if (s_lineLen < sizeof(s_line) - 1) {
      s_line[s_lineLen++] = (char)c;
    } else {
      s_lineOverflow = true;       // keep draining to the newline; drop the record
    }
  }

  if (!s_client.connected() && !s_client.available()) {
    if (s_state == PS_BODY && s_lineLen) {
      added += ingestLine();       // a final line with no trailing newline
    }
    finish("idle", true);
  }
  return added;
}
