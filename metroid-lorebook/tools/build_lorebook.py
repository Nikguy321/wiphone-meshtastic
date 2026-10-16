#!/usr/bin/env python3
"""Assemble domain entry files into a SillyTavern World Info JSON (+ readable Markdown + report)."""
import json, glob, os, re, sys, collections

ENTRIES_DIR = sys.argv[1]
OUT_DIR = sys.argv[2]
NAME = sys.argv[3] if len(sys.argv) > 3 else "Metroid Canon Lorebook"
os.makedirs(OUT_DIR, exist_ok=True)

# --- SillyTavern entry template (from public/scripts/world-info.js, release branch) ---
TEMPLATE = {
    "key": [], "keysecondary": [], "comment": "", "content": "",
    "constant": False, "vectorized": False, "selective": True, "selectiveLogic": 0,
    "addMemo": True, "order": 100, "position": 0, "disable": False, "ignoreBudget": False,
    "excludeRecursion": False, "preventRecursion": True,
    "matchPersonaDescription": False, "matchCharacterDescription": False,
    "matchCharacterPersonality": False, "matchCharacterDepthPrompt": False,
    "matchScenario": False, "matchCreatorNotes": False,
    "delayUntilRecursion": 0, "probability": 100, "useProbability": True,
    "depth": 4, "outletName": "", "group": "", "groupOverride": False, "groupWeight": 100,
    "scanDepth": None, "caseSensitive": None, "matchWholeWords": None, "useGroupScoring": None,
    "automationId": "", "role": 0, "sticky": None, "cooldown": None, "delay": None,
    "triggers": [], "displayIndex": 0,
}
LOGIC = {"AND_ANY": 0, "NOT_ALL": 1, "NOT_ANY": 2, "AND_ALL": 3}

FORBIDDEN_BARE = {w.lower() for w in """
Prime Beyond Echoes Hunters Force Mother Dark Light Queen Baby Suit Beam Ball Missile Missiles Station Ship Dread Fusion
friend friends relationship relationships rival rivals backstory past childhood family parents
Corruption Return Returns Zero Hunter Pirate Pirates Lady Princess Parasite Parasites Energy Power Ice Wave Charge Scan Space Jump
Counter Shift Memory Memories Archive Archives Soldier Soldiers Robot Robots Egg Eggs Nest Lab Research Colony Seed Seeds Worm Ghost Ghosts
Brain Bird Voice Trace Spire Deleter Nightmare Adam Anthony Ian Mission Planet Visor Tank Tanks Bomb Bombs Gravity
Other Super Metroid II Prime 2 Prime 3 Prime 4 Hatchling Commander Core Area Areas Sector Sectors Clone Clones Crisis Incident Tribe Tribes
Phase Flash Boost Spider Spring Slide Wall Speed Storm Diffusion Wide Long Plasma Nova Grapple Melee
""".split()}
# bare keys allowed only on a specific entry (matched by comment regex, case-insensitive)
RESTRICTED = {
    "samus": r"core profile", "samus aran": r"core profile", "aran": r"core profile", "the hunter": r"core profile", "hunter": r"core profile",
    "metroid": r"^Metroids: .*overview", "metroids": r"^Metroids: .*overview",
    "chozo": r"^Chozo: (The )?Chozo \(species", "federation": r"^Galactic Federation: Galactic Federation \(overview", "galactic federation": r"^Galactic Federation: Galactic Federation \(overview",
    "space pirate": r"^Space Pirates: Space Pirates \(overview", "space pirates": r"^Space Pirates: Space Pirates \(overview",
    "ridley": r"^Ridley: Ridley \(overview",
    "adam": r"^(Other M: Adam Malkovich|Other M: The Adam AI|Dread: ADAM on ZDR)",
}
ALLOWED_CONSTANT = [r"primer", r"portraying samus"]

def norm_keys(keys):
    out, seen = [], set()
    for k in keys or []:
        k = re.sub(r"\s+", " ", str(k)).strip().strip(",")
        if not k: continue
        if k.lower() in seen: continue
        seen.add(k.lower()); out.append(k)
    return out

entries, warnings, sources = [], [], {}
files = sorted(glob.glob(os.path.join(ENTRIES_DIR, "*.json")))
done_domains = set()
for f in files:
    try:
        data = json.load(open(f, encoding="utf-8"))
    except Exception as e:
        warnings.append(f"UNREADABLE {os.path.basename(f)}: {e}"); continue
    dom = data.get("domain") or os.path.splitext(os.path.basename(f))[0]
    done_domains.add(dom)
    cat = data.get("category") or ""
    for e in data.get("entries", []):
        e = dict(e); e["_domain"] = dom; e["_cat"] = e.get("category") or cat
        entries.append(e)

# Optional fallback: unverified first drafts for domains that have no final file yet.
DRAFT_DIR = os.environ.get("FALLBACK_DRAFTS")
fallback_domains = []
if DRAFT_DIR:
    for f in sorted(glob.glob(os.path.join(DRAFT_DIR, "*.json"))):
        dom = os.path.splitext(os.path.basename(f))[0]
        if dom in done_domains: continue
        try:
            res = json.load(open(f, encoding="utf-8")).get("result") or {}
        except Exception as e:
            warnings.append(f"UNREADABLE draft {dom}: {e}"); continue
        ents = res.get("entries") or []
        if not ents: continue
        fallback_domains.append(dom)
        for e in ents:
            e = dict(e); e["_domain"] = dom
            e["_cat"] = e.get("category") or (str(e.get("comment", "")).split(":")[0].strip())
            entries.append(e)

# --- normalize & policy ---
seen_titles = {}
final = []
for e in entries:
    title = re.sub(r"\s+", " ", str(e.get("comment", ""))).strip()
    content = str(e.get("content", "")).strip()
    if not title or not content:
        warnings.append(f"SKIP empty entry in {e['_domain']}: {title!r}"); continue
    if title.lower() in seen_titles:
        new = f"{title} ({e['_domain']})"; warnings.append(f"DUP TITLE {title!r} -> {new!r}"); title = new
    seen_titles[title.lower()] = True
    keys = norm_keys(e.get("keys"))
    sec = norm_keys(e.get("secondary_keys"))
    kept = []
    for k in keys:
        kl = k.lower()
        if kl in RESTRICTED:
            if re.search(RESTRICTED[kl], title, re.I): kept.append(k)
            else: warnings.append(f"KEY '{k}' removed from {title!r} (restricted to another entry)")
        elif kl in FORBIDDEN_BARE and not (sec and str(e.get("selective_logic") or "AND_ANY").upper() in ("AND_ANY", "AND_ALL")):
            warnings.append(f"KEY '{k}' removed from {title!r} (forbidden bare word)")
        else:
            kept.append(k)
    keys = kept
    constant = bool(e.get("constant"))
    if constant and not any(re.search(p, title, re.I) for p in ALLOWED_CONSTANT):
        warnings.append(f"CONSTANT cleared on {title!r}"); constant = False
    if not keys and not constant:
        warnings.append(f"NO KEYS on {title!r} after policy; entry would never fire"); 
    # meta-language check
    for bad in ["the player", "gameplay", "cutscene", "boss fight", "unlockable", "DLC", "amiibo"]:
        if re.search(r"\b" + re.escape(bad) + r"\b", content, re.I):
            warnings.append(f"META '{bad}' in {title!r}")
    if re.search(r"^\s*#", content, re.M) or "|---" in content or "**" in content:
        warnings.append(f"MARKDOWN formatting in {title!r}")
    words = len(content.split())
    if words > 600: warnings.append(f"LONG ({words} words) {title!r}")
    if words < 60: warnings.append(f"SHORT ({words} words) {title!r}")
    order = e.get("order") if isinstance(e.get("order"), int) else 100
    if constant: order = max(order, 200)
    logic = LOGIC.get(str(e.get("selective_logic") or "AND_ANY").upper(), 0)
    final.append({
        "title": title, "keys": keys, "sec": sec, "logic": logic, "content": content, "constant": constant,
        "order": order, "cat": e["_cat"], "domain": e["_domain"], "words": words,
        "sources": [s for s in (e.get("sources") or []) if isinstance(s, str)],
        "position": 1 if (constant and re.search("portraying samus", title, re.I)) else 0,
    })

# --- category ordering for display ---
CAT_ORDER = ["Universe", "Samus", "Equipment", "Chozo", "Metroids", "X Parasite", "Space Pirates", "Ridley", "Galactic Federation",
             "Manga", "Zebes", "Prime", "Prime Hunters", "Prime 2", "Prime 3", "Dark Samus", "Phazon", "Federation Force", "Prime 4",
             "SR388", "Super Metroid", "Other M", "Fusion", "Dread", "Technology", "Creatures"]
def cat_rank(c): return CAT_ORDER.index(c) if c in CAT_ORDER else len(CAT_ORDER)
final.sort(key=lambda x: (0 if x["constant"] else 1, cat_rank(x["cat"]), x["title"].lower()))

# --- build ST json ---
st = {"entries": {}}
for i, x in enumerate(final):
    ent = dict(TEMPLATE)
    ent.update({
        "uid": i, "key": x["keys"], "keysecondary": x["sec"], "comment": x["title"], "content": x["content"],
        "constant": x["constant"], "selective": True, "selectiveLogic": x["logic"], "order": x["order"],
        "position": x["position"], "displayIndex": i,
    })
    st["entries"][str(i)] = ent

json_path = os.path.join(OUT_DIR, NAME + ".json")
with open(json_path, "w", encoding="utf-8") as fh:
    json.dump(st, fh, ensure_ascii=False, indent=2)

# --- readable markdown ---
md = [f"# {NAME} — readable version\n", "Generated from the same data as the SillyTavern JSON. Keys are the trigger words. Secondary keys mean the entry fires only when a key AND at least one secondary key appear together.\n"]
cur = None
for x in final:
    if x["cat"] != cur:
        cur = x["cat"]; md.append(f"\n## {cur}\n")
    flags = " **[constant]**" if x["constant"] else ""
    md.append(f"\n### {x['title']}{flags}\n")
    md.append(f"*Keys:* {', '.join(x['keys']) or '(none)'}" + (f"  \n*Secondary (AND ANY):* {', '.join(x['sec'])}" if x["sec"] else "") + "\n")
    md.append("\n" + x["content"].strip() + "\n")
    if x["sources"]:
        md.append("\n<sub>Sources: " + " · ".join(x["sources"][:6]) + "</sub>\n")
with open(os.path.join(OUT_DIR, NAME + " (readable).md"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(md))

# --- report ---
keyuse = collections.defaultdict(list)
for x in final:
    for k in x["keys"]: keyuse[k.lower()].append(x["title"])
collisions = {k: v for k, v in keyuse.items() if len(v) > 2}
total_words = sum(x["words"] for x in final)
report = {
    "entries": len(final), "constant": sum(1 for x in final if x["constant"]), "files": len(files),
    "final_domains": sorted(done_domains), "draft_fallback_domains": fallback_domains,
    "total_words": total_words, "approx_tokens": int(total_words * 1.35),
    "per_category": dict(collections.Counter(x["cat"] for x in final)),
    "key_collisions_gt2": collisions, "warnings": warnings,
    "longest": sorted(((x["words"], x["title"]) for x in final), reverse=True)[:8],
    "shortest": sorted(((x["words"], x["title"]) for x in final))[:8],
}
with open(os.path.join(OUT_DIR, "build_report.json"), "w", encoding="utf-8") as fh:
    json.dump(report, fh, indent=2, ensure_ascii=False)
print(json.dumps({k: v for k, v in report.items() if k not in ("warnings", "key_collisions_gt2")}, indent=1, ensure_ascii=False))
print(f"\n{len(warnings)} warnings, {len(collisions)} keys used by >2 entries")
for w in warnings: print("  !", w)
for k, v in sorted(collisions.items(), key=lambda kv: -len(kv[1]))[:40]: print(f"  ~ '{k}' x{len(v)}: {v[:5]}")
print("wrote", json_path)
