#!/usr/bin/env python3
"""check_call_audio.py - where the call-audio guards of 0.9.79 must be (the SIP audit's leftovers).

Both phones register now (sip=1), so the 2026-08-15 audit's "unreachable" call-audio findings are
reachable, and 9213fc0 fixed them: F1/F2 cannot start a track over a call, END hangs up a call and
nothing else, the ring and the call get the call levels back from music, far-end RTP silence is
counted per call, an orphaned RTP session is shut down, and the mic-to-a-LAN-IP easter egg is
compiled out. Every one of those fixes is a GUARD or an ORDER in WiPhone.ino, Audio.cpp, GUI.cpp or
music_player.cpp - files the host suite cannot compile.

🛑 THE REVIEW OF 9213fc0 REVERTED SIX OF ITS FIXES IN A SCRATCH COPY AND THE SUITE STAYED GREEN:
newCall()'s rtpSilenceBegin() and flag reset, END's gui.inCall() gate, the musicLoaded gate,
startRingtone's pop-finish and yield, the loop-level yield, Settings > Audio's stash routing. Only
reverting rtp_watch.h itself was caught - tests/test_rtpwatch.cpp re-implements the WIRING in its
own struct, so it proves the header and nothing about the callers. And the newCall() wiring is
load-bearing: without it a call that ends with one silence strike outstanding makes the next call
end at connect with no BYE (test_rtpwatch reproduces that; this file pins the call that prevents it).

So, like tests/check_wifi_restore.py (whose comment/string blanking and brace matching this imports),
CONTRACTS below state POSITIVELY where each guard must be - "the `if` that sets HangUp on END also
asks gui.inCall()", "startRingtone finishes a pop, THEN yields music, THEN starts the audio" - and a
contract that cannot find its function fails too (a rename must update it, not retire it).
A guard's POLARITY is checked, not just its spelling: `!gui.inCall()` does not satisfy a contract
that needs `gui.inCall()`, and the other way round. A self-test runs first and replays the
pre-fix shapes, so a contract that has quietly stopped matching fails loudly.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_wifi_restore import IF_RX, function_body, match_close, strip_code  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent / "WiPhone"


# ── terms: a pattern that must appear in a condition, asserted or negated ──────────────────────

def POS(p):
    return (re.compile(p), False)


def NEG(p):
    return (re.compile(p), True)


def term_holds(text, term):
    """Does `text` contain the term's pattern with the right polarity? Negated = a `!` directly
    before it (whitespace aside). `x != f()` is not a negation of f()."""
    rx, negated = term
    for m in rx.finditer(text):
        j = m.start() - 1
        while j >= 0 and text[j].isspace():
            j -= 1
        is_neg = j >= 0 and text[j] == "!" and not text.startswith("!=", j)
        if is_neg == negated:
            return True
    return False


def term_name(term):
    rx, negated = term
    return ("!" if negated else "") + rx.pattern


def if_blocks(code, start, end):
    """Every `if (...)` in code[start:end] as (cond_start, cond_end, body_start, body_end).
    A braceless body runs to its `;`."""
    out = []
    for m in IF_RX.finditer(code, start, end):
        po = m.end() - 1
        pc = match_close(code, po)
        if pc < 0 or pc > end:
            continue
        k = pc + 1
        while k < end and code[k].isspace():
            k += 1
        be = match_close(code, k) if k < end and code[k] == "{" else code.find(";", k, end)
        if be >= 0:
            out.append((po + 1, pc, k, be))
    return out


def missing_terms(cond, need):
    return [term_name(t) for t in need if not term_holds(cond, t)]


def line_of(code, pos):
    return code.count("\n", 0, pos) + 1


# ── the preprocessor: which spans compile only with a macro defined ────────────────────────────

PP_RX = re.compile(r"^[ \t]*#[ \t]*(ifdef|ifndef|if|elif|else|endif)\b([^\n]*)$", re.M)


def pp_regions(code, macro):
    """Spans of `code` (comments already blanked) compiled ONLY when `macro` is defined: from
    `#ifdef MACRO` / `#if defined(MACRO)` to its #else, #elif or #endif."""
    stack, out = [], []
    ours_rx = re.compile(r"\s*(?:defined\s*\(\s*%s\s*\)|defined\s+%s)\s*$" % (macro, macro))
    for m in PP_RX.finditer(code):
        kw, rest = m.group(1), m.group(2)
        if kw in ("ifdef", "ifndef", "if"):
            ours = (kw == "ifdef" and re.match(r"\s*%s\s*$" % macro, rest) is not None) or \
                   (kw == "if" and ours_rx.match(rest) is not None)
            stack.append([ours, m.end()])
        elif kw in ("else", "elif"):
            if stack and stack[-1][0]:
                out.append((stack[-1][1], m.start()))
                stack[-1][0] = False
        elif kw == "endif" and stack:
            ours, begin = stack.pop()
            if ours:
                out.append((begin, m.start()))
    return out


def inside(pos, spans):
    return any(a <= pos < b for a, b in spans)


# ── the contracts ────────────────────────────────────────────────────────────────────────────

def contract_problem(code, c, raw=None):
    """None if contract `c` holds in `code` (blanked source; `raw` = the unblanked text), else
    what is wrong."""
    kind = c["kind"]
    if kind == "banned":                       # file-wide, no function
        m = re.search(c["pat"], code)
        return None if not m else f"line {line_of(code, m.start())}: {c['what']}"
    if kind == "egg":
        return egg_problem(code, raw, c)

    span = function_body(code, c["fn"])
    if span is None:
        return f"{c['fn']}() not found - if it was renamed or moved, update this contract"
    bs, be = span

    if kind == "calls":
        return None if re.search(c["pat"], code[bs:be]) else f"does not do {c['what']}"
    if kind == "not_calls":
        m = re.compile(c["pat"]).search(code, bs, be)
        return None if not m else f"line {line_of(code, m.start())}: {c['what']}"
    if kind == "sequence":
        return sequence_problem(code, bs, be, c["seq"], c["what"])
    if kind == "sequence_within":
        # Every `if` asking the outer terms whose body does c["has"] - so a second setup path
        # added later is held to the same order - and at least one must exist.
        has = re.compile(c["has"])
        blocks = [b for b in if_blocks(code, bs, be)
                  if not missing_terms(code[b[0]:b[1]], c["outer"]) and has.search(code, b[2], b[3])]
        if not blocks:
            return (f"no `if` asking {[term_name(t) for t in c['outer']]} that does /{c['has']}/ - "
                    f"{c['what']}")
        for b in blocks:
            p = sequence_problem(code, b[2], b[3], c["seq"], c["what"])
            if p:
                return f"line {line_of(code, b[0])}: {p}"
        return None
    if kind == "guarded":
        calls = list(re.compile(c["pat"]).finditer(code, bs, be))
        if not calls:
            return f"no {c['what']} found where it is required"
        blocks = if_blocks(code, bs, be)
        for m in calls:
            ok = any(b_bs < m.start() < b_be and not missing_terms(code[c_s:c_e], c["need"])
                     for c_s, c_e, b_bs, b_be in blocks)
            if not ok:
                return (f"line {line_of(code, m.start())}: {c['what']} outside an `if` asking "
                        f"{[term_name(t) for t in c['need']]}")
        return None
    if kind == "cond_needs":
        sel = re.compile(c["sel"])
        body = re.compile(c["body"]) if c.get("body") else None
        hits = [b for b in if_blocks(code, bs, be)
                if sel.search(code, b[0], b[1]) and (body is None or body.search(code, b[2], b[3] + 1))]
        if not hits:
            return f"the `if` for {c['what']} is gone"
        for c_s, c_e, _, _ in hits:
            cond = code[c_s:c_e]
            miss = missing_terms(cond, c["need"])
            if miss:
                return f"line {line_of(code, c_s)}: {c['what']}: the condition lacks {miss}"
            if c.get("before"):
                last = re.search(c["before"], cond)
                for t in c["need"]:
                    first = t[0].search(cond)
                    if last and first and first.start() > last.start():
                        return (f"line {line_of(code, c_s)}: {c['what']}: {term_name(t)} must come "
                                f"BEFORE /{c['before']}/ (short-circuit)")
        return None
    if kind == "assign_needs":
        ms = list(re.compile(r"\b%s\s*=(?!=)" % re.escape(c["name"])).finditer(code, bs, be))
        if not ms:
            return f"`{c['name']} =` not found - {c['what']}"
        for m in ms:
            semi = code.find(";", m.end(), be)
            miss = missing_terms(code[m.end():semi], c["need"])
            if miss:
                return f"line {line_of(code, m.start())}: `{c['name']}` lacks {miss} - {c['what']}"
        return None
    if kind == "refuses_before":
        later = re.compile(c["later"]).search(code, bs, be)
        if not later:
            return f"no /{c['later']}/ found"
        for c_s, c_e, b_bs, b_be in if_blocks(code, bs, be):
            if c_s > later.start() or missing_terms(code[c_s:c_e], c["need"]):
                continue
            blk = code[b_bs:b_be + 1]
            if not re.search(r"\breturn\b", blk):
                continue
            if c.get("forbid") and re.search(c["forbid"], blk):
                return (f"line {line_of(code, c_s)}: the refusal writes /{c['forbid']}/ - "
                        f"{c['what']}")
            return None
        return (f"no refusal (`if ({[term_name(t) for t in c['need']]}) ... return`) before "
                f"/{c['later']}/: {c['what']}")
    return f"unknown contract kind {kind}"


def sequence_problem(code, start, end, seq, what):
    last, prev = -1, None
    for p in seq:
        m = re.compile(p).search(code, start, end)
        if not m:
            return f"missing /{p}/ - {what}"
        if m.start() < last:
            return f"wrong order: /{p}/ comes before /{prev}/ - {what}"
        last, prev = m.start(), p
    return None


EGG_RX = re.compile(r'\bmemcmp\s*\([^;]*"202\*\*"')
SEND_RX = re.compile(r"\bsendRtpStreamFromMic\s*\(")
CALL_SETUP = [POS(r"\brtpRemotePort\b"), POS(r"\baudioFormat\b")]


def egg_problem(code, raw, c):
    """The **202## egg (the live mic to a hardcoded LAN address) compiles only under
    AUDIO_DEBUG_EGGS, and outside that region the mic is sent from ONE place: a call's audio
    setup. Checked on the raw text for the key string (blanking empties literals)."""
    regions = pp_regions(code, "AUDIO_DEBUG_EGGS")
    eggs = [m for m in EGG_RX.finditer(raw) if code.startswith("memcmp", m.start())]
    if not eggs:
        return "the **202## egg (memcmp ... \"202**\") is not found - if it was removed, drop this contract"
    for m in eggs:
        if not inside(m.start(), regions):
            return f"line {line_of(code, m.start())}: the **202## egg compiles without AUDIO_DEBUG_EGGS"
    span = function_body(code, "loop")
    blocks = if_blocks(code, span[0], span[1]) if span else []
    for m in SEND_RX.finditer(code):
        if inside(m.start(), regions):
            continue
        if not any(b_bs < m.start() < b_be and not missing_terms(code[c_s:c_e], CALL_SETUP)
                   for c_s, c_e, b_bs, b_be in blocks):
            return (f"line {line_of(code, m.start())}: sendRtpStreamFromMic() outside a call's audio "
                    f"setup and outside #ifdef AUDIO_DEBUG_EGGS - {c['what']}")
    return None


IN_CALL = r"\bgui\s*\.\s*inCall\s*\(\s*\)"
GBC = r"\bgGbcActive\b"
POP = r"\bmeshPopPlaying\b"
KEY_END = r"\bkeyPressed\s*==\s*WIPHONE_KEY_END\b"

CONTRACTS = [
    # ── Audio.cpp: the per-call silence clock is wired, not just written ──
    dict(file="Audio.cpp", fn="Audio::newCall", kind="calls", pat=r"\brtpSilenceBegin\s*\(",
         what="rtpSilenceBegin() (the clock starts at THIS call)",
         why="without it a strike from the last call ends the next one at connect, no BYE "
             "(test_rtpwatch: 'the per-call begin is load-bearing')"),
    dict(file="Audio.cpp", fn="Audio::newCall", kind="calls",
         pat=r"\brtpSilentPeriod\s*=\s*RTP_SILENT_OFF\b", what="rtpSilentPeriod = RTP_SILENT_OFF",
         why="a verdict about the last call's far end is not one about this call's"),
    dict(file="Audio.cpp", fn="Audio::loop", kind="calls", pat=r"\brtpSilenceHeard\s*\(",
         what="rtpSilenceHeard() on a packet", why="a packet must clear the strikes"),
    dict(file="Audio.cpp", fn="Audio::loop", kind="calls", pat=r"\brtpSilenceQuiet\s*\(",
         what="rtpSilenceQuiet() on a packet-less pass", why="the per-call silence rule"),
    # ── Audio.cpp: the device refuses music over a call on its own behalf ──
    dict(file="Audio.cpp", fn="Audio::playMusic", kind="refuses_before",
         need=[POS(r"\bPlayback::RtpStream\b|\brtpSessionArmed\s*\("),
               POS(r"\bmicrophoneStreamOut\b|\brtpSessionArmed\s*\(")],
         later=r"\bstopMusic\s*\(",
         what="playMusic() must refuse while an RTP session is armed",
         why="a track replaced RtpStream (caller silent) and reinstalled I2S under the mic"),
    # ── music_player.cpp: the refusal keeps the paused place ──
    dict(file="music_player.cpp", fn="startTrack", kind="refuses_before",
         need=[POS(r"\brtpSessionArmed\s*\(")], later=r"\baudio\s*->\s*stopMusic\s*\(",
         forbid=r"\bs_loaded\s*=|\bs_paused\s*=",
         what="startTrack() refuses an armed session WITHOUT unloading the track",
         why="playMusic()'s refusal went down the failure branch: s_loaded = -1, the paused place lost"),
    # ── WiPhone.ino: END ──
    dict(file="WiPhone.ino", fn="loop", kind="cond_needs", sel=KEY_END,
         body=r"\bsetSipState\s*\(\s*CallState::HangUp\s*\)",
         need=[POS(IN_CALL), NEG(GBC)], what="END -> HangUp",
         why="END from any screen stopped music, zeroed volumes, and stuck in HangUp with no WiFi"),
    dict(file="WiPhone.ino", fn="loop", kind="cond_needs", sel=KEY_END,
         body=r"\bsetSipState\s*\(\s*CallState::NotInited\s*\)",
         need=[POS(r"\bCallState::Error\b"), POS(r"\bhasSipAccount\s*\(\s*\)"), NEG(GBC)],
         what="END in CallState::Error -> NotInited (one SIP re-init)",
         why="nothing else leaves Error: a phone booted with no route to the proxy stayed "
             "unregistered until a reboot"),
    # ── WiPhone.ino: the music transport keys ──
    dict(file="WiPhone.ino", fn="loop", kind="assign_needs", name="musicLoaded",
         need=[NEG(IN_CALL), NEG(GBC)], what="F1/F3/F4 must not start or drive music in a call",
         why="F1 mid-call stole the call's audio"),
    dict(file="WiPhone.ino", fn="loop", kind="cond_needs",
         sel=r"\bmusicPlayerCurrent\s*\(\s*\)\s*>=\s*0", body=r"\bWIPHONE_KEY_MASK_F2\b",
         need=[NEG(IN_CALL), NEG(GBC)], what="the F2 Next/Previous tracker",
         why="Next/Previous start a track: the same theft as F1"),
    # ── WiPhone.ino: the ring takes the device in order ──
    dict(file="WiPhone.ino", fn="startRingtone", kind="sequence",
         seq=[r"\bnotifyPopFinishFor\s*\(", r"\bmusicPlayerYieldForCall\s*\(",
              r"\baudio\s*->\s*chooseSpeaker\s*\(\s*true\s*\)", r"\baudio\s*->\s*start\s*\(\s*\)"],
         what="pop finished, THEN music yields, THEN the loudspeaker, THEN audio->start()",
         why="the ring inherited the music level; a pop's restore() undid the yield"),
    # ── WiPhone.ino: a call's audio setup finishes a pop first ──
    dict(file="WiPhone.ino", fn="loop", kind="sequence_within", outer=CALL_SETUP,
         has=r"\bopenRtpConnection\s*\(",
         seq=[r"\bnotifyPopFinishFor\s*\(", r"\bopenRtpConnection\s*\(",
              r"\bsendRtpStreamFromMic\s*\(", r"\bplayRtpStream\s*\("],
         what="a pop still playing is finished BEFORE the call's RTP is set up",
         why="the pop's teardown restore()d the pre-pop rate and route over the call"),
    # ── WiPhone.ino: the loop-level yield ──
    dict(file="WiPhone.ino", fn="loop", kind="guarded", pat=r"\bmusicPlayerYieldForCall\s*\(",
         need=[POS(IN_CALL), NEG(POP)], what="the loop-level musicPlayerYieldForCall()",
         why="gui.inCall(), not the teardown states (HangUp stuck = music never plays); and not "
             "under a pop, whose restore() would put the music level back over the call's"),
    # ── WiPhone.ino: the hot-mic backstop ──
    dict(file="WiPhone.ino", fn="loop", kind="cond_needs", sel=r"\brtpOrphanCheck\s*\(",
         body=r"\baudio\s*->\s*shutdown\s*\(", before=r"\brtpOrphanCheck\s*\(",
         need=[NEG(r"\bmeshVibroActive\b"), NEG(r"\bgui\s*\.\s*state\s*\.\s*vibroOn\b"), NEG(POP)],
         what="the orphaned-RTP backstop's shutdown()",
         why="shutdown()'s codec power-down on the shared I2C bus can leave the motor running"),
    dict(file="WiPhone.ino", fn="audioBenchOrphan", kind="not_calls",
         pat=r"\bsendRtpStreamFromMic\s*\(|\bturnMicOn\s*\(",
         what="the `audio orphan` bench switches the microphone on",
         why="the bench is RECEIVE ONLY: nothing may be sent anywhere"),
    dict(file="WiPhone.ino", kind="egg",
         what="the only unconditional mic stream is a call's",
         why="**202## streamed the live mic to 192.168.1.15:5000 on a public firmware"),
    # ── GUI.cpp: Settings > Audio ──
    dict(file="GUI.cpp", fn="AudioConfigApp::processEvent", kind="guarded",
         pat=r"\baudio\s*->\s*setVolumes\s*\(", need=[NEG(r"\bmusicPlayerSetCallVolumes\s*\(")],
         what="Save's audio->setVolumes()",
         why="while music holds the codec the call levels go into its stash, not under the track"),
    dict(file="GUI.cpp", fn="AudioConfigApp::AudioConfigApp", kind="calls",
         pat=r"\bmusicPlayerCallVolumes\s*\(", what="musicPlayerCallVolumes() (seed = call levels)",
         why="seeding from the codec put the MUSIC level on the call sliders"),
    dict(file="GUI.cpp", fn="AudioConfigApp::AudioConfigApp", kind="not_calls",
         pat=r"\bini\s*\.\s*store\s*\(", what="opening Settings > Audio writes configs.ini",
         why="a failed load stored a three-key file: every other setting wiped (8b93e72's rule)"),
    # ── Audio.cpp: music's own I2S install is music's alone (0.9.79, music_feed.h) ──
    dict(file="Audio.cpp", fn="Audio::configureI2S", kind="calls",
         pat=r"\binstallI2S\s*\(\s*false\b", what="the DEFAULT install (installI2S(false, ...))",
         why="every setter reaches I2S through here; asking for music's TX-only ring would hand a "
             "call no microphone and the Game Boy the wrong pacing"),
    dict(file="Audio.cpp", fn="Audio::turnMicOn", kind="sequence",
         seq=[r"\bconfigureI2S\s*\(", r"\bturnOn\s*\("],
         what="configureI2S() before the mic starts",
         why="the mic-level meters call start()+turnMicOn() with no setter: after a track they "
             "would read a TX-only ring"),
    dict(file="Audio.cpp", fn="Audio::loop", kind="guarded", pat=r"\bi2s_read\s*\(",
         need=[POS(r"\bi2sRx\b")], what="the loop's microphone i2s_read()",
         why="microphoneOn is latched until shutdown(); on music's TX-only install every pass "
             "would fail with an IDF error line"),
    dict(file="Audio.cpp", fn="Audio::ceasePlayback", kind="sequence",
         seq=[r"\bcloseRing\s*\(", r"\bfeed\s*->\s*stop\s*\(", r"\bi2s_zero_dma_buffer\s*\("],
         what="closeRing() before the feed stops and the ring is zeroed",
         why="a half-filled driver buffer left behind comes back out of order at the next "
             "resume (test_musicfeed: 22 of 60 pauses without it)"),
    dict(file="Audio.cpp", fn="Audio::playMusic", kind="calls",
         pat=r"\bconfigureMusicI2S\s*\(\s*!\s*ringWasRunning\s*\)",
         what="configureMusicI2S(!ringWasRunning)",
         why="i2s_start() after the idle watchdog restarts the DMA at buffer 0 under a stale queue: "
             "the first half second would play scrambled unless the ring is installed fresh"),
    # ── music_player.cpp: the route and the place ──
    dict(file="music_player.cpp", fn="applyMusicVolume", kind="not_calls",
         pat=r"\bif\s*\(\s*!\s*audio\s*->\s*getHeadphones\s*\(",
         what="the loudspeaker flag set only when the jack is empty",
         why="a track started with headphones in then fell into the EARPIECE when they were pulled "
             "(the Game Boy's 2026-09-19 lesson); the reopen that hid it is gone"),
    dict(file="music_player.cpp", fn="musicPlayerLoop", kind="calls",
         pat=r"\bmusicTakeStopPlace\s*\(", what="musicTakeStopPlace() when something else stopped music",
         why="a mesh pop, the ring or a call cut the track and F1 restarted it at 0:00"),
]

BANNED = [
    (r"\bsetVolumes\s*\(\s*restore", "setVolumes(restore...) - the never-written globals (0/0/0 dB)"),
    (r"\brestore(?:Speaker|Headphones|Loudspeaker)Vol\b", "a restore*Vol global - never written"),
    (r"\brtpSilentCnt\b", "the boot-long rtpSilentCnt - silence is counted per call (rtp_watch.h)"),
    (r"\brtpSilentScan\b", "the boot-long rtpSilentScan - silence is counted per call (rtp_watch.h)"),
]


def check(texts, raws):
    problems = []
    for c in CONTRACTS:
        code = texts.get(c["file"])
        if code is None:
            problems.append(f"WiPhone/{c['file']}: file not found")
            continue
        p = contract_problem(code, c, raws.get(c["file"]))
        if p:
            problems.append(f"WiPhone/{c['file']}: {c.get('fn', '(file)')}: {p}\n"
                            f"      (why it matters: {c['why']})")
    for name, code in sorted(texts.items()):
        for pat, what in BANNED:
            m = re.search(pat, code)
            if m:
                problems.append(f"WiPhone/{name}:{line_of(code, m.start())}: {what}")
    return problems


def selftest():
    ok = True

    def expect(src, c, holds, label):
        nonlocal ok
        got = contract_problem(strip_code(src), c, src) is None
        if got != holds:
            print(f"  SELF-TEST FAIL: {label}")
            ok = False

    def C(fn, kind, **kw):
        return next(c for c in CONTRACTS if c.get("fn") == fn and c["kind"] == kind and
                    all(c.get(k) == v for k, v in kw.items()))

    # polarity
    if not term_holds("a && !gui.inCall()", NEG(IN_CALL)) or term_holds("a && !gui.inCall()", POS(IN_CALL)):
        print("  SELF-TEST FAIL: polarity of !gui.inCall()")
        ok = False
    if term_holds("x != gui.inCall()", NEG(IN_CALL)):
        print("  SELF-TEST FAIL: `!=` taken for a negation")
        ok = False

    end = next(c for c in CONTRACTS if c.get("body", "").endswith(r"HangUp\s*\)"))
    expect("void loop() {\n if (keyPressed == WIPHONE_KEY_END && !gGbcActive && gui.inCall()) {\n"
           "  gui.state.setSipState(CallState::HangUp);\n }\n}\n", end, True, "the gated END holds")
    expect("void loop() {\n if (keyPressed == WIPHONE_KEY_END && !gGbcActive) {\n"
           "  gui.state.setSipState(CallState::HangUp);\n }\n}\n", end, False,
           "the pre-0.9.79 END (every screen) fails")
    expect("void loop() {\n if (keyPressed == WIPHONE_KEY_END && !gGbcActive && !gui.inCall()) {\n"
           "  gui.state.setSipState(CallState::HangUp);\n }\n}\n", end, False,
           "END gated on NOT in a call fails")
    expect("void loop() {\n // if (keyPressed == WIPHONE_KEY_END && gui.inCall())\n"
           " if (keyPressed == WIPHONE_KEY_END) gui.state.setSipState(CallState::HangUp);\n}\n",
           end, False, "a guard that is only a comment fails")
    err = next(c for c in CONTRACTS if c.get("body", "").endswith(r"NotInited\s*\)"))
    expect("void loop() {\n if (keyPressed == WIPHONE_KEY_END && !gGbcActive && gui.inCall()) {\n"
           "  gui.state.setSipState(CallState::HangUp);\n } else if (keyPressed == WIPHONE_KEY_END && "
           "!gGbcActive &&\n gui.state.sipState == CallState::Error && gui.state.hasSipAccount()) {\n"
           "  gui.state.setSipState(CallState::NotInited);\n }\n}\n", err, True, "END in Error holds")
    expect("void loop() {\n if (keyPressed == WIPHONE_KEY_END && gui.inCall()) {\n"
           "  gui.state.setSipState(CallState::HangUp);\n }\n}\n", err, False,
           "no way out of Error fails")

    ml = C("loop", "assign_needs")
    expect("void loop() {\n const bool musicLoaded = !gGbcActive && !mapOwnsKeys && !gui.inCall() &&\n"
           "   musicPlayerCurrent() >= 0;\n}\n", ml, True, "musicLoaded gated holds")
    expect("void loop() {\n const bool musicLoaded = !gGbcActive && !mapOwnsKeys && "
           "musicPlayerCurrent() >= 0;\n}\n", ml, False, "the pre-fix musicLoaded fails")

    f2 = next(c for c in CONTRACTS if c.get("body") == r"\bWIPHONE_KEY_MASK_F2\b")
    expect("void loop() {\n if (!gGbcActive && !gui.inCall() && musicPlayerCurrent() >= 0) {\n"
           "  if (uiKeyDownOrBlip(WIPHONE_KEY_MASK_F2, x)) { a(); }\n }\n}\n", f2, True, "F2 gated holds")
    expect("void loop() {\n if (!gGbcActive && musicPlayerCurrent() >= 0) {\n"
           "  if (uiKeyDownOrBlip(WIPHONE_KEY_MASK_F2, x)) { a(); }\n }\n}\n", f2, False,
           "the pre-fix F2 tracker fails")

    rt = C("startRingtone", "sequence")
    good = ("void startRingtone() {\n notifyPopFinishFor(\"r\");\n musicPlayerYieldForCall();\n"
            " if (t) {\n  audio->chooseSpeaker(true);\n  audio->start();\n }\n}\n")
    expect(good, rt, True, "startRingtone in order holds")
    expect(good.replace("notifyPopFinishFor(\"r\");\n musicPlayerYieldForCall();",
                        "musicPlayerYieldForCall();\n notifyPopFinishFor(\"r\");"), rt, False,
           "yield BEFORE the pop's finish fails (its restore() undoes the call levels)")
    expect(good.replace(" musicPlayerYieldForCall();\n", ""), rt, False, "no yield fails")

    ly = C("loop", "guarded", pat=r"\bmusicPlayerYieldForCall\s*\(")
    expect("void loop() {\n if (gui.inCall() && !meshPopPlaying) {\n  musicPlayerYieldForCall();\n }\n}\n",
           ly, True, "the loop yield gated on the pop holds")
    expect("void loop() {\n if (gui.inCall()) {\n  musicPlayerYieldForCall();\n }\n}\n",
           ly, False, "the loop yield under an in-flight pop fails")
    expect("void loop() {\n if (sipCallActive() && !meshPopPlaying) {\n  musicPlayerYieldForCall();\n }\n}\n",
           ly, False, "the yield on the old teardown-inclusive predicate fails")

    bs = C("loop", "cond_needs", sel=r"\brtpOrphanCheck\s*\(")
    good = ("void loop() {\n if (audio && !meshVibroActive && !gui.state.vibroOn && !meshPopPlaying &&\n"
            "   rtpOrphanCheck(o, a, e, now)) {\n  audio->shutdown();\n }\n}\n")
    expect(good, bs, True, "the backstop held for the motor holds")
    expect("void loop() {\n if (audio && rtpOrphanCheck(o, a, e, now)) {\n  audio->shutdown();\n }\n}\n",
           bs, False, "the unguarded backstop fails")
    expect("void loop() {\n if (audio && rtpOrphanCheck(o, a, e, now) && !meshVibroActive && "
           "!gui.state.vibroOn && !meshPopPlaying) {\n  audio->shutdown();\n }\n}\n", bs, False,
           "motor tests AFTER the clock (it restarts on every motor pass) fail")

    cs = C("loop", "sequence_within")
    good = ("void loop() {\n if (callEstablished) {\n  if ((uint32_t)rtpRemoteIP && rtpRemotePort && "
            "audioFormat != N) {\n   notifyPopFinishFor(\"c\");\n   audio->openRtpConnection(p);\n"
            "   audio->sendRtpStreamFromMic(f, ip, rtpRemotePort);\n   audio->playRtpStream(f, p);\n  }\n }\n}\n")
    expect(good, cs, True, "call setup finishing the pop first holds")
    expect(good.replace("   notifyPopFinishFor(\"c\");\n", ""), cs, False, "call setup with no pop finish fails")

    pm = C("Audio::playMusic", "refuses_before")
    good = ("bool Audio::playMusic(fs::FS *fs, const char* p, bool s, uint32_t a) {\n"
            " if (this->playback == Playback::RtpStream || this->microphoneStreamOut) {\n"
            "  this->musicProblem = \"In a call\";\n  return false;\n }\n this->stopMusic();\n}\n")
    expect(good, pm, True, "playMusic's refusal holds")
    expect(good.replace("Playback::RtpStream || this->microphoneStreamOut", "false"), pm, False,
           "playMusic with no refusal fails")

    st = C("startTrack", "refuses_before")
    good = ("static bool startTrack(int idx, uint32_t startAt = 0) {\n if (!audio) { return false; }\n"
            " if (audio->rtpSessionArmed()) {\n  s_error = \"In a call\";\n  return false;\n }\n"
            " audio->stopMusic();\n if (!audio->playMusic(&SD, p, s, startAt)) {\n  s_loaded = -1;\n"
            "  return false;\n }\n}\n")
    expect(good, st, True, "startTrack's place-keeping refusal holds")
    expect(good.replace("  s_error = \"In a call\";\n", "  s_error = \"In a call\";\n  s_loaded = -1;\n"),
           st, False, "a refusal that unloads the track fails")
    expect(good.replace(" if (audio->rtpSessionArmed()) {\n  s_error = \"In a call\";\n  return false;\n }\n", ""),
           st, False, "no refusal (the failure branch loses the place) fails")

    nc = C("Audio::newCall", "calls", pat=r"\brtpSilenceBegin\s*\(")
    expect("void Audio::newCall() {\n rtpSilenceBegin(s_rtpSilence, millis());\n}\n", nc, True, "newCall begins")
    expect("void Audio::newCall() {\n // rtpSilenceBegin(s_rtpSilence, millis());\n}\n", nc, False,
           "a begin that is only a comment fails")

    sv = C("AudioConfigApp::processEvent", "guarded")
    good = ("appEventResult AudioConfigApp::processEvent(EventType e) {\n if (e == S) {\n  ini.store();\n"
            "  if (!musicPlayerSetCallVolumes(a, b, c)) {\n   audio->setVolumes(a, b, c);\n  }\n }\n}\n")
    expect(good, sv, True, "Save through the stash holds")
    expect("appEventResult AudioConfigApp::processEvent(EventType e) {\n if (e == S) {\n  ini.store();\n"
           "  audio->setVolumes(a, b, c);\n }\n}\n", sv, False, "Save straight to the codec fails")
    ns = C("AudioConfigApp::AudioConfigApp", "not_calls")
    expect("AudioConfigApp::AudioConfigApp(Audio* a, LCD& l)\n  : WindowedApp(l), ini(F) {\n"
           " if ((ini.load() || ini.restore()) && !ini.isEmpty()) { x(); } else {\n"
           "  ini.addSection(\"audio\");\n  ini.store();\n }\n}\n", ns, False,
           "the constructor storing a three-key file fails")

    egg = next(c for c in CONTRACTS if c["kind"] == "egg")
    good = ("void loop() {\n if (k) {\n#ifdef AUDIO_DEBUG_EGGS\n } else if (!memcmp(lastKeys + 2, \"202**\", 5)) {\n"
            "  audio->sendRtpStreamFromMic(G, IPAddress(192, 168, 1, 15), 5000);\n#endif // AUDIO_DEBUG_EGGS\n"
            " }\n if (x) {\n  if ((uint32_t)rtpRemoteIP && rtpRemotePort && audioFormat != N) {\n"
            "   audio->sendRtpStreamFromMic(f, ip, rtpRemotePort);\n  }\n }\n}\n")
    expect(good, egg, True, "the egg under AUDIO_DEBUG_EGGS holds")
    expect(good.replace("#ifdef AUDIO_DEBUG_EGGS\n", "").replace("#endif // AUDIO_DEBUG_EGGS\n", ""),
           egg, False, "the egg compiled in fails")
    expect(good.replace("#ifdef AUDIO_DEBUG_EGGS\n", "#ifdef AUDIO_DEBUG_EGGS\n#else\n"), egg, False,
           "the egg in the #else of AUDIO_DEBUG_EGGS fails")
    expect(good.replace("#ifdef AUDIO_DEBUG_EGGS", "#ifndef AUDIO_DEBUG_EGGS"), egg, False,
           "the egg under #ifndef fails")
    expect(good.replace("#ifdef AUDIO_DEBUG_EGGS\n } else if (!memcmp(lastKeys + 2, \"202**\", 5)) {\n"
                        "  audio->sendRtpStreamFromMic(G, IPAddress(192, 168, 1, 15), 5000);\n",
                        "#ifdef AUDIO_DEBUG_EGGS\n } else if (!memcmp(lastKeys + 2, \"202**\", 5)) {\n"
                        "  y();\n#endif\n  audio->sendRtpStreamFromMic(G, IPAddress(192, 168, 1, 15), 5000);\n"
                        "#ifdef AUDIO_DEBUG_EGGS\n"),
           egg, False, "a mic stream moved out of the egg region fails")

    for pat, what in BANNED:
        if not re.search(pat, strip_code("audio->setVolumes(restoreSpeakerVol, h, l); rtpSilentCnt++; "
                                         "rtpSilentScan = 0;")):
            print(f"  SELF-TEST FAIL: banned pattern matches nothing: {what}")
            ok = False
    return ok


def main():
    if not selftest():
        return 1
    files = sorted(list(ROOT.glob("*.cpp")) + list(ROOT.glob("*.h")) + list(ROOT.glob("*.ino")))
    raws = {f.name: f.read_text(errors="replace") for f in files}
    texts = {n: strip_code(t) for n, t in raws.items()}
    problems = check(texts, raws)
    for p in problems:
        print(f"  CONTRACT BROKEN: {p}")
    if problems:
        return 1
    print(f"  ok  all {len(CONTRACTS)} call-audio contracts hold, and none of the "
          f"{len(BANNED)} retired spellings is back ({len(files)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
