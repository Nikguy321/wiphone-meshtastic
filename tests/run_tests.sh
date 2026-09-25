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
    # KOSync (KOReader's sync protocol): the vectors from tools/gen_kosync_vectors.py (partial
    # MD5, CrossPoint's byte-weighted spine and its inverse), the pure protocol module, the
    # inbound chain into the booksync inbox, AND the before/after proof that no saved place
    # moves (tests/golden_positions.h, generated once from the pre-KOSync parser).
    test_kosync)   deps=(WiPhone/kosync.cpp WiPhone/epub_parse.cpp WiPhone/book_hash.cpp
                         WiPhone/booksync.cpp WiPhone/booksync_inbox.cpp WiPhone/bookstore.cpp)
                   extra=(-lz) ;;
    test_jpeg)     deps=(WiPhone/jpeg_grey.cpp) ;;
    test_music)    deps=(WiPhone/music_lib.cpp WiPhone/wav_reader.cpp) ;;
    test_sms_mirror) deps=(WiPhone/sms_mirror.cpp) ;;
    # The PKC DM crypto: the shipping mesh_pki.cpp plus the vendored donna and
    # tiny-AES beneath it (C, so csrc), against vectors from Python cryptography
    # playing the RAK's side of the exchange.
    test_pki)      deps=(WiPhone/mesh_pki.cpp WiPhone/book_hash.cpp)
                   csrc=(WiPhone/src/crypto/curve25519_donna.c WiPhone/src/crypto/tiny_aes.c) ;;
    # Position/Waypoint payloads + the distance/bearing math (python-checked).
    test_pos)      deps=(WiPhone/mesh_pos.cpp) ;;
    # Which chat messages survive a reboot: newest-per-conversation, capped.
    test_retain)   deps=(WiPhone/mesh_retain.cpp) ;;
    # The woods plate's GPS: NMEA assembly, checksums, fixed-point coords.
    test_nmea)     deps=(WiPhone/nmea.cpp) ;;
    # Sunrise/sunset/civil twilight (NOAA method) — almanac anchors + geometry.
    test_sun)      deps=(WiPhone/sun_times.cpp) ;;
    # The map: Web Mercator, the slippy-tile grid, the blit rectangles, and the pins
    # file. Pure arithmetic on purpose — a map that is one tile out looks fine on a
    # 240x320 screen, so this is the only place that error can be caught.
    test_maptiles) deps=(WiPhone/map_tiles.cpp WiPhone/map_pins.cpp) ;;
    test_tilepng)  deps=(WiPhone/tile_png.cpp WiPhone/tile_decode.cpp); extra=(-lz) ;;
    # The map download's arithmetic and decisions (tile_plan.cpp): tile counts, 64-bit bytes,
    # the Detail row's depths, the centre-out block order, the time, and the auto-resume table.
    test_tileplan) deps=(WiPhone/tile_plan.cpp WiPhone/map_tiles.cpp) ;;
    # Voltage -> SOC off phone 1's recorded discharge (tests/fixtures/p1_discharge_2026-09-03.tsv).
    # Scores the table AND the CW2015's own number against time-linear truth.
    test_battery)  deps=(WiPhone/battery_curve.cpp) ;;
    # Why the WiFi dropped: the core's reason->status/rejoin predicates (how an old health.log's
    # wifi=5 is read), the IDF reason/err names, the event ring the loop drains, the HEALTH field.
    test_wifidiag) deps=(WiPhone/wifi_diag.cpp) ;;
    # Header-only: the CPU clock gate's decisions (cpu_clock_policy.h) - never a PLL re-lock
    # under a running radio, checked for every state, and the 2026-09-25 bench day replayed.
    test_cpuclock) deps=() ;;
    # Header-only: may the WiFi station come back, and how (wifi_policy.h) - never over the
    # owner's "off", never under a game or a live hotspot, all 256 input states swept.
    test_wifi_policy) deps=() ;;
    # Header-only: a call's RTP clocks (rtp_watch.h) - far-end silence counted PER CALL (the old
    # boot-long rule, reproduced, ends the second call of a boot at connect) and the hot-mic
    # backstop that releases an RTP session no live call owns.
    test_rtpwatch) deps=() ;;
    # The mesh-history replay wire format vs vectors generated from COVEY's
    # replay.py (regenerate with tools/gen_replay_vectors.py after changes).
    test_replay)   deps=(WiPhone/replay_proto.cpp) ;;
    # NeighborInfo encoding vs the real protobuf runtime's bytes
    # (regenerate with tools/gen_neighbor_vectors.py).
    test_neighbor) deps=(WiPhone/neighbor_info.cpp) ;;
    # The Data/User protobuf codec and the on-air header, vs bytes from the real
    # protobuf runtime (regenerate with tools/gen_wire_vectors.py after an upstream bump).
    test_wire)     deps=(WiPhone/mesh_wire.cpp WiPhone/mesh_hash.cpp) ;;
    # LoRa time-on-air at the phone's registers (hand-worked AN1200.13 references) and the
    # text budget of one frame, measured with the shipping meshBuildData — the numbers
    # MeshPhy::send's timeout and the compose cap are built from.
    test_airtime)  deps=(WiPhone/mesh_airtime.cpp WiPhone/mesh_wire.cpp) ;;
    # The radio's send queue (mesh_txq.cpp): FIFO own frames with ACKs ahead of them, relays only
    # when due and cancelled on a duplicate, one start per pass. It replaced a transmit that held
    # the superloop for the frame's whole time on air - the 0.6-1.5 s 'mesh' STALL lines.
    test_txq)      deps=(WiPhone/mesh_txq.cpp) ;;
    # Compiles the REAL helix decoder so a pass proves the bytes that ship are the bytes
    # that decode. helix is C and is listed in csrc, not deps — see CFLAGS above.
    test_mp3)      deps=(WiPhone/mp3_stream.cpp)
                   csrc=(WiPhone/src/audio/helix-mp3/*.c) ;;
    # Header-only: the Game Boy cartridge arithmetic (gnuboy/gb_romsize.h). gnuboy.c itself
    # cannot be compiled here (esp_heap_caps.h, hw.h, cpu.h, sound.h, lcd.h), which is why the
    # arithmetic lives in a header both it and this suite include.
    test_gbrom)    deps=() ;;
    # Header-only: the scrolling selected menu row (WiPhone/menu_marquee.h) — its phase clock
    # and the UTF-8 glyph stepping. The drawing itself needs the LCD and stays on the phone.
    test_marquee)  deps=() ;;
    # Header-only: a display-only menu note broken into rows that fit (WiPhone/menu_wrap.h).
    test_wrap)     deps=() ;;
    # Header-only: the Files app's folder copy/move/delete path questions (files_paths.h).
    test_filepaths) deps=() ;;
    # Header-only: the notification pop's stop timer (notify_timing.h) against the shipping
    # pop_pcm[] bytes. WiPhone.ino cannot be compiled here, same reason as above.
    test_notify)   deps=() ;;
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
  # ${deps[@]+...}: macOS bash 3.2 + set -u calls an EMPTY array unbound; the
  # guard is the same one csrc/objs/extra already wear. First hit by test_chunk,
  # the first header-only suite (its whole subject compiles from the .h).
  "$CXX" "${FLAGS[@]}" -o "$OUT/$name" "$src" ${deps[@]+"${deps[@]}"} ${objs[@]+"${objs[@]}"} ${extra[@]+"${extra[@]}"}
  if ! "$OUT/$name"; then
    fail=1
  fi
done

# ── SOURCE GUARD: no menu row may be added with a key of 0 ────────────────────────────────
# See tests/check_menu_keys.py. It replaced a one-line `git grep` that would NOT have caught
# the bug it was written for: the original fault passed a NAMED CONSTANT (ROW_INERT = 0), and
# the grep needed a literal zero on the same line as `addOption(`.
echo "checking for menu rows with a key of 0"
if ! python3 tests/check_menu_keys.py; then
  fail=1
fi

# ── SOURCE GUARD: nothing but Networks.cpp brings the WiFi station back by itself ─────────
# See tests/check_wifi_restore.py. Every site that gives the radio back goes through
# wifiRestoreStation(); a bare esp_wifi_start()/WiFi.reconnect()/WiFi.begin()/WiFi.mode(WIFI_STA)
# elsewhere is how WiFi came back on behind "off" after every Game Boy game (0.9.79).
echo "checking that WiFi is only given back through wifiRestoreStation()"
if ! python3 tests/check_wifi_restore.py; then
  fail=1
fi

# ── SOURCE GUARD: the call-audio guards of 0.9.79 stay where they are ─────────────────────
# See tests/check_call_audio.py. test_rtpwatch proves rtp_watch.h, but the fixes that USE it -
# newCall()'s per-call begin, END's gui.inCall() gate and its one way out of CallState::Error,
# F1/F2 kept off a call, the ring's pop-then-music-then-loudspeaker order, the loop yield held
# under a pop, the backstop held for the motor, Settings > Audio through music's stash, the
# **202## mic egg compiled out - live in files this suite cannot compile. A review reverted six
# of them in a scratch copy and the suite stayed green; each is a positive contract now.
echo "checking the call-audio guards (END, F1/F2, ring order, RTP silence, hot-mic backstop)"
if ! python3 tests/check_call_audio.py; then
  fail=1
fi

# ── SOURCE GUARD: the LoRa radio never waits for the air on the loop task ─────────────────
# See tests/check_mesh_tx.py. test_txq proves the queue; the guards that make it safe - no blocking
# send, healthCheck() true mid-frame, TxDone before the deadline, txPump() first in loop(), the
# background senders' idle gates - live in files this suite cannot compile.
echo "checking the mesh transmit guards (no blocking send, pump first, health mid-frame)"
if ! python3 tests/check_mesh_tx.py; then
  fail=1
fi

# ── THE MAC-SIDE TILE CONVERTER, against what the phone actually reads ────────────────────
# tools/convert_tiles.py runs on a computer this suite will never see, and two of the three
# things it can get wrong are SILENT on the phone: a byte-swapped RGB565 tile still draws a
# recognisable map in wrong colours, and the sips/BMP fallback has three chances to be subtly
# off. See tests/check_convert_tiles.py. Pillow is optional.
echo "checking the tile converter against the firmware's pixel format"
if ! python3 tests/check_convert_tiles.py; then
  fail=1
fi

# ── THE COVEY PULL, against a fake COVEY in a temp dir ─────────────────────────────────────
# tools/covey_pull.py feeds the pool the phones are built from; the two ways it can go wrong
# silently are overwriting a master tile and pooling COVEY's STREAMED tiles as if they were
# downloaded. See tests/check_covey_pull.py (no ssh, no network).
echo "checking the COVEY pull"
if ! python3 tests/check_covey_pull.py; then
  fail=1
fi

exit "$fail"
