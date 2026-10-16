# Metroid Canon Lorebook (SillyTavern World Info)

A SillyTavern World Info ("lorebook") covering the canon lore of the Metroid series, written for roleplay in which
{{user}} interacts with Samus Aran (Samus as the character card).

**Files**

- `Metroid Canon Lorebook.json` — the importable lorebook (__ENTRIES__ entries, ~__TOKENS__ tokens of lore in total; only matching entries are inserted per message).
- `Metroid Canon Lorebook (readable).md` — the same entries as readable text, with trigger keys and the sources each entry was checked against.
- `build_report.json` — build statistics and key-collision report.

## Canon coverage

Every mainline and Prime-series game in the official chronology plus the official manga backstory:
Zero Mission (Zebes) · Metroid Prime / Remastered (Tallon IV) · Prime Hunters (Alimbic Cluster) · Prime 2: Echoes (Aether) ·
Prime 3: Corruption (Phazon crisis) · Federation Force (Bermuda System) · Prime 4: Beyond (Viewros) · Samus Returns (SR388) ·
Super Metroid · Other M (Bottle Ship) · Fusion (BSL) · Dread (ZDR) · the 2002–2004 Metroid manga.

Categories: Samus (biography, physiology, personality, speech, roleplay guidance), Equipment (suits, weapons, abilities),
Chozo, Metroids, X Parasites, Space Pirates, Ridley, Galactic Federation, each game's events/places/characters, Dark Samus, Phazon,
technology and ships, species/planet indexes, and creature bestiaries.

Non-canon material (Super Smash Bros., Captain N, the comics, Prime Pinball, fan games) is excluded. Manga-only and manual-only
details are labelled as such inside the entries. Every entry was drafted from Metroid Wiki / Wikipedia / official sources, then
adversarially fact-checked by a second pass and revised; sources are listed per entry in the readable Markdown.

## Import

1. SillyTavern → **World Info** panel (the globe/book icon) → **Import World Info** → choose `Metroid Canon Lorebook.json`.
2. Activate it: either select it in the **Active World(s)** dropdown (global), or bind it to the Samus character card
   (character panel → World Info link / "Character Lore").
3. Done. Entries insert automatically when their trigger words appear in recent chat.

## Recommended World Info settings

| Setting | Value | Why |
|---|---|---|
| Scan Depth | 4–6 messages | Enough context to catch topics without stale triggers |
| Context % (budget) | 20–30 % | The lorebook is large; the cap keeps inserts proportional. 8k-context models: ~20 %. |
| Recursive Scan | Either | Every entry has "Prevent further recursion" set, because entries name each other so densely that recursion would cascade through the whole book. Only what the chat itself mentions gets inserted. |
| Match Whole Words | On (default) | Keys were written for whole-word matching, plurals included |
| Case Sensitive | Off (default) | |
| Insertion Strategy | "Sorted Evenly" or "Character Lore First" | Either is fine |

Two entries are **constant** (always inserted): a ~150-word universe primer and a ~300-word "Portraying Samus" guide
(placed after the character definition). Disable the latter if your Samus card already has a detailed personality block.

## Design notes

- Keys avoid bare generic words (e.g. "Prime", "Dark", "Baby") that would misfire in ordinary chat; ambiguous names
  (e.g. "Adam", "Nightmare") are **selective** and need a disambiguating secondary key ("Malkovich", "Bottle Ship"…).
- "Samus", "Metroid(s)", "Chozo", "Federation", "Space Pirates" and "Ridley" as bare keys appear only on their main
  overview entries, so saying her name doesn't dump every Samus entry into context each turn.
- Entries are written in-universe (no "the player"/"boss" language). Game titles appear only as chronology tags.
- `order` is higher for core entries (constants 200, Samus profile 150, timeline 140, major entities 110) so they win when the
  budget is tight; bestiary/index entries are 90.

## Regenerating

The lorebook was assembled with `tools/build_lorebook.py` from per-domain JSON files; `tools/simulate.py` emulates
SillyTavern's whole-word key matching on sample chat lines to check which entries fire.
