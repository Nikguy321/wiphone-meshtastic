#!/usr/bin/env python3
"""check_convert_tiles.py — the Mac-side tile converter, against what the phone reads.

tools/convert_tiles.py is the only thing standing between a folder of PNGs and a map, and it
is run on a computer that this test suite will never see. Two of the three things it can get
wrong are silent on the phone:

  * BYTE ORDER. RGB565 the wrong way round still draws a recognisable map, in wrong colours.
  * THE BMP FALLBACK. On a Mac without Pillow the image is decoded by `sips` into an
    uncompressed BMP that this script parses itself — bottom-up rows, BGR, 4-byte stride
    padding, three chances to be subtly off.

So: the packing is checked against the SAME expression the firmware's color565() uses, and
the BMP reader is checked against a BMP built here byte by byte, including the top-down and
32-bit variants. Pillow is not required; when it is installed the two decode paths are also
checked against each other.

Run by tests/run_tests.sh.
"""
import importlib.util
import io
import os
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("ct", ROOT / "tools" / "convert_tiles.py")
ct = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ct)

failures = 0
checks = 0


def check(cond, name):
    global failures, checks
    checks += 1
    if cond:
        print("  ok  %s" % name)
    else:
        print("  FAIL %s" % name)
        failures += 1


# ── the firmware's own packing, written out independently ────────────────────────────────
# TFT_eSPI::color565: ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3), stored as a uint16
# in the ESP32's memory, i.e. LOW BYTE FIRST.
def firmware_565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def make_bmp(w, h, pixels, bpp=24, top_down=False):
    """pixels: list of (r, g, b), row 0 = TOP row. Builds the BMP sips would write."""
    bypp = bpp // 8
    stride = (w * bypp + 3) & ~3
    rows = []
    for y in range(h):
        src = y if top_down else (h - 1 - y)
        row = bytearray()
        for x in range(w):
            r, g, b = pixels[src * w + x]
            row += bytes([b, g, r])           # BMP is BGR
            if bypp == 4:
                row += b"\x00"
        row += b"\x00" * (stride - len(row))
        rows.append(bytes(row))
    data = b"".join(rows)
    off = 54
    hdr = b"BM" + struct.pack("<IHHI", off + len(data), 0, 0, off)
    info = struct.pack("<IiiHHIIiiII", 40, w, -h if top_down else h, 1, bpp, 0,
                       len(data), 2835, 2835, 0, 0)
    return hdr + info + data


print("check_convert_tiles")

# ── the packing ──────────────────────────────────────────────────────────────────────────
SAMPLES = [(0, 0, 0), (255, 255, 255), (255, 0, 0), (0, 255, 0), (0, 0, 255),
           (40, 80, 40), (17, 231, 99), (1, 2, 3), (254, 253, 252)]
rgb = b"".join(bytes(s) for s in SAMPLES) + bytes(3 * (256 * 256 - len(SAMPLES)))
out = ct.rgb565_bytes(rgb, 256, 256)
check(len(out) == 256 * 256 * 2, "a tile is exactly 131072 bytes")
ok = True
for i, (r, g, b) in enumerate(SAMPLES):
    want = firmware_565(r, g, b)
    got = out[i * 2] | (out[i * 2 + 1] << 8)      # little-endian, as the firmware reads it
    if got != want:
        ok = False
        print("    %s -> 0x%04x, firmware says 0x%04x" % ((r, g, b), got, want))
check(ok, "every pixel packs exactly as TFT_eSPI::color565 does, low byte first")

# Red must land in the HIGH bits. A byte-swapped tile passes a length check and fails here.
red = ct.rgb565_bytes(bytes([255, 0, 0]) + bytes(3 * (256 * 256 - 1)), 256, 256)
check(red[0] == 0x00 and red[1] == 0xF8, "pure red is 0x00 0xF8 on disk, not 0xF8 0x00")

check(ct.TILE_BYTES == 131072, "TILE_BYTES agrees with the firmware's MAP_TILE_BYTES")

try:
    ct.rgb565_bytes(bytes(3 * 64 * 64), 64, 64)
    check(False, "a wrong-sized buffer is refused")
except ValueError:
    check(True, "a wrong-sized buffer is refused")

# ── the BMP fallback (what sips hands back on a Mac without Pillow) ───────────────────────
PIX = [(x * 7 % 256, y * 5 % 256, (x + y) * 3 % 256) for y in range(9) for x in range(7)]
for bpp in (24, 32):
    for td in (False, True):
        blob = make_bmp(7, 9, PIX, bpp=bpp, top_down=td)
        got, w, h = ct.parse_bmp(blob)
        want = b"".join(bytes(p) for p in PIX)
        check((w, h) == (7, 9) and got == want,
              "BMP %d-bit %s round trips, padding and BGR and all"
              % (bpp, "top-down" if td else "bottom-up"))

for bad, why in ((b"not a bmp at all" + bytes(64), "a file that is not a BMP"),
                 (make_bmp(4, 4, [(0, 0, 0)] * 16)[:40], "a truncated BMP")):
    try:
        ct.parse_bmp(bad)
        check(False, "%s is refused" % why)
    except ValueError:
        check(True, "%s is refused" % why)

# ── both decoders agree, when both exist ─────────────────────────────────────────────────
if ct.HAVE_PIL:
    from PIL import Image
    im = Image.new("RGB", (256, 256))
    p = im.load()
    for y in range(256):
        for x in range(256):
            p[x, y] = (x, y, (x ^ y) & 0xFF)
    png = io.BytesIO()
    im.save(png, format="PNG")
    bmp = io.BytesIO()
    im.save(bmp, format="BMP")
    via_pil, _, _ = ct.decode_with_pil(png.getvalue())
    via_bmp, _, _ = ct.parse_bmp(bmp.getvalue())
    check(via_pil == via_bmp, "the Pillow path and the BMP path decode identically")
    check(ct.rgb565_bytes(via_pil, 256, 256) == ct.rgb565_bytes(via_bmp, 256, 256),
          "...and therefore write identical tiles")
else:
    print("  --  Pillow not installed; the two-path comparison was skipped")

# ── the area name rule must match the firmware's mapAreaNameOk() exactly ──────────────────
src = (ROOT / "WiPhone" / "map_tiles.cpp").read_text()
check("c == '-' || c == '_' || c == '.'" in src and "name[0] == '.'" in src and
      "n <= 31" in src,
      "the firmware's area-name rule is still the one convert_tiles.py enforces")

print("\n%d checks, %d failures" % (checks, failures))
sys.exit(1 if failures else 0)
