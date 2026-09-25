#!/usr/bin/env python3
"""check_mesh_tx.py - the LoRa radio never waits for the air on the loop task (0.9.79).

The 0.6-1.5 s 'mesh' LOOP STALL lines were LoRa transmits: MeshPhy::send() keyed the SX1276 and
busy-waited on TxDone for the frame's whole time on air, on the superloop task. The fix is a
queue (mesh_txq.h, host-tested by tests/test_txq.cpp) and a PHY that starts a frame and returns
(MeshPhy::startSend / serviceTx). test_txq proves the queue; it proves NOTHING about the callers,
and every guard that makes the fix safe is an ORDER or a GUARD in mesh_phy.cpp,
meshtastic_service.cpp or WiPhone.ino - files the host suite cannot compile:

  - no blocking send comes back (no MeshPhy::send, no loop polling TX_DONE, no meshPhy.send call);
  - healthCheck() answers true WITHOUT READING while a frame is on the air - without it a 5 s
    health tick inside a transmit (~13 % of them) declares the radio LOST and re-inits it mid-frame;
  - poll() keeps its hands off the IRQ flags mid-frame;
  - serviceTx() reads TxDone BEFORE the deadline (a Game Boy game comes back long after a frame
    finished whole - calling that a timeout fails a receipt for a message that went);
  - txPump() is the FIRST statement of loop(), ahead of the DB save, the health check and poll();
  - only txPump() starts a frame (one start a pass, by construction);
  - the background senders (periodic NodeInfo, the neighbour drip, the position beacon, the replay
    drip) wait for an idle pipeline, and the want_response reply is OWED, not sent from RX;
  - and the four guards that keep 'queued' HONEST (a review removed all four at once in a scratch
    copy and this file still passed): startSend() refuses on a latched RX_DONE before it touches
    the chip (keying TX clears the flags - the old send() lost a received packet that way);
    txPump()'s TIMEOUT path fails the own frame's receipt (else a message that never left stays
    'sent'); MESH RADIO LOST fails everything queued (txDropAll); and `power lora rx` never
    forces RX over a frame on the air (benchSleep's setModeRxContinuous only under !txActive).

Like tests/check_call_audio.py this states each contract POSITIVELY and a contract that cannot find
its function fails too. A self-test replays the pre-fix shapes first, so a contract that has
quietly stopped matching fails loudly - and then REMOVES each of those four guards from the real
sources, one at a time (the review's experiment), and requires its contract to trip.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_wifi_restore import function_body, ifs, match_close, strip_code  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent / "WiPhone"


def body(code, name):
    b = function_body(code, name)
    return None if b is None else code[b[0] + 1:b[1]]


def first(text, pat, start=0):
    m = re.compile(pat).search(text, start)
    return m.start() if m else -1


def if_cond(code, if_pos):
    """The condition text of the `if (...)` at if_pos."""
    po = code.index("(", if_pos)
    return code[po + 1:match_close(code, po)]


def else_block(code, block_end):
    """(start, end) of the `else { ... }` straight after the if-block ending at block_end, or None."""
    m = re.compile(r"\s*else\s*\{").match(code, block_end + 1)
    if not m:
        return None
    end = match_close(code, m.end() - 1)
    return None if end < 0 else (m.end() - 1, end)


# resolveAck() on the frame in flight with a NON-ZERO reason (0 is "delivered")
FAIL_IN_FLIGHT = re.compile(r"\bresolveAck\s*\(\s*s_txInFlight\s*\.\s*packetId\s*,\s*(?!0\s*\))[^)\s]")


def check(files):
    """files: {name: stripped code}. Returns a list of broken contracts (empty = all hold)."""
    bad = []
    phy = files.get("mesh_phy.cpp", "")
    phyh = files.get("mesh_phy.h", "")
    svc = files.get("meshtastic_service.cpp", "")
    ino = files.get("WiPhone.ino", "")
    # strip_code() blanks string literals, so the phase NAMES are read from the raw text - at a
    # position the stripped text shows is code (offsets are kept), not a comment.
    ino_raw = files.get("WiPhone.ino.raw", "")

    # ── the blocking send stays gone ──────────────────────────────────────────────────────────
    if function_body(phy, "MeshPhy::send") is not None or re.search(r"\bbool\s+send\s*\(", phyh):
        bad.append("MeshPhy::send() is back - a transmit must start and return (startSend)")
    if re.search(r"\bwhile\s*\([^;{]*IRQ_TX_DONE_MASK", phy):
        bad.append("mesh_phy.cpp loops on IRQ_TX_DONE_MASK - that is the busy-wait that stalled the loop")
    if re.search(r"\bmeshPhy\s*\.\s*send\s*\(", svc):
        bad.append("meshtastic_service.cpp calls meshPhy.send() - frames go through the queue")

    # ── healthCheck: true without reading while a frame is on the air ─────────────────────────
    hc = body(phy, "MeshPhy::healthCheck")
    if hc is None:
        bad.append("MeshPhy::healthCheck() not found")
    else:
        guard = [b for b in ifs(hc, 0, len(hc), re.compile(r"(?<![!\w])txActive\b"))
                 if re.search(r"return\s+true", hc[b[1]:b[2] + 1])]
        rd = first(hc, r"readReg\s*\(")
        if not guard or (rd >= 0 and guard[0][0] > rd):
            bad.append("healthCheck() must `if (txActive) return true;` BEFORE its first register "
                       "read - TX/STANDBY mid-frame is not a lost radio")
        # One garbled read is a bus glitch, not a dead radio (five of five 'health FAILED' lines,
        # 2026-09-23..25): the register reads sit inside a loop of at least 3 attempts, and
        # `ready = false` comes only after that loop.
        loop = re.search(r"for\s*\(\s*int\s+(\w+)\s*=\s*0\s*;\s*\1\s*<\s*(\d+)\s*;", hc)
        if not loop or int(loop.group(2)) < 3 or not (loop.start() < rd):
            bad.append("healthCheck() must read the registers inside a retry loop of >= 3 attempts "
                       "- one garbled read on the bit-bang is not a lost radio")
        else:
            lb = hc.index("{", loop.end())
            le = match_close(hc, lb)
            if le < 0 or first(hc, r"\bready\s*=\s*false", le) < 0 or \
                    first(hc[lb:le], r"\bready\s*=\s*false") >= 0:
                bad.append("healthCheck() must set `ready = false` only AFTER its retry loop "
                           "gives up, never inside it")

    # ── poll: hands off the IRQ flags mid-frame ───────────────────────────────────────────────
    pl = body(phy, "MeshPhy::poll")
    if pl is None:
        bad.append("MeshPhy::poll() not found")
    else:
        guard = [b for b in ifs(pl, 0, len(pl), re.compile(r"(?<![!\w])txActive\b"))
                 if re.search(r"return\s+false", pl[b[1]:b[2] + 1])]
        rd = first(pl, r"readReg\s*\(")
        if not guard or (rd >= 0 and guard[0][0] > rd):
            bad.append("poll() must return false while txActive, before it reads the IRQ flags")

    # ── serviceTx: TxDone before the deadline ─────────────────────────────────────────────────
    st = body(phy, "MeshPhy::serviceTx")
    if st is None:
        bad.append("MeshPhy::serviceTx() not found")
    else:
        done = first(st, r"IRQ_TX_DONE_MASK")
        limit = first(st, r"txLimitMs")
        if done < 0 or limit < 0 or done > limit:
            bad.append("serviceTx() must test TxDone BEFORE the txLimitMs deadline")

    # ── startSend: a latched RX_DONE refuses before the chip is touched ───────────────────────
    ss = body(phy, "MeshPhy::startSend")
    if ss is None:
        bad.append("MeshPhy::startSend() not found")
    else:
        guard = [b for b in ifs(ss, 0, len(ss), re.compile(r"IRQ_RX_DONE_MASK"))
                 if re.search(r"readReg\s*\(\s*REG_IRQ_FLAGS\s*\)", if_cond(ss, b[0]))
                 and not re.match(r"\s*!", if_cond(ss, b[0]))
                 and re.search(r"return\s+false", ss[b[1]:b[2] + 1])]
        touch = first(ss, r"\b(?:writeReg|writeFifo|setMode\w*)\s*\(")
        key = first(ss, r"\bwriteReg\s*\(\s*REG_OP_MODE\b")
        if not guard or touch < 0 or key < 0 or guard[0][0] > touch:
            bad.append("startSend() must `if (readReg(REG_IRQ_FLAGS) & IRQ_RX_DONE_MASK) return false;` "
                       "BEFORE it writes the chip - keying TX clears the flags and loses the packet")

    # ── benchSleep: `power lora rx` never forces RX over a frame on the air ───────────────────
    bs = body(phy, "MeshPhy::benchSleep")
    if bs is None:
        bad.append("MeshPhy::benchSleep() not found")
    else:
        idle = [b for b in ifs(bs, 0, len(bs), re.compile(r"!\s*txActive\b"))]
        calls = [m.start() for m in re.finditer(r"\bsetModeRxContinuous\s*\(", bs)]
        if not calls or any(not any(b[1] <= c <= b[2] for b in idle) for c in calls):
            bad.append("benchSleep() must call setModeRxContinuous() only inside `if (!txActive)` - "
                       "mid-frame it truncates the frame on the air")

    # ── the pump: first in loop(), the only starter ───────────────────────────────────────────
    lp = body(svc, "MeshtasticService::loop")
    if lp is None:
        bad.append("MeshtasticService::loop() not found")
    else:
        if not re.match(r"\s*txPump\s*\(\s*\)\s*;", lp):
            bad.append("txPump(); must be the FIRST statement of MeshtasticService::loop()")
        pump = first(lp, r"\btxPump\s*\(")
        for pat, what in [(r"\bsaveDbStep\s*\(", "saveDbStep()"),
                          (r"meshPhy\s*\.\s*healthCheck\s*\(", "the health check"),
                          (r"meshPhy\s*\.\s*poll\s*\(", "poll()")]:
            at = first(lp, pat)
            if at < 0 or pump < 0 or pump > at:
                bad.append(f"txPump() must run before {what} in loop()")
        # background senders wait for an idle pipeline
        for cond, what in [(r"nextNodeInfoMs", "the periodic NodeInfo"),
                           (r"nbrDripMs", "the neighbour drip"),
                           (r"\bposDue\s*&&", "the position beacon"),
                           (r"\bnodeInfoOwed\b", "the owed NodeInfo reply")]:
            hits = ifs(lp, 0, len(lp), re.compile(cond))
            if not any(re.search(r"(?<![!\w])txPipelineIdle\s*\(", lp[h[0]:h[1]]) for h in hits):
                bad.append(f"{what} must wait for txPipelineIdle() in its own condition")
        # the want_response reply is owed, not transmitted from the RX path
        wr = ifs(lp, 0, len(lp), re.compile(r"(?<=\()\s*wantResp\s*$"))
        if not wr:
            bad.append("the `if (wantResp)` NodeInfo-reply branch not found in loop()")
        elif any(re.search(r"announceNodeInfo\s*\(", lp[h[1]:h[2]]) for h in wr) or \
                not any(re.search(r"nodeInfoOwed\s*=\s*true", lp[h[1]:h[2]]) for h in wr):
            bad.append("a want_response NodeInfo must set nodeInfoOwed, not announce from the RX path")
        # MESH RADIO LOST fails everything queued, with a reason (0 would say "delivered")
        lost = ifs(lp, 0, len(lp), re.compile(r"!\s*meshPhy\s*\.\s*healthCheck\s*\("))
        if not any(re.search(r"\btxDropAll\s*\(\s*(?!0\s*\))[^)\s]", lp[h[1]:h[2] + 1]) for h in lost):
            bad.append("the `if (!meshPhy.healthCheck())` (MESH RADIO LOST) branch must call "
                       "txDropAll(<non-zero reason>) - else queued messages sit there looking sent")
    tp = function_body(svc, "MeshtasticService::txPump")
    if tp is None:
        bad.append("MeshtasticService::txPump() not found")
    else:
        # the TIMEOUT path: the else of `if (... MESH_TX_DONE)`, or an `if` on MESH_TX_TIMEOUT alone
        tpb = svc[tp[0] + 1:tp[1]]
        paths = []
        for h in ifs(tpb, 0, len(tpb), re.compile(r"\bMESH_TX_(?:DONE|TIMEOUT)\b")):
            c = if_cond(tpb, h[0])
            has_done = re.search(r"\bMESH_TX_DONE\b", c)
            has_to = re.search(r"\bMESH_TX_TIMEOUT\b", c)
            if has_done and not has_to:
                e = else_block(tpb, h[2])
                if e:
                    paths.append(e)
            elif has_to and not has_done:
                paths.append((h[1], h[2]))
        own = [h for (a, z) in paths
               for h in ifs(tpb, a, z, re.compile(r"(?:==\s*MESH_TXK_OWN|MESH_TXK_OWN\s*==)"))
               if FAIL_IN_FLIGHT.search(tpb, h[1], h[2] + 1)]
        if not own:
            bad.append("txPump()'s TIMEOUT path must `if (s_txInFlight.kind == MESH_TXK_OWN) "
                       "resolveAck(s_txInFlight.packetId, <non-zero>)` - else a message that never "
                       "left stays 'sent'")
    for m in re.finditer(r"meshPhy\s*\.\s*startSend\s*\(", svc):
        if tp is None or not (tp[0] < m.start() < tp[1]):
            line = svc.count("\n", 0, m.start()) + 1
            bad.append(f"meshtastic_service.cpp:{line} starts a frame outside txPump()")
    rp = body(svc, "MeshtasticService::replayPump")
    if rp is None:
        bad.append("MeshtasticService::replayPump() not found")
    else:
        idle = [b for b in ifs(rp, 0, len(rp), re.compile(r"!\s*txPipelineIdle\s*\("))
                if re.search(r"\breturn\b", rp[b[1]:b[2] + 1])]
        tx = first(rp, r"meshTxText\s*\(")
        if not idle or tx < 0 or idle[0][0] > tx:
            bad.append("replayPump() must return while !txPipelineIdle(), before it queues a packet")

    # ── WiPhone.ino: the stall line can tell the radio from the screen ────────────────────────
    def phase(name):
        for m in re.finditer(r'loopPhase\s*\(\s*"' + re.escape(name) + r'"\s*\)', ino_raw):
            if ino[m.start():m.start() + 9] == "loopPhase":      # code, not a comment
                return m.start()
        return -1
    a = phase("mesh")
    b = first(ino, r"meshService\s*\.\s*loop\s*\(")
    c = phase("mesh-ui")
    d = first(ino, r"gui\s*\.\s*showMeshPopup\s*\(")
    if not (0 <= a < b < c < d):
        bad.append("WiPhone.ino: loopPhase(\"mesh\") -> meshService.loop() -> loopPhase(\"mesh-ui\") "
                   "-> the popup, in that order")
    return bad


# ── self-test: the pre-fix shapes must break the contracts ─────────────────────────────────────

OLD_PHY = """
bool MeshPhy::healthCheck() {
  if (!ready) { return false; }
  uint8_t ver = readReg(REG_VERSION);
  return true;
}
bool MeshPhy::poll(uint8_t* buf) {
  if (!ready || benchSleeping) { return false; }
  uint8_t irq = readReg(REG_IRQ_FLAGS);
  return false;
}
bool MeshPhy::send(const uint8_t* data, uint8_t len) {
  writeReg(REG_OP_MODE, MODE_TX);
  while (!(readReg(REG_IRQ_FLAGS) & IRQ_TX_DONE_MASK)) { delay(1); }
  return true;
}
MeshTxState MeshPhy::serviceTx() {
  if ((uint32_t)(now - txStartMs) > txLimitMs) { return MESH_TX_TIMEOUT; }
  if (readReg(REG_IRQ_FLAGS) & IRQ_TX_DONE_MASK) { return MESH_TX_DONE; }
  return MESH_TX_BUSY;
}
bool MeshPhy::startSend(const uint8_t* data, uint8_t len) {
  if (!ready || txActive) { return false; }
  setModeIdle();
  writeReg(REG_IRQ_FLAGS, 0xFF);
  writeReg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_TX);
  return true;
}
void MeshPhy::benchSleep(bool on) {
  if (on) { writeReg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_SLEEP); }
  else { benchSleeping = false; setModeRxContinuous(); }
}
"""
OLD_SVC = """
void MeshtasticService::txPump() {
  const MeshTxState st = meshPhy.serviceTx();
  if (st == MESH_TX_DONE || st == MESH_TX_TIMEOUT) {
    if (s_txInFlight.active) {
      s_txInFlight.active = false;
      if (st == MESH_TX_DONE) { s_txStats.own++; } else { s_txStats.timeout++; }
    }
  }
  meshPhy.startSend(a, b);
}
bool MeshtasticService::loop() {
  saveDbStep();
  txPump();
  if (!meshPhy.healthCheck()) { radioState = MESH_RADIO_ERROR; }
  if ((int32_t)(now - nextNodeInfoMs) >= 0) { announceNodeInfo(false); }
  if (nodeInfoOwed) { announceNodeInfo(false); }
  if (nbrPendingMask && (int32_t)(now - nbrDripMs) >= 0) { y(); }
  if (posDue && (uint32_t)(now - s_lastPhyTxMs) >= 1500u) { z(); }
  meshPhy.send(rebroadcast[i].data, rebroadcast[i].len);
  if (!meshPhy.poll(buf)) { return false; }
  if (wantResp) { announceNodeInfo(false); }
  return false;
}
void MeshtasticService::replayPump() {
  meshTxText(a, b, c, ch);
}
void elsewhere() { meshPhy.startSend(a, b); }
"""
OLD_INO = """
    loopPhase("mesh");
    if (!gGbcActive && meshService.loop()) {
      gui.showMeshPopup(title, nm->text);
    }
"""


def selftest():
    got = check({"mesh_phy.cpp": strip_code(OLD_PHY), "mesh_phy.h": "bool send(const uint8_t* d);",
                 "meshtastic_service.cpp": strip_code(OLD_SVC), "WiPhone.ino": strip_code(OLD_INO),
                 "WiPhone.ino.raw": OLD_INO})
    want = ["MeshPhy::send() is back", "loops on IRQ_TX_DONE_MASK", "calls meshPhy.send()",
            "healthCheck() must", "poll() must", "serviceTx() must", "must be the FIRST statement",
            "before saveDbStep()", "the periodic NodeInfo", "the neighbour drip",
            "the position beacon", "the owed NodeInfo reply", "want_response NodeInfo must",
            "starts a frame outside txPump()", "replayPump() must", "WiPhone.ino: loopPhase",
            "startSend() must", "benchSleep() must", "MESH RADIO LOST", "TIMEOUT path must",
            "retry loop of >= 3"]
    missing = [w for w in want if not any(w in g for g in got)]
    if missing:
        for w in missing:
            print(f"  SELF-TEST FAILED: the pre-fix shape did not trip '{w}'")
        return False
    print(f"  ok  self-test: the pre-fix shapes break all {len(want)} contract kinds")
    return True


# The review's experiment, kept: each of the four 'queued is honest' guards REMOVED from the real
# source (stripped text, as check() sees it) must trip its own contract. A pattern that no longer
# matches the source fails too - the guard was rewritten, so this mutation needs rewriting with it.
MUTATIONS = [
    ("mesh_phy.cpp", "startSend() must",
     r"if\s*\(\s*readReg\s*\(\s*REG_IRQ_FLAGS\s*\)\s*&\s*IRQ_RX_DONE_MASK\s*\)\s*\{\s*return\s+false\s*;\s*\}",
     ""),
    ("meshtastic_service.cpp", "TIMEOUT path must",
     r"resolveAck\s*\(\s*s_txInFlight\s*\.\s*packetId\s*,\s*3\s*\)\s*;", ""),
    ("meshtastic_service.cpp", "MESH RADIO LOST", r"txDropAll\s*\(\s*4\s*\)\s*;", ""),
    ("mesh_phy.cpp", "benchSleep() must",
     r"if\s*\(\s*!\s*txActive\s*\)\s*\{\s*(setModeRxContinuous\s*\(\s*\)\s*;)\s*\}", r"\1"),
    # the health check's retry loop cut down to one read (the pre-0.9.79 single-read shape)
    ("mesh_phy.cpp", "retry loop of >= 3",
     r"for\s*\(\s*int\s+attempt\s*=\s*0\s*;\s*attempt\s*<\s*3\s*;", "for (int attempt = 0; attempt < 1;"),
]


def mutation_test(files):
    ok = True
    for name, want, pat, repl in MUTATIONS:
        mutated, n = re.subn(pat, repl, files[name], count=1)
        if n != 1:
            print(f"  SELF-TEST FAILED: the '{want}' guard is no longer where this mutation looks "
                  f"in {name} - update MUTATIONS with the guard")
            ok = False
            continue
        got = check(dict(files, **{name: mutated}))
        if not any(want in g for g in got):
            print(f"  SELF-TEST FAILED: removing the guard from {name} did not trip '{want}'")
            ok = False
    if ok:
        print(f"  ok  self-test: removing each of the {len(MUTATIONS)} guards (queued-is-honest + the health re-read) "
              "from the real source trips its contract")
    return ok


def main():
    if not selftest():
        return 1
    files = {}
    for name in ("mesh_phy.cpp", "mesh_phy.h", "meshtastic_service.cpp", "WiPhone.ino"):
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
    print("  ok  no blocking transmit; health/poll/serviceTx guards, the pump's order, the "
          "background senders' idle gates and the four 'queued is honest' guards all hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
