"""Top-down render of a theatre's terrain (levels 3..1) from .3DG/.3DT/.3D3.

Usage: python3 render_map.py GAMEDIR THEATRE OUT.png   (e.g. gamefiles NC out/map/NC.png)
"""
import sys
from pathlib import Path

import png
import shape3d
import world
from pic2png import EGA
from render_shapes import edge_loop, fill_poly, line

SIZE = 2048            # output pixels; the level-3 grid is 16x16


def draw_shape(img, sf, shape_idx, cx, cy, scale):
    """Orthographic top-down draw; scale = pixels per local unit."""
    if shape_idx >= len(sf.shapes) or not sf.shapes[shape_idx]:
        return
    body = sf.shapes[shape_idx].body
    verts = shape3d.resolve(sf, body)
    if not verts:
        return
    scr = [(cx + v[0] * scale, cy - v[1] * scale) for v in verts]
    # Draw lower geometry first so raised parts land on top.
    items = []
    for p in body.prims:
        if p[0] == "poly" and p[3] != 255:
            loop = edge_loop(body, p[2], list(range(len(verts))))
            if len(loop) >= 3:
                items.append((sum(verts[i][2] for i in loop) / len(loop), "poly", [scr[i] for i in loop], p[3]))
        elif p[0] == "line" and p[3] != 255 and p[2] < len(body.edges):
            _, a, b = body.edges[p[2]]
            if a < len(scr) and b < len(scr):
                items.append((max(verts[a][2], verts[b][2]) + 0.5, "line", [scr[a], scr[b]], p[3]))
    for _, kind, pts, col in sorted(items, key=lambda t: t[0]):
        if kind == "poly":
            fill_poly(img, SIZE, SIZE, pts, col & 15)
        else:
            line(img, SIZE, SIZE, pts[0], pts[1], col & 15)


def main(gamedir, theatre, out):
    gd = Path(gamedir)
    sf = shape3d.load(gd / f"{theatre}.3D3")
    g = world.load_3dg(gd / f"{theatre}.3DG")
    objs = world.load_3dt(gd / f"{theatre}.3DT")
    img = bytearray([16]) * (SIZE * SIZE)
    # Level 4: 8x8 base map; cells 2..5 cover the 16x16 level-3 world.
    base = world.load_base_map(Path(__file__).parent.parent / "build/unpacked/DGAME.EXE", theatre)
    tile_px = SIZE / 4
    for ty in range(8):
        for tx in range(8):
            t = base[ty * 8 + tx]
            if t < len(objs[4]):
                cx, cy = (tx - 2 + 0.5) * tile_px, (ty - 2 + 0.5) * tile_px
                for x, y, z, s in objs[4][t]:
                    draw_shape(img, sf, s, cx + x * tile_px / 4096, cy - y * tile_px / 4096, tile_px / 4096)
    for level in (3, 2, 1):
        n = 16 * 4 ** (3 - level)
        tile_px = SIZE / n
        scale = tile_px / 4096
        for ty in range(n):
            for tx in range(n):
                t = world.tile_at(g, level, tx, ty)
                if t >= len(objs[level]):
                    continue
                cx, cy = (tx + 0.5) * tile_px, (ty + 0.5) * tile_px
                for x, y, z, s in objs[level][t]:
                    draw_shape(img, sf, s, cx + x * scale, cy - y * scale, scale)
    pal = EGA + [(40, 44, 52)] + [(0, 0, 0)] * 239
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    png.write_indexed(out, SIZE, SIZE, img, pal)


if __name__ == "__main__":
    main(*sys.argv[1:4])
