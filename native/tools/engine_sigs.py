"""Byte signatures for the F-19 3D engine hook points (demo DGAME.EXE and
full EGAME.EXE). Mirrors the patterns in native/src/hires/world_capture.cpp - run this to check
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
    # --- Scene capture (native world rendering) ---
    # Shape draw entry (far): stack = shape far ptr, 3 angles, x, y, z.
    "shape_draw": ("55 8b ec 56 57 8b 46 0a a3 ?? ?? 8b 46 0c a3 ?? ?? 8b 46 0e a3 ?? ?? c4 76 06 89 36 ?? ?? "
                   "8c 06 ?? ?? 26 ac a2 ?? ?? 8b 5e 12 2b 1e ?? ?? 89 1e ?? ?? 8b 4e 14 2b 0e ?? ?? 89 0e ?? ?? "
                   "8b 6e 10 2b 2e ?? ??",
                   {"angle0": 9, "angle1": 15, "angle2": 21, "size": 37, "cam_y": 44, "cam_z": 55, "cam_x": 66}),
    # Object cull: camera matrix (9 words).
    "cull": ("56 a1 ?? ?? f7 eb 8b fa 8b f0 a1 ?? ?? f7 e9 03 f0 13 fa a1 ?? ?? f7 ed", {"matrix": 20}),
    "cull_range": ("3b 3e ?? ?? 7f 7e 8b 1e ?? ?? d1 e3 3b bf ?? ?? 7c 72 8b 36 ?? ??", {"max_range": 2}),
    # LOD walk: distance shift, threshold table.
    "lod": ("26 8a 04 a8 80 74 21 25 07 00 d1 e0 8b d8 a1 ?? ?? 8a 0e ?? ?? d3 f8 3b 87 ?? ??",
            {"lod_dist": 15, "lod_shift": 19, "lod_table": 25}),
    # Animated axis (prefix 0x6x bodies).
    "anim": ("26 ac 25 03 00 d1 e0 8b d8 a1 ?? ?? 89 87 ?? ??", {"anim": 10}),
    # Night/haze shading flag.
    "shade_flag": ("8a 26 ?? ?? 0a e4 74 28 a0 ?? ?? 98 f6 d4 22 c4 2a e4 d1 e8", {"night": 2}),
    # Shared vertex table: index arrays and value lists (z, y, x).
    "shared": ("26 ac 2a e4 8b f8 8a 9d ?? ?? 2a ff d1 e3 8b 8f ?? ?? 8a 9d ?? ?? 2a ff d1 e3 8b 87 ?? ?? "
               "a3 ?? ?? 8a 9d ?? ?? 2a ff d1 e3 8b 9f ?? ??",
               {"zi": 8, "zv": 16, "yi": 20, "yv": 28, "xi": 35, "xv": 43}),
    # Terrain loop entry (near): heading, pitch, camera x, y, z (32-bit each).
    "terrain": ("55 8b ec 83 ec 1c 56 8b 46 08 8b 56 0a a3 ?? ?? 89 16 ?? ?? 8b 46 0c 8b 56 0e a3", {}),
    "terrain_lists": ("a3 ?? ?? 8b 76 e6 d1 e6 8b 1e ?? ?? b1 06 d3 e3 8b 80 ?? ?? a3 ?? ?? c7 46 f6 00 00 eb 48 "
                      "8b 1e ?? ?? 8a 5f 06 2a ff d1 e3 8b 87 ?? ?? 05 00 00 a3 ?? ?? c7 06 ?? ?? ?? ??",
                      {"level": 10, "lists": 18, "shape_table": 43, "shape_seg": 55}),
    "terrain_counts": ("8b 46 f6 39 80 ?? ?? 77 03", {"counts": 5}),
    "terrain_enable": ("83 3e ?? ?? 01 7d 03 e9 ?? ?? 8b 1e ?? ?? d1 e3 83 bf ?? ?? 00", {"enable": 18}),
    # Tile lookup tables.
    "tile_sizes": ("8b b7 ?? ?? 39 76 06", {"sizes": 2}),
    "tile_l4": ("8b 76 08 b1 03 d3 e6 8b 5e 06 8a 80 ?? ??", {"map4": 12}),
    "tile_l3": ("8b 76 08 b1 04 d3 e6 8b 5e 06 8a 80 ?? ??", {"map3": 12}),
    "tile_l2": ("b8 03 00 50 e8 ?? ?? 83 c4 06 b1 04 d3 e0 8b 76 08 83 e6 03 d1 e6 d1 e6 03 f0 8b 5e 06 83 e3 03 8a 80 ?? ??", {"exp3": 34}),
    "tile_l1": ("b8 02 00 50 e8 ?? ?? 83 c4 06 b1 04 d3 e0 8b 76 08 83 e6 03 d1 e6 d1 e6 03 f0 8b 5e 06 83 e3 03 8a 80 ?? ??", {"exp2": 34}),
    "tile_l0": ("b8 01 00 50 e8 ?? ?? 83 c4 06 b1 04 d3 e0 8b 76 08 83 e6 03 d1 e6 d1 e6 03 f0 8b 5e 06 83 e3 03 8a 80 ?? ??", {"exp1": 34}),
    # Dynamic object draw (near, 10 args; arg 10 = scale shift).
    "object_draw": ("55 8b ec 83 ec 10 ff 76 04 e8 ?? ?? 83 c4 02 89 46 fc 80 3e ?? ?? 00 75 05 a1", {}),
    # Its camera arithmetic: aircraft x (32), camera y (32), altitude, view flags, camera x/y/alt for external views, zoom.
    "object_camera": ("80 3e ?? ?? 00 75 05 a1 ?? ?? eb 03 a1 ?? ?? 89 46 fe 8b 46 06 8b 56 08 2b 06 ?? ?? 1b 16 ?? ?? "
                      "89 46 f8 89 56 fa 8b 46 0a 8b 56 0c 03 06 ?? ?? 13 16 ?? ?? 2d 00 00 81 da 00 01 89 46 f4 89 56 f6 "
                      "8b 46 0e 2b 06 ?? ?? 89 46 f2 f6 06 ?? ?? 80 74 34 a1 ?? ?? 8b 16 ?? ?? 2b 06 ?? ?? 1b 16 ?? ?? "
                      "01 46 f8 11 56 fa a1 ?? ?? 8b 16 ?? ?? 2b 06 ?? ?? 1b 16 ?? ?? 01 46 f4 11 56 f6 a1 ?? ?? 2b 06 ?? ?? "
                      "01 46 f2 80 3e ?? ??",
                      {"ac_x": 26, "cam_y": 46, "alt": 70, "view": 77, "cam_x": 91, "ac_y": 104, "ac_alt": 129, "zoom": 136}),
    # Shape code -> shape (theatre table or STFLT table + offset).
    "shape_code": ("55 8b ec f7 46 04 00 01 74 0e 8b 5e 04 83 e3 7f d1 e3 8b 87 ?? ?? eb 18 8b 5e 04 d1 e3 8b 9f ?? ?? 8d 87 ?? ??",
                   {"theatre": 20, "stflt": 31, "stflt_add": 35}),
    # Main object list (aircraft, ships, vehicles): count, flags field (record base = flags - 0x16, 0x24 bytes);
    # its shape draw call (type field, shape code table; returns to match + 15).
    "object_list": ("a1 ?? ?? 39 46 e0 7c 03 e9 ?? ?? b8 24 00 f7 6e e0 8b d8 f6 87 ?? ?? 02", {"count": 1, "flags": 21}),
    "object_list_draw": ("8b 9c ?? ?? b1 05 d3 e3 ff b1 ?? ?? e8 ?? ?? 83 c4 14", {"type": 2, "codes": 10}),
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
