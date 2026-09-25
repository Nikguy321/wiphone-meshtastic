/* test_wifi_policy.cpp — may the WiFi station come back, and how (WiPhone/wifi_policy.h).
 *
 * WHY THIS IS WORTH A SUITE: the fault it guards is SILENT and it costs battery, not function.
 * A radio brought back behind the owner's "off" still looks off everywhere a person would look
 * (the menu row reads the switch, not the radio), and a station hunting on an erased config
 * prints nothing at the default log level. The only place every combination of the owner's
 * switches and the radio's temporary owners can be checked is here. The functions under test
 * are the REAL ones Networks.cpp's wifiRestoreStation() and the loop's retry gate call.
 *
 * The sweep covers all 2^8 input states against the rules; the named rows are the situations
 * the 2026-09-25 investigation walked through, one per gap it found.
 */
#include <cstdio>
#include <cstring>
#include <cstdint>

#include "../WiPhone/wifi_policy.h"

static int failures = 0;
static int checks = 0;

static void group(const char* name) {
  printf("\n\033[1m%s\033[0m\n", name);
}

static void ok(bool cond, const char* what) {
  checks++;
  if (!cond) {
    failures++;
    printf("  \033[31mFAIL\033[0m %s\n", what);
  } else {
    printf("  ok  %s\n", what);
  }
}

static WifiRestoreIn fromMask(unsigned m) {
  WifiRestoreIn s;
  s.radioOff     = (m & 0x01) != 0;
  s.userDisabled = (m & 0x02) != 0;
  s.reconnect    = (m & 0x04) != 0;
  s.gameActive   = (m & 0x08) != 0;
  s.softApLive   = (m & 0x10) != 0;
  s.staConnected = (m & 0x20) != 0;
  s.joinYoung    = (m & 0x40) != 0;
  s.longDrySpell = (m & 0x80) != 0;
  return s;
}

/* One rule, checked in every one of the 256 states; one printed line per rule, and the mask of
 * any state that breaks it. */
typedef bool (*Rule)(const WifiRestoreIn& s, WifiRestore d);

static void sweep(const char* what, Rule rule) {
  int bad = 0;
  for (unsigned m = 0; m < 256; m++) {
    const WifiRestoreIn s = fromMask(m);
    checks++;
    if (!rule(s, wifiRestoreDecision(s))) {
      failures++;
      if (bad++ < 4) {
        printf("  \033[31mFAIL\033[0m %s  (state 0x%02x -> %s)\n", what, m,
               wifiRestoreName(wifiRestoreDecision(s)));
      }
    }
  }
  if (!bad) {
    printf("  ok  %s  (all 256 states)\n", what);
  }
}

static bool up(WifiRestore d) {
  return d == WIFI_RESTORE_JOIN || d == WIFI_RESTORE_UP_IDLE;
}

static WifiRestoreIn base() {     // WiFi on, a saved network, nobody else holding the radio
  WifiRestoreIn s;
  memset(&s, 0, sizeof(s));
  s.reconnect = true;
  return s;
}

int main() {
  group("the sweep: every state of the owner's switches and the radio's temporary owners");
  sweep("the station never comes up when the owner said off (radioOff)",
        [](const WifiRestoreIn& s, WifiRestore d) { return !(s.radioOff && up(d)); });
  sweep("...nor when the network is disabled (userDisabled: Disconnect, Forget, none saved)",
        [](const WifiRestoreIn& s, WifiRestore d) { return !(s.userDisabled && up(d)); });
  sweep("...and radioOff is asked in its own right: userDisabled clear does not let it up",
        [](const WifiRestoreIn& s, WifiRestore d) {
          return !(s.radioOff && !s.userDisabled && up(d));
        });
  sweep("under a Game Boy game the answer is OFF, whatever else is true",
        [](const WifiRestoreIn& s, WifiRestore d) { return !s.gameActive || d == WIFI_RESTORE_OFF; });
  sweep("a live hotspot (no game) is LEFT to its owner - never OFF, never a station join",
        [](const WifiRestoreIn& s, WifiRestore d) {
          return s.gameActive || !s.softApLive || d == WIFI_RESTORE_LEAVE;
        });
  sweep("JOIN only when every input is clear (reconnect armed, nothing else set)",
        [](const WifiRestoreIn& s, WifiRestore d) {
          const bool clear = !s.radioOff && !s.userDisabled && s.reconnect && !s.gameActive &&
                             !s.softApLive && !s.staConnected && !s.joinYoung && !s.longDrySpell;
          return (d == WIFI_RESTORE_JOIN) == clear;
        });
  sweep("disconnect() disarmed the retry (!reconnect): the station is not brought up",
        [](const WifiRestoreIn& s, WifiRestore d) { return s.reconnect || !up(d); });
  sweep("an association already up is never re-begun (LEAVE, or OFF if the owner said off)",
        [](const WifiRestoreIn& s, WifiRestore d) {
          return !s.staConnected || d == WIFI_RESTORE_LEAVE || d == WIFI_RESTORE_OFF;
        });
  sweep("a join younger than 10 s is never re-begun under itself",
        [](const WifiRestoreIn& s, WifiRestore d) { return !s.joinYoung || d != WIFI_RESTORE_JOIN; });
  sweep("a long dry spell leaves the join to the loop's gated retry (never a restore JOIN)",
        [](const WifiRestoreIn& s, WifiRestore d) { return !s.longDrySpell || d != WIFI_RESTORE_JOIN; });
  sweep("the restore and the loop's retry agree: up => the retry gate's predicate is true",
        [](const WifiRestoreIn& s, WifiRestore d) {
          return !up(d) || wifiStationWantedFrom(s.radioOff, s.userDisabled, s.gameActive,
                                                 s.softApLive);
        });
  sweep("...and not wanted => OFF or LEAVE, nothing that starts the station",
        [](const WifiRestoreIn& s, WifiRestore d) {
          return wifiStationWantedFrom(s.radioOff, s.userDisabled, s.gameActive, s.softApLive) ||
                 d == WIFI_RESTORE_OFF || d == WIFI_RESTORE_LEAVE;
        });
  sweep("every answer has a name for the log line",
        [](const WifiRestoreIn& s, WifiRestore d) { return strcmp(wifiRestoreName(d), "?") != 0; });

  group("wifiStationWantedFrom: the loop's retry gate");
  {
    int bad = 0;
    for (unsigned m = 0; m < 16; m++) {
      const bool ro = (m & 1) != 0, ud = (m & 2) != 0, game = (m & 4) != 0, ap = (m & 8) != 0;
      checks++;
      if (wifiStationWantedFrom(ro, ud, game, ap) != (m == 0)) {
        failures++;
        bad++;
        printf("  \033[31mFAIL\033[0m wanted(ro=%d ud=%d game=%d ap=%d)\n", ro, ud, game, ap);
      }
    }
    if (!bad) {
      printf("  ok  wanted only with all four clear (16 states)\n");
    }
  }
  ok(!wifiStationWantedFrom(false, false, true, false),
     "G2: a game running = no join retry (it used to WiFi.begin() under the emulator)");
  ok(!wifiStationWantedFrom(true, false, false, false),
     "G5: WiFi off with the per-network flag cleared by Edit > Save = no join retry");

  group("the situations the investigation walked through");
  {
    WifiRestoreIn s = base();
    s.userDisabled = true;       // Networks::disable(): no saved network at boot / Disconnect / Forget
    s.reconnect = false;         // ...which runs disconnect() (reconnect=false, config erased)
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_OFF,
       "G1: a Disconnected phone's Game Boy exit -> OFF (was STA + reconnect() on an erased config)");
  }
  {
    WifiRestoreIn s = base();
    s.radioOff = true;
    s.userDisabled = true;
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_OFF, "the woods: WiFi off, a sync window closes -> OFF");
  }
  {
    WifiRestoreIn s = base();
    s.radioOff = true;           // the switch still says off...
    s.userDisabled = false;      // ...but loadPreferred() cleared this from the INI (G5)
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_OFF, "G5: switch off, per-network flag cleared -> OFF");
  }
  {
    WifiRestoreIn s = base();
    s.longDrySpell = true;
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_UP_IDLE,
       "G9: the car, 5+ min with no network, a window closes -> UP_IDLE (the loop's gated retry joins)");
  }
  {
    WifiRestoreIn s = base();
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_JOIN, "home, after a game -> JOIN");
  }
  {
    WifiRestoreIn s = base();
    s.gameActive = true;
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_OFF,
       "a window or uploader closes UNDER a game -> OFF (the game's exit restores)");
    s.softApLive = true;
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_OFF, "...even with a hotspot flag still set");
  }
  {
    WifiRestoreIn s = base();
    s.softApLive = true;
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_LEAVE,
       "Settings: WiFi on while a headless uploader hosts its hotspot -> LEAVE (its stop restores)");
  }
  {
    WifiRestoreIn s = base();
    s.staConnected = true;
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_LEAVE,
       "left Settings > WiFi having joined a network -> LEAVE");
  }
  {
    WifiRestoreIn s = base();
    s.joinYoung = true;
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_UP_IDLE,
       "a second restore within 10 s of the first's begin() -> UP_IDLE, no second begin()");
  }
  {
    WifiRestoreIn s = base();
    s.reconnect = false;         // standalone Edit network > Save: disconnect(), nothing re-arms
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_OFF,
       "after a standalone Edit > Save (retry disarmed) -> OFF, not a join on an erased config");
  }

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
