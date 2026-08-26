/*
 * mesh_pos.h — Meshtastic position & waypoint payloads, and the small geometry
 * needed to say "1.4km NE of Camp".
 *
 * ⚠ THIS HEADER USED TO SAY "the phone has NO GPS (yet): it only LISTENS".
 * That has been false since the woods backplate shipped — the u-blox M10 is
 * read as NMEA by nmea.cpp and its fix reaches meshtastic_service. Both
 * directions now meet here:
 *   INBOUND  — COVEY broadcasts its Position every 5 minutes and shares
 *              Waypoints from its map; this file turns those payloads into
 *              distances and bearings from a chosen reference point (a
 *              waypoint, this phone's own GPS, or the user-declared pin).
 *   OUTBOUND — meshPosBuild() serves the manual "I'm here" pin announce AND
 *              the periodic GPS position beacon (meshtastic_service.cpp).
 *
 * Wire formats verified against meshtastic/protobufs master (2026-08-19):
 *   Position (port 3): latitude_i=1 sfixed32, longitude_i=2 sfixed32,
 *                      altitude=3 int32, time=4 fixed32 — lat/lon are 1e-7 deg.
 *   Waypoint (port 8): id=1, latitude_i=2, longitude_i=3, expire=4,
 *                      locked_to=5, name=6.
 *
 * Self-contained, no Arduino headers: tests/test_pos.cpp proves the shipping
 * bytes and math on the host (book_hash.h explains the philosophy).
 */
#ifndef MESH_POS_H
#define MESH_POS_H

#include <stddef.h>
#include <stdint.h>

#define MESH_WP_NAME_LEN 20     // bounded copy of Waypoint.name

typedef struct {
  uint32_t id;
  int32_t  latI, lonI;          // 1e-7 degrees
  uint32_t expire;              // unix seconds, 0 = never
  uint32_t lockedTo;            // node that owns edits, 0 = anyone
  char     name[MESH_WP_NAME_LEN];
  bool     hasPos;              // both lat and lon were present
} MeshWaypointMsg;

// Parse a Position payload. True only when BOTH latitude_i and longitude_i
// were present (a fix-less position is useless here). timeOut optional.
bool meshPosParse(const uint8_t* pl, size_t len,
                  int32_t* latI, int32_t* lonI, uint32_t* timeOut);

// Parse a Waypoint payload. True when id != 0; check out->hasPos — a waypoint
// WITHOUT a position is a DELETION marker (the Meshtastic convention: absence
// of position is the discriminator, since expire==0 means "never expires").
bool meshWaypointParse(const uint8_t* pl, size_t len, MeshWaypointMsg* out);

/* Build a minimal Position payload {latitude_i, longitude_i, [time]}. Returns
 * bytes written: 15 with a known clock, 10 without.
 *
 * ⚠ TWO CALLERS WITH DIFFERENT PROVENANCE share these bytes now — the manual
 * "I'm here" pin (a statement by the user) and the periodic GPS beacon (a
 * measurement). The payload cannot tell them apart and neither can a receiver,
 * so the distinction lives entirely in which caller decided to transmit. That
 * is why the beacon's freshness gate sits in the service, not here.
 *
 * The 15/10 sizes are asserted by tests/test_pos.cpp on purpose: the beacon's
 * whole airtime budget (37 B on air, 518 ms at SF11/BW250) is computed from
 * them, so a silent change to this function invalidates the duty-cycle
 * reasoning behind the offered intervals. */
size_t meshPosBuild(uint8_t* out, int32_t latI, int32_t lonI, uint32_t time);

// Great-circle distance in meters (haversine, R=6371km — meters-accurate at
// hunt scale, which is all the display shows).
double meshPosDistanceM(int32_t aLatI, int32_t aLonI, int32_t bLatI, int32_t bLonI);

// Initial bearing from a to b, degrees 0..359 (0 = true north).
int meshPosBearingDeg(int32_t aLatI, int32_t aLonI, int32_t bLatI, int32_t bLonI);

// "N" "NE" "E" ... for a bearing in degrees.
const char* meshPosCompass8(int deg);

// "850m" / "1.4km" / "23km".
void meshPosFmtDist(double meters, char* out, size_t cap);

/* ---- The position beacon's movement gate --------------------------------
 * A stationary phone re-sending an identical position is pure waste of a
 * shared band. A phone that goes silent for hours looks like a dead radio,
 * and on a hunting channel "he's still on the stand" is real information.
 * So: suppress the slot when the fix has barely moved, but never suppress
 * more than MESH_POS_MAX_SKIPS in a row. At a 15-minute interval a parked
 * phone therefore transmits somewhere between every 15 and every 60 minutes.
 *
 * 100 m matches upstream's broadcast_smart_minimum_distance.
 * UNKNOWN: that default is upstream knowledge, not verifiable in this tree.
 *
 * This is the ONE piece of the beacon that is arithmetic rather than policy,
 * so it lives in this Arduino-free file where tests/test_pos.cpp can prove it. */
#define MESH_POS_MIN_MOVE_M   100.0
#define MESH_POS_MAX_SKIPS    3

/* ── HOW GOOD A FIX HAS TO BE BEFORE IT IS SOMEBODY ELSE'S PROBLEM ────────────────────────
 * ⚠ THREE SATELLITES IS A 2D FIX: it solves lat/lon by ASSUMING an altitude, and a wrong
 * assumption pushes the error SIDEWAYS — into the one number a hunting party reads. MEASURED
 * 2026-08-25: an indoor WiPhone reported `sats=3 hdop=6.4` at 47.33821,-122.16501 while the
 * phone was actually at 47.4965,-122.3749 — **about 20 km out**, and nothing would have
 * refused it. Four is the arithmetic minimum for a real 3D fix, so this is the standard bar
 * and not a strict one; under canopy a working receiver tracks five to ten. HDOP 10 is the
 * usual "poor" boundary and is a backstop, not the main gate. */
#define MESH_POS_MIN_SATS         4
#define MESH_POS_MAX_HDOP_X10   100

/* True when this slot should actually transmit. `skipRuns` is how many
 * consecutive slots have already been suppressed. haveLast == false (nothing
 * transmitted since boot) always transmits — the first beacon is what tells
 * the mesh this node exists. */
/* Is this fix good enough to put on other people's maps?
 * ⚠ REFUSES ONLY WHAT IT POSITIVELY KNOWS IS BAD: both fields are -1 when the receiver has
 * not said (they come from GGA), and an unknown is NOT a failure — refusing on silence would
 * break any receiver that does not emit GGA, which is a different bug than the one this fixes.
 * Pure, so tests/test_pos.cpp can prove it. */
bool meshPosFixUsable(int sats, int hdopX10);

bool meshPosShouldBeacon(bool haveLast, int32_t lastLatI, int32_t lastLonI,
                         int32_t latI, int32_t lonI, int skipRuns);

#endif // MESH_POS_H
