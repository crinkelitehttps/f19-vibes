"""Export .3D3 shapes and theatre terrain to glTF 2.0 binary (.glb).

Usage:
    python3 export_gltf.py shapes FILE.3D3 OUT.glb
        One mesh + node per shape, laid out in a row (node names "<file>_<index>").
    python3 export_gltf.py terrain GAMEDIR THEATRE OUT.glb
        The theatre's terrain: a node per placed object, instancing the
        theatre's shape meshes. Needs build/unpacked/DGAME.EXE (level-4 map).

Conventions:
- Game axes are x east, y north, z up. glTF is Y-up, right-handed, so
  glTF (X, Y, Z) = (x, z, -y).
- Units are raw game units (a level-1 terrain tile is 4096). Objects on
  level L tiles are scaled by 4**(L-1), as the engine does.
- Each polygon is triangulated as a fan and wound so its front face matches
  the shape's face-plane normal (the engine's backface test). Line primitives
  export as LINES, point/light shapes as POINTS.
- One unlit material per EGA colour index (KHR_materials_unlit).
- Only each shape's own body is exported; LOD jumps to coarser shapes are
  recorded in node extras ("lods": [[level, target_offset], ...]).
- Ground decals (roads, runway markings) are coplanar with the ground; the
  engine relies on draw order, so a renderer needs depth bias or ordering.
"""
import json
import struct
import sys
from pathlib import Path

import shape3d
import world
from pic2png import EGA
from render_shapes import edge_loop


def _srgb_to_linear(c):
    c /= 255
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def _to_gltf(v):
    x, y, z = v
    return (float(x), float(z), float(-y))


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _loop_normal(pts):
    """Newell's method, robust for non-convex / slightly non-planar loops."""
    n = [0.0, 0.0, 0.0]
    for (x0, y0, z0), (x1, y1, z1) in zip(pts, pts[1:] + pts[:1]):
        n[0] += (y0 - y1) * (z0 + z1)
        n[1] += (z0 - z1) * (x0 + x1)
        n[2] += (x0 - x1) * (y0 + y1)
    return tuple(n)


class GlbBuilder:
    def __init__(self):
        self.bin = bytearray()
        self.buffer_views, self.accessors, self.meshes, self.nodes = [], [], [], []
        self.materials = []
        for i, (r, g, b) in enumerate(EGA):
            self.materials.append({
                "name": f"ega{i:02d}",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [_srgb_to_linear(r), _srgb_to_linear(g), _srgb_to_linear(b), 1.0],
                    "metallicFactor": 0.0, "roughnessFactor": 1.0},
                "extensions": {"KHR_materials_unlit": {}},
            })
        # 16..31: double-sided copies, for faces with no plane (always drawn).
        for i in range(16):
            self.materials.append(dict(self.materials[i], name=f"ega{i:02d}_2s", doubleSided=True))

    def _view(self, data, target):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.buffer_views.append({"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data), "target": target})
        self.bin += data
        return len(self.buffer_views) - 1

    def _positions(self, pts):
        data = b"".join(struct.pack("<3f", *p) for p in pts)
        view = self._view(data, 34962)
        self.accessors.append({
            "bufferView": view, "componentType": 5126, "count": len(pts), "type": "VEC3",
            "min": [min(p[k] for p in pts) for k in range(3)],
            "max": [max(p[k] for p in pts) for k in range(3)]})
        return len(self.accessors) - 1

    def _indices(self, idx):
        data = struct.pack(f"<{len(idx)}H", *idx)
        view = self._view(data, 34963)
        self.accessors.append({"bufferView": view, "componentType": 5123, "count": len(idx), "type": "SCALAR"})
        return len(self.accessors) - 1

    def add_mesh(self, name, groups):
        """groups: {(mode, colour): (positions, indices)}. Returns mesh index or None."""
        prims = []
        for (mode, colour), (pts, idx) in sorted(groups.items()):
            if not idx:
                continue
            prims.append({"attributes": {"POSITION": self._positions(pts)}, "indices": self._indices(idx),
                          "mode": mode, "material": colour})
        if not prims:
            return None
        self.meshes.append({"name": name, "primitives": prims})
        return len(self.meshes) - 1

    def add_node(self, node):
        self.nodes.append(node)
        return len(self.nodes) - 1

    def write(self, path, root_nodes):
        gltf = {
            "asset": {"version": "2.0", "generator": "f19 tools/export_gltf.py"},
            "extensionsUsed": ["KHR_materials_unlit"],
            "scene": 0, "scenes": [{"nodes": root_nodes}],
            "nodes": self.nodes, "meshes": self.meshes, "materials": self.materials,
            "accessors": self.accessors, "bufferViews": self.buffer_views,
            "buffers": [{"byteLength": len(self.bin)}],
        }
        js = json.dumps(gltf, separators=(",", ":")).encode()
        js += b" " * (-len(js) % 4)
        binc = bytes(self.bin) + b"\0" * (-len(self.bin) % 4)
        total = 12 + 8 + len(js) + 8 + len(binc)
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        with open(path, "wb") as f:
            f.write(struct.pack("<3I", 0x46546C67, 2, total))
            f.write(struct.pack("<2I", len(js), 0x4E4F534A) + js)
            f.write(struct.pack("<2I", len(binc), 0x004E4942) + binc)


def shape_groups(sf, shape, stats):
    """Build {(mode, colour): (positions, indices)} for a shape's body."""
    body = shape.body
    verts = shape3d.resolve(sf, body)
    groups = {}

    def group(mode, colour):
        return groups.setdefault((mode, colour), ([], []))

    for p in body.prims:
        if p[0] == "poly" and p[3] != 255:
            loop = edge_loop(body, p[2], verts)
            if len(loop) < 3:
                stats["degenerate"] += 1
                continue
            material = p[3] & 15
            if p[1] < len(body.planes):
                plane_n = body.planes[p[1]][:3]
                if _dot(_loop_normal(loop), plane_n) < 0:
                    loop = loop[::-1]
                stats["oriented"] += 1
            else:
                # No face plane: the engine's visibility mask starts all-ones,
                # so the face is always drawn, from either side.
                material += 16
                stats["double_sided"] += 1
            pts, idx = group(4, material)
            base = len(pts)
            pts.extend(_to_gltf(v) for v in loop)
            for i in range(1, len(loop) - 1):
                idx += [base, base + i, base + i + 1]
        elif p[0] == "line" and p[3] != 255 and p[2] < len(body.edges):
            _, a, b = body.edges[p[2]]
            if a < len(verts) and b < len(verts):
                pts, idx = group(1, p[3] & 15)
                idx += [len(pts), len(pts) + 1]
                pts += [_to_gltf(verts[a]), _to_gltf(verts[b])]
        elif p[0] in ("point", "lights"):
            pts, idx = group(0, p[1] & 15)
            for v in verts or [(0, 0, 0)]:
                idx.append(len(pts))
                pts.append(_to_gltf(v))
    return groups


def build_shape_meshes(gb, sf, prefix, stats):
    """Add a mesh for every shape; returns {shape_index: mesh_index}."""
    meshes = {}
    for s in sf.shapes:
        if s is None or s.body is None:
            continue
        m = gb.add_mesh(f"{prefix}_{s.index:03d}", shape_groups(sf, s, stats))
        if m is not None:
            meshes[s.index] = m
    return meshes


def _new_stats():
    return {"oriented": 0, "double_sided": 0, "degenerate": 0}


def export_shapes(path, out):
    sf = shape3d.load(path)
    prefix = Path(path).stem
    gb, stats = GlbBuilder(), _new_stats()
    meshes = build_shape_meshes(gb, sf, prefix, stats)
    roots, x = [], 0.0
    for s in sf.shapes:
        if s is None or s.index not in meshes:
            continue
        verts = shape3d.resolve(sf, s.body)
        half = max([abs(c) for v in verts for c in v[:2]] or [1])
        x += half
        roots.append(gb.add_node({
            "name": f"{prefix}_{s.index:03d}", "mesh": meshes[s.index], "translation": [x, 0.0, 0.0],
            "extras": {"shape": s.index, "size_class": s.size_class, "lods": s.lods}}))
        x += half * 1.25
    gb.write(out, roots)
    return stats, len(roots)


def export_terrain(gamedir, theatre, out):
    gd = Path(gamedir)
    sf = shape3d.load(gd / f"{theatre}.3D3")
    g = world.load_3dg(gd / f"{theatre}.3DG")
    objs = world.load_3dt(gd / f"{theatre}.3DT")
    base = world.load_base_map(Path(__file__).parent.parent / "build/unpacked/DGAME.EXE", theatre)
    gb, stats = GlbBuilder(), _new_stats()
    meshes = build_shape_meshes(gb, sf, theatre, stats)
    level_nodes, placed = [], 0

    def place(children, level, tx, ty, t):
        nonlocal placed
        tile = 0x1000 * 4 ** (level - 1)
        scale = 4.0 ** (level - 1)
        cx, cy = (tx + 0.5) * tile, (ty + 0.5) * tile
        for x, y, z, s in objs[level][t]:
            # Bit 7 (state-dependent shapes) is already masked off by world.load_3dt.
            if s not in meshes:
                continue
            children.append(gb.add_node({
                "name": f"L{level}_{tx}_{ty}_s{s}", "mesh": meshes[s],
                "translation": list(_to_gltf((cx + x * scale, cy + y * scale, z * scale))),
                "scale": [scale] * 3}))
            placed += 1

    # Level 4: 8x8 base map, cells 2..5 are the playable 4x4.
    children = []
    for ty in range(8):
        for tx in range(8):
            t = base[ty * 8 + tx]
            if t < len(objs[4]):
                place(children, 4, tx - 2, ty - 2, t)
    level_nodes.append(gb.add_node({"name": "level4", "children": children}))
    for level in (3, 2, 1):
        n = 16 * 4 ** (3 - level)
        children = []
        for ty in range(n):
            for tx in range(n):
                t = world.tile_at(g, level, tx, ty)
                if t < len(objs[level]):
                    place(children, level, tx, ty, t)
        level_nodes.append(gb.add_node({"name": f"level{level}", "children": children}))
    root = gb.add_node({"name": theatre, "children": level_nodes,
                        "extras": {"units": "game units; level-1 tile = 4096", "axes": "glTF X=east, Y=up, -Z=north"}})
    gb.write(out, [root])
    return stats, placed


if __name__ == "__main__":
    if sys.argv[1] == "shapes":
        stats, n = export_shapes(sys.argv[2], sys.argv[3])
        print(f"{sys.argv[3]}: {n} shapes, polys {stats}")
    elif sys.argv[1] == "terrain":
        stats, n = export_terrain(sys.argv[2], sys.argv[3], sys.argv[4])
        print(f"{sys.argv[4]}: {n} placed objects, polys {stats}")
    else:
        sys.exit(__doc__)
