# File formats

## .PIC / .SPR — compressed full-screen images

Used by `TITLE16.PIC`, `LAND.PIC`, `COCKPIT.PIC`, `256PIT.PIC`, `F19.SPR`.
Decoder: `python3 tools/pic2png.py out/pic gamefiles/*.PIC gamefiles/F19.SPR`.

1. **Byte 0**: header. `0x0B` (max LZW code width, 11 bits) in the 16-colour
   files; `0xF5` in `256PIT.PIC`, whose stream otherwise starts byte-identical
   to `COCKPIT.PIC`. Meaning of `0xF5` unknown; decoding with 11 bits works.
2. **LZW** from byte 1: codes LSB-first, starting at 9 bits. Codes 0–255 are
   literals, 256 is reserved (never emitted), new entries start at 257. Width
   grows when the dictionary fills; when full at max width the dictionary
   resets to 257 entries / 9 bits with no explicit clear code.
3. **RLE**: `0x90 n` repeats the previous byte `n-1` more times; `0x90 0x00`
   is a literal `0x90`.
4. **Pixels**, 320×200:
   - 16-colour: 32000 bytes, two pixels per byte, **low nibble first**.
     Default EGA palette.
   - 256-colour: 64000 bytes, one byte per pixel. Palette below.

`F19.SPR` is a sprite sheet in the same format: enemy aircraft at 16
headings, explosion frames, weapon icons, the damage diagram, logo.

### MCGA palette

6-bit DAC triples at `DGAME.EXE` file offset `0x13724` (provisional, found
by pattern search, not yet confirmed in code). The first five bytes there are
not palette data, so entries 0–1 are taken as EGA black/blue. Layout:
0–15 EGA colours, 16–47 white→black grey ramp, 48–63 cyan ramp, 64–94 hue
wheel, 95–107 reds/blues. A second table follows at `0x13869`: the 16 EGA
colours repeated at progressively greyer/lighter shades (purpose TBD —
likely haze/depth shading).

## Not yet decoded

- `LAND2C.PAK` — likely the CGA version of `LAND.PIC` (begins with what look
  like CGA dither patterns: `0000 5555 aaaa ffff`).
