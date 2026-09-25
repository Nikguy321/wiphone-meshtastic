/*
 * clock_source.h — WHO set the phone's clock, and the rules that let GPS or the mesh set it.
 *
 * Nick, 2026-09-25: *"for wiphones, are we able to also do gps time as long as the gps is
 * enabled?"* Until 0.9.79 the clock had exactly one source — NTP over WiFi — and one flag,
 * set the first time NTP answered. No RTC, no GPS time, nothing from the mesh: a phone that
 * had not seen WiFi since boot did not know what day it was, however long its GPS had had a
 * fix. In the woods that is every phone, all day.
 *
 * Now there are three sources, ranked:
 *
 *   ntp   authoritative whenever it is PRESENT (it set the clock within CLOCK_NTP_FRESH_MS).
 *         Never overridden by anything while fresh.
 *   gps   the woods plate's RMC time. Trusted: a GNSS receiver with a fix knows UTC to well
 *         under a microsecond; everything between it and this clock (the NMEA output delay,
 *         the UART, the loop) costs a few hundred ms, and the clock only keeps seconds.
 *   mesh  a Meshtastic Position packet's `time` field, for phones with no GPS. LOWER TRUST:
 *         nothing on the air is authenticated beyond a channel key (see the spoofing note at
 *         clockMeshOffer), so it only ever fills an UNKNOWN clock, and trusted() is false.
 *
 * isTimeKnown() is true for any of them (a clock on the screen, message times, "5 min ago").
 * clockSourceTrusted() is the bar for everything that either LEAVES the phone or DESTROYS
 * something on the strength of the time — the position beacon's time, booksync's turnedAt,
 * the replay ring, waypoint expiry, KOSync's "provably older". Those all required a known
 * clock before 0.9.79, which meant NTP; they now require ntp or gps, never mesh, so a mesh
 * clock changes NOTHING any other device sees and deletes nothing. (clock.h has the list.)
 *
 * Pure C++, no Arduino: tests/test_clocksrc.cpp runs every rule here on a Mac, fed through the
 * shipping NMEA reader (nmea.cpp). clock.cpp is only the glue: it takes the mutex, asks these
 * functions, and applies the answer.
 */
#ifndef CLOCK_SOURCE_H
#define CLOCK_SOURCE_H

#include <stdint.h>
#include "nmea.h"

enum ClockSource {
  CLOCK_SRC_NONE = 0,               // not set: isTimeKnown() false
  CLOCK_SRC_NTP  = 1,
  CLOCK_SRC_GPS  = 2,
  CLOCK_SRC_MESH = 3,
};
const char* clockSourceName(int src);           // "none" "ntp" "gps" "mesh"
bool        clockSourceTrusted(int src);        // ntp or gps

/* ── SANE YEARS ────────────────────────────────────────────────────────────────────────────
 * 2026 <= year < 2100. The floor is this firmware's own year: nothing it will ever see is
 * older, and it is exactly the bar a GPS WEEK-ROLLOVER date fails — a receiver that has lost
 * track of the 1024-week era reports 2006/2007 (19.6 years back), which would otherwise be a
 * perfectly well-formed date. The ceiling keeps the arithmetic inside uint32_t seconds. */
#define CLOCK_YEAR_MIN            2026
#define CLOCK_YEAR_LIMIT          2100                 // first year NOT accepted
#define CLOCK_UNIX_MIN            1767225600u          // 2026-01-01T00:00:00Z (test-checked)
#define CLOCK_UNIX_LIMIT          4102444800u          // 2100-01-01T00:00:00Z (test-checked)
bool clockUnixSane(uint32_t unixSec);

/* A calendar UTC instant -> ms since 1970. False (out untouched) unless it is a real instant
 * in a sane year: month 1-12, the day within THAT month (29 Feb only in a leap year), hh < 24,
 * mm < 60, ss < 60 (a leap second's :60 is refused — the next reading is one second away),
 * ms < 1000. No libc, no time zone: timegm() is not portable and mktime() is local time. */
bool clockCivilToUnixMs(int year, int month, int day, int hour, int minute, int second, int ms,
                        int64_t* outMs);

/* Every outcome the clock can report, one list so `clock` and the log print the same words. */
enum ClockVerdict {
  CLK_NONE = 0,                     // nothing judged yet
  /* One RMC, judged alone. */
  CLK_GPS_NO_FIX,                   // status not 'A' (or 'A' with unreadable coordinates) — see the NOTE ON 'V' below
  CLK_GPS_BAD_MODE,                 // mode indicator says it is not a GNSS solution
  CLK_GPS_NO_TIME,                  // the RMC's own time field empty or garbled
  CLK_GPS_NO_DATE,                  // ...its date field
  CLK_GPS_BAD_DATE,                 // not a calendar instant (31 Sep, 24:00, :60)
  CLK_GPS_BAD_YEAR,                 // outside 2026..2099 (a week-rollover date)
  /* Two readings. */
  CLK_GPS_FIRST,                    // one good reading, waiting for the next to agree
  CLK_GPS_NOT_LATER,                // the next did not move forward (repeat or backwards)
  CLK_GPS_GAP,                      // too long between them: they prove nothing together
  CLK_GPS_DISAGREE,                 // their seconds and the millis() between them disagree
  /* The policy, for a pair that agreed. */
  CLK_GPS_SET_UNKNOWN,              // SET: the clock was not set
  CLK_GPS_SET_UPGRADE,              // SET: the clock came from the mesh (lower trust)
  CLK_GPS_SET_DRIFT,                // SET: off by more than CLOCK_GPS_STEP_MS, NTP not fresh
  CLK_GPS_KEEP_AGREES,              // kept: within CLOCK_GPS_STEP_MS
  CLK_GPS_KEEP_NTP_FRESH,           // kept: NTP set it recently and stays authoritative
  /* A mesh Position's time. */
  CLK_MESH_NO_TIME,                 // the packet carried no time (stock nodes strip it)
  CLK_MESH_BAD_YEAR,                // outside 2026..2099
  CLK_MESH_BAD_NODE,                // node 0 / broadcast: not a sender
  CLK_MESH_CLOCK_KNOWN,             // the clock is set: the mesh never overrides it
  CLK_MESH_WAIT,                    // public channel: held until a second node agrees
  CLK_MESH_SET_PRIVATE,             // SET: one packet on a private channel
  CLK_MESH_SET_AGREED,              // SET: two different nodes agreed within 60 s
};
const char* clockVerdictText(int verdict);      // "kept - agrees with the clock", ...
bool        clockVerdictSets(int verdict);      // one of the SET verdicts

/* ── GPS, STEP 1: ONE RMC ON ITS OWN ────────────────────────────────────────────────────────
 * Reads ONLY the rmc* fields of `fx` (see nmea.h for why the others are never paired).
 * True with the sentence's UTC in *utcMs; false with the reason in *why.
 *
 * 🛑 THE NOTE ON 'V': A NO-FIX RMC's TIME IS NOT USED, EVEN WHEN IT LOOKS COMPLETE. u-blox
 * receivers (the M100 Mini is a u-blox M10) print a time and date in RMC before any fix: from
 * the backup RTC if one kept running, else from the FIRST satellite decoded — before the
 * almanac has delivered the GPS-UTC leap-second count, before the week number is resolved,
 * sometimes a firmware-default date outright. The fixture this reader was built on is one:
 * `$GNRMC,081836.00,V,,,,,,,130826,,,N` — a date and a time, from a receiver that could see
 * NO satellites (tests/test_nmea.cpp, NOFIX_RMC). 'V' plus a plausible date is exactly the
 * case that cannot be told apart from 'V' plus a wrong one. Waiting for 'A' costs a minute
 * outdoors and guarantees the time came from a solved position.
 *
 * The mode indicator (NMEA 2.3+), when present, must be a GNSS solution: A autonomous,
 * D differential, F/R RTK, P precise. Refused: N (no fix), E (dead reckoning — the receiver's
 * own estimate), M (MANUAL INPUT) and S (SIMULATOR) — the last two are, literally, a time
 * somebody typed. Absent (an NMEA 2.1 receiver) is accepted: the status carries it. */
bool clockGpsRmcTime(const NmeaFix* fx, int64_t* utcMs, int* why);

/* ── GPS, STEP 2: TWO CONSECUTIVE READINGS THAT AGREE ─────────────────────────────────────
 * One sentence is one opinion. A checksum proves the bytes arrived, not that the receiver was
 * right — and the moment a receiver first reports 'A' is exactly when its time solution is
 * newest. So the clock moves only on a PAIR: the previous good reading and this one, with
 *   - this one strictly LATER in UTC (a repeated sentence is not a second opinion),
 *   - at most CLOCK_GPS_PAIR_MAX_GAP_MS of millis() between them (a fix lost and regained in
 *     between breaks the chain; a bad sentence between them breaks it too — the caller
 *     resets the pair on every refused RMC, so the two are CONSECUTIVE),
 *   - their UTC difference equal to the millis() difference within CLOCK_GPS_PAIR_TOL_MS.
 * The tolerance is not about the GPS: it is the LOOP. Bytes are read in the superloop, and
 * a loop stall (the mesh DB save alone is a measured 1.5 s) delays the read of whichever
 * sentence it lands on. A stall of more than the tolerance fails the pair and the next one,
 * a second later, forms normally.
 *
 * On success, *utcAtRxMs is UTC at millis() == rxMs (this reading's arrival), taken from
 * whichever reading was read SOONER: a reading can only ever be read LATE, never early, so
 * the larger of the two estimates is the closer one. */
#define CLOCK_GPS_PAIR_MAX_GAP_MS  5000u
#define CLOCK_GPS_PAIR_TOL_MS      1000
struct ClockGpsPair {
  bool     have;                    // a previous good reading is held
  int64_t  utcMs;                   // its UTC
  uint32_t rxMs;                    // the millis() it was read at
};
void clockGpsPairReset(ClockGpsPair* p);
bool clockGpsPairFeed(ClockGpsPair* p, int64_t utcMs, uint32_t rxMs, int64_t* utcAtRxMs,
                      int* why);

/* ── GPS, STEP 3: DOES AN AGREEING PAIR MOVE THE CLOCK? ───────────────────────────────────
 * In order:
 *   clock not set                         -> SET   (CLK_GPS_SET_UNKNOWN)
 *   clock set from the mesh               -> SET   (CLK_GPS_SET_UPGRADE: gps outranks mesh)
 *   NTP set it within CLOCK_NTP_FRESH_MS  -> KEEP, WHATEVER THE DIFFERENCE (CLK_GPS_KEEP_NTP_FRESH)
 *   off by more than CLOCK_GPS_STEP_MS    -> SET   (CLK_GPS_SET_DRIFT)
 *   otherwise                             -> KEEP  (CLK_GPS_KEEP_AGREES)
 *
 * Why NTP wins while fresh, even against a GPS that disagrees: NTP is what this clock has
 * always been and what every other device in the house (COVEY, the X4's server) agrees with;
 * a disagreement is far more likely to be ours (a bad sentence that slipped through, a loop
 * stall) than pool.ntp.org's, and a clock that flips between two sources is worse than
 * either. The disagreement is still MEASURED (*diffMs) and logged, so if it is ever NTP's
 * the log shows it. FRESH is three of NTP's own 10-minute resyncs (TIME_UPDATE_DELAY_MS):
 * three missed in a row means WiFi or NTP is gone, and from then on the clock is free-running
 * on the crystal and GPS may correct it.
 *
 * Why 2 s and not 0: NTP here keeps whole seconds (clock.cpp drops the fraction) and a GPS
 * sentence is read a few hundred ms late, so two correct sources routinely differ by up to a
 * second. Stepping the clock over that would move it on every pair for no gain.
 *
 * *diffMs = gps - clock, whenever the clock was set (0 otherwise). */
#define CLOCK_NTP_FRESH_MS         (30u * 60u * 1000u)
#define CLOCK_GPS_STEP_MS          2000
#define CLOCK_NEVER_MS             0xFFFFFFFFu        // msSinceNtp: NTP never set it
struct ClockNow {
  int      source;                  // ClockSource
  int64_t  utcMs;                   // what the clock reads at the moment being judged
  uint32_t msSinceNtp;              // since NTP last set it; CLOCK_NEVER_MS = never
};
int clockGpsDecide(const ClockNow* c, int64_t gpsUtcMs, int64_t* diffMs);

/* ── THE WHOLE GPS PATH FOR ONE NEW RMC (what clock.cpp calls) ────────────────────────────
 * Step 1; a refused sentence RESETS the pair (so a pair is always two CONSECUTIVE good RMCs
 * — "good, bad, good" is not two agreeing readings); step 2; and, for an agreeing pair, step
 * 3 against `now` (the clock as it reads at rxMs). Returns the verdict of the last step that
 * ran. On a SET verdict *setUtcMs is the UTC to give the clock at millis() == rxMs.
 * *diffMs is step 3's (0 when step 3 did not run). */
int clockGpsStep(ClockGpsPair* p, const NmeaFix* fx, uint32_t rxMs, const ClockNow* now,
                 int64_t* setUtcMs, int64_t* diffMs);

/* ── MESH TIME: ONLY INTO AN UNKNOWN CLOCK, AND NEVER ON ONE STRANGER'S WORD ──────────────
 * For phones with no GPS (phone 1). A Meshtastic Position's `time` (field 4) is the sender's
 * clock when it took the position. Adopted ONLY when:
 *   - this clock is NOT SET — the mesh never overrides ntp, gps, or an earlier mesh time;
 *   - the time is in a sane year (2026..2099);
 *   - and EITHER it arrived on a PRIVATE channel (a key that is not the well-known default and
 *     not empty — MeshtasticService::channelIsPublic() is the test), where one packet is
 *     enough; OR, on a public channel, TWO DIFFERENT nodes agree within CLOCK_MESH_AGREE_S
 *     (each aged by the millis() between their arrivals).
 *
 * WHY A PRIVATE CHANNEL NEEDS ONE PACKET AND LONGFAST NEEDS TWO: the realistic failure is not
 * an attacker, it is one node with a wrong clock. On LongFast that is any stranger's radio in
 * range — a Pi with no RTC, a node somebody set by hand — so one opinion is not enough. On a
 * private channel the senders are your own party's devices: WiPhones, which (after this
 * change) put a time on the air ONLY when it is trusted (ntp or gps — never a mesh-derived
 * time, so a second-hand time cannot come back as an "independent" witness), and stock
 * Meshtastic nodes, whose PositionModule — as I understand upstream; it is not verifiable in
 * this tree — strips `time` unless their own clock came from GPS or NTP. And a woods trip is
 * often exactly two phones: requiring two nodes there would mean phone 1 never gets a time.
 *
 * ⚠ THE SPOOFING TRADE-OFF, stated plainly. Nothing on the mesh is authenticated beyond the
 * channel key. On LongFast anyone can transmit, under ANY node number — "two different nodes"
 * defends against one misconfigured radio, not against somebody who means it, and who can
 * invent two node numbers as easily as one. On a private channel an outsider without the key
 * can still REPLAY a recorded packet (the packet-id dedup is a short window), which sets this
 * clock to the moment it was recorded: behind, never ahead. What bounds the damage is not
 * these rules but where a mesh time is allowed to go: it is never trusted (KOSync ignores it,
 * waypoints never expire on it, nothing this phone transmits carries it), it never replaces
 * a set clock, and NTP or GPS replaces IT the first time either appears. What it can get
 * wrong is what the phone SHOWS — the clock, message times, and the `sun` legal-light times,
 * which say "clock from the mesh" when it is. */
#define CLOCK_MESH_AGREE_S         60
#define CLOCK_MESH_CAND_MAX        4
#define CLOCK_MESH_CAND_MAX_AGE_MS (60u * 60u * 1000u)   // older candidates are forgotten
struct ClockMeshCand {
  uint32_t node;                    // 0 = empty slot
  uint32_t t;                       // its time (unix s)
  uint32_t rxMs;                    // millis() when it arrived
};
struct ClockMeshVote {
  ClockMeshCand c[CLOCK_MESH_CAND_MAX];
};
void clockMeshVoteReset(ClockMeshVote* v);
int  clockMeshVoteCount(const ClockMeshVote* v, uint32_t nowMs);   // live candidates held
/* One Position's time. On a SET verdict *adoptUtc is UTC (s) at millis() == rxMs, and the
 * vote is cleared. A public-channel packet that does not yet agree with anybody is HELD
 * (one slot per node, the newest; the oldest goes when full) and CLK_MESH_WAIT returned. */
int  clockMeshOffer(ClockMeshVote* v, int clockSource, uint32_t node, bool privateChannel,
                    uint32_t t, uint32_t rxMs, uint32_t* adoptUtc);

#endif // CLOCK_SOURCE_H
