"""Decode MicroProse .PIC/.SPR images to PNG.

Format: byte 0 = header (0x0B = max LZW code width for 16-colour files),
then LZW (see mpslzw), then RLE (0x90 escape). 16-colour files decode to
32000 bytes (320x200, two pixels per byte); 256-colour to 64000 bytes.
"""
import sys
from pathlib import Path

import mpslzw
import png

EGA = [(0, 0, 0), (0, 0, 170), (0, 170, 0), (0, 170, 170), (170, 0, 0), (170, 0, 170),
       (170, 85, 0), (170, 170, 170), (85, 85, 85), (85, 85, 255), (85, 255, 85),
       (85, 255, 255), (255, 85, 85), (255, 85, 255), (255, 255, 85), (255, 255, 255)]


# 256-colour (MCGA) palette: 6-bit VGA DAC triples in DGAME.EXE's data.
# Provisional: found by scanning, not yet confirmed by disassembly. The first
# five bytes at this offset are not palette data, so entries 0-1 use EGA values;
# only entries 0-107 are meaningful.
VGA_PAL_FILE, VGA_PAL_OFFSET = "DGAME.EXE", 0x13724


def vga_palette(gamedir):
    d = (Path(gamedir) / VGA_PAL_FILE).read_bytes()[VGA_PAL_OFFSET:VGA_PAL_OFFSET + 768]
    pal = [tuple(min(c, 63) * 255 // 63 for c in d[i * 3:i * 3 + 3]) for i in range(256)]
    pal[0:2] = EGA[0:2]
    return pal


def decode(data):
    raw = mpslzw.rle_decode(mpslzw.lzw_decode(data, 1, 11))
    if len(raw) == 32000:
        return bytes(p for b in raw for p in (b & 15, b >> 4))
    return raw


def main(out_dir, files):
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for f in files:
        pix = decode(Path(f).read_bytes())
        pal = EGA + [(0, 0, 0)] * 240 if max(pix) < 16 else vga_palette(Path(f).parent)
        png.write_indexed(out / (Path(f).name + ".png"), 320, 200, pix, pal, 2)
        print(f, len(pix), "max index", max(pix))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:])
