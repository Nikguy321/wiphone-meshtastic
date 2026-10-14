#!/usr/bin/env python3
"""Write a compact index of all entries (title | keys | first sentence) for critic/consistency agents."""
import json, glob, os, re, sys
src, out = sys.argv[1], sys.argv[2]
lines = []
for f in sorted(glob.glob(os.path.join(src, "*.json"))):
    d = json.load(open(f, encoding="utf-8"))
    dom = d.get("domain") or os.path.basename(f)[:-5]
    for e in d.get("entries", []):
        first = re.split(r"(?<=[.!?])\s", e.get("content", "").strip(), 1)[0][:220]
        lines.append(f"[{dom}] {e.get('comment')} | keys: {', '.join(e.get('keys') or [])}"
                     + (f" | secondary: {', '.join(e.get('secondary_keys') or [])}" if e.get('secondary_keys') else "")
                     + f" | {len(e.get('content','').split())}w | {first}")
open(out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
print(len(lines), "entries indexed ->", out)
