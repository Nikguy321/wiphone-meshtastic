#!/bin/bash
# card_clone.sh - copy a WiPhone SD card to the Mac, or a copy back onto a card.
#
#   tools/card_clone.sh pull /Volumes/WIPHONE ~/wiphone-cards/phone1     # card -> folder
#   tools/card_clone.sh push ~/wiphone-cards/phone1 /Volumes/NEWCARD      # folder -> card
#
# Everything on the card is copied - configs, roms, books, photos, messages, the maps - except
# what macOS itself leaves on a FAT volume (._sidecars, .DS_Store, .Spotlight-V100, .fseventsd,
# .Trashes). Nothing is ever deleted at the destination, so a push onto a card that already has
# files adds to them. Both directions end with a count of files on each side so a copy that
# stopped early cannot pass for a finished one.
#
# The card must be FAT32 (the phone does not read exFAT). A fresh 32 GB card comes that way;
# a card over 32 GB does not, and needs `diskutil eraseDisk FAT32 WIPHONE MBRFormat diskN`
# first (look the disk number up with `diskutil list` - it is a card, not your Mac).
#
# Copying is rsync with -rt: FAT32 has no owners or permissions to keep, and the timestamps
# are kept to the 2 s FAT allows. Eject with `diskutil eject` before pulling the card.
set -u

usage() {
  echo "usage: $0 pull /Volumes/<card> <folder>" >&2
  echo "       $0 push <folder> /Volumes/<card>" >&2
  exit 2
}

[ $# -eq 3 ] || usage
mode=$1; src=$2; dst=$3
case "$mode" in
  pull) card=$src; folder=$dst ;;
  push) card=$dst; folder=$src ;;
  *) usage ;;
esac

# The card side must be a mounted volume, and not the Mac's own.
case "$card" in
  /Volumes/*) ;;
  *) echo "$card is not under /Volumes - refusing (that is where cards mount)" >&2; exit 2 ;;
esac
if [ ! -d "$card" ]; then
  echo "$card is not mounted" >&2; exit 2
fi
if [ "$mode" = pull ] && [ ! -d "$folder" ]; then
  mkdir -p "$folder" || exit 1
fi
if [ "$mode" = push ] && [ ! -d "$folder" ]; then
  echo "no such folder: $folder" >&2; exit 2
fi

EXCL=(--exclude '._*' --exclude '.DS_Store' --exclude '.Spotlight-V100' --exclude '.fseventsd'
      --exclude '.Trashes' --exclude '.TemporaryItems' --exclude '.metadata_never_index')

count() {   # files, not counting the macOS leftovers
  find "$1" -type f ! -name '._*' ! -name '.DS_Store' ! -name '.metadata_never_index' \
       ! -path '*/.Spotlight-V100/*' ! -path '*/.fseventsd/*' ! -path '*/.Trashes/*' \
       ! -path '*/.TemporaryItems/*' | wc -l | tr -d ' '
}

if [ "$mode" = push ]; then
  # Spotlight would otherwise index every tile as it lands and slow the copy.
  mdutil -i off "$card" >/dev/null 2>&1 || true
  touch "$card/.metadata_never_index" 2>/dev/null || true
fi

echo "$mode: $src -> $dst"
before=$(count "$dst")
t0=$(date +%s)
rsync -rt --no-perms --no-owner --no-group "${EXCL[@]}" "$src/" "$dst/"
rc=$?
t1=$(date +%s)
if [ "$mode" = push ]; then
  dot_clean -m "$card" 2>/dev/null || true
fi
a=$(count "$src"); b=$(count "$dst")
echo "rsync exit $rc in $((t1 - t0))s: $a files at the source, $b at the destination (was $before)"
if [ $rc -ne 0 ] || [ "$b" -lt "$a" ]; then
  echo "NOT COMPLETE - re-run the same command; what landed is kept" >&2
  exit 1
fi
if [ "$mode" = push ]; then
  echo "eject before pulling the card:  diskutil eject $card"
fi
