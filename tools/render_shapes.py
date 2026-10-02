"""Render a contact sheet of every shape in a .3D3 file (3/4 view, flat shaded).

Usage: python3 render_shapes.py FILE.3D3 OUT.png
"""
import math
import sys

import png
import shape3d
from pic2png import EGA

CELL, COLS = 160, 10


def edge_loop(body, edge_ids, verts):
    """Chain a polygon's edge list into an ordered vertex loop."""
    es = [body.edges[e][1:] for e in edge_ids if e < len(body.edges)]
    if not es:
        return []
    loop = list(es[0])
    rest = es[1:]
    while rest:
        for i, (a, b) in enumerate(rest):
            if a == loop[-1]:
                loop.append(b)
            elif b == loop[-1]:
                loop.append(a)
            else:
                continue
            rest.pop(i)
            break
        else:
            break
    if len(loop) > 1 and loop[0] == loop[-1]:
        loop.pop()
    return [verts[i] for i in loop if i < len(verts)]


def fill_poly(img, w, h, pts, col):
    ys = [p[1] for p in pts]
    for y in range(max(0, int(min(ys))), min(h, int(max(ys)) + 1)):
        xs = []
        for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1]):
            if (y0 <= y < y1) or (y1 <= y < y0):
                xs.append(x0 + (y - y0) * (x1 - x0) / (y1 - y0))
        xs.sort()
        for a, b in zip(xs[::2], xs[1::2]):
            for x in range(max(0, int(a)), min(w, int(b) + 1)):
                img[y * w + x] = col


def line(img, w, h, p0, p1, col):
    (x0, y0), (x1, y1) = p0, p1
    n = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
    for i in range(n + 1):
        x, y = int(x0 + (x1 - x0) * i / n), int(y0 + (y1 - y0) * i / n)
        if 0 <= x < w and 0 <= y < h:
            img[y * w + x] = col


def render(sf, shape, img, W, ox, oy):
    body = shape.body
    verts = shape3d.resolve(sf, body)
    if not verts:
        return
    yaw, pitch = math.radians(35), math.radians(30)

    def view(v):
        x, y, z = v
        x, y = x * math.cos(yaw) - y * math.sin(yaw), x * math.sin(yaw) + y * math.cos(yaw)
        y, z = y * math.cos(pitch) - z * math.sin(pitch), y * math.sin(pitch) + z * math.cos(pitch)
        return x, y, z      # screen x, depth, screen up

    vv = [view(v) for v in verts]
    span = max(max(abs(a) for a in (p[0], p[2])) for p in vv) or 1
    s = (CELL / 2 - 8) / span

    def scr(p):
        return (ox + CELL / 2 + p[0] * s, oy + CELL / 2 - p[2] * s)

    items = []
    for p in body.prims:
        if p[0] == "poly" and p[3] != 255:
            loop = edge_loop(body, p[2], list(range(len(verts))))
            if len(loop) >= 3:
                depth = sum(vv[i][1] for i in loop) / len(loop)
                items.append((depth, "poly", [scr(vv[i]) for i in loop], p[3]))
        elif p[0] == "line" and p[3] != 255 and p[2] < len(body.edges):
            _, a, b = body.edges[p[2]]
            if a < len(vv) and b < len(vv):
                items.append(((vv[a][1] + vv[b][1]) / 2 - 1e-3, "line", [scr(vv[a]), scr(vv[b])], p[3]))
        elif p[0] in ("point", "lights"):
            for v in vv:
                items.append((-1e9, "line", [scr(v), scr(v)], p[1]))
    for _, kind, pts, col in sorted(items, key=lambda t: -t[0]):
        if kind == "poly":
            fill_poly(img, W, len(img) // W, pts, col & 15)
        else:
            line(img, W, len(img) // W, pts[0], pts[1], col & 15)


def main(path, out):
    sf = shape3d.load(path)
    n = len(sf.shapes)
    rows = (n + COLS - 1) // COLS
    W, H = COLS * CELL, rows * CELL
    img = bytearray([16]) * (W * H)
    for i, shape in enumerate(sf.shapes):
        ox, oy = (i % COLS) * CELL, (i // COLS) * CELL
        for k in range(CELL):  # cell border
            img[oy * W + ox + k] = 17
            img[(oy + k) * W + ox] = 17
        if shape:
            render(sf, shape, img, W, ox, oy)
    pal = EGA + [(40, 44, 52), (70, 74, 84)] + [(0, 0, 0)] * 238
    png.write_indexed(out, W, H, img, pal)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
