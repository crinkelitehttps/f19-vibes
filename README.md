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
