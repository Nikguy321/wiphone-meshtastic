/* test_placesearch.cpp - the pure half of the map's place search (WiPhone/placesearch.cpp, D-177):
 * the query's collapse and its 100-character word cut (UTF-8 characters, never inside one), RFC 3986
 * encoding (a space, &, #, /, "Café"), the 3-decimal bias by integer arithmetic (half away from zero,
 * never "-0.000"), the limit's clamp, the {q}/{lat}/{lon}/{n} template (https only, every refusal),
 * the exact default URL, the GeoJSON parser on the REAL reply captured 2026-10-20
 * (tests/fixtures/place/photon_teneriffe.json: the waterfall and two trails, `county` but no `city`,
 * `extent` ignored) and on synthetic shapes (an empty collection, not JSON, not a FeatureCollection,
 * the fixture cut at EVERY byte, a 120 KB body, 12 results cut to 8, a missing / null name, bad or
 * non-numeric or missing coordinates, a 3-element Point, keys in any order, a 4,000-deep hostile
 * array), the dedupe (same name within 50 m, case-blind), the rows exactly as the lists write them
 * (nearest first, stable, the fold, the second line, the country rule, the row cut, US units), the
 * header, the pin-name prefill rule in pixels, every failure's words, the pinned roots by SHA-256 and
 * the budgets the glue pins - and the screen's words measured in the firmware's own glyph tables
 * (test_websearch's measurer): the fixture's rows in the lists' face (Akrobat Bold 20, 232 px), the
 * strip's hints (Akrobat Bold 16, 232 px) and every failure line wrapped into the strip's two rows.
 *
 * WHY THIS IS WORTH A SUITE: a shared firmware with no key asks a public, unpaid service. Every
 * failure here is quiet on the phone: a query sent raw with a '&' in it asks for the wrong thing, a
 * 5-decimal bias sends a fix, a limit of 50 is rude, an http:// template hands the query to the air,
 * a parser that trusts `features[].properties.name` without checking the coordinates jumps the map
 * into the sea, a cut inside a UTF-8 character garbles the name on the screen.
 *
 * NO NETWORK: everything here is pure code on bytes in memory and the one fixture file.
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../WiPhone/placesearch.h"
#include "../WiPhone/gemini.h"         // GEM_NET_*
#include "../WiPhone/mesh_pos.h"
#include "../WiPhone/units.h"
#include "../WiPhone/map_tiles.h"
#include "../WiPhone/book_hash.h"      // SHA-256, for the roots' fingerprints
#include "../WiPhone/isrg_roots.h"
#include "../WiPhone/menu_wrap.h"      // wrapNote: the strip's row wrapping, the code the phone runs
#define PROGMEM
#include "../WiPhone/src/assets/fonts.h"   // the firmware's glyph tables: Akrobat_Bold20 (the lists), Bold16 (the strip)

static int failures = 0;
static int checks = 0;

static void group(const char* name) {
  printf("\n\033[1m%s\033[0m\n", name);
}

static void ok(bool cond, const char* what) {
  checks++;
  if (!cond) {
    failures++;
    printf("  \033[31mFAIL\033[0m %s\n", what);
  } else {
    printf("  ok  %s\n", what);
  }
}

static std::string readFile(const char* path) {
  std::string s;
  FILE* f = fopen(path, "rb");
  if (!f) {
    printf("  \033[31mmissing fixture %s\033[0m\n", path);
    failures++;
    return s;
  }
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}

static int utf8Chars(const char* s) {
  int n = 0;
  for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
    if ((*p & 0xC0) != 0x80) n++;
  }
  return n;
}

// ── the query ────────────────────────────────────────────────────────────────────────────

static std::string query(const char* q) {
  char out[PLACE_QUERY_RAW];
  memset(out, 'X', sizeof(out));
  const size_t n = placeQuery(q, out, sizeof(out));
  if (n == 0) return "";
  return std::string(out, n);
}

static void testQuery() {
  group("the query: collapsed, trimmed, cut at a word at 100 characters");
  ok(query("  teneriffe   falls ") == "teneriffe falls", "runs of spaces one, both ends trimmed");
  ok(query("a\tb\r\nc\x01" "d") == "a b c d", "tabs, line ends and control characters are spaces");
  ok(query("") == "" && query("   \t") == "" && query(NULL) == "", "nothing to search for is \"\"");
  ok(query("Café Ñandú") == "Café Ñandú", "UTF-8 goes through untouched");
  {
    std::string s(100, 'a');
    ok(query(s.c_str()) == s, "exactly 100 characters are kept");
    s += 'a';
    ok(query(s.c_str()) == std::string(100, 'a'), "one long word of 101 is cut at the 100th character");
  }
  {
    std::string s;
    for (int i = 0; i < 101; i++) s += "é";       // 101 characters, 202 bytes
    const std::string r = query(s.c_str());
    ok(utf8Chars(r.c_str()) == 100 && r.size() == 200, "characters are counted, not bytes: 101 x e-acute -> 100 (200 bytes)");
  }
  {
    std::string s;
    for (int i = 0; i < 30; i++) s += "abcd ";    // 150 characters; index 99 is a space
    const std::string r = query(s.c_str());
    ok(r.size() == 99 && r.substr(r.size() - 4) == "abcd" && r[r.size() - 5] == ' ',
       "cut at the last space at or before 100 (no trailing space)");
  }
  {
    std::string s;
    for (int i = 0; i < 20; i++) s += (i ? " abcde" : "abcde");   // 119 characters, index 100 mid-word
    const std::string r = query(s.c_str());
    ok(r.size() == 95 && r.substr(r.size() - 5) == "abcde" && utf8Chars(r.c_str()) <= 100,
       "a cut that would fall mid-word backs up to the last space (95 chars)");
  }
  {
    std::string s;
    for (int i = 0; i < 25; i++) s += "abc ";     // 100 characters with a trailing space
    ok(query(s.c_str()).size() == 99, "a 100th character that is a space is trimmed, nothing cut");
  }
  {
    char small[8];
    ok(placeQuery("teneriffe falls", small, sizeof(small)) == 0 && small[0] == '\0', "a result that would not fit is \"\" (0)");
  }
}

// ── encoding, the bias, the URL ──────────────────────────────────────────────────────────

static std::string enc(const char* s) {
  char out[1024];
  const size_t n = placeEncode(s, out, sizeof(out));
  return n ? std::string(out, n) : std::string("");
}

static std::string bias(double v) {
  char out[16];
  const size_t n = placeFmtBias(v, out, sizeof(out));
  return n ? std::string(out, n) : std::string("");
}

static std::string url(const char* tmpl, const char* q, double lat, double lon, int limit) {
  char out[PLACE_URL_MAX];
  memset(out, 'X', sizeof(out));
  const size_t n = placeBuildUrl(tmpl, q, lat, lon, limit, out, sizeof(out));
  return n ? std::string(out, n) : std::string(out[0] == '\0' ? "" : "<not cleared>");
}

static void testUrl() {
  group("RFC 3986: unreserved kept, everything else %XX");
  ok(enc("teneriffe falls") == "teneriffe%20falls", "a space is %20 (never +)");
  ok(enc("a&b#c/d?e=f+g") == "a%26b%23c%2Fd%3Fe%3Df%2Bg", "& # / ? = + are encoded");
  ok(enc("Café") == "Caf%C3%A9", "UTF-8: each byte its own %XX, upper-case hex");
  ok(enc("AZaz09-._~") == "AZaz09-._~", "the unreserved set is kept");
  ok(enc("") == "", "an empty text encodes to \"\"");
  {
    char small[6];
    ok(placeEncode("abcdef", small, sizeof(small)) == 0 && small[0] == '\0', "no fit: \"\" (0)");
    ok(placeEncode("a b", small, sizeof(small)) == 5 && !strcmp(small, "a%20b"), "a%20b fits 6 exactly");
    ok(placeEncode("a bc", small, 5) == 0 && small[0] == '\0', "no fit after a %XX either: \"\" (0)");
  }

  group("the bias: 3 decimals by integer arithmetic");
  ok(bias(47.49) == "47.490", "47.49 -> 47.490");
  ok(bias(-121.70) == "-121.700", "-121.70 -> -121.700");
  ok(bias(47.4996) == "47.500", "47.4996 rounds up");
  ok(bias(47.4994) == "47.499", "47.4994 rounds down");
  ok(bias(-0.0004) == "0.000", "-0.0004 is 0.000, never -0.000");
  ok(bias(-0.0005) == "-0.001", "half away from zero: -0.0005 -> -0.001");
  ok(bias(0.0) == "0.000" && bias(180.0) == "180.000" && bias(-90.0) == "-90.000", "0, 180, -90");
  ok(bias(12.3456) == "12.346", "12.3456 -> 12.346");
  ok(bias(NAN) == "" && bias(1e9) == "", "NaN or an absurd value: \"\" (no bias)");
  ok(PLACE_BIAS_DECIMALS == 3, "PLACE_BIAS_DECIMALS is 3 (~100 m)");

  group("the URL: the default template, exactly");
  ok(url(NULL, "teneriffe falls", 47.49, -121.70, 8) ==
     "https://photon.komoot.io/api/?q=teneriffe%20falls&lat=47.490&lon=-121.700&limit=8&lang=en",
     "https://photon.komoot.io/api/?q=teneriffe%20falls&lat=47.490&lon=-121.700&limit=8&lang=en");
  ok(url("", "x", 0, 0, 8) == url(NULL, "x", 0, 0, 8), "\"\" is the default template too");
  ok(url(NULL, "x", 1, 2, 0).find("&limit=1&") != std::string::npos, "limit 0 is clamped to 1");
  ok(url(NULL, "x", 1, 2, 20).find("&limit=8&") != std::string::npos, "limit 20 is clamped to PLACE_LIMIT (8)");
  ok(url(NULL, "x", 1, 2, 3).find("&limit=3&") != std::string::npos, "limit 3 is 3");
  ok(url(NULL, "", 1, 2, 8) == "", "an empty query builds nothing");
  ok(url(NULL, "Café", 1, 2, 8).find("q=Caf%C3%A9&") != std::string::npos, "the query is encoded");
  ok(strncmp(PLACE_URL_DEFAULT, "https://photon.komoot.io/", 25) == 0, "the default is https to photon.komoot.io");
  ok(strstr(PLACE_URL_DEFAULT, "&lang=en") != NULL, "the default asks lang=en");
  ok(strstr(PLACE_URL_DEFAULT, "limit={n}") != NULL, "the default carries the limit as {n}");
  ok(strstr(PLACE_URL_DEFAULT, "lat={lat}&lon={lon}") != NULL, "the default carries the bias");
  ok(PLACE_LIMIT == 8, "PLACE_LIMIT is 8");
  ok(PLACE_QUERY_CHARS == 100, "PLACE_QUERY_CHARS is 100");

  group("the URL: a custom template");
  ok(url("https://geo.example.org/api/?q={q}&limit={n}", "café", 1, 2, 8) ==
     "https://geo.example.org/api/?q=caf%C3%A9&limit=8", "{q} and {n} substituted, no bias when not asked");
  ok(url("https://geo.example.org:8443/api/?q={q}&limit={n}", "café", 1, 2, 8) == "", "a :port template builds nothing (443 only)");
  ok(url("https://h/x?q={q}&lat={lat}&lon={lon}", "a", 47.4996, -121.7, 8) == "https://h/x?q=a&lat=47.500&lon=-121.700",
     "{lat}/{lon} to 3 decimals");
  ok(url("https://h/?q={q}&q2={q}", "a b", 0, 0, 8) == "https://h/?q=a%20b&q2=a%20b", "a placeholder may repeat");
  ok(url("http://photon.komoot.io/api/?q={q}", "a", 0, 0, 8) == "", "http:// is refused");
  ok(url("https://h/api/?text={x}", "a", 0, 0, 8) == "", "a template without {q} is refused");
  ok(url("https://h/api/?q={q}&z={z}", "a", 0, 0, 8) == "", "an unknown placeholder is refused");
  {
    char small[40];
    const size_t n = placeBuildUrl(NULL, "teneriffe falls", 47.49, -121.70, 8, small, sizeof(small));
    ok(n == 0 && small[0] == '\0', "a URL that would not fit is \"\" (0), never half a URL");
  }
  {
    // The longest query the cut allows (100 x 4-byte characters) fits PLACE_URL_MAX with the default.
    std::string q;
    for (int i = 0; i < 100; i++) q += "\xF0\x9F\x98\x80";
    char out[PLACE_URL_MAX];
    const size_t n = placeBuildUrl(NULL, q.c_str(), -89.999, -179.999, 8, out, sizeof(out));
    ok(n > 1200 && n < sizeof(out), "the longest cut query fits PLACE_URL_MAX");
  }

  group("the template check: https only, a host, {q}, known braces");
  char why[96];
  ok(placeTemplateCheck(PLACE_URL_DEFAULT, why, sizeof(why)) == 1, "the default passes");
  ok(placeTemplateCheck("https://geo.example.org:8443/api/?q={q}", why, sizeof(why)) == 0 && strstr(why, "443") != NULL,
     "a :port refused - 443 only (the worker has no port field; accepted, it failed every search)");
  ok(placeTemplateCheck("https://h/api/?q={q}&lat={lat}&lon={lon}&limit={n}", why, sizeof(why)) == 1, "all four placeholders pass");
  ok(placeTemplateCheck("http://h/api/?q={q}", why, sizeof(why)) == 0 && strstr(why, "https") != NULL, "http:// refused, and why says https");
  ok(placeTemplateCheck("https:///api/?q={q}", why, sizeof(why)) == 0 && strstr(why, "host") != NULL, "no host refused");
  ok(placeTemplateCheck("https://h/api/?q=x", why, sizeof(why)) == 0 && strstr(why, "{q}") != NULL, "no {q} refused");
  ok(placeTemplateCheck("https://h/api/?q={q}&k={key}", why, sizeof(why)) == 0 && strstr(why, "{q} {lat} {lon} {n}") != NULL, "an unknown placeholder refused");
  ok(placeTemplateCheck("https://h/api/?q={q}}", why, sizeof(why)) == 0, "a stray } refused");
  ok(placeTemplateCheck("https://h/api/?q={q} ", why, sizeof(why)) == 0 && strstr(why, "space") != NULL, "a space refused");
  ok(placeTemplateCheck("https://h/api/?q={q}\xC3\xA9", why, sizeof(why)) == 0, "a non-ASCII byte refused");
  ok(placeTemplateCheck("https://h_x/api/?q={q}", why, sizeof(why)) == 0, "an underscore in the host refused");
  ok(placeTemplateCheck("https://h:abc/?q={q}", why, sizeof(why)) == 0, "a :port that is not a number refused too");
  ok(placeTemplateCheck("", why, sizeof(why)) == 0 && placeTemplateCheck(NULL, why, sizeof(why)) == 0, "\"\" / NULL refused (the caller means the default by them)");
  {
    std::string t = "https://h/?q={q}&pad=";
    t += std::string(PLACE_TMPL_MAX - t.size(), 'a');       // exactly PLACE_TMPL_MAX bytes: one too many
    ok(placeTemplateCheck(t.c_str(), why, sizeof(why)) == 0 && strstr(why, "long") != NULL, "a template of PLACE_TMPL_MAX bytes refused");
    t.resize(PLACE_TMPL_MAX - 1);
    ok(placeTemplateCheck(t.c_str(), why, sizeof(why)) == 1, "one of PLACE_TMPL_MAX - 1 passes");
  }
  ok(placeTemplateCheck("http://h/?q={q}", NULL, 0) == 0, "a NULL why is fine");

  group("the host of a template");
  char host[PLACE_HOST_MAX];
  ok(placeTemplateHost(NULL, host, sizeof(host)) == 16 && !strcmp(host, "photon.komoot.io"), "NULL -> the default's host");
  ok(placeTemplateHost("", host, sizeof(host)) == 16 && !strcmp(host, "photon.komoot.io"), "\"\" -> the default's host");
  ok(placeTemplateHost("https://geo.example.org:8443/api/?q={q}", host, sizeof(host)) == 15 && !strcmp(host, "geo.example.org"),
     "no scheme, no port, no path");
  ok(placeTemplateHost("https:///x", host, sizeof(host)) == 0 && host[0] == '\0', "no host: \"\" (0)");
  ok(placeTemplateHost("https://geo.example.org/", host, 8) == 0 && host[0] == '\0', "no fit: \"\" (0)");
}

// ── the parser ───────────────────────────────────────────────────────────────────────────

static PlaceResult R[PLACE_LIMIT + 2];

static int parse(const std::string& body, int* n, int cap = PLACE_LIMIT) {
  memset(R, 0x5A, sizeof(R));                      // anything past *n is never read: poison it
  return placeParse(body.data(), body.size(), R, cap, n);
}

/* A Photon-shaped feature. `coords` is the JSON of "coordinates" (NULL = no geometry at all). */
static std::string feat(const char* name, const char* coords, const char* extraProps = "", const char* osmValue = "path",
                        const char* type = "street") {
  std::string s = "{\"type\":\"Feature\",\"properties\":{";
  if (name) {
    s += "\"name\":";
    s += name;                                     // JSON as given: "\"x\"" or "null"
    s += ",";
  }
  s += "\"osm_type\":\"W\",\"osm_id\":1,\"osm_key\":\"highway\",\"osm_value\":\"";
  s += osmValue;
  s += "\",\"type\":\"";
  s += type;
  s += "\",\"country\":\"United States\",\"countrycode\":\"US\"";
  s += extraProps;
  s += "}";
  if (coords) {
    s += ",\"geometry\":{\"type\":\"Point\",\"coordinates\":";
    s += coords;
    s += "}";
  }
  s += "}";
  return s;
}

static std::string coll(const std::vector<std::string>& feats, const char* type = "\"FeatureCollection\"") {
  std::string s = "{";
  if (type) {
    s += "\"type\":";
    s += type;
    s += ",";
  }
  s += "\"features\":[";
  for (size_t i = 0; i < feats.size(); i++) {
    if (i) s += ",";
    s += feats[i];
  }
  s += "]}";
  return s;
}

static void testParseReal() {
  group("the parser: the real reply (tests/fixtures/place/photon_teneriffe.json, 2026-10-20)");
  const std::string body = readFile("tests/fixtures/place/photon_teneriffe.json");
  int n = -1;
  ok(parse(body, &n) == PLACE_PARSE_OK && n == 3, "a FeatureCollection of three: OK, 3 kept");
  if (n == 3) {
    ok(!strcmp(R[0].name, "Teneriffe Falls") && !strcmp(R[1].name, "Teneriffe Falls Trail") &&
       !strcmp(R[2].name, "Teneriffe Falls Connector Trail"), "the names, in Photon's order");
    ok(!strcmp(R[0].osmKey, "waterway") && !strcmp(R[0].osmValue, "waterfall") && !strcmp(R[0].type, "other"),
       "the waterfall: osm_key waterway, osm_value waterfall, type other");
    ok(!strcmp(R[1].osmKey, "highway") && !strcmp(R[1].osmValue, "path") && !strcmp(R[1].type, "street"),
       "the trail: highway / path / street");
    ok(fabs(R[0].lat - 47.5021084) < 1e-9 && fabs(R[0].lon - (-121.7080644)) < 1e-9, "coordinates are [lon, lat] -> lat, lon");
    ok(R[0].city[0] == '\0' && !strcmp(R[0].state, "Washington") && !strcmp(R[0].country, "United States"),
       "no city (only a county, which is not read), state Washington, country United States");
    ok(fabs(R[2].lat - 47.5006071) < 1e-9 && fabs(R[2].lon - (-121.7112775)) < 1e-9, "the third's coordinates (its extent ignored)");
  }
  ok(body.find("\"extent\"") != std::string::npos, "the fixture has an extent (skipped whole)");
  ok(body.find("\"postcode\"") != std::string::npos && R[0].name[0] != '\0', "the fixture has keys this never reads (postcode)");

  group("the parser: the fixture cut at every byte is BAD");
  bool allBad = true;
  int firstOk = -1;
  for (size_t cut = 1; cut < body.size(); cut++) {
    int m = 0;
    if (parse(body.substr(0, cut), &m) != PLACE_PARSE_BAD) {
      allBad = false;
      if (firstOk < 0) firstOk = (int)cut;
    }
  }
  ok(allBad, "every prefix of the real body is BAD (a cut body never shows half a list)");
  if (!allBad) printf("     first prefix that parsed: %d bytes\n", firstOk);
  ok(parse(body + " x", &n) == PLACE_PARSE_BAD, "text after the object is BAD");
  ok(parse(body + " {}", &n) == PLACE_PARSE_BAD, "a second root is BAD");
  ok(parse(body + "  \n", &n) == PLACE_PARSE_OK && n == 3, "trailing whitespace is fine");
}

static void testParseShapes() {
  group("the parser: shapes");
  int n = -1;
  ok(parse(coll({}), &n) == PLACE_PARSE_OK && n == 0, "an empty collection: OK, 0 (nothing found)");
  ok(parse("", &n) == PLACE_PARSE_BAD && n == 0, "an empty body is BAD");
  ok(parse("<html><body>login</body></html>", &n) == PLACE_PARSE_BAD, "an HTML page (a Wi-Fi login) is BAD");
  ok(parse("[1,2,3]", &n) == PLACE_PARSE_BAD, "an array root is BAD");
  ok(parse("{\"features\":[]}", &n) == PLACE_PARSE_BAD, "no type at all is BAD (not a FeatureCollection)");
  ok(parse(coll({}, "\"Feature\""), &n) == PLACE_PARSE_BAD, "type Feature is BAD");
  ok(parse("{\"type\":\"FeatureCollection\"}", &n) == PLACE_PARSE_BAD, "no features array is BAD");
  ok(parse("{\"type\":\"FeatureCollection\",\"features\":{}}", &n) == PLACE_PARSE_BAD, "features that is not an array is BAD");
  ok(parse("{\"type\":\"FeatureCollection\",\"features\":[1]}", &n) == PLACE_PARSE_BAD, "a feature that is not an object is BAD");
  ok(parse("{\"type\":\"FeatureCollection\",\"features\":[],\"features\":[]}", &n) == PLACE_PARSE_BAD, "features twice is BAD");
  ok(parse("{\"features\":[],\"type\":\"FeatureCollection\"}", &n) == PLACE_PARSE_OK, "the type may come after the features");
  ok(parse("{\"type\":\"FeatureCollection\",\"features\":[],\"licence\":\"ODbL\",\"n\":[1,{\"a\":null}]}", &n) == PLACE_PARSE_OK,
     "unknown top-level keys are skipped");

  const char* P = "[-121.7,47.5]";
  ok(parse(coll({ feat("\"A\"", P) }), &n) == PLACE_PARSE_OK && n == 1 && !strcmp(R[0].name, "A") &&
     fabs(R[0].lat - 47.5) < 1e-12 && fabs(R[0].lon + 121.7) < 1e-12, "one good feature");
  ok(parse(coll({ feat(NULL, P) }), &n) == PLACE_PARSE_OK && n == 0, "no name: dropped");
  ok(parse(coll({ feat("null", P) }), &n) == PLACE_PARSE_OK && n == 0, "a null name: dropped");
  ok(parse(coll({ feat("\"\"", P) }), &n) == PLACE_PARSE_OK && n == 0, "an empty name: dropped");
  ok(parse(coll({ feat("\"A\"", NULL) }), &n) == PLACE_PARSE_OK && n == 0, "no geometry: dropped");
  ok(parse(coll({ feat("\"A\"", "[-121.7,95.0]") }), &n) == PLACE_PARSE_OK && n == 0, "lat 95: dropped");
  ok(parse(coll({ feat("\"A\"", "[-181.0,47.5]") }), &n) == PLACE_PARSE_OK && n == 0, "lon -181: dropped");
  ok(parse(coll({ feat("\"A\"", "[\"-121.7\",\"47.5\"]") }), &n) == PLACE_PARSE_OK && n == 0, "coordinates as strings: dropped");
  ok(parse(coll({ feat("\"A\"", "[null,47.5]") }), &n) == PLACE_PARSE_OK && n == 0, "a null coordinate: dropped");
  ok(parse(coll({ feat("\"A\"", "[-121.7]") }), &n) == PLACE_PARSE_OK && n == 0, "one coordinate: dropped");
  ok(parse(coll({ feat("\"A\"", "{\"lat\":47.5}") }), &n) == PLACE_PARSE_OK && n == 0, "coordinates that are an object: dropped");
  ok(parse(coll({ feat("\"A\"", "[-121.7,47.5,812.0]") }), &n) == PLACE_PARSE_OK && n == 1, "a 3-element Point (altitude) is fine");
  ok(parse(coll({ feat("\"A\"", "[180.0,-90.0]") }), &n) == PLACE_PARSE_OK && n == 1, "the edges -90 / 180 are in range");
  ok(parse(coll({ feat("\"A\"", P, ",\"extent\":[-121.71,47.50,-121.70,47.49]") }), &n) == PLACE_PARSE_OK && n == 1, "extent is skipped");
  ok(parse(coll({ feat("\"A\"", P, ",\"city\":\"North Bend\",\"state\":\"Washington\",\"postcode\":\"98045\"") }), &n) == PLACE_PARSE_OK &&
     !strcmp(R[0].city, "North Bend") && !strcmp(R[0].state, "Washington"), "city and state are read");
  ok(parse(coll({ feat("\"A\"", P, ",\"city\":null") }), &n) == PLACE_PARSE_OK && R[0].city[0] == '\0', "a null city is \"\"");
  ok(parse(coll({ feat("\"A\"", P, ",\"city\":{\"x\":[1,2]}") }), &n) == PLACE_PARSE_OK && R[0].city[0] == '\0', "a city that is an object is skipped");
  {
    // geometry before properties, and unknown keys between
    std::string f = "{\"geometry\":{\"coordinates\":[-121.7,47.5],\"type\":\"Point\"},\"bbox\":[1,2,3,4],"
                    "\"properties\":{\"name\":\"B\",\"osm_value\":\"peak\"},\"type\":\"Feature\"}";
    ok(parse(coll({ f }), &n) == PLACE_PARSE_OK && n == 1 && !strcmp(R[0].name, "B") && !strcmp(R[0].osmValue, "peak"),
       "keys in any order");
  }
  {
    std::string f = "{\"properties\":{\"name\":\"Na\\u00efve \\\"Caf\\u00e9\\\"\"},\"geometry\":{\"coordinates\":[1,2]}}";
    ok(parse(coll({ f }), &n) == PLACE_PARSE_OK && n == 1 && !strcmp(R[0].name, "Na\xC3\xAFve \"Caf\xC3\xA9\""),
       "\\u escapes become UTF-8, quotes unescaped");
  }
  {
    std::string longName(200, 'n');
    std::string f = feat(("\"" + longName + "\"").c_str(), P);
    ok(parse(coll({ f }), &n) == PLACE_PARSE_OK && n == 1 && strlen(R[0].name) == PLACE_NAME_MAX - 1, "a long name is cut to its buffer");
  }
  {
    std::string f = "{\"properties\":{\"name\":\"deep\",\"junk\":" + std::string(4000, '[') + std::string(4000, ']') +
                    "},\"geometry\":{\"coordinates\":[1,2]}}";
    ok(parse(coll({ f }), &n) == PLACE_PARSE_OK && n == 1 && !strcmp(R[0].name, "deep"), "a 4,000-deep hostile array is skipped without recursion");
  }
  {
    std::string f = feat("\"A\"", P);
    f.resize(f.size() - 3);                        // inside the geometry
    ok(parse(coll({ f }) , &n) == PLACE_PARSE_BAD, "a feature cut short is BAD");
  }

  group("the parser: the cap, the dedupe");
  {
    std::vector<std::string> many;
    for (int i = 0; i < 12; i++) {
      char nm[16], c[64];
      snprintf(nm, sizeof(nm), "\"P%d\"", i);
      snprintf(c, sizeof(c), "[-121.%d,47.%d]", 10 + i, 10 + i);
      many.push_back(feat(nm, c));
    }
    ok(parse(coll(many), &n) == PLACE_PARSE_OK && n == 8 && !strcmp(R[0].name, "P0") && !strcmp(R[7].name, "P7"),
       "12 results: the first 8 kept, Photon's order");
    ok(parse(coll(many), &n, 3) == PLACE_PARSE_OK && n == 3 && !strcmp(R[2].name, "P2"), "a caller's cap of 3 keeps 3 (the rest still read)");
    ok(parse(coll(many), &n, 50) == PLACE_PARSE_OK && n == 8, "a cap over PLACE_LIMIT is PLACE_LIMIT");
  }
  {
    // 120 KB: 60 features each with a 2 KB string nobody reads.
    std::vector<std::string> many;
    const std::string pad = ",\"description\":\"" + std::string(2000, 'd') + "\"";
    for (int i = 0; i < 60; i++) {
      char nm[16], c[64];
      snprintf(nm, sizeof(nm), "\"Q%d\"", i);
      snprintf(c, sizeof(c), "[-121.%d,47.%d]", 100 + i, 100 + i);
      many.push_back(feat(nm, c, pad.c_str()));
    }
    const std::string big = coll(many);
    ok(big.size() > 100 * 1024 && parse(big, &n) == PLACE_PARSE_OK && n == 8 && !strcmp(R[7].name, "Q7"),
       "a 120 KB body parses in one pass: 8 kept (the worker's own cap is netHttps's overflow)");
    ok(parse(big.substr(0, 70000), &n) == PLACE_PARSE_BAD, "...and cut at 70,000 bytes it is BAD");
  }
  {
    // the same name 30 m apart (lat 0.00027 deg) collapses; 80 m apart does not; another name 10 m away stays
    ok(parse(coll({ feat("\"Falls\"", "[-121.7,47.5]"), feat("\"Falls\"", "[-121.7,47.50027]") }), &n) == PLACE_PARSE_OK && n == 1,
       "the same name 30 m apart: one");
    ok(parse(coll({ feat("\"Falls\"", "[-121.7,47.5]"), feat("\"falls\"", "[-121.7,47.50027]") }), &n) == PLACE_PARSE_OK && n == 1,
       "the dedupe is case-blind");
    ok(parse(coll({ feat("\"Falls\"", "[-121.7,47.5]"), feat("\"Falls\"", "[-121.7,47.50072]") }), &n) == PLACE_PARSE_OK && n == 2,
       "the same name 80 m apart: two");
    ok(parse(coll({ feat("\"Falls\"", "[-121.7,47.5]"), feat("\"Falls Trail\"", "[-121.7,47.50009]") }), &n) == PLACE_PARSE_OK && n == 2,
       "another name 10 m away: two");
    ok(parse(coll({ feat("\"Falls\"", "[-121.7,47.5]"), feat("\"X\"", "[-121.7,47.6]"), feat("\"Falls\"", "[-121.7,47.50027]") }), &n) ==
       PLACE_PARSE_OK && n == 2 && !strcmp(R[1].name, "X"), "a duplicate of an earlier one is dropped wherever it is");
    ok(PLACE_DEDUPE_M == 50.0, "PLACE_DEDUPE_M is 50");
  }
}

// ── the rows ─────────────────────────────────────────────────────────────────────────────

static PlaceResult mk(const char* name, double lat, double lon, const char* osmValue, const char* type, const char* city,
                      const char* state, const char* country) {
  PlaceResult r;
  memset(&r, 0, sizeof(r));
  snprintf(r.name, sizeof(r.name), "%s", name);
  snprintf(r.osmValue, sizeof(r.osmValue), "%s", osmValue);
  snprintf(r.type, sizeof(r.type), "%s", type);
  snprintf(r.city, sizeof(r.city), "%s", city);
  snprintf(r.state, sizeof(r.state), "%s", state);
  snprintf(r.country, sizeof(r.country), "%s", country);
  r.lat = lat;
  r.lon = lon;
  return r;
}

static void testRows() {
  group("the rows: nearest first, the lists' text, the second line");
  PlaceResult r[4];
  r[0] = mk("Bus_Stop", 47.01, -122.0, "bus_stop", "other", "", "Washington", "Canada");
  r[1] = mk("South Place", 46.99, -122.0, "", "locality", "", "", "United States");
  r[2] = mk("Caf\xC3\xA9 \xC3\x91" "and\xC3\xBA", 47.0, -122.0, "", "", "Olympia", "Washington", "United States");
  PlaceRow rows[4];
  int n = placeRows(r, 3, 47.0, -122.0, UNITS_METRIC, NULL, rows);
  ok(n == 3, "three rows");
  ok(rows[0].idx == 2 && rows[1].idx == 0 && rows[2].idx == 1, "nearest first; equal distances keep Photon's order (stable)");
  ok(rows[0].distM == 0.0 && rows[1].distM > 1100 && rows[1].distM < 1125 && fabs(rows[1].distM - rows[2].distM) < 1e-6,
     "distances from the reference (0.01 deg of latitude is ~1,112 m)");
  ok(!strcmp(rows[0].text, "0m N  Cafe Nandu"), "the row: \"%s %s  %s\" - distance, compass, the name folded to ASCII");
  ok(!strcmp(rows[1].text, "1.1km N  Bus_Stop"), "1.1km N  Bus_Stop (a name's underscore is the name's)");
  ok(!strcmp(rows[2].text, "1.1km S  South Place"), "1.1km S  South Place");
  ok(!strcmp(rows[0].sub, "Olympia"), "second line: no kind, the city, the home country left out");
  ok(!strcmp(rows[1].sub, "bus stop, Washington, Canada"), "second line: osm_value with ' ' for '_', the state (no city), a foreign country");
  ok(!strcmp(rows[2].sub, "locality"), "second line: type when there is no osm_value; no city nor state; home country out");
  {
    // the same text the Pins / Places / Nodes lists write: unitsFmtDist + meshPosCompass8(meshPosBearingDeg)
    const int32_t rl = mapDegToI7(47.0), ro = mapDegToI7(-122.0);
    char d[16], want[PLACE_ROW_MAX];
    unitsFmtDist(meshPosDistanceM(rl, ro, mapDegToI7(47.01), mapDegToI7(-122.0)), UNITS_METRIC, d, sizeof(d));
    snprintf(want, sizeof(want), "%s %s  %s", d, meshPosCompass8(meshPosBearingDeg(rl, ro, mapDegToI7(47.01), mapDegToI7(-122.0))), "Bus_Stop");
    ok(!strcmp(rows[1].text, want), "...byte for byte what buildList() writes");
  }
  n = placeRows(r, 3, 47.0, -122.0, UNITS_METRIC, "Canada", rows);
  ok(n == 3 && !strcmp(rows[0].sub, "Olympia, United States") && !strcmp(rows[1].sub, "bus stop, Washington"),
     "a given home country: the others show theirs");
  n = placeRows(r, 3, 47.0, -122.0, UNITS_US, NULL, rows);
  ok(n == 3 && !strcmp(rows[0].text, "0ft N  Cafe Nandu") && !strncmp(rows[1].text, "0.7mi N  ", 9), "US units: 0ft, 0.7mi");
  {
    // A name is at most 63 bytes (its field); "1.4km N  " + 63 = 72 bytes is one too many for the row.
    PlaceResult one = mk(std::string(100, 'x').c_str(), 47.0126, -122.0, std::string(30, 'k').c_str(), "", std::string(50, 'c').c_str(),
                         "", std::string(40, 'z').c_str());
    PlaceRow row;
    ok(strlen(one.name) == PLACE_NAME_MAX - 1, "a name is cut to its field (63 bytes)");
    ok(placeRows(&one, 1, 47.0, -122.0, UNITS_METRIC, "elsewhere", &row) == 1 && !strncmp(row.text, "1.4km N  xxx", 12) &&
       strlen(row.text) == PLACE_ROW_MAX - 1 && strlen(row.sub) == PLACE_ROW_MAX - 1,
       "a long name and a long second line are cut to the row (71 bytes)");
  }
  {
    PlaceResult two[2];
    two[0] = mk("Far", 48.0, -122.0, "peak", "", "", "", "United States");
    two[1] = mk("Near", 47.001, -122.0, "peak", "", "", "", "Canada");
    PlaceRow rr[2];
    placeRows(two, 2, 47.0, -122.0, UNITS_METRIC, NULL, rr);
    ok(rr[0].idx == 1 && !strcmp(rr[0].sub, "peak") && !strcmp(rr[1].sub, "peak, United States"),
       "home = the NEAREST result's country (Canada): the far one shows United States");
  }
  {
    PlaceResult nc = mk("No Country", 47.0, -122.0, "peak", "", "", "", "");
    PlaceRow rr;
    placeRows(&nc, 1, 47.0, -122.0, UNITS_METRIC, NULL, &rr);
    ok(!strcmp(rr.sub, "peak"), "no country anywhere: no country on the line");
  }
  ok(placeRows(r, 0, 47.0, -122.0, UNITS_METRIC, NULL, rows) == 0 && placeRows(NULL, 3, 0, 0, 0, NULL, rows) == 0, "nothing: 0 rows");
  ok(PLACE_ROW_MAX == 72, "PLACE_ROW_MAX is the lists' MapsListRow.text[72]");

  group("the header");
  char h[64];
  ok(placeHeader("teneriffe falls", 3, h, sizeof(h)) == 29 && !strcmp(h, "Found 3 for 'teneriffe falls'"), "Found 3 for 'teneriffe falls'");
  ok(placeHeader("xyz", 0, h, sizeof(h)) > 0 && !strcmp(h, "Nothing found for 'xyz'"), "Nothing found for 'xyz'");
  ok(placeHeader("Caf\xC3\xA9", 1, h, sizeof(h)) > 0 && !strcmp(h, "Found 1 for 'Cafe'"), "the query folded to ASCII");
  ok(placeHeader("teneriffe falls", 3, h, 24) == 23 && !strcmp(h, "Found 3 for 'teneriffe'"), "cut to fit, the quote closed");
  ok(placeHeader("x", 1, h, 10) == 0 && h[0] == '\0', "no room for the query: \"\"");

  group("the pin-name prefill: within 10 px of the last place");
  ok(placeNearPx(15, 47.5, -121.7, 47.5, -121.7, PLACE_NEAR_PX), "the same point");
  ok(placeNearPx(15, 47.5, -121.7, 47.50018, -121.7, PLACE_NEAR_PX), "20 m north at z15 (~3.2 m/px): near");
  ok(!placeNearPx(15, 47.5, -121.7, 47.5009, -121.7, PLACE_NEAR_PX), "100 m north at z15: not near");
  ok(placeNearPx(10, 47.5, -121.7, 47.5009, -121.7, PLACE_NEAR_PX), "100 m north at z10 (~103 m/px): near");
  ok(!placeNearPx(10, 47.5, -121.7, 47.52, -121.7, PLACE_NEAR_PX), "2.2 km at z10: not near");
  ok(PLACE_NEAR_PX == 10, "PLACE_NEAR_PX is 10");
}

// ── every failure's words ────────────────────────────────────────────────────────────────

static std::string errLine(int err, int code, const char* host) {
  char out[96];
  placeErrLine(err, code, host, out, sizeof(out));
  return out;
}

static void testWords() {
  group("the outcome and its words");
  ok(placeOutcome(200, GEM_NET_OK, false, PLACE_PARSE_OK) == PLACE_ERR_NONE, "200 + a good parse: none");
  ok(placeOutcome(200, GEM_NET_OK, false, PLACE_PARSE_BAD) == PLACE_ERR_BAD_ANSWER, "200 + BAD: could not read the answer");
  ok(placeOutcome(200, GEM_NET_OK, true, PLACE_PARSE_OK) == PLACE_ERR_BAD_ANSWER, "200 past the 64 KB cap: could not read the answer");
  ok(placeOutcome(429, GEM_NET_OK, false, -1) == PLACE_ERR_REFUSED, "429: refused");
  ok(placeOutcome(503, GEM_NET_OK, false, -1) == PLACE_ERR_REFUSED, "503: refused");
  ok(placeOutcome(301, GEM_NET_OK, false, -1) == PLACE_ERR_REFUSED, "301: refused (a redirect is never followed)");
  ok(placeOutcome(400, GEM_NET_OK, false, -1) == PLACE_ERR_REFUSED, "400: refused");
  ok(placeOutcome(0, GEM_NET_DNS, false, -1) == PLACE_ERR_UNREACHABLE, "DNS: could not reach");
  ok(placeOutcome(0, GEM_NET_CONNECT, false, -1) == PLACE_ERR_UNREACHABLE, "connect: could not reach");
  ok(placeOutcome(0, GEM_NET_TLS, false, -1) == PLACE_ERR_UNREACHABLE && placeOutcome(0, GEM_NET_TLS_OTHER, false, -1) == PLACE_ERR_UNREACHABLE,
     "TLS: could not reach");
  ok(placeOutcome(0, GEM_NET_TIMEOUT, false, -1) == PLACE_ERR_TIMEOUT, "timeout: no answer in 10 s");
  ok(placeOutcome(0, GEM_NET_LOST, false, -1) == PLACE_ERR_LOST, "lost: the connection dropped");
  ok(placeOutcome(0, GEM_NET_NOMEM, false, -1) == PLACE_ERR_NOMEM, "nomem: low on memory");
  ok(placeOutcome(0, GEM_NET_TOO_BIG, false, -1) == PLACE_ERR_BAD_ANSWER && placeOutcome(0, GEM_NET_BAD_BODY, false, -1) == PLACE_ERR_BAD_ANSWER,
     "too big / a bad chunked body: could not read the answer");
  ok(errLine(PLACE_ERR_TIMEOUT, 0, NULL) == "Place search: no answer in 10 s", "Place search: no answer in 10 s");
  ok(errLine(PLACE_ERR_REFUSED, 429, NULL) == "Place search: the service refused (HTTP 429)", "Place search: the service refused (HTTP 429)");
  ok(errLine(PLACE_ERR_REFUSED, 503, NULL) == "Place search: the service refused (HTTP 503)", "...(HTTP 503)");
  ok(errLine(PLACE_ERR_NOWIFI, 0, NULL) == "Place search: not on WiFi", "Place search: not on WiFi");
  ok(errLine(PLACE_ERR_UNREACHABLE, 0, NULL) == "Place search: could not reach photon.komoot.io", "Place search: could not reach photon.komoot.io");
  ok(errLine(PLACE_ERR_UNREACHABLE, 0, "geo.example.org") == "Place search: could not reach geo.example.org", "...a custom endpoint's host");
  ok(errLine(PLACE_ERR_BAD_ANSWER, 0, NULL) == "Place search: could not read the answer", "Place search: could not read the answer");
  ok(errLine(PLACE_ERR_LOST, 0, NULL) == "Place search: the connection dropped", "Place search: the connection dropped");
  ok(errLine(PLACE_ERR_NOMEM, 0, NULL) == "Place search: phone low on memory - reboot first", "...low on memory - reboot first");
  ok(errLine(PLACE_ERR_CANCELLED, 0, NULL) == "Place search: cancelled", "Place search: cancelled");
  ok(errLine(PLACE_ERR_BUSY, 0, NULL) == "Place search: another network job is running", "Place search: another network job is running");
  ok(errLine(PLACE_ERR_OFF, 0, NULL) == "Place search: off - turn it on in the Maps menu", "Place search: off - turn it on in the Maps menu");
  ok(errLine(PLACE_ERR_EMPTY, 0, NULL) == "Place search: nothing to search for", "Place search: nothing to search for");
  ok(errLine(PLACE_ERR_NONE, 0, NULL) == "", "none: \"\"");
  for (int e = 1; e < PLACE_ERR_KINDS; e++) {
    if (errLine(e, 500, NULL).compare(0, 14, "Place search: ") != 0) {
      ok(false, "every failure's line starts \"Place search: \"");
      break;
    }
    if (e == PLACE_ERR_KINDS - 1) ok(true, "every failure's line starts \"Place search: \"");
  }
  {
    char small[12];
    ok(placeErrLine(PLACE_ERR_TIMEOUT, 0, NULL, small, sizeof(small)) == 11 && !strcmp(small, "Place searc"), "a small buffer: cut, NUL-terminated");
  }

  group("the screen's words and the budgets");
  ok(!strcmp(placeSettingRow(true), "Place search: On") && !strcmp(placeSettingRow(false), "Place search: Off"), "the setting row");
  ok(!strcmp(PLACE_ROW_SEARCH, "Search a place...") && !strcmp(PLACE_ROW_SEARCH_NOWIFI, "Search a place (needs Wi-Fi)") &&
     !strcmp(PLACE_ROW_SEARCH_OFF, "Search a place: off"), "the menu row's three texts");
  ok(!strcmp(PLACE_NOTE_NOWIFI, "Not on WiFi - join a network first"), "the Wi-Fi gate's note (the download form's)");
  ok(!strcmp(PLACE_HEADER, "Search a place") && !strcmp(PLACE_FOOTER_SEARCHING, "Searching...") && !strcmp(PLACE_ROW_AGAIN, "Search again"),
     "the field's header, the footer, the empty list's row");
  ok(strstr(PLACE_ATTRIBUTION, "OpenStreetMap") != NULL && strstr(PLACE_ATTRIBUTION, "Photon") != NULL && strstr(PLACE_ATTRIBUTION, "ODbL") != NULL,
     "the attribution names OpenStreetMap, Photon and the ODbL");
  ok(PLACE_CONNECT_MS == 6000u, "PLACE_CONNECT_MS is 6 s (the Tavily search's handshake cap)");
  ok(PLACE_TOTAL_MS == 10000u, "PLACE_TOTAL_MS is 10 s (DNS to the last byte)");
  ok(sizeof(PlaceResult) * PLACE_LIMIT < 4096, "eight results are under 4 KB (one PSRAM struct)");
}

// ── the phone's font, as SmoothFont reads it (test_gemini.cpp's measurer) ───────────────────────

struct Font7 {
  int      n;
  uint16_t uni[256];
  uint8_t  w[256], adv[256];
  int8_t   dx[256];
  int      space;
  bool     ok;
};

static uint32_t runDecode(const unsigned char** p) {
  uint32_t v = 0;
  unsigned char more = 0;
  do {
    v <<= 7;
    v |= (**p) & 0x7F;
    more = (**p) & 0x80;
    (*p)++;
  } while (more);
  return v;
}

static void fontLoad(Font7* f, const unsigned char* data) {
  memset(f, 0, sizeof(*f));
  if (data[0] != '7' || data[1] != 'S' || data[2] != 'F') {
    return;
  }
  const unsigned char* p = data + 3;
  f->n = (int)runDecode(&p);
  runDecode(&p);
  const int ascent = (int)runDecode(&p);
  const int descent = (int)runDecode(&p);
  if (f->n <= 0 || f->n > 256) {
    return;
  }
  for (int g = 0; g < f->n; g++) {
    f->uni[g] = (uint16_t)(p[0] | (p[1] << 8));
    p += 4;
    runDecode(&p);
    f->w[g] = (uint8_t)runDecode(&p);
    f->adv[g] = (uint8_t)runDecode(&p);
    runDecode(&p);
    f->dx[g] = (int8_t)runDecode(&p);
  }
  f->space = (ascent + descent) * 2 / 7;
  f->ok = true;
}

static bool fontGlyph(const Font7* f, uint16_t u, int* g) {
  for (int i = 0; i < f->n; i++) {
    if (f->uni[i] == u) {
      *g = i;
      return true;
    }
  }
  return false;
}

static int textWidth(const Font7* f, const char* s) {
  int w = 0;
  for (const char* p = s; *p; p++) {
    const uint16_t u = (unsigned char)*p;
    if (u == 0x20) {
      w += f->space;
      continue;
    }
    int g = 0;
    if (fontGlyph(f, u, &g)) {
      if (w == 0 && f->dx[g] < 0) {
        w -= f->dx[g];
      }
      w += p[1] ? f->adv[g] : (f->dx[g] + f->w[g]);
    } else {
      w += f->space + 1;
    }
  }
  return w;
}

static int fitTextLength(const Font7* f, const char* s, int width) {
  int fit = 0, sw = 0;
  for (const char* p = s; *p; p++) {
    int cw, g = 0;
    if (*p == ' ') {
      cw = f->space;
    } else if (fontGlyph(f, (unsigned char)*p, &g)) {
      cw = f->adv[g];
      if (sw == 0 && f->dx[g] < 0) cw -= f->dx[g];
    } else {
      cw = f->space + 1;
    }
    if (sw + cw > width) break;
    sw += cw;
    fit = (int)(p - s) + 1;
  }
  return fit;
}

static bool allGlyphs(const Font7* f, const char* s) {
  for (const char* p = s; *p; p++) {
    int g = 0;
    if (*p != ' ' && !fontGlyph(f, (unsigned char)*p, &g)) {
      printf("    no glyph for '%c' in \"%s\"\n", *p, s);
      return false;
    }
  }
  return true;
}

/* MenuWidget::addNoteWrapped's rows: menu_wrap.h's wrapNote, the code the phone runs. */
struct NoteCtx {
  const Font7* f;
  int w;
  int rows;
};
static size_t noteFit(const char* s, void* ctx) {
  const NoteCtx* c = (const NoteCtx*)ctx;
  return (size_t)fitTextLength(c->f, s, c->w);
}
static void noteRow(const char*, size_t, void* ctx) {
  ((NoteCtx*)ctx)->rows++;
}
static int noteRows(const Font7* f, const char* s, int w) {
  NoteCtx c = { f, w, 0 };
  wrapNote(s, noteFit, noteRow, &c, 99);
  return c.rows;
}

/* The row's title: MenuOption::redraw draws it at windowWidth - leftOffset = 240 - 8 (buildSearchList:
 * leftOffset 8); the subtitle in AKROBAT_BOLD_16 at the same width. The strip under the field:
 * drawSearch ellipsizes to lcd.width() - 8 and wraps a note into MAPS_NOTE_ROWS = 2 rows of it. */
static const int ROW_W = 240 - 8;
static const int STRIP_W = 240 - 8;
static const int NOTE_ROWS = 2;

static void testWidths() {
  Font7 f20, f16;
  fontLoad(&f20, Akrobat_Bold20);
  fontLoad(&f16, Akrobat_Bold16);
  group("the list's rows in the lists' face (Akrobat Bold 20 - newMenu's - at 232 px; the second line Bold 16)");
  ok(f20.ok && f16.ok && f20.n > 90 && f16.n > 90, "the firmware's Akrobat_Bold20 and Akrobat_Bold16 glyph tables load");
  const std::string body = readFile("tests/fixtures/place/photon_teneriffe.json");
  PlaceResult r[PLACE_LIMIT];
  PlaceRow rows[PLACE_LIMIT];
  int n = 0, k = 0;
  if (placeParse(body.data(), body.size(), r, PLACE_LIMIT, &n) == PLACE_PARSE_OK) {
    k = placeRows(r, n, 47.49, -121.70, UNITS_METRIC, NULL, rows);
  }
  ok(k == 3, "the fixture's three rows from the reference the reply was measured at (47.49,-121.70)");
  bool subs = k == 3;
  for (int i = 0; i < k; i++) {
    printf("      %3d px: \"%s\"  /  %3d px: \"%s\"\n", textWidth(&f20, rows[i].text), rows[i].text, textWidth(&f16, rows[i].sub), rows[i].sub);
    if (textWidth(&f16, rows[i].sub) > ROW_W || !allGlyphs(&f16, rows[i].sub) || !allGlyphs(&f20, rows[i].text)) subs = false;
  }
  ok(k == 3 && !strcmp(rows[0].text, "1.4km NW  Teneriffe Falls Trail") && textWidth(&f20, rows[0].text) <= ROW_W,
     "\"1.4km NW  Teneriffe Falls Trail\" fits the row (225 px of 232 when written; 258 px in the ExtraBold 22 it first had)");
  ok(k == 3 && !strcmp(rows[2].text, "1.5km NW  Teneriffe Falls") && textWidth(&f20, rows[2].text) <= ROW_W,
     "\"1.5km NW  Teneriffe Falls\" fits (188 px)");
  ok(k == 3 && strstr(rows[1].text, "Connector Trail") != NULL && textWidth(&f20, rows[1].text) > ROW_W,
     "the Connector Trail row (308 px) is cut in any face the lists use - the selected row's marquee shows the rest");
  ok(subs, "every second line fits in Akrobat Bold 16, and every character has a glyph");

  group("the strip under the field (Akrobat Bold 16, 232 px, 2 rows)");
  const char* from[] = { "you", "your pin", "the crosshair", "Wi-Fi +/-65535 m" };   // searchReference's four forms
  bool hints = true;
  char l[80];
  for (size_t i = 0; i < sizeof(from) / sizeof(from[0]); i++) {
    snprintf(l, sizeof(l), "Nearest %s first", from[i]);
    printf("      %3d px: \"%s\"\n", textWidth(&f16, l), l);
    if (textWidth(&f16, l) > STRIP_W || !allGlyphs(&f16, l)) hints = false;
  }
  ok(hints, "\"Nearest <reference> first\" fits for all four references (\"OK searches, nearest the crosshair first\" was 251 px)");
  const char* lines[] = { "Back deletes   End leaves", "Asking photon.komoot.io...", "Back cancels", "The console's search: End leaves" };
  bool fixed = true;
  for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
    if (textWidth(&f16, lines[i]) > STRIP_W || !allGlyphs(&f16, lines[i])) {
      printf("      %3d px: \"%s\"\n", textWidth(&f16, lines[i]), lines[i]);
      fixed = false;
    }
  }
  ok(fixed, "the strip's four fixed lines fit on one row");
  /* Every failure line the strip can show wraps into its two rows: placeErrLine's words with the
   * default host, the gates placesearch_net prefixes, and the console-pending refusal. */
  bool wraps = true;
  char e[96];
  for (int err = PLACE_ERR_NONE + 1; err < PLACE_ERR_KINDS; err++) {
    placeErrLine(err, 503, NULL, e, sizeof(e));
    if (noteRows(&f16, e, STRIP_W) > NOTE_ROWS || !allGlyphs(&f16, e)) {
      printf("      %d rows: \"%s\"\n", noteRows(&f16, e, STRIP_W), e);
      wraps = false;
    }
  }
  const char* gates[] = { "Place search: the console's answer is not printed yet - try again",
                          "Place search: No internal RAM for the network task - reboot first",   // netWorkerReady's longest, prefixed
                          "Place search: a call is on, or ended under a minute ago",
                          "Place search: the WiFi uploader is on - stop it first" };
  for (size_t i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
    if (noteRows(&f16, gates[i], STRIP_W) > NOTE_ROWS || !allGlyphs(&f16, gates[i]) || strlen(gates[i]) >= 72) {
      printf("      %d rows, %d bytes: \"%s\"\n", noteRows(&f16, gates[i], STRIP_W), (int)strlen(gates[i]), gates[i]);
      wraps = false;
    }
  }
  ok(wraps, "every failure and gate line wraps into the strip's 2 rows, and the gate lines fit the map's note[72]");
  {
    // The longest line the strip can get: "could not reach" + a 63-byte custom host (PLACE_HOST_MAX - 1).
    const std::string host(PLACE_HOST_MAX - 1, 'h');
    const size_t len = placeErrLine(PLACE_ERR_UNREACHABLE, 0, host.c_str(), e, sizeof(e));
    ok(len > 80 && len < 96 && strstr(e, host.c_str()) != NULL, "\"could not reach <63-byte host>\" fits searchWhy[96] / why[96] whole (94 bytes)");
  }
}

// ── the roots ────────────────────────────────────────────────────────────────────────────

static int b64v(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static std::vector<uint8_t> derOf(const char* pem, int nth) {
  std::vector<uint8_t> der;
  const char* p = pem;
  for (int i = 0; i <= nth; i++) {
    p = strstr(p, "-----BEGIN CERTIFICATE-----\n");
    if (!p) return der;
    p += strlen("-----BEGIN CERTIFICATE-----\n");
  }
  const char* end = strstr(p, "-----END CERTIFICATE-----");
  uint32_t acc = 0;
  int bits = 0;
  for (; p < end; p++) {
    const int v = b64v(*p);
    if (v < 0) continue;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      der.push_back((uint8_t)((acc >> bits) & 0xFF));
    }
  }
  return der;
}

static std::string sha256Hex(const std::vector<uint8_t>& d) {
  uint8_t h[32];
  bsSha256(d.data(), d.size(), h);
  std::string s;
  char b[4];
  for (int i = 0; i < 32; i++) {
    snprintf(b, sizeof(b), i ? ":%02X" : "%02X", h[i]);
    s += b;
  }
  return s;
}

static void testRoots() {
  group("the pinned roots: ISRG Root X1 + the self-signed Root YR (isrg_roots.h; photon.komoot.io <- YR1 <- Root YR)");
  static const char BOTH[] = ISRG_ROOT_X1_PEM ISRG_ROOT_YR_PEM;
  const std::vector<uint8_t> x1 = derOf(BOTH, 0), yr = derOf(BOTH, 1);
  ok(x1.size() > 1000 && yr.size() > 1000 && derOf(BOTH, 2).empty(), "exactly two certificates, concatenated");
  ok(sha256Hex(x1) == "96:BC:EC:06:26:49:76:F3:74:60:77:9A:CF:28:C5:A7:CF:E8:A3:C0:AA:E1:1A:8F:FC:EE:05:C0:BD:DF:08:C6",
     "X1: SHA-256 96:BC:EC...DF:08:C6");
  ok(sha256Hex(yr) == "E5:7B:7E:6F:15:0C:41:91:02:E8:D5:C0:55:72:9F:F9:67:B9:D1:A8:29:BF:00:CE:C8:9C:A6:04:EB:F4:A8:6F",
     "Root YR: SHA-256 E5:7B:7E...A8:6F");
}

int main() {
  testQuery();
  testUrl();
  testParseReal();
  testParseShapes();
  testRows();
  testWords();
  testWidths();
  testRoots();
  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
