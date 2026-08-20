/*
 * mesh_pos.h — Meshtastic position & waypoint payloads, and the small geometry
 * needed to say "1.4km NE of Camp".
 *
 * The phone has NO GPS (yet): it never computes a fix, it only LISTENS —
 * COVEY broadcasts its Position every 5 minutes and shares Waypoints from its
 * map, and this file turns those payloads into distances and bearings from a
 * chosen reference point (a waypoint, or the phone's own user-declared pin).
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

// Build a minimal Position payload {latitude_i, longitude_i, [time]} for the
// phone's own "I'm here" announce. Returns bytes written (max 15).
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

#endif // MESH_POS_H
