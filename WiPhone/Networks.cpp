/*
Copyright © 2019, 2020, 2021, 2022 HackEDA, Inc.
Licensed under the WiPhone Public License v.1.0 (the "License"); you
may not use this file except in compliance with the License. You may
obtain a copy of the License at
https://wiphone.io/WiPhone_Public_License_v1.0.txt.

Unless required by applicable law or agreed to in writing, software,
hardware or documentation distributed under the License is distributed
on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
either express or implied. See the License for the specific language
governing permissions and limitations under the License.
*/

#include "Networks.h"
#include <Preferences.h>
#include "helpers.h"

extern void heapEvent(const char* what);   // WiPhone.ino - the ratchet instrument
#include "esp_wifi.h"
#include "esp_bt.h"

void Networks::getMac(uint8_t* mac) {
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  log_d("%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ===================================================== EXTERNS =====================================================

MDNSResponder mdnsResponder;

Networks wifiState;

//wifi event handler
void processWiFiEvent(WiFiEvent_t event) {
  switch(event) {
  case SYSTEM_EVENT_STA_GOT_IP:
    /* 🛑 TWO WRITE-ONLY SOCKETS USED TO BE OPENED HERE, AND THEY COULD KILL THE PHONE.
     *
     * `udp.begin(localUdpPort)` and `udpRtcp.begin(localUdpPort+1)` each cost a
     * `new char[1460]` inside WiFiUDP::begin(), behind the same DEAD null check as
     * parsePacket() and _scanDone() — and this runs on the WIFI EVENT TASK, so unlike
     * udpParsePacketSafe() there is nowhere to put a try/catch. Two 1,460-byte contiguous
     * INTERNAL requests, on the association that follows a scan that has just taken ~3.6 KB.
     *
     * Nothing ever read either socket. Verified 2026-08-29 across the whole tree: no
     * read/parsePacket/write/beginPacket/available/stop on either, ever. RTP audio uses its
     * own `rtp` socket (Audio.cpp:1500) and the only `udpRtcp` mentions are inside a
     * commented-out block. So this was ~2,920 bytes of internal heap held for the life of
     * the boot, plus an uncatchable throw, in exchange for nothing at all.
     *
     * The `delay(100)` went with them: its comment said it existed solely to stop the
     * begin() calls below it from erroring, so it was blocking the shared event task for
     * 100 ms on every GOT_IP for the sake of code that is now gone. */
    wifiState.setConnected(true, true);
    //When connected set
    log_d("connected! IP address: %s", WiFi.localIP().toString().c_str());
    break;
  case SYSTEM_EVENT_STA_DISCONNECTED:
    log_d("lost connection");
    wifiState.setConnected(false, true);
    break;
  case SYSTEM_EVENT_WIFI_READY:
    //log_d("ready");      // comes up very frequently
    break;
  case SYSTEM_EVENT_SCAN_DONE:
    log_d("scan done");
    break;
  case SYSTEM_EVENT_STA_START:
  case SYSTEM_EVENT_STA_STOP:
  case SYSTEM_EVENT_STA_CONNECTED:
  case SYSTEM_EVENT_STA_AUTHMODE_CHANGE:
  case SYSTEM_EVENT_STA_LOST_IP:
  case SYSTEM_EVENT_STA_WPS_ER_SUCCESS:
  case SYSTEM_EVENT_STA_WPS_ER_FAILED:
  case SYSTEM_EVENT_STA_WPS_ER_TIMEOUT:
  case SYSTEM_EVENT_STA_WPS_ER_PIN:
  case SYSTEM_EVENT_AP_START:
  case SYSTEM_EVENT_AP_STOP:
  default:
    break;
  }
}

static uint32_t s_msLastConnectAttempt = 0;

uint32_t lastWifiConnectAttemptMs() {
  return s_msLastConnectAttempt;
}

void connectToWiFi(const char* ssid, const char* pwd) {

  // TODO: do not connect while scanning for networks

  s_msLastConnectAttempt = millis();   // every join path stamps this — see Networks.h

  log_d("Connecting to network: %s", ssid);

  /* 🔑 `false`, NOT `true`. The argument is `wifioff`, so WiFi.disconnect(true) calls
   * esp_wifi_stop() and tears the whole radio down just to reassociate — and MEASURED on this
   * phone that costs **5007 ms**, against 30 ms for the WiFi.begin() that follows it:
   *
   *     SLOW WIFI: connectToWiFi [disconnect(true)=5007 begin=30 other=0]
   *
   * Everything here is one task, so those five seconds froze the keypad, the screen and the
   * WiFi stack together. That is the freeze Nick reported while scrolling — it needed the
   * hotspot to blip, which is why it was rare and why it always came with "and WiFi dropped":
   * the drop was not a symptom of the freeze, it was the trigger for it.
   *
   * A plain disassociate is all a reconnect needs; WiFi.begin() sets the new config regardless,
   * so "delete old config" was never doing work that begin() would not redo.
   * ⚠ The FULL cycle still exists deliberately elsewhere (`WiFi.disconnect(true, true)` in the
   * hard reset) — this is only the reconnect path. */
  const uint32_t _wt0 = millis();
  WiFi.disconnect(false);
  const uint32_t _wtDisc = millis();
  wifiState.setConnected(false, false);

  /* ⚠ THE EVENT HANDLER IS REGISTERED ONCE, IN Networks::init() — NOT HERE.
   *
   * This used to call WiFi.onEvent(processWiFiEvent) on every attempt, and
   * WiFiGenericClass::onEvent only ever does cbEventList.push_back() — it does not
   * deduplicate, and removeEvent() is never called anywhere in this firmware. So the
   * list grew by one entry per connect attempt and NEVER shrank except at reboot.
   *
   * That matters because connectToWiFi() is the RETRY: WiPhone.ino:1667 calls it every
   * WIFI_RETRY_PERIOD_MS (20 s) for as long as there is no network. Out of range for the
   * length of a car journey — 143 minutes, measured — that is roughly 430 registrations.
   *
   * Then the moment WiFi comes back, ONE SYSTEM_EVENT_STA_GOT_IP runs processWiFiEvent
   * N times, and this handler is not free: each pass does delay(100) plus udp.begin() and
   * udpRtcp.begin(), and WiFiUDP::begin() calls stop() (delete[] tx_buffer) then
   * `new char[1460]`. At N=430 that is ~43 s of blocking delay and ~1,720 alloc/free
   * cycles of 1,460 INTERNAL bytes, from one reconnect.
   *
   * ⚠ There is no malloc→PSRAM auto-diversion in this build to soften that: the framework
   * sdkconfig.h defines CONFIG_SPIRAM_USE_CAPS_ALLOC and NOT CONFIG_SPIRAM_USE_MALLOC, so
   * plain new/malloc is internal at EVERY size. (docs/MUSIC.md's "threshold is 16 KB" is
   * wrong for this build — there is no threshold, only heap_caps_malloc reaches PSRAM.)
   *
   * This is the best candidate found for the measured signature of the reset_reason=4
   * panics: free heap recovers while the LARGEST FREE BLOCK never does, and it degrades
   * on WiFi events rather than with uptime. The registry growth is monotonic, which is
   * exactly the "never recovers" part. */

  //Initiate connection
  const uint32_t _wtPre = millis();
  WiFi.begin(ssid, pwd);
  const uint32_t _wtBegin = millis();
  if (_wtBegin - _wt0 > 150) {
    log_e("SLOW WIFI: connectToWiFi [disconnect=%u begin=%u other=%u]",
          (unsigned)(_wtDisc - _wt0), (unsigned)(_wtBegin - _wtPre),
          (unsigned)(_wtPre - _wtDisc));
  }

  // Limit transmit power to 14 dBm
  int rv = 0;
  if ((rv = esp_wifi_set_max_tx_power(56)) != ESP_OK) {
    log_e("failed to limit transmit power: %d", rv);
  }

  /* ── WiFi MODEM SLEEP ─────────────────────────────────────────────────────────────
   * The radio was running with power save OFF, meaning the receiver stayed powered
   * continuously — tens of milliamps, forever, on a phone that spends most of its life
   * in a pocket doing nothing. MIN_MODEM parks the radio between the access point's DTIM
   * beacons and wakes for each one.
   *
   * ⚠ It still receives everything. Broadcast and buffered traffic is delivered at the
   * DTIM interval (typically 100–300 ms), so an incoming SIP INVITE or a page load is
   * delayed by up to that, not lost. That is the right trade for a phone; MAX_MODEM
   * would save more and can miss beacons, which is not.
   *
   * Set after begin() on purpose: the driver resets the power-save mode when the station
   * starts, so setting it earlier is silently undone. */
  if ((rv = esp_wifi_set_ps(WIFI_PS_MIN_MODEM)) != ESP_OK) {
    log_e("failed to enable wifi modem sleep: %d", rv);
  }

  log_d("Waiting for connection...");
}

// Inspired by: https://github.com/nkolban/esp32-snippets/blob/master/cpp_utils/WiFi.cpp
// Alternative way: use dns_gethostbyname. See: https://gist.github.com/MakerAsia/37d2659310484bdbba9d38558e2c3cdb
/* 🛑 THIS BLOCKS THE MAIN LOOP, AND THE UI IS SINGLE-THREADED.
 *
 * Everything the user sees runs on the same task, so every millisecond spent in here is a
 * millisecond the screen is frozen. Reached from SIP connect (tinySIP.cpp), from RTP setup
 * DURING a call (WiPhone.ino) and from NTP (clock.cpp).
 *
 * Reported symptom that led here: "sometimes when clicking menus the phone will freeze for
 * like 5 seconds". Two blocking calls in series were doing it:
 *
 *   1. An mDNS query with a 500 ms timeout, run for EVERY name — including public ones like
 *      `seattle1.voip.ms` and `pool.ntp.org`. mDNS only ever answers for `.local` hosts, so
 *      that half second was guaranteed wasted every single time. The overnight log is full of
 *      `seattle1.voip.ms not found on local network`; that line IS the wasted 500 ms.
 *   2. lwip_gethostbyname(), which retries internally and can block for SEVERAL SECONDS when a
 *      DNS server is slow or unreachable. `errno=210: unable to resolve "pool.ntp.org"` in the
 *      same log is that, and NTP keeps retrying it.
 *
 * So: only ask mDNS about names it could possibly answer for, and cache what DNS returns so a
 * reconnect does not pay for it twice.
 *
 * ⚠ `.local` answers are deliberately NOT cached. mDNS names move with the network — the
 * documented `wiphone.local` staleness trap is exactly that — so they must stay live. Only
 * public DNS answers are cached, and only for TTL_MS. */
IPAddress resolveDomain(const char* hostName) {
  if (hostName == NULL || !hostName[0]) {
    return IPAddress((uint32_t)0);
  }

  /* mDNS can only answer for single-label names and `.local`. Anything else is a public name,
   * and asking mDNS about it is a guaranteed half second of nothing. */
  const char* dot = strchr(hostName, '.');
  const bool couldBeLocal = (dot == NULL) || (strcasecmp(dot, ".local") == 0);

  /* ⚠ FAILURES ARE CACHED TOO, and that half matters more than the successes.
   *
   * Measured on the device 2026-08-17: `pool.ntp.org` failed to resolve SEVEN times in two
   * minutes, because NTP retries on TIME_UPDATE_RETRY_DELAY_MS (500 ms) and every retry is a
   * fresh blocking lwip_gethostbyname(). On a network where that name does not resolve — a
   * work WiFi with restrictive DNS, say — the phone spends whole seconds inside this function,
   * over and over, with the UI frozen because it is all one task.
   *
   * A negative entry makes a failing name cost NOTHING until NEG_TTL_MS has passed, no matter
   * how eagerly the caller retries. That is the general fix: it protects against any caller
   * hammering any name that will not resolve, not just this one. Kept much shorter than the
   * positive TTL so a genuinely transient DNS outage recovers quickly. */
  static const int      RESOLVE_CACHE_N = 6;
  static const uint32_t TTL_MS     = 10u * 60u * 1000u;    // 10 min for a good answer
  static const uint32_t NEG_TTL_MS = 60u * 1000u;          // 1 min for a failure
  struct CacheEntry {
    char     host[64];
    uint32_t addr;        // 0 == negative entry (this name did not resolve)
    uint32_t at;          // 0 == slot never used
  };
  static CacheEntry s_cache[RESOLVE_CACHE_N] = {};
  static uint8_t    s_next = 0;
  const uint32_t    nowMs = millis() | 1u;                 // never 0, so `at` marks "used"

  if (!couldBeLocal) {
    for (int i = 0; i < RESOLVE_CACHE_N; i++) {
      if (!s_cache[i].at || strcmp(s_cache[i].host, hostName)) {
        continue;
      }
      const uint32_t age = (uint32_t)(nowMs - s_cache[i].at);
      if (s_cache[i].addr && age < TTL_MS) {
        return IPAddress(s_cache[i].addr);                 // known good: no blocking call
      }
      if (!s_cache[i].addr && age < NEG_TTL_MS) {
        return IPAddress((uint32_t)0);                     // known bad: fail instantly
      }
      break;                                                // stale — fall through and re-look
    }
  }

  if (wifiState.mdnsOk && couldBeLocal) {
    IPAddress addr = mdnsResponder.queryHost(hostName, 500);
    if (addr) { // where is the class definition of IPAddress? Need to know if this is a valid way to test if addr is set.
      log_i("resolved: %s -> %d.%d.%d.%d", hostName, addr[3], addr[2], addr[1], addr[0]);
      return addr;
    } else {
      log_e("%s not found on local network", hostName);
    }
  }

  unsigned long retAddr;
  struct hostent* he = lwip_gethostbyname(hostName);
  if (he != nullptr) {
    retAddr = *(unsigned long*) (he->h_addr_list[0]);       // take only first address
    log_d("resolved: %s -> %d.%d.%d.%d", hostName, retAddr & 0xFF, (retAddr >> 8) & 0xFF, (retAddr >> 16) & 0xFF, (retAddr >> 24) & 0xFF);
  } else {
    retAddr = 0;
    log_e("errno=%d: unable to resolve \"%s\"", h_errno, hostName);
  }

  /* Record the outcome either way — a failure is exactly what we must remember. */
  if (!couldBeLocal && strlen(hostName) < sizeof(s_cache[0].host)) {
    int slot = -1;
    for (int i = 0; i < RESOLVE_CACHE_N; i++) {       // reuse this name's slot if it has one
      if (s_cache[i].at && !strcmp(s_cache[i].host, hostName)) {
        slot = i;
        break;
      }
    }
    if (slot < 0) {
      slot = s_next;
      s_next = (s_next + 1) % RESOLVE_CACHE_N;
    }
    strlcpy(s_cache[slot].host, hostName, sizeof(s_cache[0].host));
    s_cache[slot].addr = (uint32_t)retAddr;           // 0 here means "known bad"
    s_cache[slot].at   = nowMs;
  }
  return IPAddress(retAddr);
}

// ===================================================== WIFI STATE =====================================================

Networks::Networks() : ini(filename), _userDisabled(false) {
  prefSsidDyn = NULL;
  wifiSsidDyn = NULL;
  wifiPassDyn = NULL;
  connected = false;
  reconnect = true;
}

Networks::~Networks() {
  freeNull((void **) &prefSsidDyn);
  freeNull((void **) &wifiSsidDyn);
  freeNull((void **) &wifiPassDyn);
}

void Networks::init() {
  /* Register the WiFi event handler EXACTLY ONCE, for the life of the boot. It used to be
   * done inside connectToWiFi(), i.e. on every retry — see the long note there for why that
   * was the most likely cause of the reset_reason=4 panics. Guarded as well as moved, so
   * that a second call to init() cannot quietly reintroduce the bug. */
  static bool s_eventHandlerRegistered = false;
  if (!s_eventHandlerRegistered) {
    s_eventHandlerRegistered = true;
    WiFi.onEvent(processWiFiEvent);
  }

  // Reset WiFi (these are needed for proper scanning!!!)
  WiFi.mode(WIFI_STA);
  log_v("Free memory after wifi mode: %d", ESP.getFreeHeap());
  WiFi.disconnect();
  const char* host = "WiPhone"; // Later append serial number here
  log_v("Free memory after disconnect: %d", ESP.getFreeHeap());
  if (mdnsResponder.begin(host)) {
    mdnsOk = true;
    log_i("MDNS Responder Hostname: %s", host);
  } else {
    mdnsOk = false;
    log_e("MDNS Responder Hostname: %s failed to initialize", host);
  }

  log_v("Free memory after responder begin: %d", ESP.getFreeHeap());
  delay(100);
}

// ===================================================== NETWORK CONNECTIONS =====================================================

void Networks::disconnect() {
  WiFi.disconnect(true, true);  // wifioff = true, eraseap = true (erasing might be needed when using Arduino-ESP32 ver. >= 1.0.3)
  connected = false;
  reconnect = false;
}

/*
 * Disable the radio.
 */

void Networks::disable() {
  /* Every caller of disable() is an explicit user action (the menu WIFI toggle OFF, Remove
   * network, the Disconnect button, the edit screen's WIFI-OFF) — so say so in the LIVE
   * flag, not just the INI. Before 2026-08-27 only loadPreferred() ever set _userDisabled
   * (from the INI, typically at boot), so a Disconnect pressed mid-session left the live
   * flag false — and anything gating on userDisabled() was reading a stale answer. The
   * flag is cleared by the symmetric acts: a deliberate connectTo() or resumeReconnect(). */
  _userDisabled = true;
  disconnect();
  WiFi.mode(WIFI_OFF);
  btStop(); // we don't currently use bluetooth for anything, leave it off to save power
  esp_wifi_stop(); //likely unnecessary
  esp_bt_controller_disable(); //likely unnecessary
  log_d("WiFi and BT disabled");
}

/* Description:
 *      load password for a network and set the network as current network (SSID/pass get remembered)
 */
bool Networks::loadNetworkSettings(const char* ssid) {
  log_d("loadNetworkSettings: %s", ssid);
  // TODO: unload ini after using if it is too big (or there is no PSRAM)
  // TODO: consider that there might be multiple networks with the same name
  ini.unload();
  if ((ini.load() || ini.restore()) && !ini.isEmpty()) {
    int i = ini.query("s", ssid);
    if (i>=0 && ini[i].hasKey("p")) {   // check correctness        TODO: use mnemonics for these tiny key names
      // One network found
      log_d("found");
      freeNull((void **) &wifiSsidDyn);
      freeNull((void **) &wifiPassDyn);
      wifiSsidDyn = strdup(ssid);
      wifiPassDyn = strdup(ini[i]["p"]);
      return true;
    }
  }
  return false;
}

/* Desctiption:
 *      load name of the preferred network
 */
void Networks::loadPreferred() {
  log_d("loadPreferred");
  freeNull((void **) &prefSsidDyn);
  ini.unload();
  if ((ini.load() || ini.restore()) && !ini.isEmpty()) {
    int i = ini.findKey("m");
    if (i>=0 && ini[i].hasKey("s")) {
      // Preferred network found
      log_d("preferred network = %s", ini[i]["s"]);
      prefSsidDyn = strdup(ini[i]["s"]);
      const char *disabled = ini[i]["disabled"];
      log_d("loadPreferred: 0x%x", disabled);
      if (disabled != NULL) {
        log_d("loadPreferred is: %s", disabled);
        if (strcmp(disabled, "true") == 0) {
          _userDisabled = true;
        } else {
          _userDisabled = false;
        }
      }
    }
  }
}

bool Networks::connectTo(const char* ssid) {
  log_d("connectTo");
  /* Split-timed: loadNetworkSettings() reads networks.ini from SPIFFS, and SPIFFS `open` alone
   * costs ~1.6 s on this part (measured with `bench`), while connectToWiFi() is documented as
   * async. One of those two claims is wrong and this says which. */
  const uint32_t t0 = millis();
  // Load password and connect to WiFi network
  const bool loaded = this->loadNetworkSettings(ssid);
  const uint32_t tIni = millis();
  bool r = false;
  if (loaded) {
    /* ── A DELIBERATE JOIN RESTORES THE RECONNECT FLAG (found 2026-08-27) ──────
     * `reconnect` is set true in the constructor and false in disconnect()/disable(),
     * and NOTHING ever set it back — so the WiFi settings screen's save path (which
     * calls disconnect() before rejoining) left every phone that ever had a network
     * EDITED in a state where it would never auto-rejoin again until reboot. That is
     * the real shape of phone 1's months of "keeps losing WiFi": not the radio, not
     * SIP, not hardware — a one-way flag, flipped by the very screen used to fix the
     * WiFi. Every caller of connectTo() (the settings screen, connectToPreferred,
     * the auto-switch hop) is expressing "I want to be on this network", which is
     * exactly what reconnect means. The same argument clears _userDisabled (added
     * 2026-08-27 when disable() started setting it live): a deliberate join IS the user
     * re-enabling WiFi, and the settings screen's join path writes disabled=false to the
     * INI in the same breath — the live flag must agree with it. */
    reconnect = true;
    _userDisabled = false;
    _joinsTried++;                              // instrument: pairs with _joinsSkipped
    connectToWiFi(wifiSsidDyn, wifiPassDyn);    // "async"
    r = true;
  }
  const uint32_t total = millis() - t0;
  if (total > 150) {
    log_e("SLOW WIFI: connectTo %u ms [loadNetworkSettings=%u connectToWiFi=%u]",
          (unsigned)total, (unsigned)(tIni - t0), (unsigned)(millis() - tIni));
  }
  return r;
}

bool Networks::hasPreferredSsid(void) {
  log_d("hasPreferredSsid");
  // Do we have a saved default WiFi network?
  if (prefSsidDyn==NULL) {
    this->loadPreferred();
  }
  if (prefSsidDyn==NULL) {
    log_d("SSID NOT LOADED");
    return false;
  }
  return true;
}

bool Networks::connectToPreferred(void) {
  log_d("connectToPreferred");
  /* MEASURED 2026-08-24: this call blocks the WHOLE superloop for ~5 s when the hotspot
   * blips, which is the freeze Nick feels while scrolling. Split-timed here to say which
   * half — the INI read (SPIFFS, and `open` alone costs 1.6 s on this part) or the join. */
  const uint32_t t0 = millis();
  // Load name of preferred WiFi network
  // TODO: find the exact preferred network, not just find the name and then search by name
  if (prefSsidDyn==NULL) {
    this->loadPreferred();
  }
  const uint32_t tLoad = millis();
  if (prefSsidDyn==NULL) {
    log_d("SSID NOT LOADED");
    return false;
  }
  const bool r = connectTo(prefSsidDyn);
  const uint32_t total = millis() - t0;
  if (total > 150) {
    log_e("SLOW WIFI: connectToPreferred %u ms [loadPreferred=%u connectTo=%u]",
          (unsigned)total, (unsigned)(tLoad - t0), (unsigned)(millis() - tLoad));
  }
  return r;
}

/* Radio OFF and back to STA, with the state layer told the truth FIRST (2026-08-27).
 * The opening plain disconnect() matters: issued while associated it delivers
 * STA_DISCONNECTED, which is what clears `connected`. The first cut of `wifi bounce`
 * went straight to disconnect(true) — radio off swallowed that event, `connected`
 * stayed true, and BOTH rescue loops (retry and auto-switch) sat gated on it: an
 * associated bench phone was wedged half-down until a reflash. Found live, same day
 * the command was written. The explicit `connected = false` after the delay is belt
 * and braces — recovery must not depend on event-delivery timing. Ends by making the
 * next auto-switch scan due immediately, so the cure is followed by the look-around
 * that uses it. Does NOT touch `reconnect`, does NOT eraseap. */
void Networks::bounceRadio(void) {
  WiFi.disconnect();            // event → connected=false while the radio can still say so
  delay(400);
  connected = false;
  WiFi.disconnect(true /*radio OFF*/);
  delay(300);
  WiFi.mode(WIFI_STA);          // back up, idle
  _dryScans = 0;
  _dryBounced = false;          // a deliberate bounce starts the spell accounting over
  _msLastScan = millis() - 600000u;   // next autoSwitchTick: a scan is due NOW
}

/* "Manage WiFi again": the symmetric partner of disable(), without naming a network.
 * Called by the WIFI-ON toggles (menu and edit screen) and by NetworksApp's destructor
 * for the peek-and-back-out case. Arms the retry loop and the auto-switcher; the actual
 * join is theirs — blocking the UI on a join is what froze the edit screen once. */
void Networks::resumeReconnect(void) {
  _userDisabled = false;
  reconnect = true;
}

// ===================================================== WIFI AUTO-SWITCH =====================================================
// Scans in the background (async, keeps the current association) and hops to
// the strongest saved network. Driven by autoSwitchTick() from the main loop.

#define AUTO_SCAN_PERIOD_MS      600000u  // scan every 10 min while connected (battery)
#define AUTO_SCAN_DISC_PERIOD_MS 120000u  // every 2 min while disconnected (nothing to lose)
#define AUTO_SCAN_RETRY_MS       30000u   // re-try soon after a failed/aborted scan
#define AUTO_SWITCH_MARGIN_DB 10        // hop only if this much stronger than current

bool Networks::autoSwitchEnabled(void) {
  if (_autoSwitch < 0) {                // lazy-load once; cached afterwards
    _autoSwitch = 1;                    // default: enabled
    ini.unload();
    if ((ini.load() || ini.restore()) && !ini.isEmpty()) {
      const char* v = ini[0]["autosw"];
      if (v != NULL && !strcmp(v, "0")) {
        _autoSwitch = 0;
      }
    }
  }
  return _autoSwitch == 1;
}

void Networks::setAutoSwitch(bool enabled) {
  _autoSwitch = enabled ? 1 : 0;
  ini.unload();
  if (ini.load() || ini.restore()) {
    ini[0]["autosw"] = enabled ? "1" : "0";
    ini.store();
  }
}

void Networks::autoSwitchTick(bool screenOn) {
  bool wake = screenOn && !_prevScreenOn;
  _prevScreenOn = screenOn;

  // Diagnostic line (log_e = visible over serial @500000) — ON STATE CHANGE, not on a
  // timer. The 15 s heartbeat wrote ~5,700 identical lines a day into the field log and
  // buried the lines that matter; the states it reports change a handful of times a day.
  // The feature has failed silently twice, so the line itself stays — every transition of
  // any gate still prints, which is what the heartbeat was really for.
  {
    static uint32_t s_lastState = 0xFFFFFFFF;
    const uint32_t state = ((uint32_t)_userDisabled)
                         | ((uint32_t)reconnect          << 1)
                         | ((uint32_t)autoSwitchEnabled() << 2)
                         | ((uint32_t)connected          << 3)
                         | ((uint32_t)_scanning          << 4);
    if (state != s_lastState) {
      s_lastState = state;
      log_e("[autosw] ud=%d rec=%d en=%d conn=%d scanning=%d sinceScan=%lus",
            (int)_userDisabled, (int)reconnect, (int)autoSwitchEnabled(),
            (int)connected, (int)_scanning, (unsigned long)((millis() - _msLastScan) / 1000));
    }
  }

  /* Maintain the dry-spell clock BEFORE the gates below, so it is honest even while the
   * auto-switcher is switched off — the retry in loop() reads the same predicate. millis()
   * can legitimately be 0 for one millisecond after boot, so 1 is used as the sentinel-safe
   * stamp; the tick runs at ~1 kHz, so at most one millisecond of spell is lost. */
  if (connected) {
    _drySpellStartMs = 0;
    _joinsSkippedRun = 0;               // back on a network: the valve starts over
  } else if (_drySpellStartMs == 0) {
    _drySpellStartMs = millis() | 1u;
  }

  if (_userDisabled || !reconnect || !autoSwitchEnabled()) {
    return;                             // same gates as the reconnect loop honors
  }

  uint32_t now = millis();

  if (_scanning) {                      // poll the async scan
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
      return;
    }
    _scanning = false;
    if (n > 0) {
      log_e("[autosw] scan done: n=%d", n);
      wifiScanNoteResult(n);          // size the next scan's heap estimate to THIS air
      heapEvent("scan-post");         // pairs with scan-pre: did this scan cost anything permanent?
      _dryScans = 0;
      _dryBounced = false;
      _scanDoneMs = now | 1u;           // evidence: a scan completed; evaluate says what it saw
      autoSwitchEvaluate(n);
    } else if (n == 0) {
      /* 🛑 A COMPLETED SCAN THAT FOUND NOTHING IS the out-of-range case — OR THE DEAF
       * RADIO (2026-08-27): after hours of disconnected retry churn the driver can reach a
       * state where scans complete empty forever while the AP is on air at -50 dBm. The two
       * are indistinguishable from one result, so count them: out of range clears on the
       * walk home anyway, and the deaf state gets a radio bounce at _dryScans >= 2 (below).
       * Either way the scan is stamped as ordinary so the 2/5-minute easing does its job —
       * taking the retry branch here used to defeat that backoff; see scheduleScanRetry(). */
      if (!connected && _dryScans < 1000) {
        _dryScans++;
      }
      log_e("[autosw] scan done: n=0 dry=%u", (unsigned)_dryScans);
      _msLastScan = now;
      _scanDoneMs = now | 1u;           // evidence: a scan completed and saw nothing at all
    } else {
      /* n < 0: the scan was ABORTED or the API failed — commonly a reconnect attempt
       * cycled WiFi and killed it, or the radio was mid-connect. Learned nothing about the
       * AIR, so try again soon rather than serving out a full period. But it IS evidence
       * about the MACHINERY (2026-08-27, the booksync wedge): the mid-connect churn that
       * presents as deafness alternates 0 and -2 completions, and counting only the zeros
       * let the wedge outlive the detector for a whole afternoon. Disconnected, a failed
       * completion counts toward the bounce like an empty one. */
      if (!connected && _dryScans < 1000) {
        _dryScans++;
      }
      log_e("[autosw] scan done: n=%d dry=%u", n, (unsigned)_dryScans);
      scheduleScanRetry(now, connected);
    }
    WiFi.scanDelete();
    return;
  }

  if (!_scanPending) {
    /* ⚠ Back off when there is plainly nothing to join. Disconnected, this scanned every
     * two minutes forever, and a scan lights up the radio for a few hundred milliseconds
     * — so a phone carried out of range all afternoon paid for thirty scans an hour to
     * learn the same thing thirty times.
     *
     * The first few stay at two minutes, which covers the common case of stepping briefly
     * out of range and wanting a quick rejoin. Only a sustained absence (roughly ten
     * minutes of failures) eases to five, and the counter resets the moment anything
     * connects, so coming home is still prompt. */
    uint32_t discPeriod = currentDiscPeriod();
    /* Deaf-evidence fast path (2026-08-27): with dry evidence on the books and no bounce
     * tried yet this spell, do not serve out the eased 2-5 min period — confirm and cure
     * within ~90 s. Capped to the first bounce (see _dryBounced in the header), so a phone
     * genuinely out of range still gets the battery easing. */
    /* ⚠ NOT during a long dry spell. This fast path exists to confirm-and-cure a DEAF RADIO
     * within ~90 s, and it is right to be aggressive for that — but a phone that has simply
     * been away from any network for ten minutes is not deaf, and letting this branch pull the
     * cadence back to 30 s would undo the easing above entirely. The bounce it leads to has
     * also already had its chance by then (_dryBounced). */
    if (!connected && _dryScans >= 1 && !_dryBounced && !inLongDrySpell() &&
        discPeriod > AUTO_SCAN_RETRY_MS) {
      discPeriod = AUTO_SCAN_RETRY_MS;
    }
    bool due = (now - _msLastScan >= (connected ? AUTO_SCAN_PERIOD_MS : discPeriod));
    if (wake && !connected) {
      /* Screen woke with no WiFi: scan right away. Backdating instead of a bare due=true
       * keeps the wish STICKY — the young-join guard below may hold the scan for a few
       * seconds (the wake RETRY fires earlier in this same loop pass), and a transient
       * flag would have been lost with it. */
      _msLastScan = now - discPeriod;
      due = true;
    }
    if (!due) {
      return;
    }
    if (!connected &&
        (uint32_t)(now - lastWifiConnectAttemptMs()) < 10000u) {
      /* A join younger than 10 s is still associating/DHCPing — it reads as 'not
       * connected' but is about to not be. The pre-scan disconnect below would abort it:
       * measured 2026-08-27, the screen-wake retry and the wake-forced scan killed each
       * other in the SAME loop pass, and at the 20 s retry cadence the mutual kill is
       * what keeps the radio perpetually mid-connect (= every scan completes empty).
       * `due` stays true, so the scan runs the moment the attempt is stale. */
      return;
    }
    _msLastScan = now;
    if (connected) {
      _discScans = 0;                   // on a network: forget the dry spell
      _dryScans = 0;
      _dryBounced = false;
    } else if (_discScans < 1000) {
      _discScans++;
    }
    _scanPending = true;                // scanBusy() now holds the reconnect loop off
    _msScanPendingSince = now;
    if (!connected) {
      if (_dryScans >= 2) {
        /* THE DEAF-RADIO RECOVERY (2026-08-27, measured on phone 1 at the work desk):
         * two consecutive scans that completed EMPTY while the twin radio heard the AP at
         * -50 dBm. No amount of scanning recovers from this state — screen wake at 240 MHz
         * read n=0 too — but a radio off/on clears it instantly (a rebooted radio heard 6
         * networks and SIP registered inside a minute). This is exactly what the user's
         * "open the WiFi screen and rescan" ritual did by accident: NetworksApp's ctor is
         * disconnect(true, true). Done here deliberately — WITHOUT eraseap, the driver's
         * remembered AP is not the disease — the next scanNetworks() below re-enables STA,
         * which is the "on" half. Counter resets so a genuinely out-of-range phone bounces
         * at most every second round (~10 min), not every scan. */
        log_e("[autosw] %u consecutive empty scans: restarting the radio before this one",
              (unsigned)_dryScans);
        _dryScans = 0;
        _dryBounced = true;             // this spell had its bounce: cadence re-eases
        WiFi.disconnect(true /*radio OFF; the scan start turns it back on*/);
      } else {
        // The Arduino WiFi driver auto-reconnects on "AP not found", so with the
        // preferred network absent the radio is perpetually mid-connect and scan
        // starts get rejected — the auto-switcher looked completely dead. A plain
        // disconnect stops that cycle (its disconnect reason is one the driver
        // does NOT auto-reconnect from) and frees the radio to scan.
        WiFi.disconnect();
      }
      return;                           // give the driver a tick to settle; start next pass
    }
  }

  // Start (or keep trying to start) the scan. The disconnect above settles
  // asynchronously, so the first attempts can be rejected — retrying every
  // tick for a few seconds beats losing a 30s-backoff race against the radio
  // (that race is why a 5-minute wait once produced zero completed scans).
  /* 🛑 THE ALLOCATION THAT KILLED PHONE 1 ON 2026-08-28 IS THE ONE THIS SCAN IS ABOUT TO
   * MAKE. _scanDone() will `new wifi_ap_record_t[n]` — ~192 contiguous internal bytes per
   * access point, measured — on the WiFi event task, where nothing can catch the throw.
   * This is the unattended scan: it runs every few minutes on an idle phone in a pocket,
   * so it is the one that turns a fragmented heap into a reboot while nobody is looking.
   * Skipping a round costs nothing — we stay on the AP we already have. See helpers.h. */
  if (!wifiScanMemoryOk("autosw")) {
    _scanPending = false;
    scheduleScanRetry(now, WiFi.status() == WL_CONNECTED);
    return;
  }
  heapEvent("scan-pre");            // the scan is the prime remaining suspect: bracket it
  int16_t r = WiFi.scanNetworks(true /*async*/);   // keeps an existing association
  if (r != WIFI_SCAN_FAILED) {
    log_e("[autosw] scan started (wifi status %d)", (int)WiFi.status());
    _scanPending = false;
    _scanning = true;
    return;
  }
  if (now - _msScanPendingSince > 5000) {
    log_e("[autosw] scan start kept failing (wifi status %d)", (int)WiFi.status());
    _scanPending = false;               // give up; normal backoff retries soon
    scheduleScanRetry(now, WiFi.status() == WL_CONNECTED);
  }
}

/* The disconnected scan period in force right now: two minutes for a brief blip, easing to
 * five once it is clearly not one. One definition, because the due-check and the retry
 * scheduler MUST agree — they did not, and that is what broke the backoff. */
/* The one WiFi switch, persisted across reboots.
 *
 * 🛑 THE ORIGINAL TOGGLE WAS DELIBERATELY NOT PERSISTED, and the reason given was sound:
 * "a radio that stays off across a power cycle is a setting you can forget you set, and the
 * failure mode is a phone that silently never connects again." Nick asked for persistence
 * anyway (2026-09-01) because the measured cost of leaving it on away from a network is real
 * — ~106 mV/h searching against ~60 mV/h associated. **So the objection is answered rather
 * than ignored**: the menu row now says "off" in the large font with "saves power - survives
 * restarts" underneath, and the boot log prints WIFI: radio is OFF at log_e. The state is
 * loud in the two places someone would look.
 *
 * Preferences rather than the configs INI: this is one bool, the INI is a CriticalFile with a
 * restore path, and the mesh settings already established this namespace pattern. */
void Networks::setRadioOff(bool off) {
  _radioOff = off;
  Preferences prefs;
  prefs.begin("wpwifi", false);
  prefs.putBool("radiooff", off);
  prefs.end();
  if (off) {
    disable();
  } else {
    resumeReconnect();
  }
  log_e("WIFI: radio switched %s by the user (persisted)", off ? "OFF" : "ON");
}

/* Read the switch at boot. Split from applying it so setup() can decide the order — the
 * radio must not be started first and stopped a moment later. */
bool Networks::loadRadioOff() {
  Preferences prefs;
  prefs.begin("wpwifi", true);          // read-only
  _radioOff = prefs.getBool("radiooff", false);
  prefs.end();
  return _radioOff;
}

bool Networks::worthAttemptingJoin() {
  /* Every early return here is a FAIL-OPEN. Read the note on the declaration before changing
   * any of them: this may only skip a join it has positive, recent evidence is pointless. */
  if (!inLongDrySpell()) {
    return true;                        // a brief blip: behave exactly as before, no latency
  }
  const uint32_t now = millis();
  if (_scanDoneMs == 0 ||
      (uint32_t)(now - _scanDoneMs) > WIFI_SCAN_EVIDENCE_MS) {
    return true;                        // no evidence, or stale — never block on ignorance
  }
  if (_savedSeenMs != 0 &&
      (uint32_t)(now - _savedSeenMs) <= WIFI_SCAN_EVIDENCE_MS) {
    return true;                        // a saved network was on the air recently: go
  }
  if (++_joinsSkippedRun >= WIFI_JOIN_BLIND_EVERY) {
    _joinsSkippedRun = 0;
    log_e("[wifi] scans see no saved network, but trying blind anyway "
          "(insurance for a hidden SSID or a lying scan)");
    return true;
  }
  _joinsSkipped++;
  log_e("[wifi] join skipped: last scan saw no saved network (%u in a row, %lu total) - "
        "a scan costs ~350ms, a failed join up to 30s of radio",
        (unsigned)_joinsSkippedRun, (unsigned long)_joinsSkipped);
  return false;
}

uint32_t Networks::currentDiscPeriod() const {
  /* Third tier added 2026-09-01 at Nick's ask. Two minutes covers a brief blip, five covers
   * a spell, and past ten minutes with no association at all the phone is somewhere without
   * WiFi — a car, the woods — where scanning three times as often learns the same thing
   * three times. See Networks::inLongDrySpell(). */
  if (inLongDrySpell()) {
    return WIFI_DRY_SCAN_PERIOD_MS;
  }
  return (_discScans >= 5) ? 300000u : AUTO_SCAN_DISC_PERIOD_MS;
}

/* Put the next scan AUTO_SCAN_RETRY_MS away, under whichever period the due-check will apply.
 *
 * 🛑 THIS EXISTS BECAUSE `_msLastScan = now - AUTO_SCAN_PERIOD_MS + AUTO_SCAN_RETRY_MS` IS
 * ONLY CORRECT WHILE CONNECTED. That expression means "600 s ago, minus 30", and the
 * due-check compares against AUTO_SCAN_PERIOD_MS *only when connected*. Disconnected it
 * compares against 120 s (or 300 s), and a stamp 570 s in the past is already older than
 * either — so the scan was due IMMEDIATELY and the next tick started another one.
 *
 * MEASURED on phone 1, 2026-08-26, with the access point gone: **114 scans in 280 seconds,
 * one every ~2.5 s**, where the design in autoSwitchTick() says one every two minutes. Each
 * scan lights the radio for a few hundred ms, so a phone carried out of range was burning
 * the radio essentially continuously — the exact cost the backoff was written to avoid, in
 * the exact situation it was written for. */
void Networks::scheduleScanRetry(uint32_t now, bool connected) {
  const uint32_t period = connected ? AUTO_SCAN_PERIOD_MS : currentDiscPeriod();
  _msLastScan = (period > AUTO_SCAN_RETRY_MS) ? (now - (period - AUTO_SCAN_RETRY_MS)) : now;
}

void Networks::autoSwitchEvaluate(int n) {
  ini.unload();
  if (!(ini.load() || ini.restore()) || ini.isEmpty()) {
    return;
  }

  int bestRssi = -127;
  String bestSsid;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);
    if (ssid.length() == 0 || rssi <= bestRssi) {
      continue;
    }
    int idx = ini.query("s", ssid.c_str());
    const char* dis = NULL;
    if (idx >= 0) {
      dis = ini[idx]["disabled"];
    }
    log_e("[autosw] seen '%s' %ddBm saved=%d dis=%s", ssid.c_str(), rssi,
          (int)(idx >= 0 && ini[idx].hasKey("p")), dis ? dis : "-");
    if (idx < 0 || !ini[idx].hasKey("p")) {
      continue;                         // not one of our saved networks
    }
    if (dis != NULL && !strcmp(dis, "true")) {
      continue;                         // user explicitly disconnected this one
    }
    bestRssi = rssi;
    bestSsid = ssid;
  }
  if (bestSsid.length() == 0) {
    log_e("[autosw] no saved network in range");
    return;                             // _savedSeenMs deliberately NOT stamped
  }
  _savedSeenMs = millis() | 1u;         // a saved network really is on the air right now

  if (connected && wifiSsidDyn != NULL && bestSsid.equals(wifiSsidDyn)) {
    return;                             // already on the best network
  }
  if (connected && bestRssi < WiFi.RSSI() + AUTO_SWITCH_MARGIN_DB) {
    return;                             // not enough gain to justify the hop
  }

  log_e("[autosw] switching to '%s' (%d dBm)", bestSsid.c_str(), bestRssi);
  // Make it the preferred network so the existing 20s reconnect loop pulls
  // toward the same place instead of fighting the switch.
  int i = ini.query("s", bestSsid.c_str());
  if (i >= 0 && ini.setUniqueFlag(i, "m")) {
    ini.store();
  }
  freeNull((void **) &prefSsidDyn);
  prefSsidDyn = strdup(bestSsid.c_str());
  connectTo(bestSsid.c_str());
}

bool Networks::scan(void) {
  // used as a reference only
  WiFi.mode(WIFI_STA);
  disconnect();
  delay(100);
  // Uncalled today, but guarded anyway: an unguarded scan site is how the phone reboots,
  // and nothing should be revived into that shape by accident. See helpers.h.
  if (!wifiScanMemoryOk("Networks::scan")) {
    return false;
  }
  int n = WiFi.scanNetworks();
  wifiScanNoteResult(n);
  log_d("scan done");
  log_d("networks: %d", n);
  for (int i=0; i<n; i++) {
    log_d("%d: %s (%d) %s", i, WiFi.SSID(i), WiFi.RSSI(i), (WiFi.encryptionType(i) == WIFI_AUTH_OPEN)? "\t- OPEN":"\t- closed");
    delay(10);
  }
  return n >= 0;
}
