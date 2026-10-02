# F-19 Stealth Fighter demo — reverse engineering & rewrite

Reverse engineering the 1988 MicroProse *F-19 Stealth Fighter* DOS demo, with
the goal of rewriting it on a modern renderer.

## Layout

- `gamefiles/` — original demo files (read-only, not tracked; checksums in
  `gamefiles.sha256`).
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
