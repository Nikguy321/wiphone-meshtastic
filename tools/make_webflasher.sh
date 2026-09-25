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
MERGED=$(mktemp -t wiphone-merged)
trap 'rm -f "$MERGED"' EXIT
python3 "$ESPTOOL" --chip esp32 merge_bin   --flash_mode dio --flash_freq 80m --flash_size 16MB   -o "$MERGED"   0x1000 "$FRAMEWORK/tools/sdk/bin/bootloader_dio_80m.bin"   0x8000 "$BUILD/partitions.bin"   0xe000 "$FRAMEWORK/tools/partitions/boot_app0.bin"   0x10000 "$BUILD/firmware.bin"
rm -f "$OUT/bootloader.bin" "$OUT/partitions.bin" "$OUT/boot_app0.bin" "$OUT/firmware.bin" \
      "$OUT/wiphone-merged.bin"

# 🛑 NEVER WRITE OVER NVS. merge_bin pads the gap between the partition table and boot_app0
# with 0xFF — and that gap IS the nvs partition (0x9000, 20 KB). Written from offset 0, the
# one merged image ERASED every user's NVS on every web-flasher install through 0.9.78: the
# mesh identity key (other nodes then flag a MISMATCH and DMs to the phone fail until they
# clear nodes — seen on phone 2 after phone 1 was flashed this way, 2026-09-25), the booksync
# passcode and device name, the KOSync last-move memo, the WiFi driver's calibration. So the
# patched image is cut into two parts that step AROUND nvs (read from this build's own
# partition table, not assumed): the bootloader + table, then boot_app0 + the app. otadata
# (boot_app0) is still written on purpose: it points the bootloader at app0, where the app goes.
python3 - "$MERGED" "$BUILD/partitions.bin" "$OUT" <<'PY'
import struct, sys
merged, table, out = sys.argv[1], sys.argv[2], sys.argv[3]
img = open(merged, "rb").read()
t = open(table, "rb").read()
nvs = otadata = None
for i in range(0, len(t) - 31, 32):
    magic, ptype, sub, off, size = struct.unpack_from("<HBBII", t, i)
    if magic != 0x50AA:
        break
    if ptype == 1 and sub == 0x02:
        nvs = (off, off + size)
    if ptype == 1 and sub == 0x00:
        otadata = (off, off + size)
if not nvs or not otadata:
    sys.exit("partition table has no nvs/otadata entry - refusing to guess the offsets")
if not (0x9000 <= nvs[0] and nvs[1] <= otadata[0]):
    sys.exit("nvs %x-%x is not between the table and otadata - this split no longer fits" % nvs)
parts = [("wiphone-boot.bin", 0x1000, nvs[0]), ("wiphone-app.bin", otadata[0], len(img))]
for name, a, b in parts:
    if a < nvs[1] and b > nvs[0]:
        sys.exit("%s would cover nvs - refusing to stage it" % name)
    open("%s/%s" % (out, name), "wb").write(img[a:b])
    print("  %-18s 0x%06x..0x%06x  %d bytes" % (name, a, b, b - a))
print("  nvs 0x%x..0x%x is in neither part" % nvs)
PY

cat > "$OUT/manifest.json" <<EOF
{
  "name": "WiPhone",
  "version": "$VER",
  "new_install_prompt_erase": false,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [
        { "path": "wiphone-boot.bin", "offset": 4096 },
        { "path": "wiphone-app.bin", "offset": 57344 }
      ]
    }
  ]
}
EOF

echo "staged $VER:"
ls -la "$OUT"
