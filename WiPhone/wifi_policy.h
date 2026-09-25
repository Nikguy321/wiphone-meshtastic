/* wifi_policy.h — may the WiFi STATION come back right now, and if so how? ONE answer for every
 * site that switched the radio off (or lent it to a hotspot) and now hands it back. Kept in a
 * header of its own so the HOST SUITE tests the decisions the firmware executes
 * (tests/test_wifi_policy.cpp; the cpu_clock_policy.h pattern). Networks.cpp's
 * wifiRestoreStation() gathers the inputs and does the radio work; this file only decides.
 *
 * 🛑 THE BUG CLASS THIS FILE EXISTS FOR: WiFi TURNED BACK ON BEHIND THE OWNER (0.9.79, 2026-09-25).
 * Nothing answered the shared question, so every site that gave the radio back asked its own
 * subset of it:
 *   ~GbcApp (the Game Boy's exit) ......... radioOff() only
 *   the loop's join retry ................. userDisabled() + "hotspot live" - no game, no radioOff
 *   the uploader/window teardown .......... radioOff() + userDisabled(), a separate game branch
 *   Settings > WiFi's rescan / its exit ... radioOff() / userDisabled() - one each
 *   xferStart() ........................... nothing at all (serial `up on` mid-game = a softAP)
 *   the two "WiFi on" controls ............ a bare esp_wifi_start(), which restarts whatever mode
 *                                            the DRIVER last had - AP, after a hotspot session
 *                                            that ended with WiFi off (WiFi.mode(OFF) never calls
 *                                            esp_wifi_set_mode: arduino-esp32 1.0.6
 *                                            WiFiGeneric.cpp:541 `cm && !m -> espWiFiStop()`).
 * The worst of it, measured in the source: a phone with NO saved network, or after Disconnect
 * or Forget, is userDisabled() but NOT radioOff() (Networks::disable() sets one, not the other),
 * so every Game Boy exit ran `WiFi.mode(WIFI_STA); WiFi.reconnect()` on the ERASED config - and
 * nothing ever quieted it, because the loop's retry, its quiesce and the auto-switcher all stand
 * down for a userDisabled phone. The radio stayed up until the next reboot.
 *
 * ⚠ AND "radioOff IMPLIES userDisabled" IS NOT A RULE ANYONE ENFORCES. loadPreferred() rewrites
 * _userDisabled from the per-network INI flag (Edit network > Save does that with WiFi off), so
 * both switches are always asked - never one standing in for the other.
 *
 * The inputs, in the order they win:
 *   gameActive     a Game Boy game owns the cores and the internal RAM it took with WiFi off.
 *                  The station is NEVER brought back under it; the game's own exit asks again.
 *   softApLive     the uploader's or a KOSync window's hotspot is on the air. A station join
 *                  under a live softAP is the chip panic WiPhone.ino's reconnect gate exists to
 *                  prevent, so the radio is left to its owner, whose teardown asks again.
 *   radioOff / userDisabled   the owner said off (the global switch / Disconnect, Forget, no
 *                  saved network). / !reconnect: Networks::disconnect() left the station off and
 *                  the retry disarmed - nothing is waiting for it, so it is not brought up.
 *   staConnected   already associated: leave it alone (WiFi.status(), never the `connected`
 *                  flag, which a radio stopped while associated leaves stale-TRUE).
 *   joinYoung      a join started < 10 s ago is still associating: bring the station up but do
 *                  not re-begin under it (the loop's own young-join rule, Networks.cpp). Only
 *                  while its station RUNS - the gatherer reports false once the station was
 *                  stopped (a hotspot's mode(AP), a game, disable()), since nothing is then in
 *                  flight and waiting for the loop's next retry would only delay the rejoin.
 *   longDrySpell   5+ min with no network: come up idle and let the loop's GATED retry join -
 *                  it arms the 30 s quiesce and consults worthAttemptingJoin(); a restore's own
 *                  begin() did neither, so a window close in the car (with auto=on, every book
 *                  close) started a NO_AP_FOUND churn - the core's capless auto-reconnect - that
 *                  nothing quieted until the next auto-switch round's pre-scan disconnect.
 *
 * ⚠ Deliberately free of every Arduino and ESP-IDF header. Keep it that way.
 */
#ifndef WIFI_POLICY_H
#define WIFI_POLICY_H

#include <stdint.h>

enum WifiRestore : uint8_t {
  WIFI_RESTORE_JOIN    = 0,   // station up AND rejoin the network the driver remembers
  WIFI_RESTORE_UP_IDLE = 1,   // station up, no join: the loop's gated retry decides
  WIFI_RESTORE_OFF     = 2,   // radio off
  WIFI_RESTORE_LEAVE   = 3,   // touch nothing: someone else owns the radio, or it is already fine
};

struct WifiRestoreIn {
  bool radioOff;       // the global switch (Settings > WiFi: off), persisted
  bool userDisabled;   // Disconnect / Forget / no saved network / the INI's per-network flag
  bool reconnect;      // Networks::doReconnect(): false after disconnect() until something re-arms
  bool gameActive;     // gGbcActive
  bool softApLive;     // xferServing() && xferUsingAP(): the uploader's or a window's hotspot
  bool staConnected;   // WiFi.status() == WL_CONNECTED
  bool joinYoung;      // a join started less than 10 s ago AND its station is still running
  bool longDrySpell;   // Networks::inLongDrySpell()
};

/* May the STATION run at all right now? The loop's join retry asks exactly this (plus its own
 * cadence and scan gates). A NEW predicate, not an edit of radioOff()/userDisabled(): both of
 * those have other readers that mean something narrower (the menu label, the INI flag). */
static inline bool wifiStationWantedFrom(bool radioOff, bool userDisabled, bool gameActive,
                                         bool softApLive) {
  return !radioOff && !userDisabled && !gameActive && !softApLive;
}

/* The joinYoung input: is a join still ASSOCIATING? Started less than 10 s ago (`lastJoinMs`: the
 * attempt stamp, 0 = never) AND its station still running. A join whose station has since been
 * stopped - a hotspot's mode(AP), a game's disconnect(true), disable() - is not in flight, and
 * calling it young only made the restore come up idle and wait for the loop's next retry (20 s,
 * or 3 min after five failures) where a begin() now is right (0.9.79 review). Wrap-safe. */
static inline bool wifiJoinInFlight(bool staRunning, uint32_t now, uint32_t lastJoinMs) {
  return staRunning && lastJoinMs != 0 && (uint32_t)(now - lastJoinMs) < 10000u;
}

static inline WifiRestore wifiRestoreDecision(const WifiRestoreIn& s) {
  if (s.gameActive) {
    return WIFI_RESTORE_OFF;        // the game's exit asks again
  }
  if (s.softApLive) {
    return WIFI_RESTORE_LEAVE;      // the hotspot's teardown asks again
  }
  if (s.radioOff || s.userDisabled || !s.reconnect) {
    return WIFI_RESTORE_OFF;        // the owner said off (or nothing is waiting for the station)
  }
  if (s.staConnected) {
    return WIFI_RESTORE_LEAVE;
  }
  if (s.joinYoung || s.longDrySpell) {
    return WIFI_RESTORE_UP_IDLE;
  }
  return WIFI_RESTORE_JOIN;
}

static inline const char* wifiRestoreName(WifiRestore d) {
  switch (d) {
  case WIFI_RESTORE_JOIN:    return "JOIN";
  case WIFI_RESTORE_UP_IDLE: return "UP_IDLE";
  case WIFI_RESTORE_OFF:     return "OFF";
  case WIFI_RESTORE_LEAVE:   return "LEAVE";
  }
  return "?";
}

#endif // WIFI_POLICY_H
