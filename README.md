# F-19 Stealth Fighter demo — reverse engineering & rewrite

Reverse engineering the 1988 MicroProse *F-19 Stealth Fighter* DOS demo, with
the goal of rewriting it on a modern renderer.

## Layout

- `gamefiles/` — original demo files (read-only, not tracked; checksums in
  `gamefiles.sha256`).
- `tools/` — format decoders and analysis scripts.
- `docs/` — notes on file formats and program structure.
