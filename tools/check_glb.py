"""Structural check of a .glb, plus an optional top-down render from it alone.

Usage: python3 check_glb.py FILE.glb [OUT_TOPDOWN.png]

Checks: chunk layout, index references, bufferView/accessor bounds, index
ranges, POSITION min/max. The render reads only the glTF (node TRS, meshes,
materials), so it independently verifies axis conversion and placement.
"""
import json
import struct
import sys

import png
from render_shapes import fill_poly, line

SIZE = 1024


def load(path):
    d = open(path, "rb").read()
    magic, ver, total = struct.unpack_from("<3I", d, 0)
    assert magic == 0x46546C67 and ver == 2 and total == len(d), "bad GLB header"
    jl, jt = struct.unpack_from("<2I", d, 12)
    assert jt == 0x4E4F534A and jl % 4 == 0
    gltf = json.loads(d[20:20 + jl])
    bl, bt = struct.unpack_from("<2I", d, 20 + jl)
    assert bt == 0x004E4942 and bl % 4 == 0 and 28 + jl + bl == len(d)
    return gltf, d[28 + jl:28 + jl + bl]


def read_accessor(g, binc, i):
    a = g["accessors"][i]
    v = g["bufferViews"][a["bufferView"]]
    comps = {"SCALAR": 1, "VEC3": 3}[a["type"]]
    fmt = {5126: "f", 5123: "H", 5125: "I"}[a["componentType"]]
    size = struct.calcsize(fmt) * comps
    off = v["byteOffset"] + a.get("byteOffset", 0)
    assert off + size * a["count"] <= v["byteOffset"] + v["byteLength"] <= len(binc), f"accessor {i} out of bounds"
    vals = struct.unpack_from(f"<{a['count'] * comps}{fmt}", binc, off)
    return [vals[k:k + comps] for k in range(0, len(vals), comps)] if comps > 1 else list(vals)


def check(g, binc):
    assert g["buffers"][0]["byteLength"] == len(binc) or g["buffers"][0]["byteLength"] + 3 >= len(binc)
    meshes = []
    for m in g["meshes"]:
        prims = []
        for p in m["primitives"]:
            pos = read_accessor(g, binc, p["attributes"]["POSITION"])
            a = g["accessors"][p["attributes"]["POSITION"]]
            for k in range(3):
                assert abs(min(v[k] for v in pos) - a["min"][k]) < 1e-3 and abs(max(v[k] for v in pos) - a["max"][k]) < 1e-3
            idx = read_accessor(g, binc, p["indices"])
            assert max(idx) < len(pos), "index out of range"
            if p["mode"] == 4:
                assert len(idx) % 3 == 0
            assert 0 <= p["material"] < len(g["materials"])
            prims.append((p["mode"], p["material"], pos, idx))
        meshes.append(prims)
    for n in g["nodes"]:
        assert "mesh" not in n or n["mesh"] < len(meshes)
        for c in n.get("children", []):
            assert c < len(g["nodes"])
    return meshes


def render_topdown(g, meshes, out):
    """Orthographic view looking down -Y (glTF), north (-Z) at the top."""
    inst = []

    def walk(i, t, s):
        n = g["nodes"][i]
        nt, ns = n.get("translation", [0, 0, 0]), n.get("scale", [1, 1, 1])
        t2 = [t[k] + nt[k] * s[k] for k in range(3)]
        s2 = [s[k] * ns[k] for k in range(3)]
        if "mesh" in n:
            inst.append((n["mesh"], t2, s2))
        for c in n.get("children", []):
            walk(c, t2, s2)

    for r in g["scenes"][0]["nodes"]:
        walk(r, [0, 0, 0], [1, 1, 1])
    pts = [(t[0] + v[0] * s[0], t[2] + v[2] * s[2]) for m, t, s in inst for p in meshes[m] for v in p[2]]
    x0, x1 = min(p[0] for p in pts), max(p[0] for p in pts)
    z0, z1 = min(p[1] for p in pts), max(p[1] for p in pts)
    k = (SIZE - 1) / max(x1 - x0, z1 - z0)
    img = bytearray([16]) * (SIZE * SIZE)
    items = []
    for m, t, s in inst:
        for mode, mat, pos, idx in meshes[m]:
            w = [((t[0] + v[0] * s[0] - x0) * k, (t[2] + v[2] * s[2] - z0) * k, t[1] + v[1] * s[1]) for v in pos]
            if mode == 4:
                for a in range(0, len(idx), 3):
                    tri = [w[idx[a]], w[idx[a + 1]], w[idx[a + 2]]]
                    items.append((sum(p[2] for p in tri) / 3, 4, [p[:2] for p in tri], mat % 16))
            elif mode == 1:
                for a in range(0, len(idx), 2):
                    items.append((max(w[idx[a]][2], w[idx[a + 1]][2]) + 0.5, 1, [w[idx[a]][:2], w[idx[a + 1]][:2]], mat % 16))
    for _, mode, p, col in sorted(items, key=lambda it: it[0]):
        if mode == 4:
            fill_poly(img, SIZE, SIZE, p, col)
        else:
            line(img, SIZE, SIZE, p[0], p[1], col)
    from pic2png import EGA
    png.write_indexed(out, SIZE, SIZE, img, EGA + [(40, 44, 52)] + [(0, 0, 0)] * 239)


if __name__ == "__main__":
    g, binc = load(sys.argv[1])
    meshes = check(g, binc)
    tris = sum(len(p[3]) // 3 for m in meshes for p in m if p[0] == 4)
    print(f"{sys.argv[1]}: OK — {len(g['nodes'])} nodes, {len(meshes)} meshes, {tris} triangles, "
          f"{len(g['materials'])} materials")
    if len(sys.argv) > 2:
        render_topdown(g, meshes, sys.argv[2])
