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

The music rebuild's review round (2026-09-25) added the device-borrowing contracts at the end of
CONTRACTS: the pop finished before a track or a game takes the device, the mic apps pause music,
setSampleRate() marks the ring's queue stale, music refuses a failed I2S install, and a card that
will not read stops the track keeping its place. Each was checked by reverting it in a scratch copy.

The integration review's SA-1/SA-2 (2026-09-25) added the ROUTE-AND-LEVEL contracts after those:
the call screen's UP/DOWN steps ONE level - the route in use - and stores only that key (it moved
all three, and the loudspeaker one is the ring's: two presses on a loud earpiece left every later
ring 12 dB quieter, across reboots); the ring and a call read the stored call levels after music
yields; a call chooses its own route where it takes the device (dialling and connect), from the
call screen's key, which the CallApp constructor resets and no longer overrides with the ring's
LOUDSPEAKER; and the Game Boy puts back the levels its F1/F2 moved, not just the route.
Its verification round pinned what that let slip: the stored KEY (levelField) is chosen like the
level and written after the clamp (an earpiece press stored under loudspeaker_vol passed), the
Loud Spkr key records the route it picks, and connect reads NO file (a 0.5-1.6 s SPIFFS open
there, RTP port shut, for levels the dial or the ring already applied) - `sequence_within` takes
a `forbid` pattern for that.
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
            if c.get("forbid"):
                m = re.compile(c["forbid"]).search(code, b[2], b[3])
                if m:
                    return f"line {line_of(code, m.start())}: {c['forbid_what']}"
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
CALL_ROUTE = r"\baudio\s*->\s*chooseSpeaker\s*\(\s*gui\s*\.\s*state\s*\.\s*callLoudspeaker\s*\)"
TAKES_DEVICE = r"\bcallTakesAudioDevice\s*\("
TAKES_DEVICE_READ = r"\bcallTakesAudioDevice\s*\([^;]*,\s*true\s*\)"      # dialling: stored levels read
TAKES_DEVICE_KEEP = r"\bcallTakesAudioDevice\s*\([^;]*,\s*false\s*\)"     # connect: no file read

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
    dict(file="music_player.cpp", fn="adoptStopPlace", kind="calls",
         pat=r"\bmusicTakeStopPlace\s*\(", what="musicTakeStopPlace() when something else stopped music",
         why="a mesh pop, the ring or a call cut the track and F1 restarted it at 0:00"),
    dict(file="music_player.cpp", fn="musicPlayerLoop", kind="calls",
         pat=r"\badoptStopPlace\s*\(", what="adoptStopPlace() when something else stopped music",
         why="a mesh pop, the ring or a call cut the track and F1 restarted it at 0:00"),
    dict(file="music_player.cpp", fn="musicPlayerTogglePause", kind="sequence",
         seq=[r"\badoptStopPlace\s*\(", r"\bmusicPlayerIsPlaying\s*\("],
         what="a stop not yet adopted is adopted before F1 decides",
         why="F1 in the pass a pop cut the track found neither playing nor paused: restart at 0:00"),
    # ── 0.9.79 review round: the pop, the ring's queue, the install, the microphone ──
    dict(file="music_player.cpp", fn="startTrack", kind="sequence",
         seq=[r"\bnotifyPopFinishFor\s*\(", r"\bplayMusic\s*\("],
         what="a pop in flight is finished BEFORE the track takes the device",
         why="F1 inside a pop's 300 ms: the pop's teardown stopped the new track and restore()d "
             "the pre-pop state over it"),
    dict(file="WiPhone.ino", fn="loop", kind="guarded",
         pat=r"\baudio\s*->\s*discardPreserved\s*\(",
         need=[POS(r"\baudio\s*->\s*musicPlaying\s*\(")],
         what="the pop teardown dropping its snapshot only when music has taken over",
         why="the teardown's ceasePlayback()+restore() pulled a track's ring out from under it"),
    dict(file="app_gbc.cpp", fn="GbcApp::startGame", kind="sequence",
         seq=[r"\bnotifyPopFinishNow\s*\(", r"\bmusicPlayerPause\s*\(",
              r"\baudio\s*->\s*setSampleRate\s*\(", r"\baudio\s*->\s*setMonoOutput\s*\("],
         what="the pop finished, THEN music paused, THEN the game's rate and format",
         why="the pop's restore() put the pre-pop 22.05 kHz mono back over the game: 50% speed"),
    dict(file="Audio.cpp", fn="Audio::setSampleRate", kind="calls",
         pat=r"\bi2sQueueStale\s*=\s*true\b", what="i2sQueueStale = true on a rate change",
         why="i2s_set_clk() restarts the DMA at buffer 0 under the old free queue; with the cache "
             "matching, a pop after a music session played on that scrambled ring, late and cut"),
    dict(file="Audio.cpp", fn="Audio::setSampleRate", kind="guarded",
         pat=r"\bi2s_set_sample_rates\s*\(",
         need=[POS(r"\bi2sInstalled\b"), POS(r"\bi2sRate\s*!=")],
         what="i2s_set_sample_rates() only on an installed driver and a CHANGED rate",
         why="an unchanged rate restarted the ring for nothing; no driver = a NULL dereference in IDF"),
    dict(file="Audio.cpp", fn="Audio::installI2S", kind="calls",
         pat=r"!\s*this\s*->\s*i2sQueueStale\b", what="the cache refusing to match a stale queue",
         why="the reinstall after setSampleRate() is what gives a pop, the ring and a call a clean ring"),
    dict(file="Audio.cpp", fn="Audio::playMusic", kind="refuses_before",
         need=[NEG(r"\bthis\s*->\s*i2sInstalled\b")], later=r"\bi2s_zero_dma_buffer\s*\(",
         what="playMusic() refusing when music's I2S install failed",
         why="IDF 3.3's i2s_zero_dma_buffer()/i2s_write() dereference a NULL driver: a panic"),
    dict(file="Audio.cpp", fn="Audio::ceasePlayback", kind="guarded", pat=r"\bi2s_zero_dma_buffer\s*\(",
         need=[POS(r"\bi2sInstalled\b")], what="ceasePlayback()'s i2s_zero_dma_buffer()",
         why="after a failed install there is no driver, and IDF would dereference NULL"),
    dict(file="Audio.cpp", fn="Audio::loop", kind="guarded", pat=r"\bfeed\s*->\s*pass\s*\(",
         need=[POS(r"\bi2sInstalled\b")], what="the music feed's pass",
         why="it writes I2S; no driver = a NULL dereference"),
    dict(file="Audio.cpp", fn="Audio::loop", kind="sequence",
         seq=[r"\bfeed\s*->\s*pass\s*\(", r"\bfeed\s*->\s*failed\s*\(", r"\bceasePlayback\s*\("],
         what="a card that will not read stops the track keeping its place",
         why="a read error was taken for the end of the file: the rest skipped, a pulled card "
             "walked the queue"),
    dict(file="Audio.cpp", fn="Audio::turnMicOn", kind="sequence",
         seq=[r"\bmusicPlaying\s*\(", r"\bceasePlayback\s*\(", r"\bconfigureI2S\s*\("],
         what="a playing track stopped (place kept) before the microphone's install",
         why="configureI2S() swapped music's 534 ms ring for 186 ms under a playing track"),
    dict(file="GUI.cpp", fn="MicTestApp::MicTestApp", kind="sequence",
         seq=[r"\bmusicPlayerPause\s*\(", r"\baudio\s*->\s*start\s*\("],
         what="music paused before the Mic test takes the device",
         why="start() restarts I2S and turnMicOn() swaps the ring under a playing track"),
    dict(file="GUI.cpp", fn="RecorderApp::RecorderApp", kind="sequence",
         seq=[r"\bmusicPlayerPause\s*\(", r"\baudio\s*->\s*start\s*\("],
         what="music paused before the Recorder takes the device",
         why="start() restarts I2S and turnMicOn() swaps the ring under a playing track"),
    dict(file="GUI.cpp", fn="LedMicApp::LedMicApp", kind="sequence",
         seq=[r"\bmusicPlayerPause\s*\(", r"\baudio\s*->\s*setSampleRate\s*\(",
              r"\baudio\s*->\s*start\s*\("],
         what="music paused before the LED mic app sets 16 kHz",
         why="the rate change played the track at 0.73x, then the ring was swapped under it"),
    # ── review SA-1 (2026-09-25): the call screen's keys step ONE level, the ring reads its own ──
    dict(file="GUI.cpp", fn="CallApp::processEvent", kind="sequence",
         seq=[r"\bonHeadphones\s*=\s*audio\s*->\s*getHeadphones\s*\(\s*\)",
              r"\bonLoudspeaker\s*=\s*!\s*onHeadphones\s*&&\s*audio\s*->\s*isLoudspeaker\s*\(\s*\)",
              r"\bint8_t\s*&\s*level\s*=\s*onHeadphones\s*\?\s*headphonesVol\s*:\s*\(\s*onLoudspeaker\s*\?"
              r"\s*loudspeakerVol\s*:\s*earpieceVol\s*\)",
              r"\blevelField\s*=\s*onHeadphones\s*\?\s*headphonesVolField\s*:\s*\(\s*onLoudspeaker\s*\?"
              r"\s*loudspeakerVolField\s*:\s*earpieceVolField\s*\)",
              r"\blevel\s*\+=", r"\baudio\s*->\s*setVolumes\s*\(",
              r"\bini\s*\[[^\]]*\]\s*\[\s*levelField\s*\]\s*=(?!=)\s*level\b"],
         what="UP/DOWN stepping the level of the route in use (headphones > loudspeaker > earpiece), "
              "and storing it under THAT route's key, after the driver's clamp",
         why="it moved all three and the loudspeaker level is the RING's: two presses on a loud "
             "earpiece left every later ring 12 dB quieter, across reboots"),
    dict(file="GUI.cpp", fn="CallApp::processEvent", kind="not_calls",
         pat=r"\b(?:earpiece|headphones|loudspeaker)Vol\s*\+=",
         what="a named level stepped (the three used to move together)",
         why="an earpiece press moved the ring's loudspeaker level"),
    dict(file="GUI.cpp", fn="CallApp::processEvent", kind="not_calls",
         pat=r"\]\s*\[\s*(?:earpiece|headphones|loudspeaker)VolField\s*\]\s*=(?!=)",
         what="a named level key stored (all three were written back on every press)",
         why="storing the levels that did not move wrote whatever the codec held over the ring's"),
    dict(file="WiPhone.ino", fn="startRingtone", kind="sequence",
         seq=[r"\bmusicPlayerYieldForCall\s*\(", r"\bapplyStoredCallVolumes\s*\(",
              r"\baudio\s*->\s*chooseSpeaker\s*\(\s*true\s*\)", r"\baudio\s*->\s*start\s*\(\s*\)"],
         what="the ring applying the stored call levels AFTER music yields, before audio->start()",
         why="the ring inherited the codec's levels: an in-call press, a game's F1/F2"),
    dict(file="WiPhone.ino", fn="applyStoredCallVolumes", kind="sequence",
         seq=[r"\bini\s*\.\s*load\s*\(", r"\bgetIntValueSafe\s*\(", r"\baudio\s*->\s*setVolumes\s*\("],
         what="the stored levels read from configs.ini and applied",
         why="an empty helper would leave the ring and a call inheriting again"),
    dict(file="app_gbc.cpp", fn="GbcApp::startGame", kind="sequence",
         seq=[r"\bmusicPlayerPause\s*\(",
              r"\baudio\s*->\s*getVolumes\s*\(\s*savedEar\s*,\s*savedHp\s*,\s*savedLoud\s*\)",
              r"\baudio\s*->\s*chooseSpeaker\s*\(\s*true\s*\)"],
         what="the phone's levels saved (after music gives them back) before the game takes the device",
         why="F1/F2 move all three levels; only the route went back"),
    dict(file="app_gbc.cpp", fn="GbcApp::~GbcApp", kind="guarded",
         pat=r"\baudio\s*->\s*setVolumes\s*\(\s*savedEar\s*,\s*savedHp\s*,\s*savedLoud\s*\)",
         need=[POS(r"\brouteSaved\b")], what="the game putting the saved levels back",
         why="a game turned down left the ring and the next call's earpiece quieter until a reboot"),
    # ── review SA-2 (2026-09-25): a call chooses its own route where it takes the device ──
    dict(file="WiPhone.ino", fn="callTakesAudioDevice", kind="sequence",
         seq=[r"\bmusicPlayerYieldForCall\s*\(", r"\bapplyStoredCallVolumes\s*\(", CALL_ROUTE],
         what="music yields, the stored levels, THEN the call's route (the call screen's key)",
         why="a caller's call played on the LOUDSPEAKER the CallApp constructor picked for the "
             "ring, or on music's saved route (the yield restores it) when a track had played"),
    dict(file="WiPhone.ino", fn="callTakesAudioDevice", kind="guarded",
         pat=r"\bapplyStoredCallVolumes\s*\(", need=[POS(r"\breadStoredLevels\b")],
         what="the stored-level read only when the caller asks for it",
         why="at connect the configs.ini open (0.5-1.6 s) stalls the loop with the RTP port shut"),
    dict(file="WiPhone.ino", fn="loop", kind="sequence_within", outer=CALL_SETUP,
         has=r"\bopenRtpConnection\s*\(",
         seq=[r"\bnotifyPopFinishFor\s*\(", TAKES_DEVICE_KEEP, r"\bopenRtpConnection\s*\("],
         forbid=r"\bapplyStoredCallVolumes\s*\(|\bCriticalFile\b|\bSPIFFS\b|" + TAKES_DEVICE_READ,
         forbid_what="connect reading configs.ini: a 0.5-1.6 s SPIFFS open with the RTP port still "
                     "shut loses the far end's first words, for levels applied at dial or at the ring",
         what="connect: the pop finished, THEN the call takes the device (no file read), THEN RTP",
         why="nothing chose a caller's route; the loop's own yield later in the pass would put "
             "music's route back over one chosen before it; and a configs.ini read here is a "
             "0.5-1.6 s stall with the RTP port still shut"),
    dict(file="WiPhone.ino", fn="loop", kind="sequence_within", outer=[POS(r"\bcalleeUriDyn\b")],
         has=r"\bsip\s*\.\s*startCall\s*\(",
         seq=[r"\bnotifyPopFinishFor\s*\(", TAKES_DEVICE_READ, r"\bsip\s*\.\s*startCall\s*\("],
         what="dialling: the pop finished, THEN the call takes the device and the stored levels, "
              "before the INVITE",
         why="before connect UP/DOWN stepped the level of a stale route - often the ring's; and "
             "dialling is the only place a CALLER's levels are read (connect no longer reads them)"),
    dict(file="GUI.cpp", fn="CallApp::processEvent", kind="sequence_within",
         outer=[POS(r"\bsipState\s*==\s*CallState\s*::\s*Call\b")],
         has=r"\bcallLoudspeaker\b",
         seq=[r"\bchooseSpeaker\s*\(\s*!\s*EARSPEAKER\s*\)",
              r"\bcontrolState\s*\.\s*callLoudspeaker\s*=(?!=)\s*true\b",
              r"\bchooseSpeaker\s*\(\s*EARSPEAKER\s*\)",
              r"\bcontrolState\s*\.\s*callLoudspeaker\s*=(?!=)\s*false\b"],
         what="the Loud Spkr / Ear Spkr key recording the route it chose",
         why="the key's next press and its label read it: moving the speaker without recording "
             "it made the next press a silent no-op and ran the labels backwards (the pre-SA-2 key)"),
    dict(file="GUI.cpp", fn="CallApp::CallApp", kind="not_calls",
         pat=r"\baudio\s*->\s*chooseSpeaker\s*\(",
         what="the CallApp constructor choosing a route",
         why="its LOUDSPEAKER (meant for the ring) was every caller's route; it runs before music "
             "yields, and it also runs for the No-SIP-URI popup"),
    dict(file="GUI.cpp", fn="CallApp::CallApp", kind="calls",
         pat=r"\bcontrolState\s*\.\s*callLoudspeaker\s*=\s*false\b",
         what="controlState.callLoudspeaker = false (every call starts on the earpiece)",
         why="the old global was never reset: after a call ended on speaker the next call's key "
             "labels ran backwards"),
]

BANNED = [
    (r"\bsetVolumes\s*\(\s*restore", "setVolumes(restore...) - the never-written globals (0/0/0 dB)"),
    (r"\brestore(?:Speaker|Headphones|Loudspeaker)Vol\b", "a restore*Vol global - never written"),
    (r"\brtpSilentCnt\b", "the boot-long rtpSilentCnt - silence is counted per call (rtp_watch.h)"),
    (r"\brtpSilentScan\b", "the boot-long rtpSilentScan - silence is counted per call (rtp_watch.h)"),
    (r"\bloudSpkr\b", "the file-scope loudSpkr - nothing reset it per call and the call's audio never "
                       "read it (ControlState::callLoudspeaker, SA-2)"),
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

    # ── SA-1: the call screen's UP/DOWN steps one level and stores one key ──
    step = C("CallApp::processEvent", "sequence")
    good = ("appEventResult CallApp::processEvent(EventType event) {\n"
            " if (event == WIPHONE_KEY_UP || event == WIPHONE_KEY_DOWN) {\n"
            "  int8_t earpieceVol, headphonesVol, loudspeakerVol;\n"
            "  audio->getVolumes(earpieceVol, headphonesVol, loudspeakerVol);\n"
            "  const bool onHeadphones = audio->getHeadphones();\n"
            "  const bool onLoudspeaker = !onHeadphones && audio->isLoudspeaker();\n"
            "  int8_t& level = onHeadphones ? headphonesVol : (onLoudspeaker ? loudspeakerVol : earpieceVol);\n"
            "  const char* levelField = onHeadphones ? headphonesVolField :\n"
            "                           (onLoudspeaker ? loudspeakerVolField : earpieceVolField);\n"
            "  level += (event == WIPHONE_KEY_UP) ? 6 : -6;\n"
            "  audio->setVolumes(earpieceVol, headphonesVol, loudspeakerVol);\n"
            "  audio->getVolumes(earpieceVol, headphonesVol, loudspeakerVol);\n"
            "  if (loaded) {\n   ini[\"audio\"][levelField] = level;\n   ini.store();\n  }\n }\n}\n")
    pre = (good.replace("  int8_t& level = onHeadphones ? headphonesVol : (onLoudspeaker ? loudspeakerVol : earpieceVol);\n", "")
               .replace("  level += (event == WIPHONE_KEY_UP) ? 6 : -6;\n",
                        "  int8_t d = event == WIPHONE_KEY_UP ? 6 : -6;\n  earpieceVol += d;\n"
                        "  headphonesVol += d;\n  loudspeakerVol += d;\n")
               .replace("   ini[\"audio\"][levelField] = level;\n",
                        "   ini[\"audio\"][earpieceVolField] = earpieceVol;\n"
                        "   ini[\"audio\"][headphonesVolField] = headphonesVol;\n"
                        "   ini[\"audio\"][loudspeakerVolField] = loudspeakerVol;\n"))
    expect(good, step, True, "UP/DOWN stepping the route's level holds")
    expect(pre, step, False, "the pre-SA-1 all-three step fails")
    expect(good.replace("!onHeadphones && audio->isLoudspeaker()", "!onHeadphones"), step, False,
           "the loudspeaker picked without asking the codec's route fails")
    expect(good.replace("(onLoudspeaker ? loudspeakerVolField : earpieceVolField)",
                        "(onLoudspeaker ? loudspeakerVolField : loudspeakerVolField)"), step, False,
           "an earpiece press STORED under the ring's loudspeaker key fails")
    expect(good.replace("headphonesVolField :\n", "loudspeakerVolField :\n"), step, False,
           "a headphones press stored under the loudspeaker key fails")
    expect(good.replace("   ini[\"audio\"][levelField] = level;\n", ""), step, False,
           "UP/DOWN that stores nothing fails")
    expect(good.replace("   ini[\"audio\"][levelField] = level;\n", "").replace(
           "  level += (event", "  if (loaded) { ini[\"audio\"][levelField] = level; }\n  level += (event"),
           step, False, "the key stored BEFORE the step and the driver's clamp fails")
    expect(good.replace("ini[\"audio\"][levelField] = level;", "ini[\"audio\"][levelField] == level;"),
           step, False, "a comparison is not the store")
    nstep = C("CallApp::processEvent", "not_calls", pat=r"\b(?:earpiece|headphones|loudspeaker)Vol\s*\+=")
    expect(good, nstep, True, "no named level stepped holds")
    expect(pre, nstep, False, "earpieceVol/headphonesVol/loudspeakerVol += d fails")
    expect(good.replace("  level += (event", "  loudspeakerVol += 6;\n  level += (event"), nstep, False,
           "the ring's level stepped beside the route's fails")
    nstore = next(c for c in CONTRACTS if c.get("fn") == "CallApp::processEvent" and c["kind"] == "not_calls"
                  and "VolField" in c["pat"])
    expect(good, nstore, True, "storing only the moved key holds")
    expect(good.replace("   ini[\"audio\"][levelField] = level;\n",
                        "   ini[\"audio\"][levelField] = level;\n"
                        "   ini[\"audio\"][loudspeakerVolField] = loudspeakerVol;\n"), nstore, False,
           "the loudspeaker key stored beside the moved one fails")
    expect(good.replace("ini[\"audio\"][levelField] = level;", "if (x[loudspeakerVolField] == 1) {}"),
           nstore, True, "a comparison is not a store")

    rs = C("startRingtone", "sequence", seq=[r"\bmusicPlayerYieldForCall\s*\(", r"\bapplyStoredCallVolumes\s*\(",
                                             r"\baudio\s*->\s*chooseSpeaker\s*\(\s*true\s*\)",
                                             r"\baudio\s*->\s*start\s*\(\s*\)"])
    good = ("void startRingtone() {\n notifyPopFinishFor(\"r\");\n musicPlayerYieldForCall();\n"
            " applyStoredCallVolumes(\"ring\");\n if (t) {\n  audio->chooseSpeaker(true);\n  audio->start();\n }\n}\n")
    expect(good, rs, True, "the ring applying the stored levels after the yield holds")
    expect(good.replace(" applyStoredCallVolumes(\"ring\");\n", ""), rs, False,
           "the ring inheriting the codec's levels fails")
    expect(good.replace(" musicPlayerYieldForCall();\n applyStoredCallVolumes(\"ring\");\n",
                        " applyStoredCallVolumes(\"ring\");\n musicPlayerYieldForCall();\n"), rs, False,
           "the stored levels applied BEFORE the yield (its stash lands on top) fails")
    ap = C("applyStoredCallVolumes", "sequence")
    good = ("static void applyStoredCallVolumes(const char* who) {\n CriticalFile ini(F);\n"
            " if (!((ini.load() || ini.restore()) && !ini.isEmpty())) { return; }\n"
            " ear = ini[\"audio\"].getIntValueSafe(\"speaker_vol\", ear);\n audio->setVolumes(ear, hp, loud);\n}\n")
    expect(good, ap, True, "the helper reading and applying holds")
    expect(good.replace(" audio->setVolumes(ear, hp, loud);\n", ""), ap, False, "a helper that applies nothing fails")

    gs = C("GbcApp::startGame", "sequence", seq=[r"\bmusicPlayerPause\s*\(",
           r"\baudio\s*->\s*getVolumes\s*\(\s*savedEar\s*,\s*savedHp\s*,\s*savedLoud\s*\)",
           r"\baudio\s*->\s*chooseSpeaker\s*\(\s*true\s*\)"])
    good = ("void GbcApp::startGame() {\n notifyPopFinishNow();\n musicPlayerPause();\n"
            " savedLoudspeaker = audio->isLoudspeaker();\n audio->getVolumes(savedEar, savedHp, savedLoud);\n"
            " routeSaved = true;\n audio->chooseSpeaker(true);\n}\n")
    expect(good, gs, True, "the game saving the levels holds")
    expect(good.replace(" audio->getVolumes(savedEar, savedHp, savedLoud);\n", ""), gs, False,
           "the game saving only the route fails")
    gd = C("GbcApp::~GbcApp", "guarded")
    good = ("GbcApp::~GbcApp() {\n if (soundOn && audio) {\n  audio->shutdown();\n }\n"
            " if (routeSaved && audio) {\n  audio->chooseSpeaker(savedLoudspeaker);\n"
            "  audio->setVolumes(savedEar, savedHp, savedLoud);\n  routeSaved = false;\n }\n}\n")
    expect(good, gd, True, "the game putting its levels back holds")
    expect(good.replace("  audio->setVolumes(savedEar, savedHp, savedLoud);\n", ""), gd, False,
           "the game putting back only the route (pre-SA-1) fails")

    # ── SA-2: a call takes its own route, after music yields, at dial and at connect ──
    tk = C("callTakesAudioDevice", "sequence")
    good = ("static void callTakesAudioDevice(const char* who, bool readStoredLevels) {\n"
            " musicPlayerYieldForCall();\n if (readStoredLevels) {\n  applyStoredCallVolumes(who);\n }\n"
            " audio->chooseSpeaker(gui.state.callLoudspeaker);\n}\n")
    tg = C("callTakesAudioDevice", "guarded")
    expect(good, tg, True, "the stored-level read on the caller's say-so holds")
    expect(good.replace(" if (readStoredLevels) {\n  applyStoredCallVolumes(who);\n }\n",
                        " applyStoredCallVolumes(who);\n"), tg, False,
           "the read on every take (connect included) fails")
    expect(good.replace("if (readStoredLevels)", "if (!readStoredLevels)"), tg, False,
           "the read on the wrong say-so fails")
    expect(good, tk, True, "the call taking the device in order holds")
    expect(good.replace(" audio->chooseSpeaker(gui.state.callLoudspeaker);\n", ""), tk, False,
           "a call that chooses no route (pre-SA-2) fails")
    expect(good.replace(" musicPlayerYieldForCall();\n", "").replace(
           " audio->chooseSpeaker(gui.state.callLoudspeaker);\n",
           " audio->chooseSpeaker(gui.state.callLoudspeaker);\n musicPlayerYieldForCall();\n"), tk, False,
           "the route chosen BEFORE the yield (music's route lands on top) fails")
    expect(good.replace("gui.state.callLoudspeaker", "true"), tk, False,
           "the call put on the loudspeaker regardless of its key fails")
    cn = next(c for c in CONTRACTS if c["kind"] == "sequence_within" and TAKES_DEVICE_KEEP in c["seq"]
              and c["has"].startswith(r"\bopenRtp"))
    good = ("void loop() {\n if (callEstablished) {\n  if ((uint32_t)rtpRemoteIP && rtpRemotePort && "
            "audioFormat != N) {\n   notifyPopFinishFor(\"c\");\n   callTakesAudioDevice(\"call\", false);\n"
            "   audio->openRtpConnection(p);\n   audio->sendRtpStreamFromMic(f, ip, rtpRemotePort);\n"
            "   audio->playRtpStream(f, p);\n  }\n }\n}\n")
    expect(good, cn, True, "connect taking the device before RTP holds")
    expect(good.replace("   callTakesAudioDevice(\"call\", false);\n", ""), cn, False,
           "connect with no route of its own (pre-SA-2) fails")
    expect(good.replace("   notifyPopFinishFor(\"c\");\n   callTakesAudioDevice(\"call\", false);\n",
                        "   callTakesAudioDevice(\"call\", false);\n   notifyPopFinishFor(\"c\");\n"), cn, False,
           "the device taken before the pop is finished (its restore() lands on top) fails")
    expect(good.replace("callTakesAudioDevice(\"call\", false)", "callTakesAudioDevice(\"call\", true)"),
           cn, False, "connect reading configs.ini through the helper (e87eeb0) fails")
    expect(good.replace("   audio->openRtpConnection(p);\n",
                        "   applyStoredCallVolumes(\"call\");\n   audio->openRtpConnection(p);\n"), cn, False,
           "connect reading configs.ini beside the helper fails")
    expect(good.replace("   audio->openRtpConnection(p);\n",
                        "   CriticalFile ini(Storage::ConfigsFile);\n   audio->openRtpConnection(p);\n"), cn, False,
           "connect opening a SPIFFS file of its own fails")
    dl = next(c for c in CONTRACTS if c["kind"] == "sequence_within" and TAKES_DEVICE_READ in c["seq"]
              and "startCall" in c["has"])
    good = ("void loop() {\n if (s == InvitingCallee) {\n  if (strchr(gui.state.calleeUriDyn, '@') != NULL) {\n"
            "   notifyPopFinishFor(\"d\");\n   callTakesAudioDevice(\"dial\", true);\n"
            "   sip.startCall(gui.state.calleeUriDyn, now);\n  }\n }\n}\n")
    expect(good, dl, True, "dialling taking the device holds")
    expect(good.replace("   callTakesAudioDevice(\"dial\", true);\n", ""), dl, False,
           "dialling on a stale route fails")
    expect(good.replace("callTakesAudioDevice(\"dial\", true)", "callTakesAudioDevice(\"dial\", false)"),
           dl, False, "dialling without the stored levels (a caller would never read them) fails")

    sel = next(c for c in CONTRACTS if c.get("fn") == "CallApp::processEvent" and
               c["kind"] == "sequence_within")
    good = ("appEventResult CallApp::processEvent(EventType event) {\n if (event == WIPHONE_KEY_SELECT) {\n"
            "  if (controlState.sipState == CallState::Call) {\n   if (controlState.callLoudspeaker == false){\n"
            "    footer->setButtons(\"Ear Spkr\", \"Hang up\");\n    audio->chooseSpeaker(!EARSPEAKER);\n"
            "    controlState.callLoudspeaker = true;\n   } else {\n"
            "    footer->setButtons(\"Loud Spkr\", \"Hang up\");\n    audio->chooseSpeaker(EARSPEAKER);\n"
            "    controlState.callLoudspeaker = false;\n   }\n  }\n }\n}\n")
    expect(good, sel, True, "the key recording its route holds")
    expect(good.replace("    controlState.callLoudspeaker = true;\n", ""), sel, False,
           "the key moving to the loudspeaker without recording it fails")
    expect(good.replace("    controlState.callLoudspeaker = false;\n   }", "   }"), sel, False,
           "the key moving to the earpiece without recording it fails")
    expect(good.replace("callLoudspeaker = true;", "callLoudspeaker = X;").replace(
           "callLoudspeaker = false;", "callLoudspeaker = true;").replace("callLoudspeaker = X;",
           "callLoudspeaker = false;"), sel, False, "the key recording the OTHER route fails")
    cc = C("CallApp::CallApp", "not_calls")
    cr = C("CallApp::CallApp", "calls")
    good = ("CallApp::CallApp(Audio* audio, LCD& lcd, ControlState& state, bool c, HeaderWidget* h, FooterWidget* f)\n"
            "  : WindowedApp(lcd, state, h, f), audio(audio), caller(c) {\n"
            " reasonHash = hash_murmur(s);\n controlState.callLoudspeaker = false;\n}\n")
    expect(good, cc, True, "the constructor choosing no route holds")
    expect(good.replace(" controlState.callLoudspeaker = false;\n",
                        " audio->chooseSpeaker(LOUDSPEAKER);\n controlState.callLoudspeaker = false;\n"), cc, False,
           "the constructor's LOUDSPEAKER (pre-SA-2) fails")
    expect(good, cr, True, "the per-call reset holds")
    expect(good.replace(" controlState.callLoudspeaker = false;\n", ""), cr, False,
           "a route key never reset per call (pre-SA-2) fails")

    for pat, what in BANNED:
        if not re.search(pat, strip_code("audio->setVolumes(restoreSpeakerVol, h, l); rtpSilentCnt++; "
                                         "rtpSilentScan = 0; loudSpkr = true;")):
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
