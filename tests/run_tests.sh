#!/usr/bin/env bash
# Build and run the host-side tests.
#
# These compile the PHONE'S OWN source files with the Mac's compiler. That is the point:
# a pass proves the bytes that ship are the bytes COVEY expects. Nothing here needs
# PlatformIO, an ESP32, or the phone to be plugged in.
#
#   ./tests/run_tests.sh
#
# To regenerate the interop vectors after a change to COVEY's booksync.py:
#   python3 tools/gen_booksync_vectors.py \
#       --booksync ../New_phone_project/.../covey_ui/booksync.py \
#       --out tests/vectors_booksync.h
set -euo pipefail

cd "$(dirname "$0")/.."
OUT=".pio/hosttests"
mkdir -p "$OUT"

CXX="${CXX:-c++}"
FLAGS=(-std=c++11 -O1 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined)

fail=0
for src in tests/test_*.cpp; do
  name="$(basename "$src" .cpp)"
  # Every WiPhone source the tests need must be free of Arduino headers; that constraint is
  # what keeps this suite possible, so a link error here is a design warning, not a nuisance.
  extra=()
  case "$name" in
    test_booksync) deps=(WiPhone/booksync.cpp WiPhone/book_hash.cpp) ;;
    test_epub)     deps=(WiPhone/epub_parse.cpp WiPhone/book_hash.cpp); extra=(-lz) ;;
    test_bookstore) deps=(WiPhone/bookstore.cpp WiPhone/booksync.cpp WiPhone/book_hash.cpp) ;;
    test_layout)   deps=(WiPhone/book_layout.cpp) ;;
    test_inbox)    deps=(WiPhone/booksync_inbox.cpp WiPhone/booksync.cpp WiPhone/book_hash.cpp) ;;
    *)             deps=() ;;
  esac
  echo "building $name"
  "$CXX" "${FLAGS[@]}" -o "$OUT/$name" "$src" "${deps[@]}" ${extra[@]+"${extra[@]}"}
  if ! "$OUT/$name"; then
    fail=1
  fi
done

exit "$fail"
