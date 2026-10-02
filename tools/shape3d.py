"""Parser for MicroProse F-19 .3D3 shape files.

Reverse engineered from the shape renderer in DGAME.EXE (segment 0x2000 in
the unpacked image; see docs/formats.md for the byte-level description).
"""
import struct
from dataclasses import dataclass, field


class Reader:
    def __init__(self, data, pos):
        self.d, self.p = data, pos

    def u8(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def u16(self):
        v = struct.unpack_from("<H", self.d, self.p)[0]
        self.p += 2
        return v

    def s16(self):
        v = struct.unpack_from("<h", self.d, self.p)[0]
        self.p += 2
        return v


@dataclass
class Body:
    offset: int                  # absolute file offset of the body
    kind: str = "mesh"           # "mesh", "point" or "lights"
    prefix: int | None = None    # optional 0x6x prefix byte (ground-snap axis etc.)
    flags: int = 0               # high bits of the plane-count byte
    planes: list = field(default_factory=list)    # (nx, ny, nz, d)
    vertices: list = field(default_factory=list)  # (mask, (x, y, z)) or (mask, shared_index)
    shared_vertices: bool = False
    edges: list = field(default_factory=list)     # (mask, v0, v1)
    prims: list = field(default_factory=list)     # ("poly", plane, [edges], colour) / ("line", mask, edge, colour)
    sorted_groups: list | None = None             # per-plane primitive groups in 0xFF mode
    end: int = 0


@dataclass
class Shape:
    index: int
    size_class: int
    lods: list          # (level, target_offset) — "if farther than level threshold, jump"
    body: Body | None


def _mask(r, wide):
    return r.u16() | (r.u16() << 16 if wide else 0)


def _prim(r, wide):
    b = r.u8()
    if b & 3 == 1:
        plane = (b >> 3) & 0x1F
        n = r.u8()
        edges = [r.u8() for _ in range(n)]
        return ("poly", plane, edges, r.u8())
    m = _mask(r, wide)
    return ("line", m, r.u8(), r.u8())


def parse_body(d, pos):
    r = Reader(d, pos)
    body = Body(offset=pos)
    b = d[r.p]
    if b & 0x60 == 0x60:
        body.prefix = r.u8()
        b = d[r.p]
    t = b & 0x3F
    if t == 0x3F:
        # Single point at the object origin: [3F][colour].
        r.u8()
        body.kind = "point"
        body.prims = [("point", r.u8())]
        body.end = r.p
        return body
    if t == 0x3E:
        # Light points from the shared table: [3E][colour][n][index * n].
        r.u8()
        body.kind = "lights"
        colour, n = r.u8(), r.u8()
        body.shared_vertices = True
        body.vertices = [(0, r.u8()) for _ in range(n)]
        body.prims = [("lights", colour)]
        body.end = r.p
        return body
    r.u8()
    body.flags = b & 0xE0
    np_ = b & 0x1F
    wide = np_ > 16
    body.planes = [(r.s16(), r.s16(), r.s16(), r.s16()) for _ in range(np_)]

    nv = r.u8()
    if nv & 0x80:
        body.shared_vertices = True
        body.vertices = [(_mask(r, wide), r.u8()) for _ in range(nv & 0x7F)]
    else:
        body.vertices = [(_mask(r, wide), (r.s16(), r.s16(), r.s16())) for _ in range(nv)]

    ne = r.u8()
    body.edges = [(_mask(r, wide), r.u8(), r.u8()) for _ in range(ne)]

    n = r.u8()
    if n == 0xFF:
        # Plane-sorted mode: 2*np bytes of sort data, 1 byte, offsets[np], counts[np], prims.
        r.p += 2 * np_ + 1
        offs = [r.u16() for _ in range(np_)]
        counts = [r.u8() for _ in range(np_)]
        base = r.p
        body.sorted_groups = []
        end = base
        for o, c in zip(offs, counts):
            g = Reader(d, base + o)
            body.sorted_groups.append([_prim(g, wide) for _ in range(c)])
            end = max(end, g.p)
        body.prims = [p for g in body.sorted_groups for p in g]
        r.p = end
    else:
        body.prims = [_prim(r, wide) for _ in range(n)]
    body.end = r.p
    return body


def parse_shape(d, index, pos):
    r = Reader(d, pos)
    size = r.u8()
    lods = []
    while d[r.p] & 0x80:
        here = r.p
        lvl = r.u8() & 7
        lods.append((lvl, here + 1 + struct.unpack_from("<h", d, here + 1)[0]))
        r.p += 2
    return Shape(index, size, lods, parse_body(d, r.p))


@dataclass
class ShapeFile:
    data: bytes
    base: int
    offsets: list
    shapes: list
    shared: list    # shared vertex table (x, y, z), from the trailer (theatre files only)


def _trailer(d, pos):
    """Shared vertex table: m index triples into three coordinate lists."""
    if pos >= len(d) or d[pos] == 0:
        return []
    m = d[pos]
    pos += 1
    ia, ib, ic = d[pos:pos + m], d[pos + m:pos + 2 * m], d[pos + 2 * m:pos + 3 * m]
    pos += 3 * m
    lists = []
    for _ in range(3):
        k = d[pos]
        lists.append(struct.unpack_from(f"<{k}h", d, pos + 1))
        pos += 1 + 2 * k
    return [(lists[0][a], lists[1][b], lists[2][c]) for a, b, c in zip(ia, ib, ic)]


def load(path):
    d = open(path, "rb").read()
    _magic, n = struct.unpack_from("<2H", d, 0)
    offs = list(struct.unpack_from(f"<{n}H", d, 4))
    (length,) = struct.unpack_from("<H", d, 4 + 2 * n)
    base = 6 + 2 * n
    offs.append(length)
    shapes = [parse_shape(d, i, base + offs[i]) if offs[i + 1] > offs[i] else None for i in range(n)]
    return ShapeFile(d, base, offs, shapes, _trailer(d, base + length))


def resolve(sf, body):
    """Vertex positions for a body, resolving shared-table references."""
    if body.shared_vertices:
        return [sf.shared[i] if i < len(sf.shared) else (0, 0, 0) for _, i in body.vertices]
    return [p for _, p in body.vertices]
