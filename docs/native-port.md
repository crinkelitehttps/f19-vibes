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
until a drawing driver call from elsewhere or an engine 2D entry (C line,
rect fill). Non-drawing calls from elsewhere do not end the run: the timer
interrupt's palette call (slot 46) lands in the middle of engine drawing. At each flip (slot 44) the frame = primitives + page + mask.
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
texture on a quad in the aircraft frame (centre (0, -0.43, 0.9), width 1,
top tilted 25° away; its top edge just below the original viewport, ~15°
down). The HUD (non-world pixels of the viewport rows, full width, minus
the last row, which is the panel's top bevel) is collimated like a real
HUD: drawn at infinity (rotation only, no eye offset, so no stereo
parallax and no shift when leaning), each page pixel in the direction the
engine's projection gives it (tan = (x − cx)/256, (cy − y)/192, times the
zoom divisor). The game's symbology (target boxes, gun cross, tracers)
therefore overlays the world where the engine aims it. The desktop cockpit
view frames it as the original screen: the projection centre sits cy/192
below the top edge (the vertical extent is unchanged), so the HUD and the
panel are both in view. `f19trace -H` also writes `.eyeL`/`.eyeR` renders
3.2 cm either side; the HUD must be identical in both.

Native scene (`scene.cpp`, default in the 3D view; Shift+F11 switches to
the captured primitives): the world is drawn from the game's shape and
terrain data instead of the engine's output, which only covers its own
forward frustum and tiles ahead. Per frame `WorldCapture` records, at the
terrain loop's entry (demo `1000:03D0`, main view only), the camera's world
position and view matrix (`[9B8]`, 9 Q15 words), and builds:

- terrain: every tile within `--terrain-radius` (default 6) tiles of the
  camera at each level 1-4, all directions, from the engine's tile tables
  and per-type object lists in memory (lookup as `FUN_0848`, level
  coordinates as `FUN_07C6`; level 4 repeats the base map's border beyond
  it). State-dependent objects (shape bit 7, e.g. destroyed) use the shape
  the engine last chose for that tile slot (seen at its shape draw entry).
- the main object list (aircraft, ships, vehicles; 0x24-byte records,
  demo `[87AA]-0x16`, count `[95B2]`), placed with the object routine's
  arithmetic (`FUN_CA58`) but without its forward-cone test, so they also
  appear beside and behind; the engine's own draws from that loop are
  skipped. Other dynamic objects (own aircraft in external views, weapons,
  shadows) are taken from the engine's shape draw calls (`FUN_082C`).

Shapes are parsed from memory (`ShapeCache`, by address; shared vertices
from the theatre table in DGROUP). Rendering follows the engine's maths:
camera space `M^T u` with u = (x, z, y), rotated objects `C = D M` with D
from the three angles (`1FE6:1425`), face visibility from the eye in object
space (`-D u`), colours through the remap table plus the night/haze offset
(`[8DC]`, from depth and size class). Level of detail uses the engine's
distance estimate and thresholds (`[8EE]`), divided by our focal length /
256 so models switch at the same on-screen size, times `--lod-detail F`
(default 2; Ctrl+F12 steps 1/2/3/4/6). Terrain is enumerated
`--terrain-radius N` tiles in every direction (default 6) at levels 2-4 and
`--detail-radius N` (default 12; Alt+F12 steps 6/9/12/18/24) at level 1,
whose 4096-unit tiles hold the buildings and sites: the engine's LOD
thresholds are 128 << level shape units (`[8EE]`, shift 0), and most
building shapes switch at level 5-7, so at 1080p (focal ~6x the original's)
their detailed bodies reach 25k-100k units, past the old 6-tile radius. Ground lights (the speed-cue dots) are kept within
distance 0x2400 of the aircraft (about two tiles) and shaded by distance;
the engine uses depth ahead for both, which in other view directions
stretches them sideways and drops them behind. They are 10-foot squares
(`--light-size`) kept 2-6 px wide at 1080 rows (the original's one pixel is
6 px there; a realistic size makes them vanish), and fade out over the last
30% of the range; classic lines keep the
original's one pixel. Ground objects keep the engine's
painter order (level 4 to 1, far tiles first); the rest are depth-tested.
Building the scene takes about 0.2-0.4 ms per frame (~1500 instances).

Lines (`HiresPrim::LineStyle`; `--classic-lines` or Alt+F11 for the
original's one screen width). The engine draws every line one pixel wide at
320x200, so at high resolution roads are chunky far away and thin up close,
and distant mountain ranges become a tangle of ridge lines. The scene
classifies each line from the shape data (all four theatres checked):

- Ground: both ends at z = 0 (roads, runway borders, site outlines). Drawn
  as a strip in the shape's ground plane: free lines on ground decals (roads)
  `--road-width` feet wide (default 40), lines along a polygon edge
  (runway borders) and other ground lines 1 shape unit wide.
- Relief: lines of raised terrain shapes with at most 5 faces whose extent
  is at least 2048 world units, placed at level 2+ (the hill/ridge shapes:
  NC 3/4, LB 9/10, PG 10/11, CE 3), and their far bodies. Screen width up to
  25k units, thinning to the minimum and fading to 50% by 200k (mountains
  are typically 50-100k away).
- Edge: everything else (sites, buildings, aircraft): 1 shape unit wide,
  facing the camera.

Ground and edge lines stay at least 1.25 px wide (at 1080 rows) and fade in
proportion below that, to 25%; opacity uses the width facing the camera, so
a road seen at a grazing angle is thin but still visible. Ground decals
(runways, roads, fields) also keep their detailed bodies 8x farther
(`SceneBuildParams::ground_lod`): their far bodies are crude (a runway
becomes one line) and cost little. `f19trace -H` writes
`.cockpit3d-classic.ppm` / `.cockpit3d-left-classic.ppm` for comparison.

Not yet native: the far "dot" the engine draws in 2D for distant list
objects (stays on the HUD layer), the carrier wake, smoke and other
non-shape effects.

Head tracking (`native/src/host/headtrack.cpp`): OpenTrack's "UDP over
network" output (six little-endian doubles per datagram: x, y, z in cm,
yaw, pitch, roll in degrees), received on 127.0.0.1:4242 by default
(`--headtrack-port N`, 0 disables; `--headtrack-bind ADDR`). Rotation
drives the camera (yaw right +, pitch up +, roll right ear down +; mouse
look adds to it); position (x right, y up, z back) moves only the eye
relative to the panel quad (1 unit = 60 cm, eye kept behind the panel),
so leaning in brings the instruments closer. The pose drops back to
neutral if no data arrives for 1 s. `f19trace -H` also writes a
`.cockpit3d-lean.ppm` with a sample 6DOF pose.

VR (`native/src/host/xr.cpp`, `f19 --vr`): OpenXR output, stereo 3D view
through `GlRenderer::render_view` (per-eye pose and asymmetric frustum),
menus on a virtual screen. Works end to end with Monado and an Acer WMR
headset (rotation-only tracking, per-eye display offsets); Basalt 6DOF
diverges on it. Setup,
findings, implementation and options: [vr.md](vr.md).

Joystick / gamepad (`native/src/host/gamepad.cpp`, port 201h in
`machine.cpp`): the game reads the PC game port itself. EGAME
(`EGAME.EXE` file offset 0x12E85) writes 201h, then counts passes of a
10-instruction loop until axis bits 0/1 clear (CX = 0, so 65536 passes
means no joystick: 0xFFFF), and calibrates itself: the centre is captured
once, the minimum/maximum widen as they are seen, so each direction reads
full scale after it has been reached once. Buttons come from MISC's
`in al,201h` (bit 4 + n, active low). SU asks "Do you have a joystick
(Y/N)" and calibrates with fire button 1. The emulated port times each
axis as 24 us + 0..1.1 ms (a 100 kOhm pot) in emulated instructions, so
the counts don't depend on host speed; it reads as empty while no
controller is connected. An SDL gamepad drives it through bindings in
`native/gamepad.cfg` (built in; `~/.config/f19/gamepad.cfg` or
`--gamepad FILE` override): stick axes, the two fire buttons, look-around
(springs back; shift 1 + left stick on the pad), and keys with modifiers (held = typematic
repeat after 500 ms at 20/s), in three layers: two controls act as shift 1
and shift 2 (left trigger and left bumper / grip), chosen at press time.
The default layout follows the cockpit's left and right halves; see the
comments in gamepad.cfg. In VR the WMR motion controllers feed the
same bindings (`left-`/`right-` controls; see [vr.md](vr.md)). Which game functions the two fire buttons
trigger is not yet traced.

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

## Status and next steps (2026-10-03, native Arch laptop)

Working:
- `build/native/f19 out/run/native` runs the full game natively: original
  stages in the interpreter, native MGRAPHIC driver (80/84 slots, verified),
  OpenGL renderer, emulation on its own thread (25 MIPS default, VGA refresh
  = display refresh).
- High-res 3D world (MSAA, object depth): native scene in all directions
  (shapes and terrain from the game's data), engine capture as fallback; cockpit view
  as a 3D scene (panel as a tilted textured quad, HUD collimated at infinity above it); widescreen
  external views; F11 toggles 3D/flat; right-drag mouse look; OpenTrack
  UDP head tracking (6DOF; confirmed live with OpenTrack and the laptop
  webcam); gamepad as the PC joystick plus key bindings (gamepad.cfg; game
  port loop checked headless; WMR motion controllers confirmed live in VR, needs a patched Monado, see vr.md); F12
  screenshots (+ engine frame + primitive dump); F19_PERF=1 timing.
- A clipboard in the cockpit (`host/manual.cpp`): controls pages generated
  from the loaded bindings (per device, then host keys and the game keys
  the bindings press; console font), then the game manual: the user's PDF
  (`--manual FILE`, else the first *.pdf in GAMEDIR or the current
  directory) rendered with poppler-cpp (optional at build time) on a
  background thread, margins trimmed, onto a clipboard image (page n / N on
  the clip); a mipmapped quad in the 3D views, drawn flat over menus and
  flat views. Ctrl+F11 shows it; PgUp/PgDn (Shift: 10), Home/End, wheel;
  controllers through the `manual` binding layer (gamepad.cfg). The page
  is remembered in ~/.local/state/f19/manual-page.
- Headless checks: `build/native/f19trace out/run/demo_native -n 380 -k n1 -H x.ppm`
  (demo flies itself; writes hi-res, world-only, page, mask, 3D renders;
  -n 380 lands on a cockpit frame, -n 330 now gives an external view).

Open / next:
- Native scene: confirmed live in the full game (2026-10-03). Still to
  check: other theatres, night missions. Next: smooth motion by interpolating
  instances between engine frames; GPU-resident meshes if more draw
  distance is wanted.
- `f19trace` runs are not reproducible (flip counts vary between identical
  runs, also without the scene hooks): something reads host time.
- HUD in the 3D cockpit is the upscaled 320x200 pixels; sharper later by
  capturing the HUD's driver line/text calls instead of pixels.
- Carrier wake not captured (another fill path); distant thin land slivers.
- Possible residual hitches (user suspects their VNC session); measure with
  F19_PERF=1.
- VR: playable with the WMR headset (3DOF, offsets in vr.md). Not done:
  external views' overlay head-locked, 90 Hz, world/cockpit scale check,
  6DOF (Basalt diverges).
- Sound: PC speaker emulated (`core/pcspeaker.cpp`); the original
  `ISOUND.EXE` runs in the interpreter (SU picks it for every display but
  Tandy, which gets `TSOUND`, SN76489 on port C0h: not emulated; `BSOUND`
  has no port I/O). Effects use PIT channel 2 in mode 3, retuned every
  timer tick (the driver sets IRQ0 to 60.9 Hz and chains the BIOS every
  third tick); the title theme is a 3-voice 1-bit synth toggling port 61h
  bit 1 from a delay loop, calibrated at init against the timer, so its
  pitch shifts slightly with `--mips` (about 40 cents sharp at 25 vs 4),
  as it did between real PCs. Changes are applied at their emulated time,
  integrated exactly per sub-sample (4x), band-limited, and shaped by an
  approximate small-cone response (`--raw-speaker` skips that). The demo
  never starts a sound. `f19trace -w OUT.wav` records the speaker.
  Not yet checked by ear in flight. Display modes other than VGA unsupported.
- Long-term: 3D cockpit with instrument bitmaps on modelled screens;
  progressive porting of game logic (flight model first, STREAM.DTA as a
  regression input).
