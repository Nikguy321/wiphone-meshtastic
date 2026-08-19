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

cp "$FRAMEWORK/tools/sdk/bin/bootloader_dio_80m.bin" "$OUT/bootloader.bin"
cp "$BUILD/partitions.bin"                           "$OUT/partitions.bin"
cp "$FRAMEWORK/tools/partitions/boot_app0.bin"       "$OUT/boot_app0.bin"
cp "$BUILD/firmware.bin"                             "$OUT/firmware.bin"

cat > "$OUT/manifest.json" <<EOF
{
  "name": "WiPhone",
  "version": "$VER",
  "new_install_prompt_erase": false,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [
        { "path": "bootloader.bin", "offset": 4096 },
        { "path": "partitions.bin", "offset": 32768 },
        { "path": "boot_app0.bin", "offset": 57344 },
        { "path": "firmware.bin", "offset": 65536 }
      ]
    }
  ]
}
EOF

echo "staged $VER:"
ls -la "$OUT"
