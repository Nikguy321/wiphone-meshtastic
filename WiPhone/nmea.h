/* NMEA 0183 reader for the woods backplate's GPS (HGLRC M100 Mini, u-blox M10).
 *
 * Pure C++ on purpose — no Arduino, no floats in the position math — so the host
 * suite (tests/test_nmea.cpp) compiles the exact bytes that ship. The caller owns
 * all timekeeping: feed() reports WHEN something position-relevant completed and
 * the caller stamps it with millis(); nothing in here reads a clock.
 *
 * What it accepts, deliberately: bytes are ignored until a '$' — the woods UART
 * shares the phone with a wake-garbling habit (the first bytes after DFS idle
 * arrive mangled; see tools/panicwatch.py), so junk-tolerance is a requirement,
 * not politeness. A sentence without a checksum, or with a wrong one, is COUNTED
 * and dropped — never parsed "best effort". Only RMC and GGA are read; everything
 * else valid is counted as ignored.
 *
 * Position format: 1e-7 degrees in int32, the same fixed-point the mesh uses
 * (mesh_pos.h), so a fix drops into meshtastic_service without conversion.
 */
#ifndef NMEA_H
#define NMEA_H

#include <stdint.h>

struct NmeaFix {
  bool     valid;        // RMC status 'A' seen more recently than a 'V'
  int32_t  latI, lonI;   // 1e-7 degrees; only meaningful while/after valid
  uint32_t timeHms;      // hhmmss UTC from the last RMC/GGA that carried one (0 = none yet)
  uint32_t dateDmy;      // ddmmyy from the last RMC (0 = none yet)
  int      sats;         // GGA satellites in use (-1 = no GGA yet)
  int      hdopX10;      // GGA HDOP x10 (-1 = unknown)
  int      altM;         // GGA antenna altitude, metres, rounded toward zero (INT32-min sentinel unused; -10000 = unknown)

  /* ── THE LAST RMC'S OWN TIME, READ ONLY FROM THAT ONE SENTENCE (the clock's input) ──────
   * 🛑 timeHms/dateDmy above are NOT a timestamp and must never be paired into one: timeHms
   * is shared with GGA, and dateDmy survives an RMC whose date field was empty. Joined, they
   * can put 00:00:01 on YESTERDAY's date at midnight — a clock a whole day wrong from two
   * individually correct fields. clock_source.cpp reads ONLY these, and each is rewritten by
   * every RMC (absent = -1 / 0), so a field can never be inherited from an older sentence. */
  uint32_t rmcCount;     // RMC sentences parsed; a change means a NEW one completed
  char     rmcStatus;    // 'A' / 'V' as the sentence said it; 0 when empty or anything else
  char     rmcMode;      // NMEA 2.3+ mode indicator (A D E F M N P R S) after the date; 0 = absent
  int32_t  rmcHms;       // hhmmss, -1 when absent or not exactly "hhmmss[.f...]"
  int16_t  rmcMs;        // the time's fraction in ms (0 when none) - 0 at the M100's 1 Hz
  int32_t  rmcDmy;       // ddmmyy, -1 when absent or not exactly six digits

  /* ── MOTION, ALSO READ ONLY FROM THE LAST RMC (0.9.80, the Almanac's Position screen) ────
   * Speed over ground (RMC field 6, knots) and course over ground (field 7, degrees TRUE), in
   * fixed point - no floats here (see the file header). The rmc* rule above applies: EVERY RMC
   * rewrites both, and a field that is empty, absent or unreadable in THAT sentence is -1, so a
   * receiver that stops reporting a course (it does, when you stand still) can never show the
   * last walk's course as if it were now. A GGA never touches them. */
  int32_t  speedKnX100;  // knots x100: "0.36" -> 36, "5" -> 500, "12.345" -> 1234 (truncated); -1 = absent
  int32_t  courseX10;    // degrees true x10, 0..3599: "212.5" -> 2125, "360.0" -> 0; -1 = absent or > 360
};

class NmeaReader {
public:
  NmeaReader() { reset(); }
  void reset();

  /* Feed one raw UART byte. Returns true when an RMC or GGA sentence just
   * completed WITH a good checksum — including a no-fix RMC (status V), so the
   * caller can tell "GPS alive, sky not seen yet" from "no GPS talking at all".
   * Read the result from fix(). */
  bool feed(char c);

  const NmeaFix& fix() const { return f; }

  // Counters for the serial `gps` status line — what a bench session asks first.
  // bytes rising while sentences stays 0 = wrong baud, THE first bench question.
  uint32_t bytes()       const { return nBytes; }        // every byte fed
  uint32_t sentences()   const { return nSentences; }    // good checksum, any type
  uint32_t badChecksum() const { return nBadChecksum; }
  uint32_t overruns()    const { return nOverrun; }      // line hit the cap; discarded

  /* Parse one complete sentence (between '$' and '*', checksum already verified).
   * Public and static for the host tests; feed() is the production entry. */
  static bool parseSentence(const char* body, NmeaFix* f);

private:
  static const int LINE_CAP = 96;   // NMEA maximum is 82 including framing
  char     line[LINE_CAP];
  int      len;
  bool     inSentence;
  NmeaFix  f;
  uint32_t nBytes, nSentences, nBadChecksum, nOverrun;
};

#endif // NMEA_H
