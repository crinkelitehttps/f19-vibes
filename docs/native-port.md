# Native port — architecture and plan

Goal: F-19 running natively on Linux, with an option for higher resolutions.
Approach (decided): **hybrid**, in **C++20** (SDL3 + OpenGL, CMake).

The original stage programs run unmodified inside a native host: a 16-bit
x86 interpreter plus high-level emulation (HLE) of the DOS/BIOS services and
hardware the game actually uses, and a **native implementation of the
game's driver API** in place of `MGRAPHIC.EXE` & co. High-resolution 3D then
comes from intercepting the engine's draw entry points; game logic is
replaced with native code piece by piece, each piece differential-tested
against the interpreted original.

No MicroProse code or data ships with the port; it loads the user's files
(`fullgame/`) at runtime.

## How the original is put together

`F19.COM` (loader):

1. Shrinks itself, allocates the shared game-state block and stores its
   segment at `0000:04F0` (BIOS inter-application area). Every stage finds
   shared state there.
2. Runs `SU.EXE` (setup, driver selection) with the command line; continues
   only if its exit code is `0x24`.
3. Loads the chosen graphics driver (name from the shared block) and stores
   its segment in the block (`+0x1c`); loads `MISC.EXE` and others.
4. Runs `START.EXE` (menus, briefing), `EGAME.EXE` (flight), `END.EXE`
   (debrief), with `DS.EXE` between stages.

(`missions.EXE` and `cv.exe` strings in this copy's `F19.COM` are unused.)

### The driver API

Every stage's data segment starts (DGROUP+0x8c in the demo) with a table of
`JMP FAR 0000:0000` stubs — slot *n* at `0x8c + 5n`. At startup the stage
patches the stubs from each loaded driver's export table
(`FUN_1000_025c` in the demo):

```
driver load image (after the 0x200-byte MZ header):
  +0x18  u16   segment adjustment: entry = (load_seg + adj) : offset
  +0x1c  u8    first stub slot
  +0x22  u16   count
  +0x24  u16   offset[count]
```

| Driver | Slots | Entries |
|---|---|---|
| `CGRAPHIC`/`EGRAPHIC`/`MGRAPHIC`/`TGRAPHIC`/`NGRAPHIC` | 0–83 | 84 (75 distinct in MGRAPHIC) |
| `MISC` | 90–95 | 6 |
| `BSOUND`/`ISOUND`/`TSOUND` | 100–108 | 9 |
| `SCENERY0` | 0–139 (own table) | 140 |

All full-game stages (`START`, `EGAME`, `END`, `DS`) have the same stub
layout. **No stage writes video memory (A000) itself** — only the graphics
drivers do — so a native driver can own all drawing.

### MGRAPHIC slot map (reverse engineered; native in `native/src/drivers/mgraphic.cpp`)

Driver data segment D = load segment; code segment C = relocated header
word +0x18. State: `C:019C` draw segment, `C:019E` origin offset,
`C:01A0` flip flag, `C:000C` row-offset table (200 words), `C:067F` page
table (page 0 = A000), `D:1B6E..1B78` line state, `D:1B7A` colour,
`D:1C44..1C50` text state. Fonts: `D:00E2` width-table ptrs, `D:00EE`
glyph-data ptrs, `D:0106` fixed width (FF = proportional), `D:00FA`
heights, `D:0112` row offsets. "reg" = register-call entry.

| Slot | Function |
|---|---|
| 0 | page memory: arg 0 -> page 0 seg; else DOS-allocate a 64000-byte page |
| 1, 2, 3, 4, 6 | text (reg: BX string, BP param block): vertical / left / right / no / all clipping |
| 5, 7, 8, 10, 9 | text wrappers (stack: param block, string) for 4, 1, 2, 3, 6 |
| 11 | vertical bar gauge (reg) |
| 12 / 13 / 14 | draw target = page 1 / page AX / page arg |
| 15 / 16 | set / get draw segment (reg AX) |
| 17,73 / 18,74 | sprite blit, colour 0 transparent (stack ptr / reg BP param block) |
| 19,71 / 20,72 | clipped sprite blit (stack ptr / reg BP) |
| 21, 22, 34, 35, 53-55, 61, 80-83 | no-ops |
| 23, 49, 66, 67, 76-78 | constants (FA00, FA00, 0, 1, 1, 1, 0) |
| 24,25 / 26 / 27 / 30 | origin = 0 / arg / AX / get |
| 28 / 29 / 63 | constants 5580 / 1950 / 3 |
| 31 | line AX,BX -> CX,DX (reg), current colour |
| 32 / 33 | colour = AH / arg |
| 36 | plot current point |
| 37, 40 | polygon spans rows AX..CX, SS:BX left / +1B8 right tables |
| 38,64 / 39,65 | set D:00CC / D:00CE (reg / arg) |
| 41 | replace colour in rectangle |
| 42 | copy rectangle between pages |
| 43 / 59 | clear screen A000 / clear ES (64000 bytes) |
| 44 | flip: copy page 1 -> page 0, set flip flag |
| 45 | get flip flag |
| 46 | palette flicker (9 DAC entries) + CRTC start-address screen shake |
| 47 | character width (font, char) |
| 48 | copy 64000 bytes from segment arg to draw segment |
| 50 | largest free DOS block |
| 51, 52 | copy 320 bytes SS:BP -> ES:DI |
| 56 / 57, 75 / 58 / 62 | ES = page[SI] / set page entry / AX = row[DI] / AX = row[y]+x |
| 60 | set mode 13h (exit with message on failure) |
| 68 | set 16-colour palette set via INT 10h/1012h |
| 69 / 70 | screen off / on at vertical retrace |
| 79 | set screen-shake counter |

Status: all slots except 0, 50, 60, 68 (DOS/BIOS calls; still original
code) are native. Verified against the original on every call through the
self-running demo flight: 788,936 calls, 0 mismatches (memory written and
all registers; free stack space below SP excluded). Slots 7-10 and some
blit variants are only exercised by the menus: run
`f19 GAMEDIR --verify-driver` and read the report on exit.

### DOS/BIOS/hardware surface (static census; to be confirmed by tracing)

- INT 21h: 09 0B 0E 19 1A 25 2A 2C 30 35 3C 3D 3E 3F 40 41 42 43 44 48 49
  4A 4B 4C 4D 4E — console string, files, find-first, memory blocks, EXEC,
  vectors, date/time, version.
- INT 10h: mode set / palette / teletype (stages), mode/palette in drivers.
- INT 16h keyboard, INT 1Ah timer, INT 13h disk (`SU`, `DS` — likely copy
  protection or disk detection).
- Hooked vectors: 08h (timer), 1Bh (Ctrl-Break), 00h (divide error).
- Ports: PIT 40h/43h (timer reprogrammed), PIC 20h, keyboard 60h,
  joystick 201h, VGA status 3DAh (retrace), sequencer 3C4h and A000 writes
  (drivers only), PC speaker (sound drivers).

## Milestones

### 1. Original code, native host, 320×200

- `x86` interpreter: 8086/80186 (+286 real-mode if traced), 1 MB address
  space, segment arithmetic; far-call traps for HLE.
- DOS HLE: memory-control blocks, PSP, EXEC (load MZ/COM with relocations,
  run child, return code), file handles mapped to a game directory
  (case-insensitive), find-first/next, vectors, date/time.
- Hardware HLE: PIT/IRQ0 at the programmed rate, keyboard (scan codes from
  SDL into the BIOS buffer and port 60h), joystick port, retrace bit.
- Drivers: first run the **original** `MGRAPHIC`/`*SOUND` inside the
  interpreter against an emulated VGA (mode 13h framebuffer + DAC) to get a
  correct baseline quickly; then replace them with native implementations
  of the 84 + 9 + 6 entries, reverse engineered from `MGRAPHIC.EXE` (10 KB).
- Verification: screenshots against DOSBox; the demo's recorded flight
  (`STREAM.DTA`) as a deterministic regression run.

### 2. High-resolution 3D

Intercept the 3D engine's shape and terrain draw entry points (in the demo:
`FUN_1fe6_082c` shape draw, the terrain loop `FUN_1000_03d0`; to be located
in `EGAME.EXE`) and render the decoded assets (`tools/shape3d.py`,
`world.py`) on the GPU at any resolution, with the original's draw order
and distance rules (see `viewer/`). Composite the 2D layer (cockpit, HUD,
menus): first upscaled, then redrawn at high resolution where it matters.

**Status (implemented):** `native/src/hires/`. `WorldCapture` finds the
engine by byte signature once the flight program has unpacked itself
(`tools/engine_sigs.py` checks the signatures against both executables;
data-segment offsets are read from the matched code, so the demo and full
game share one build). Hooks:

| Hook | Where (demo DGAME, engine-segment offsets as in `build/seg2000.asm`) | Captured |
|---|---|---|
| edge setup | 1000 | edge record -> vertex indices |
| polygon | 16A4 (far call to set colour) | AH colour, edges at ES:SI (n at SI-1) |
| line | 1640 | AH colour, BX edge record |
| dot (3 sites) | 17EE, 18E6, 1997 (call 1AC0) | vertex slot 0, driver colour |
| horizon | 0486 | up vector [9B0..9B6], view mode, altitude, colours |

Camera space: x right, y up, z forward, 32-bit; projection
`x = cx + 256·X/Z`, `y = cy − 192·Y/Z` (Z doubled with the zoom flag,
halved per shift step), near plane Z ≥ 65536; viewport `[3A89]`,`[3A8B]`,
driver draw origin added. The horizon line passes through
`(cx − d·s, cy − 0.75·d·c)` with direction `(c, −0.75·s)`,
`d = 256·[9B0]/[9B2]`, `s = −[9B4]/32768`, `c = [9B6]/32768` (one colour
when `[9B2]` ≤ 0x1F0B; view mode 2 draws sky only).

World tagging: the back page is watched (memory write tags); a native
driver call is "world" when the engine makes it after a capture hook,
until a driver call from elsewhere or an engine 2D entry (C line, rect
fill). At each flip (slot 44) the frame = primitives + page + mask.
`HiresRenderer` draws the primitives in the engine's order with SDL3's
geometry API at window resolution, then the page with world pixels
transparent. Checked against the engine's own output frame by frame
(`f19trace -H out.ppm` writes the hi-res frame, a world-only render, the
engine's page and the mask).

Renderer (`gl_render.cpp`, OpenGL 3.3 core): 8x MSAA; per-vertex depth
`65536/Z` (exact under screen-space interpolation) in a 32-bit float
buffer, larger = nearer. Primitives are grouped per shape instance (hook on
the engine's vertex-transform loop). Flat objects (all vertices coplanar:
ground tiles, decals) keep the engine's painter's order and only test
against solid objects; solid objects draw colour with depth test but no
write (their faces keep the engine's order), then write depth. Where the
engine's sort is right the result is identical to painter's order (checked:
0 differing pixels on a cockpit frame); where it is wrong, solid objects
now occlude correctly. Under WSL, `GALLIUM_DRIVER=d3d12` is set
automatically when the D3D12 bridge exists (GPU instead of llvmpipe).

Frame rate: the game renders as fast as the emulated CPU allows and scales
its physics by frame time (flips per emulated second in the demo: 4 MIPS
~8, 8 ~21, 16 ~43, 32 ~62). The interpreter manages ~50 MIPS here; the
default is 25 MIPS.

Frame pacing: the game locks its frames to the emulated VGA vertical
retrace (in-flight frame intervals: median 14.27 ms = one 70 Hz refresh,
p99 23.8 ms). The host sets the emulated refresh to the display's
(`--vga-hz` to override): at 60 Hz, 93% of demo frame intervals are exactly
one refresh. Emulation runs on its own thread against a real-time clock in
~1 ms slices (time is dropped only if it falls >250 ms behind); the main
thread renders snapshots and paces itself to the refresh, since vsync is
not honoured under WSLg. `F19_PERF=1` prints display/game fps, emulation
load and dropped time once a second.

3D cockpit (`GlRenderer::render_cockpit3d`, cockpit view only — main
viewport shorter than 200 rows): the main view's primitives are rendered
through our own perspective camera (original 64° horizontal FOV at 4:3,
wider windows see more) rotated by a head orientation; sky and ground are
shaded per pixel from the camera-space up vector, reconstructed from the
horizon inputs as (-s·[9B2], c·[9B2], -[9B0]); the page rows below the
viewport (instrument panel, live each frame incl. map and TrackCam) are a
texture on a quad in the aircraft frame (centre (0, -0.22, 0.9), width 1,
top tilted 25° away). The HUD/canopy (top of the page) is not drawn.
Limitation for freelook: the engine only draws objects inside its own
forward view, so looking far aside shows sky/ground but no objects.

View keys (from the key handler): F1 cockpit; Shift+F1..F6 external views
(view variable `[9422]` = 87h, 84h, 85h, 89h, 88h, 8Bh). EGAME's INT 9 handler
dedupes the BIOS key buffer using the AT BIOS bounds at 0040:0080/0082,
which must be initialised.

Not captured yet (stay 320x200): the carrier's wake (drawn through another
path, probably a C-called polygon fill in the engine segment).

Known differences: distant thin geometry (land along the horizon) becomes
sub-pixel slivers at high resolution where the 320x200 engine always
produced at least one pixel.

### 3. Progressive porting

Replace game systems with native C++ one function at a time, starting with
the flight model, each differential-tested against the interpreted
original on recorded inputs. The interpreter remains as a fallback until
nothing calls into it.

## Status and next steps (2026-10-03)

Working:
- `build/native/f19 out/run/native` runs the full game natively: original
  stages in the interpreter, native MGRAPHIC driver (80/84 slots, verified),
  OpenGL renderer, emulation on its own thread (25 MIPS default, VGA refresh
  = display refresh).
- High-res 3D world (MSAA, object depth) via engine capture; cockpit view
  as a 3D scene (panel as a tilted textured quad, HUD hidden); widescreen
  external views; F11 toggles 3D/flat; right-drag mouse look; F12
  screenshots (+ engine frame + primitive dump); F19_PERF=1 timing.
- Headless checks: `build/native/f19trace out/run/demo_native -n 330 -k n1 -H x.ppm`
  (demo flies itself; writes hi-res, world-only, page, mask, 3D renders).

Open / next:
- Freelook beyond ~±51°: the engine culls outside its frustum; needs the
  engine to produce geometry for other directions (e.g. extra engine passes
  with rotated camera, or widening its cull tables at 0x9BBC/0x9BC0).
- Ground colour outside engine geometry is the engine ground colour (green)
  even over sea.
- HUD/canopy in the 3D cockpit: planned as wireframe (capture the HUD's
  driver line/text calls instead of pixels).
- Carrier wake not captured (another fill path); distant thin land slivers.
- Possible residual hitches (user suspects their VNC session); measure with
  F19_PERF=1.
- Sound: PC speaker not emulated. Display modes other than VGA unsupported.
- Long-term: 3D cockpit with instrument bitmaps on modelled screens; head
  tracking via the head yaw/pitch already plumbed through the renderer;
  progressive porting of game logic (flight model first, STREAM.DTA as a
  regression input).
