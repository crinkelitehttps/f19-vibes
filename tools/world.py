"""Theatre terrain: .3DG (hierarchical tile map) and .3DT (objects per tile type).

Terrain is a 4-level quadtree-like hierarchy. Level 3 is a 16x16 grid of tile
types; a tile type at level L (1..3) expands into a 4x4 block of level L-1 tile
types via a 32-entry template table. Each (level, tile type) has a list of
objects from .3DT — (x, y, z, shape) relative to the tile centre — drawn at
scale 4**level (tile = 0x1000 local units).
"""
import struct
from pathlib import Path

LEVELS = 4  # 0..3; .3DT also has a group for level 4 (coarse 8x8 overview)


def load_3dg(path):
    d = Path(path).read_bytes()
    p = 2 + 16                      # magic, then 16 bytes the loader reads and discards
    top = d[p:p + 256]
    p += 256
    # Expansion templates for levels 3->2, 2->1, 1->0: 32 types x 4x4.
    t2, t1, t0 = (d[p + i * 512:p + (i + 1) * 512] for i in range(3))
    return {"top": top, "expand": {3: t2, 2: t1, 1: t0}}


def tile_at(g, level, x, y):
    """Tile type at (x, y) on the given level's grid (size 16 * 4**(3-level))."""
    if level == 3:
        return g["top"][y * 16 + x]
    parent = tile_at(g, level + 1, x >> 2, y >> 2)
    return g["expand"][level + 1][parent * 16 + (y & 3) * 4 + (x & 3)]


# Level-4 base maps: 8x8 tile types per theatre, in DGAME.EXE's data segment
# (DGROUP offset 0x758, 64 bytes each). The inner 4x4 (types 0-15) is the
# playable area; the border is filler (0x10/0x11). Lookups add (2, 2).
DGROUP_FILE_OFFSET = 0x480 + 0x23050   # in the EXEPACK-unpacked DGAME.EXE
BASE_MAPS = 0x758
THEATRE_INDEX = {"LB": 0, "PG": 1, "NC": 2, "CE": 3}   # provisional ordering


def load_base_map(unpacked_dgame, theatre):
    d = Path(unpacked_dgame).read_bytes()
    o = DGROUP_FILE_OFFSET + BASE_MAPS + 64 * THEATRE_INDEX[theatre]
    return d[o:o + 64]


def load_3dt(path):
    """Returns objs[group][tile_type] = [(x, y, z, shape), ...] for 5 groups."""
    d = Path(path).read_bytes()
    counts = struct.unpack_from("<5H", d, 2)
    p = 12
    per_tile = []
    for c in counts:
        per_tile.append(struct.unpack_from(f"<{c}H", d, p))
        p += 2 * c
    objs = []
    for g in per_tile:
        lst = []
        for n in g:
            tile = []
            for _ in range(n):
                x, y, z, s = struct.unpack_from("<3hH", d, p)
                tile.append((x, y, z, s & 0xFF))
                p += 8
            lst.append(tile)
        objs.append(lst)
    assert p == len(d), (p, len(d))
    return objs


# .WLD site records: 16 bytes (id, x, y, status, a, b, c, type) from offset
# 0x38 until a zero-type record. Positions are world / 32; WLD y increases
# southwards (verified: every site lands on its terrain feature cluster, the
# other orientation puts many in open sea). Type codes seen in NC:
# 0x124 airfield, 0x12b airfield/carrier?, 0x113 site 0x80 east of an
# airfield (SAM/radar?), 0x11a target, 0x146/0x148/0x149 other.
WORLD_SIZE = 16 * 0x10000


def load_wld(path, offset=0):
    """Returns [{id, type, x, y (world units, +y north), raw}] for the site table."""
    d = Path(path).read_bytes()[offset:]
    assert d[:2] == b"BN", "not a WLD block"
    sites = []
    for o in range(0x38, len(d) - 15, 16):
        r = struct.unpack_from("<8H", d, o)
        if r[7] == 0:
            break
        sites.append({"id": r[0], "type": r[7], "x": r[1] * 32, "y": WORLD_SIZE - r[2] * 32, "raw": list(r)})
    return sites
