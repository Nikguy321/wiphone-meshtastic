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
