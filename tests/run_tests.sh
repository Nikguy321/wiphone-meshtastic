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
CC="${CC:-cc}"
FLAGS=(-std=c++11 -O1 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined)
# Vendored C (helix) is built with the C compiler and its own flags. It MUST NOT go
# through the C++ front end: C++11 narrowing rules reject helix's constant tables, which
# are full of values above INT_MAX written as plain integers. Perfectly legal C.
#
# ASan yes, UBSan no. helix is fixed-point DSP and shifts negative values left all over
# dct32.c and imdct.c, which UBSan reports and which is exactly what the algorithm means
# to do. Leaving it on buried a passing run in hundreds of lines of noise about
# third-party code that is not ours to change. ASan is kept because a buffer overrun in a
# decoder fed untrusted files is a real thing worth catching.
CFLAGS=(-std=c99 -O1 -g -fsanitize=address -w)

fail=0
for src in tests/test_*.cpp; do
  name="$(basename "$src" .cpp)"
  # Every WiPhone source the tests need must be free of Arduino headers; that constraint is
  # what keeps this suite possible, so a link error here is a design warning, not a nuisance.
  extra=()
  csrc=()
  case "$name" in
    test_booksync) deps=(WiPhone/booksync.cpp WiPhone/book_hash.cpp) ;;
    test_epub)     deps=(WiPhone/epub_parse.cpp WiPhone/book_hash.cpp); extra=(-lz) ;;
    test_bookstore) deps=(WiPhone/bookstore.cpp WiPhone/booksync.cpp WiPhone/book_hash.cpp) ;;
    test_layout)   deps=(WiPhone/book_layout.cpp) ;;
    test_inbox)    deps=(WiPhone/booksync_inbox.cpp WiPhone/booksync.cpp WiPhone/book_hash.cpp) ;;
    test_jpeg)     deps=(WiPhone/jpeg_grey.cpp) ;;
    test_music)    deps=(WiPhone/music_lib.cpp WiPhone/wav_reader.cpp) ;;
    test_sms_mirror) deps=(WiPhone/sms_mirror.cpp) ;;
    # The PKC DM crypto: the shipping mesh_pki.cpp plus the vendored donna and
    # tiny-AES beneath it (C, so csrc), against vectors from Python cryptography
    # playing the RAK's side of the exchange.
    test_pki)      deps=(WiPhone/mesh_pki.cpp WiPhone/book_hash.cpp)
                   csrc=(WiPhone/src/crypto/curve25519_donna.c WiPhone/src/crypto/tiny_aes.c) ;;
    # Compiles the REAL helix decoder so a pass proves the bytes that ship are the bytes
    # that decode. helix is C and is listed in csrc, not deps — see CFLAGS above.
    test_mp3)      deps=(WiPhone/mp3_stream.cpp)
                   csrc=(WiPhone/src/audio/helix-mp3/*.c) ;;
    *)             deps=() ;;
  esac
  echo "building $name"
  objs=()
  if [ "${#csrc[@]}" -gt 0 ]; then
    cobj="$OUT/$name-c"
    mkdir -p "$cobj"
    for c in ${csrc[@]+"${csrc[@]}"}; do
      o="$cobj/$(basename "$c" .c).o"
      "$CC" "${CFLAGS[@]}" -I"$(dirname "$c")" -c "$c" -o "$o"
      objs+=("$o")
    done
  fi
  "$CXX" "${FLAGS[@]}" -o "$OUT/$name" "$src" "${deps[@]}" ${objs[@]+"${objs[@]}"} ${extra[@]+"${extra[@]}"}
  if ! "$OUT/$name"; then
    fail=1
  fi
done

exit "$fail"
