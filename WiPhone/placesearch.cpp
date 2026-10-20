/*
 * placesearch.cpp - see placesearch.h. Pure C++: no Arduino, no IDF, no allocation, no recursion
 * (json_read.h skips a nested value by counting brackets). Every buffer is the caller's; the locals
 * are small (one PlaceResult of ~230 bytes while a feature is read: the worker's 8 KB stack decides).
 */
#include "placesearch.h"
#include "json_read.h"
#include "gemini.h"       // geminiFoldNext: the Wikipedia text's fold (COVEY and the X4 port the same table)
#include "mesh_pos.h"     // meshPosDistanceM / meshPosBearingDeg / meshPosCompass8 (the lists' helpers)
#include "units.h"        // unitsFmtDist (the lists' distance text)
#include "map_tiles.h"    // mapDegToI7, mapLatLonToWorld

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// ── the fold (one ASCII byte at a time, through gemini.cpp's table) ──────────────────────

struct PFold {
  const char* s;
  const char* rep;
  char        one[2];
};

static void pfInit(PFold* f, const char* s) {
  f->s = s ? s : "";
  f->rep = "";
}

/* The next folded byte; -1 at the end (and every call after it). */
static int pfGet(PFold* f) {
  while (!*f->rep) {
    f->rep = geminiFoldNext(&f->s, f->one);
    if (!f->rep) {
      f->rep = "";
      return -1;
    }
  }
  return (unsigned char)*f->rep++;
}

/* `in` folded to ASCII into out (< cap, cut at its end): whitespace and control characters one
 * space, none at either end. Returns the length. */
static size_t foldInto(const char* in, char* out, size_t cap) {
  if (!out || cap == 0) {
    return 0;
  }
  PFold f;
  pfInit(&f, in);
  size_t n = 0;
  bool space = false;
  int c;
  while ((c = pfGet(&f)) >= 0) {
    if (c <= 0x20 || c == 0x7F) {
      space = n > 0;
      continue;
    }
    if (space) {
      if (n + 1 >= cap) break;
      out[n++] = ' ';
      space = false;
    }
    if (n + 1 >= cap) break;
    out[n++] = (char)c;
  }
  out[n] = '\0';
  return n;
}

/* Append `s` to out (n bytes used, < cap): as much as fits. */
static void catCut(char* out, size_t cap, size_t* n, const char* s) {
  while (*s && *n + 1 < cap) {
    out[(*n)++] = *s++;
  }
  out[*n] = '\0';
}

// ── the query ────────────────────────────────────────────────────────────────────────────

size_t placeQuery(const char* q, char* out, size_t cap) {
  if (out && cap) {
    out[0] = '\0';
  }
  /* Collapsed into b up to one character PAST the limit: that character says whether the cut
   * falls at a word. A character is a byte that is not a UTF-8 continuation byte. */
  char b[PLACE_QUERY_RAW + 8];
  size_t n = 0;
  int chars = 0;
  bool space = false, cut = false, cutAtSpace = false;
  for (const unsigned char* p = (const unsigned char*)q; q && *p; p++) {
    const unsigned char c = *p;
    if (c <= 0x20 || c == 0x7F) {
      space = n > 0;                             // a run of them is one space (none at the start)
      continue;
    }
    if (space) {
      if (chars >= PLACE_QUERY_CHARS) {
        cut = true;
        cutAtSpace = true;                       // the limit ends a word
        break;
      }
      b[n++] = ' ';
      chars++;
      space = false;
    }
    if ((c & 0xC0) != 0x80) {                    // a character starts
      if (chars >= PLACE_QUERY_CHARS) {
        cut = true;                              // mid-word: back to the last space below
        break;
      }
      chars++;
    }
    if (n + 1 >= sizeof(b)) {
      cut = true;                                // cannot happen under the cap: a guard
      break;
    }
    b[n++] = (char)c;
  }
  size_t len = n;
  if (cut && !cutAtSpace) {
    size_t sp = 0;
    bool found = false;
    for (size_t i = n; i > 0; i--) {
      if (b[i - 1] == ' ') {
        sp = i - 1;
        found = true;
        break;
      }
    }
    len = found ? sp : n;                        // one long word: at the character boundary n is
  }
  while (len > 0 && b[len - 1] == ' ') len--;
  if (!out || len + 1 > cap) {
    return 0;
  }
  memcpy(out, b, len);
  out[len] = '\0';
  return len;
}

// ── the template and the URL ─────────────────────────────────────────────────────────────

static bool hostChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
}

static void setWhy(char* why, size_t cap, const char* text) {
  if (why && cap) {
    snprintf(why, cap, "%s", text);
  }
}

static const char* const PLACEHOLDERS[] = { "{q}", "{lat}", "{lon}", "{n}" };

int placeTemplateCheck(const char* t, char* why, size_t whyCap) {
  setWhy(why, whyCap, "");
  if (!t || !*t) {
    setWhy(why, whyCap, "no template given");
    return 0;
  }
  const size_t len = strlen(t);
  if (len >= PLACE_TMPL_MAX) {
    setWhy(why, whyCap, "the template is too long (199 characters at most)");
    return 0;
  }
  for (size_t i = 0; i < len; i++) {
    const unsigned char c = (unsigned char)t[i];
    if (c < 0x21 || c > 0x7E) {
      setWhy(why, whyCap, "a space or a non-ASCII character in the template");
      return 0;
    }
  }
  if (strncmp(t, "https://", 8) != 0) {
    setWhy(why, whyCap, "https:// only (the roots pinned are Let's Encrypt's ISRG roots)");
    return 0;
  }
  const char* p = t + 8;
  const char* h = p;
  while (hostChar(*p)) p++;
  if (p == h) {
    setWhy(why, whyCap, "no host after https://");
    return 0;
  }
  /* The worker connects to 443 only (NetHttpReq has a host, no port): a :port accepted here
   * would fail every search with "could not reach" after `maps searchurl` said "set". */
  if (*p == ':') {
    setWhy(why, whyCap, "no :port - the phone connects to 443 only");
    return 0;
  }
  if (*p != '\0' && *p != '/' && *p != '?') {
    setWhy(why, whyCap, "the host may use letters, digits, . and - only");
    return 0;
  }
  bool haveQ = false;
  for (p = t; *p; p++) {
    if (*p == '}') {
      setWhy(why, whyCap, "a stray } in the template");
      return 0;
    }
    if (*p != '{') {
      continue;
    }
    size_t k = 0;
    for (; k < sizeof(PLACEHOLDERS) / sizeof(PLACEHOLDERS[0]); k++) {
      const size_t l = strlen(PLACEHOLDERS[k]);
      if (!strncmp(p, PLACEHOLDERS[k], l)) {
        if (k == 0) haveQ = true;
        p += l - 1;
        break;
      }
    }
    if (k == sizeof(PLACEHOLDERS) / sizeof(PLACEHOLDERS[0])) {
      setWhy(why, whyCap, "only {q} {lat} {lon} {n} may be in braces");
      return 0;
    }
  }
  if (!haveQ) {
    setWhy(why, whyCap, "the template needs {q} (where the typed text goes)");
    return 0;
  }
  return 1;
}

size_t placeTemplateHost(const char* t, char* out, size_t cap) {
  if (!out || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  if (!t || !*t) {
    t = PLACE_URL_DEFAULT;
  }
  const char* p = strstr(t, "://");
  p = p ? p + 3 : t;
  const char* h = p;
  while (hostChar(*p)) p++;
  const size_t n = (size_t)(p - h);
  if (n == 0 || n + 1 > cap) {
    return 0;
  }
  memcpy(out, h, n);
  out[n] = '\0';
  return n;
}

size_t placeEncode(const char* s, char* out, size_t cap) {
  if (!out || cap == 0) {
    return 0;
  }
  static const char HEX[] = "0123456789ABCDEF";
  size_t n = 0;
  for (const unsigned char* p = (const unsigned char*)s; s && *p; p++) {
    const unsigned char c = *p;
    const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '.' || c == '_' || c == '~';
    if (keep) {
      if (n + 1 >= cap) {
        out[0] = '\0';
        return 0;
      }
      out[n++] = (char)c;
    } else {
      if (n + 3 >= cap) {
        out[0] = '\0';
        return 0;
      }
      out[n++] = '%';
      out[n++] = HEX[c >> 4];
      out[n++] = HEX[c & 15];
    }
  }
  out[n] = '\0';
  return n;
}

size_t placeFmtBias(double v, char* out, size_t cap) {
  if (!out || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  if (!(v == v) || v > 1000.0 || v < -1000.0) {   // NaN or absurd: no bias at all
    return 0;
  }
  long s = lround(v * 1000.0);                      // PLACE_BIAS_DECIMALS = 3: half away from zero
  const bool neg = s < 0;
  if (neg) s = -s;
  const int n = snprintf(out, cap, "%s%ld.%03ld", neg ? "-" : "", s / 1000, s % 1000);
  if (n <= 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n;
}

size_t placeBuildUrl(const char* tmpl, const char* query, double lat, double lon, int limit, char* out,
                     size_t cap) {
  if (!out || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  if (!query || !*query) {
    return 0;                                     // an empty query sends nothing
  }
  const char* t = PLACE_URL_DEFAULT;
  if (tmpl && *tmpl) {
    if (!placeTemplateCheck(tmpl, NULL, 0)) {
      return 0;
    }
    t = tmpl;
  }
  if (limit < 1) limit = 1;
  if (limit > PLACE_LIMIT) limit = PLACE_LIMIT;
  size_t n = 0;
  for (const char* p = t; *p;) {
    if (*p != '{') {
      if (n + 1 >= cap) {
        out[0] = '\0';
        return 0;
      }
      out[n++] = *p++;
      continue;
    }
    size_t k = 0;
    if (!strncmp(p, "{q}", 3)) {
      k = placeEncode(query, out + n, cap - n);
      p += 3;
    } else if (!strncmp(p, "{lat}", 5)) {
      k = placeFmtBias(lat, out + n, cap - n);
      p += 5;
    } else if (!strncmp(p, "{lon}", 5)) {
      k = placeFmtBias(lon, out + n, cap - n);
      p += 5;
    } else if (!strncmp(p, "{n}", 3)) {
      const int w = snprintf(out + n, cap - n, "%d", limit);
      k = (w > 0 && (size_t)w < cap - n) ? (size_t)w : 0;
      p += 3;
    } else {
      out[0] = '\0';                              // unreachable for a checked template
      return 0;
    }
    if (k == 0) {
      out[0] = '\0';
      return 0;
    }
    n += k;
  }
  out[n] = '\0';
  return n;
}

// ── the reply ────────────────────────────────────────────────────────────────────────────

static bool coordOk(double lat, double lon) {
  return lat == lat && lon == lon && lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

/* At the value of "coordinates": [lon, lat(, alt...)] - numbers. False = the body is not JSON /
 * cut short; *good = two finite numbers in range came. Anything else (a string, null, an object)
 * is skipped and not good. */
static bool readCoords(Js* j, double* lat, double* lon, bool* good) {
  *good = false;
  jsWs(j);
  if (j->p >= j->e) {
    return false;
  }
  if (*j->p != '[') {
    return jsSkip(j);
  }
  jsOpen(j, '[');
  int i = 0;
  bool nums = true;
  for (;;) {
    const int r = jsArrNext(j);
    if (r < 0) {
      return false;
    }
    if (r == 0) {
      break;
    }
    double v = 0;
    if (jsNumber(j, &v) != 1) {
      nums = false;                               // null, a string, an object: skipped (a cut body
    } else if (i == 0) {                          // fails the next jsArrNext)
      *lon = v;
    } else if (i == 1) {
      *lat = v;
    }
    i++;
  }
  *good = nums && i >= 2 && coordOk(*lat, *lon);
  return true;
}

/* At the value of "geometry": {"type":"Point","coordinates":[...]}. */
static bool readGeometry(Js* j, double* lat, double* lon, bool* good) {
  *good = false;
  jsWs(j);
  if (j->p >= j->e) {
    return false;
  }
  if (*j->p != '{') {
    return jsSkip(j);
  }
  jsOpen(j, '{');
  char key[16];
  for (;;) {
    const int r = jsObjNext(j, key, sizeof(key));
    if (r < 0) {
      return false;
    }
    if (r == 0) {
      break;
    }
    if (!strcmp(key, "coordinates")) {
      bool g = false;
      if (!readCoords(j, lat, lon, &g)) {
        return false;
      }
      *good = g;                                  // a second "coordinates": the last one counts
    } else if (!jsSkip(j)) {
      return false;
    }
  }
  return true;
}

/* At the value of "properties": the seven strings kept, everything else skipped. */
static bool readProps(Js* j, PlaceResult* r) {
  jsWs(j);
  if (j->p >= j->e) {
    return false;
  }
  if (*j->p != '{') {
    return jsSkip(j);
  }
  jsOpen(j, '{');
  char key[16];
  for (;;) {
    const int n = jsObjNext(j, key, sizeof(key));
    if (n < 0) {
      return false;
    }
    if (n == 0) {
      break;
    }
    char*  into = NULL;
    size_t cap = 0;
    if (!strcmp(key, "name"))           { into = r->name;     cap = sizeof(r->name); }
    else if (!strcmp(key, "osm_key"))   { into = r->osmKey;   cap = sizeof(r->osmKey); }
    else if (!strcmp(key, "osm_value")) { into = r->osmValue; cap = sizeof(r->osmValue); }
    else if (!strcmp(key, "type"))      { into = r->type;     cap = sizeof(r->type); }
    else if (!strcmp(key, "city"))      { into = r->city;     cap = sizeof(r->city); }
    else if (!strcmp(key, "state"))     { into = r->state;    cap = sizeof(r->state); }
    else if (!strcmp(key, "country"))   { into = r->country;  cap = sizeof(r->country); }
    if (into) {
      into[0] = '\0';
      if (!jsStrInto(j, into, cap)) {
        return false;
      }
    } else if (!jsSkip(j)) {
      return false;
    }
  }
  return true;
}

/* One feature into r (zeroed first). False = not JSON / cut short; *keep = it has a name and
 * usable coordinates. */
static bool readFeature(Js* j, PlaceResult* r, bool* keep) {
  memset(r, 0, sizeof(*r));
  *keep = false;
  if (!jsOpen(j, '{')) {
    return false;                                 // a feature that is not an object: not Photon's
  }
  bool good = false;
  char key[16];
  for (;;) {
    const int n = jsObjNext(j, key, sizeof(key));
    if (n < 0) {
      return false;
    }
    if (n == 0) {
      break;
    }
    if (!strcmp(key, "geometry")) {
      if (!readGeometry(j, &r->lat, &r->lon, &good)) {
        return false;
      }
    } else if (!strcmp(key, "properties")) {
      if (!readProps(j, r)) {
        return false;
      }
    } else if (!jsSkip(j)) {
      return false;
    }
  }
  *keep = good && r->name[0] != '\0';
  return true;
}

/* The same name (case-blind) within PLACE_DEDUPE_M of one already kept. */
static bool duplicate(const PlaceResult* kept, int n, const PlaceResult* r) {
  const int32_t la = mapDegToI7(r->lat), lo = mapDegToI7(r->lon);
  for (int i = 0; i < n; i++) {
    if (strcasecmp(kept[i].name, r->name) == 0 &&
        meshPosDistanceM(la, lo, mapDegToI7(kept[i].lat), mapDegToI7(kept[i].lon)) <= PLACE_DEDUPE_M) {
      return true;
    }
  }
  return false;
}

int placeParse(const char* body, size_t len, PlaceResult* out, int cap, int* n) {
  if (n) {
    *n = 0;
  }
  if (!body || len == 0 || !out || cap <= 0 || !n) {
    return PLACE_PARSE_BAD;
  }
  if (cap > PLACE_LIMIT) {
    cap = PLACE_LIMIT;
  }
  Js j = { body, body + len };
  if (!jsOpen(&j, '{')) {
    return PLACE_PARSE_BAD;                       // not an object (an HTML page, text)
  }
  bool haveFeatures = false, typeOk = false, typeSeen = false;
  /* 24: "FeatureCollection" is 17; a longer type is cut and cannot match. */
  char key[16], type[24];
  for (;;) {
    const int r = jsObjNext(&j, key, sizeof(key));
    if (r < 0) {
      return PLACE_PARSE_BAD;
    }
    if (r == 0) {
      break;
    }
    if (!strcmp(key, "type")) {
      type[0] = '\0';
      if (!jsStrInto(&j, type, sizeof(type))) {
        return PLACE_PARSE_BAD;
      }
      typeSeen = true;
      typeOk = !strcmp(type, "FeatureCollection");
    } else if (!strcmp(key, "features")) {
      if (haveFeatures || !jsOpen(&j, '[')) {
        return PLACE_PARSE_BAD;                   // twice, or not an array
      }
      haveFeatures = true;
      for (;;) {
        const int a = jsArrNext(&j);
        if (a < 0) {
          return PLACE_PARSE_BAD;
        }
        if (a == 0) {
          break;
        }
        /* Read into the slot after the kept ones when there is room, else into a scratch slot:
         * every feature is read whole, so a cut body is bad wherever it is cut. */
        PlaceResult scratch;
        PlaceResult* slot = (*n < cap) ? &out[*n] : &scratch;
        bool keep = false;
        if (!readFeature(&j, slot, &keep)) {
          return PLACE_PARSE_BAD;
        }
        if (keep && *n < cap && !duplicate(out, *n, slot)) {
          (*n)++;
        }
      }
    } else if (!jsSkip(&j)) {
      return PLACE_PARSE_BAD;
    }
  }
  jsWs(&j);
  if (j.p != j.e) {
    return PLACE_PARSE_BAD;                       // a second root, or text after the object
  }
  if (!haveFeatures || !typeSeen || !typeOk) {
    *n = 0;
    return PLACE_PARSE_BAD;                       // not a FeatureCollection
  }
  return PLACE_PARSE_OK;
}

// ── the rows ─────────────────────────────────────────────────────────────────────────────

/* One part of the second line: folded, '_' a space, appended after ", " when something is there. */
static void subPart(char* out, size_t cap, size_t* n, const char* raw) {
  char f[PLACE_LOCAL_MAX];
  if (!raw || !raw[0] || foldInto(raw, f, sizeof(f)) == 0) {
    return;
  }
  for (char* p = f; *p; p++) {
    if (*p == '_') *p = ' ';
  }
  if (*n > 0) {
    catCut(out, cap, n, ", ");
  }
  catCut(out, cap, n, f);
}

int placeRows(const PlaceResult* r, int n, double refLat, double refLon, int units, const char* homeCountry,
              PlaceRow* rows) {
  if (!r || !rows || n <= 0) {
    return 0;
  }
  if (n > PLACE_LIMIT) {
    n = PLACE_LIMIT;
  }
  const int32_t rLat = mapDegToI7(refLat), rLon = mapDegToI7(refLon);
  // The order: nearest first, Photon's rank for a tie (an insertion sort is stable; n <= 8).
  int order[PLACE_LIMIT];
  double dist[PLACE_LIMIT];
  for (int i = 0; i < n; i++) {
    dist[i] = meshPosDistanceM(rLat, rLon, mapDegToI7(r[i].lat), mapDegToI7(r[i].lon));
    int k = i;
    while (k > 0 && dist[order[k - 1]] > dist[i]) {
      order[k] = order[k - 1];
      k--;
    }
    order[k] = i;
  }
  const char* home = homeCountry ? homeCountry : r[order[0]].country;
  for (int k = 0; k < n; k++) {
    const int i = order[k];
    const PlaceResult* p = &r[i];
    PlaceRow* row = &rows[k];
    row->idx = i;
    row->distM = dist[i];
    char d[16], name[PLACE_NAME_MAX];
    unitsFmtDist(dist[i], units, d, sizeof(d));
    foldInto(p->name, name, sizeof(name));
    snprintf(row->text, sizeof(row->text), "%s %s  %s", d,
             meshPosCompass8(meshPosBearingDeg(rLat, rLon, mapDegToI7(p->lat), mapDegToI7(p->lon))), name);
    size_t sn = 0;
    row->sub[0] = '\0';
    subPart(row->sub, sizeof(row->sub), &sn, p->osmValue[0] ? p->osmValue : p->type);
    subPart(row->sub, sizeof(row->sub), &sn, p->city[0] ? p->city : p->state);
    if (p->country[0] && (!home || !home[0] || strcasecmp(p->country, home) != 0)) {
      subPart(row->sub, sizeof(row->sub), &sn, p->country);
    }
  }
  return n;
}

size_t placeHeader(const char* query, int n, char* out, size_t cap) {
  if (!out || cap == 0) {
    return 0;
  }
  char q[PLACE_QUERY_RAW];
  foldInto(query, q, sizeof(q));
  char head[32];
  if (n > 0) {
    snprintf(head, sizeof(head), "Found %d for '", n);
  } else {
    snprintf(head, sizeof(head), "Nothing found for '");
  }
  size_t k = 0;
  out[0] = '\0';
  catCut(out, cap, &k, head);
  if (k + 2 >= cap) {
    out[0] = '\0';                                // no room for even one character and the quote
    return 0;
  }
  /* The query, leaving room for the closing quote. */
  for (const char* p = q; *p && k + 2 < cap; p++) {
    out[k++] = *p;
  }
  out[k++] = '\'';
  out[k] = '\0';
  return k;
}

bool placeNearPx(int zoom, double aLat, double aLon, double bLat, double bLon, int px) {
  double ax = 0, ay = 0, bx = 0, by = 0;
  mapLatLonToWorld(aLat, aLon, zoom, &ax, &ay);
  mapLatLonToWorld(bLat, bLon, zoom, &bx, &by);
  const double dx = ax - bx, dy = ay - by;
  return dx * dx + dy * dy <= (double)px * (double)px;
}

// ── every failure's words ────────────────────────────────────────────────────────────────

int placeOutcome(int httpCode, int net, bool overflow, int parsed) {
  if (httpCode == 200) {
    return (overflow || parsed != PLACE_PARSE_OK) ? PLACE_ERR_BAD_ANSWER : PLACE_ERR_NONE;
  }
  if (httpCode > 0) {
    return PLACE_ERR_REFUSED;                     // 429, 5xx, a 4xx, a 3xx (never followed)
  }
  switch (net) {
  case GEM_NET_DNS:
  case GEM_NET_CONNECT:
  case GEM_NET_TLS:
  case GEM_NET_TLS_OTHER: return PLACE_ERR_UNREACHABLE;
  case GEM_NET_TIMEOUT:   return PLACE_ERR_TIMEOUT;
  case GEM_NET_LOST:      return PLACE_ERR_LOST;
  case GEM_NET_NOMEM:     return PLACE_ERR_NOMEM;
  case GEM_NET_TOO_BIG:
  case GEM_NET_BAD_BODY:  return PLACE_ERR_BAD_ANSWER;
  default:                return PLACE_ERR_UNREACHABLE;
  }
}

size_t placeErrLine(int err, int httpCode, const char* host, char* out, size_t cap) {
  if (!out || cap == 0) {
    return 0;
  }
  int n = 0;
  switch (err) {
  case PLACE_ERR_NONE:        out[0] = '\0'; return 0;
  case PLACE_ERR_TIMEOUT:     n = snprintf(out, cap, "Place search: no answer in %u s", (unsigned)(PLACE_TOTAL_MS / 1000u)); break;
  case PLACE_ERR_REFUSED:     n = snprintf(out, cap, "Place search: the service refused (HTTP %d)", httpCode); break;
  case PLACE_ERR_NOWIFI:      n = snprintf(out, cap, "Place search: not on WiFi"); break;
  case PLACE_ERR_UNREACHABLE: n = snprintf(out, cap, "Place search: could not reach %s", (host && host[0]) ? host : PLACE_HOST_DEFAULT); break;
  case PLACE_ERR_BAD_ANSWER:  n = snprintf(out, cap, "Place search: could not read the answer"); break;
  case PLACE_ERR_LOST:        n = snprintf(out, cap, "Place search: the connection dropped"); break;
  case PLACE_ERR_NOMEM:       n = snprintf(out, cap, "Place search: phone low on memory - reboot first"); break;
  case PLACE_ERR_CANCELLED:   n = snprintf(out, cap, "Place search: cancelled"); break;
  case PLACE_ERR_BUSY:        n = snprintf(out, cap, "Place search: another network job is running"); break;
  case PLACE_ERR_OFF:         n = snprintf(out, cap, "Place search: off - turn it on in the Maps menu"); break;
  case PLACE_ERR_EMPTY:       n = snprintf(out, cap, "Place search: nothing to search for"); break;
  default:                    n = snprintf(out, cap, "Place search: failed"); break;
  }
  if (n < 0) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}

const char* placeSettingRow(bool on) {
  return on ? "Place search: On" : "Place search: Off";
}
