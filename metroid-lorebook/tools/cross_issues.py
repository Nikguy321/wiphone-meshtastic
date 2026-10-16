#!/usr/bin/env python3
"""Save cross-cluster issues and notes from a consistency workflow output file."""
import json, sys, os
d = json.load(open(sys.argv[1]))
os.makedirs("build/cross", exist_ok=True)
for c in d.get("result") or []:
    txt = f"CLUSTER {c['cluster']}\nEDITED {[e.split('/')[-1] for e in (c.get('edited') or [])]}\nFIXES {c.get('fixes')}\n\nCROSS-CLUSTER ISSUES:\n{c.get('cross_cluster_issues') or ''}\n\nNOTES:\n{c.get('notes') or ''}\n"
    open(f"build/cross/{c['cluster']}.txt", "w").write(txt)
    print(txt)
