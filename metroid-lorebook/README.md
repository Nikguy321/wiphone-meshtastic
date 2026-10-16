# Metroid Canon Lorebook (SillyTavern World Info)

A SillyTavern World Info ("lorebook") covering the canon lore of the Metroid series, written for roleplay in which
{{user}} interacts with Samus Aran (Samus as the character card).

**Files**

- `Metroid Canon Lorebook.json` — the importable lorebook (270 entries, ~140k tokens of lore in total; only matching entries are inserted per message).
- `characters/ADAM.json` — a SillyTavern character card for ADAM, the AI carrying Adam Malkovich's mind (see below).
- `Metroid Canon Lorebook (readable).md` — the same entries as readable text, with trigger keys and the sources each entry was checked against.
- `build/build_report.json` — build statistics and key-collision report.

## Canon coverage

Every mainline and Prime-series game in the official chronology plus the official manga backstory:
Zero Mission (Zebes) · Metroid Prime / Remastered (Tallon IV) · Prime Hunters (Alimbic Cluster) · Prime 2: Echoes (Aether) ·
Prime 3: Corruption (Phazon crisis) · Federation Force (Bermuda System) · Prime 4: Beyond (Viewros) · Samus Returns (SR388) ·
Super Metroid · Other M (Bottle Ship) · Fusion (BSL) · Dread (ZDR) · the 2002–2004 Metroid manga.

Entries per category (the category is the prefix of each entry's title in SillyTavern):

| Category | Entries |
|---|---|
| Universe | 6 |
| Samus | 18 |
| Equipment | 35 |
| Chozo | 10 |
| Metroids | 10 |
| X Parasite | 8 |
| Space Pirates | 10 |
| Ridley | 8 |
| Galactic Federation | 9 |
| Manga | 7 |
| Zebes | 8 |
| Super Metroid | 8 |
| SR388 | 6 |
| Prime | 9 |
| Prime Hunters | 11 |
| Prime 2 | 10 |
| Prime 3 | 13 |
| Dark Samus | 6 |
| Phazon | 7 |
| Federation Force | 6 |
| Prime 4 | 10 |
| Other M | 9 |
| Fusion | 8 |
| Dread | 12 |
| Technology | 7 |
| Creatures | 19 |

Non-canon material (Super Smash Bros., Captain N, the comics, Prime Pinball, fan games) is excluded. Manga-only and manual-only
details are labelled as such inside the entries. Every entry was drafted from Metroid Wiki / Wikipedia / official sources, then
adversarially fact-checked by a second pass and revised. Cross-entry consistency passes then reconciled contradictions
between related entries, and a completeness audit added entries for missing subjects. Sources are listed per entry in the readable Markdown.

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

Two entries are **constant** (always inserted, about 850 tokens together): a ~250-word universe primer and a ~375-word "Portraying Samus" guide
(placed after the character definition). Disable the latter if your Samus card already has a detailed personality block.

## Design notes

- Keys avoid bare generic words (e.g. "Prime", "Dark", "Baby") that would misfire in ordinary chat; ambiguous names
  (e.g. "Nightmare", "Anthony", "Trace") are **selective** and need a disambiguating secondary key ("Malkovich", "Bottle Ship"…).
- "Samus", "Metroid(s)", "Chozo", "Federation", "Space Pirates" and "Ridley" as bare keys appear only on their main
  overview entries, so saying her name doesn't dump every Samus entry into context each turn.
- Entries are written in-universe (no "the player"/"boss" language). Game titles appear only as chronology tags.
- `order` is higher for core entries (constants 200, Samus profile 150, timeline 140, major entities 110) so they win when the
  budget is tight; bestiary/index entries are 90.

## Character card: ADAM

`characters/ADAM.json` is a SillyTavern character card (V2) for ADAM, the gunship computer that carries the mind of
Samus's former commanding officer, Adam Malkovich. Import it from the character panel (Import Character, choose the JSON).

- **Default setting:** shortly after the BSL incident (Metroid Fusion), aboard Samus's gunship. {{user}} is Samus, or a
  separate Samus card in a group chat. One alternate greeting starts on the BSL before the reveal; another works when
  {{user}} is not Samus.
- **Canon basis:** dialogue and facts checked against the Fusion and Other M scripts, Dread's game text and the manga.
  Lines Raven Beak spoke while impersonating ADAM on ZDR are listed so ADAM never claims them.
- **Lorebook link:** the card points at "Metroid Canon Lorebook", so it uses this lorebook automatically once imported under that name.
- **Group chats:** lower talkativeness to about 0.3 if ADAM interjects too often.

## Things you may want to change

- **Your persona is named Adam?** Remove the key `Adam` from the entry "Other M: Adam Malkovich" (the other Adam entries need context words to fire).
- **Small context window (8k)?** Set the World Info budget to around 1,500-2,000 tokens. Entries are 150-600 words; the core and always-on entries have higher order, so they win when the budget is tight.
- **Samus's name triggers her core profile.** Saying "Samus" inserts the ~550-word core biography. That is usually what you want; if your card already holds her full bio, disable "Samus: Samus Aran – core profile" or give it a cooldown.
- **Spoilers.** The lorebook covers every game through Metroid Prime 4: Beyond (2025), including endings.
- **Canon caveats.** Where official sources conflict (the manga vs. the games, Japanese vs. English Other M script, single-source Prime 4 details), entries say "per the manga" or hedge; nothing non-canon was added on purpose.

## Regenerating

The lorebook is assembled from the per-domain files in `entries/`:

```
python3 tools/build_lorebook.py entries . "Metroid Canon Lorebook"
python3 tools/simulate.py "Metroid Canon Lorebook.json"            # which entries fire on sample chat lines
python3 tools/simulate_recursion.py "Metroid Canon Lorebook.json"  # confirms recursion stays contained
```

## Folder layout

- `entries/<domain>.json`: the final entries, one file per lore domain (30 domains plus `gap1`-`gap4` from the completeness audit).
- `checkpoint/draft`, `checkpoint/verify`, `checkpoint/revise`: every first draft, adversarial fact-check report and revision changelog.
- `checkpoint/consistency`, `build/cross/`: the cross-entry consistency reports and the cross-cluster issues they raised.
- `tools/workflow-*.js`: the multi-agent scripts that researched, verified and polished the entries.
