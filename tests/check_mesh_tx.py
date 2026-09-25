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
    drip) wait for an idle pipeline, and the want_response reply is OWED, not sent from RX.

Like tests/check_call_audio.py this states each contract POSITIVELY and a contract that cannot find
its function fails too. A self-test replays the pre-fix shapes first, so a contract that has
quietly stopped matching fails loudly.
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
    tp = function_body(svc, "MeshtasticService::txPump")
    if tp is None:
        bad.append("MeshtasticService::txPump() not found")
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
"""
OLD_SVC = """
void MeshtasticService::txPump() { meshPhy.startSend(a, b); }
bool MeshtasticService::loop() {
  saveDbStep();
  txPump();
  if (!meshPhy.healthCheck()) { x(); }
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
            "starts a frame outside txPump()", "replayPump() must", "WiPhone.ino: loopPhase"]
    missing = [w for w in want if not any(w in g for g in got)]
    if missing:
        for w in missing:
            print(f"  SELF-TEST FAILED: the pre-fix shape did not trip '{w}'")
        return False
    print(f"  ok  self-test: the pre-fix shapes break all {len(want)} contract kinds")
    return True


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
    print("  ok  no blocking transmit; health/poll/serviceTx guards, the pump's order and the "
          "background senders' idle gates all hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
