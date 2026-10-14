# Metroid lorebook: work-in-progress recovery notes

This folder is a work in progress. It is checkpointed to the branch `ccr-ea1d0e45-3qrky4` automatically,
so nothing finished is lost if the session or usage limit cuts out.

## What is where

- `entries/<domain>.json` — FINAL entries for a lore domain (drafted, fact-checked, revised). These are what get assembled.
- `checkpoint/draft/<domain>.json` — first drafts (researched from Metroid Wiki, Wikipedia, web search).
- `checkpoint/verify/<domain>.json` — the adversarial fact-check report for that draft.
- `checkpoint/revise/<domain>.json` — the reviser's final output (same content as `entries/`, plus a changelog).
- `checkpoint/STATUS.txt` — which stages each domain has completed.
- `tools/workflow-shard.js` — the workflow script (30 domains; pass `args.slugs` to run a subset).
- `tools/build_lorebook.py` — assembles `entries/*.json` into the SillyTavern JSON:
  `python3 tools/build_lorebook.py entries <out-dir> "Metroid Canon Lorebook"`
- `tools/simulate.py` — checks which entries fire on sample chat lines.

## Domains (30)

samus-bio, samus-rp, suits, weapons, abilities, chozo, metroids, x-parasite, space-pirates, ridley, zebes,
super-metroid, federation, adam-other-m, sr388, fusion, dread, prime1, hunters, echoes, prime3, dark-samus,
phazon, federation-force, prime4, timeline, bestiary-2d, bestiary-prime, tech, manga

## Resuming after an interruption

1. A domain with a file in `entries/` is done.
2. A domain with only `draft` (+ maybe `verify`) needs the remaining stages. In the same session, re-run the workflow with
   `resumeFromRunId` (cached agents return instantly). In a new session, re-run `tools/workflow-shard.js` with
   `args.slugs` set to the unfinished domains, or have Claude run the verify/revise steps by hand from the checkpoint files.
3. After all 30 are done: a gap-finding pass, a cross-entry consistency pass, then `build_lorebook.py`, `simulate.py`,
   and the README (`tools/README_template.md`).
