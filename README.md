[![IMAGE ALT TEXT HERE](https://youtu.be/vi/Aq5WXmQQooo?is=JEefi-PMYXV5x6AJ/0.jpg)](h[ttps://www.youtube.com/watch?v=](https://youtu.be/Aq5WXmQQooo?is=JEefi-PMYXV5x6AJ)

https://youtu.be/Aq5WXmQQooo?is=JEefi-PMYXV5x6AJ

# F-19 Stealth Fighter demo — reverse engineering & rewrite

Reverse engineering the 1988 MicroProse *F-19 Stealth Fighter* DOS demo, with
the goal of rewriting it on a modern renderer.

## Layout

- `gamefiles/` — original demo files (read-only, not tracked; checksums in
  `gamefiles.sha256`).
- `fullgame/` — the full game, v435.00 (read-only, not tracked; checksums in
  `fullgame.sha256`; extracted from `f19stealthfighter.zip`).
- `tools/` — format decoders and analysis scripts.
- `docs/` — notes on file formats and program structure.

## Tools

```
python3 tools/unexepack.py gamefiles/DGAME.EXE build/unpacked/DGAME.EXE   # also DSTART, DSU, DEND
python3 tools/pic2png.py out/pic gamefiles/*.PIC gamefiles/F19.SPR
python3 tools/render_shapes.py gamefiles/NC.3D3 out/shapes/NC.png
python3 tools/render_map.py gamefiles NC out/map/NC.png
python3 tools/export_gltf.py shapes gamefiles/STFLT.3D3 out/gltf/STFLT.glb
python3 tools/export_gltf.py terrain gamefiles NC out/gltf/NC_terrain.glb
python3 tools/check_glb.py out/gltf/NC_terrain.glb out/gltf/check.png
```

The map and terrain exports need the unpacked `DGAME.EXE` in `build/unpacked/`.

## Viewer

A three.js viewer for the exported `.glb` files (shapes and the North Cape
terrain). Export the glTF files first (see above), then:

```
python3 tools/serve_viewer.py        # http://localhost:8019/viewer/
```

Terrain mode emulates the engine's draw order and draw distances (each level
only near the camera). `/viewer/?selftest` renders fixed views and writes
screenshots plus `report.json` to `out/viewer_report/`; run it headless with
`firefox --headless --no-remote --profile <tmpdir> "http://localhost:8019/viewer/?selftest"`.
Three.js is loaded from the jsDelivr CDN.

## Running the game

```
tools/run_f19.sh            # full game
tools/run_f19.sh demo       # the demo
tools/run_f19.sh full --fresh   # reset the run copy (roster, saves)
```

Runs DOSBox from a disposable copy in `out/run/<which>/`. Ctrl+F5
screenshot, Ctrl+Alt+F5 record video (to `out/run/capture/`),
Ctrl+F11/F12 slower/faster, Ctrl+F10 release the mouse.

## Native port (work in progress)

See `docs/native-port.md`. Build:

```
cmake -S native -B build/native -G Ninja && ninja -C build/native
```

CPU conformance (SingleStepTests 8088, ~760 MB download into `build/sst/`):

```
python3 native/tools/sst_fetch.py build/sst
build/native/sst_runner build/sst/*.bin
```

Play it natively (SDL3 window). The game directory must be a writable copy:

```
mkdir -p out/run/native && cp fullgame/* out/run/native/ && chmod -R u+w out/run/native
build/native/f19 out/run/native            # options: --scale N, --mips N, --trace
```

Headless bring-up runner (boots F19.COM, traces DOS/BIOS calls):

```
build/native/f19trace out/run/native -n 20 -t -s shot.ppm
```
