/*
 * mesh_pos.cpp — see mesh_pos.h. Proven by tests/test_pos.cpp on the host.
 */

#include "mesh_pos.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------- protobuf

static uint32_t rdVarint(const uint8_t* d, size_t len, size_t* i) {
  uint32_t v = 0;
  int shift = 0;
  while (*i < len && (d[*i] & 0x80)) {
    v |= (uint32_t)(d[*i] & 0x7f) << shift;
    shift += 7;
    (*i)++;
  }
  if (*i < len) {
    v |= (uint32_t)(d[*i] & 0x7f) << shift;
    (*i)++;
  }
  return v;
}

static uint32_t rdFixed32(const uint8_t* d, size_t* i) {
  uint32_t v = (uint32_t)d[*i] | ((uint32_t)d[*i + 1] << 8) |
               ((uint32_t)d[*i + 2] << 16) | ((uint32_t)d[*i + 3] << 24);
  *i += 4;
  return v;
}

/* One walker for both messages: calls back per field, skips what it must.
 * Returns false on a malformed buffer (truncated fixed32/length overrun). */
typedef struct {
  // wire 0 fields land in vVarint, wire 5 in vFixed, wire 2 in ptr/len.
  void (*field)(void* ctx, uint32_t fieldNo, uint8_t wire,
                uint32_t vVarint, uint32_t vFixed, const uint8_t* p, uint32_t plen);
  void* ctx;
} PbWalk;

static bool pbWalk(const uint8_t* d, size_t len, const PbWalk* w) {
  size_t i = 0;
  while (i < len) {
    /* ⚠ The tag is a VARINT, not a byte. Field numbers >= 16 need two tag bytes
     * and REAL packets carry them: stock firmware appends precision_bits
     * (field 22/23) to every channel Position broadcast, and sats_in_view is
     * field 19. A one-byte read misparses everything after such a field —
     * caught by the adversarial review before it met a real packet. */
    uint32_t tag = rdVarint(d, len, &i);
    uint32_t fieldNo = tag >> 3;
    uint8_t wire = (uint8_t)(tag & 7);
    if (wire == 0) {
      uint32_t v = rdVarint(d, len, &i);
      w->field(w->ctx, fieldNo, 0, v, 0, NULL, 0);
    } else if (wire == 5) {
      if (i + 4 > len) return false;
      uint32_t v = rdFixed32(d, &i);
      w->field(w->ctx, fieldNo, 5, 0, v, NULL, 0);
    } else if (wire == 2) {
      uint32_t l = rdVarint(d, len, &i);
      if (l > len - i) return false;
      w->field(w->ctx, fieldNo, 2, 0, 0, d + i, l);
      i += l;
    } else if (wire == 1) {
      if (i + 8 > len) return false;
      i += 8;
    } else {
      return false;                          // wire types 3/4/6/7: not protobuf we know
    }
  }
  return true;
}

// ---------------------------------------------------------------- Position

typedef struct {
  int32_t lat, lon;
  uint32_t time;
  bool hasLat, hasLon;
} PosCtx;

static void posField(void* vctx, uint32_t f, uint8_t wire,
                     uint32_t vv, uint32_t vf, const uint8_t* p, uint32_t plen) {
  (void)vv; (void)p; (void)plen;
  PosCtx* c = (PosCtx*)vctx;
  if (wire != 5) return;
  if (f == 1)      { c->lat = (int32_t)vf; c->hasLat = true; }
  else if (f == 2) { c->lon = (int32_t)vf; c->hasLon = true; }
  else if (f == 4) { c->time = vf; }
}

bool meshPosParse(const uint8_t* pl, size_t len,
                  int32_t* latI, int32_t* lonI, uint32_t* timeOut) {
  if (!pl || len == 0) return false;
  PosCtx c;
  memset(&c, 0, sizeof(c));
  PbWalk w = { posField, &c };
  if (!pbWalk(pl, len, &w) || !c.hasLat || !c.hasLon) return false;
  if (latI) *latI = c.lat;
  if (lonI) *lonI = c.lon;
  if (timeOut) *timeOut = c.time;
  return true;
}

// ---------------------------------------------------------------- Waypoint

typedef struct {
  MeshWaypointMsg* out;
  bool hasLat, hasLon;
} WpCtx;

static void wpField(void* vctx, uint32_t f, uint8_t wire,
                    uint32_t vv, uint32_t vf, const uint8_t* p, uint32_t plen) {
  WpCtx* c = (WpCtx*)vctx;
  if (wire == 0) {
    if (f == 1)      c->out->id = vv;
    else if (f == 4) c->out->expire = vv;
    else if (f == 5) c->out->lockedTo = vv;
  } else if (wire == 5) {
    if (f == 2)      { c->out->latI = (int32_t)vf; c->hasLat = true; }
    else if (f == 3) { c->out->lonI = (int32_t)vf; c->hasLon = true; }
  } else if (wire == 2 && f == 6 && p) {
    size_t n = plen < MESH_WP_NAME_LEN - 1 ? plen : MESH_WP_NAME_LEN - 1;
    memcpy(c->out->name, p, n);
    c->out->name[n] = '\0';
  }
}

bool meshWaypointParse(const uint8_t* pl, size_t len, MeshWaypointMsg* out) {
  if (!pl || len == 0 || !out) return false;
  memset(out, 0, sizeof(*out));
  WpCtx c = { out, false, false };
  PbWalk w = { wpField, &c };
  if (!pbWalk(pl, len, &w)) return false;
  out->hasPos = c.hasLat && c.hasLon;
  /* A waypoint with an id and NO position is a DELETION — that absence is the
   * Meshtastic convention's marker (COVEY's waypoints.is_delete: position is
   * the discriminator, NOT expire, because expire == 0 legitimately means
   * "never expires"). So id-only payloads are VALID and the caller branches
   * on hasPos. Rejecting them here silently broke delete-on-COVEY ->
   * delete-on-phone until 2026-08-19. */
  return out->id != 0;
}

// ---------------------------------------------------------------- build

static size_t wrFixed32(uint8_t* out, uint8_t tag, uint32_t v) {
  out[0] = tag;
  out[1] = (uint8_t)(v >> 0);
  out[2] = (uint8_t)(v >> 8);
  out[3] = (uint8_t)(v >> 16);
  out[4] = (uint8_t)(v >> 24);
  return 5;
}

size_t meshPosBuild(uint8_t* out, int32_t latI, int32_t lonI, uint32_t time) {
  size_t d = 0;
  d += wrFixed32(out + d, 0x0d, (uint32_t)latI);   // field 1, sfixed32
  d += wrFixed32(out + d, 0x15, (uint32_t)lonI);   // field 2, sfixed32
  if (time) {
    d += wrFixed32(out + d, 0x25, time);           // field 4, fixed32
  }
  return d;
}

// ---------------------------------------------------------------- geometry

#define DEG(i)  ((double)(i) * 1e-7)
#define RAD(x)  ((x) * (M_PI / 180.0))

double meshPosDistanceM(int32_t aLatI, int32_t aLonI, int32_t bLatI, int32_t bLonI) {
  double p1 = RAD(DEG(aLatI)), p2 = RAD(DEG(bLatI));
  double dp = p2 - p1;
  double dl = RAD(DEG(bLonI) - DEG(aLonI));
  double s1 = sin(dp / 2), s2 = sin(dl / 2);
  double a = s1 * s1 + cos(p1) * cos(p2) * s2 * s2;
  return 6371000.0 * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

int meshPosBearingDeg(int32_t aLatI, int32_t aLonI, int32_t bLatI, int32_t bLonI) {
  double p1 = RAD(DEG(aLatI)), p2 = RAD(DEG(bLatI));
  double dl = RAD(DEG(bLonI) - DEG(aLonI));
  double y = sin(dl) * cos(p2);
  double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
  double deg = atan2(y, x) * (180.0 / M_PI);
  int b = (int)lround(deg);
  return ((b % 360) + 360) % 360;
}

const char* meshPosCompass8(int deg) {
  static const char* PTS[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
  return PTS[((deg + 22) / 45) & 7];
}

void meshPosFmtDist(double m, char* out, size_t cap) {
  if (m < 0) m = 0;
  if (m < 999.5) {                 // 999.6 must become "1.0km", never "1000m"
    snprintf(out, cap, "%dm", (int)lround(m));
  } else if (m < 10000.0) {
    snprintf(out, cap, "%.1fkm", m / 1000.0);
  } else {
    snprintf(out, cap, "%.0fkm", m / 1000.0);
  }
}
