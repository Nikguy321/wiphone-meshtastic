#!/usr/bin/env python3
"""check_place_glue.py - the map's place search keeps its rules in the device files (D-177, 2026-10-20).

test_placesearch proves the pure half (placesearch.cpp: the query's cut, the URL, the template, the
parser, the rows, the words). The rules below live in GLUE the host suite cannot compile -
placesearch_net.cpp, https_worker.cpp/.h, serial_cmd.cpp, WiPhone.ino - or are constants a later
edit could change with every test still green unless something pins them:

  1. THE ROOTS PINNED. PLACE_ROOTS_PEM is exactly ISRG_ROOT_X1_PEM + ISRG_ROOT_YR_PEM (isrg_roots.h;
     photon.komoot.io chains leaf <- Let's Encrypt YR1 <- Root YR, cross-signed by X1 - measured
     2026-10-20), and placeGet sends it (q.rootsPem = PLACE_ROOTS_PEM).
  2. THE USER-AGENT names the firmware, its version, the feature and the repo:
     "WiPhone-NH/" FIRMWARE_VERSION " maps-search (+https://github.com/Nikguy321/wiphone-meshtastic)",
     and placeGet sends it.
  3. THE BUDGETS. PLACE_CONNECT_MS is 6000u and PLACE_TOTAL_MS 10000u (placesearch.h: the Tavily
     search's); placeGet sets connectMs = PLACE_CONNECT_MS, totalMs = PLACE_TOTAL_MS, timeoutMs =
     PLACE_TIMEOUT_MS (= PLACE_TOTAL_MS), a non-zero waitSliceMs, connectFirst, heapCheck, the shared
     sink with NET_SINK_CAP, stopFn = tileFetchCallRecent, method GET and no body.
  4. THE REQUEST'S SHAPE. PLACE_LIMIT is 8; the default template is https to photon.komoot.io with
     limit={n} and lang=en; placeSearchStart builds the URL with PLACE_LIMIT; PLACE_BIAS_DECIMALS is 3
     and placeFmtBias rounds with lround(v * 1000.0) and prints "%03ld"; placeBuildUrl puts {lat} and
     {lon} through placeFmtBias only (no float printf in it).
  5. HTTPS ONLY. placeTemplateCheck tests the "https://" prefix; placeSetSearchUrl asks it BEFORE
     putString; loadSettings asks it of the stored template too.
  6. NEVER RETRIED, NEVER REDIRECTED. Exactly one netHttps() in placesearch_net.cpp, in placeGet,
     with no loop in placeGet or placeJob; netWorkerSubmit(NET_JOB_PLACE, ...) once, in
     placeSearchStart; setFollowRedirects nowhere.
  7. CANCEL HONOURED. placeSearchCancel sets s_cancel; placeGet passes &s_cancel; placeLoopTick's
     first branch is `if (s_cancel)` and makes the state PLACE_IDLE; placeRequestActive is
     s_busy || s_done; placeJob sets s_done BEFORE it drops s_busy (the predicate never blinks).
  8. THE WORKER. https_worker.h's job enum has NET_JOB_PLACE; netRequestActive() includes
     placeRequestActive() (WiPhone.ino's busy, hardBusy and the 10-tick sleep); WiPhone.ino's loop
     calls placeLoopTick(); placeSearchStart calls tlsInstallPsramHook() before the submit.
  9. THE GATES. placeSearchStart refuses when the setting is off, when WiFi is off or not connected,
     when another job is on the worker (aiRequestActive || wxRequestActive || geoRequestActive - never
     queued behind one), on gateWhy() (a map download, the uploader, a sync window, a call, a game, a
     Files job), under the heap bar, and without a worker; placeJob asks gateWhy() again on the worker.
 10. NO URL, QUERY OR COORDINATE IN A LOG LINE: no log_*() in placesearch_net.cpp is given the url,
     the query, the template, a lat/lon or a %f; `maps search` prints the HOST, never the template or
     the URL (placeReport and placeConsoleDrain read ->url nowhere and ->tmpl only as a flag); the
     console's searchurl branch echoes the host, not the template.
 11. NVS. The namespace is "maps", the keys "search" (getInt with 1 as the absent value) and
     "searchurl" (isKey() before getString(); remove() on clear); placeSetSearchUrl refuses while
     placeRequestActive() - the worker reads ->host (DNS, the connect, the SNI) while a request runs.
 12. THE CONSOLE. serial_cmd.cpp handles `maps searchurl` (before `maps search`), `maps search <text>`
     (placeConsoleSearch) and `maps search` (placeReport); serialCmdLoop drains a finished console
     search (placeConsoleDrain); the help names both commands; placeSearchStart refuses while a console
     answer is still unprinted (consolePending tested BEFORE it is cleared - else the answer is lost).
 13. MEMORY. The module's state is one PSRAM calloc (MALLOC_CAP_SPIRAM); the console's rows are
     PSRAM too; no Arduino String in placesearch_net.cpp; the pure half includes no Arduino, WiFi,
     ESP-IDF, Preferences or SD header.
 14. THE SCREEN (app_maps.cpp, implementer B). dropPin() asks placeLastPlace() and placeNearPx(zoom, ...,
     PLACE_NEAR_PX) and puts the name through mapPinSanitizeName (the pin-name prefill); ~MapsApp() calls
     placeForgetLastPlace(); the MAPS_SEARCH case never tests LOGIC_BUTTON_BACK (Back is backspace), END
     leaves, Back while THIS SCREEN'S search runs calls placeSearchCancel(), OK runs searchStart(); buildMenu() has the
     three row texts and placeSettingRow(); buildSearch() sets controlState.t9Field AFTER
     setInputState(AlphaNum) and opens on placeLastQuery(); searchStart() hands placeSearchStart the text as
     typed; buildSearchList() keeps the rows in PSRAM and calls placeRows(..., gUnits, ...); the list's OK
     calls placeSetLastPlace() before centreOn() and looks the result up again (placeResult) on the gen the
     list was built on (placeSearchGen() == searchGen - a console search that finished under the list
     replaced them); none of the screen's search functions logs anything.

THE SELF-TEST MUTATES THE REAL FILES IN PLACE (the spec's rule: "mutation-check the key rules IN
PLACE with an md5-verified restore"): each mistake is written into the source on disk, the check is
run again from disk - and, for the rules a constant carries, tests/test_placesearch.cpp is compiled
and run against the mutant too - and the file is restored from a backup whose MD5 was verified
before the write and checked again after the restore. A mutation nothing catches fails the run.
⚠ Run it alone: a firmware build reading WiPhone/ while a mutant is on disk would compile it.
"""
import hashlib
import os
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_wifi_restore import function_body, match_close, strip_code  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parent.parent
ROOT = REPO / "WiPhone"
FILES = ("placesearch.h", "placesearch.cpp", "placesearch_net.h", "placesearch_net.cpp", "https_worker.h",
         "https_worker.cpp", "serial_cmd.cpp", "WiPhone.ino", "isrg_roots.h", "app_maps.cpp")
TEST = "tests/test_placesearch.cpp"
TEST_DEPS = ("WiPhone/placesearch.cpp", "WiPhone/gemini.cpp", "WiPhone/json_read.cpp", "WiPhone/mesh_pos.cpp",
             "WiPhone/units.cpp", "WiPhone/map_tiles.cpp", "WiPhone/book_hash.cpp")
OUT_DIR = REPO / ".pio" / "hosttests"
UA = '"WiPhone-NH/" FIRMWARE_VERSION " maps-search (+https://github.com/Nikguy321/wiphone-meshtastic)"'


def load():
    return {name: (ROOT / name).read_text(encoding="utf-8", errors="replace") for name in FILES}


def body_of(code, name):
    span = function_body(code, name)
    return None if span is None else code[span[0]:span[1]]


def define(raw, name):
    m = re.search(r"^#define\s+%s\s+((?:.*\\\n)*.*)$" % re.escape(name), raw, re.M)
    return re.sub(r"\\\n", " ", m.group(1)).strip() if m else None


def strip_trailing_comment(v):
    return re.sub(r"\s*//.*$", "", v).strip() if v is not None else None


def count(rx, text):
    return len(re.findall(rx, text))


def check(texts):
    out = []
    code = {k: strip_code(v) for k, v in texts.items()}
    pn, pnr = code["placesearch_net.cpp"], texts["placesearch_net.cpp"]
    pc, pcr = code["placesearch.cpp"], texts["placesearch.cpp"]
    phr = texts["placesearch.h"]
    hw, hh = code["https_worker.cpp"], texts["https_worker.h"]
    sc, scr = code["serial_cmd.cpp"], texts["serial_cmd.cpp"]
    ino = code["WiPhone.ino"]

    def need(fn, where=pn, label="placesearch_net.cpp"):
        b = body_of(where, fn)
        if b is None:
            out.append("%s: %s() not found" % (label, fn))
        return b

    get = need("placeGet")
    getr = None
    if get is not None:
        span = function_body(pn, "placeGet")
        getr = pnr[span[0]:span[1]]
    job = need("placeJob")
    start = need("placeSearchStart")
    tick = need("placeLoopTick")
    cancel = need("placeSearchCancel")
    active = need("placeRequestActive")
    gate = need("gateWhy")
    load_ = need("loadSettings")
    seturl = need("placeSetSearchUrl")
    report = need("placeReport")
    drain = need("placeConsoleDrain")

    # 1. the roots
    if not re.search(r"static const char PLACE_ROOTS_PEM\[\]\s*=\s*ISRG_ROOT_X1_PEM\s+ISRG_ROOT_YR_PEM\s*;", pnr):
        out.append("placesearch_net.cpp: PLACE_ROOTS_PEM must be exactly ISRG_ROOT_X1_PEM ISRG_ROOT_YR_PEM")
    if get is not None and not re.search(r"q\.rootsPem\s*=\s*PLACE_ROOTS_PEM\s*;", get):
        out.append("placesearch_net.cpp placeGet(): q.rootsPem = PLACE_ROOTS_PEM")
    for name in ("ISRG_ROOT_X1_PEM", "ISRG_ROOT_YR_PEM"):
        if define(texts["isrg_roots.h"], name) is None:
            out.append("isrg_roots.h: %s missing" % name)

    # 2. the User-Agent
    ua = define(pnr, "PLACE_USER_AGENT")
    if ua is None or re.sub(r"\s+", " ", ua) != UA:
        out.append("placesearch_net.cpp: PLACE_USER_AGENT must be %s (got %r)" % (UA, ua))
    if get is not None and not re.search(r"q\.userAgent\s*=\s*PLACE_USER_AGENT\s*;", get):
        out.append("placesearch_net.cpp placeGet(): q.userAgent = PLACE_USER_AGENT")

    # 3. the budgets
    if strip_trailing_comment(define(phr, "PLACE_CONNECT_MS")) != "6000u":
        out.append("placesearch.h: PLACE_CONNECT_MS must be 6000u")
    if strip_trailing_comment(define(phr, "PLACE_TOTAL_MS")) != "10000u":
        out.append("placesearch.h: PLACE_TOTAL_MS must be 10000u")
    if strip_trailing_comment(define(pnr, "PLACE_TIMEOUT_MS")) != "PLACE_TOTAL_MS":
        out.append("placesearch_net.cpp: PLACE_TIMEOUT_MS must be PLACE_TOTAL_MS (the header wait inside the budget)")
    slice_ = strip_trailing_comment(define(pnr, "PLACE_SLICE_MS"))
    if not slice_ or not re.fullmatch(r"[1-9]\d*u?", slice_):
        out.append("placesearch_net.cpp: PLACE_SLICE_MS must be a non-zero number (the client's sliced wait honours Cancel)")
    if get is not None:
        for field, value in (("connectMs", "PLACE_CONNECT_MS"), ("totalMs", "PLACE_TOTAL_MS"), ("timeoutMs", "PLACE_TIMEOUT_MS"),
                             ("waitSliceMs", "PLACE_SLICE_MS"), ("connectFirst", "true"), ("heapCheck", "true"),
                             ("sink", r"netWorkerSink\(\)"), ("cap", "NET_SINK_CAP"), ("stopFn", "tileFetchCallRecent"),
                             ("cancel", "&s_cancel")):
            if not re.search(r"q\.%s\s*=\s*%s\s*;" % (field, re.escape(value) if not value.startswith("netWorker") else value), get):
                out.append("placesearch_net.cpp placeGet(): q.%s = %s" % (field, value))
        if getr is not None and not re.search(r'q\.method\s*=\s*"GET"\s*;', getr):
            out.append("placesearch_net.cpp placeGet(): the search is a GET")
        if re.search(r"q\.body\s*=", get):
            out.append("placesearch_net.cpp placeGet(): a GET has no body")

    # 4. the request's shape
    if strip_trailing_comment(define(phr, "PLACE_LIMIT")) != "8":
        out.append("placesearch.h: PLACE_LIMIT must be 8")
    if strip_trailing_comment(define(phr, "PLACE_BIAS_DECIMALS")) != "3":
        out.append("placesearch.h: PLACE_BIAS_DECIMALS must be 3")
    if strip_trailing_comment(define(phr, "PLACE_HOST_DEFAULT")) != '"photon.komoot.io"':
        out.append("placesearch.h: PLACE_HOST_DEFAULT must be \"photon.komoot.io\"")
    tmpl = define(phr, "PLACE_URL_DEFAULT") or ""
    if not tmpl.startswith('"https://" PLACE_HOST_DEFAULT'):
        out.append("placesearch.h: PLACE_URL_DEFAULT must start \"https://\" PLACE_HOST_DEFAULT")
    if "limit={n}" not in tmpl:
        out.append("placesearch.h: PLACE_URL_DEFAULT must carry limit={n}")
    if "lang=en" not in tmpl:
        out.append("placesearch.h: PLACE_URL_DEFAULT must carry lang=en")
    if "{q}" not in tmpl or "lat={lat}" not in tmpl or "lon={lon}" not in tmpl:
        out.append("placesearch.h: PLACE_URL_DEFAULT must carry {q}, lat={lat} and lon={lon}")
    if start is not None and not re.search(r"placeBuildUrl\s*\([^;]*\bPLACE_LIMIT\b[^;]*;", start):
        out.append("placesearch_net.cpp placeSearchStart(): placeBuildUrl(..., PLACE_LIMIT, ...)")
    fmt = body_of(pc, "placeFmtBias")
    fmtr = pcr[function_body(pc, "placeFmtBias")[0]:function_body(pc, "placeFmtBias")[1]] if fmt is not None else ""
    if fmt is None or not re.search(r"lround\s*\(\s*v\s*\*\s*1000\.0\s*\)", fmt) or '%03ld' not in fmtr:
        out.append("placesearch.cpp placeFmtBias(): lround(v * 1000.0) and \"%03ld\" (3 decimals by integer arithmetic)")
    build = body_of(pc, "placeBuildUrl")
    buildr = pcr[function_body(pc, "placeBuildUrl")[0]:function_body(pc, "placeBuildUrl")[1]] if build is not None else ""
    if build is None or count(r"\bplaceFmtBias\s*\(\s*(lat|lon)\b", build) != 2:
        out.append("placesearch.cpp placeBuildUrl(): {lat} and {lon} go through placeFmtBias (two calls)")
    if re.search(r"%\.?\d*[fFeEgG]", buildr):
        out.append("placesearch.cpp placeBuildUrl(): no float printf in the URL builder")

    # 5. https only
    tc = body_of(pc, "placeTemplateCheck")
    tcr = pcr[function_body(pc, "placeTemplateCheck")[0]:function_body(pc, "placeTemplateCheck")[1]] if tc is not None else ""
    if tc is None or not re.search(r'strncmp\s*\(\s*t\s*,\s*"https://"\s*,\s*8\s*\)\s*!=\s*0', tcr):
        out.append("placesearch.cpp placeTemplateCheck(): the template must start \"https://\" (strncmp(t, \"https://\", 8) != 0 refused)")
    if seturl is not None:
        c = re.search(r"\bplaceTemplateCheck\s*\(\s*tmpl\b", seturl)
        p = re.search(r"\bputString\s*\(", seturl)
        if not c or not p or c.start() > p.start():
            out.append("placesearch_net.cpp placeSetSearchUrl(): placeTemplateCheck(tmpl, ...) before putString")
        if not re.search(r"if\s*\(\s*tmpl\s*&&\s*\*tmpl\s*&&\s*!\s*placeTemplateCheck\s*\(", seturl):
            out.append("placesearch_net.cpp placeSetSearchUrl(): a given template that fails placeTemplateCheck is refused")
    if load_ is not None and not re.search(r"if\s*\(\s*s_m->tmpl\[0\]\s*&&\s*!\s*placeTemplateCheck\s*\(\s*s_m->tmpl\b", load_):
        out.append("placesearch_net.cpp loadSettings(): a stored template is checked (placeTemplateCheck) before use")

    # 6. never retried, never redirected
    n_https = count(r"\bnetHttps\s*\(", pn)
    if n_https != 1:
        out.append("placesearch_net.cpp: exactly one netHttps() call (got %d) - a search is never retried" % n_https)
    if get is not None and count(r"\bnetHttps\s*\(", get) != 1:
        out.append("placesearch_net.cpp: the one netHttps() is in placeGet()")
    for name, b in (("placeGet", get), ("placeJob", job)):
        if b is not None and re.search(r"\b(for|while|do)\s*[\(\{]", b):
            out.append("placesearch_net.cpp %s(): no loop around the request" % name)
    if job is not None and count(r"\bplaceGet\s*\(", job) != 1:
        out.append("placesearch_net.cpp placeJob(): placeGet() once")
    n_sub = count(r"\bnetWorkerSubmit\s*\(\s*NET_JOB_PLACE\b", pn)
    if n_sub != 1 or (start is not None and count(r"\bnetWorkerSubmit\s*\(\s*NET_JOB_PLACE\b", start) != 1):
        out.append("placesearch_net.cpp: netWorkerSubmit(NET_JOB_PLACE, ...) exactly once, in placeSearchStart()")
    if re.search(r"setFollowRedirects", pn) or re.search(r"setFollowRedirects", hw):
        out.append("a redirect is never followed (setFollowRedirects appears)")
    if re.search(r"\bretr(y|ies|ied)\b", pn, re.I):
        out.append("placesearch_net.cpp: no retry in the code")

    # 7. cancel
    if cancel is None or not re.search(r"\bs_cancel\s*=\s*true\s*;", cancel):
        out.append("placesearch_net.cpp placeSearchCancel(): s_cancel = true")
    if tick is not None:
        m = re.search(r"if\s*\(\s*s_cancel\s*\)\s*\{", tick)
        first = re.search(r"\bif\s*\(", tick)
        if not m:
            out.append("placesearch_net.cpp placeLoopTick(): an `if (s_cancel)` branch (a cancelled search is no error)")
        elif first and first.start() < m.start() and "s_done" not in tick[first.start():m.start()]:
            out.append("placesearch_net.cpp placeLoopTick(): the cancel branch comes first (after the s_done test)")
        else:
            close = match_close(tick, m.end() - 1)
            if close < 0 or not re.search(r"state\s*=\s*PLACE_IDLE\s*;", tick[m.end():close]):
                out.append("placesearch_net.cpp placeLoopTick(): a cancelled search makes the state PLACE_IDLE")
        if not re.search(r"\bs_cancel\s*=\s*false\s*;", tick) or not re.search(r"\bs_done\s*=\s*false\s*;", tick):
            out.append("placesearch_net.cpp placeLoopTick(): clears s_cancel and s_done at its end")
    if active is None or not re.search(r"return\s+s_busy\s*\|\|\s*s_done\s*;", active):
        out.append("placesearch_net.cpp placeRequestActive(): return s_busy || s_done")
    if job is not None:
        d = re.search(r"\bs_done\s*=\s*true\s*;", job)
        b = re.search(r"\bs_busy\s*=\s*false\s*;", job)
        if not d or not b or d.start() > b.start():
            out.append("placesearch_net.cpp placeJob(): s_done = true BEFORE s_busy = false")

    # 8. the worker
    enum = re.search(r"enum\s*\{\s*NET_JOB_AI\s*=\s*0\s*,([^}]*)\}", hh)
    kinds = [k.strip() for k in enum.group(1).split(",")] if enum else []
    if "NET_JOB_PLACE" not in kinds or kinds[-1] != "NET_JOB_KINDS" or kinds.index("NET_JOB_PLACE") >= kinds.index("NET_JOB_KINDS"):
        out.append("https_worker.h: the job enum must hold NET_JOB_PLACE before NET_JOB_KINDS")
    nb = body_of(hw, "netRequestActive")
    if nb is None or not re.search(r"\bplaceRequestActive\s*\(\s*\)", nb):
        out.append("https_worker.cpp: netRequestActive() must include placeRequestActive()")
    if not re.search(r'#include\s+"placesearch_net\.h"', texts["https_worker.cpp"]):
        out.append("https_worker.cpp: #include \"placesearch_net.h\"")
    loop = body_of(ino, "loop")
    if loop is None or not re.search(r"\bplaceLoopTick\s*\(\s*\)\s*;", loop):
        out.append("WiPhone.ino loop(): placeLoopTick() every pass")
    if start is not None:
        h = re.search(r"\btlsInstallPsramHook\s*\(", start)
        s_ = re.search(r"\bnetWorkerSubmit\s*\(\s*NET_JOB_PLACE\b", start)
        if not h or not s_ or h.start() > s_.start():
            out.append("placesearch_net.cpp placeSearchStart(): tlsInstallPsramHook() before netWorkerSubmit(NET_JOB_PLACE, ...)")

    # 9. the gates
    if start is not None:
        for rx, what in ((r"if\s*\(\s*!\s*m->enabled\s*\)", "the setting (if (!m->enabled))"),
                         (r"if\s*\(\s*wifiState\.radioOff\(\)\s*\|\|\s*WiFi\.status\(\)\s*!=\s*WL_CONNECTED\s*\)", "WiFi off or not connected"),
                         (r"if\s*\(\s*aiRequestActive\(\)\s*\|\|\s*wxRequestActive\(\)\s*\|\|\s*geoRequestActive\(\)\s*\)",
                          "another job on the worker (never queued behind one)"),
                         (r"\bgateWhy\s*\(\s*\)", "gateWhy()"),
                         (r"\bnetHeapOkForHandshake\s*\(", "the heap bar"),
                         (r"\bnetWorkerReady\s*\(", "netWorkerReady"),
                         (r"\bplaceQuery\s*\(", "the query through placeQuery (an empty one refused)")):
            if not re.search(rx, start):
                out.append("placesearch_net.cpp placeSearchStart(): must gate on %s" % what)
        for rx, what in ((r"PLACE_ERR_NOWIFI", "PLACE_ERR_NOWIFI's words"), (r"PLACE_ERR_BUSY", "PLACE_ERR_BUSY's words"),
                         (r"PLACE_ERR_OFF", "PLACE_ERR_OFF's words"), (r"PLACE_ERR_EMPTY", "PLACE_ERR_EMPTY's words")):
            if not re.search(rx, start):
                out.append("placesearch_net.cpp placeSearchStart(): a refusal gives %s" % what)
        # every gate's `why` is set before the request is built: the URL after the last gate
        g = [m.start() for m in re.finditer(r"\bnetWorkerReady\s*\(", start)]
        u = re.search(r"\bplaceBuildUrl\s*\(", start)
        if g and u and g[0] > u.start():
            out.append("placesearch_net.cpp placeSearchStart(): the gates before the URL is built")
    if gate is not None:
        for fn in ("tileFetchActive", "xferOn", "kosyncWindowActive", "xferWindowUp", "tileFetchCallRecent", "filesJobActive"):
            if not re.search(r"\b%s\s*\(\s*\)" % fn, gate):
                out.append("placesearch_net.cpp gateWhy(): %s()" % fn)
        if not re.search(r"\bgGbcActive\b", gate) or not re.search(r"WL_CONNECTED", gate):
            out.append("placesearch_net.cpp gateWhy(): a game, and WiFi connected")
    if job is not None and not re.search(r"\bgateWhy\s*\(\s*\)", job):
        out.append("placesearch_net.cpp placeJob(): gateWhy() again on the worker")

    # 10. no URL, query or coordinate in a log line; the host only on the console
    for m in re.finditer(r"\blog_[eiwdv]\s*\(", pn):
        close = match_close(pn, m.end() - 1)
        args = pnr[m.end():close] if close > 0 else ""
        if re.search(r"\b(url|query|tmpl|refLat|refLon|lat|lon)\b", args) or re.search(r"%[\d.]*[fFeEgG]", args) or "->res" in args:
            out.append("placesearch_net.cpp:%d: a log line given the url / query / template / a coordinate / a result"
                       % (pn.count("\n", 0, m.start()) + 1))
    for name, b in (("placeReport", report), ("placeConsoleDrain", drain)):
        if b is None:
            continue
        if re.search(r"->url\b", b):
            out.append("placesearch_net.cpp %s(): never the URL" % name)
        if re.search(r"->tmpl\b(?!\[0\])", b):
            out.append("placesearch_net.cpp %s(): the template only as a flag (->tmpl[0]), never printed" % name)
    # the url is read only where the request is built and sent
    for m in re.finditer(r"\b(m|s_m)->url\b", pn):
        line = pn[pn.rfind("\n", 0, m.start()) + 1:pn.find("\n", m.start())]
        if not (re.search(r"\bplaceBuildUrl\s*\(", line) or re.search(r"q\.url\s*=", line) or re.search(r"sizeof\s*\(", line)):
            out.append("placesearch_net.cpp:%d: ->url read outside placeBuildUrl / q.url" % (pn.count("\n", 0, m.start()) + 1))
    if scr is not None:
        m = re.search(r'if\s*\(\s*!strncasecmp\s*\(\s*arg\s*,\s*"searchurl"\s*,\s*9\s*\)', scr)
        if m:
            close = match_close(sc, sc.find("{", m.start()))
            branch = scr[m.start():close] if close > 0 else ""
            if re.search(r"\bsay\s*\([^;]*\btmpl\b", branch):
                out.append("serial_cmd.cpp maps searchurl: say() never given the template (the host only)")

    # 11. NVS
    if strip_trailing_comment(define(pnr, "PLACE_NVS")) != '"maps"':
        out.append("placesearch_net.cpp: PLACE_NVS is \"maps\"")
    if strip_trailing_comment(define(pnr, "PLACE_NVS_ON")) != '"search"' or strip_trailing_comment(define(pnr, "PLACE_NVS_URL")) != '"searchurl"':
        out.append("placesearch_net.cpp: the keys are \"search\" and \"searchurl\"")
    if load_ is not None:
        if not re.search(r"p\.getInt\s*\(\s*PLACE_NVS_ON\s*,\s*1\s*\)", load_):
            out.append("placesearch_net.cpp loadSettings(): getInt(PLACE_NVS_ON, 1) - absent = on")
        k = re.search(r"if\s*\(\s*p\.isKey\s*\(\s*PLACE_NVS_URL\s*\)\s*\)\s*\{", load_)
        g = re.search(r"p\.getString\s*\(\s*PLACE_NVS_URL\b", load_)
        if not k or not g or not (k.end() <= g.start() < match_close(load_, k.end() - 1)):
            out.append("placesearch_net.cpp loadSettings(): isKey(PLACE_NVS_URL) before getString (an absent key logs an error line)")
        if not re.search(r"p\.begin\s*\(\s*PLACE_NVS\s*,\s*true\s*\)", load_):
            out.append("placesearch_net.cpp loadSettings(): a read-only begin")
    se = body_of(pn, "placeSetEnabled")
    if se is None or not re.search(r"p\.putInt\s*\(\s*PLACE_NVS_ON\b", se):
        out.append("placesearch_net.cpp placeSetEnabled(): putInt(PLACE_NVS_ON, ...)")
    if seturl is not None and not re.search(r"p\.remove\s*\(\s*PLACE_NVS_URL\s*\)", seturl):
        out.append("placesearch_net.cpp placeSetSearchUrl(): clear = remove(PLACE_NVS_URL)")
    if seturl is not None and not re.search(r"\bplaceRequestActive\s*\(\s*\)", seturl):
        out.append("placesearch_net.cpp placeSetSearchUrl(): refuses while placeRequestActive() (the worker reads ->host)")

    # 12. the console
    i_url = scr.find('!strncasecmp(arg, "searchurl", 9)')
    i_s = scr.find('!strncasecmp(arg, "search", 6)')
    if i_url < 0 or i_s < 0 or i_url > i_s:
        out.append("serial_cmd.cpp: `maps searchurl` is handled, before `maps search`")
    for fn in ("placeConsoleSearch", "placeReport", "placeSetSearchUrl"):
        if not re.search(r"\b%s\s*\(" % fn, sc):
            out.append("serial_cmd.cpp: %s() is called" % fn)
    scl = body_of(sc, "serialCmdLoop")
    if scl is None or not re.search(r"\bplaceConsoleDrain\s*\(\s*sayLine\s*\)\s*;", scl):
        out.append("serial_cmd.cpp serialCmdLoop(): placeConsoleDrain(sayLine) every pass")
    if "maps search <text>" not in scr or "maps searchurl" not in scr:
        out.append("serial_cmd.cpp: the help names `maps search <text>` and `maps searchurl`")
    if not re.search(r'#include\s+"placesearch_net\.h"', scr):
        out.append("serial_cmd.cpp: #include \"placesearch_net.h\"")
    if start is not None:
        t_ = re.search(r"if\s*\(\s*s_m\s*&&\s*s_m->consolePending\s*\)", start)
        c_ = re.search(r"\bm->consolePending\s*=\s*false\s*;", start)
        if not t_ or not c_ or t_.start() > c_.start():
            out.append("placesearch_net.cpp placeSearchStart(): an unprinted console answer (consolePending) is refused BEFORE it is cleared")

    # 13. memory and purity
    if not re.search(r"heap_caps_calloc\s*\(\s*1\s*,\s*sizeof\s*\(\s*PlaceMem\s*\)\s*,\s*MALLOC_CAP_SPIRAM\s*\)", pn):
        out.append("placesearch_net.cpp: the state is one PSRAM calloc (MALLOC_CAP_SPIRAM)")
    if drain is not None and not re.search(r"heap_caps_malloc\s*\([^;]*MALLOC_CAP_SPIRAM\s*\)", drain):
        out.append("placesearch_net.cpp placeConsoleDrain(): the rows in PSRAM")
    if re.search(r"\bString\b", pn):
        out.append("placesearch_net.cpp: no Arduino String")
    for name in ("placesearch.h", "placesearch.cpp"):
        if re.search(r'#include\s*[<"](Arduino|WiFi|esp_|freertos|Preferences|SD|HTTPClient|driver/)', texts[name]):
            out.append("%s: the pure half includes no Arduino / IDF header" % name)

    # 14. the screen (app_maps.cpp)
    am = code["app_maps.cpp"]
    drop = body_of(am, "MapsApp::dropPin")
    if drop is None or not re.search(r"\bplaceLastPlace\s*\(", drop) or \
            not re.search(r"\bplaceNearPx\s*\(\s*zoom\s*,[^;]*\bPLACE_NEAR_PX\s*\)", drop):
        out.append("app_maps.cpp dropPin(): the pin-name prefill asks placeLastPlace() and placeNearPx(zoom, ..., PLACE_NEAR_PX)")
    elif not re.search(r"\bmapPinSanitizeName\s*\(", drop):
        out.append("app_maps.cpp dropPin(): the place's name goes through mapPinSanitizeName")
    dtor = body_of(am, "MapsApp::~MapsApp")
    if dtor is None or not re.search(r"\bplaceForgetLastPlace\s*\(\s*\)\s*;", dtor):
        out.append("app_maps.cpp ~MapsApp(): placeForgetLastPlace() - the last place lives only while the map is open")
    pe = body_of(am, "MapsApp::processEvent") or ""    # (enterState has a `case MAPS_SEARCH:` of its own)
    i0 = pe.find("case MAPS_SEARCH:")
    i1 = pe.find("case MAPS_HELP:", i0 if i0 >= 0 else 0)
    srch = pe[i0:i1] if i0 >= 0 and i1 > i0 else ""
    if not srch:
        out.append("app_maps.cpp processEvent(): a `case MAPS_SEARCH:` before `case MAPS_HELP:`")
    else:
        if "LOGIC_BUTTON_BACK" in srch:
            out.append("app_maps.cpp MAPS_SEARCH: Back is backspace - never LOGIC_BUTTON_BACK on a text screen")
        if "WIPHONE_KEY_END" not in srch:
            out.append("app_maps.cpp MAPS_SEARCH: END leaves the screen")
        m = re.search(r"if\s*\(\s*event\s*==\s*WIPHONE_KEY_BACK\s*\)\s*\{", srch)
        if not m or "placeSearchCancel()" not in srch[m.end():match_close(srch, m.end() - 1)]:
            out.append("app_maps.cpp MAPS_SEARCH: Back while a search runs calls placeSearchCancel()")
        if not re.search(r"\bsearchStart\s*\(\s*\)\s*;", srch):
            out.append("app_maps.cpp MAPS_SEARCH: OK / Search runs searchStart()")
    bm = body_of(am, "MapsApp::buildMenu")
    for tok in ("PLACE_ROW_SEARCH_OFF", "PLACE_ROW_SEARCH_NOWIFI", "PLACE_ROW_SEARCH", "placeSettingRow("):
        if bm is None or tok not in bm:
            out.append("app_maps.cpp buildMenu(): %s" % tok)
    bs = body_of(am, "MapsApp::buildSearch")
    if bs is None:
        out.append("app_maps.cpp: buildSearch() not found")
    else:
        a = re.search(r"setInputState\s*\(\s*InputType::AlphaNum\s*\)", bs)
        t9 = re.search(r"controlState\.t9Field\s*=\s*true\s*;", bs)
        if not a or not t9 or t9.start() < a.start():
            out.append("app_maps.cpp buildSearch(): controlState.t9Field = true AFTER setInputState(AlphaNum), which clears it")
        if not re.search(r"\bplaceLastQuery\s*\(\s*\)", bs):
            out.append("app_maps.cpp buildSearch(): the field opens on placeLastQuery()")
    ss = body_of(am, "MapsApp::searchStart")
    if ss is None or not re.search(r"\bplaceSearchStart\s*\(\s*typed\b", ss):
        out.append("app_maps.cpp searchStart(): placeSearchStart(typed, ...) - the text as typed")
    bl = body_of(am, "MapsApp::buildSearchList")
    if bl is None or not re.search(r"\(PlaceRow\*\)\s*ps_malloc\s*\(", bl) or not re.search(r"\bplaceRows\s*\([^;]*\bgUnits\b", bl):
        out.append("app_maps.cpp buildSearchList(): the rows in PSRAM (ps_malloc) and placeRows(..., gUnits, ...)")
    m = re.search(r"else\s+if\s*\(\s*listKind\s*==\s*LIST_SEARCH\s*\)\s*\{", am)
    if not m:
        out.append("app_maps.cpp MAPS_LIST OK: a LIST_SEARCH branch")
    else:
        blk = am[m.end():match_close(am, m.end() - 1)]
        s_ = re.search(r"\bplaceSetLastPlace\s*\(", blk)
        c_ = re.search(r"\bcentreOn\s*\(", blk)
        if not s_ or not c_ or s_.start() > c_.start():
            out.append("app_maps.cpp MAPS_LIST OK (LIST_SEARCH): placeSetLastPlace() before centreOn()")
        if not re.search(r"\bplaceResult\s*\(", blk):
            out.append("app_maps.cpp MAPS_LIST OK (LIST_SEARCH): the result looked up again (placeResult)")
        if not re.search(r"\bplaceSearchGen\s*\(\s*\)\s*==\s*searchGen\b", blk):
            out.append("app_maps.cpp MAPS_LIST OK (LIST_SEARCH): the result is this list's (placeSearchGen() == searchGen), else 'gone'")
    for fn in ("MapsApp::buildSearch", "MapsApp::searchStart", "MapsApp::searchSettle", "MapsApp::buildSearchList",
               "MapsApp::drawSearch"):
        b = body_of(am, fn)
        if b is None:
            out.append("app_maps.cpp: %s() not found" % fn)
        elif re.search(r"\blog_[eiwdv]\s*\(", b):
            out.append("app_maps.cpp %s(): no log line (the query and the names are the screen's only)" % fn)
    return out


# ── the self-test: the real files, mutated IN PLACE, restored from an MD5-verified backup ──

# (what, file, mutation, compile): `compile` = tests/test_placesearch.cpp is compiled and run against
# the mutant too, and the mutation counts as caught when the glue OR the test trips.
MUTATIONS = [
    ("the roots: X1 alone", "placesearch_net.cpp",
     lambda t: t.replace("= ISRG_ROOT_X1_PEM ISRG_ROOT_YR_PEM;", "= ISRG_ROOT_X1_PEM;", 1), False),
    ("the roots replaced in the request", "placesearch_net.cpp",
     lambda t: t.replace("q.rootsPem = PLACE_ROOTS_PEM;", "q.rootsPem = ISRG_ROOT_X1_PEM;", 1), False),
    ("a generic User-Agent", "placesearch_net.cpp",
     lambda t: t.replace('#define PLACE_USER_AGENT   "WiPhone-NH/" FIRMWARE_VERSION " maps-search (+https://github.com/Nikguy321/wiphone-meshtastic)"',
                         '#define PLACE_USER_AGENT   "WiPhone"', 1), False),
    ("the handshake cap 10 s", "placesearch.h",
     lambda t: t.replace("#define PLACE_CONNECT_MS     6000u", "#define PLACE_CONNECT_MS     10000u", 1), True),
    ("the whole budget 20 s", "placesearch.h",
     lambda t: t.replace("#define PLACE_TOTAL_MS       10000u", "#define PLACE_TOTAL_MS       20000u", 1), True),
    ("no total budget in the request", "placesearch_net.cpp",
     lambda t: t.replace("  q.totalMs = PLACE_TOTAL_MS;", "", 1), False),
    ("the limit 20", "placesearch.h",
     lambda t: t.replace("#define PLACE_LIMIT          8 ", "#define PLACE_LIMIT          20", 1), True),
    ("a hard-coded limit in the request", "placesearch_net.cpp",
     lambda t: t.replace("placeBuildUrl(m->tmpl, q, refLat, refLon, PLACE_LIMIT,", "placeBuildUrl(m->tmpl, q, refLat, refLon, 50,", 1), False),
    ("lang dropped from the default", "placesearch.h",
     lambda t: t.replace("&limit={n}&lang=en", "&limit={n}", 1), True),
    ("the bias to 5 decimals", "placesearch.cpp",
     lambda t: t.replace("lround(v * 1000.0)", "lround(v * 100000.0)", 1).replace('"%s%ld.%03ld"', '"%s%ld.%05ld"', 1)
                .replace("s / 1000, s % 1000", "s / 100000, s % 100000", 1), True),
    ("a printf bias in the URL", "placesearch.cpp",
     lambda t: t.replace("      k = placeFmtBias(lat, out + n, cap - n);",
                         "      { const int w = snprintf(out + n, cap - n, \"%.5f\", lat); k = (w > 0 && (size_t)w < cap - n) ? (size_t)w : 0; }", 1), True),
    ("http:// accepted", "placesearch.cpp",
     lambda t: t.replace('strncmp(t, "https://", 8) != 0', 'strncmp(t, "http", 4) != 0', 1), True),
    ("a :port accepted (the worker connects to 443 only)", "placesearch.cpp",
     lambda t: t.replace('    setWhy(why, whyCap, "no :port - the phone connects to 443 only");\n    return 0;\n',
                         "    const char* d = ++p;\n    while (*p >= '0' && *p <= '9') p++;\n    if (p == d) {\n"
                         '      setWhy(why, whyCap, "a port must be a number");\n      return 0;\n    }\n', 1), True),
    ("the endpoint set under a running search", "placesearch_net.cpp",
     lambda t: t.replace('  if (placeRequestActive()) {\n    if (why) strlcpy(why, "a search is running - set it when it ends", whyCap);\n    return false;\n  }\n',
                         "", 1), False),
    ("an unprinted console answer overwritten", "placesearch_net.cpp",
     lambda t: t.replace("  if (s_m && s_m->consolePending) {\n", "  if (false) {\n", 1), False),
    ("the template set unchecked", "placesearch_net.cpp",
     lambda t: t.replace("  if (tmpl && *tmpl && !placeTemplateCheck(tmpl, why, whyCap)) {\n    return false;",
                         "  if (false) {\n    return false;", 1), False),
    ("the stored template unchecked", "placesearch_net.cpp",
     lambda t: t.replace("  if (s_m->tmpl[0] && !placeTemplateCheck(s_m->tmpl, NULL, 0)) {", "  if (false) {", 1), False),
    ("a retry loop", "placesearch_net.cpp",
     lambda t: t.replace("    placeGet(m);\n", "    for (int i = 0; i < 2 && m->code != 200; i++) placeGet(m);\n", 1), False),
    ("a second request", "placesearch_net.cpp",
     lambda t: t.replace("  netHttps(&q, &r);\n", "  netHttps(&q, &r);\n  if (r.code != 200) netHttps(&q, &r);\n", 1), False),
    ("cancel not passed to the request", "placesearch_net.cpp",
     lambda t: t.replace("  q.cancel = &s_cancel;\n", "", 1), False),
    ("cancel never set", "placesearch_net.cpp",
     lambda t: t.replace("    s_cancel = true;                              // the request ends", "    (void)0;                                      // the request ends", 1), False),
    ("a cancelled search shown as an error", "placesearch_net.cpp",
     lambda t: t.replace("  if (s_cancel) {\n    /* Back while it ran", "  if (false) {\n    /* Back while it ran", 1), False),
    ("s_busy dropped before s_done", "placesearch_net.cpp",
     lambda t: t.replace("  s_done = true;                                  // before s_busy drops: placeRequestActive never blinks\n  s_busy = false;",
                         "  s_busy = false;\n  s_done = true;", 1), False),
    ("the worker's predicate without the place search", "https_worker.cpp",
     lambda t: t.replace("  if (placeRequestActive()) {\n    return true;                                   // the map's place search (D-177)\n  }\n", "", 1), False),
    ("the kind dropped from the enum", "https_worker.h",
     lambda t: t.replace("NET_JOB_GEO, NET_JOB_PLACE, NET_JOB_KINDS", "NET_JOB_GEO, NET_JOB_KINDS", 1), False),
    ("the loop tick dropped", "WiPhone.ino",
     lambda t: t.replace("      placeLoopTick();\n", "", 1), False),
    ("the PSRAM hook after the submit", "placesearch_net.cpp",
     lambda t: t.replace("  tlsInstallPsramHook();                          // BEFORE the first WiFiClientSecure (tile_fetch.h)\n", "", 1)
                .replace("  netWorkerSubmit(NET_JOB_PLACE, placeJob);\n", "  netWorkerSubmit(NET_JOB_PLACE, placeJob);\n  tlsInstallPsramHook();\n", 1), False),
    ("queued behind the AI", "placesearch_net.cpp",
     lambda t: t.replace("  if (aiRequestActive() || wxRequestActive() || geoRequestActive()) {", "  if (false) {", 1), False),
    ("the Wi-Fi gate dropped", "placesearch_net.cpp",
     lambda t: t.replace("  if (wifiState.radioOff() || WiFi.status() != WL_CONNECTED) {\n    placeErrLine(PLACE_ERR_NOWIFI",
                         "  if (false) {\n    placeErrLine(PLACE_ERR_NOWIFI", 1), False),
    ("the setting ignored", "placesearch_net.cpp",
     lambda t: t.replace("  if (!m->enabled) {", "  if (false) {", 1), False),
    ("the worker's gate dropped", "placesearch_net.cpp",
     lambda t: t.replace("  const char* gate = gateWhy();                   // again, on the worker", "  const char* gate = NULL;                        // again, on the worker", 1), False),
    ("the URL in a log line", "placesearch_net.cpp",
     lambda t: t.replace('log_e("place search: cancelled");', 'log_e("place search: cancelled %s", m->url);', 1), False),
    ("the query in a log line", "placesearch_net.cpp",
     lambda t: t.replace('log_e("place search: failed http %d", m->code);', 'log_e("place search: failed http %d for %s", m->code, m->query);', 1), False),
    ("a coordinate in a log line", "placesearch_net.cpp",
     lambda t: t.replace('log_e("place search: ok %d %u.%u s", m->n,', 'log_e("place search: ok %d %u.%u s %.5f", m->refLat, m->n,', 1), False),
    ("the template printed whole by `maps search`", "placesearch_net.cpp",
     lambda t: t.replace("           m->host, m->tmpl[0] ?", "           m->tmpl, m->tmpl[0] ?", 1), False),
    ("the template echoed by `maps searchurl`", "serial_cmd.cpp",
     lambda t: t.replace('say("maps searchurl: set - host %s (https; the roots pinned are Let\'s Encrypt\'s ISRG roots)\\n", host);',
                         'say("maps searchurl: set - %s\\n", tmpl);', 1), False),
    ("getString without isKey", "placesearch_net.cpp",
     lambda t: t.replace("    if (p.isKey(PLACE_NVS_URL)) {\n      p.getString(PLACE_NVS_URL, s_m->tmpl, sizeof(s_m->tmpl));\n    }",
                         "    p.getString(PLACE_NVS_URL, s_m->tmpl, sizeof(s_m->tmpl));", 1), False),
    ("the setting's absent value Off", "placesearch_net.cpp",
     lambda t: t.replace("p.getInt(PLACE_NVS_ON, 1)", "p.getInt(PLACE_NVS_ON, 0)", 1), False),
    ("the console drain dropped", "serial_cmd.cpp",
     lambda t: t.replace("  placeConsoleDrain(sayLine);\n", "", 1), False),
    ("an Arduino header in the pure half", "placesearch.cpp",
     lambda t: t.replace("#include <math.h>\n", "#include <Arduino.h>\n#include <math.h>\n", 1), False),
    ("the state on the internal heap", "placesearch_net.cpp",
     lambda t: t.replace("heap_caps_calloc(1, sizeof(PlaceMem), MALLOC_CAP_SPIRAM)", "calloc(1, sizeof(PlaceMem))", 1), False),
    # the screen (contract 14)
    ("the prefill without the 10 px rule", "app_maps.cpp",
     lambda t: t.replace("placeNearPx(zoom, pla, plo, lat, lon, PLACE_NEAR_PX)", "true", 1), False),
    ("the last place kept past the app", "app_maps.cpp",
     lambda t: t.replace("  placeForgetLastPlace();\n", "", 1), False),
    ("Back while searching does not cancel", "app_maps.cpp",
     lambda t: t.replace("        if (searchMine && searchMine == placeSearchGen()) {\n          placeSearchCancel();\n",
                         "        if (searchMine && searchMine == placeSearchGen()) {\n", 1), False),
    ("another search's result centred on from the list", "app_maps.cpp",
     lambda t: t.replace("        if (p && placeSearchGen() == searchGen) {", "        if (p) {", 1), False),
    ("Back tested as a soft key on the text screen", "app_maps.cpp",
     lambda t: t.replace("    if (searchRunning) {\n      if (event == WIPHONE_KEY_BACK) {",
                         "    if (LOGIC_BUTTON_BACK(event)) {\n      enterState(MAPS_MENU);\n      return REDRAW_ALL;\n    }\n"
                         "    if (searchRunning) {\n      if (event == WIPHONE_KEY_BACK) {", 1), False),
    ("the setting row dropped", "app_maps.cpp",
     lambda t: t.replace("  menu->addOption(placeSettingRow(placeSearchEnabled()), ROW_M_SEARCH_ON);   // D-177: NVS maps/search\n", "", 1), False),
    ("the place chosen not remembered", "app_maps.cpp",
     lambda t: t.replace("          placeSetLastPlace(p->name, p->lat, p->lon);   // the pin-name prefill (dropPin)\n", "", 1), False),
    ("T9 not opted in", "app_maps.cpp",
     lambda t: t.replace("  /* AFTER setInputState, which clears it: a field gets predictive text by opting in. */\n  controlState.t9Field = true;\n", "", 1), False),
    ("the rows on the loop task's stack", "app_maps.cpp",
     lambda t: t.replace("(PlaceRow*)ps_malloc(sizeof(PlaceRow) * PLACE_LIMIT)", "(PlaceRow*)alloca(sizeof(PlaceRow) * PLACE_LIMIT)", 1), False),
    ("the query in a log line on the screen", "app_maps.cpp",
     lambda t: t.replace("  searchMine = searchGen = placeSearchGen();\n",
                         "  log_e(\"place search: '%s'\", typed);\n  searchMine = searchGen = placeSearchGen();\n", 1), False),
]


def host_test_passes():
    """Compile tests/test_placesearch.cpp against WiPhone/ AS IT IS ON DISK NOW and run it."""
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    exe = OUT_DIR / "check_place_glue_mutant"
    cxx = os.environ.get("CXX", "c++")
    cmd = [cxx, "-std=c++11", "-O1", "-w", "-g", "-fsanitize=address,undefined", "-o", str(exe), TEST] + list(TEST_DEPS)
    r = subprocess.run(cmd, cwd=str(REPO), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0:
        return False                                   # a mutant that does not compile is caught too
    r = subprocess.run([str(exe)], cwd=str(REPO), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return r.returncode == 0


def md5_of(data):
    return hashlib.md5(data).hexdigest()


def mutate_in_place(name, f, compile_):
    """Write f(file) over the real file, run the check (and the host test when asked) from DISK,
    restore from an MD5-verified backup. Returns (applied, caught_by_glue, caught_by_test)."""
    path = ROOT / name
    original = path.read_bytes()
    digest = md5_of(original)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    backup = OUT_DIR / (name + ".place_glue.bak")
    backup.write_bytes(original)
    if md5_of(backup.read_bytes()) != digest:
        print("  BACKUP FAILED for %s - nothing mutated" % name)
        sys.exit(2)
    text = original.decode("utf-8")
    mutated = f(text)
    if mutated == text:
        backup.unlink()
        return False, False, False
    glue = test = False
    try:
        path.write_bytes(mutated.encode("utf-8"))
        glue = bool(check(load()))
        if compile_:
            test = not host_test_passes()
    finally:
        path.write_bytes(backup.read_bytes())
        if md5_of(path.read_bytes()) != digest:
            print("  🛑 RESTORE FAILED for %s - the backup is at %s" % (name, backup))
            sys.exit(2)
        backup.unlink()
    return True, glue, test


def selftest():
    if check(load()):
        return False
    if not host_test_passes():
        print("  SELF-TEST: test_placesearch does not pass on the real sources - nothing can be measured")
        return False
    ok = True
    for what, name, f, compile_ in MUTATIONS:
        applied, glue, test = mutate_in_place(name, f, compile_)
        if not applied:
            print("  SELF-TEST: %s - the mutation no longer applies (update it with the code)" % what)
            ok = False
            continue
        if not glue and not test:
            print("  SELF-TEST: %s - not caught" % what)
            ok = False
    # every file back as it was, byte for byte
    for name in FILES:
        if (OUT_DIR / (name + ".place_glue.bak")).exists():
            print("  SELF-TEST: a backup of %s is still there" % name)
            ok = False
    return ok


def main():
    problems = check(load())
    for p in problems:
        print("  FAIL " + p)
    if problems:
        print("check_place_glue: %d problem(s)" % len(problems))
        return 1
    if not selftest():
        print("check_place_glue: SELF-TEST FAILED")
        return 1
    print("check_place_glue: ok (14 contracts, %d in-place mutations caught, every file restored md5-verified)" % len(MUTATIONS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
