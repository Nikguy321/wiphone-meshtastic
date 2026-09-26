#!/usr/bin/env python3
"""check_gbc_ring.py - a Game Boy game's exit puts the I2S ring back where the heap wants it (0.9.79).

After a game, free internal RAM came back but the LARGEST block did not: 63,716 -> 32,816 on phone 1,
64,152 -> 28,732 on phone 2, for the rest of the boot. The game's setMonoOutput() (re)installs the
I2S ring (8 x 4 KB of DMA RAM) with the emulator's stacks, VRAM and audio buffer (32 KB) already at
the bottom of the phone's big free block, and IDF 3.3's heap is best fit: what is left of that block
is then smaller than the hole the old ring leaves, so the ring lands right on top of the emulator's
blocks, and at quit they free BELOW it. Phone 1's hole after quit was exactly those four blocks
(32,768 + 4 x 12 B of heap poisoning); on phone 2 music's own install moving the ring away gave
64,316 back. See Audio::reseatI2S() in WiPhone/Audio.cpp.

The fix is an ORDER in ~GbcApp and two GUARDS in Audio::reseatI2S(), in files the host suite cannot
compile, so - like tests/check_call_audio.py, whose contract engine this uses - they are pinned here
POSITIVELY:
  G1  ~GbcApp: gbcReleaseEmulator(), THEN audio->shutdown(), THEN audio->reseatI2S(), THEN
      wifiRestoreStation(). Before the release the ring is placed over the same blocks again; before
      the shutdown the codec loses its clocks while powered; after WiFi, the station's allocations
      have already taken the holes the ring should go back into.
  G2  ~GbcApp: the reseat only when startGame set the ring up (`ringReseat`) - a visit to the picker
      must not reinstall anything.
  G3  startGame: `ringReseat = true` after the setMonoOutput() that (re)installs the ring.
  G4  reseatI2S() refuses while the device is ON (audioOn) or with no driver to move (!i2sInstalled).
  G5  reseatI2S() asks installI2S(false, true) - the DEFAULT ring, FRESH - and then stops it.
      A non-fresh install finds the game's ring matching and returns: nothing would move.
  G6  reseatI2S()'s i2s_stop() only on an installed driver (IDF 3.3 dereferences NULL otherwise).
  G7  i2s_driver_install()/i2s_driver_uninstall() are called NOWHERE but Audio::installI2S(): its
      cache (and so the reseat, which goes through it) is only true while that holds.
  G8  Audio::start() refuses with no driver (after one reinstall) BEFORE it powers the codec or calls
      i2s_start(): a failed install - the reseat's among them - leaves none, IDF 3.3's i2s_start()
      dereferences NULL, and playMusic() calls turnOn() before it installs its own ring.
  G9  Audio::shutdown()'s i2s_stop() only with a driver installed (the same NULL, for every teardown).
  G10 ~GbcApp's shutdown() asks `soundStarted` and NOT `soundOn`: the emu thread clears soundOn when
      I2S starves and leaves the device ON, and skipping the shutdown there made the reseat refuse.
  G11 startGame: `soundStarted = soundOn` right after `soundOn = audio->start()`.

MUTATIONS then breaks each one in the REAL source, one at a time, and requires its own contract to
report it (tests/check_wifi_restore.py's pattern): a guard rewritten so its mutation no longer
matches fails too, so the mutation gets updated with the guard.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_wifi_restore import function_body, strip_code  # noqa: E402
from check_call_audio import NEG, POS, contract_problem, if_blocks, line_of  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent / "WiPhone"

RESEAT = r"\baudio\s*->\s*reseatI2S\s*\("
THIS = r"(?:\bthis\s*->\s*)?"          # `this->x` and `x` alike; a `!` before `this` negates it

CONTRACTS = [
    dict(id="G1-quit-order", file="app_gbc.cpp", fn="GbcApp::~GbcApp", kind="sequence",
         seq=[r"\bgbcReleaseEmulator\s*\(", r"\baudio\s*->\s*shutdown\s*\(", RESEAT,
              r"\bwifiRestoreStation\s*\("],
         what="the emulator's RAM freed, THEN the device off, THEN the ring reinstalled, THEN WiFi",
         why="installed with the emulator's 32 KB still below it, the ring cut the phone's largest "
             "block in two for the rest of the boot (63,716 -> 32,816 on phone 1)"),
    dict(id="G2-quit-gated", file="app_gbc.cpp", fn="GbcApp::~GbcApp", kind="guarded",
         pat=RESEAT, need=[POS(r"\bringReseat\b")],
         what="the ring's reinstall at quit",
         why="only a game that set the ring up has one to move; a picker visit must cost nothing"),
    dict(id="G3-start-flag", file="app_gbc.cpp", fn="GbcApp::startGame", kind="sequence",
         seq=[r"\baudio\s*->\s*setMonoOutput\s*\(", r"\bringReseat\s*=(?!=)\s*true\b"],
         what="ringReseat = true once the game's setMonoOutput() has set the ring up",
         why="without it the exit never reinstalls the ring and the split comes back"),
    dict(id="G4-refuse-powered", file="Audio.cpp", fn="Audio::reseatI2S", kind="refuses_before",
         need=[POS(THIS + r"\baudioOn\b"), NEG(THIS + r"\bi2sInstalled\b")],
         later=r"\binstallI2S\s*\(",
         what="reseatI2S() must refuse a powered device and a missing driver before installing",
         why="a powered codec losing its I2S clocks clicks; the game's starved path leaves it on"),
    dict(id="G5-fresh-default", file="Audio.cpp", fn="Audio::reseatI2S", kind="sequence",
         seq=[r"\binstallI2S\s*\(\s*false\s*,\s*true\s*\)", r"\bi2s_stop\s*\("],
         what="installI2S(false, true) - the DEFAULT ring, FRESH - THEN i2s_stop()",
         why="a non-fresh install finds the game's ring matching and returns without moving it; "
             "install starts the DMA, and the ring it replaces was stopped by shutdown()"),
    dict(id="G6-stop-guarded", file="Audio.cpp", fn="Audio::reseatI2S", kind="guarded",
         pat=r"\bi2s_stop\s*\(", need=[POS(THIS + r"\bi2sInstalled\b")],
         what="reseatI2S()'s i2s_stop()",
         why="after a failed install there is no driver, and IDF 3.3's i2s_stop() dereferences NULL"),
    dict(id="G8-start-needs-driver", file="Audio.cpp", fn="Audio::start", kind="refuses_before",
         need=[NEG(THIS + r"\bi2sInstalled\b")],
         later=r"\bcodec\s*\.\s*powerUp\s*\(|\bi2s_start\s*\(",
         what="start() must refuse with no driver before it powers the codec or calls i2s_start()",
         why="a failed install leaves no driver and IDF 3.3's i2s_start() dereferences NULL: the "
             "next track after a failed reseat (playMusic() starts the device BEFORE its install) "
             "was a LoadProhibited panic"),
    dict(id="G9-shutdown-stop-guarded", file="Audio.cpp", fn="Audio::shutdown", kind="guarded",
         pat=r"\bi2s_stop\s*\(", need=[POS(THIS + r"\bi2sInstalled\b")],
         what="shutdown()'s i2s_stop()",
         why="every teardown runs it, and with no driver IDF 3.3's i2s_stop() dereferences NULL "
             "inside its critical section"),
    dict(id="G11-start-flag-sound", file="app_gbc.cpp", fn="GbcApp::startGame", kind="sequence",
         seq=[r"\bsoundOn\s*=(?!=)\s*audio\s*->\s*start\s*\(",
              r"\bsoundStarted\s*=(?!=)\s*soundOn\b"],
         what="soundStarted = soundOn right after soundOn = audio->start()",
         why="without it the quit never shuts the device down, starved or not"),
]

SHUTDOWN = r"\baudio\s*->\s*shutdown\s*\("
QUIT = ("app_gbc.cpp", "GbcApp::~GbcApp")


def quit_shutdown_problems(texts):
    """G10: every audio->shutdown() in ~GbcApp sits in an `if` that asks soundStarted and does not
    ask soundOn (the emu thread clears soundOn when I2S starves; the device is still ON then)."""
    code = texts.get(QUIT[0])
    span = function_body(code, QUIT[1]) if code is not None else None
    if span is None:
        return [f"G10-quit-shutdown-started: {QUIT[1]}() not found in WiPhone/{QUIT[0]} - if it was "
                f"renamed or moved, update this check"]
    bs, be = span
    calls = list(re.finditer(SHUTDOWN, code[bs:be]))
    if not calls:
        return [f"G10-quit-shutdown-started: WiPhone/{QUIT[0]}: {QUIT[1]}: no audio->shutdown() - "
                f"the game must turn the device off at quit"]
    out = []
    blocks = if_blocks(code, bs, be)
    for m in calls:
        pos = bs + m.start()
        conds = [code[c_s:c_e] for c_s, c_e, b_bs, b_be in blocks if b_bs < pos < b_be]
        ok = any(re.search(r"\bsoundStarted\b", c) for c in conds) and \
            not any(re.search(r"\bsoundOn\b", c) for c in conds)
        if not ok:
            out.append(f"G10-quit-shutdown-started: WiPhone/{QUIT[0]}:{line_of(code, pos)}: "
                       f"audio->shutdown() must sit in an `if` asking soundStarted and not soundOn - "
                       f"the starved path leaves the device ON with soundOn false, so the reseat "
                       f"refuses and the heap stays split")
    return out

DRIVER_RX = re.compile(r"\bi2s_driver_(?:un)?install\s*\(")
INSTALLER = ("Audio.cpp", "Audio::installI2S")


def installer_problems(texts):
    """G7: every i2s_driver_install/uninstall call (comments and strings are blanked) sits inside
    Audio::installI2S()."""
    out = []
    code = texts.get(INSTALLER[0])
    span = function_body(code, INSTALLER[1]) if code is not None else None
    if span is None:
        return [f"G7-one-installer: {INSTALLER[1]}() not found in WiPhone/{INSTALLER[0]} - if it "
                f"was renamed or moved, update this check"]
    for name in sorted(texts):
        for m in DRIVER_RX.finditer(texts[name]):
            if name == INSTALLER[0] and span[0] < m.start() < span[1]:
                continue
            out.append(f"G7-one-installer: WiPhone/{name}:{line_of(texts[name], m.start())}: "
                       f"{m.group(0).rstrip('( ')}() outside {INSTALLER[1]}() - installI2S()'s "
                       f"cache, and the Game Boy's reseat through it, go stale")
    return out


def check(texts):
    problems = []
    for c in CONTRACTS:
        code = texts.get(c["file"])
        if code is None:
            problems.append(f"{c['id']}: WiPhone/{c['file']}: file not found")
            continue
        p = contract_problem(code, c)
        if p:
            problems.append(f"{c['id']}: WiPhone/{c['file']}: {c['fn']}: {p}\n"
                            f"      (why it matters: {c['why']})")
    return problems + installer_problems(texts) + quit_shutdown_problems(texts)


# (file, the contract id that must be reported, pattern on the BLANKED source, replacement)
MUTATIONS = [
    ("app_gbc.cpp", "G1-quit-order", RESEAT + r"\s*\)", "audio->isOn()"),
    # the reseat put BEFORE the emulator's release: the ring lands over the same blocks again
    ("app_gbc.cpp", "G1-quit-order", r"(?<!void )\bgbcReleaseEmulator\s*\(\s*\)\s*;",
     "audio->reseatI2S(); gbcReleaseEmulator();"),
    # WiFi given back before the ring moved
    ("app_gbc.cpp", "G1-quit-order", r"(?<!void )\bgbcReleaseEmulator\s*\(\s*\)\s*;",
     "wifiRestoreStation(0); gbcReleaseEmulator();"),
    ("app_gbc.cpp", "G1-quit-order", r"\baudio\s*->\s*shutdown\s*\(\s*\)\s*;", ""),
    ("app_gbc.cpp", "G2-quit-gated", r"\bif\s*\(\s*ringReseat\s*&&\s*audio\s*\)", "if (audio)"),
    ("app_gbc.cpp", "G3-start-flag", r"\bringReseat\s*=\s*true\s*;", ""),
    ("Audio.cpp", "G4-refuse-powered",
     r"\bif\s*\(\s*this\s*->\s*audioOn\s*\|\|\s*!\s*this\s*->\s*i2sInstalled\s*\)",
     "if (!this->i2sInstalled)"),
    ("Audio.cpp", "G4-refuse-powered",
     r"\bif\s*\(\s*this\s*->\s*audioOn\s*\|\|\s*!\s*this\s*->\s*i2sInstalled\s*\)",
     "if (this->audioOn)"),
    ("Audio.cpp", "G5-fresh-default", r"\binstallI2S\s*\(\s*false\s*,\s*true\s*\)",
     "installI2S(false, false)"),
    ("Audio.cpp", "G5-fresh-default", r"\binstallI2S\s*\(\s*false\s*,\s*true\s*\)",
     "installI2S(true, true)"),
    ("Audio.cpp", "G6-stop-guarded",
     r"\bif\s*\(\s*this\s*->\s*i2sInstalled\s*\)\s*\{\s*i2s_stop\s*\(\s*i2s_num\s*\)\s*;\s*\}",
     "i2s_stop(i2s_num);"),
    # a second uninstall site: shutdown() uninstalling instead of stopping
    ("Audio.cpp", "G7-one-installer",
     r"\bif\s*\(\s*i2s_stop\s*\(\s*i2s_num\s*\)\s*!=\s*ESP_OK\s*\)",
     "if (i2s_driver_uninstall(i2s_num)!=ESP_OK)"),
    # start() without its no-driver guard, and with the guard's refusal gone (retry only)
    ("Audio.cpp", "G8-start-needs-driver",
     r"\bif\s*\(\s*!\s*this\s*->\s*i2sInstalled\s*\)\s*\{\s*this\s*->\s*configureI2S\s*\(\s*\)\s*;"
     r"\s*if\s*\(\s*!\s*this\s*->\s*i2sInstalled\s*\)\s*\{[^{}]*\}\s*\}", ""),
    ("Audio.cpp", "G8-start-needs-driver",
     r"(\bif\s*\(\s*!\s*this\s*->\s*i2sInstalled\s*\)\s*\{[^{}]*?)\breturn\s+false\s*;", r"\1"),
    ("Audio.cpp", "G9-shutdown-stop-guarded",
     r"\bif\s*\(\s*this\s*->\s*i2sInstalled\s*\)\s*\{\s*(if\s*\(\s*i2s_stop\s*\(\s*i2s_num\s*\)\s*!=\s*ESP_OK"
     r"\s*\)\s*\{\s*succ\s*=\s*false\s*;\s*\})\s*\}", r"\1"),
    # the quit back on soundOn (the starved path skips the shutdown again), or asking both
    ("app_gbc.cpp", "G10-quit-shutdown-started", r"\bif\s*\(\s*soundStarted\s*&&\s*audio\s*\)",
     "if (soundOn && audio)"),
    ("app_gbc.cpp", "G10-quit-shutdown-started", r"\bif\s*\(\s*soundStarted\s*&&\s*audio\s*\)",
     "if (soundStarted && soundOn && audio)"),
    ("app_gbc.cpp", "G11-start-flag-sound", r"\bsoundStarted\s*=\s*soundOn\s*;", ""),
]


def selftest():
    """The G7 scan on its own, on synthetic text: a call inside installI2S passes, one elsewhere is
    reported, one in a comment or a string is not a call."""
    ok = True
    good = ("void Audio::installI2S(bool m, bool f) {\n i2s_driver_uninstall(i2s_num);\n"
            " i2s_driver_install(i2s_num, &c, 0, NULL);\n}\n"
            "bool Audio::shutdown() {\n // i2s_driver_uninstall(i2s_num);\n"
            " log_e(\"i2s_driver_uninstall(\");\n i2s_stop(i2s_num);\n}\n")
    if installer_problems({"Audio.cpp": strip_code(good)}):
        print("  SELF-TEST FAIL: G7 reported a call inside installI2S(), a comment or a string")
        ok = False
    bad = good.replace(" i2s_stop(i2s_num);", " i2s_driver_uninstall(i2s_num);")
    if not installer_problems({"Audio.cpp": strip_code(bad)}):
        print("  SELF-TEST FAIL: G7 missed an uninstall in shutdown()")
        ok = False
    if not installer_problems({"Audio.cpp": strip_code(good),
                               "app_gbc.cpp": strip_code("void f() { i2s_driver_install(a, b, 0, 0); }\n")}):
        print("  SELF-TEST FAIL: G7 missed an install in another file")
        ok = False
    # G10: the quit's shutdown on soundStarted, never on soundOn (alone or alongside)
    quit = ("GbcApp::~GbcApp() {\n gbcReleaseEmulator();\n if (%s) {\n  audio->shutdown();\n }\n}\n")
    for cond, holds in (("soundStarted && audio", True), ("audio && soundStarted", True),
                        ("soundOn && audio", False), ("soundStarted && soundOn && audio", False),
                        ("audio", False)):
        if (not quit_shutdown_problems({"app_gbc.cpp": strip_code(quit % cond)})) != holds:
            print(f"  SELF-TEST FAIL: G10 on `{cond}` should {'hold' if holds else 'fail'}")
            ok = False
    if not quit_shutdown_problems({"app_gbc.cpp": strip_code(
            "GbcApp::~GbcApp() {\n if (soundStarted && audio) {\n  x();\n }\n audio->shutdown();\n}\n")}):
        print("  SELF-TEST FAIL: G10 missed an unguarded shutdown() after a guarded block")
        ok = False
    # G4's polarity: `this->audioOn` asserted, `!this->i2sInstalled` negated - not the other way
    g4 = next(c for c in CONTRACTS if c["id"] == "G4-refuse-powered")
    src = ("bool Audio::reseatI2S() {\n if (%s) {\n  return false;\n }\n"
           " this->installI2S(false, true);\n}\n")
    for cond, holds in (("this->audioOn || !this->i2sInstalled", True),
                        ("!this->audioOn || !this->i2sInstalled", False),
                        ("this->audioOn || this->i2sInstalled", False)):
        if (contract_problem(strip_code(src % cond), g4) is None) != holds:
            print(f"  SELF-TEST FAIL: G4 on `{cond}` should {'hold' if holds else 'fail'}")
            ok = False
    return ok


def mutation_test(texts):
    ok = True
    for name, want, pat, repl in MUTATIONS:
        mutated, n = re.subn(pat, repl, texts[name], count=1)
        if n != 1:
            print(f"  SELF-TEST FAILED: the '{want}' guard is no longer where its mutation looks in "
                  f"{name} - update MUTATIONS with the guard")
            ok = False
            continue
        if not any(g.startswith(want) for g in check(dict(texts, **{name: mutated}))):
            print(f"  SELF-TEST FAILED: /{pat}/ -> '{repl}' in {name} did not trip '{want}'")
            ok = False
    if ok:
        print(f"  ok  self-test: each of the {len(MUTATIONS)} mutations of the real source trips "
              "its own contract")
    return ok


def main():
    if not selftest():
        return 1
    files = sorted(list(ROOT.glob("*.cpp")) + list(ROOT.glob("*.h")) + list(ROOT.glob("*.ino")))
    texts = {f.name: strip_code(f.read_text(errors="replace")) for f in files}
    problems = check(texts)
    for p in problems:
        print(f"  CONTRACT BROKEN: {p}")
    if problems:
        return 1
    if not mutation_test(texts):
        return 1
    print(f"  ok  all {len(CONTRACTS)} ring contracts hold, the quit shuts the device down on "
          f"soundStarted, and I2S is installed nowhere but {INSTALLER[1]}() ({len(files)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
