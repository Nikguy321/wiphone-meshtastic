# Maps on the WiPhone

**Menu → Maps.** An offline map: tiles off the SD card, pins you drop yourself, and
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
| **Arrows** | scroll. A tap is a **12 px nudge**, always — taps never speed up; **hold one down and it keeps scrolling and speeds up** (after 400 ms, 240 → 480 → 720 → 960 px/s over the next 0.6 s — by TIME, so a frame that comes late moves further, not later, at ~12 frames a second; since 0.9.73); let go and the next press is a nudge again. A hold also rides through the keypad chip's habit of reporting a release and a re-press 4 ms apart under a thumb that never moved (0.9.73 — it used to stop the sweep dead after one to four jumps). With **Snap to markers** on (the menu; it is on by default) the map snaps onto a pin, a place from the mesh or a node with a position **when the crosshair lands within 10 px of it** — after a tap, or where a hold stops. Nothing else: a marker in the path of a sweep is sailed past, and the tap after a snap steps clear (the nudge is bigger than the radius), so you can scroll around them. Since taps on the two axes can put the crosshair within 8.5 px of anything, "as near as you can get" is always near enough. The strip says what you landed on: `Pin 3 (3 of 4)`, `hunt camp (place from the mesh)`, `Nick H (node, 12m ago)`. `7` / `9` walk your pins in order |
| **2 4 6 8** | scroll too, for gloves |
| **Side button 1 / 2** (top two) | zoom in / out. Also `#`/`*` or `3`/`1` |
| **Side button 3** | centre on me: a live GPS fix (4+ satellites), else the pin you declared by hand. Also `0` |
| **Side button 4** (bottom) | the next map type — the same switch as **Menu → Map area**, one press at a time, round and round (`usgs-topo` → `usgs-img` → `otm` → …). The ground under the crosshair stays put; the strip says which map you are on. With one map on the card it says so |
| **OK** | drop a pin on the crosshair and name it — or, with a pin under the crosshair, open it (rename / move / share / delete). Also `5` |
| **Menu** (top-left soft key) | **What the buttons do...** is its first row — the phone's buttons drawn with what each does. Then download maps, go to coordinates, follow me, measure (an anchor at the crosshair; scroll away and the strip reads the distance and bearing), **Snap to markers: ON/off**, pins, places, nodes — the three lists are **nearest-first** with distance and bearing on every row |
| **7 / 9** | previous / next pin, centring the map on it |
| **Back** | leave. Where you were looking is saved — also on power-off |

While the map is open the side buttons belong to it, not to the music player (the Game Boy
has the same rule), so zoom cannot silently turn into play/pause because a track is loaded.

The strip's message row grows to two rows when a message needs them — "No fix yet, and no pin
of your own to fall back on" used to end in `..` — and shrinks back at the next key (0.9.70).

The left soft key reads **Pin/Menu** because it does both: the pin under the crosshair if there
is one, the map menu if there is not. **Centre on me** means *this phone*; it falls through to
the reference place only as a last resort, and then the screen names it — a key labelled
"centre on me" that quietly lands on somebody else's camp has told you something false.

The same table is on the phone: **Menu → Keys and colours**.

## What the colours mean

| Mark | What it is |
|---|---|
| orange dot | one of your pins |
| yellow ring | one of your pins, shared with the mesh — hollow centre, so it is not a colour judgement |
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

Three ways, in the order you will probably use them:

1. **The phone downloads them itself** — `Menu → Download maps...` on the map, over WiFi.
   See the next section. This is the one that works with no computer in the loop.
2. **Push a converted tree from the Mac**, no card swapping: convert as above into any
   folder, then `python3 tools/wiphone_send.py --app maps --tree ~/tiles-565/home` sends it
   to `/maps/home/...` over the phone's own upload server (271 tiles, 34 MB: about ten
   minutes). The uploader's `maps` mode is the only one that accepts folder paths, and only
   `<area>/<z>/<x>/<y>.565` shaped ones. Two things to know: a tile that is already on the
   card at the right size is skipped (add `--replace` to push a re-converted tree, since every
   tile is the same size), and if Maps is OPEN while a tree arrives it may blacklist a tile it
   caught half-written — `Menu → Rescan the card` afterwards, or push with Maps closed. The
   same goes for `pins.txt`: push it with Maps closed, or the next pin edit writes over it.
3. **Straight onto the card**, as the commands above show.

A tile server's terms are between you and the server. USGS is public domain and asks for a
credit line; OpenTopoMap is CC-BY-SA and asks not to be mass-downloaded — the phone waits
0.6 s between its tiles and says so on the help screen. Both credits are printed there.

### Pooling the tiles of several devices

Two phones and a COVEY that each downloaded their own patches end up with three different
maps. The union of them is one tree of PNG originals on the Mac, and every device gets the
whole of it. The tree is the standard `<source>/<z>/<x>/<y>.png` — COVEY's own cache layout,
so its cache seeds it directly and is refilled from it with plain `rsync`.

The phones need their **cards in the Mac's reader**: the union is 128 KB per tile, and a
hundred thousand tiles is ~13 GB — a week over the phone's WiFi uploader (measured 48–69 KB/s)
against under an hour onto a card. Nothing reads files off the phone over WiFi at all.

```bash
# 1. the originals: COVEY's cache (its WiFi link is slow; ~2.5 GB took ~90 min)
ssh covey 'sudo tar cf - -C /root/covey-tiles .' | tar xf - -C ~/tiles-master

# 2. each phone's card, in the reader: copy the whole card, then add its tiles to the master
#    (a PNG already in the master is kept — an original beats a tile that has been through 565)
tools/card_clone.sh pull /Volumes/WIPHONE ~/wiphone-cards/phone1
for a in ~/wiphone-cards/phone1/maps/*/; do
  python3 tools/tiles_565_to_png.py "$a" ~/tiles-master/$(basename "$a")   # --jpeg for usgs-img
done

# 3. the union back onto a card (a new one: clone the old card's contents first)
tools/card_clone.sh push ~/wiphone-cards/phone1 /Volumes/NEWCARD
for s in ~/tiles-master/*/; do
  python3 tools/convert_tiles.py "$s" /Volumes/NEWCARD/maps/$(basename "$s")
done
diskutil eject /Volumes/NEWCARD

# 4. and back to COVEY — only what it lacks travels
for s in ~/tiles-master/*/; do
  rsync -a --ignore-existing --rsync-path='sudo rsync' "$s" covey:/root/covey-tiles/$(basename "$s")/
done
```

The area name on the phone is the source's folder name (`usgs-topo`, `usgs-img`, `otm`), so
the same tile is the same file on all three devices and a re-run copies nothing twice:
`convert_tiles.py` keeps a tile that is already there at the right length, `tiles_565_to_png.py`
keeps a PNG that exists, and `card_clone.sh` never deletes. `convert_tiles.py` does ~300
tiles/s with numpy installed (`python3 -m pip install --user numpy`); the card is the limit.

`tiles_565_to_png.py` is `convert_tiles.py` run backwards (5-6-5 bits replicated up to 8, so
white stays white); `tests/check_convert_tiles.py` holds the two to each other, because a tile
that goes phone → master → other phone passes through both. ⚠ COVEY's aerial tiles are JPEG
bytes under `.png` names (its downloader keeps whatever the server sent, and the map loads by
content) — `--jpeg` writes the same, at a third of the bytes of a true-colour PNG of a photo.

---

## Download maps on the phone

`Menu → Download maps...` fetches an area around the crosshair over WiFi and writes it to the
card in the raw format above, so the viewer never learns where a tile came from.

| Row | Choices |
|---|---|
| Source | **USGS Topo** (contours, roads, labels — the hunt map), **USGS Aerial**, **OpenTopoMap** (denser contours, CC-BY-SA, slow server) |
| Radius | 2 / 5 / 10 / 20 km around the crosshair |
| Detail | to z13, z14, z15 or z16. z15 is 3 m per pixel; USGS z16 is the same drawing scaled up and costs four times as much |

The line under them is the honest estimate — tiles, megabytes on the card, minutes — from
the rates measured on the phone (USGS about a second a tile, OpenTopoMap about four). **Start
download** needs WiFi and either USB power or a battery above 3.8 V; it says which is missing.
While it runs the screen shows `Downloading 24/271`, the zoom level it is on, bytes, seconds
and failures, and **Stop** finishes the tile in hand and quits. The screen stays awake while
you watch it; leave it and the download carries on (the map's own tiles, the messages, the
clock — all still work), and it pauses by itself for a phone call or a dropped WiFi and
resumes when they clear. Re-running the same area only fetches what is missing: a tile is
"there" only when it is exactly 131072 bytes, so a power-off mid-tile leaves nothing the
next run will not repair. When a run is over the screen keeps its account — `Last run: 5517
new, 7332 already had, 4 failed, 7013 s` — and, if anything failed, `Last problem:` with the
last tile that did and why. Every line on this screen **wraps** onto more rows rather than
ending in `..` (0.9.68).

`card refused the write` is the card saying no to one tile's 128 KB: a short write from the
SD layer, usually one busy timeout on a card that is otherwise fine (phone 2 lost 4 tiles of
12,853 to it). Since 0.9.68 a refused write is tried once more before it counts; a tile that
still fails is simply missing, and **Start download** again fetches only the missing ones.
A card that refuses ten in a row stops the run — that is a full or pulled card.

Each source is its own area on the card (`/maps/usgs-topo`, `/maps/usgs-img`, `/maps/otm`);
`Map area:` in the menu switches between them, as does the bottom side button. An area the
downloader did not make — `home`, the tiles converted on the computer with
`tools/convert_tiles.py` — is a map too, just one the downloader cannot add to.

**Where a map's tiles are.** An area that is not where you are looking is indistinguishable
from an empty one — grey squares either way — so since 0.9.71 the phone reads each area's
extent once when Maps opens (the folder names at its coarsest zoom: three small directory
listings) and says so: switching onto a map whose tiles are elsewhere puts `Map: home (z11-15)
- its tiles are 35km NW of here` on the strip, and every row of **Map area** ends with `here`
or the distance and bearing to that map's tiles from the crosshair. Serial `maps` prints each
area's box in degrees. It is a bounding box, so an L-shaped download reads as its enclosing
rectangle. Deleting an area is a job for the computer: remove `/maps/<area>` from the card.

What it costs (measured on WiPhone 2, 2026-09-18):

| Area | Tiles | On the card | USGS Topo | OpenTopoMap |
|---|---|---|---|---|
| 2 km, z11–15 | ~55 | 7 MB | ~1 min | ~4 min |
| 5 km, z11–15 | ~270 | 34 MB | ~4.5 min | ~18 min |
| 10 km, z11–15 | ~865 | 113 MB | ~15 min | ~1 h |
| 20 km, z11–15 | ~3,260 | 427 MB | ~1 h | ~3.5 h |

### How it works, and why it took a night to make it work

The phone had never opened an HTTPS connection: the framework builds mbedTLS to allocate from
**internal** RAM, its two 16 KB record buffers alone exceed the phone's largest free block, and
the handshake runs on the calling task's 8 KB stack. `tile_fetch.h` has the whole story. The
short version: the downloader runs on its own task with a 10 KB stack, redirects mbedTLS's
allocator into PSRAM once (`mbedtls_platform_set_calloc_free`), keeps one connection alive for
the whole area, decodes each tile with the decoders already in the ESP32's ROM (TJpgDec for
USGS's JPEG, the `tinfl` inflater for OpenTopoMap's PNG — `tile_png.cpp` is the 300 lines of
PNG around it, proven against 41 host checks), and writes the raw file to a temporary name
before renaming it into place.

Internal RAM is the thing to watch: the task's stack is 10 KB for the life of the firmware
from the first download, and the first TLS handshake of a run dips the internal heap by
another ~12 KB for a second or two. The downloader refuses to start when the largest free
block is under 14 KB (24 KB before the stack exists) and tells you to reboot first. The
serial console has the numbers: `maps dl` prints the run's heap floor and the task's stack
high-water mark.

### From the console

```
maps dl                                  what the last / current run did, with the heap floor
maps dl 0 47.42 -121.75 5 15             start: source 0=USGS Topo 1=USGS Aerial 2=OpenTopoMap
maps dl stop                             finish the tile in hand and quit
maps dlurl http://192.168.1.17:8765/{z}/{x}/{y}.jpg    a plain-HTTP relay as source 3
tlstest <url> [n]                        the TLS bench: n kept-alive GETs, heap and timing
open maps                                jump into the app; `hold on` keeps the screen awake
maps hold right 1500 [300]               hold an arrow for 1500 ms from the cable; with the
                                         third number, fake the chip's release-blip at 300 ms
                                         (`MAPS: hold continues past a ...` proves the ride-through)
keys raw                                 the last 64 bytes the keypad chip sent, with gaps
```

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

- **Since 0.9.74 the whole phone has ~40 KB more internal RAM** (the SDK's unused Bluetooth
  reserve, released at boot — CHANGELOG 0.9.74), so the numbers below that say "~19 KB" or
  "~25 KB" of internal heap describe the phones as they were when measured; idle is ~66 KB now
  and a download bottoms out near 48 KB. The downloader's 14 KB handshake bar is unchanged;
  when it cannot be met the Download screen and `maps dl` now print how far short the phone is.
- **768 KB of PSRAM** while the app is open (six 128 KB tile slots), allocated once and freed
  on the way out. PSRAM runs ~3.6 MB free; internal RAM — the ~19 KB that SIP and WiFi fight
  over — is untouched by the viewer. The downloader is the exception: see above.
- **A tile arrives in 32 KB pieces**, one per 25 ms tick, because a 128 KB SD read is
  100–250 ms and everything in this firmware shares one task. 250 ms is exactly the threshold
  the superloop's stall detector was built to complain about; four tiles read the obvious way
  is a second of frozen phone, a dropped WiFi connection and some eaten keypresses, every
  time you pan. A tile that has not arrived is drawn as a plainly-marked grey square.
- **No timer at all** once the screen is painted. A map that is finished must not keep the
  CPU awake.
