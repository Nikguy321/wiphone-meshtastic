/*
 * placesearch.h - the pure half of the map's PLACE SEARCH (D-177, 2026-10-20): Maps > "Search a
 * place..." asks Photon (komoot's OpenStreetMap geocoder, https://photon.komoot.io - no key, no
 * sign-up, results from OpenStreetMap under the ODbL) for up to PLACE_LIMIT places matching what
 * was typed, nearest the reference first, and the map jumps to the one chosen. Every rule that can
 * be decided without a network, a card or a screen lives here so the host suite
 * (tests/test_placesearch.cpp) can prove it: the query's cut, the URL (RFC 3986 encoding, the
 * {q}/{lat}/{lon}/{n} template, https only), the GeoJSON parser on json_read.h, the drops, the
 * dedupe, the rows exactly as the Pins / Places / Nodes lists write theirs, the header, every
 * failure's words, and the pin-name prefill rule. The device half is placesearch_net.h (the GET on
 * the shared HTTPS worker, the gates, NVS, the console bench); the screens are app_maps.cpp's.
 *
 * ⚠ Deliberately free of every Arduino and ESP-IDF header (websearch.h's rule). No allocation, no
 * recursion (json_read.h skips by counting brackets), every buffer the caller's - on the phone a
 * PSRAM struct, never the loop task's stack. C++11, no float printf in the URL (the bias is
 * integer-formatted: a 3-decimal number is a decision, not a printf default).
 *
 * SCALABLE TO BE SHARED (the spec's rule for a public firmware with no key):
 *   - ONE request per deliberate Search press; never autocomplete, never a request per keystroke,
 *     never a retry (placesearch_net.cpp: one netHttps call, nothing around it);
 *   - a real User-Agent naming the firmware and its repo (placesearch_net.cpp, PLACE_USER_AGENT);
 *   - the endpoint is a SETTING (NVS maps/searchurl, a URL template; console only) so a group can
 *     run its own Photon (https://github.com/komoot/photon) and point every phone at it, and a change
 *     at komoot needs no reflash; https only, and the roots pinned are the two ISRG roots
 *     (isrg_roots.h) - a self-hosted instance needs a Let's Encrypt certificate;
 *   - at most PLACE_LIMIT (8) results asked for, lang=en, the reference rounded to 3 decimals
 *     (~100 m: enough to rank by, not a fix), the query collapsed and cut at a word at 100
 *     characters, nothing else about the user in the request (no node id, no name).
 *
 * THE REQUEST:  GET https://photon.komoot.io/api/?q=<text>&lat=<ref>&lon=<ref>&limit=8&lang=en
 * THE REPLY:    a GeoJSON FeatureCollection; features[].geometry.coordinates = [lon, lat] and
 *               properties.{name, osm_key, osm_value, type, city, state, country} are read, every
 *               other key (extent, postcode, street, osm_id...) skipped whole. A feature with no
 *               name, no usable coordinates (not two finite numbers in -90..90 / -180..180) is
 *               dropped; two with the same name (case-blind) within PLACE_DEDUPE_M collapse to the
 *               first (Photon's own rank); at most PLACE_LIMIT kept. A body that is not a
 *               FeatureCollection, is cut short or is not JSON at all is PLACE_PARSE_BAD ("could not
 *               read the answer"). Measured 2026-10-20 (tests/fixtures/place/photon_teneriffe.json):
 *               q=teneriffe falls near 47.49,-121.70 -> the waterfall and two trails, 1,137 bytes.
 *
 * PRIVACY: nothing is stored but the session's last query and last place (RAM, placesearch_net);
 * no history on the card; the URL is never logged in full (its host only); the query text appears
 * only on the screen and in the console's own echo.
 *
 * ═══════════════════════════════════════════════════════════════════════════════════════════
 * THE APP'S API (implementer B, app_maps.cpp) - the device functions are declared in
 * placesearch_net.h; this is how they fit together:
 *
 *   SETTING   bool placeSearchEnabled();           NVS maps/search (int, default 1). The Maps menu
 *             void placeSetEnabled(bool on);        row "Place search: On/Off" (placeSettingRow)
 *                                                   cycles it; "Search a place: off" opens it.
 *             size_t placeSearchHost(char*, size_t) the endpoint's host, for the help page / notes.
 *             (placeSearchUrl / placeSetSearchUrl: the template, console only - `maps searchurl`.)
 *
 *   START     bool placeSearchStart(const char* query, double refLat, double refLon,
 *                                   char* why, size_t whyCap);
 *             From MAPS_SEARCH's Search/OK: the field's text as typed (this collapses and cuts it;
 *             an empty one is refused with PLACE_ERR_EMPTY's text - the app shows nothing for an
 *             empty field) and the reference (selfPosition(), else the crosshair - the same rule
 *             as buildList's). False with `why` = the note to show (every gate's text, each
 *             starting "Place search: ..." - not on WiFi, off, another network job, low memory,
 *             still cancelling). True = queued on the worker: the footer says "Searching...".
 *
 *   POLL      int placeSearchState();   PLACE_IDLE / PLACE_RUNNING / PLACE_DONE / PLACE_ERROR
 *             uint32_t placeSearchGen(); changes with every start and every finish - redraw on it.
 *             const char* placeSearchError();  PLACE_ERROR's line ("Place search: no answer in 10 s"
 *                                               ...), "" otherwise - setNote("%s", it).
 *             DONE and ERROR persist until the next start or placeSearchClear(). The fold from the
 *             worker happens in placeLoopTick() (WiPhone.ino, every pass) - the app only polls.
 *
 *   RESULTS   int placeResultCount();                     0..PLACE_LIMIT once DONE (0 = "Nothing
 *             const PlaceResult* placeResult(int i);       found for '<query>'")
 *             const char* placeLastQuery();                the query as sent (collapsed/cut; "" none):
 *                                                          the field's prefill for a second search
 *                                                          and the header's '<query>'
 *             Rows: placeRows(results, n, refLat, refLon, gUnits, NULL, rows) into a PSRAM
 *             PlaceRow[PLACE_LIMIT] - nearest first, text = the list row ("850m NE  Teneriffe
 *             Falls"), sub = the second line ("waterfall, Washington"); rows[k].idx is the result
 *             it came from (what OK centres on: placeResult(idx)->lat/lon, ->name for the note).
 *             placeHeader(placeLastQuery(), n, buf, cap) is the list's header.
 *
 *   CHOOSE    void placeSetLastPlace(const char* name, double lat, double lon);
 *             On OK on a row, before centreOn + setNote. Then, on OK on the crosshair:
 *             bool placeLastPlace(char* name, size_t cap, double* lat, double* lon)  and
 *             placeNearPx(zoom, lastLat, lastLon, crosshairLat, crosshairLon, PLACE_NEAR_PX) ->
 *             prefill the pin-name field with the place's name. placeForgetLastPlace() when the app
 *             closes (the last place lives until then, the last query for the session).
 *
 *   CANCEL    void placeSearchCancel();   Back while RUNNING: the worker's request ends (its
 *             handshake cannot be interrupted, so the state stays RUNNING a moment) and the fold
 *             makes it IDLE with no error - return to the field. A Search pressed meanwhile is
 *             refused with "Place search: still cancelling the last search".
 *
 *   CLEAR     void placeSearchClear();    DONE/ERROR -> IDLE (results kept until the next start).
 *
 *   TEXTS     PLACE_ROW_SEARCH / PLACE_ROW_SEARCH_NOWIFI / PLACE_ROW_SEARCH_OFF (the menu row),
 *             PLACE_NOTE_NOWIFI (the note for the Wi-Fi gate), placeSettingRow(on), PLACE_HEADER
 *             ("Search a place"), PLACE_FOOTER_SEARCHING ("Searching..."), PLACE_ROW_AGAIN
 *             ("Search again"), PLACE_ATTRIBUTION (the help page's sentence).
 * ═══════════════════════════════════════════════════════════════════════════════════════════
 */
#ifndef PLACESEARCH_H
#define PLACESEARCH_H

#include <stddef.h>
#include <stdint.h>

// ── the provider, the limits ─────────────────────────────────────────────────────────────
#define PLACE_HOST_DEFAULT   "photon.komoot.io"
/* The built-in template: {q} the encoded query, {lat}/{lon} the reference to 3 decimals, {n} the
 * limit. A custom one (NVS maps/searchurl) must be https and carry {q}; the rest are optional. */
#define PLACE_URL_DEFAULT    "https://" PLACE_HOST_DEFAULT "/api/?q={q}&lat={lat}&lon={lon}&limit={n}&lang=en"
#define PLACE_LIMIT          8            // results asked for ({n}) and kept
#define PLACE_QUERY_CHARS    100          // the query as sent: UTF-8 characters, cut at a word
#define PLACE_QUERY_RAW      (PLACE_QUERY_CHARS * 4 + 1)  // bytes a cut query can need, NUL included
#define PLACE_TMPL_MAX       200          // a template, NUL included (serial_cmd's dlurl tmpl[200])
/* The URL: the template, {q} as up to 12 bytes a character (%XX x 4), the two biases, the limit. */
#define PLACE_URL_MAX        (PLACE_TMPL_MAX + PLACE_QUERY_CHARS * 12 + 32)
#define PLACE_HOST_MAX       64
#define PLACE_DEDUPE_M       50.0         // the same name this close is one place
#define PLACE_BIAS_DECIMALS  3            // ~100 m: enough to rank by, not a fix
/* The budgets (placesearch_net.cpp's request): TCP + the handshake, and the whole exchange, DNS to
 * the last byte - the Tavily search's (ai_net.cpp: AI_WEB_CONNECT_MS, AI_WEB_TOTAL_MS). */
#define PLACE_CONNECT_MS     6000u
#define PLACE_TOTAL_MS       10000u
#define PLACE_NEAR_PX        10           // the pin-name prefill: the crosshair this close to the last place

// ── one result ───────────────────────────────────────────────────────────────────────────
#define PLACE_NAME_MAX       64           // UTF-8 as read (cut to fit; folded for the rows)
#define PLACE_KIND_MAX       24           // osm_value / type
#define PLACE_LOCAL_MAX      40           // city / state
#define PLACE_COUNTRY_MAX    32
struct PlaceResult {
  char   name[PLACE_NAME_MAX];
  char   osmKey[PLACE_KIND_MAX];          // "natural", "highway", "place"...
  char   osmValue[PLACE_KIND_MAX];        // "waterfall", "path", "city"... ("" when absent)
  char   type[PLACE_KIND_MAX];            // house|street|district|city|county|state|country|locality|other
  char   city[PLACE_LOCAL_MAX];
  char   state[PLACE_LOCAL_MAX];
  char   country[PLACE_COUNTRY_MAX];
  double lat, lon;
};

// ── the query ────────────────────────────────────────────────────────────────────────────
/* The query as sent: every run of whitespace and control characters one space, both ends trimmed,
 * then cut at the last space at or before PLACE_QUERY_CHARS characters (one long word: at a
 * character, never inside a UTF-8 sequence). Returns the length; "" (0) for nothing to search. A
 * result that would not fit `cap` is "" too (cap PLACE_QUERY_RAW always fits). */
size_t placeQuery(const char* q, char* out, size_t cap);

// ── the template and the URL ─────────────────────────────────────────────────────────────
/* A template is accepted when: printable ASCII only, under PLACE_TMPL_MAX, "https://" then a host
 * ([A-Za-z0-9.-]; no :port - 443 only), {q} in it, and every '{' one of {q} {lat} {lon} {n}. 1 = ok;
 * 0 with `why` (a short sentence) otherwise. NULL / "" is refused too (the caller treats "" as "use
 * the default", not as a template). */
int    placeTemplateCheck(const char* tmpl, char* why, size_t whyCap);
/* The host of a template (NULL / "" = the default's): "photon.komoot.io" (no scheme, no port, no
 * path); "" (0) when there is none. For the status line and the error text - never the whole URL. */
size_t placeTemplateHost(const char* tmpl, char* out, size_t cap);
/* RFC 3986 percent-encoding: unreserved [A-Za-z0-9-._~] kept, every other byte %XX (upper-case
 * hex; a space is %20, UTF-8 bytes each their own %XX). Returns the length; "" (0) when it would
 * not fit. */
size_t placeEncode(const char* s, char* out, size_t cap);
/* A coordinate to PLACE_BIAS_DECIMALS decimals by integer arithmetic, half away from zero:
 * 47.4996 -> "47.500", -121.7 -> "-121.700", -0.0004 -> "0.000" (never "-0.000"). */
size_t placeFmtBias(double v, char* out, size_t cap);
/* The URL from `tmpl` (NULL / "" = PLACE_URL_DEFAULT; else it must pass placeTemplateCheck) with
 * {q} = placeEncode(query), {lat}/{lon} = placeFmtBias, {n} = limit clamped to 1..PLACE_LIMIT.
 * `query` is used AS GIVEN (placeQuery first). Returns the length; "" (0) for an empty query, a
 * refused template, or a URL that would not fit. ⚠ The result holds the query: never log it. */
size_t placeBuildUrl(const char* tmpl, const char* query, double lat, double lon, int limit, char* out,
                     size_t cap);

// ── the reply ────────────────────────────────────────────────────────────────────────────
enum { PLACE_PARSE_OK = 0, PLACE_PARSE_BAD };
/* Parse a whole body (PLACE_PARSE_OK with *n = 0 is a good "nothing found"). Into out[0..cap-1],
 * cap at most PLACE_LIMIT kept; the rest of the features are still read (a body is good or bad as a
 * whole). BAD: not JSON, not an object, no "features" array, a "type" that is not
 * "FeatureCollection", a feature that is not an object, or a body cut short anywhere. */
int    placeParse(const char* body, size_t len, PlaceResult* out, int cap, int* n);

// ── the rows ─────────────────────────────────────────────────────────────────────────────
#define PLACE_ROW_MAX        72           // app_maps.cpp's MapsListRow.text[72]
struct PlaceRow {
  int    idx;                             // the PlaceResult it came from
  double distM;                           // from the reference
  char   text[PLACE_ROW_MAX];             // "850m NE  Teneriffe Falls" (the lists' "%s %s  %s")
  char   sub[PLACE_ROW_MAX];              // "waterfall, Washington" / "path, Kootenay, Canada"
};
/* One row per result, NEAREST FIRST by the reference (stable: Photon's rank for equal distances),
 * distance in `units` (units.h) + compass (mesh_pos.h) + the name folded to ASCII (geminiFoldNext,
 * the Wikipedia text's fold) and cut to the row. The second line: <osm_value, else type> ('_' a
 * space), <city, else state>, and the country only when it is not `homeCountry`'s (NULL = the
 * NEAREST result's country: the reference is taken to be there) - each folded, the whole cut to
 * the row. `rows` has n slots. Returns n. */
int    placeRows(const PlaceResult* r, int n, double refLat, double refLon, int units, const char* homeCountry,
                 PlaceRow* rows);
/* "Found 3 for 'teneriffe falls'" / "Nothing found for 'xyz'" - the query folded to ASCII; cut to
 * fit (the quote closed). */
size_t placeHeader(const char* query, int n, char* out, size_t cap);
/* The pin-name prefill rule: the two points within `px` screen pixels of each other at this zoom
 * (world pixels, map_tiles.h - Mercator, so a northern 10 px is a northern 10 px). */
bool   placeNearPx(int zoom, double aLat, double aLon, double bLat, double bLon, int px);

// ── every failure's words ────────────────────────────────────────────────────────────────
enum {
  PLACE_ERR_NONE = 0,       // results came (maybe none)
  PLACE_ERR_TIMEOUT,        // "Place search: no answer in 10 s"
  PLACE_ERR_REFUSED,        // "Place search: the service refused (HTTP 429)" - any non-200 (3xx never followed)
  PLACE_ERR_NOWIFI,         // "Place search: not on WiFi"
  PLACE_ERR_UNREACHABLE,    // "Place search: could not reach photon.komoot.io" (DNS, connect, TLS)
  PLACE_ERR_BAD_ANSWER,     // "Place search: could not read the answer" (not a FeatureCollection, past the cap, garbled)
  PLACE_ERR_LOST,           // "Place search: the connection dropped"
  PLACE_ERR_NOMEM,          // "Place search: phone low on memory - reboot first"
  PLACE_ERR_CANCELLED,      // "Place search: cancelled" (the console's word; the app goes back to the field)
  PLACE_ERR_BUSY,           // "Place search: another network job is running"
  PLACE_ERR_OFF,            // "Place search: off - turn it on in the Maps menu"
  PLACE_ERR_EMPTY,          // "Place search: nothing to search for"
  PLACE_ERR_KINDS
};
/* After a request: the HTTP code (<= 0: `net`, GEM_NET_* of gemini.h, says why), whether the body
 * passed the sink's cap, and the parse (PLACE_PARSE_*; ignored unless code == 200). */
int    placeOutcome(int httpCode, int net, bool overflow, int parsed);
/* The line for the note (and the console): `httpCode` for REFUSED, `host` for UNREACHABLE (NULL =
 * the default host). Returns the length; "" for PLACE_ERR_NONE. */
size_t placeErrLine(int err, int httpCode, const char* host, char* out, size_t cap);

// ── the screen's words (app_maps.cpp draws them; the suite pins them) ────────────────────
#define PLACE_ROW_SEARCH         "Search a place..."
#define PLACE_ROW_SEARCH_NOWIFI  "Search a place (needs Wi-Fi)"
#define PLACE_ROW_SEARCH_OFF     "Search a place: off"
#define PLACE_NOTE_NOWIFI        "Not on WiFi - join a network first"
#define PLACE_HEADER             "Search a place"
#define PLACE_FOOTER_SEARCHING   "Searching..."
#define PLACE_ROW_AGAIN          "Search again"
#define PLACE_ATTRIBUTION        "Place search results come from OpenStreetMap through Photon (komoot), " \
                                 "under the ODbL. What is sent: the text you typed and your position " \
                                 "rounded to about 100 m - nothing else."
const char* placeSettingRow(bool on);   // "Place search: On" / "Place search: Off"

#endif // PLACESEARCH_H
