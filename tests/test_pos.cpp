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
