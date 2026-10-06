#include "hires/scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/cpu.h"

namespace f19 {

// Parsing: shape data lives in one segment; offsets wrap at 64K. Cache keys
// are linear addresses.
namespace {

struct Rd {
    const Memory& m;
    uint16_t seg, off;
    uint8_t u8() { return m.read8(Memory::linear(seg, off++)); }
    uint8_t peek() const { return m.read8(Memory::linear(seg, off)); }
    uint16_t u16() {
        uint16_t v = m.read16(seg, off);
        off = uint16_t(off + 2);
        return v;
    }
    int16_t s16() { return int16_t(u16()); }
};

}  // namespace

std::shared_ptr<const SceneShape> ShapeCache::get(const Memory& mem, uint32_t linear, const Shared& sh) {
    auto it = shapes_.find(linear);
    if (it != shapes_.end()) return it->second;
    auto s = std::make_shared<SceneShape>();
    uint16_t seg = uint16_t(linear >> 4), off = uint16_t(linear & 15);
    s->size_class = mem.read8(linear);
    s->node = node(mem, Memory::linear(seg, uint16_t(off + 1)), sh, 0);
    const SceneNode* n = s->node.get();
    while (n && !n->body) n = n->near_node.get();
    if (n && n->body && n->body->kind == SceneBody::Mesh && !n->body->verts.empty()) {
        const SceneBody& b = *n->body;
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (auto& v : b.verts)
            for (int i = 0; i < 3; i++) lo[i] = std::min(lo[i], v[i]), hi[i] = std::max(hi[i], v[i]);
        int polys = 0;
        for (auto& p : b.prims) polys += p.poly;
        s->flat = b.flat;
        s->extent = std::max(hi[0] - lo[0], hi[1] - lo[1]);
        s->relief = !b.flat && polys <= 5 && hi[2] > 0;
    }
    shapes_[linear] = s;
    return s;
}

std::shared_ptr<const SceneNode> ShapeCache::node(const Memory& mem, uint32_t linear, const Shared& sh, int depth) {
    auto it = nodes_.find(linear);
    if (it != nodes_.end()) return it->second;
    auto n = std::make_shared<SceneNode>();
    uint8_t b = mem.read8(linear);
    if ((b & 0x80) && depth < 16) {
        // LOD record: 0x80|level, s16 rel; far target = (address of rel) + rel.
        uint16_t seg = uint16_t(linear >> 4), off = uint16_t(linear & 15);
        int16_t rel = int16_t(mem.read16(seg, uint16_t(off + 1)));
        n->lod_level = b & 7;
        n->far_node = node(mem, Memory::linear(seg, uint16_t(off + 1 + rel)), sh, depth + 1);
        n->near_node = node(mem, Memory::linear(seg, uint16_t(off + 3)), sh, depth + 1);
    } else {
        n->body = body(mem, linear, sh);
    }
    nodes_[linear] = n;
    return n;
}

std::shared_ptr<const SceneBody> ShapeCache::body(const Memory& mem, uint32_t linear, const Shared& sh) {
    auto it = bodies_.find(linear);
    if (it != bodies_.end()) return it->second;
    auto bd = std::make_shared<SceneBody>();
    Rd r{mem, uint16_t(linear >> 4), uint16_t(linear & 15)};
    auto shared_vertex = [&](uint8_t i) -> std::array<float, 3> {
        auto val = [&](uint16_t idx_tab, uint16_t val_tab) {
            uint8_t k = mem.read8(Memory::linear(sh.ds, uint16_t(idx_tab + i)));
            return float(int16_t(mem.read16(sh.ds, uint16_t(val_tab + 2 * k))));
        };
        return {val(sh.xi, sh.xv), val(sh.yi, sh.yv), val(sh.zi, sh.zv)};
    };
    uint8_t b = r.peek();
    bd->first = b;
    if ((b & 0x60) == 0x60) {
        bd->anim_axis = b & 3;
        r.u8();
        b = r.peek();
    }
    uint8_t t = b & 0x3F;
    if (t == 0x3F) {
        r.u8();
        bd->kind = SceneBody::Point;
        bd->point_color = r.u8();
    } else if (t == 0x3E) {
        // Ground lights from the shared vertex table: [3E][xx][n][index * n].
        r.u8();
        r.u8();
        bd->kind = SceneBody::Lights;
        bd->flat = true;
        int n = r.u8();
        for (int i = 0; i < n; i++) bd->verts.push_back(shared_vertex(r.u8()));
    } else {
        r.u8();
        int np = b & 0x1F;
        bool wide = np > 16;
        auto mask = [&]() { uint32_t m = r.u16(); if (wide) m |= uint32_t(r.u16()) << 16; return m; };
        for (int i = 0; i < np; i++) {
            int16_t nx = r.s16(), ny = r.s16(), nz = r.s16(), d = r.s16();
            bd->planes.push_back({nx, ny, nz, d});
        }
        uint8_t nv = r.u8();
        if (nv & 0x80) {
            for (int i = 0; i < (nv & 0x7F); i++) {
                mask();
                bd->verts.push_back(shared_vertex(r.u8()));
            }
        } else {
            for (int i = 0; i < nv; i++) {
                mask();
                float x = r.s16(), y = r.s16(), z = r.s16();
                bd->verts.push_back({x, y, z});
            }
        }
        uint8_t ne = r.u8();
        std::vector<std::array<uint8_t, 2>> edges(ne);
        for (auto& e : edges) {
            mask();
            e[0] = r.u8();
            e[1] = r.u8();
        }
        auto prim = [&](Rd& pr) {
            SceneBody::Prim p{};
            uint8_t pb = pr.u8();
            if ((pb & 3) == 1) {
                p.poly = true;
                p.plane = (pb >> 3) & 0x1F;
                int n = pr.u8();
                std::vector<std::array<uint8_t, 2>> es;
                for (int i = 0; i < n; i++) {
                    uint8_t e = pr.u8();
                    if (e < edges.size()) es.push_back(edges[e]);
                }
                p.color = pr.u8();
                // Chain the edges into a vertex loop.
                if (es.size() >= 2) {
                    std::vector<uint8_t> loop = {es[0][0], es[0][1]};
                    es.erase(es.begin());
                    while (!es.empty()) {
                        bool linked = false;
                        for (size_t i = 0; i < es.size(); i++) {
                            if (es[i][0] == loop.back()) loop.push_back(es[i][1]);
                            else if (es[i][1] == loop.back()) loop.push_back(es[i][0]);
                            else continue;
                            es.erase(es.begin() + long(i));
                            linked = true;
                            break;
                        }
                        if (!linked) break;
                    }
                    if (loop.size() > 1 && loop.front() == loop.back()) loop.pop_back();
                    p.v = loop;
                }
            } else {
                p.poly = false;
                p.mask = uint32_t(pr.u16());
                if (wide) p.mask |= uint32_t(pr.u16()) << 16;
                uint8_t e = pr.u8();
                p.color = pr.u8();
                if (e < edges.size()) p.v = {edges[e][0], edges[e][1]};
            }
            bool ok = p.poly ? p.v.size() >= 3 : p.v.size() == 2;
            for (uint8_t v : p.v) ok = ok && v < bd->verts.size();
            if (ok && p.color != 0xFF) bd->prims.push_back(std::move(p));
        };
        uint8_t n = r.u8();
        if (n == 0xFF) {
            // Plane-sorted: 2*np sort bytes, 1 byte, u16 offset[np], u8 count[np], groups.
            // The sort data is the BSP root, then each plane's two children.
            bd->sort_root = r.u8();
            bd->sort_tree.resize(np);
            for (auto& t : bd->sort_tree) t = {r.u8(), r.u8()};
            std::vector<uint16_t> offs(np);
            for (auto& o : offs) o = r.u16();
            std::vector<uint8_t> counts(np);
            for (auto& c : counts) c = r.u8();
            uint16_t base = r.off;
            for (int g = 0; g < np; g++) {
                Rd gr{mem, r.seg, uint16_t(base + offs[g])};
                size_t first = bd->prims.size();
                for (int i = 0; i < counts[g]; i++) prim(gr);
                bd->sort_groups.push_back({uint16_t(first), uint16_t(bd->prims.size() - first)});
            }
        } else {
            for (int i = 0; i < n; i++) prim(r);
        }
        bd->flat = !bd->verts.empty();
        for (auto& v : bd->verts) bd->flat = bd->flat && v[2] == 0;
        // Lines on the ground plane, and lines that outline a polygon (a
        // runway's border) rather than standing alone (a road).
        auto same = [&](uint8_t a, uint8_t b) {
            return bd->verts[a] == bd->verts[b];
        };
        for (auto& p : bd->prims) {
            if (p.poly) continue;
            p.ground = bd->verts[p.v[0]][2] == 0 && bd->verts[p.v[1]][2] == 0;
            for (const auto& q : bd->prims) {
                if (!q.poly) continue;
                for (size_t i = 0; i < q.v.size() && !p.outline; i++) {
                    uint8_t a = q.v[i], b = q.v[(i + 1) % q.v.size()];
                    p.outline = (same(a, p.v[0]) && same(b, p.v[1])) || (same(a, p.v[1]) && same(b, p.v[0]));
                }
            }
        }
    }
    bodies_[linear] = bd;
    return bd;
}

// The engine's plane-sorted draw order (EGAME 2000:03E4): an in-order walk
// of the BSP tree from the root. At a plane facing the eye, child 1 (the
// far side) is drawn first, then the plane's group, then child 0; at a
// plane facing away, the other way round.
std::vector<uint16_t> SceneBody::draw_order(uint32_t visible) const {
    std::vector<uint16_t> order;
    if (sort_root < 0) {
        for (size_t i = 0; i < prims.size(); i++) order.push_back(uint16_t(i));
        return order;
    }
    const size_t np = sort_tree.size();
    std::vector<bool> seen(np);
    auto walk = [&](auto& self, int node, int depth) -> void {
        if (node == 0xFF || size_t(node) >= np || seen[size_t(node)] || depth > 64) return;
        seen[size_t(node)] = true;
        bool facing = node < 32 && (visible >> node & 1);
        const auto& t = sort_tree[size_t(node)];
        self(self, facing ? t[1] : t[0], depth + 1);
        if (size_t(node) < sort_groups.size())
            for (int i = 0; i < sort_groups[size_t(node)][1]; i++) order.push_back(uint16_t(sort_groups[size_t(node)][0] + i));
        self(self, facing ? t[0] : t[1], depth + 1);
    };
    walk(walk, sort_root, 0);
    for (size_t g = 0; g < np && g < sort_groups.size(); g++)  // malformed tree: draw the rest anyway
        if (!seen[g])
            for (int i = 0; i < sort_groups[g][1]; i++) order.push_back(uint16_t(sort_groups[g][0] + i));
    return order;
}

// Transform (from the engine's object routines). Coordinates in "u" order
// (x, z, y) = (east, up, north). With M the 9 words at 0x9B8 (Q15), camera
// space is X = M0 x + M6 y + M3 z, Y = M1 x + M7 y + M4 z, Z = M2 x + M8 y +
// M5 z, i.e. cam = M^T u. A rotated object uses C = D M (row-major 3x3)
// instead, D built from its three angles; its plane test uses the eye
// e_u = -D u_rel. Units are the instance's (terrain level or the dynamic
// object's shift); `scale` converts to world units.
namespace {

std::array<float, 3> cam_of(const float* m, float x, float y, float z) {
    return {m[0] * x + m[6] * y + m[3] * z, m[1] * x + m[7] * y + m[4] * z, m[2] * x + m[8] * y + m[5] * z};
}

void angle_matrix(const uint16_t a[3], float* d) {
    auto sc = [](uint16_t ang, float& s, float& c) {
        double r = double(ang) * (2.0 * M_PI / 65536.0);
        s = float(std::sin(r));
        c = float(std::cos(r));
    };
    float sa, ca, sb, cb, sc_, cc;
    sc(a[0], sa, ca);   // 0x988
    sc(a[2], sb, cb);   // 0x98C
    sc(a[1], sc_, cc);  // 0x98A
    float t1 = sc_ * sb, t2 = sc_ * cb;
    d[0] = ca * cb - t1 * sa;
    d[3] = t2 * sa + ca * sb;
    d[6] = -(sa * cc);
    d[1] = -(sb * cc);
    d[4] = cb * cc;
    d[7] = sc_;
    d[2] = t1 * ca + sa * cb;
    d[5] = sa * sb - t2 * ca;
    d[8] = ca * cc;
}

void mul3(const float* a, const float* b, float* c) {
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) c[r * 3 + k] = a[r * 3] * b[k] + a[r * 3 + 1] * b[3 + k] + a[r * 3 + 2] * b[6 + k];
}

}  // namespace

void build_scene_prims(const Scene& s, const SceneBuildParams& bp, std::vector<HiresPrim>& out) {
    out.clear();
    struct Item { size_t idx; bool immediate; float key; };
    std::vector<Item> items;
    items.reserve(s.instances.size());
    for (size_t i = 0; i < s.instances.size(); i++) {
        const SceneInstance& in = s.instances[i];
        float rz = float(in.pos[2] - in.cam[2]);
        auto c = cam_of(s.view, float(in.pos[0] - in.cam[0]), float(in.pos[1] - in.cam[1]), rz);
        bool immediate = in.level > 0 && in.pos[2] == 0;
        items.push_back({i, immediate, immediate ? 0.0f : c[2] * in.scale});
    }
    // Ground objects in the engine's painter order; the rest far to near.
    std::stable_sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
        if (a.immediate != b.immediate) return a.immediate;
        const SceneInstance &ia = s.instances[a.idx], &ib = s.instances[b.idx];
        if (a.immediate) {
            if (ia.level != ib.level) return ia.level > ib.level;
            return ia.order > ib.order;
        }
        return a.key > b.key;
    });

    HiresProj proj{};
    proj.vp_w = 320;
    proj.vp_h = 200;
    proj.zdiv = 1;
    uint32_t object = 0;
    for (const Item& it : items) {
        const SceneInstance& in = s.instances[it.idx];
        if (!in.shape || !in.shape->node) continue;
        // rel and o (camera space) in instance units; the engine's
        // decisions (LOD, face visibility, shading) are in shape units
        // (instance units / size).
        const float sz = in.size;
        float rel[3] = {float(in.pos[0] - in.cam[0]), float(in.pos[1] - in.cam[1]), float(in.pos[2] - in.cam[2])};
        auto o = cam_of(s.view, rel[0], rel[1], rel[2]);
        // Level of detail: the engine's distance estimate and thresholds,
        // scaled for our resolution.
        // Ground decals are cheap and their far bodies crude (a runway
        // becomes one line), so they keep detail farther.
        const bool decal = in.level > 0 && in.shape->flat;
        float dist = (std::fabs(o[2]) + (std::fabs(o[0]) + std::fabs(o[1])) * 0.25f) / sz / bp.lod_scale /
                     (decal ? bp.ground_lod : 1.0f);
        const bool relief = in.level >= 2 && in.shape->relief && in.shape->extent * sz * in.scale >= bp.relief_extent;
        const SceneNode* n = in.shape->node.get();
        while (n && !n->body) n = (dist > float(s.lod_table[n->lod_level & 7]) ? n->far_node : n->near_node).get();
        if (!n || !n->body) continue;
        const SceneBody& b = *n->body;
        uint16_t ang[3] = {in.angle[0], in.angle[1], in.angle[2]};
        if (b.anim_axis == 0) rel[2] = float(s.anim) * sz;
        else if (b.anim_axis > 0) ang[b.anim_axis - 1] = uint16_t(s.anim);
        if (b.anim_axis == 0) o = cam_of(s.view, rel[0], rel[1], rel[2]);
        bool immediate = !(b.first & 0x40) && in.level > 0 && rel[2] == float(-in.cam[2]);
        bool rotated = ang[0] | ang[1] | ang[2];
        float C[9], D[9];
        float eye[3];  // x, y, z (shape axes)
        if (rotated) {
            angle_matrix(ang, D);
            mul3(D, s.view, C);
            float u[3] = {rel[0] / sz, rel[2] / sz, rel[1] / sz};
            float e[3];
            for (int r = 0; r < 3; r++) e[r] = -(D[r * 3] * u[0] + D[r * 3 + 1] * u[1] + D[r * 3 + 2] * u[2]);
            eye[0] = e[0];
            eye[1] = e[2];
            eye[2] = e[1];
        } else {
            std::copy_n(s.view, 9, C);
            eye[0] = -rel[0] / sz;
            eye[1] = -rel[1] / sz;
            eye[2] = -rel[2] / sz;
        }
        uint32_t visible = 0xFFFFFFFFu;
        for (size_t k = 0; k < b.planes.size() && k < 32; k++) {
            const auto& pl = b.planes[k];
            if (double(pl[0]) * eye[0] + double(pl[1]) * eye[1] + double(pl[2]) * eye[2] <= double(pl[3])) visible &= ~(1u << k);
        }
        // Night/haze palette offset (0x8DC) from the depth's high byte.
        uint8_t shade = 0;
        if (s.night) {
            int zhi = int(std::floor(o[2] / sz));
            int al = int8_t(uint8_t(zhi >> 8));
            if (al < 0) al = 0;
            int ah = (al >> 1) - in.shape->size_class;
            ah = std::clamp(ah, 0, 7);
            shade = uint8_t(0x80 + (ah << 4));
        }
        const float k = in.scale * 65536.0f;
        auto vert = [&](const std::array<float, 3>& v) {
            auto c = cam_of(C, v[0] * sz, v[1] * sz, v[2] * sz);
            return std::array<float, 3>{(o[0] + c[0]) * k, (o[1] + c[1]) * k, (o[2] + c[2]) * k};
        };
        object++;
        HiresPrim base;
        base.object = object;
        base.proj = proj;
        base.flat = int8_t(b.flat && immediate && !rotated ? 1 : 0);
        if (b.kind == SceneBody::Point) {
            if (o[2] < 1) continue;
            HiresPrim p = base;
            p.kind = HiresPrim::Dot;
            p.color = uint8_t(s.remap[b.point_color & 15] + shade);
            p.v = {{o[0] * k, o[1] * k, o[2] * k}};
            out.push_back(std::move(p));
            continue;
        }
        if (b.kind == SceneBody::Lights) {
            for (auto& v : b.verts) {
                auto c = cam_of(C, v[0] * sz, v[1] * sz, v[2] * sz);
                float z = (o[2] + c[2]) / sz;
                // Ground lights are speed cues near the aircraft: beyond
                // lights_range they only add noise along the horizon.
                if (z < 1 || z > bp.lights_range) continue;
                HiresPrim p = base;
                p.kind = HiresPrim::Dot;
                p.flat = 1;
                p.color = s.remap[z > 0x1388 ? 8 : z > 0x9C4 ? 7 : 15];
                p.v = {{(o[0] + c[0]) * k, (o[1] + c[1]) * k, (o[2] + c[2]) * k}};
                out.push_back(std::move(p));
            }
            continue;
        }
        std::vector<std::array<float, 3>> cv(b.verts.size());
        for (size_t i = 0; i < b.verts.size(); i++) cv[i] = vert(b.verts[i]);
        const auto up = cam_of(C, 0, 0, 1);  // the shape's ground-plane normal
        for (uint16_t pi : b.draw_order(visible)) {
            const auto& pr = b.prims[pi];
            if (pr.poly ? !(visible & (1u << (pr.plane & 31))) : !(visible & pr.mask)) continue;
            HiresPrim p = base;
            p.kind = pr.poly ? HiresPrim::Poly : HiresPrim::Line;
            p.color = uint8_t(s.remap[pr.color & 15] + shade);
            for (uint8_t v : pr.v) p.v.push_back(cv[v]);
            if (!pr.poly && !bp.classic_lines) {
                if (relief) {
                    p.line_style = HiresPrim::Relief;
                } else if (pr.ground) {
                    p.line_style = HiresPrim::Ground;
                    p.normal = up;
                    p.width = b.flat && !pr.outline ? bp.road_width * 65536.0f : bp.edge_width * sz * k;
                } else {
                    p.line_style = HiresPrim::Edge;
                    p.width = bp.edge_width * sz * k;
                }
            }
            out.push_back(std::move(p));
        }
    }
}

}  // namespace f19
