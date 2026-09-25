#!/usr/bin/env python3
"""check_health_log.py - the HEALTH line stays out of loop()'s frame, and WiFi link lines reach the
card at most one pair a minute (review F1 and F3, 0.9.79).

F1. The HEALTH line's buffers (hl[256], kl[176]) have their addresses passed to snprintf, so while
    they sat inline in loop() the compiler reserved them for the WHOLE pass - under every call
    loop() makes, the deepest being a Books picture page (~7.6 KB of the 8 KB loop task). objdump:
    loop() was 560 B on main, 640 B with this branch's longer hl, and is 240 B with the line built
    in healthLineTick(). That only holds while healthLineTick() is `noinline` (a static function
    with one caller is exactly what -Os inlines), while loop() declares no big buffer of its own,
    and while the calls that reach app redraws and the power-off saves (BATTERY_UPDATE_EVENT,
    powerOff) stay in loop() and out of the helper - inside it they would run on top of hl and kl.

F3. At the edge of an AP the core's auto-reconnect can associate and drop every 10-20 s, and each
    WIFI LOST / WIFI JOIN (and each MARK assoc/disassoc) was an SD open/append/close with no limit
    - a flapping hour pushed the boot reset_reason lines off a log that keeps ~9 h. WifiCardGate
    (wifi_diag.h, host-tested in tests/test_wifidiag.cpp) decides; these contracts pin that every
    such card write actually ASKS it: each wifiDiagDurable() call passes s_card.lost()/join(), the
    helper writes the card only under that answer, diagTick() writes no card line directly and
    wakes for the gate's summary, and the 2 s status poll goes through wifiMarkLink(), whose card
    write is gated the same way.

Each is a POSITIVE contract; one that cannot find its function fails too. The self-test REMOVES
each guard from the real sources, one at a time, and requires its contract to trip.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_wifi_restore import function_body, ifs, strip_code  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent / "WiPhone"
FILES = ("WiPhone.ino", "Networks.cpp")
BIG = 128   # a char/uint8_t array this size or more does not belong in loop()'s own frame


def span(code, name):
    return function_body(code, name)


def check(f):
    bad = []
    ino, net = f["WiPhone.ino"], f["Networks.cpp"]

    # ── F1: the HEALTH line's buffers live in a noinline helper ───────────────────────────────
    if not re.search(r"\bstatic\s+void\s+__attribute__\s*\(\(\s*noinline\s*\)\)\s+healthLineTick\s*\(", ino):
        bad.append("healthLineTick() must be `static void __attribute__((noinline))` - inlined, its "
                   "hl/kl buffers are back in loop()'s frame")
    lp = span(ino, "loop")
    hb = span(ino, "healthLineTick")
    if lp is None or hb is None:
        bad.append("loop() or healthLineTick() not found")
    else:
        a, z = lp
        for m in re.finditer(r"\b(?:char|uint8_t|int8_t|byte)\s+(\w+)\s*\[\s*(\d+)\s*\]", ino[a:z]):
            if int(m.group(2)) >= BIG:
                bad.append(f"loop() declares {m.group(1)}[{m.group(2)}] - a buffer that size reserves "
                           "its bytes in loop()'s frame under every call (the picture-page chain); "
                           "build it in a noinline helper like healthLineTick()")
        if not re.search(r"\bhealthLineTick\s*\(", ino[a:z]):
            bad.append("loop() no longer calls healthLineTick()")
        for what, pat in (("BATTERY_UPDATE_EVENT", r"\bBATTERY_UPDATE_EVENT\b"),
                          ("the low-battery powerOff()", r"\bpowerOff\s*\(")):
            if not re.search(pat, ino[a:z]):
                bad.append(f"loop() must keep {what} itself")
        h0, h1 = hb
        for what, pat in (("processEvent()", r"\bprocessEvent\s*\("), ("powerOff()", r"\bpowerOff\s*\("),
                          ("the power-off saves", r"\b(?:booksSaveOpenPosition|mapsSaveOpenView|gbcSaveForPowerOff)\s*\(")):
            if re.search(pat, ino[h0:h1]):
                bad.append(f"healthLineTick() calls {what} - app redraws and the saves would run on "
                           "top of hl and kl; keep them in loop()")

    # ── F3: every WiFi link line asks the card gate ───────────────────────────────────────────
    dd = span(net, "wifiDiagDurable")
    if dd is None:
        bad.append("wifiDiagDurable() not found")
    else:
        a, z = dd
        hl = [m.start() for m in re.finditer(r"\bhealthLogLine\s*\(", net[a:z])]
        gated = ifs(net, a, z, re.compile(r"\bcard\b"))
        if not hl or any(not any(g[1] <= a + p <= g[2] for g in gated) for p in hl):
            bad.append("wifiDiagDurable() must write the card only under `if (card ...)`")
    uses = list(re.finditer(r"\bwifiDiagDurable\s*\(\s*line\s*,\s*([^,]+),", net))
    if len(uses) < 3:
        bad.append(f"only {len(uses)} wifiDiagDurable() call(s) - the LOST, JOIN and radio-stopped "
                   "LOST lines all go through it")
    for u in uses:
        if not re.fullmatch(r"\s*s_card\s*\.\s*(?:lost|join)\s*\(\s*e\s*\.\s*ms\s*\)\s*", u.group(1)):
            bad.append(f"wifiDiagDurable(line, {u.group(1).strip()}, ...) - the card answer must come "
                       "from s_card.lost()/join() (one pair a minute)")
    dt = span(net, "Networks::diagTick")
    if dt is None:
        bad.append("Networks::diagTick() not found")
    else:
        a, z = dt
        if re.search(r"\bhealthLogLine\s*\(", net[a:z]):
            bad.append("diagTick() writes healthLogLine() directly - only through the card gate")
        early = [b for b in ifs(net, a, z, re.compile(r"\bhead\s*==\s*s_next\b"))
                 if re.search(r"\breturn\b", net[b[1]:b[2] + 1])]
        if not early or not re.search(r"summaryDue\s*\(", net[early[0][0]:early[0][1]]):
            bad.append("diagTick()'s early return must also wait for s_card.summaryDue() - else a "
                       "storm's end never reaches the card")
        sm = [b for b in ifs(net, a, z, re.compile(r"\bs_card\s*\.\s*summaryDue\s*\("))
              if re.search(r"\bwifiCardHeldLine\s*\(", net[b[1]:b[2] + 1])]
        if not sm:
            bad.append("diagTick() must write the held count (wifiCardHeldLine) when summaryDue()")

    # the MARK assoc/disassoc lines
    if re.search(r"\bheapEvent\s*\(\s*ws\s*==", ino):
        bad.append("WiPhone.ino: the status poll calls heapEvent() again - every status change to the "
                   "card, ungated; it goes through wifiMarkLink()")
    if not re.search(r"\bwifiMarkLink\s*\(\s*s_lastWifiStatus\s*,\s*ws\s*,", ino):
        bad.append("WiPhone.ino: the 2 s status poll must call wifiMarkLink(s_lastWifiStatus, ws, now)")
    mk = span(ino, "wifiMarkLink")
    if mk is None:
        bad.append("wifiMarkLink() not found")
    else:
        a, z = mk
        if not re.search(r"\bcard\s*=\s*edge\s*&&\s*\(\s*assoc\s*\?\s*s_gate\s*\.\s*join\s*\(\s*now\s*\)"
                         r"\s*:\s*s_gate\s*\.\s*lost\s*\(\s*now\s*\)\s*\)", ino[a:z]):
            bad.append("wifiMarkLink(): `card` must be `edge && (assoc ? s_gate.join(now) : s_gate.lost(now))`")
        hl = [m.start() for m in re.finditer(r"\bhealthLogLine\s*\(", ino[a:z])]
        gated = [g for g in ifs(ino, a, z, re.compile(r"\bcard\b"))
                 if re.fullmatch(r"\s*card\s*", ino[ino.index("(", g[0]) + 1:ino.rindex(")", g[0], g[1] + 1)])]
        if not hl or any(not any(g[1] <= a + p <= g[2] for g in gated) for p in hl):
            bad.append("wifiMarkLink() must write the card only under `if (card)`")
    return bad


MUTATIONS = [
    ("WiPhone.ino", "must be `static void __attribute__((noinline))`",
     r"static void __attribute__\(\(noinline\)\) healthLineTick\(", "static void healthLineTick("),
    ("WiPhone.ino", "loop() declares hl[256]",
     r"healthLineTick\(now, soc, v\);", "char hl[256]; healthLineTick(now, soc, v);"),
    ("WiPhone.ino", "loop() must keep BATTERY_UPDATE_EVENT",
     r"redrawWhat \|= gui\.processEvent\(now, BATTERY_UPDATE_EVENT\);", "(void)0;"),
    ("WiPhone.ino", "healthLineTick() calls processEvent()",
     r"  log_e\(\"%s\", hl\);\n", "  log_e(\"%s\", hl);\n  gui.processEvent(now, BATTERY_UPDATE_EVENT);\n"),
    ("Networks.cpp", "the card answer must come from s_card",
     r"wifiDiagDurable\(line, s_card\.lost\(e\.ms\), e\.ms\);", "wifiDiagDurable(line, true, e.ms);"),
    ("Networks.cpp", "the card answer must come from s_card",
     r"wifiDiagDurable\(line, s_card\.join\(e\.ms\), e\.ms\);", "wifiDiagDurable(line, true, e.ms);"),
    ("Networks.cpp", "wifiDiagDurable() must write the card only under",
     r"if \(card && !gGbcActive\) \{\n    healthLogLine\(line\);", "if (!gGbcActive) {\n    healthLogLine(line);"),
    ("Networks.cpp", "early return must also wait",
     r" &&\n      !s_card\.summaryDue\(now\)\) \{", ") {"),
    ("Networks.cpp", "held count (wifiCardHeldLine) when summaryDue",
     r"wifiCardHeldLine\(now, s_linkView \? \"UP\" : \"down\"\);", "(void)0;"),
    ("WiPhone.ino", "the status poll calls heapEvent() again",
     r"wifiMarkLink\(s_lastWifiStatus, ws, now\);", "heapEvent(ws == WL_CONNECTED ? \"assoc\" : \"disassoc\");"),
    ("WiPhone.ino", "wifiMarkLink(): `card` must be",
     r"const bool card = edge && \(assoc \? s_gate\.join\(now\) : s_gate\.lost\(now\)\);", "const bool card = edge;"),
    ("WiPhone.ino", "wifiMarkLink() must write the card only under",
     r"  if \(card\) \{\n    healthLogLine\(line\);\n  \}\n\}", "  healthLogLine(line);\n}"),
]


def mutation_test(files):
    ok = True
    for mut in MUTATIONS:
        name, want, pat, repl = mut[:4]
        raw = files[name + ".raw"]
        ms = list(re.finditer(pat, raw))
        if not ms:
            print(f"  SELF-TEST FAILED: the '{want}' guard is no longer where this mutation looks "
                  f"in {name} - update MUTATIONS with the guard")
            ok = False
            continue
        m = ms[0]
        mutated = raw[:m.start()] + repl + raw[m.end():]
        got = check(dict(files, **{name: strip_code(mutated), name + ".raw": mutated}))
        if not any(want in g for g in got):
            print(f"  SELF-TEST FAILED: removing the guard from {name} did not trip '{want}' (got {got})")
            ok = False
    if ok:
        print(f"  ok  self-test: removing each of the {len(MUTATIONS)} guards from the real "
              "source trips its contract")
    return ok


def main():
    files = {}
    for name in FILES:
        raw = (ROOT / name).read_text(errors="replace")
        files[name] = strip_code(raw)
        files[name + ".raw"] = raw
    problems = check(files)
    for p in problems:
        print(f"  CONTRACT BROKEN: {p}")
    if problems:
        return 1
    if not mutation_test(files):
        return 1
    print("  ok  the HEALTH buffers stay out of loop()'s frame (noinline helper, the redraw/power-off "
          "calls kept in loop), and every WIFI LOST/JOIN and MARK assoc/disassoc card write asks the gate")
    return 0


if __name__ == "__main__":
    sys.exit(main())
