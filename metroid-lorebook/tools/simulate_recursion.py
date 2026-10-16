#!/usr/bin/env python3
"""Estimate how many entries SillyTavern's recursive scan would pull in (no budget cap)."""
import json, re, sys
book = list(json.load(open(sys.argv[1], encoding="utf-8"))["entries"].values())
def hit(k, text): return re.search(r"(?<![\w])" + re.escape(k) + r"(?![\w])", text, re.I) is not None
def fires(ent, text):
    if ent.get("constant"): return True
    if not any(hit(k, text) for k in ent["key"]): return False
    sec = ent.get("keysecondary") or []
    if not sec: return True
    s = [k for k in sec if hit(k, text)]
    return {0: bool(s), 3: len(s) == len(sec), 1: len(s) != len(sec), 2: not s}[ent.get("selectiveLogic", 0)]
samples = sys.argv[2:] or ["Samus, what was it like growing up with the Chozo on Zebes?", "What happened on Viewros with Sylux?", "Tell me about the X parasites and the SA-X."]
for text in samples:
    active = {i for i, e in enumerate(book) if fires(e, text)}
    levels = [len(active)]
    frontier = set(active)
    for depth in range(6):
        buf = " ".join(book[i]["content"] for i in frontier if not book[i].get("preventRecursion"))
        new = {i for i, e in enumerate(book) if i not in active and not e.get("excludeRecursion") and fires(e, buf)}
        if not new: break
        active |= new; frontier = new; levels.append(len(active))
    words = sum(len(book[i]["content"].split()) for i in active)
    print(f">> {text}\n   entries by recursion depth: {levels}  -> ~{int(words*1.35)} tokens uncapped")
