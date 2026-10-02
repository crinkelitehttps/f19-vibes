"""Byte signatures for the F-19 3D engine hook points (demo DGAME.EXE and
full EGAME.EXE). Mirrors native/src/hires/engine_sigs.h - run this to check
that every signature matches exactly once in both executables and to see the
operand values extracted from the matched code.

Usage: python3 engine_sigs.py UNPACKED.EXE...
"""
import struct
import sys

# name: (pattern, {operand: byte offset of a little-endian u16 in the match})
SIGS = {
    # Projection: camera-space tables, zoom flag, z shift, screen centre.
    "proj": ("8b 8f ?? ?? 80 3e ?? ?? 00 74 02 d1 e1 80 3e ?? ?? 00 74 0a 87 d1 8a 0e ?? ?? d3 fa 87 d1 "
             "0b c9 7e 4d 8b 97 ?? ?? 8a 87 ?? ?? 98 92 f7 f9 99 0b c0 03 06 ?? ??",
             {"z_hi": 2, "zoom": 6, "zshift": 15, "x_mid": 36, "cx": 51}),
    "proj_y": ("8b 97 ?? ?? 8a 87 ?? ?? 98 92 8b f2 8b f8 d1 fa d1 d8 d1 fa d1 d8 2b c7 1b d6 f7 f9 99 "
               "0b c0 03 06 ?? ??",
               {"y_mid": 2, "cy": 33}),
    # Edge loop: edge record base.
    "edge_loop": ("2a e4 26 ac 0b c0 74 15 8b c8 2b ff 81 c7 ?? ?? e8", {"edge_base": 14}),
    # Edge setup: ES:SI -> v0, v1 bytes; DI = edge record. Hook at match start.
    "edge_setup": ("2a e4 26 ac 8b d8 d1 e3 d1 e3 26 ac 8b e8 d1 e5 d1 e5 c6 45 18 00", {}),
    # Polygon: hook at the far call (offset 10): AH = colour, ES:SI -> edge list, n at ES:SI-1.
    "poly": ("8b fb 8a a5 ?? ?? 02 26 ?? ?? 9a", {"color_table": 4, "shade": 8}),
    # Line primitive: hook at the far call (offset 13): AH = colour, BX = edge record.
    "line": ("26 ac 2a e4 8b f8 8a a5 ?? ?? 02 26 ?? ?? 9a ?? ?? ?? ?? 8b 4f 08 8b 57 0c", {}),
    # Dot (point / ground light): hook at the near call (offset 18); camera
    # coordinates are vertex slot 0.
    "dot": ("a1 ?? ?? a3 ?? ?? a3 ?? ?? a1 ?? ?? a3 ?? ?? a3 ?? ?? e8", {}),
    # Horizon: hook at start.
    "horizon": ("a1 ?? ?? f7 d8 a3 ?? ?? a1 ?? ?? 99 8a d4 8a e0 2a c0 8b 0e ?? ?? 81 f9 0b 1f",
                {"up_s": 1, "sin": 6, "up_n": 9, "up_d": 20}),
    "horizon_cos": ("a1 ?? ?? d1 e0 f7 2e ?? ?? d1 e0 d1 d2 8b f0", {"cos": 7}),
    "horizon_mode": ("80 3e ?? ?? 02 75 15 8a 16 ?? ??", {"view_mode": 2, "alt": 9}),
    "horizon_colors": ("c7 06 ?? ?? 00 00 8a 26 ?? ?? 9a", {"sky": 8}),
    "horizon_ground": ("83 3e ?? ?? 00 78 13 8a 26 ?? ?? 9a", {"ground": 9}),
    # Viewport limits (clip routine).
    "viewport": ("8b d0 a1 ?? ?? 3b f0 77 10 3b e8 77 0c a1 ?? ??", {"vp_x": 3, "vp_y": 14}),
    # Far entries C code uses to draw plain 2D lines through the engine.
    "c_line": ("e8 0d 00 cb 55 56 57 06 e8 05 00 07 5f 5e 5d cb", {}),
}


def compile_pattern(p):
    return [None if t == "??" else int(t, 16) for t in p.split()]


def find_all(img, pat):
    out, first = [], pat[0]
    i = img.find(bytes([first]))
    while i >= 0:
        if all(b is None or img[i + k] == b for k, b in enumerate(pat)):
            out.append(i)
        i = img.find(bytes([first]), i + 1)
    return out


def main(paths):
    for path in paths:
        d = open(path, "rb").read()
        img = d[struct.unpack_from("<H", d, 8)[0] * 16:]
        print(path)
        for name, (p, ops) in SIGS.items():
            hits = find_all(img, compile_pattern(p))
            vals = {k: hex(struct.unpack_from("<H", img, hits[0] + o)[0]) for k, o in ops.items()} if hits else {}
            print(f"  {name:15s} {len(hits)} hit(s) {[hex(h) for h in hits[:3]]} {vals}")


if __name__ == "__main__":
    main(sys.argv[1:])
