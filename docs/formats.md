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

## Not yet decoded (images)

- `LAND2C.PAK` — likely the CGA version of `LAND.PIC` (begins with what look
  like CGA dither patterns: `0000 5555 aaaa ffff`).

## Executables

`DGAME.EXE`, `DSTART.EXE`, `DSU.EXE` and `DEND.EXE` are Microsoft **EXEPACK**
compressed (Microsoft C 1988 runtime). `tools/unexepack.py` restores them;
unpacked copies go in `build/unpacked/`. Addresses below refer to the unpacked
`DGAME.EXE`: load image starts at file offset 0x480, DGROUP (DS) is image
segment 0x2305 (file offset 0x480 + 0x23050), and the hand-written 3D engine
lives in image segment 0x1000.

Theatre file stems are in DGROUP: `regn`, `lb`, `pg`, `nc`, `ce` (`.xxx`
placeholders; extensions are substituted at load time). The demo ships only
`NC` (North Cape).

## .3D3 — shape library

Loaders: `FUN_1000_098a` (theatre file + `photo.3d3` subset) and
`FUN_1000_c966` (`STFLT.3D3`, aircraft/weapons). Parser:
`tools/shape3d.py`; preview: `tools/render_shapes.py`.

```
u16 magic ("33")
u16 n
u16 offset[n]          relative to the data start
u16 data_length
u8  data[data_length]  shapes
-- theatre files only (trailer, the shared vertex table):
u8  m                  (0 = no trailer)
u8  xi[m], yi[m], zi[m]
u8  kx; s16 X[kx]      shared vertex i = (X[xi[i]], Y[yi[i]], Z[zi[i]])
u8  ky; s16 Y[ky]
u8  kz; s16 Z[kz]
```

Shape:

```
u8  size_class                    indexes cull/LOD distance tables
{ u8 0x80|level; s16 rel } *      LOD: if farther than threshold[level],
                                  jump to (address of rel) + rel (may point
                                  into another shape's data); repeat
body
```

Body — first byte decides the kind (after an optional prefix byte with
`(b & 0x60) == 0x60`, whose low 2 bits select an axis snapped to ground):

- `0x3F cc` — single point at the origin, colour `cc`.
- `0x3E xx n idx[n]` — n ground dots from the shared vertex table (`xx` is
  skipped). Drawn white, EGA 7 beyond camera depth 0x9C4, EGA 8 beyond 0x1388:
  ground-texture speed cues. Level-1 tile type 0 places one (shape 15) in most
  level-1 tiles.
- otherwise a mesh:

```
u8  np (low 5 bits; bit 6 = draw-order flag)
{ s16 nx, ny, nz, d } [np]        face planes; plane visible if
                                  n·eye > d. Builds a visibility bitmask.
u8  nv                            bit 7 set: shared-table vertices
  { mask; s16 x, y, z } [nv]        own vertices, or
  { mask; u8 index } [nv & 0x7F]    shared vertices
u8  ne;  { mask; u8 v0, v1 } [ne] edges
u8  nprims  (0xFF = plane-sorted mode, see below)
prims:
  poly: u8 b (b&3 == 1, plane = b>>3); u8 n; u8 edge[n]; u8 colour
  line: u8 b (b&3 != 1); mask; u8 edge; u8 colour
```

`mask` is u16, or u32 when np > 16; an element is processed only if it
shares a bit with the visible-plane mask. Colour 255 = not drawn; others
are 0–15, remapped at runtime through a table at DGROUP 0x8de (EGA indices
render correctly as-is). Coordinates: x, y horizontal, z up; terrain tiles
span ±2048.

Plane-sorted mode (`nprims == 0xFF`): `2*np` bytes of sort data, 1 byte,
`u16 offset[np]`, `u8 count[np]`, then primitive groups (per plane) at
base + offset.

All 118 shapes in `NC.3D3`, `STFLT.3D3` and `PHOTO.3D3` parse to exactly
their table lengths.

## .3DG — terrain tile hierarchy

Loader `FUN_1000_0e2a`, lookup `FUN_1000_0848`. Parser: `tools/world.py`.

```
u16 magic ("22")
u8  unused[16]           read then overwritten
u8  top[16*16]           level-3 tile types
u8  expand3[32][4*4]     level-3 type -> 4x4 level-2 types
u8  expand2[32][4*4]     level-2 type -> 4x4 level-1 types
u8  expand1[32][4*4]     level-1 type -> 4x4 level-0 types (all 0 in NC)
```

Level 4 is an 8x8 base map per theatre in DGROUP at 0x758 (64 bytes each),
inner 4x4 = types 0–15, border = filler: `0x10` land (shape 0), `0x11` sea
(shape 18). Lookups at level 4 add (2, 2). Which map belongs to which theatre
is chosen at runtime (`[0x6580]+0x38`); for NC, map 2 is the best fit — its
border continues the coastline on 13.5 of 16 edge segments (map 0: 12.5,
map 3: 10, map 1: 5.5). Evidence, not proof.

## .3DT — objects per tile type

Loader `FUN_1000_0cc8`.

```
u16 magic ("11")
u16 types[5]                     per level 0..4 (32 each in NC)
u16 count[level][types[level]]
{ s16 x, y, z; u16 shape } ...   per level, per type; shape low byte used;
                                 shape bit 7 = state-dependent (destroyable)
```

Positions are relative to the tile centre, in units where a tile is 0x1000.

### Placement (from `FUN_1000_03d0`, the terrain draw loop)

For each level the camera's 32-bit world position is rescaled
(`FUN_1000_07c6`: level 0 ×2, 1 ×1, 2 ÷4, 3 ÷16, 4 ÷64, rounded), then split
into tile index (`>> 12`) and position within the tile (`& 0xFFF`). So a tile
spans 0x1000 world units at level 1, 0x4000 at level 2, 0x10000 at level 3
and 0x40000 at level 4; the level-3 world is 16 × 0x10000 square. Grid sizes
per level (DGROUP 0x4ec) are 1024, 256, 64, 16, 8.

A tile's objects are drawn at `tile_centre + (x, y, z)`. Grid column and
object x both increase with world X; grid row and object y both increase
with world Y. World +Y appears to be north: drawn that way, the North Cape
map has open sea to the north (`tools/render_map.py`, output north-up).

Level 0's ×2 scale gives a 0x800 tile, which does not fit the 4× hierarchy
used elsewhere; NC has no level-0 data, so this is unresolved.
## glTF export

`tools/export_gltf.py` writes `.glb` files (`out/gltf/`, not tracked):

- `shapes FILE.3D3 OUT.glb` — one mesh + node per shape, in a row.
- `terrain GAMEDIR NC OUT.glb` — whole theatre: ~75k nodes instancing the
  theatre's 79 shape meshes, grouped by level.

Game (x east, y north, z up) → glTF (X, Y, Z) = (x, z, −y); raw game units;
level-L objects scaled 4^(L−1). Polygons are fan-triangulated and wound to
match their face-plane normals; faces of plane-less shapes (two in
`STFLT.3D3`) use double-sided material copies (materials 16–31). Lines and
light points export as LINES/POINTS. Unlit EGA materials. Only each shape's
own body is exported; LOD links are in node `extras`.

`tools/check_glb.py FILE.glb [OUT.png]` validates structure and renders a
top-down view from the glTF alone (it matches `render_map.py`).

Polygons are fan-triangulated; all 1362 in the demo are convex, so this is
exact. Mesh extras carry `shape` and `kind` (`mesh`, `point`, `lights`).

Rendering notes (implemented in `viewer/index.html`):
- **Draw order.** Mesh primitives keep the engine's order. Flat shapes stack
  coplanar polygons and the engine relies on painter's order: draw flat
  geometry level 4 → 1, then by primitive index, without depth writes.
- **Draw distance.** At each level the engine draws only 9 tiles: a 3×3
  block reaching two tiles ahead of the camera, chosen from 8 heading sectors
  (DGROUP 0x332; 3×3 centred table at 0x4b2/0x4c4 when looking steeply down),
  far to near. Only level 4 covers the world. Drawing every level everywhere
  buries the ground under ~450k ground dots (level-1 tile type 0 → shape 15).

## .WLD — world sites (+ mission tables, place names)

Not opened by name in the demo: its contents travel inside the game-state
block (see REPLAY.DTA, offset 0x7a). Parser: `world.load_wld`.

```
"BN" u16 46, 18, 32, 0, 0, 0 ...      header (counts, meaning TBD)
from 0x38, 16-byte site records until type == 0:
  u16 id, x, y, status, a, b, c, type
```

Positions are world / 32. **WLD y increases southwards**: world
`(X, Y) = (x * 32, 0x100000 - y * 32)`. Verified on the map: every site lands
on its own terrain feature cluster; the other orientation puts many sites
in open sea. Player start (`FUN_1000_4cf4`) takes a site record from a
16-byte table at DGROUP 0x7fea and uses x << 5 and −(y + 0x8000) << 5, which
is consistent with these units.

Types seen: 0x124 airfield (16), 0x12b one special airfield/carrier,
0x113 site 0x80 east of an airfield (SAM/radar?), 0x11a target,
0x146/0x148/0x149 other. After the records: mission tables, a terrain-ish
byte grid, then a place-name string table (Severomorsk, Kandalaksha,
Gällivare, Luleå, Nikel, Ristikent, ...). Undecoded beyond the records.

## REPLAY.DTA / STREAM.DTA — the recorded demo

Saved by `FUN_1000_4fde` when Insert is pressed while recording; loaded by
`FUN_1000_5012`. Decoder: `tools/replay.py`.

**REPLAY.DTA** (0x1400 bytes) is a snapshot of the shared game-state block
(far pointer at DGROUP 0x9bc6): driver file names, mission/state words, and
at 0x7a a full WLD block as modified by the mission (site status fields
changed; mission-generated units in previously empty records). Playback
restores it, keeping block words 0x1a–0x20.

**STREAM.DTA** (16000 bytes, far buffer 0x1ede:0x3e8 in the image) is
recorded input, not positions. Three channels share one sequence of 4-byte
records `(s16 value, s16 count)`:

- A channel holds `value` for `count + 1` reads, then takes the next record
  in the sequence. The recorder reserves a record when a run starts, so
  records appear in run-start order; records 0–2 are each channel's first
  run (`FUN_1000_ddae`). Stored count is `count + (value >> 15)`.
- Read order per frame (main loop `FUN_1000_1b8c`):
  1. ch 0: key code, BIOS `scan << 8 | ascii` (`FUN_1000_200a`)
  2. ch 1: joystick, high nibble X, low nibble Y, 8 = centre
  3. ch 2: fire button, only when frame counter [0x5442] is odd
     (`FUN_1000_4874`); the counter then increments
  4. ch 2: button 1 (`FUN_1000_d298`, every frame)
- The counter starts at `random & 0x7ff8` (even, `FUN_1000_4ab6`).

Decoding with an even start uses 2599 records for 4737 frames, ends exactly
on the Insert key (0x5200) that stopped recording, and gives zero
implausible values; the odd start fails on 1639 values. The remaining 1401
records are stale buffer contents. The pilot used keypad flight keys
(8/2/6), F-key views, `+` (full throttle), 28 frames of fire, 7 of button 1.

To turn this into a flight path the flight model has to be reimplemented
(integration is in `FUN_1000_200a` around the `0x8d68`/`0x9388`/`0x45fe`
updates). The starting airfield index (`[0x86d8]`) is not yet located in
the snapshot.
