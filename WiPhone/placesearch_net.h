/*
 * placesearch_net.h - the device half of the map's PLACE SEARCH (D-177, 2026-10-20): the setting
 * and the endpoint template in NVS, the gates, ONE GET to Photon as a NET_JOB_PLACE job on the
 * shared HTTPS worker (https_worker.h), the parse on the worker, the fold on the loop, the
 * session's last query and last place, and the console bench (`maps search`). Every decision -
 * the query's cut, the URL, the parser, the rows, the failure words - is placesearch.h's and
 * host-tested; this file only asks them. placesearch.h's API block says how app_maps.cpp uses it.
 *
 * ONE SEARCH = ONE GET on the worker, on a fresh WiFiClientSecure, the handshake timed alone
 * (connect first), pinned to ISRG Root X1 + the self-signed Root YR (isrg_roots.h: photon.komoot.io
 * chains leaf <- Let's Encrypt YR1 <- Root YR, cross-signed by X1 - measured 2026-10-20), the
 * internal-heap bar checked again right before the handshake, PLACE_CONNECT_MS for TCP + the
 * handshake and PLACE_TOTAL_MS for the whole exchange (the Tavily search's budgets), Cancel and a
 * call ringing in both honoured. NEVER RETRIED: a failure is a note, and the next request is the
 * next Search press. The body (the worker's 64 KB sink; past it = "could not read the answer") is
 * parsed ON THE WORKER into a PSRAM result; the LOOP folds it (placeLoopTick, every pass - a flag
 * test when idle), so the app polls a state and never waits on the worker.
 *
 * NOT WHILE ANOTHER JOB RUNS: a search is refused (not queued) while an AI question, a weather
 * fetch or a Wi-Fi location look is on the worker, while a map download holds its own TLS session,
 * the uploader or a sync window runs the radio as a server, a call is on, a game owns the card, or
 * a Files job runs - the weather's gates, with their words. An explicit Search pauses the music,
 * as a weather Refresh does (a handshake beside a decoding track failed on phone 1).
 *
 * 🛑 NOTHING HERE LOGS THE URL, THE QUERY OR A COORDINATE: one outcome line a search ("place
 * search: ok 3 0.6 s", "place search: failed dns"). The console's own echo is the one place the
 * query is written back, and `maps search` prints the endpoint's HOST only.
 */
#ifndef PLACESEARCH_NET_H
#define PLACESEARCH_NET_H

#include <stddef.h>
#include <stdint.h>
#include "placesearch.h"

enum { PLACE_IDLE = 0, PLACE_RUNNING, PLACE_DONE, PLACE_ERROR };

// ── LOOP TASK: the setting and the endpoint ──────────────────────────────────────────────
bool   placeSearchEnabled();                       // NVS maps/search (int; absent = on)
void   placeSetEnabled(bool on);                   // persists
/* The template in use ("" = PLACE_URL_DEFAULT): NVS maps/searchurl. ⚠ For the console only, and
 * never printed whole - placeSearchHost() for anything shown. */
size_t placeSearchUrl(char* out, size_t cap);
/* `maps searchurl <template>` / `clear` (NULL / ""): placeTemplateCheck first (https only, 443,
 * {q}...); false with `why` - also while a search runs (the worker reads the host). The next
 * search uses it. */
bool   placeSetSearchUrl(const char* tmpl, char* why, size_t whyCap);
size_t placeSearchHost(char* out, size_t cap);     // the host of the template in use

// ── LOOP TASK: one search ────────────────────────────────────────────────────────────────
/* Start one from `query` (as typed: collapsed and cut here) and the reference. False with `why`
 * (the note to show; each gate's words start "Place search: ..."). True = queued: poll. */
bool   placeSearchStart(const char* query, double refLat, double refLon, char* why, size_t whyCap);
int    placeSearchState();                         // PLACE_IDLE / RUNNING / DONE / ERROR
uint32_t placeSearchGen();                         // changes with every start and finish
const char* placeSearchError();                    // PLACE_ERROR's line; "" otherwise
void   placeSearchCancel();                        // the worker's request ends; the fold makes it IDLE
void   placeSearchClear();                         // DONE / ERROR -> IDLE (results kept until the next start)
/* Queued, running, or finished and not folded yet: netRequestActive()'s term (https_worker.cpp). */
bool   placeRequestActive();
/* Every pass (WiPhone.ino): fold a finished search into the result slot. A flag test when idle. */
void   placeLoopTick();

// ── LOOP TASK: what came back ────────────────────────────────────────────────────────────
int    placeResultCount();                         // 0..PLACE_LIMIT (0 while running, after an error or a cancel)
const PlaceResult* placeResult(int i);             // NULL out of range; valid until the next start
const char* placeLastQuery();                      // the session's last query as sent ("" none)
bool   placeLastRef(double* lat, double* lon);     // the reference the last search was made from
/* The place chosen on the list (app_maps.cpp, OK on a row): kept until placeForgetLastPlace()
 * (the app closing) - the pin-name prefill (placeNearPx). */
void   placeSetLastPlace(const char* name, double lat, double lon);
bool   placeLastPlace(char* name, size_t cap, double* lat, double* lon);
void   placeForgetLastPlace();

// ── the console (serial_cmd.cpp) ─────────────────────────────────────────────────────────
/* `maps search <text>`: one search from the phone's own reference (a live fix, else the pin, else
 * the newest fix, else the saved map view); the rows are printed by placeConsoleDrain when the
 * answer lands. True with `why` = the echo line ("searching 'x' from you (47.490,-121.700)"),
 * false with the refusal. */
bool   placeConsoleSearch(const char* text, char* why, size_t whyCap);
/* Every serialCmdLoop pass: a console-asked search that finished is printed - the header, the
 * rows exactly as the list shows them, the timing (dns / connect+handshake / all ms, bytes). */
void   placeConsoleDrain(void (*emit)(const char* line));
/* `maps search` alone: the setting, the endpoint (host only), the last query, the last count and
 * timing, the state. */
void   placeReport(void (*emit)(const char* line));

#endif // PLACESEARCH_NET_H
