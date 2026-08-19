#!/usr/bin/env bash
# Stage the current PlatformIO build into webflasher/ and write its manifest.
#
# Run after `pio run`. The four parts and their offsets are the standard ESP32 layout this
# board flashes with (the same offsets `pio run -t upload` uses):
#   0x1000   bootloader (dio/80m — matches board_build.flash_mode/f_flash in platformio.ini)
#   0x8000   partition table
#   0xe000   boot_app0 (OTA data — selects the app slot)
#   0x10000  the application
#
# ⚠ Committing every build's binaries would bloat the repo forever. Commit them when a
# release is CUT, or (better, at go-live) publish binaries as GitHub Release assets and
# point the manifest at those URLs — see docs/webflasher-plan.md.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=.pio/build/wiphone
FRAMEWORK="$HOME/.platformio/packages/framework-arduinoespressif32"
OUT=webflasher

VER=$(sed -n 's/#define FIRMWARE_VERSION "\(.*\)"/\1/p' WiPhone/config.h)
[ -n "$VER" ] || { echo "cannot read FIRMWARE_VERSION"; exit 1; }
[ -f "$BUILD/firmware.bin" ] || { echo "no build - run pio run first"; exit 1; }

# ⚠ ONE MERGED IMAGE, patched by esptool itself — not the four raw parts. This is the
# lesson of the first field test (black screen, boot loop): `pio upload` runs esptool
# with --flash_size detect, which PATCHES THE BOOTLOADER HEADER on the wire to 16MB; the
# raw SDK bootloader says 4MB, and a 4MB-believing bootloader rejects the 16MB partition
# layout and reset-loops. merge_bin applies exactly the same header patching (and fixes
# the appended hash), so the browser writes what the cable would have written.
ESPTOOL=$(ls -d "$HOME/.platformio/packages/tool-esptoolpy"*/esptool.py | head -1)
python3 "$ESPTOOL" --chip esp32 merge_bin   --flash_mode dio --flash_freq 80m --flash_size 16MB   -o "$OUT/wiphone-merged.bin"   0x1000 "$FRAMEWORK/tools/sdk/bin/bootloader_dio_80m.bin"   0x8000 "$BUILD/partitions.bin"   0xe000 "$FRAMEWORK/tools/partitions/boot_app0.bin"   0x10000 "$BUILD/firmware.bin"
rm -f "$OUT/bootloader.bin" "$OUT/partitions.bin" "$OUT/boot_app0.bin" "$OUT/firmware.bin"

cat > "$OUT/manifest.json" <<EOF
{
  "name": "WiPhone",
  "version": "$VER",
  "new_install_prompt_erase": false,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [
        { "path": "wiphone-merged.bin", "offset": 0 }
      ]
    }
  ]
}
EOF

echo "staged $VER:"
ls -la "$OUT"
