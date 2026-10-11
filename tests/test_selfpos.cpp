/*
 * test_selfpos.cpp — self_pos.cpp: which source this phone believes, and the phone-over-WiFi
 * stream's gate, stale guards, port search and words. Fed through the SHIPPING NMEA reader
 * (nmea.cpp) and quality bar (mesh_pos.cpp), so a pass proves the bytes a relay app sends are
 * judged the way the phone will judge them.
 *
 * Dummy coordinates only (40.0,-100.0): this repo is public. The checksums of the sentences
 * built here come from ck() below; the checksum rule itself, and the parser's coordinates, are
 * proven against Python-computed literals in test_nmea.cpp.
 */

#include "../WiPhone/self_pos.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, name) do { \
    checks++; \
    if (cond) { printf("  ok  %s\n", name); } \
    else { printf("  FAIL %s (line %d)\n", name, __LINE__); failures++; } \
  } while (0)

static const int32_t D_LAT = 400000000, D_LON = -1000000000;   // 40.0, -100.0

// ---------------------------------------------------------------- sentence builders
static void ck(char* out, size_t cap, const char* body, bool crlf) {
  unsigned x = 0;
  for (const char* p = body; *p; p++) {
    x ^= (unsigned char)*p;
  }
  snprintf(out, cap, "$%s*%02X%s", body, x, crlf ? "\r\n" : "");
}

/* A GGA at 40.0,-100.0. Every field a string, so "" is an EMPTY field. */
static const char* gga(const char* hms, const char* q, const char* sats, const char* hdop,
                       bool crlf = true) {
  static char buf[4][160];
  static int k = 0;
  char body[140];
  snprintf(body, sizeof(body), "GPGGA,%s,4000.00000,N,10000.00000,W,%s,%s,%s,100.0,M,,M,,",
           hms, q, sats, hdop);
  char* out = buf[k++ & 3];
  ck(out, 160, body, crlf);
  return out;
}

static const char* rmc(const char* hms, const char* status, const char* mode, bool crlf = true) {
  static char buf[4][160];
  static int k = 0;
  char body[140];
  snprintf(body, sizeof(body), "GPRMC,%s,%s,4000.00000,N,10000.00000,W,0.0,,101026,,,%s",
           hms, status, mode);
  char* out = buf[k++ & 3];
  ck(out, 160, body, crlf);
  return out;
}

static bool feedS(SelfPhoneStream* s, const char* text, uint32_t now, int32_t sod, SelfPhoneOut* o) {
  memset(o, 0, sizeof(*o));
  return selfPhoneFeed(s, (const uint8_t*)text, strlen(text), now, sod, o);
}

/* A stream already past its settle window, started at t = 1000. */
static void live(SelfPhoneStream* s) {
  selfPhoneInit(s);
  selfPhoneBegin(s, 1000);
}
static const uint32_t T = 1000 + SELF_PHONE_SETTLE_MS;     // the first settled millisecond

/* #define NAME <number | SELF_*_FRESH_MS> in a header this suite cannot compile. */
static long readDefine(const char* path, const char* name) {
  FILE* f = fopen(path, "r");
  if (!f) {
    return -2;
  }
  char line[512];
  long v = -1;
  const size_t nl = strlen(name);
  while (fgets(line, sizeof(line), f)) {
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "#define", 7) != 0) continue;
    p += 7;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, name, nl) != 0 || (p[nl] != ' ' && p[nl] != '\t')) continue;
    p += nl;
    while (*p == ' ' || *p == '\t') p++;
    if (*p >= '0' && *p <= '9') {
      v = strtol(p, NULL, 10);
    } else if (strncmp(p, "SELF_SHOW_FRESH_MS", 18) == 0) {
      v = (long)SELF_SHOW_FRESH_MS;
    } else if (strncmp(p, "SELF_TX_FRESH_MS", 16) == 0) {
      v = (long)SELF_TX_FRESH_MS;
    }
    break;
  }
  fclose(f);
  return v;
}

int main() {
  printf("test_selfpos\n");

  // ---- the freshness bars are the GPS's own, and cannot drift from them ---------------------
  {
    CHECK(readDefine("WiPhone/meshtastic_service.h", "MESH_GPS_FRESH_MS") == (long)SELF_SHOW_FRESH_MS,
          "SELF_SHOW_FRESH_MS == meshtastic_service.h's MESH_GPS_FRESH_MS (120 s)");
    CHECK(readDefine("WiPhone/meshtastic_service.h", "MESH_POS_TX_FRESH_MS") == (long)SELF_TX_FRESH_MS,
          "SELF_TX_FRESH_MS == meshtastic_service.h's MESH_POS_TX_FRESH_MS (30 s)");
  }

  // ---- slots ---------------------------------------------------------------------------------
  {
    SelfSlot s;
    memset(&s, 0, sizeof(s));
    s.enabled = true;
    selfSlotSet(&s, D_LAT, D_LON, -7, -3, 0, false);
    CHECK(s.fixMs == 1 && s.sats == -1 && s.hdopX10 == -1,
          "a stamp of millis()==0 is kept as 1; negative quality reads as unknown");
    selfSlotClear(&s);
    CHECK(s.fixMs == 0 && s.enabled && s.sats == -1 && !s.injected, "clear forgets the fix, keeps 'enabled'");
  }

  // ---- the priority matrix -------------------------------------------------------------------
  {
    SelfSlot sl[SELF_SRC_COUNT];
    memset(sl, 0, sizeof(sl));
    for (int i = 0; i < SELF_SRC_COUNT; i++) selfSlotClear(&sl[i]);
    SelfSlot& gps = sl[SELF_GPS - 1];
    SelfSlot& ph = sl[SELF_PHONE - 1];
    SelfSlot& net = sl[SELF_NET - 1];
    const uint32_t now = 500000;
    SelfFix fx;

    CHECK(selfPick(sl, now, 0, &fx) == SELF_NONE && fx.src == SELF_NONE, "nothing on: nothing picked");
    CHECK(!selfAnyEnabled(sl), "nothing on: no source enabled");

    gps.enabled = ph.enabled = true;
    selfSlotSet(&gps, D_LAT, D_LON, 9, 12, now - 1000, false);
    selfSlotSet(&ph, D_LAT + 100, D_LON, 8, 9, now - 500, false);
    CHECK(selfPick(sl, now, SELF_PICK_TX, &fx) == SELF_GPS && fx.latI == D_LAT &&
          fx.locSrc == MESH_LOC_INTERNAL && fx.precBits == 32 && fx.ageMs == 1000,
          "both live: the woods plate's GPS wins (INTERNAL, 32 bits)");

    gps.enabled = false;
    CHECK(selfPick(sl, now, SELF_PICK_TX, &fx) == SELF_PHONE && fx.latI == D_LAT + 100 &&
          fx.locSrc == MESH_LOC_EXTERNAL && fx.precBits == 32,
          "GPS switched off: the phone (EXTERNAL, 32 bits)");
    gps.enabled = true;

    selfSlotSet(&gps, D_LAT, D_LON, 3, 64, now - 1000, false);
    CHECK(selfSlotState(&gps, SELF_GPS, now, SELF_PICK_TX) == SELF_ST_POOR &&
          selfPick(sl, now, SELF_PICK_TX, &fx) == SELF_PHONE,
          "a POOR GPS fix (3 sats) ranks below a live phone fix");

    selfSlotSet(&gps, D_LAT, D_LON, 9, 12, now - 40000, false);
    CHECK(selfPick(sl, now, SELF_PICK_TX, &fx) == SELF_PHONE &&
          selfPick(sl, now, 0, &fx) == SELF_GPS,
          "a 40 s GPS fix: too old to send (phone sent), fresh enough to show (GPS shown)");

    selfSlotClear(&gps);
    CHECK(selfSlotState(&gps, SELF_GPS, now, 0) == SELF_ST_NEVER && selfPick(sl, now, 0, &fx) == SELF_PHONE,
          "GPS on but never fixed: the phone");

    /* NET (Wi-Fi location, 0.9.82): ranked LAST. A slot written the GNSS way (selfSlotSet, no
     * accuracy) is not a Wi-Fi fix: POOR, never picked. */
    selfSlotClear(&ph);
    net.enabled = true;
    selfSlotSet(&net, D_LAT, D_LON, 12, 5, now - 10, false);
    CHECK(selfSlotState(&net, SELF_NET, now, 0) == SELF_ST_POOR &&
          selfPick(sl, now, SELF_PICK_INJECTED, &fx) == SELF_NONE,
          "NET written without an accuracy (selfSlotSet): POOR, never picked");
    selfNetSet(&net, D_LAT + 300, D_LON, 60, now - 10, 0, false);
    gps.enabled = true;
    selfSlotSet(&gps, D_LAT, D_LON, 9, 12, now - 1000, false);
    CHECK(selfPick(sl, now, 0, &fx) == SELF_GPS, "GPS live and NET live: GPS");
    ph.enabled = true;
    gps.enabled = false;
    selfSlotSet(&ph, D_LAT + 100, D_LON, 8, 9, now - 500, false);
    CHECK(selfPick(sl, now, 0, &fx) == SELF_PHONE, "phone live and NET live: the phone");
    selfSlotClear(&ph);
    CHECK(selfPick(sl, now, 0, &fx) == SELF_NET && fx.latI == D_LAT + 300 && fx.accM == 60 &&
          fx.locSrc == MESH_LOC_UNSET && fx.precBits == SELF_NET_PREC_BITS,
          "nothing measured: the Wi-Fi fix is shown (UNSET, 17 bits, its accuracy)");
    ph.enabled = false;
    CHECK(selfAnyEnabled(sl) && selfAnyEnabledForTx(sl),
          "NET alone ('map + mesh'): a source for the screens and for sending");
    net.noTx = true;
    CHECK(selfAnyEnabled(sl) && !selfAnyEnabledForTx(sl),
          "NET alone, 'map only': a source for the screens, NOT for sending (review item 16)");
    net.enabled = false;
    CHECK(!selfAnyEnabled(sl) && !selfAnyEnabledForTx(sl), "everything off: nothing either way");
  }

  // ---- freshness edges and millis() wrap -----------------------------------------------------
  {
    SelfSlot s;
    selfSlotClear(&s);
    s.enabled = true;
    selfSlotSet(&s, D_LAT, D_LON, 8, 10, 1000000, false);
    CHECK(selfSlotState(&s, SELF_PHONE, 1000000 + 119999, 0) == SELF_ST_LIVE &&
          selfSlotState(&s, SELF_PHONE, 1000000 + 120000, 0) == SELF_ST_OLD,
          "shown: 119.999 s live, 120.000 s old");
    CHECK(selfSlotState(&s, SELF_PHONE, 1000000 + 29999, SELF_PICK_TX) == SELF_ST_LIVE &&
          selfSlotState(&s, SELF_PHONE, 1000000 + 30000, SELF_PICK_TX) == SELF_ST_OLD,
          "sent: 29.999 s live, 30.000 s old");
    selfSlotSet(&s, D_LAT, D_LON, 8, 10, 0xFFFFF000u, false);
    CHECK(selfSlotState(&s, SELF_PHONE, 0x00001000u, SELF_PICK_TX) == SELF_ST_LIVE &&
          selfSlotState(&s, SELF_PHONE, (uint32_t)(0xFFFFF000u + SELF_TX_FRESH_MS), SELF_PICK_TX) == SELF_ST_OLD,
          "millis() wrap: 8 s across the wrap is live, 30 s across it old");
    /* The phone bar: unknown is good enough to SHOW (the gate vouched with an RMC), never to
     * SEND (review 2026-10-10: GPS 2 IP's position may be CoreLocation's Wi-Fi/cell guess, or
     * fuzzed by Precise Location off); measured bad is poor either way. */
    selfSlotSet(&s, D_LAT, D_LON, -1, -1, 5000, false);
    CHECK(selfSlotState(&s, SELF_PHONE, 6000, 0) == SELF_ST_LIVE &&
          selfSlotState(&s, SELF_PHONE, 6000, SELF_PICK_TX) == SELF_ST_UNRATED,
          "no sats/HDOP in a phone slot: live on the screens, UNRATED for sending");
    selfSlotSet(&s, D_LAT, D_LON, -1, 12, 5000, false);
    CHECK(selfSlotState(&s, SELF_PHONE, 6000, SELF_PICK_TX) == SELF_ST_LIVE, "HDOP alone said: sendable");
    selfSlotSet(&s, D_LAT, D_LON, 7, -1, 5000, false);
    CHECK(selfSlotState(&s, SELF_PHONE, 6000, SELF_PICK_TX) == SELF_ST_LIVE, "sats alone said: sendable");
    selfSlotSet(&s, D_LAT, D_LON, -1, -1, 5000, false);
    CHECK(selfSlotState(&s, SELF_GPS, 6000, SELF_PICK_TX) == SELF_ST_LIVE,
          "the woods plate's receiver that has not said is not refused (its house rule)");
    selfSlotSet(&s, D_LAT, D_LON, -1, -1, 5000, true);
    CHECK(selfSlotState(&s, SELF_PHONE, 6000, SELF_PICK_TX | SELF_PICK_INJECTED) == SELF_ST_LIVE,
          "a bench phone fix with no numbers: `pos now` may send it (as MANUAL, below)");
    selfSlotSet(&s, D_LAT, D_LON, 9, 101, 5000, false);
    CHECK(selfSlotState(&s, SELF_PHONE, 6000, SELF_PICK_TX) == SELF_ST_POOR, "HDOP 10.1: poor");
    // ...and an UNRATED phone fix next to nothing else: nothing for the schedule.
    SelfSlot sl[SELF_SRC_COUNT];
    for (int i = 0; i < SELF_SRC_COUNT; i++) { selfSlotClear(&sl[i]); sl[i].enabled = true; }
    selfSlotSet(&sl[SELF_PHONE - 1], D_LAT, D_LON, -1, -1, 5000, false);
    SelfFix fx;
    CHECK(selfPick(sl, 6000, SELF_PICK_TX, &fx) == SELF_NONE && selfPick(sl, 6000, 0, &fx) == SELF_PHONE &&
          selfNewest(sl, 6000, SELF_PICK_TX, &fx) == SELF_PHONE && fx.state == SELF_ST_UNRATED,
          "unrated: never picked to send, shown, and newest says why");
  }

  // ---- injected (bench) fixes: shown, never sent by the schedule -----------------------------
  {
    SelfSlot sl[SELF_SRC_COUNT];
    for (int i = 0; i < SELF_SRC_COUNT; i++) { selfSlotClear(&sl[i]); sl[i].enabled = false; }
    sl[SELF_PHONE - 1].enabled = true;
    selfSlotSet(&sl[SELF_PHONE - 1], D_LAT, D_LON, 9, 9, 100000, true);
    SelfFix fx;
    CHECK(selfPick(sl, 100500, SELF_PICK_TX, &fx) == SELF_NONE &&
          selfSlotState(&sl[SELF_PHONE - 1], SELF_PHONE, 100500, SELF_PICK_TX) == SELF_ST_INJECTED,
          "the scheduled beacon never picks an injected fix");
    CHECK(selfPick(sl, 100500, SELF_PICK_TX | SELF_PICK_INJECTED, &fx) == SELF_PHONE && fx.injected,
          "'pos now' may, and the pick says it is injected");
    /* Review 2026-10-10: a typed position must not reach COVEY as EXTERNAL/32 - a live
     * phone-GPS claim stock receivers take the packet's time on. MANUAL, field 23 left out. */
    CHECK(fx.locSrc == MESH_LOC_MANUAL && fx.precBits == 0, "a bench fix is sent as MANUAL, no precision_bits");
    CHECK(selfSource(sl, SELF_PHONE, 100500, SELF_PICK_INJECTED, &fx) && fx.injected &&
          fx.locSrc == MESH_LOC_MANUAL && fx.state == SELF_ST_LIVE, "selfSource: the same fields");
    CHECK(selfPick(sl, 100500, SELF_PICK_INJECTED, &fx) == SELF_PHONE, "the screens show it");
    // An injected GPS fix does not hide a real phone fix from the schedule.
    sl[SELF_GPS - 1].enabled = true;
    selfSlotSet(&sl[SELF_GPS - 1], D_LAT, D_LON, 9, 9, 100000, true);
    selfSlotSet(&sl[SELF_PHONE - 1], D_LAT + 7, D_LON, 9, 9, 100000, false);
    CHECK(selfPick(sl, 100500, SELF_PICK_TX, &fx) == SELF_PHONE && !fx.injected &&
          fx.locSrc == MESH_LOC_EXTERNAL && fx.precBits == 32,
          "injected GPS + real phone: the schedule sends the phone's (EXTERNAL, 32)");
  }

  // ---- one source's own fix (`pos`, the Almanac's step 2) ------------------------------------
  {
    SelfSlot sl[SELF_SRC_COUNT];
    for (int i = 0; i < SELF_SRC_COUNT; i++) { selfSlotClear(&sl[i]); sl[i].enabled = false; }
    SelfFix fx;
    CHECK(!selfSource(sl, SELF_PHONE, 1000, 0, &fx) && fx.src == SELF_NONE, "off: none");
    sl[SELF_PHONE - 1].enabled = true;
    CHECK(!selfSource(sl, SELF_PHONE, 1000, 0, &fx), "on, never fixed: none");
    selfSlotSet(&sl[SELF_PHONE - 1], D_LAT, D_LON, 3, 64, 1000, false);
    CHECK(selfSource(sl, SELF_PHONE, 500000, 0, &fx) && fx.state == SELF_ST_OLD && fx.sats == 3 &&
          fx.ageMs == 499000 && fx.locSrc == MESH_LOC_EXTERNAL, "old and poor: still that source's fix");
    sl[SELF_NET - 1].enabled = true;
    CHECK(!selfSource(sl, SELF_NET, 1000, 0, &fx), "NET on, never fixed: none");
    selfNetSet(&sl[SELF_NET - 1], D_LAT, D_LON, 70, 1000, 0, false);
    CHECK(selfSource(sl, SELF_NET, 1000 + SELF_NET_SHOW_MS, 0, &fx) && fx.state == SELF_ST_OLD &&
          fx.accM == 70, "NET: its own fix, OLD past the 12-minute bar (the `pos` line still sees it)");
    CHECK(!selfSource(sl, SELF_NONE, 1000, 0, &fx) && !selfSource(sl, 4, 1000, 0, &fx),
          "NONE and out of range: never");
  }

  // ---- the newest fix (grey "last phone" marker) ---------------------------------------------
  {
    SelfSlot sl[SELF_SRC_COUNT];
    for (int i = 0; i < SELF_SRC_COUNT; i++) { selfSlotClear(&sl[i]); sl[i].enabled = true; }
    const uint32_t now = 1000000;
    selfSlotSet(&sl[SELF_GPS - 1], D_LAT, D_LON, 9, 9, now - 200000, false);
    selfSlotSet(&sl[SELF_PHONE - 1], D_LAT + 1, D_LON, 9, 9, now - 150000, false);
    SelfFix fx;
    CHECK(selfPick(sl, now, 0, &fx) == SELF_NONE, "both past 120 s: nothing live");
    CHECK(selfNewest(sl, now, 0, &fx) == SELF_PHONE && fx.state == SELF_ST_OLD && fx.ageMs == 150000,
          "newest: the phone's (150 s), state OLD");
    selfSlotSet(&sl[SELF_GPS - 1], D_LAT, D_LON, 9, 9, now - 150000, false);
    CHECK(selfNewest(sl, now, 0, &fx) == SELF_GPS, "a tie keeps the better rank (GPS)");
    sl[SELF_GPS - 1].enabled = false;
    sl[SELF_PHONE - 1].injected = true;
    CHECK(selfNewest(sl, now, 0, &fx) == SELF_NONE && selfNewest(sl, now, SELF_PICK_INJECTED, &fx) == SELF_PHONE,
          "a switched-off source and (without the flag) an injected one are skipped");
  }

  // ---- labels --------------------------------------------------------------------------------
  {
    CHECK(!strcmp(selfMapLabel(SELF_GPS), "me") && !strcmp(selfMapLabel(SELF_PHONE), "me (phone)"),
          "map: 'me' and 'me (phone)'");
    CHECK(!strcmp(selfSrcName(SELF_PHONE), "phone") && !strcmp(selfSrcLastName(SELF_PHONE), "last phone") &&
          !strcmp(selfSrcName(SELF_GPS), "GPS") && !strcmp(selfSrcLastName(SELF_GPS), "last GPS"),
          "source names");
    CHECK(!strcmp(selfBeaconTag(SELF_PHONE), "phone beacon") && !strcmp(selfBeaconTag(SELF_GPS), "gps beacon"),
          "the log's provenance");
    CHECK(selfSrcLocSource(SELF_NET) == MESH_LOC_UNSET && selfSrcPrecBits(SELF_NET) == 17 &&
          selfSrcLocSource(SELF_NONE) == MESH_LOC_UNSET && selfSrcPrecBits(SELF_NONE) == 0,
          "only GNSS sources claim a measurement; Wi-Fi is a 17-bit cell");
    CHECK(!strcmp(selfStateName(SELF_ST_POOR), "poor fix") && !strcmp(selfStateName(SELF_ST_OLD), "too old"),
          "state names");
  }

  // ---- the Wi-Fi slot (SELF_NET): its clock is the scans ---------------------------------
  {
    SelfSlot n;
    memset(&n, 0, sizeof(n));
    selfSlotClear(&n);
    selfNetSet(&n, D_LAT, D_LON, 60, 5000, 1760000000u, false);
    CHECK(n.fixMs == 0, "a switched-off Wi-Fi slot keeps nothing (setting turned off mid-lookup)");
    n.enabled = true;
    n.noTx = true;
    selfNetSet(&n, D_LAT, D_LON, 60, 0, 1760000000u, false);
    CHECK(n.fixMs == 1 && n.firstMs == 1 && n.accM == 60 && n.scanUtc == 1760000000u &&
          n.sats == -1 && n.hdopX10 == -1 && !n.injected,
          "selfNetSet: as of its scan (millis 0 kept as 1), no satellites, the accuracy and UTC kept");
    selfSlotClear(&n);
    CHECK(n.fixMs == 0 && n.accM == 0 && n.firstMs == 0 && n.scanUtc == 0 && n.enabled && n.noTx,
          "clear forgets the fix and its scan stamps, keeps 'enabled' and 'map only'");
    selfNetSet(&n, D_LAT, D_LON, 60, 5000, 7, false);
    selfSlotSet(&n, D_LAT, D_LON, 9, 9, 6000, false);
    CHECK(n.accM == 0 && n.firstMs == 0 && n.scanUtc == 0,
          "a GNSS-style write (selfSlotSet) leaves no Wi-Fi accuracy behind");
  }
  {
    SelfSlot n;
    memset(&n, 0, sizeof(n));
    selfSlotClear(&n);
    n.enabled = true;
    const uint32_t t0 = 1000000;
    selfNetSet(&n, D_LAT, D_LON, 60, t0, 100, false);
    // shown: 12 minutes from the LATEST confirming scan
    CHECK(selfSlotState(&n, SELF_NET, t0 + SELF_NET_SHOW_MS - 1, 0) == SELF_ST_LIVE &&
          selfSlotState(&n, SELF_NET, t0 + SELF_NET_SHOW_MS, 0) == SELF_ST_OLD,
          "shown: 11:59.999 after its scan live, 12:00 old (gone)");
    CHECK(!selfNetConfirm(&n, t0, 101) && n.scanUtc == 100, "the same scan twice confirms nothing");
    CHECK(!selfNetConfirm(&n, t0 - 5, 99) && n.fixMs == t0, "an older scan confirms nothing");
    CHECK(selfNetConfirm(&n, t0 + 600000, 700) && n.fixMs == t0 + 600000 && n.scanUtc == 700 &&
          n.firstMs == t0, "a later scan confirms: it becomes the clock and the time; the first stays");
    CHECK(selfSlotState(&n, SELF_NET, t0 + 600000 + SELF_NET_SHOW_MS - 1, 0) == SELF_ST_LIVE,
          "...and the 12 minutes run from it");
    SelfSlot e;
    memset(&e, 0, sizeof(e));
    selfSlotClear(&e);
    e.enabled = true;
    CHECK(!selfNetConfirm(&e, t0, 1) && e.fixMs == 0, "confirming an empty slot writes nothing");
    // the cadence's scan late: ask for one before the fix expires (a scan, not a lookup)
    const uint32_t c = t0 + 600000;
    CHECK(!selfNetWantsRefresh(&n, c + SELF_NET_REFRESH_AT_MS - 1) && selfNetWantsRefresh(&n, c + SELF_NET_REFRESH_AT_MS) &&
          selfNetWantsRefresh(&n, c + SELF_NET_SHOW_MS - 1) && !selfNetWantsRefresh(&n, c + SELF_NET_SHOW_MS),
          "10:30 after its last scan with none since: a scan is asked for; after 12:00 it is gone, no rescue");
    CHECK(SELF_NET_REFRESH_AT_MS > 600000UL && SELF_NET_REFRESH_AT_MS + 60000UL < SELF_NET_SHOW_MS,
          "the refresh waits out the switcher's own 10-minute scan, and leaves a minute and more for one");
    CHECK(!selfNetWantsRefresh(&e, c + SELF_NET_REFRESH_AT_MS) && !selfNetWantsRefresh(NULL, 5), "no fix: no scan");
    n.enabled = false;
    CHECK(!selfNetWantsRefresh(&n, c + SELF_NET_REFRESH_AT_MS), "switched off: no scan");
    n.enabled = true;
  }
  // sending: map only, two scans 60 s apart, a confirming scan within 60 s, the accuracy bar
  {
    SelfSlot n;
    memset(&n, 0, sizeof(n));
    selfSlotClear(&n);
    n.enabled = true;
    const uint32_t t0 = 2000000;
    selfNetSet(&n, D_LAT, D_LON, 60, t0, 100, false);
    CHECK(selfSlotState(&n, SELF_NET, t0 + 1000, 0) == SELF_ST_LIVE &&
          selfSlotState(&n, SELF_NET, t0 + 1000, SELF_PICK_TX) == SELF_ST_SETTLING,
          "one scan: shown, and NOT sent (a fix taken while moving never goes out)");
    selfNetConfirm(&n, t0 + SELF_NET_SETTLE_MS - 1, 101);
    CHECK(selfSlotState(&n, SELF_NET, t0 + SELF_NET_SETTLE_MS, SELF_PICK_TX) == SELF_ST_SETTLING,
          "a second scan 59.999 s on: still settling");
    selfNetConfirm(&n, t0 + SELF_NET_SETTLE_MS, 102);
    CHECK(selfSlotState(&n, SELF_NET, t0 + SELF_NET_SETTLE_MS, SELF_PICK_TX) == SELF_ST_LIVE,
          "a second agreeing scan 60.000 s on: sendable");
    const uint32_t c = t0 + SELF_NET_SETTLE_MS;
    CHECK(selfSlotState(&n, SELF_NET, c + SELF_NET_CONFIRM_MS, SELF_PICK_TX) == SELF_ST_LIVE &&
          selfSlotState(&n, SELF_NET, c + SELF_NET_CONFIRM_MS + 1, SELF_PICK_TX) == SELF_ST_UNCONFIRMED &&
          selfSlotState(&n, SELF_NET, c + SELF_NET_CONFIRM_MS + 1, 0) == SELF_ST_LIVE,
          "sent within 60.000 s of its confirming scan; 60.001 s on it needs a fresh scan (still shown)");
    n.noTx = true;
    CHECK(selfSlotState(&n, SELF_NET, c + 1, SELF_PICK_TX) == SELF_ST_MAP_ONLY &&
          selfSlotState(&n, SELF_NET, c + 1, SELF_PICK_TX | SELF_PICK_INJECTED) == SELF_ST_MAP_ONLY &&
          selfSlotState(&n, SELF_NET, c + SELF_NET_SHOW_MS + 5, SELF_PICK_TX) == SELF_ST_MAP_ONLY &&
          selfSlotState(&n, SELF_NET, c + 1, 0) == SELF_ST_LIVE,
          "'map only': never sent, `pos now` included, whatever its age; shown on the screens");
    n.noTx = false;
    n.accM = 0;
    CHECK(selfSlotState(&n, SELF_NET, c + 1, 0) == SELF_ST_POOR, "accuracy 0: not a fix (POOR)");
    n.accM = SELF_NET_MAX_ACC_M + 1;
    CHECK(selfSlotState(&n, SELF_NET, c + 1, 0) == SELF_ST_POOR, "accuracy 101 m: POOR");
    n.accM = SELF_NET_MAX_ACC_M;
    CHECK(selfSlotState(&n, SELF_NET, c + 1, SELF_PICK_TX) == SELF_ST_LIVE, "accuracy 100 m: live");
    n.accM = 1;
    CHECK(selfSlotState(&n, SELF_NET, c + 1, SELF_PICK_TX) == SELF_ST_LIVE, "accuracy 1 m: live");
    // across the millis() wrap
    SelfSlot w;
    memset(&w, 0, sizeof(w));
    selfSlotClear(&w);
    w.enabled = true;
    selfNetSet(&w, D_LAT, D_LON, 50, 0xFFFFC000u, 1, false);
    CHECK(selfNetConfirm(&w, 0x0000C000u, 2) &&
          selfSlotState(&w, SELF_NET, 0x0000D000u, SELF_PICK_TX) == SELF_ST_LIVE,
          "millis() wrap: a confirming scan 64 s later, across the wrap, settles and sends");
  }
  // the scan an owed beacon slot asks for
  {
    SelfSlot n;
    memset(&n, 0, sizeof(n));
    selfSlotClear(&n);
    n.enabled = true;
    CHECK(!selfNetScanUseful(&n, 5000), "no fix: no scan wanted");
    const uint32_t t0 = 3000000;
    selfNetSet(&n, D_LAT, D_LON, 60, t0, 0, false);
    CHECK(!selfNetScanUseful(&n, t0 + SELF_NET_SETTLE_MS - 1),
          "under 60 s since the first scan: a scan now could not settle it");
    CHECK(selfNetScanUseful(&n, t0 + SELF_NET_SETTLE_MS), "60 s on: a scan now would be the second");
    selfNetConfirm(&n, t0 + 70000, 0);
    CHECK(!selfNetScanUseful(&n, t0 + 80000), "settled and confirmed 10 s ago: nothing to add");
    CHECK(selfNetScanUseful(&n, t0 + 70000 + SELF_NET_CONFIRM_MS + 1),
          "settled, confirming scan over 60 s old: a fresh one is wanted");
  }
  // injected (`netloc answer`): shown, never by the schedule, MANUAL but still a 17-bit cell
  {
    SelfSlot sl[SELF_SRC_COUNT];
    for (int i = 0; i < SELF_SRC_COUNT; i++) { selfSlotClear(&sl[i]); sl[i].enabled = false; }
    SelfSlot& net = sl[SELF_NET - 1];
    net.enabled = true;
    const uint32_t t0 = 4000000;
    selfNetSet(&net, D_LAT, D_LON, 80, t0, 0, true);
    selfNetConfirm(&net, t0 + 61000, 0);
    SelfFix fx;
    CHECK(selfPick(sl, t0 + 62000, SELF_PICK_TX, &fx) == SELF_NONE &&
          selfSlotState(&net, SELF_NET, t0 + 62000, SELF_PICK_TX) == SELF_ST_INJECTED,
          "the schedule never sends a bench Wi-Fi fix");
    CHECK(selfPick(sl, t0 + 62000, SELF_PICK_TX | SELF_PICK_INJECTED, &fx) == SELF_NET &&
          fx.injected && fx.locSrc == MESH_LOC_MANUAL && fx.precBits == SELF_NET_PREC_BITS && fx.accM == 80,
          "`pos now` may: MANUAL, and still a 17-bit cell (coarse stays coarse)");
    CHECK(selfPick(sl, t0 + 62000, SELF_PICK_INJECTED, &fx) == SELF_NET, "the screens show it");
  }
  // the pick to SEND, and the newest
  {
    SelfSlot sl[SELF_SRC_COUNT];
    for (int i = 0; i < SELF_SRC_COUNT; i++) { selfSlotClear(&sl[i]); sl[i].enabled = true; }
    SelfSlot& net = sl[SELF_NET - 1];
    const uint32_t t0 = 5000000;
    selfNetSet(&net, D_LAT, D_LON, 60, t0, 1760000100u, false);
    SelfFix fx;
    CHECK(selfPick(sl, t0 + 1000, SELF_PICK_TX, &fx) == SELF_NONE && selfPick(sl, t0 + 1000, 0, &fx) == SELF_NET,
          "one scan: nothing to send, the Wi-Fi fix on the screens");
    CHECK(selfNewest(sl, t0 + 1000, SELF_PICK_TX, &fx) == SELF_NET && fx.state == SELF_ST_SETTLING,
          "the beacon's 'why not' names the Wi-Fi fix: waiting for a second scan");
    selfNetConfirm(&net, t0 + 65000, 1760000165u);
    CHECK(selfPick(sl, t0 + 70000, SELF_PICK_TX, &fx) == SELF_NET && fx.scanUtc == 1760000165u &&
          fx.ageMs == 5000 && fx.locSrc == MESH_LOC_UNSET && fx.precBits == 17,
          "two scans 65 s apart, the last 5 s ago: sent (UNSET, 17 bits), as of that scan");
    CHECK(selfFixSendTime(&fx, 1760000170u) == 1760000165u,
          "field 4 = the confirming scan's UTC, not the send's (review item 2)");
    // a phone fix outranks it even for sending
    selfSlotSet(&sl[SELF_PHONE - 1], D_LAT + 50, D_LON, 9, 9, t0 + 69000, false);
    CHECK(selfPick(sl, t0 + 70000, SELF_PICK_TX, &fx) == SELF_PHONE &&
          selfFixSendTime(&fx, 1760000170u) == 1760000170u,
          "a live phone fix wins; its time is the send's own");
    selfSlotClear(&sl[SELF_PHONE - 1]);
    // expired is gone: no 'last Wi-Fi'
    selfSlotSet(&sl[SELF_GPS - 1], D_LAT, D_LON, 9, 9, t0 - 400000, false);
    CHECK(selfNewest(sl, t0 + 65000 + SELF_NET_SHOW_MS, 0, &fx) == SELF_GPS && fx.state == SELF_ST_OLD,
          "an expired Wi-Fi fix is not the newest - the old GPS fix is (grey 'last GPS')");
    selfSlotClear(&sl[SELF_GPS - 1]);
    CHECK(selfNewest(sl, t0 + 65000 + SELF_NET_SHOW_MS, 0, &fx) == SELF_NONE &&
          selfNewest(sl, t0 + 65000 + SELF_NET_SHOW_MS, SELF_PICK_TX | SELF_PICK_INJECTED, &fx) == SELF_NONE,
          "...and alone, nothing: no grey 'last Wi-Fi' marker, for any purpose");
    selfNetSet(&net, D_LAT, D_LON, 60, 0, 0, false);
    CHECK(selfFixSendTime(NULL, 5) == 0, "no pick: no time");
    SelfFix nf;
    memset(&nf, 0, sizeof(nf));
    nf.src = SELF_NET;
    nf.scanUtc = 0;
    CHECK(selfFixSendTime(&nf, 1760000170u) == 0,
          "a Wi-Fi fix whose scan had no trusted clock: field 4 left out, never the send's time");
  }
  // the words
  {
    SelfFix fx;
    memset(&fx, 0, sizeof(fx));
    char b[40];
    fx.src = SELF_NET;
    fx.accM = 60;
    CHECK(selfFixLabel(b, sizeof(b), &fx) == strlen("me +/-60 m Wi-Fi") && !strcmp(b, "me +/-60 m Wi-Fi"),
          "the map's Wi-Fi label carries its size: 'me +/-60 m Wi-Fi'");
    CHECK(selfFixName(b, sizeof(b), &fx) && !strcmp(b, "Wi-Fi +/-60 m"), "its name: 'Wi-Fi +/-60 m'");
    for (const char* p = b; *p; p++) {
      if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e) { CHECK(false, "printable ASCII only"); break; }
    }
    fx.src = SELF_PHONE;
    CHECK(selfFixLabel(b, sizeof(b), &fx) && !strcmp(b, "me (phone)") &&
          selfFixName(b, sizeof(b), &fx) && !strcmp(b, "phone"), "the phone: 'me (phone)', 'phone'");
    fx.src = SELF_GPS;
    CHECK(selfFixLabel(b, sizeof(b), &fx) && !strcmp(b, "me") && selfFixName(b, sizeof(b), &fx) &&
          !strcmp(b, "GPS"), "the GPS: 'me', 'GPS'");
    fx.src = SELF_NET;
    fx.accM = 100;
    char s[8];
    CHECK(selfFixLabel(s, sizeof(s), &fx) == 7 && !strcmp(s, "me +/-1"), "cut to fit, NUL kept");
    CHECK(selfFixLabel(s, 0, &fx) == 0 && selfFixLabel(NULL, 5, &fx) == 0, "no room: nothing");
    CHECK(strcmp(selfStateName(SELF_ST_MAP_ONLY), "?") && strcmp(selfStateName(SELF_ST_SETTLING), "?") &&
          strcmp(selfStateName(SELF_ST_UNCONFIRMED), "?"), "the new states have names");
  }
  // keep the cell; the precise fix wins
  {
    // a 17-bit cell is 2^15 = 32768 units of 1e-7 deg (~364 m) of latitude
    const int32_t edge = (int32_t)(((uint32_t)D_LAT & 0xFFFF8000u) + 0x8000u);
    const int32_t m15 = 135;                         // ~15 m of latitude in 1e-7 deg
    SelfFix a, b;
    memset(&a, 0, sizeof(a));
    a.src = SELF_NET;
    a.latI = edge - m15;
    a.lonI = D_LON;
    a.accM = 60;
    b = a;
    b.latI = edge + m15;
    SelfNetTxMem m;
    selfNetTxInit(&m);
    int32_t la = 0, lo = 0, lb = 0, lob = 0;
    selfNetTxCell(&m, &a, &la, &lo);
    CHECK(la == meshPosTruncate(a.latI, 17) && lo == meshPosTruncate(a.lonI, 17),
          "the first Wi-Fi beacon: the fix cut to its 17-bit cell's centre");
    selfNetTxCell(NULL, &b, &lb, &lob);
    CHECK(lb != la && meshPosDistanceM(la, lo, lb, lob) > 300.0,
          "30 m apart across a cell edge: two cells ~364 m apart (the flip-flop, review item 15)");
    selfNetTxNoteSent(&m, &a, la, lo);
    selfNetTxCell(&m, &b, &lb, &lob);
    CHECK(lb == la && lob == lo, "...so within the fixes' accuracy of the last one, the SAME cell is sent");
    uint8_t p1[MESH_POS_BUILD_EX_MAX], p2[MESH_POS_BUILD_EX_MAX];
    const size_t n1 = meshPosBuildEx(p1, sizeof(p1), la, lo, 1760000000u, MESH_LOC_UNSET, 17);
    const size_t n2 = meshPosBuildEx(p2, sizeof(p2), a.latI, a.lonI, 1760000000u, MESH_LOC_UNSET, 17);
    CHECK(n1 == 18 && n1 == n2 && !memcmp(p1, p2, n1),
          "the cell's bytes are the raw fix's bytes at 17 bits (the cut is idempotent): 18 B");
    SelfFix far = a;
    far.latI = a.latI + 9000;                         // ~100 m on
    selfNetTxCell(&m, &far, &lb, &lob);
    CHECK(lb == meshPosTruncate(far.latI, 17), "100 m on (accuracy 60): a new cell, honestly");
    SelfFix g;
    memset(&g, 0, sizeof(g));
    g.src = SELF_GPS;
    SelfNetTxMem m2 = m;
    selfNetTxNoteSent(&m2, &g, 1, 2);
    CHECK(m2.cellLatI == m.cellLatI, "a GPS beacon is not a Wi-Fi cell sent");

    // precise fix wins: no Wi-Fi beacon within accM + 50 m of a GPS/phone beacon of the last hour
    selfNetTxInit(&m);
    const uint32_t t0 = 9000000;
    CHECK(!selfNetPreciseWins(&m, &a, t0), "no precise beacon yet: Wi-Fi may go");
    selfNetTxNotePrecise(&m, a.latI + 900, a.lonI, t0);          // ~10 m off
    CHECK(selfNetPreciseWins(&m, &a, t0 + 1000), "a precise beacon 10 m away a second ago: it wins");
    selfNetTxNotePrecise(&m, a.latI + 9800, a.lonI, t0);         // ~109 m: within 60 + 50
    CHECK(selfNetPreciseWins(&m, &a, t0 + 1000), "109 m away (inside 60 + 50): it still wins");
    selfNetTxNotePrecise(&m, a.latI + 10350, a.lonI, t0);        // ~115 m: outside
    CHECK(!selfNetPreciseWins(&m, &a, t0 + 1000), "115 m away (outside 60 + 50): the Wi-Fi fix may go");
    selfNetTxNotePrecise(&m, a.latI, a.lonI, t0);
    CHECK(selfNetPreciseWins(&m, &a, t0 + SELF_NET_PRECISE_WINS_MS - 1) &&
          !selfNetPreciseWins(&m, &a, t0 + SELF_NET_PRECISE_WINS_MS),
          "the same place: it wins for 59:59.999, not at 60:00");
    CHECK(!selfNetPreciseWins(&m, &g, t0 + 1000) && !selfNetPreciseWins(NULL, &a, t0 + 1000),
          "only a Wi-Fi pick is ever held back");
  }

  // ---- the stale guards' arithmetic ----------------------------------------------------------
  {
    CHECK(selfHmsToSod(0) == 0 && selfHmsToSod(235959) == 86399 && selfHmsToSod(120000) == 43200,
          "hhmmss -> second of day");
    CHECK(selfHmsToSod(-1) == -1 && selfHmsToSod(240000) == -1 && selfHmsToSod(126000) == -1 &&
          selfHmsToSod(120099) == -1, "out-of-range times refused");
    CHECK(selfHmsToSod(235960) == 86400, "a leap second is a real second");
    CHECK(selfSodDiff(86399, 0) == 1 && selfSodDiff(0, 86399) == 1 && selfSodDiff(43200, 0) == 43200 &&
          selfSodDiff(100, 50) == 50 && selfSodDiff(86400, 0) == 0, "differences across midnight");
    CHECK(selfPhoneTimeOk(235959, 2) && !selfPhoneTimeOk(235955, 2) && selfPhoneTimeOk(1, 86398),
          "23:59:59 vs 00:00:02 is 3 s (ok); 23:59:55 is 7 s (stale); and the other way");
    CHECK(selfPhoneTimeOk(120005, 43200) && !selfPhoneTimeOk(120006, 43200), "5 s ok, 6 s stale");
    CHECK(selfPhoneTimeOk(-1, -1) && !selfPhoneTimeOk(-1, 43200) && !selfPhoneTimeOk(250000, 43200),
          "untrusted clock: anything; trusted: no time or a bad time is stale");
    CHECK(!selfPhoneSettled(1000, 1000 + SELF_PHONE_SETTLE_MS - 1) &&
          selfPhoneSettled(1000, 1000 + SELF_PHONE_SETTLE_MS) &&
          selfPhoneSettled(0xFFFFFC00u, (uint32_t)(0xFFFFFC00u + SELF_PHONE_SETTLE_MS)), "settle: 1.999 s no, 2 s yes, across the wrap");
    CHECK(selfRmcModeOk(0) && selfRmcModeOk('A') && selfRmcModeOk('D') && !selfRmcModeOk('E') &&
          !selfRmcModeOk('N') && !selfRmcModeOk('M') && !selfRmcModeOk('S') && !selfRmcModeOk('X'),
          "RMC modes: E/N/M/S (and unknown letters) are not measurements");
  }

  // ---- the phone gate: one GGA, nothing inherited --------------------------------------------
  {
    SelfPhoneStream s;
    SelfPhoneOut o;
    live(&s);

    CHECK(feedS(&s, gga("120000.00", "1", "09", "0.9"), T, -1, &o) && o.verdict == SELF_PV_FIX &&
          o.latI == D_LAT && o.lonI == D_LON && o.sats == 9 && o.hdopX10 == 9,
          "a good GGA: FIX with its own position, 9 sats, HDOP 0.9");
    CHECK(!feedS(&s, gga("120001.00", "0", "09", "0.9"), T, -1, &o) && s.lastVerdict == SELF_PV_NOFIX,
          "quality 0: NOFIX, nothing written (the last fix is NOT re-stamped)");
    CHECK(!feedS(&s, gga("120002.00", "1", "", ""), T + 1, -1, &o) && s.lastVerdict == SELF_PV_WAIT,
          "no sats, no HDOP, no RMC: held (WAIT), nothing written - the old 9 sats are not inherited");
    char noPos[160];
    ck(noPos, sizeof(noPos), "GPGGA,120003.00,,,,,1,09,0.9,,M,,M,,", true);
    CHECK(!feedS(&s, noPos, T, -1, &o) && s.lastVerdict == SELF_PV_NOFIX,
          "quality 1 with empty coordinates: NOFIX");
    CHECK(!feedS(&s, gga("120004.00", "6", "09", "0.9"), T, -1, &o) && s.lastVerdict == SELF_PV_NOT_MEASURED,
          "quality 6 (dead reckoning): NOT_MEASURED");
    CHECK(!feedS(&s, gga("120005.00", "7", "09", "0.9"), T, -1, &o) &&
          !feedS(&s, gga("120005.00", "8", "09", "0.9"), T, -1, &o) &&
          s.lastVerdict == SELF_PV_NOT_MEASURED, "quality 7 (manual) and 8 (simulator): NOT_MEASURED");
    CHECK(feedS(&s, gga("120006.00", "2", "09", "0.9"), T, -1, &o) &&
          feedS(&s, gga("120006.00", "3", "09", "0.9"), T, -1, &o) &&
          feedS(&s, gga("120006.00", "4", "09", "0.9"), T, -1, &o) &&
          feedS(&s, gga("120006.00", "5", "09", "0.9"), T, -1, &o), "quality 2, 3, 4, 5: FIX");
    CHECK(!feedS(&s, gga("120007.00", "", "09", "0.9"), T, -1, &o) && s.lastVerdict == SELF_PV_NOFIX,
          "an empty quality: NOFIX");

    CHECK(feedS(&s, gga("120008.00", "1", "03", "6.4"), T, -1, &o) && o.verdict == SELF_PV_POOR &&
          o.sats == 3 && o.hdopX10 == 64, "3 sats: POOR, written WITH its own numbers (drawn grey, not sent)");
    CHECK(feedS(&s, gga("120009.00", "1", "04", "10.0"), T, -1, &o) && o.verdict == SELF_PV_FIX,
          "4 sats, HDOP 10.0: FIX (both bars exactly)");
    CHECK(feedS(&s, gga("120010.00", "1", "09", "10.1"), T, -1, &o) && o.verdict == SELF_PV_POOR,
          "HDOP 10.1: POOR");
    CHECK(feedS(&s, gga("120011.00", "1", "09", ""), T, -1, &o) && o.verdict == SELF_PV_FIX &&
          o.sats == 9 && o.hdopX10 == -1, "sats present, HDOP empty: FIX (HDOP unknown)");
    CHECK(feedS(&s, gga("120012.00", "1", "", "1.2"), T, -1, &o) && o.verdict == SELF_PV_FIX &&
          o.sats == -1 && o.hdopX10 == 12, "HDOP present, sats empty: FIX (sats unknown)");
    CHECK(!feedS(&s, gga("120013.00", "1", "1x", "0.9"), T, -1, &o) && s.lastVerdict == SELF_PV_GARBLED,
          "sats '1x': GARBLED, nothing written");
    CHECK(!feedS(&s, gga("120014.00", "1", "12", "1.2E-4"), T, -1, &o) && s.lastVerdict == SELF_PV_GARBLED,
          "HDOP '1.2E-4' (Double.toString): GARBLED");

    /* gpsdRelay's GENERATED GGA: accuracy in METRES in the HDOP field, always 12 sats. */
    CHECK(feedS(&s, gga("120015.00", "1", "12", "3.9"), T, -1, &o) && o.verdict == SELF_PV_FIX,
          "generated GGA, 3.9 m accuracy: FIX");
    CHECK(feedS(&s, gga("120016.00", "1", "12", "15.0"), T, -1, &o) && o.verdict == SELF_PV_POOR,
          "generated GGA, 15 m accuracy: POOR");
    char sci[160];
    ck(sci, sizeof(sci), "GPGGA,120017.00,4000.0E-4,N,10000.00000,W,1,12,3.0,100.0,M,,M,,", true);
    CHECK(!feedS(&s, sci, T, -1, &o) && s.lastVerdict == SELF_PV_NOFIX,
          "a scientific-notation coordinate does not parse: NOFIX, never a misread");
  }

  // ---- the RMC as a cross-check: confirmation and veto ---------------------------------------
  {
    SelfPhoneStream s;
    SelfPhoneOut o;
    live(&s);
    // GPS 2 IP style: a GGA with empty sats/HDOP, its RMC after it.
    CHECK(!feedS(&s, gga("130000.00", "1", "", ""), T, -1, &o) && s.lastVerdict == SELF_PV_WAIT, "held");
    CHECK(feedS(&s, rmc("130000.00", "A", "A"), T + 300, -1, &o) && o.verdict == SELF_PV_FIX &&
          o.latI == D_LAT && o.sats == -1 && o.hdopX10 == -1, "...the same second's 'A' RMC vouches: FIX");
    // ...and before it.
    CHECK(feedS(&s, rmc("130001.00", "A", "D") , T, -1, &o) == false, "an RMC alone writes nothing");
    CHECK(feedS(&s, gga("130001.00", "1", "", ""), T, -1, &o) && o.verdict == SELF_PV_FIX,
          "RMC first, then its GGA: FIX at once");
    // A different second never confirms.
    feedS(&s, gga("130002.00", "1", "", ""), T, -1, &o);
    CHECK(!feedS(&s, rmc("130003.00", "A", "A"), T, -1, &o) && s.lastVerdict == SELF_PV_WAIT,
          "an RMC of another second does not vouch");
    // V, or a mode that is not a measurement, refuses.
    feedS(&s, gga("130004.00", "1", "", ""), T, -1, &o);
    CHECK(!feedS(&s, rmc("130004.00", "V", "N"), T, -1, &o) && s.lastVerdict == SELF_PV_NOFIX,
          "the same second's 'V' RMC: NOFIX");
    feedS(&s, gga("130005.00", "1", "", ""), T, -1, &o);
    CHECK(!feedS(&s, rmc("130005.00", "A", "E"), T, -1, &o) && s.lastVerdict == SELF_PV_NOFIX,
          "'A' with mode E (estimated): NOFIX");
    // Too late to vouch.
    feedS(&s, gga("130006.00", "1", "", ""), T, -1, &o);
    CHECK(!feedS(&s, rmc("130006.00", "A", "A"), T + SELF_PHONE_PEND_MS + 1, -1, &o),
          "an RMC more than 2 s after its GGA vouches for nothing");
    // A newer GGA replaces the one waiting.
    feedS(&s, gga("130007.00", "1", "", ""), T, -1, &o);
    feedS(&s, gga("130008.00", "1", "", ""), T, -1, &o);
    CHECK(!feedS(&s, rmc("130007.00", "A", "A"), T, -1, &o), "a newer GGA supersedes the one waiting");
    // Veto: the RMC said V first, and a GGA of the same second WITH good numbers is still refused.
    feedS(&s, rmc("130009.00", "V", "N"), T, -1, &o);
    CHECK(!feedS(&s, gga("130009.00", "1", "09", "0.9"), T, -1, &o) && s.lastVerdict == SELF_PV_NOFIX,
          "the same second's 'V' RMC vetoes a good-looking GGA");
    feedS(&s, rmc("130010.00", "A", "S"), T, -1, &o);
    CHECK(!feedS(&s, gga("130010.00", "1", "09", "0.9"), T, -1, &o), "...and so does mode S (simulator)");
    // Pre-2.3 RMC (no mode field at all) is a measurement.
    char old23[160];
    ck(old23, sizeof(old23), "GPRMC,130011.00,A,4000.00000,N,10000.00000,W,0.0,,101026,,", true);
    feedS(&s, gga("130011.00", "1", "", ""), T, -1, &o);
    CHECK(feedS(&s, old23, T, -1, &o) && o.verdict == SELF_PV_FIX, "an RMC with no mode field vouches");
  }

  // ---- stale guards on the stream: settle, time of day, the 30-line burst --------------------
  {
    SelfPhoneStream s;
    SelfPhoneOut o;
    selfPhoneInit(&s);
    selfPhoneBegin(&s, 50000);
    // gpsdRelay's queue: 30 old lines the moment the socket is accepted.
    char burst[30 * 90];
    burst[0] = 0;
    for (int i = 0; i < 15; i++) {
      char h[16];
      snprintf(h, sizeof(h), "0900%02d.00", i);
      strcat(burst, rmc(h, "A", "A"));
      strcat(burst, gga(h, "1", "09", "0.9"));
    }
    CHECK(!feedS(&s, burst, 50000 + 10, -1, &o) && s.nStale == 30 && s.nFix == 0,
          "the 30-line backlog at connect: all STALE, nothing written");
    CHECK(!feedS(&s, gga("120000.00", "1", "09", "0.9"), 50000 + SELF_PHONE_SETTLE_MS - 1, -1, &o),
          "1.999 s after the connect: still settling");
    CHECK(feedS(&s, gga("120000.00", "1", "09", "0.9"), 50000 + SELF_PHONE_SETTLE_MS, -1, &o),
          "2 s after the connect: judged (untrusted clock: no time check)");

    // A trusted clock at 12:00:00.
    const int32_t noon = 43200;
    CHECK(feedS(&s, gga("120005.00", "1", "09", "0.9"), 60000, noon, &o), "5 s from a trusted clock: FIX");
    CHECK(!feedS(&s, gga("120006.00", "1", "09", "0.9"), 60000, noon, &o) && s.lastVerdict == SELF_PV_STALE,
          "6 s from a trusted clock: STALE");
    CHECK(!feedS(&s, gga("090000.00", "1", "09", "0.9"), 60000, noon, &o), "three hours old: STALE");
    CHECK(!feedS(&s, gga("", "1", "09", "0.9"), 60000, noon, &o) && s.lastVerdict == SELF_PV_STALE,
          "a trusted clock and a GGA with no time: STALE (cannot be shown current)");
    CHECK(!feedS(&s, gga("", "0", "", ""), 60000, noon, &o) && s.lastVerdict == SELF_PV_NOFIX,
          "...but a no-fix GGA with no time is just NOFIX (it writes nothing either way)");
    // Midnight.
    CHECK(feedS(&s, gga("235959.00", "1", "09", "0.9"), 60000, 2, &o), "23:59:59 at 00:00:02: FIX");
    CHECK(!feedS(&s, gga("235955.00", "1", "09", "0.9"), 60000, 2, &o), "23:59:55 at 00:00:02: STALE");
    CHECK(feedS(&s, gga("000001.00", "1", "09", "0.9"), 60000, 86398, &o), "00:00:01 at 23:59:58: FIX");
    // A stale 'A' RMC does not vouch for a GGA without numbers.
    feedS(&s, rmc("080000.00", "A", "A"), 60000, noon, &o);
    CHECK(!feedS(&s, gga("080000.00", "1", "", ""), 60000, -1, &o) && s.lastVerdict == SELF_PV_WAIT,
          "a stale 'A' RMC is not remembered as this second's word");
  }

  // ---- the '\n' before every '$' -------------------------------------------------------------
  {
    char noEol[400];
    snprintf(noEol, sizeof(noEol), "%s%s%s", rmc("140000.00", "A", "A", false),
             gga("140000.00", "1", "09", "0.9", false), rmc("140001.00", "A", "A", false));
    NmeaReader plain, helped;
    int a = 0, b = 0;
    for (const char* p = noEol; *p; p++) {
      if (plain.feed(*p)) a++;
      if (selfNmeaStreamFeed(&helped, *p)) b++;
    }
    CHECK(a == 0, "without the helper, a stream with no line endings is discarded whole");
    CHECK(b == 2 && helped.fix().ggaCount == 1 && helped.fix().rmcCount == 1 &&
          helped.badChecksum() == 0, "with it, each sentence completes at the next '$' (the last waits)");

    char crlf[400];
    snprintf(crlf, sizeof(crlf), "%s%s", rmc("140000.00", "A", "A"), gga("140000.00", "1", "09", "0.9"));
    NmeaReader r2;
    int c = 0;
    for (const char* p = crlf; *p; p++) {
      if (selfNmeaStreamFeed(&r2, *p)) c++;
    }
    CHECK(c == 2 && r2.badChecksum() == 0, "a CRLF stream loses nothing to the extra '\\n'");

    // The stream API, in chunks of every size, against the no-line-ending text.
    for (size_t chunk = 1; chunk <= 13; chunk += 4) {
      SelfPhoneStream s;
      SelfPhoneOut o;
      live(&s);
      bool any = false;
      size_t n = strlen(noEol);
      for (size_t i = 0; i < n; i += chunk) {
        memset(&o, 0, sizeof(o));
        if (selfPhoneFeed(&s, (const uint8_t*)noEol + i, (n - i < chunk ? n - i : chunk), T, -1, &o)) {
          any = true;
        }
      }
      char nm[80];
      snprintf(nm, sizeof(nm), "no line endings, %u-byte chunks: the GGA is judged FIX", (unsigned)chunk);
      CHECK(any && s.lastVerdict == SELF_PV_FIX && s.nFix == 1, nm);
    }

    // A line cut short is now a counted bad checksum, not a silent loss.
    NmeaReader r3;
    const char* cut = "$GPGGA,140000.00,4000.0$GPRMC";
    for (const char* p = cut; *p; p++) selfNmeaStreamFeed(&r3, *p);
    CHECK(r3.badChecksum() == 1, "a sentence cut by the next '$' counts as a bad checksum");
  }

  // ---- the stream's counters and watchdog ----------------------------------------------------
  {
    SelfPhoneStream s;
    SelfPhoneOut o;
    selfPhoneInit(&s);
    selfPhoneBegin(&s, 0);
    feedS(&s, gga("150000.00", "1", "09", "0.9"), 3000, -1, &o);
    feedS(&s, "$GPGGA,bad*00\r\n", 3000, -1, &o);
    CHECK(selfPhoneSentences(&s) == 1 && selfPhoneBadChecksums(&s) == 1, "counted on the first connection");
    CHECK(!selfPhoneSilent(&s, 3000 + SELF_PHONE_SILENT_MS - 1) && selfPhoneSilent(&s, 3000 + SELF_PHONE_SILENT_MS),
          "silent 10 s after the last good sentence (not after the bad one)");
    selfPhoneBegin(&s, 20000);
    CHECK(!selfPhoneSilent(&s, 20000 + 5000), "a new connection restarts the watchdog");
    feedS(&s, gga("150001.00", "1", "09", "0.9"), 23000, -1, &o);
    CHECK(selfPhoneSentences(&s) == 2 && selfPhoneBadChecksums(&s) == 1 && s.nFix == 2,
          "since-boot counters survive a reconnect");
    // A GGA held on one connection is not vouched for by an RMC on the next.
    feedS(&s, gga("150002.00", "1", "", ""), 23000, -1, &o);
    selfPhoneBegin(&s, 24000);
    CHECK(!feedS(&s, rmc("150002.00", "A", "A"), 24000 + SELF_PHONE_SETTLE_MS, -1, &o),
          "a reconnect drops the GGA that was waiting");
  }

  // ---- finding the app: ports, rounds, back-off ----------------------------------------------
  {
    uint16_t pp[SELF_PHONE_PORTS_MAX];
    CHECK(selfPhonePorts(0, pp) == 2 && pp[0] == 10110 && pp[1] == 11123, "default ports: 10110 then 11123");
    CHECK(selfPhonePorts(5000, pp) == 3 && pp[2] == 5000, "a custom port goes third");
    CHECK(selfPhonePorts(10110, pp) == 2 && selfPhonePorts(11123, pp) == 2, "a custom port that repeats one is not asked twice");

    CHECK(selfProbeDelayMs(0, 0) == 0 && selfProbeDelayMs(1, 0) == 2000 && selfProbeDelayMs(2, 0) == 5000 &&
          selfProbeDelayMs(3, 0) == 10000 && selfProbeDelayMs(4, 0) == 30000 && selfProbeDelayMs(5, 0) == 60000 &&
          selfProbeDelayMs(9, 0) == 60000, "the ladder: 2, 5, 10, 30, 60 s (the cap)");
    CHECK(selfProbeDelayMs(1, 3) == 300000 && selfProbeDelayMs(9, 7) == 300000, "3 dead rounds: 5 minutes");

    SelfProbe p;
    selfProbeStart(&p, 777);
    uint32_t now = 100000;
    CHECK(selfProbeDue(&p, now) && selfProbePort(&p, 0) == 10110 && !selfProbeNoApp(&p),
          "a new association: ask 10110 now");
    selfProbeFailed(&p, 0, now);
    CHECK(selfProbeDue(&p, now) && selfProbePort(&p, 0) == 11123, "refused: 11123 straight after");
    selfProbeFailed(&p, 0, now);
    CHECK(!selfProbeDue(&p, now + 1999) && selfProbeDue(&p, now + 2000) && selfProbeNoApp(&p) &&
          selfProbePort(&p, 0) == 10110, "round 1 failed: 2 s, 'no app found', back to 10110");
    now += 2000;
    selfProbeFailed(&p, 0, now); selfProbeFailed(&p, 0, now);
    CHECK(!selfProbeDue(&p, now + 4999) && selfProbeDue(&p, now + 5000), "round 2 failed: 5 s");
    now += 5000;
    selfProbeFailed(&p, 0, now); selfProbeFailed(&p, 0, now);
    CHECK(!selfProbeDue(&p, now + 299999) && selfProbeDue(&p, now + 300000), "round 3 failed: 5 minutes");
    now += 300000;
    selfProbeFailed(&p, 0, now); selfProbeFailed(&p, 0, now);
    CHECK(!selfProbeDue(&p, now + 299999) && selfProbeDue(&p, now + 300000), "and every 5 minutes after");

    selfProbeStart(&p, 778);
    CHECK(selfProbeDue(&p, now) && p.deadRounds == 0 && p.fails == 0, "a new association forgets it all");

    // With a custom port, a round is three connects.
    selfProbeFailed(&p, 5000, now); selfProbeFailed(&p, 5000, now);
    CHECK(selfProbeDue(&p, now) && selfProbePort(&p, 5000) == 5000, "the custom port is the third of the round");
    selfProbeFailed(&p, 5000, now);
    CHECK(!selfProbeDue(&p, now) && selfProbeNoApp(&p), "...and its refusal ends the round");

    // GPS 2 IP answers on 11123: remembered, failures forgotten.
    selfProbeStart(&p, 779);
    selfProbeFailed(&p, 0, now);
    selfProbeGood(&p, 11123);
    CHECK(selfProbePort(&p, 0) == 11123 && selfProbeDue(&p, now) && !selfProbeNoApp(&p) && p.fails == 0,
          "a good sentence on 11123: remembered for the association");
    selfProbeDropped(&p, now);
    CHECK(!selfProbeDue(&p, now + 1999) && selfProbeDue(&p, now + 2000) && selfProbePort(&p, 0) == 11123,
          "the stream dropped: 2 s, the same port");
    now += 2000;
    selfProbeFailed(&p, 0, now);                  // the remembered port refused: 5 s
    CHECK(selfProbePort(&p, 0) == 11123 && selfProbeDue(&p, now + 5000) && !selfProbeDue(&p, now + 4999),
          "the remembered port refused once: kept, 5 s");
    now += 5000;
    selfProbeFailed(&p, 0, now);                  // twice: 10 s
    now += 10000;
    selfProbeFailed(&p, 0, now);                  // three times: forgotten, rounds again
    CHECK(selfProbePort(&p, 0) == 10110 && p.port == 0 && selfProbeDue(&p, now + 30000) &&
          !selfProbeDue(&p, now + 29999), "refused 3 times in a row: forgotten (the app changed?), 30 s, rounds again");

    // millis() wrap on the wait.
    selfProbeStart(&p, 780);
    selfProbeFailed(&p, 0, 0xFFFFFF00u); selfProbeFailed(&p, 0, 0xFFFFFF00u);
    CHECK(!selfProbeDue(&p, 0xFFFFFF00u + 1999) && selfProbeDue(&p, 0xFFFFFF00u + 2000),
          "a 2 s wait across the millis() wrap");
  }

  // ---- when the client may run; the bench address ---------------------------------------------
  {
    int yes = 0, which = -1;
    for (int m = 0; m < 16; m++) {
      if (selfPhoneMayRun(m & 1, m & 2, m & 4, m & 8)) { yes++; which = m; }
    }
    CHECK(yes == 1 && which == 3, "runs only when on, joined, not our own hotspot, radio on (all 16 states)");

    uint8_t ip[4] = { 0, 0, 0, 0 };
    uint16_t port = 1;
    CHECK(selfPhoneParseAt("192.168.1.5", ip, &port) && ip[0] == 192 && ip[3] == 5 && port == 0,
          "'phonepos at 192.168.1.5': no port");
    CHECK(selfPhoneParseAt("10.0.0.2:10110", ip, &port) && ip[0] == 10 && port == 10110, "with a port");
    CHECK(selfPhoneParseAt("1.2.3.4:65535", ip, &port) && port == 65535, "65535");
    CHECK(!selfPhoneParseAt("256.1.1.1", ip, &port) && !selfPhoneParseAt("1.2.3", ip, &port) &&
          !selfPhoneParseAt("1.2.3.4:", ip, &port) && !selfPhoneParseAt("1.2.3.4:0", ip, &port) &&
          !selfPhoneParseAt("1.2.3.4:65536", ip, &port) && !selfPhoneParseAt("1.2.3.4x", ip, &port) &&
          !selfPhoneParseAt("1.2.3.4 ", ip, &port) && !selfPhoneParseAt("", ip, &port) &&
          !selfPhoneParseAt("1234.1.1.1", ip, &port), "bad addresses refused");
  }

  // ---- the screen's words ----------------------------------------------------------------------
  {
    char t[48];
    selfPhoneStatusText(t, sizeof(t), SELF_LINK_OFF, NULL, 0);
    CHECK(!strcmp(t, "off"), "off");
    selfPhoneStatusText(t, sizeof(t), SELF_LINK_NO_APP, NULL, 0);
    CHECK(!strcmp(t, "no app found on this network"), "no app found on this network");
    SelfPhoneStream s;
    SelfPhoneOut o;
    live(&s);
    selfPhoneStatusText(t, sizeof(t), SELF_LINK_STREAM, &s, T);
    CHECK(!strcmp(t, "connected, waiting for data"), "connected, waiting");
    feedS(&s, gga("160000.00", "1", "09", "0.9"), T, -1, &o);
    selfPhoneStatusText(t, sizeof(t), SELF_LINK_STREAM, &s, T);
    /* "this network", not "on hotspot": ANY network's gateway is asked (phone_pos.h's trust
     * model) - a boat's NMEA gateway on 10110 is not a hotspot. */
    CHECK(!strcmp(t, "this network: fix, 9 sats"), "this network: fix, 9 sats");
    feedS(&s, gga("160001.00", "1", "03", "6.4"), T, -1, &o);
    selfPhoneStatusText(t, sizeof(t), SELF_LINK_STREAM, &s, T);
    CHECK(!strcmp(t, "weak fix, 3 sats: not sent"), "weak fix, 3 sats: not sent");
    feedS(&s, gga("160002.00", "0", "", ""), T, -1, &o);
    selfPhoneStatusText(t, sizeof(t), SELF_LINK_STREAM, &s, T);
    CHECK(!strcmp(t, "no fix (indoors?)"), "no fix (indoors?)");
    feedS(&s, gga("160003.00", "1", "", ""), T, -1, &o);
    feedS(&s, rmc("160003.00", "A", "A"), T, -1, &o);
    selfPhoneStatusText(t, sizeof(t), SELF_LINK_STREAM, &s, T);
    CHECK(!strcmp(t, "fix, accuracy unknown: not sent"),
          "a fix vouched by its RMC: shown, and the line says it is not sent (UNRATED)");
    feedS(&s, gga("160007.00", "1", "", "1.1"), T, -1, &o);
    selfPhoneStatusText(t, sizeof(t), SELF_LINK_STREAM, &s, T);
    CHECK(!strcmp(t, "this network: fix"), "HDOP but no sats: a fix, no count to show");

    live(&s);
    feedS(&s, rmc("160004.00", "A", "A"), T, -1, &o);
    feedS(&s, rmc("160005.00", "A", "A"), T, -1, &o);
    feedS(&s, rmc("160006.00", "A", "A"), T, -1, &o);
    selfPhoneStatusText(t, sizeof(t), SELF_LINK_STREAM, &s, T);
    CHECK(!strcmp(t, "the app sends no GGA"), "RMC only: the app sends no GGA");

    char tiny[8];
    size_t n = selfPhoneStatusText(tiny, sizeof(tiny), SELF_LINK_NO_APP, NULL, 0);
    CHECK(n == 7 && strlen(tiny) == 7, "cut to the buffer, terminated");
  }

  // ---- two fixes in a row must agree (the glue's guard against gpsdRelay's ddToDDMM) ---------
  {
    const int32_t dLat100m = 8993;                 // ~100 m of latitude in 1e-7 deg (0.0008993)
    CHECK(selfPhoneAgrees(D_LAT, D_LON, 1000, D_LAT, D_LON, 2000), "the same point a second later agrees");
    CHECK(selfPhoneAgrees(D_LAT, D_LON, 1000, D_LAT + dLat100m, D_LON, 2000),
          "100 m in 1 s agrees (50 m + 300 m/s)");
    CHECK(!selfPhoneAgrees(D_LAT, D_LON, 1000, D_LAT + 4 * dLat100m, D_LON, 2000),
          "400 m in 1 s does not");
    CHECK(selfPhoneAgrees(D_LAT, D_LON, 1000, D_LAT, D_LON, 1000 + SELF_PHONE_AGREE_GAP_MS),
          "the gap bar itself still agrees");
    CHECK(!selfPhoneAgrees(D_LAT, D_LON, 1000, D_LAT, D_LON, 1001 + SELF_PHONE_AGREE_GAP_MS),
          "one ms past the gap: too long ago to vouch");
    CHECK(selfPhoneAgrees(D_LAT, D_LON, 5000, D_LAT + 4 * dLat100m / 10, D_LON, 5000),
          "two GGAs of one chunk (dt 0): 40 m agrees");
    CHECK(!selfPhoneAgrees(D_LAT, D_LON, 5000, D_LAT + 6 * dLat100m / 10, D_LON, 5000),
          "...60 m does not");
    CHECK(selfPhoneAgrees(D_LAT, D_LON, 0xFFFFFE00u, D_LAT, D_LON, 0x000001E8u),
          "across the millis() wrap");
    /* The case itself: 40 deg 0.00006 min (40.000001) as gpsdRelay's generator writes it,
     * "4005.999999" — 40 deg 5.999999 min — through the shipping reader, a second later. */
    NmeaReader r;
    r.reset();
    char s[160];
    ck(s, sizeof(s), "GNGGA,160000.00,4005.999999,N,10000.000000,W,1,12,3.9,500.0,M,0,M,,", true);
    bool got = false;
    for (const char* c = s; *c; c++) {
      got = r.feed(*c) || got;
    }
    CHECK(got && r.fix().ggaHasPos && r.fix().ggaLatI > D_LAT + 900000,
          "gpsdRelay's E-notation cut parses as ~11 km north (the reader cannot know)");
    CHECK(!selfPhoneAgrees(400000100, D_LON, 1000, r.fix().ggaLatI, r.fix().ggaLonI, 2000),
          "...and does not agree with the true fix a second before");
  }

  // ---- keeping a fix: the screen's "fix" means the PHONE slot has one (review 2026-10-10) --------
  {
    SelfPhoneStream s;
    SelfPhoneOut o;
    char t[48];
    live(&s);
    CHECK(feedS(&s, gga("160000.00", "1", "09", "0.9"), T, -1, &o) && o.verdict == SELF_PV_FIX,
          "keep: the gate lets the first fix through");
    CHECK(!selfPhoneKeep(&s, &o, T) && s.lastVerdict == SELF_PV_CHECKING && s.nDisagree == 0,
          "keep: a connection's first fix waits (CHECKING), not counted as a disagreement");
    selfPhoneStatusText(t, sizeof(t), SELF_LINK_STREAM, &s, T);
    CHECK(!strcmp(t, "fix found, checking it"), "keep: the line does not say 'fix' over an empty slot");
    CHECK(feedS(&s, gga("160001.00", "1", "09", "0.9"), T + 1000, -1, &o) &&
          selfPhoneKeep(&s, &o, T + 1000) && s.lastVerdict == SELF_PV_FIX,
          "keep: the next, a second later in the same place, is kept");
    /* A slow but steady app: every 8 s, inside the 10 s silence watchdog. At the old 5 s gap
     * this stayed connected and never kept one fix while the screen said "fix". */
    CHECK(feedS(&s, gga("160009.00", "1", "09", "0.9"), T + 9000, -1, &o) &&
          selfPhoneKeep(&s, &o, T + 9000), "keep: an app sending every 8 s still produces fixes");
    CHECK(SELF_PHONE_AGREE_GAP_MS >= SELF_PHONE_SILENT_MS,
          "keep: the agreement gap is at least the silence watchdog's");
    // A jump: the same stream, the next GGA ~11 km north (gpsdRelay's E-notation cut).
    char body[160], jump[192];
    snprintf(body, sizeof(body), "GPGGA,160010.00,4006.00000,N,10000.00000,W,1,09,0.9,100.0,M,,M,,");
    ck(jump, sizeof(jump), body, true);
    CHECK(feedS(&s, jump, T + 10000, -1, &o) && !selfPhoneKeep(&s, &o, T + 10000) &&
          s.lastVerdict == SELF_PV_JUMPED && s.nDisagree == 1, "keep: a jump is not kept (JUMPED, counted)");
    selfPhoneStatusText(t, sizeof(t), SELF_LINK_STREAM, &s, T + 10000);
    CHECK(!strcmp(t, "fixes jump about: not used"), "keep: ...and the line says so");
    CHECK(feedS(&s, gga("160011.00", "1", "09", "0.9"), T + 11000, -1, &o) &&
          !selfPhoneKeep(&s, &o, T + 11000) && s.nDisagree == 2,
          "keep: ...and the true fix after it disagrees with the jump: that one is lost too");
    CHECK(feedS(&s, gga("160012.00", "1", "09", "0.9"), T + 12000, -1, &o) &&
          selfPhoneKeep(&s, &o, T + 12000) && s.lastVerdict == SELF_PV_FIX, "keep: then back to kept");
    /* A GGA that wrote, then one that wrote nothing in the same chunk: the line keeps the later
     * judgement, not a CHECKING laid over it. */
    selfPhoneBegin(&s, 1000);
    char two[400];
    snprintf(two, sizeof(two), "%s%s", gga("160013.00", "1", "09", "0.9"), gga("160014.00", "0", "", ""));
    CHECK(feedS(&s, two, T, -1, &o) && !selfPhoneKeep(&s, &o, T) && s.lastVerdict == SELF_PV_NOFIX,
          "keep: a later no-fix GGA in the chunk is what the line says");
    CHECK(!strcmp(selfPhoneVerdictName(SELF_PV_CHECKING), "checking") &&
          !strcmp(selfPhoneVerdictName(SELF_PV_JUMPED), "jumped"), "verdict names");
  }

  // ---- GPS 2 IP's stand-in 8 sats / HDOP 0.9 are read as "not said" (UNRATED) --------------
  {
    static SelfPhoneStream s;
    SelfPhoneOut o;
    selfPhoneInit(&s);
    selfPhoneBegin(&s, 1000);
    selfPhoneSetPort(&s, SELF_PHONE_PORT_GPS2IP);
    CHECK(feedS(&s, gga("160000.00", "1", "8", "0.9"), T, -1, &o) && o.verdict == SELF_PV_FIX &&
              o.sats == -1 && o.hdopX10 == -1 && s.placeholder,
          "GPS 2 IP's port: the 8 / 0.9 pair is dropped at once (a fix, accuracy not said)");
    char line[64];
    selfPhoneStatusText(line, sizeof(line), SELF_LINK_STREAM, &s, T);
    CHECK(strstr(line, "iPhone") && strstr(line, "not sent"), "...and the line says the iPhone app gives no accuracy");

    selfPhoneBegin(&s, 1000);
    selfPhoneSetPort(&s, SELF_PHONE_PORT_NMEA);
    CHECK(feedS(&s, gga("160000.00", "1", "8", "0.9"), T, -1, &o) && o.sats == 8 && o.hdopX10 == 9,
          "another port: one 8 / 0.9 GGA is a real receiver's numbers");
    bool kept = true;
    char hms[16];
    for (int i = 1; i < SELF_PHONE_PLACEHOLDER_RUN; i++) {
      snprintf(hms, sizeof(hms), "1600%02d.00", i);
      feedS(&s, gga(hms, "1", "8", "0.9"), T + 1000u * i, -1, &o);
      if (i < SELF_PHONE_PLACEHOLDER_RUN - 1) {
        kept = kept && o.sats == 8;
      }
    }
    CHECK(kept && o.sats == -1 && o.hdopX10 == -1 && s.placeholder,
          "another port: five GGAs running with exactly 8 / 0.9 are stand-ins (dropped from the fifth)");
    feedS(&s, gga("160010.00", "1", "11", "0.7"), T + 10000u, -1, &o);
    CHECK(o.sats == 11 && o.hdopX10 == 7 && !s.placeholder, "a moving pair is a real rating again");
    feedS(&s, gga("160011.00", "1", "8", "0.9"), T + 11000u, -1, &o);
    CHECK(o.sats == 8 && o.hdopX10 == 9, "...and the run starts over");
  }

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
