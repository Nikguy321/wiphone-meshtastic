/*
 * test_pos.cpp — mesh_pos.cpp: Position/Waypoint payload parsing and the
 * distance/bearing math behind "1.4km NE of Camp". Reference values computed
 * independently in Python (same haversine/bearing formulas, double precision).
 */

#include "../WiPhone/mesh_pos.h"
#include <cstdio>
#include <cstring>
#include <cmath>

static int failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  ok  %s\n", name); } \
    else { printf("  FAIL %s (line %d)\n", name, __LINE__); failures++; } \
  } while (0)

static size_t putF32(uint8_t* b, uint8_t tag, uint32_t v) {
  b[0] = tag; b[1] = v; b[2] = v >> 8; b[3] = v >> 16; b[4] = v >> 24;
  return 5;
}

int main() {
  printf("test_pos\n");

  const int32_t SEA_LAT = 476062000, SEA_LON = -1223321000;   // 47.6062, -122.3321
  const int32_t PDX_LAT = 455152000, PDX_LON = -1226784000;   // 45.5152, -122.6784

  // ---- Position parse -------------------------------------------------------
  {
    uint8_t b[64];
    size_t n = 0;
    n += putF32(b + n, 0x0d, (uint32_t)SEA_LAT);     // latitude_i
    n += putF32(b + n, 0x15, (uint32_t)SEA_LON);     // longitude_i
    b[n++] = 0x18; b[n++] = 0x64;                    // altitude=100 (varint, skipped)
    n += putF32(b + n, 0x25, 1766000000u);           // time

    int32_t lat = 0, lon = 0; uint32_t t = 0;
    CHECK(meshPosParse(b, n, &lat, &lon, &t) &&
          lat == SEA_LAT && lon == SEA_LON && t == 1766000000u, "position parses");

    /* A REAL stock-firmware Position: high-numbered fields use TWO-byte varint
     * tags. sats_in_view = field 19 (tag 0x98 0x01), precision_bits = field 22
     * (tag 0xB0 0x01) — stock appends the latter to every channel broadcast.
     * A one-byte tag reader shears apart here (the adversarial review's catch). */
    uint8_t stock[64];
    size_t sn = 0;
    stock[sn++] = 0x98; stock[sn++] = 0x01; stock[sn++] = 0x07;   // sats_in_view=7
    sn += putF32(stock + sn, 0x0d, (uint32_t)SEA_LAT);
    sn += putF32(stock + sn, 0x15, (uint32_t)SEA_LON);
    stock[sn++] = 0xB0; stock[sn++] = 0x01; stock[sn++] = 0x20;   // precision_bits=32
    CHECK(meshPosParse(stock, sn, &lat, &lon, &t) &&
          lat == SEA_LAT && lon == SEA_LON, "field>=16 tags (stock precision_bits) survive");

    CHECK(!meshPosParse(b, 5, &lat, &lon, &t), "lat-only position refused");
    CHECK(!meshPosParse(b, 3, &lat, &lon, &t), "truncated fixed32 refused");
    uint8_t junk[4] = { 0x0b, 0x01, 0x02, 0x03 };    // wire type 3: not our protobuf
    CHECK(!meshPosParse(junk, sizeof(junk), &lat, &lon, &t), "unknown wire type refused");
  }

  // ---- build -> parse round trip -------------------------------------------
  {
    uint8_t b[20];
    size_t n = meshPosBuild(b, PDX_LAT, PDX_LON, 1766000123u);
    int32_t lat, lon; uint32_t t;
    CHECK(n == 15 && meshPosParse(b, n, &lat, &lon, &t) &&
          lat == PDX_LAT && lon == PDX_LON && t == 1766000123u, "own announce round-trips");
    n = meshPosBuild(b, PDX_LAT, PDX_LON, 0);
    CHECK(n == 10 && meshPosParse(b, n, &lat, &lon, &t) && t == 0, "time omitted when unknown");

    /* 🔑 THE TWO SIZES ABOVE ARE AN AIRTIME CONTRACT, not a formatting detail.
     * 15 B payload + 6 B Data protobuf framing + 16 B MeshPacketHeader = 37 B
     * on air, which at the LongFast registers this repo writes (SF11, BW250,
     * CR4/5, preamble 16 — mesh_phy.cpp:114) is 518 ms of transmission. Every
     * duty-cycle number behind the offered beacon intervals is derived from
     * that. If either size changes, the intervals need re-justifying and this
     * assertion is what says so. */

    /* Southern hemisphere AND western longitude: both axes negative, which is
     * the sfixed32 case most likely to be got wrong by a sign-extension slip.
     * (Hobart is -42.88, 147.33 — one negative axis; this is Punta Arenas.) */
    const int32_t PA_LAT = -531629000, PA_LON = -708546000;
    n = meshPosBuild(b, PA_LAT, PA_LON, 1766000123u);
    CHECK(n == 15 && meshPosParse(b, n, &lat, &lon, &t) &&
          lat == PA_LAT && lon == PA_LON && t == 1766000123u,
          "negative lat AND lon round-trip");
  }

  // ---- Waypoint parse -------------------------------------------------------
  {
    uint8_t b[64];
    size_t n = 0;
    b[n++] = 0x08; b[n++] = 0x2a;                    // id=42
    n += putF32(b + n, 0x15, (uint32_t)SEA_LAT);     // latitude_i (field 2)
    n += putF32(b + n, 0x1d, (uint32_t)SEA_LON);     // longitude_i (field 3)
    b[n++] = 0x20; b[n++] = 0x00;                    // expire=0
    b[n++] = 0x32; b[n++] = 4;                       // name="Camp"
    memcpy(b + n, "Camp", 4); n += 4;

    MeshWaypointMsg wp;
    CHECK(meshWaypointParse(b, n, &wp) && wp.id == 42 && wp.latI == SEA_LAT &&
          wp.lonI == SEA_LON && wp.expire == 0 && strcmp(wp.name, "Camp") == 0,
          "waypoint parses");

    uint8_t noId[16];
    size_t m = 0;
    m += putF32(noId + m, 0x15, (uint32_t)SEA_LAT);
    m += putF32(noId + m, 0x1d, (uint32_t)SEA_LON);
    CHECK(!meshWaypointParse(noId, m, &wp), "waypoint without id refused");

    /* id + NO position = a DELETION marker (COVEY's map sends exactly this on
     * delete — waypoints.is_delete: absence of position is the discriminator).
     * Must PARSE (hasPos false), not be rejected: rejecting it silently broke
     * delete-on-COVEY -> delete-on-phone. */
    uint8_t del[8];
    m = 0;
    del[m++] = 0x08; del[m++] = 0x2a;                // id=42, nothing else
    CHECK(meshWaypointParse(del, m, &wp) && wp.id == 42 && !wp.hasPos,
          "id-only waypoint parses as a delete marker");

    // Oversized name is truncated, never overrun (ASan watches).
    uint8_t big[64];
    m = 0;
    big[m++] = 0x08; big[m++] = 0x01;
    m += putF32(big + m, 0x15, (uint32_t)SEA_LAT);
    m += putF32(big + m, 0x1d, (uint32_t)SEA_LON);
    big[m++] = 0x32; big[m++] = 30;
    memset(big + m, 'A', 30); m += 30;
    CHECK(meshWaypointParse(big, m, &wp) && strlen(wp.name) == MESH_WP_NAME_LEN - 1,
          "30-char name truncated safely");
  }

  // ---- geometry (python-checked references) ---------------------------------
  {
    double d = meshPosDistanceM(SEA_LAT, SEA_LON, PDX_LAT, PDX_LON);
    CHECK(fabs(d - 234010.5) < 30.0, "Seattle->Portland ~234.0 km");
    CHECK(meshPosBearingDeg(SEA_LAT, SEA_LON, PDX_LAT, PDX_LON) == 187, "bearing 187 (S)");

    // Hunt scale: ~1 km due east at 47N, ~500 m due north.
    d = meshPosDistanceM(470000000, -1200000000, 470000000, -1199868290);
    CHECK(fabs(d - 998.8) < 2.0, "1 km east at hunt scale");
    CHECK(meshPosBearingDeg(470000000, -1200000000, 470000000, -1199868290) == 90, "east = 90");
    d = meshPosDistanceM(470000000, -1200000000, 470044966, -1200000000);
    CHECK(fabs(d - 500.0) < 1.0, "500 m north");
    CHECK(meshPosBearingDeg(470000000, -1200000000, 470044966, -1200000000) == 0, "north = 0");
    CHECK(meshPosDistanceM(SEA_LAT, SEA_LON, SEA_LAT, SEA_LON) < 0.01, "zero distance to self");
  }

  /* ---- the position beacon's movement gate ---------------------------------
   * Offsets computed independently in Python with the same haversine (the
   * method this file already uses for the geometry block above):
   *   99 m due north of 47.0,-120.0  = +8903 in 1e-7 deg  (measured 98.997 m)
   *  101 m due north                 = +9083              (measured 100.998 m)
   * The pair straddles MESH_POS_MIN_MOVE_M from both sides on purpose: an
   * off-by-one in the comparison shows up as exactly one of them flipping. */
  {
    const int32_t LAT = 470000000, LON = -1200000000;

    CHECK(meshPosShouldBeacon(false, 0, 0, LAT, LON, 0),
          "first beacon after boot always transmits");

    // Parked: suppressed for MESH_POS_MAX_SKIPS slots, then forced out.
    CHECK(!meshPosShouldBeacon(true, LAT, LON, LAT, LON, 0) &&
          !meshPosShouldBeacon(true, LAT, LON, LAT, LON, 1) &&
          !meshPosShouldBeacon(true, LAT, LON, LAT, LON, 2),
          "stationary phone stays quiet for three slots");
    CHECK(meshPosShouldBeacon(true, LAT, LON, LAT, LON, MESH_POS_MAX_SKIPS),
          "keep-alive floor: the fourth slot transmits anyway");

    CHECK(!meshPosShouldBeacon(true, LAT, LON, LAT + 8903, LON, 0), "99 m does not beacon");
    CHECK(meshPosShouldBeacon(true, LAT, LON, LAT + 9083, LON, 0), "101 m beacons");

    // 5 km: transmits at every skip count, including a freshly reset one.
    CHECK(meshPosShouldBeacon(true, LAT, LON, LAT, LON + 658550, 0) &&
          meshPosShouldBeacon(true, LAT, LON, LAT, LON + 658550, 2),
          "5 km beacons regardless of skip count");

    /* Sign handling: a step ACROSS the antimeridian is 111 m, not 40,000 km.
     * If meshPosDistanceM ever loses the wrap, the gate would read a two-metre
     * shuffle at the date line as a continent-crossing move and transmit every
     * slot — the one place this arithmetic can silently cost real airtime. */
    CHECK(meshPosShouldBeacon(true, 0, 1799995000, 0, -1799995000, 0),
          "antimeridian: 111 m across the line beacons");
    CHECK(!meshPosShouldBeacon(true, 0, 1799999500, 0, -1799999500, 0),
          "antimeridian: 11 m across the line does not");
    // ...and across the equator, where the latitude sign flips.
    CHECK(meshPosShouldBeacon(true, -4497, 0, 4497, 0, 0) &&
          !meshPosShouldBeacon(true, -2000, 0, 2000, 0, 0),
          "equator crossing: 100 m beacons, 44 m does not");
  }

  // ---- presentation ---------------------------------------------------------
  {
    CHECK(strcmp(meshPosCompass8(0), "N") == 0 && strcmp(meshPosCompass8(359), "N") == 0 &&
          strcmp(meshPosCompass8(23), "NE") == 0 && strcmp(meshPosCompass8(187), "S") == 0 &&
          strcmp(meshPosCompass8(292), "W") == 0 && strcmp(meshPosCompass8(293), "NW") == 0,
          "compass points");
    char s[16];
    meshPosFmtDist(850, s, sizeof(s));    CHECK(strcmp(s, "850m") == 0, "850m");
    meshPosFmtDist(1437, s, sizeof(s));   CHECK(strcmp(s, "1.4km") == 0, "1.4km");
    meshPosFmtDist(23400, s, sizeof(s));  CHECK(strcmp(s, "23km") == 0, "23km");
    meshPosFmtDist(999.6, s, sizeof(s));  CHECK(strcmp(s, "1.0km") == 0, "999.6 rounds up to km");
  }

  if (failures) {
    printf("test_pos: %d FAILURE(S)\n", failures);
    return 1;
  }
  printf("test_pos: all passed\n");
  return 0;
}
