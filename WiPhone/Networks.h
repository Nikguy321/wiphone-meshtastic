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

#ifndef _WIFI_NETWORKS_
#define _WIFI_NETWORKS_

#include <WiFi.h>
#include "WiFiUdp.h"
#include "Storage.h"
#include "config.h"
#include "lwip/netdb.h"
#include "src/ping/ping.h"
#include <ESPmDNS.h>

#ifdef WIPHONE_PRODUCTION
#define WIFI_DEBUG(fmt, ...)
#else
#define WIFI_DEBUG(fmt, ...)      DEBUG("[wifi] " fmt, ##__VA_ARGS__)
#endif // WIPHONE_PRODUCTION

//IP address to send UDP data to: either use the ip address of the server or a network broadcast address


extern void connectToWiFi(const char* ssid, const char* pwd);

/* millis() of the last connectToWiFi() from ANY path — the periodic retry, the
 * auto-switcher, or a manual join in the networks app. The reconnect backoff's radio
 * quiesce consults this so it never disconnects an association it did not start. */
uint32_t lastWifiConnectAttemptMs();
extern IPAddress resolveDomain(const char* hostName);

// Class to save/load WiFi networks data from Flash
class Networks {
public:
  Networks(void);
  ~Networks(void);

  void init();
  void getMac(uint8_t* mac);

  // Interfaces
  bool loadNetworkSettings(const char* ssid);
  void loadPreferred();

  inline bool isConnected(void) {
    return connected;
  }
  inline bool isConnectionEvent(void) {
    return connectionEvent;
  }
  inline bool doReconnect(void) {
    return reconnect;
  }
  inline void setConnected(bool conn) {
    connected = conn;
  }
  inline void setConnected(bool conn, bool event) {
    connected = conn;
    connectionEvent = event;
  }

  // WiFi network staff
  bool connectToPreferred(void);
  void bounceRadio(void);        // radio off/on + state truth + prompt rescan — see .cpp
  void resumeReconnect(void);    // "manage WiFi again": clears user-disabled, arms rejoin
  bool hasPreferredSsid(void);
  bool connectTo(const char* ssid);
  void disconnect(void);
  void disable(void);
  bool scan(void);

  // WiFi auto-switch: periodically scan (async) for the strongest SAVED network
  // and hop to it. Runs from the main loop via autoSwitchTick(); a scan fires
  // immediately when the screen wakes with no connection. User-toggleable
  // (Settings -> WiFi auto-switch), persisted in networks.ini.
  bool autoSwitchEnabled(void);
  void setAutoSwitch(bool enabled);
  void autoSwitchTick(bool screenOn);
  bool scanBusy(void) {          // a scan is in flight or starting: hold off reconnect
    return _scanning || _scanPending;   // (connectToWiFi hard-cycles WiFi, killing scans)
  }

  /* Has this phone been off WiFi for long enough that it is plainly not a brief blip?
   *
   * 🔑 NICK ASKED FOR THIS BY NUMBER (2026-09-01): "if it doesn't see a Wi-Fi signal in 10
   * minutes, [make] the retry interval a lot less [often]". He had just driven 50 minutes home
   * with no WiFi, and that stretch measured ~106 mV/h against ~60 mV/h associated.
   *
   * ⚠ THE SCANS WERE NEVER THE EXPENSIVE PART — a scan is a few hundred ms. The cost is the
   * JOIN RETRY: WIFI_RETRY_PERIOD_MS is 20 s and eases only to 180 s, and every failed attempt
   * leaves the radio associating for up to 30 s before the quiesce in loop() disconnects it.
   * Out of range that is roughly 20 attempts an hour x 30 s ≈ an 8 % duty cycle at full radio
   * power, on top of the scans. Both cadences read this one predicate so they cannot disagree
   * — the file already learned that lesson once (see scheduleScanRetry).
   *
   * Both escape hatches survive: connecting clears the spell, and a screen wake still forces an
   * immediate scan and an immediate retry, so picking the phone up is prompt however long the
   * spell has run. */
  bool     inLongDrySpell() const {
    return _drySpellStartMs != 0 &&
           (uint32_t)(millis() - _drySpellStartMs) >= WIFI_DRY_SPELL_LONG_MS;
  }

  // Properties
  const char* ssid() {
    return wifiSsidDyn;
  }
  const char* pass() {
    return wifiPassDyn;
  }
  const char* prefSsid() {
    return prefSsidDyn;
  }

  bool userDisabled() {
    return _userDisabled;
  }

  static constexpr const char* filename = "/networks.ini";
  bool mdnsOk;                // MDNS service is operational

protected:
  void autoSwitchEvaluate(int n);   // pick the strongest saved network from a scan

  char* prefSsidDyn;
  char* wifiSsidDyn;    // current (or last) WiFi network
  char* wifiPassDyn;

  // Auto-switch state
  int8_t   _autoSwitch = -1;        // -1 = not loaded from ini yet
  bool     _scanning = false;       // an async scan is in flight
  bool     _scanPending = false;    // scan requested; keep retrying start briefly
  uint32_t _msScanPendingSince = 0;
  bool     _prevScreenOn = true;    // for the wake-up edge
  uint32_t _msLastScan = 0;
  uint32_t currentDiscPeriod() const;               // 2 min, easing to 5 — see the .cpp
  void     scheduleScanRetry(uint32_t now, bool connected);
  /* Consecutive scans run while disconnected. Used to stretch the scan interval when
   * there is clearly nothing in range — see autoSwitchTick(). Reset on any connect. */
  uint32_t _discScans = 0;
  /* When the current disconnected spell began, or 0 while connected. Drives the LONG-SPELL
   * easing — see inLongDrySpell(). Time, not a scan count, because the ask was in minutes and
   * a count only maps to minutes through whatever cadence happens to be in force. */
  uint32_t _drySpellStartMs = 0;
  /* Consecutive scans that COMPLETED with zero results while disconnected. Distinct from
   * _discScans (which counts rounds for the backoff): this one is the deaf-radio detector.
   * MEASURED 2026-08-27 (phone 1, work desk): after hours of disconnected retry churn the
   * driver enters a state where every scan completes EMPTY — async and blocking, 80 and
   * 240 MHz, quiesced or not — while the twin phone hears the same AP at -50 dBm, and the
   * ONLY cure is a radio off/on (which is why "open the WiFi screen and rescan" always
   * fixed it: NetworksApp's constructor bounces the radio by accident). At 2 the
   * auto-switcher bounces the radio itself before scanning — see autoSwitchTick().
   * MECHANISM (2026-08-27, the booksync wedge): the "deaf" radio is usually the radio held
   * perpetually MID-CONNECT — esp_wifi documents that scans run while the station is
   * connecting are ineffective, and the framework's event-task auto-reconnect retries
   * capless after every reason>=200, so once a drop starts churning, every scan completes
   * 0 or -2 until something cycles the radio. Failed completions (-2) therefore count as
   * dry evidence too, not just empty ones. */
  uint32_t _dryScans = 0;
  /* True once this dry spell has had its radio bounce. While false and _dryScans >= 1 the
   * disconnected scan interval drops to AUTO_SCAN_RETRY_MS so the deaf state is confirmed
   * and cured in ~90 s instead of 10-15 min (2026-08-27: the wedge outlived the eased
   * cadence all afternoon and the user's reboot always beat the bounce to it). After the
   * bounce the eased cadence returns: if one bounce did not cure it, the phone is genuinely
   * out of range and fast scanning would just drain the battery. Cleared whenever a scan
   * hears anything or a connection is made. */
  bool _dryBounced = false;

  bool _userDisabled;
  bool reconnect;             // should it try to reconnect when disconnected? TODO: save this in configs somehow
  bool connected;
  bool connectionEvent;       // connection/disconnection event processed

  MDNSResponder mdnsResponder;

  CriticalFile ini;
};

extern Networks wifiState;

#endif // _WIFI_NETWORKS_
