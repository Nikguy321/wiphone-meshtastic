#!/usr/bin/env python3
"""Emulate SillyTavern key matching (case-insensitive, whole-word) on sample chat lines and show which entries fire."""
import json, re, sys
book = json.load(open(sys.argv[1], encoding="utf-8"))["entries"].values()
samples = [
  "Samus, what was it like growing up with the Chozo on Zebes?",
  "Do you ever think about Adam? I heard he died saving you.",
  "I dread going back out there. Tell me about the X parasites and the SA-X.",
  "What happened on Viewros with Sylux? Were the marines okay?",
  "Is it true you're part Metroid now? What did Raven Beak want with you?",
  "Ridley killed your parents, right? How do you deal with seeing him again?",
  "Can you explain how the Morph Ball works and whether the Screw Attack hurts?",
  "What's the Galactic Federation's deal with breeding Metroids on the Bottle Ship?",
  "Let's take the gunship to Daiban and grab a drink. Any objections, Lady?",
  "I saw a Zoomer and a couple of Geemers in the cave. Also a Sheegoth.",
  "Phazon corrupted you during the fight with Dark Samus on Phaaze, didn't it?",
  "Just a normal evening. We talk about the weather, dinner, and my job at the station.",
  "Do you ever think about Adam? I heard he died saving you.",
  "What happened to the Luminoth after you saved Aether? Did U-Mos survive?",
  "Tell me about the Thoha and the Mawkin. Was Quiet Robe the last of them?",
  "I keep hearing marines talk about your reputation. Are you really a living legend?",
  "How does the Screw Attack compare to the Shinespark and the Speed Booster?",
  "Mother Brain built the Zebesians? Or did the Pirates come later?",
  "Let's get dinner and watch a movie tonight, I'm tired after work.",
  "Is Kraid really that big? What about Phantoon and the Wrecked Ship?",
  "What did Gray Voice and Old Bird teach you when you were a kid?",
  "Are you human? I mean, with all the Chozo and Metroid DNA.",
]
def fires(ent, text):
    def hit(k):
        return re.search(r"(?<![\w])" + re.escape(k) + r"(?![\w])", text, re.I) is not None
    prim = [k for k in ent["key"] if hit(k)]
    if ent.get("constant"): return ["<constant>"]
    if not prim: return []
    sec = ent.get("keysecondary") or []
    if sec:
        logic = ent.get("selectiveLogic", 0); s = [k for k in sec if hit(k)]
        ok = {0: bool(s), 3: len(s) == len(sec), 1: len(s) != len(sec), 2: not s}[logic]
        if not ok: return []
    return prim
tot = []
for text in samples:
    print("\n>>", text)
    n = 0; words = 0
    for ent in book:
        f = fires(ent, text)
        if f:
            n += 1; words += len(ent["content"].split()); print(f"   - {ent['comment']}  <- {f}")
    tot.append(words)
    print(f"   ({n} entries, ~{int(words*1.35)} tokens before budget cap)")
print(f"\nAverage ~{int(sum(tot)/len(tot)*1.35)} tokens per message, max ~{int(max(tot)*1.35)} (before recursion and the World Info budget cap)")
