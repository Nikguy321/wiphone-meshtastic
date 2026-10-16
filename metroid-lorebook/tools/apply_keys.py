#!/usr/bin/env python3
"""Apply 'KEYS: <entry title> += k1, k2' lines to entry files; only add keys the entry's content mentions."""
import json, glob, os, re, sys
ENT = sys.argv[1]; lines = open(sys.argv[2]).read().splitlines(); only = set(sys.argv[3].split(",")) if len(sys.argv) > 3 else None
files = {f: json.load(open(f, encoding="utf-8")) for f in glob.glob(os.path.join(ENT, "*.json"))}
def find(title):
    t = re.sub(r"\s*\(final file title:.*?\)", "", title).strip()
    m = re.search(r"\(final file title: (.*?)\)", title)
    cands = [t] + ([m.group(1).strip()] if m else [])
    for c in cands:
        for f, d in files.items():
            for e in d["entries"]:
                if e["comment"].strip().lower() == c.lower(): return f, e
    for c in cands:  # prefix match
        for f, d in files.items():
            for e in d["entries"]:
                if e["comment"].lower().startswith(c.lower()[:40]): return f, e
    return None, None
changed = set()
for ln in lines:
    if not ln.startswith("KEYS:"): continue
    title, _, ks = ln[5:].partition("+=")
    ks = re.sub(r"\(.*?\)", "", ks)
    f, e = find(title.strip())
    if not e: print("  ? no entry for", title.strip()); continue
    dom = os.path.basename(f)[:-5]
    if only and dom not in only: print(f"  - deferred ({dom}) {e['comment']}"); continue
    body = e["content"].lower()
    have = {k.lower() for k in e.get("keys") or []}
    added, skipped = [], []
    for k in [x.strip() for x in ks.split(",") if x.strip()]:
        stem = k.lower().rstrip("s") if len(k) > 4 else k.lower()
        if k.lower() in have: continue
        if stem in body or k.lower() in body:
            e.setdefault("keys", []).append(k); have.add(k.lower()); added.append(k)
        else: skipped.append(k)
    if added: changed.add(f)
    print(f"  {e['comment']}: +{added}" + (f"  (not in content, skipped: {skipped})" if skipped else ""))
for f in changed:
    json.dump(files[f], open(f, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
print(len(changed), "files updated")
