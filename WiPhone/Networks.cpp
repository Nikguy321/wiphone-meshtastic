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
#include "helpers.h"
#include "esp_wifi.h"
#include "esp_bt.h"

void Networks::getMac(uint8_t* mac) {
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  log_d("%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ===================================================== EXTERNS =====================================================

WiFiUDP udp;
WiFiUDP udpRtcp;
MDNSResponder mdnsResponder;

Networks wifiState;

//wifi event handler
void processWiFiEvent(WiFiEvent_t event) {
  switch(event) {
  case SYSTEM_EVENT_STA_GOT_IP:
    //initializes the UDP state
    //This initializes the transfer buffer
    delay(100);               // without the delay, occasionally throws errors with Arduino-ESP32 ver. 1.0.4
    udp.begin(localUdpPort);
    udpRtcp.begin(localUdpPort+1);
    //delay(100);
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

void connectToWiFi(const char* ssid, const char* pwd) {

  // TODO: do not connect while scanning for networks

  log_d("Connecting to network: %s", ssid);

  // delete old config
  WiFi.disconnect(true);
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
  WiFi.begin(ssid, pwd);

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
  // Load password and connect to WiFi network
  if (this->loadNetworkSettings(ssid)) {
    connectToWiFi(wifiSsidDyn, wifiPassDyn);    // async
    return true;
  }
  return false;
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
  // Load name of preferred WiFi network
  // TODO: find the exact preferred network, not just find the name and then search by name
  if (prefSsidDyn==NULL) {
    this->loadPreferred();
  }
  if (prefSsidDyn==NULL) {
    log_d("SSID NOT LOADED");
    return false;
  }
  return connectTo(prefSsidDyn);
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

  // Diagnostic heartbeat (log_e = visible over serial @500000): the state of
  // every gate, every 15s. Cheap, and this feature has failed silently twice.
  static uint32_t dbgMs = 0;
  if (millis() - dbgMs > 15000) {
    dbgMs = millis();
    log_e("[autosw] ud=%d rec=%d en=%d conn=%d scanning=%d sinceScan=%lus",
          (int)_userDisabled, (int)reconnect, (int)autoSwitchEnabled(),
          (int)connected, (int)_scanning, (unsigned long)((millis() - _msLastScan) / 1000));
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
    log_e("[autosw] scan done: n=%d", n);
    if (n > 0) {
      autoSwitchEvaluate(n);
    } else {
      // Failed or empty — commonly a reconnect attempt cycled WiFi and killed
      // the scan before scanBusy() gating existed to prevent it, or the radio
      // was mid-connect. Try again soon, not in 10 minutes.
      _msLastScan = now - AUTO_SCAN_PERIOD_MS + AUTO_SCAN_RETRY_MS;
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
    uint32_t discPeriod = AUTO_SCAN_DISC_PERIOD_MS;
    if (_discScans >= 5) {
      discPeriod = 300000u;             // 5 min once it is clearly not a brief blip
    }
    bool due = (now - _msLastScan >= (connected ? AUTO_SCAN_PERIOD_MS : discPeriod));
    if (wake && !connected) {
      due = true;                       // screen woke with no WiFi: scan right away
    }
    if (!due) {
      return;
    }
    _msLastScan = now;
    if (connected) {
      _discScans = 0;                   // on a network: forget the dry spell
    } else if (_discScans < 1000) {
      _discScans++;
    }
    _scanPending = true;                // scanBusy() now holds the reconnect loop off
    _msScanPendingSince = now;
    if (!connected) {
      // The Arduino WiFi driver auto-reconnects on "AP not found", so with the
      // preferred network absent the radio is perpetually mid-connect and scan
      // starts get rejected — the auto-switcher looked completely dead. A plain
      // disconnect stops that cycle (its disconnect reason is one the driver
      // does NOT auto-reconnect from) and frees the radio to scan.
      WiFi.disconnect();
      return;                           // give the driver a tick to settle; start next pass
    }
  }

  // Start (or keep trying to start) the scan. The disconnect above settles
  // asynchronously, so the first attempts can be rejected — retrying every
  // tick for a few seconds beats losing a 30s-backoff race against the radio
  // (that race is why a 5-minute wait once produced zero completed scans).
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
    _msLastScan = now - AUTO_SCAN_PERIOD_MS + AUTO_SCAN_RETRY_MS;
  }
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
    return;
  }

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
  int n = WiFi.scanNetworks();
  log_d("scan done");
  log_d("networks: %d", n);
  for (int i=0; i<n; i++) {
    log_d("%d: %s (%d) %s", i, WiFi.SSID(i), WiFi.RSSI(i), (WiFi.encryptionType(i) == WIFI_AUTH_OPEN)? "\t- OPEN":"\t- closed");
    delay(10);
  }
  return n >= 0;
}
