#!/bin/bash
# cardday.sh — the tile pool, card by card.  See docs/maps.md "Pooling the tiles of several
# devices" for what this is doing and why it is cards rather than WiFi.
#
#   cardday.sh phone2 pull /Volumes/WIPHONE    # OLD card -> a full backup + its tiles into the master
#   cardday.sh phone2 push /Volumes/NEWCARD    # that backup + the WHOLE master onto the new card
#   cardday.sh covey push                      # whatever COVEY lacks, over SSH
#   cardday.sh status                          # what the master holds
#
# Never deletes anything, at either end, and every step is re-runnable: a tile already on the
# card at the right length is skipped, a PNG already in the master is kept (an original beats
# a tile that has been through RGB565), and the backup is an --ignore-existing rsync.
set -u
REPO=/Users/nickhowe/wiphone
M=${CARDDAY_MASTER:-~/tiles-master}
CARDS=${CARDDAY_CARDS:-~/wiphone-cards}
LOG=$M/cardday.log

say() { echo "$(date '+%H:%M:%S') $*" | tee -a "$LOG"; }
die() { echo "$*" >&2; exit 2; }

# usgs-img is photography: COVEY's cache holds it as JPEG bytes under .png names (its
# downloader keeps whatever the server sent), and a photo as a true-colour PNG is 105 KB
# against 42 KB. Anything map-shaped stays PNG.
jpeg_flag() { case "$1" in usgs-img|*-img|*aerial*) echo "--jpeg" ;; *) echo "" ;; esac; }

count_png() { find "$1" -type f -name '*.png' 2>/dev/null | wc -l | tr -d ' '; }
count_565() { find "$1" -type f -name '*.565' 2>/dev/null | wc -l | tr -d ' '; }

status() {
  echo "master $M:"
  local tot=0
  for s in "$M"/*/; do
    [ -d "$s" ] || continue
    local n; n=$(count_png "$s")
    [ "$n" = 0 ] && continue
    printf "  %-12s %7d tiles  %6s  (z%s)\n" "$(basename "$s")" "$n" \
           "$(du -sh "$s" | cut -f1)" "$(ls "$s" | sort -n | tr '\n' ' ' | sed 's/ $//')"
    tot=$((tot + n))
  done
  printf "  %-12s %7d tiles = %d.%d GB as .565 on a card\n" TOTAL "$tot" \
         $((tot * 131072 / 1073741824)) $(( (tot * 131072 % 1073741824) * 10 / 1073741824 ))
  for c in "$CARDS"/*/; do
    [ -d "$c" ] || continue
    printf "  backup %-8s %d files, %s\n" "$(basename "$c")" \
           "$(find "$c" -type f ! -name '._*' | wc -l | tr -d ' ')" "$(du -sh "$c" | cut -f1)"
  done
}

pull() {          # $1 = phone tag, $2 = card volume
  local tag=$1 card=$2 dst=$CARDS/$1
  [ -d "$card" ] || die "$card is not mounted (ls /Volumes)"
  [ -d "$card/maps" ] || say "⚠ no /maps on this card — copying it anyway"
  say "=== PULL $tag from $card ==="
  # The manifest is taken from the CARD, not from the copy: it is the record of what this card
  # held, and it is what a later push is checked against.
  ( cd "$card" && find . -type f ! -name '._*' -exec stat -f '%z %N' {} + | sort -k2 ) > "$M/manifest_$tag.txt"
  say "$tag: the card holds $(wc -l < "$M/manifest_$tag.txt" | tr -d ' ') files, $(grep -c '\.565$' "$M/manifest_$tag.txt" | tr -d ' ') of them tiles"
  # Everything but the tiles: they come back from the master, which ends this pull holding a
  # PNG for every one of them. See the note in card_clone.sh.
  "$REPO/tools/card_clone.sh" pull --no-tiles "$card" "$dst" 2>&1 | tee -a "$LOG" || die "the card copy did not finish — re-run this same command"
  local added=0
  for a in "$card"/maps/*/; do          # read the tiles off the CARD (they are not in the copy)
    [ -d "$a" ] || continue
    local area; area=$(basename "$a")
    local have; have=$(count_565 "$a")
    [ "$have" = 0 ] && { say "  $area: no tiles, skipped"; continue; }
    local before; before=$(count_png "$M/$area")
    say "  $area: $have tiles on the card -> $M/$area (had $before)"
    python3 "$REPO/tools/tiles_565_to_png.py" "$a" "$M/$area" $(jpeg_flag "$area") 2>&1 | grep -E '^(wrote|[0-9]+ tile|  (skipped|stopped))' | tee -a "$LOG"
    local after; after=$(count_png "$M/$area")
    say "  $area: master $before -> $after (+$((after - before)) this phone had that nothing else did)"
    added=$((added + after - before))
  done
  say "=== PULL $tag done: +$added tiles into the master ==="
  say "Eject it:  diskutil eject $card"
}

push() {          # $1 = phone tag, $2 = card volume
  local tag=$1 card=$2 src=$CARDS/$1
  [ -d "$card" ] || die "$card is not mounted (ls /Volumes)"
  [ -d "$src" ] || die "no backup at $src — run `cardday.sh $tag pull` with the OLD card first"
  case "$(diskutil info "$card" | awk -F: '/File System Personality/{print $2}' | xargs)" in
    *FAT32*|*MS-DOS*) ;;
    *) die "$card is not FAT32 — the phone does not read exFAT. diskutil eraseDisk FAT32 WIPHONE MBRFormat diskN" ;;
  esac
  say "=== PUSH $tag to $card ==="
  say "step 1: the old card's contents ($(find "$src" -type f ! -name '._*' | wc -l | tr -d ' ') files, tiles excluded — they are step 2)"
  "$REPO/tools/card_clone.sh" push --no-tiles "$src" "$card" 2>&1 | grep -v '^eject before' | tee -a "$LOG" || die "the restore did not finish — re-run this same command"
  say "step 2: the master, one source at a time"
  for s in "$M"/*/; do
    [ -d "$s" ] || continue
    local area; area=$(basename "$s")
    [ "$(count_png "$s")" = 0 ] && continue
    local t0=$SECONDS
    python3 "$REPO/tools/convert_tiles.py" "$s" "$card/maps/$area" 2>&1 | grep -E '^(wrote|[0-9]+ tile|  (skipped|stopped))' | tee -a "$LOG"
    say "  $area: $(count_565 "$card/maps/$area") tiles on the card ($((SECONDS - t0))s)"
  done
  dot_clean -m "$card" 2>/dev/null
  say "=== PUSH $tag done: $(count_565 "$card/maps") tiles, $(df -h "$card" | tail -1 | awk '{print $4}') free ==="
  say "Eject it:  diskutil eject $card    then: phone on, Maps, Menu > Rescan card"
}

covey() {
  say "=== PUSH covey ==="
  for s in "$M"/*/; do
    [ -d "$s" ] || continue
    local area; area=$(basename "$s")
    [ "$(count_png "$s")" = 0 ] && continue
    local t0=$SECONDS
    rsync -a --ignore-existing --rsync-path='sudo rsync' "$s" "covey:/root/covey-tiles/$area/" 2>&1 | tail -2 | tee -a "$LOG"
    say "  $area: $(ssh covey "sudo find /root/covey-tiles/$area -type f -name '*.png' | wc -l" | tr -d ' ') tiles on COVEY ($((SECONDS - t0))s)"
  done
  say "=== PUSH covey done ==="
}

case "${1:-}" in
  status) status ;;
  covey)  [ "${2:-}" = push ] || die "usage: cardday.sh covey push"; covey ;;
  phone1|phone2)
    case "${2:-}" in
      pull) [ $# -eq 3 ] || die "usage: cardday.sh $1 pull /Volumes/<card>"; pull "$1" "$3" ;;
      push) [ $# -eq 3 ] || die "usage: cardday.sh $1 push /Volumes/<card>"; push "$1" "$3" ;;
      *) die "usage: cardday.sh $1 pull|push /Volumes/<card>" ;;
    esac ;;
  *) awk 'NR>1 && /^#/ {sub(/^# ?/, ""); print; next} NR>1 {exit}' "$0"; exit 2 ;;
esac
