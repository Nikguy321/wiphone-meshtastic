# Maps on the WiPhone

**Menu → Tools → Maps.** An offline map: tiles off the SD card, pins you drop yourself, and
everything the mesh already knows about where people are, on one screen.

The Meshtastic screens already carry every fact this draws — camp is a waypoint, the truck is
a waypoint, everyone's position arrives on port 3, and the Nodes list turns it into
"3.2km E of camp · 4 min ago". That sentence is correct and it is the wrong shape for the
question people actually ask, which is *is he on the other side of the creek or this side*.

It needs no radio traffic of its own and no GPS. With neither, it still shows the places the
mesh has shared and the last position of everyone who has spoken.

---

## Keys

| Key | What it does |
|---|---|
| **Arrows** | scroll. Hold one down and it speeds up; let go and the next press is a nudge again |
| **2 4 6 8** | scroll too, for gloves |
| **\*** or **1** | zoom out |
| **#** or **3** | zoom in |
| **5** | drop a pin on the crosshair, and open the name field |
| **OK** | the pin under the crosshair (rename / move / share / delete) — or the map menu if there is no pin there |
| **7 / 9** | previous / next pin, centring the map on it |
| **0** | centre on me: a live GPS fix, else the pin you declared by hand |
| **Back** | leave. Where you were looking is saved |

The left soft key reads **Pin/Menu** because it does both: the pin under the crosshair if there
is one, the map menu if there is not. **Centre on me** means *this phone*; it falls through to
the reference place only as a last resort, and then the screen names it — a key labelled
"centre on me" that quietly lands on somebody else's camp has told you something false.

The same table is on the phone: **Menu → Keys and colours**.

## What the colours mean

| Mark | What it is |
|---|---|
| orange dot | one of your pins |
| yellow dot | one of your pins, shared with the mesh |
| green gem | a place heard from the mesh (a Meshtastic waypoint — COVEY's map shares these) |
| cyan dot | somebody else's last position, with their name |
| grey dot | …the same, but over 30 minutes old, and it says how old |
| white ring | this phone, from a GPS fix under two minutes old |
| grey ring | …the same, but the fix is older than that, and it says how much older |
| white gem | this phone's manually declared pin |

A stale position is drawn grey **on purpose**. On foot, half an hour is far enough to be
somewhere else entirely, and a map that draws an old dot the same as a live one is lying by
omission. The same rule applies to **you**: `getGpsFix()` answers "there has ever been a fix",
so a phone that lost the sky under canopy would otherwise keep drawing a confident white ring
over where it was an hour ago. Past two minutes — the mesh service's own freshness bar, not a
second one invented for the map — your ring goes grey and wears its age.

**Names are dropped, never squashed.** Eight places plus twenty nodes on a 240×250 screen is
not hypothetical on a hunting channel, and unmanaged labels overprint into a grey smear. Names
are placed most-important-first — you, your pins, the mesh's places, then other people (starred
first) — and one that would land on another is not drawn. Its **marker** is still there, which
is the part that says where somebody is, and the name is on the Nodes list either way.

## Where it opens

1. Where you closed it last. That is the whole point of it, and it is the only one of these
   that works with no sky and no signal.
2. This phone's GPS fix, if the receiver is on and has one.
3. The mesh's reference place — a chosen waypoint, a fresh fix, or your declared pin,
   whichever the Meshtastic app's own precedence picks.
4. The middle of whatever tiles are actually on the card.
5. 0, 0 — with the map saying there is nothing to show.

## Pins, and what "share" means

A pin is a private mark on your own map: *park here*, *the gate*, *blood trail starts*. It
lives in a text file on the card (`/maps/pins.txt`), there can be 64 of them, and **nothing
about it reaches the air**.

**Share it on the mesh** turns that pin into a Meshtastic *waypoint*: it goes out on the
radio, appears on COVEY's map and on any Meshtastic device that shares the channel, and comes
back to this phone's own Places list. That is a deliberate act, every time, and:

- it prefers a **private** channel — a location on LongFast is readable by every Meshtastic
  radio in range;
- it is **locked to this node**, so nobody else's radio can move or delete it;
- the phone says whether it actually **reached the air**. "Saved, but NOT sent" means the
  radio was not ready and nobody else has it;
- renaming or moving a shared pin **re-sends it**, so the mesh never holds a stale copy;
- **Take it off the mesh** sends the deletion marker and removes it from other people's maps
  too. A camp that has moved is worse than no camp at all.

The mesh's own place table holds **eight** waypoints, shared with COVEY. Your 64 pins are
separate and unaffected — only shared ones take one of those eight slots.

---

## The tiles

```
/maps/<area>/<z>/<x>/<y>.565
```

Standard slippy-map `z/x/y` numbering — the same numbering every tile downloader and every
`z/x/y` cache on a computer already uses, COVEY's included. `<area>` is whatever you want the
map called on the phone (letters, digits, `-`, `_`, `.`; at most 31 characters; not starting
with a dot). You can have several; the Maps menu lists them when there is more than one.

**A tile is 256×256 pixels, RGB565, little-endian, 131072 bytes exactly.** Raw. Not PNG, not
JPEG.

### Why raw, when raw is eight times bigger

The phone decodes nothing, and that is the design rather than a shortcut.

- There is **no PNG decoder** in this firmware. Adding one costs a vendored library plus
  contiguous internal RAM on a phone whose crashes are internal-heap fragmentation.
- The ESP32's ROM **JPEG decoder refuses greyscale outright** — that is why `jpeg_grey.cpp`
  exists at all (33 of 45 pictures in one test book were 1-component JPEGs).

So a map made of compressed tiles would work right up until the day it met a tile of the
wrong flavour, in the woods, with nothing on the screen to say what was wrong. Raw costs card
space and buys zero decode time, zero decoder memory, and exactly **one** failure mode:
*the file is not 131072 bytes*, which the phone checks in one comparison and reports.

It also means a PNG copied to the card without being converted is refused rather than drawn
as a screenful of confetti.

### What it costs on the card

A 20 × 20 km area at roughly 47° N:

| zoom | ground per pixel | tiles | size |
|---|---|---|---|
| z12 | ~26 m | ~9 | 1.2 MB |
| z13 | ~13 m | ~35 | 4.5 MB |
| z14 | ~6.5 m | ~130 | 17 MB |
| z15 | ~3.2 m | ~500 | 63 MB |

About **85 MB for the whole stack** — nothing on a 32 GB card. z13–z15 is the useful range
on a 240×320 screen; z16 is sharper but four times the card space for ground you can already
see.

---

## On the Mac: making the card

`tools/convert_tiles.py` converts a tile tree (or an `.mbtiles` file) into the format above.
It needs **nothing installed** — it uses Pillow when you have it and macOS's own `sips` when
you do not.

**The command**, with the card mounted and a tile tree in `~/covey-tiles`:

```bash
python3 tools/convert_tiles.py ~/covey-tiles /Volumes/WIPHONE/maps/home --zoom 12-15
```

Check the damage before committing to it:

```bash
python3 tools/convert_tiles.py ~/covey-tiles /Volumes/WIPHONE/maps/home --zoom 12-15 --dry-run
```

From an `.mbtiles` file instead (the script unpacks it; the phone never sees it):

```bash
python3 tools/convert_tiles.py camp.mbtiles /Volumes/WIPHONE/maps/camp --zoom 12-15
```

Re-running is cheap: tiles that are already there and the right length are left alone, so
adding a zoom level or a new corner of the map only converts what is new.

### Three things that bite

⚠ **Convert straight onto the card, or copy with `rsync`.** Finder writes a `._name` sidecar
next to every file it copies onto FAT32 — one per tile, thousands of them, which the phone
then has to skip through. The command above writes directly to the card and creates none. If
you must copy a converted tree:

```bash
rsync -a --exclude '._*' --exclude '.DS_Store' ~/tiles-565/ /Volumes/WIPHONE/maps/home/
dot_clean -m /Volumes/WIPHONE
```

⚠ **The card must be FAT32.** Same rule as everything else on this phone; see the README.
Eject it properly — `diskutil eject /Volumes/WIPHONE` — before pulling it, and swap cards
with the phone **off**.

⚠ **`.mbtiles` rows are upside down.** MBTiles counts rows from the south (TMS), slippy tiles
count from the north. The script flips them. If you unpack an `.mbtiles` with some other tool
and the map looks *almost* right but subtly wrong, that is what happened — and on forest it
is remarkably hard to see.

### Where tiles come from

Whatever you already use for COVEY's map. This repo does not fetch them: a tile server's
terms are between you and the server, bulk downloading is exactly what those terms are about,
and an area worth carrying into the woods is small enough to fetch politely and once.

---

## Checking it worked, without pressing anything

A map's failure mode is showing the **wrong ground**, and that looks exactly like showing the
right ground — no screenshot can tell a tile tree that is one column out from a correct one.
So the serial console can drive the whole thing:

```
maps                          what the card holds, the saved view, the pins file
maps goto 47.6062 -122.3321 15 home     set where Maps opens next
```

`maps goto` writes the same saved view the app itself writes, so you can put a known
coordinate in over the cable, open the app, and compare the screen against a map on the
computer. `tools/shot.py` will take the screenshot.

---

## What it costs the phone

- **768 KB of PSRAM** while the app is open (six 128 KB tile slots), allocated once and freed
  on the way out. PSRAM runs ~3.6 MB free; internal RAM — the ~19 KB that SIP and WiFi fight
  over — is untouched.
- **A tile arrives in 32 KB pieces**, one per 25 ms tick, because a 128 KB SD read is
  100–250 ms and everything in this firmware shares one task. 250 ms is exactly the threshold
  the superloop's stall detector was built to complain about; four tiles read the obvious way
  is a second of frozen phone, a dropped WiFi connection and some eaten keypresses, every
  time you pan. A tile that has not arrived is drawn as a plainly-marked grey square.
- **No timer at all** once the screen is painted. A map that is finished must not keep the
  CPU awake.
