#!/usr/bin/env python3
"""Snapshot workflow progress into the repo: final entry files plus every completed draft/verify/revise result."""
import json, glob, os, shutil, sys
SCRATCH = sys.argv[1]; WF = sys.argv[2]; OUT = sys.argv[3]
os.makedirs(os.path.join(OUT, "entries"), exist_ok=True)
n_entries = 0
for f in glob.glob(os.path.join(SCRATCH, "entries", "*.json")):
    try: json.load(open(f))
    except Exception: continue          # skip half-written files
    dst = os.path.join(OUT, "entries", os.path.basename(f))
    if not os.path.exists(dst) or open(f, "rb").read() != open(dst, "rb").read():
        shutil.copyfile(f, dst)
    n_entries += 1
status = {}
for j in sorted(glob.glob(os.path.join(WF, "wf_*", "journal.jsonl"))):
    run = os.path.basename(os.path.dirname(j))
    labels = {}
    for line in open(j, encoding="utf-8"):
        try: ev = json.loads(line)
        except Exception: continue
        if ev.get("type") == "started": labels[ev["key"]] = ev.get("label", "unknown")
        elif ev.get("type") == "result" and ev.get("key") in labels:
            stage, _, slug = labels[ev["key"]].partition(":")
            d = os.path.join(OUT, "checkpoint", stage); os.makedirs(d, exist_ok=True)
            p = os.path.join(d, slug + ".json")
            txt = json.dumps({"run": run, "label": labels[ev["key"]], "result": ev.get("result")}, ensure_ascii=False, indent=1)
            if stage in ("draft", "verify") and os.path.exists(p):
                pass                    # first completed draft/verify wins; never overwrite the inputs of a revision
            elif not os.path.exists(p) or open(p, encoding="utf-8").read() != txt:
                open(p, "w", encoding="utf-8").write(txt)
            status.setdefault(slug, set()).add(stage)
lines = [f"{slug}: {', '.join(s for s in ('draft','verify','revise') if s in st)}" for slug, st in sorted(status.items())]
open(os.path.join(OUT, "checkpoint", "STATUS.txt"), "w").write("\n".join(lines) + "\n")
print(f"{n_entries} final entry files; {sum(len(v) for v in status.values())} stage results across {len(status)} domains")
