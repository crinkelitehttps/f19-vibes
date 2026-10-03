#include "hires/world_capture.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <optional>
#include <set>

#include "core/machine.h"
#include "drivers/mgraphic.h"
#include "hires/scene.h"

namespace f19 {

namespace {

// Signature: hex bytes with ?? wildcards (kept in sync with tools/engine_sigs.py).
struct Sig {
    const char* pattern;
    std::vector<int> bytes;  // -1 = wildcard
};

std::vector<int> compile(const char* p) {
    std::vector<int> out;
    for (const char* s = p; *s;) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (s[0] == '?') out.push_back(-1);
        else out.push_back(int(std::strtol(std::string(s, 2).c_str(), nullptr, 16)));
        s += 2;
    }
    return out;
}

std::vector<uint32_t> find_all(const uint8_t* mem, uint32_t lo, uint32_t hi, const char* pattern) {
    std::vector<int> pat = compile(pattern);
    std::vector<uint32_t> out;
    for (uint32_t a = lo; a + pat.size() <= hi; a++) {
        bool ok = true;
        for (size_t k = 0; k < pat.size() && ok; k++)
            if (pat[k] >= 0 && mem[a + k] != pat[k]) ok = false;
        if (ok) out.push_back(a);
    }
    return out;
}

const char* kProj = "8b 8f ?? ?? 80 3e ?? ?? 00 74 02 d1 e1 80 3e ?? ?? 00 74 0a 87 d1 8a 0e ?? ?? d3 fa 87 d1 "
                    "0b c9 7e 4d 8b 97 ?? ?? 8a 87 ?? ?? 98 92 f7 f9 99 0b c0 03 06 ?? ??";
const char* kProjY = "8b 97 ?? ?? 8a 87 ?? ?? 98 92 8b f2 8b f8 d1 fa d1 d8 d1 fa d1 d8 2b c7 1b d6 f7 f9 99 "
                     "0b c0 03 06 ?? ??";
const char* kEdgeLoop = "2a e4 26 ac 0b c0 74 15 8b c8 2b ff 81 c7 ?? ?? e8";
const char* kEdgeSetup = "2a e4 26 ac 8b d8 d1 e3 d1 e3 26 ac 8b e8 d1 e5 d1 e5 c6 45 18 00";
const char* kPoly = "8b fb 8a a5 ?? ?? 02 26 ?? ?? 9a";
const char* kLine = "26 ac 2a e4 8b f8 8a a5 ?? ?? 02 26 ?? ?? 9a ?? ?? ?? ?? 8b 4f 08 8b 57 0c";
const char* kDot = "a1 ?? ?? a3 ?? ?? a3 ?? ?? a1 ?? ?? a3 ?? ?? a3 ?? ?? e8";
const char* kHorizon = "a1 ?? ?? f7 d8 a3 ?? ?? a1 ?? ?? 99 8a d4 8a e0 2a c0 8b 0e ?? ?? 81 f9 0b 1f";
const char* kHorizonCos = "a1 ?? ?? d1 e0 f7 2e ?? ?? d1 e0 d1 d2 8b f0";
const char* kHorizonMode = "80 3e ?? ?? 02 75 15 8a 16 ?? ??";
const char* kHorizonSky = "c7 06 ?? ?? 00 00 8a 26 ?? ?? 9a";
const char* kHorizonGround = "83 3e ?? ?? 00 78 13 8a 26 ?? ?? 9a";
const char* kViewport = "8b d0 a1 ?? ?? 3b f0 77 10 3b e8 77 0c a1 ?? ??";
// Vertex transform loop: one call per shape body drawn (object boundary).
const char* kVertexLoop = "26 ac a8 80 75 ?? a8 7f 74 ?? 8a d8";
const char* kCLine = "e8 0d 00 cb 55 56 57 06 e8 05 00 07 5f 5e 5d cb";
// Rectangle fill called from C (2D, e.g. the TrackCam background).
const char* kRectFill = "55 8b ec 57 56 55 1e 07 9a ?? ?? ?? ?? 50 8b 5e 06 8b 07 9a";

// Scene capture (see scene.h; offsets of the operands read are noted).
// Shape draw entry (far): stack = shape far ptr, 3 angles, x, y, z.
const char* kShapeDraw = "55 8b ec 56 57 8b 46 0a a3 ?? ?? 8b 46 0c a3 ?? ?? 8b 46 0e a3 ?? ?? c4 76 06 89 36 ?? ?? "
                         "8c 06 ?? ?? 26 ac a2 ?? ?? 8b 5e 12 2b 1e ?? ?? 89 1e ?? ?? 8b 4e 14 2b 0e ?? ?? 89 0e ?? ?? "
                         "8b 6e 10 2b 2e ?? ??";  // angles 9/15/21, camera y 44, z 55, x 66
const char* kCull = "56 a1 ?? ?? f7 eb 8b fa 8b f0 a1 ?? ?? f7 e9 03 f0 13 fa a1 ?? ?? f7 ed";  // matrix 20
const char* kLod = "26 8a 04 a8 80 74 21 25 07 00 d1 e0 8b d8 a1 ?? ?? 8a 0e ?? ?? d3 f8 3b 87 ?? ??";  // shift 19, table 25
const char* kAnim = "26 ac 25 03 00 d1 e0 8b d8 a1 ?? ?? 89 87 ?? ??";  // 10
const char* kShadeFlag = "8a 26 ?? ?? 0a e4 74 28 a0 ?? ?? 98 f6 d4 22 c4 2a e4 d1 e8";  // 2
const char* kShared = "26 ac 2a e4 8b f8 8a 9d ?? ?? 2a ff d1 e3 8b 8f ?? ?? 8a 9d ?? ?? 2a ff d1 e3 8b 87 ?? ?? "
                      "a3 ?? ?? 8a 9d ?? ?? 2a ff d1 e3 8b 9f ?? ??";  // zi 8, zv 16, yi 20, yv 28, xi 35, xv 43
// Terrain loop entry (near): heading, pitch, camera x, y, z (32-bit each).
const char* kTerrain = "55 8b ec 83 ec 1c 56 8b 46 08 8b 56 0a a3 ?? ?? 89 16 ?? ?? 8b 46 0c 8b 56 0e a3";
const char* kTerrainLists = "a3 ?? ?? 8b 76 e6 d1 e6 8b 1e ?? ?? b1 06 d3 e3 8b 80 ?? ?? a3 ?? ?? c7 46 f6 00 00 eb 48 "
                            "8b 1e ?? ?? 8a 5f 06 2a ff d1 e3 8b 87 ?? ?? 05 00 00 a3 ?? ?? c7 06 ?? ?? ?? ??";
                            // level 10, lists 18, shape table 43, shape segment 55
const char* kTerrainCounts = "8b 46 f6 39 80 ?? ?? 77 03";  // 5
const char* kTerrainEnable = "83 3e ?? ?? 01 7d 03 e9 ?? ?? 8b 1e ?? ?? d1 e3 83 bf ?? ?? 00";  // 18
const char* kTileSizes = "8b b7 ?? ?? 39 76 06";  // 2
const char* kTileL4 = "8b 76 08 b1 03 d3 e6 8b 5e 06 8a 80 ?? ??";  // 12
const char* kTileL3 = "8b 76 08 b1 04 d3 e6 8b 5e 06 8a 80 ?? ??";  // 12
const char* kTileExp3 = "b8 03 00 50 e8 ?? ?? 83 c4 06 b1 04 d3 e0 8b 76 08 83 e6 03 d1 e6 d1 e6 03 f0 8b 5e 06 83 e3 03 8a 80 ?? ??";
const char* kTileExp2 = "b8 02 00 50 e8 ?? ?? 83 c4 06 b1 04 d3 e0 8b 76 08 83 e6 03 d1 e6 d1 e6 03 f0 8b 5e 06 83 e3 03 8a 80 ?? ??";
const char* kTileExp1 = "b8 01 00 50 e8 ?? ?? 83 c4 06 b1 04 d3 e0 8b 76 08 83 e6 03 d1 e6 d1 e6 03 f0 8b 5e 06 83 e3 03 8a 80 ?? ??";  // 34
// Dynamic object draw (near, 10 args; arg 10 = scale shift).
const char* kObjectDraw = "55 8b ec 83 ec 10 ff 76 04 e8 ?? ?? 83 c4 02 89 46 fc 80 3e ?? ?? 00 75 05 a1";
// Its camera arithmetic (x1 26, y1 46, alt1 70, view flags 77, x2 91, y2 104, alt2 129, zoom 136).
const char* kObjectCamera = "80 3e ?? ?? 00 75 05 a1 ?? ?? eb 03 a1 ?? ?? 89 46 fe 8b 46 06 8b 56 08 2b 06 ?? ?? 1b 16 ?? ?? "
                            "89 46 f8 89 56 fa 8b 46 0a 8b 56 0c 03 06 ?? ?? 13 16 ?? ?? 2d 00 00 81 da 00 01 89 46 f4 89 56 f6 "
                            "8b 46 0e 2b 06 ?? ?? 89 46 f2 f6 06 ?? ?? 80 74 34 a1 ?? ?? 8b 16 ?? ?? 2b 06 ?? ?? 1b 16 ?? ?? "
                            "01 46 f8 11 56 fa a1 ?? ?? 8b 16 ?? ?? 2b 06 ?? ?? 1b 16 ?? ?? 01 46 f4 11 56 f6 a1 ?? ?? 2b 06 ?? ?? "
                            "01 46 f2 80 3e ?? ??";
// Shape code -> shape offset (theatre table 20; STFLT table 31 + offset 35).
const char* kShapeCode = "55 8b ec f7 46 04 00 01 74 0e 8b 5e 04 83 e3 7f d1 e3 8b 87 ?? ?? eb 18 8b 5e 04 d1 e3 8b 9f ?? ?? 8d 87 ?? ??";
// Main object list: count 1, flags field 21 (record = flags - 0x16, 0x24 bytes); its shape draw (type field 2,
// shape code table 10; the call returns to match + 15).
const char* kObjectList = "a1 ?? ?? 39 46 e0 7c 03 e9 ?? ?? b8 24 00 f7 6e e0 8b d8 f6 87 ?? ?? 02";
const char* kObjectListDraw = "8b 9c ?? ?? b1 05 d3 e3 ff b1 ?? ?? e8 ?? ?? 83 c4 14";

}  // namespace

WorldCapture::~WorldCapture() = default;

WorldCapture::WorldCapture(Machine& m, MGraphicNative& gfx) : m_(m), gfx_(gfx), shapes_(std::make_unique<ShapeCache>()) {
    edges_.resize(1024);
    mask_.assign(64000, 0);
    // The flight program is EXEPACK-compressed: scan for the engine once it
    // has unpacked itself, i.e. at its first DOS call.
    m_.program_listeners.push_back([this](const std::string& name, uint16_t seg, uint32_t size) {
        pending_ = {name, seg, size};
        has_pending_ = name == "EGAME.EXE" || name == "DGAME.EXE";
    });
    m_.dos_call_listeners.push_back([this] {
        if (!has_pending_) return;
        has_pending_ = false;
        on_program(pending_.name, pending_.seg, pending_.size);
    });
    gfx_.on_flip = [this] { flip(); };
    // A driver call is world drawing if the 3D engine makes it after a
    // capture hook fired (polygon, line, dot, horizon). Calls from anywhere
    // else end the world run.
    gfx_.is_world_call = [this] {
        if (!found_) return false;
        const Regs& r = m_.cpu.regs;
        uint16_t ret_cs = m_.mem.read16(r.s[SS], uint16_t(r.r[SP] + 2));
        bool world = ret_cs == engine_cs_ && world_flag_;
        // Only calls that draw end a world run: the timer interrupt's
        // palette/shake call (slot 46) and other queries can arrive in the
        // middle of the engine's drawing.
        static const std::set<int> kDrawing = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 17, 18, 19, 20, 31, 36, 37, 40,
                                               41, 42, 43, 48, 51, 52, 59, 71, 72, 73, 74};
        if (ret_cs != engine_cs_ && kDrawing.count(gfx_.current_slot())) world_flag_ = false;
        return world;
    };
}

uint16_t WorldCapture::ds16(uint16_t off) const { return m_.mem.read16(m_.cpu.regs.s[DS], off); }
uint8_t WorldCapture::ds8(uint16_t off) const { return m_.mem.read8(Memory::linear(m_.cpu.regs.s[DS], off)); }

void WorldCapture::on_program(const std::string& name, uint16_t seg, uint32_t size) {
    if (!enabled || (name != "EGAME.EXE" && name != "DGAME.EXE")) return;
    const uint8_t* mem = m_.mem.data();
    uint32_t lo = Memory::linear(seg, 0), hi = std::min<uint32_t>(lo + size, Memory::kSize);
    auto one = [&](const char* p, const char* what) -> std::optional<uint32_t> {
        auto hits = find_all(mem, lo, hi, p);
        if (hits.size() != 1) {
            status += std::string(what) + ": " + std::to_string(hits.size()) + " matches; ";
            return std::nullopt;
        }
        return hits[0];
    };
    auto w = [&](uint32_t a) { return uint16_t(mem[a] | (mem[a + 1] << 8)); };
    found_ = false;
    status.clear();
    auto proj = one(kProj, "proj"), projy = one(kProjY, "proj_y"), eloop = one(kEdgeLoop, "edge_loop"),
         esetup = one(kEdgeSetup, "edge_setup"), poly = one(kPoly, "poly"), line = one(kLine, "line"),
         hor = one(kHorizon, "horizon"), hcos = one(kHorizonCos, "horizon_cos"), hmode = one(kHorizonMode, "horizon_mode"),
         hsky = one(kHorizonSky, "horizon_sky"), hgnd = one(kHorizonGround, "horizon_ground"),
         vp = one(kViewport, "viewport"), cline = one(kCLine, "c_line");
    auto dots = find_all(mem, lo, hi, kDot);
    auto vloop = one(kVertexLoop, "vertex_loop");
    if (!proj || !projy || !eloop || !esetup || !poly || !line || !hor || !hcos || !hmode || !hsky || !hgnd || !vp ||
        !cline || !vloop || dots.empty()) {
        status = "engine signatures not found: " + status;
        m_.log("hires: %s\n", status.c_str());
        return;
    }
    z_tab_ = uint16_t(w(*proj + 2) - 2);
    zoom_ = w(*proj + 6);
    zshift_ = w(*proj + 15);
    x_tab_ = uint16_t(w(*proj + 36) - 1);
    cx_ = w(*proj + 51);
    y_tab_ = uint16_t(w(*projy + 2) - 1);
    cy_ = w(*projy + 33);
    edge_base_ = w(*eloop + 14);
    up_s_ = w(*hor + 1);
    up_n_ = w(*hor + 9);
    up_d_ = w(*hor + 20);
    cos_ = w(*hcos + 7);
    view_mode_ = w(*hmode + 2);
    alt_ = w(*hmode + 9);
    sky_ = w(*hsky + 8);
    ground_ = w(*hgnd + 9);
    vp_x_ = w(*vp + 3);
    vp_y_ = w(*vp + 14);
    c_line_lin_ = *cline;

    m_.add_breakpoint(*esetup, [this] { edge_setup(); return false; });
    m_.add_breakpoint(*vloop, [this] { object_++; return false; });
    m_.add_breakpoint(*poly + 10, [this] { capture_poly(); return false; });
    poly_lin_ = *poly + 10;
    m_.add_breakpoint(*line + 14, [this] { capture_line(); return false; });
    for (uint32_t d : dots) m_.add_breakpoint(d + 18, [this] { capture_dot(); return false; });
    m_.add_breakpoint(*hor, [this] { capture_horizon(); return false; });
    // C-callable 2D routines that live in the engine segment end world
    // drawing until the next capture hook.
    for (uint32_t entry : {*cline, *cline + 4}) m_.add_breakpoint(entry, [this] { world_flag_ = false; return false; });
    for (uint32_t entry : find_all(mem, lo, hi, kRectFill)) m_.add_breakpoint(entry, [this] { world_flag_ = false; return false; });
    found_ = true;

    // Scene capture: optional (the captured primitives remain the fallback).
    {
        std::string missing;
        auto opt = [&](const char* p, const char* what) -> std::optional<uint32_t> {
            auto hits = find_all(mem, lo, hi, p);
            if (hits.size() != 1) missing += std::string(" ") + what;
            return hits.size() == 1 ? std::optional<uint32_t>(hits[0]) : std::nullopt;
        };
        auto sd = opt(kShapeDraw, "shape_draw"), cu = opt(kCull, "cull"), lod = opt(kLod, "lod"), an = opt(kAnim, "anim"),
             sf = opt(kShadeFlag, "shade"), sh = opt(kShared, "shared"), te = opt(kTerrain, "terrain"),
             tl = opt(kTerrainLists, "terrain_lists"), tc = opt(kTerrainCounts, "terrain_counts"),
             ten = opt(kTerrainEnable, "terrain_enable"), ts = opt(kTileSizes, "tile_sizes"), t4 = opt(kTileL4, "tile_l4"),
             t3 = opt(kTileL3, "tile_l3"), e3 = opt(kTileExp3, "tile_exp3"), e2 = opt(kTileExp2, "tile_exp2"),
             e1 = opt(kTileExp1, "tile_exp1"), od = opt(kObjectDraw, "object_draw");
        scene_found_ = missing.empty() && !std::getenv("F19_NO_SCENE");
        if (scene_found_) {
            angle_[0] = w(*sd + 9);
            angle_[1] = w(*sd + 15);
            angle_[2] = w(*sd + 21);
            cam_off_[0] = w(*sd + 66);
            cam_off_[1] = w(*sd + 44);
            cam_off_[2] = w(*sd + 55);
            matrix_ = w(*cu + 20);
            lod_shift_ = w(*lod + 19);
            lod_table_ = w(*lod + 25);
            anim_ = w(*an + 10);
            night_ = w(*sf + 2);
            color_table_ = w(*poly + 4);
            shared_[0] = w(*sh + 35);   // xi
            shared_[1] = w(*sh + 20);   // yi
            shared_[2] = w(*sh + 8);    // zi
            shared_[3] = w(*sh + 43);   // xv
            shared_[4] = w(*sh + 28);   // yv
            shared_[5] = w(*sh + 16);   // zv
            level_ = w(*tl + 10);
            lists_ = w(*tl + 18);
            shape_table_ = w(*tl + 43);
            shape_seg_lin_ = *tl + 55;   // relocated segment immediate: read at run time
            counts_ = w(*tc + 5);
            enable_ = w(*ten + 18);
            sizes_ = w(*ts + 2);
            map4_ = w(*t4 + 12);
            map3_ = w(*t3 + 12);
            exp3_ = w(*e3 + 34);
            exp2_ = w(*e2 + 34);
            exp1_ = w(*e1 + 34);
            terrain_lo_ = *te;
            shapes_->clear();
            overrides_.clear();
            m_.add_breakpoint(*te, [this] { on_terrain(); return false; });
            m_.add_breakpoint(*sd, [this] { on_shape_draw(); return false; });
            auto oc = opt(kObjectCamera, "object_camera"), sc = opt(kShapeCode, "shape_code"), ol = opt(kObjectList, "object_list"),
                 old_ = opt(kObjectListDraw, "object_list_draw");
            objects_found_ = oc && sc && ol && old_;
            if (objects_found_) {
                oc_x1_ = w(*oc + 26);
                oc_y1_ = w(*oc + 46);
                oc_a1_ = w(*oc + 70);
                oc_view_ = w(*oc + 77);
                oc_x2_ = w(*oc + 91);
                oc_y2_ = w(*oc + 104);
                oc_a2_ = w(*oc + 129);
                oc_zoom_ = w(*oc + 136);
                code_theatre_ = w(*sc + 20);
                code_stflt_ = w(*sc + 31);
                code_add_ = w(*sc + 35);
                list_count_ = w(*ol + 1);
                list_base_ = uint16_t(w(*ol + 21) - 0x16);
                list_type_ = w(*old_ + 2);
                list_codes_ = w(*old_ + 10);
                list_draw_ret_ = *old_ + 15;
            }
            m_.add_breakpoint(*od, [this] {
                const Regs& r = m_.cpu.regs;
                uint32_t ret = Memory::linear(r.s[CS], m_.mem.read16(r.s[SS], r.r[SP]));
                pending_shift_ = int16_t(m_.mem.read16(r.s[SS], uint16_t(r.r[SP] + 20)));
                pending_skip_ = objects_found_ && ret == list_draw_ret_;
                if (pending_skip_) list_p10_ = pending_shift_;
                return false;
            });
        }
        m_.log("hires: scene capture %s%s\n", scene_found_ ? "on" : "off; missing:", missing.c_str());
    }
    char b[200];
    std::snprintf(b, sizeof b, "engine hooks set (%s): xyz %04X/%04X/%04X centre %04X/%04X edges %04X viewport %04X/%04X",
                  name.c_str(), x_tab_, y_tab_, z_tab_, cx_, cy_, edge_base_, vp_x_, vp_y_);
    status = b;
    m_.log("hires: %s\n", status.c_str());
}

HiresProj WorldCapture::proj_state() const {
    HiresProj p{};
    p.cx = float(int16_t(ds16(cx_)));
    p.cy = float(int16_t(ds16(cy_)));
    float zdiv = 1.0f;
    if (ds8(zoom_)) zdiv *= 2.0f;
    if (uint8_t n = ds8(zshift_)) zdiv /= float(1u << n);
    p.zdiv = zdiv;
    p.vp_w = float(ds16(vp_x_)) + 1;
    p.vp_h = float(ds16(vp_y_)) + 1;
    uint16_t org = gfx_.current_origin();
    p.ox = float(org % 320);
    p.oy = float(org / 320);
    return p;
}

std::array<float, 3> WorldCapture::vertex(int v) const {
    uint16_t ds = m_.cpu.regs.s[DS];
    auto r32 = [&](uint16_t base) {
        uint16_t off = uint16_t(base + v * 4);
        return float(int32_t(m_.mem.read16(ds, off) | (uint32_t(m_.mem.read16(ds, uint16_t(off + 2))) << 16)));
    };
    return {r32(x_tab_), r32(y_tab_), r32(z_tab_)};
}

void WorldCapture::edge_setup() {
    dbg[6]++;
    engine_cs_ = m_.cpu.regs.s[CS];
    const Regs& r = m_.cpu.regs;
    int idx = (int(r.r[DI]) - int(edge_base_)) / 26;
    if (idx < 0 || idx >= int(edges_.size())) return;
    edges_[idx] = {m_.mem.read8(Memory::linear(r.s[ES], r.r[SI])), m_.mem.read8(Memory::linear(r.s[ES], uint16_t(r.r[SI] + 1)))};
}

void WorldCapture::capture_poly() {
    world_flag_ = true;
    dbg[0]++;
    if (gfx_.current_draw_seg() != gfx_.page_seg(1)) { dbg[1]++; return; }
    Regs& r = m_.cpu.regs;
    uint8_t n = m_.mem.read8(Memory::linear(r.s[ES], uint16_t(r.r[SI] - 1)));
    // Chain the polygon's edges into a vertex loop.
    std::vector<std::array<uint8_t, 2>> es;
    for (int i = 0; i < n; i++) {
        uint8_t e = m_.mem.read8(Memory::linear(r.s[ES], uint16_t(r.r[SI] + i)));
        es.push_back(edges_[e]);
    }
    if (es.size() < 2) { dbg[2]++; return; }
    std::vector<int> loop = {es[0][0], es[0][1]};
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
    if (loop.size() < 3) { dbg[3]++; return; }
    HiresPrim p;
    p.kind = HiresPrim::Poly;
    p.object = object_;
    p.color = r.r8(4);
    p.proj = proj_state();
    for (int v : loop) p.v.push_back(vertex(v));
    prims_.push_back(std::move(p));
}

void WorldCapture::capture_line() {
    world_flag_ = true;
    dbg[4]++;
    if (gfx_.current_draw_seg() != gfx_.page_seg(1)) { dbg[5]++; return; }
    Regs& r = m_.cpu.regs;
    int idx = (int(r.r[BX]) - int(edge_base_)) / 26;
    if (idx < 0 || idx >= int(edges_.size())) return;
    HiresPrim p;
    p.kind = HiresPrim::Line;
    p.object = object_;
    p.color = r.r8(4);
    p.proj = proj_state();
    p.v = {vertex(edges_[idx][0]), vertex(edges_[idx][1])};
    prims_.push_back(std::move(p));
}

void WorldCapture::capture_dot() {
    world_flag_ = true;
    if (gfx_.current_draw_seg() != gfx_.page_seg(1)) return;
    HiresPrim p;
    p.kind = HiresPrim::Dot;
    p.object = object_;
    p.color = uint8_t(gfx_.current_color());
    p.proj = proj_state();
    p.v = {vertex(0)};
    prims_.push_back(std::move(p));
}

// Mirrors the engine's horizon routine (see docs/native-port.md).
void WorldCapture::capture_horizon() {
    engine_cs_ = m_.cpu.regs.s[CS];
    world_flag_ = true;
    if (gfx_.current_draw_seg() != gfx_.page_seg(1)) return;
    HiresPrim p;
    p.kind = HiresPrim::Horizon;
    p.object = object_;
    p.proj = proj_state();
    p.color = ds8(sky_);
    p.color2 = ds8(ground_);
    int16_t up_n = int16_t(ds16(up_n_)), up_d = int16_t(ds16(up_d_));
    if (std::getenv("F19_HORIZON_LOG"))
        std::fprintf(stderr, "horizon raw: up_n %d up_d %d up_s %d cos %d mode %u alt %u zoom %u shift %u cx %d cy %d\n", up_n, up_d,
                     int16_t(ds16(up_s_)), int16_t(ds16(cos_)), ds16(view_mode_), ds8(alt_), ds8(zoom_), ds8(zshift_),
                     int16_t(ds16(cx_)), int16_t(ds16(cy_)));
    float s = -float(int16_t(ds16(up_s_))) / 32768.0f;
    float c = float(int16_t(ds16(cos_))) / 32768.0f;
    bool mode2 = ds16(view_mode_) == 2;
    p.sky_only = mode2;
    // The engine's horizon is the vanishing line of the ground plane:
    // -s*x + c*y = d in unscaled screen units (x = 256X/Z, y = 256Y/Z) with
    // d = 256*up_n/up_d, so world up in camera space is proportional to
    // (-s*up_d, c*up_d, -up_n).
    {
        float ux = -s * float(up_d), uy = c * float(up_d), uz = -float(up_n);
        float n = std::sqrt(ux * ux + uy * uy + uz * uz);
        if (n > 0) { p.up[0] = ux / n; p.up[1] = uy / n; p.up[2] = uz / n; }
    }
    if (up_d <= 0x1F0B) {
        // Looking (nearly) straight up or down: one colour.
        p.uniform = true;
        if (up_n >= 0) {
            if (mode2) return;  // ground not drawn in this view mode
            p.color = p.color2;
        }
    } else {
        float d = 256.0f * float(up_n) / float(up_d);
        if (mode2) {
            int alt = ds8(alt_);
            d -= float(((alt + (alt >> 1)) >> 3) + 4);
        }
        if (uint8_t n = ds8(zshift_)) d *= float(1u << n);
        if (ds8(zoom_)) d *= 0.5f;
        p.hx = p.proj.cx - d * s;
        p.hy = p.proj.cy - 0.75f * d * c;
        // Sky side: the normal to the line direction (c, -0.75 s) that
        // points up the screen when level.
        p.ux = -0.75f * s;
        p.uy = -c;
    }
    prims_.push_back(std::move(p));
}

int32_t WorldCapture::level_coord(int level, int32_t v) const {
    // FUN_07c6: world -> level units (level 0 x2, 1 x1, 2..4 rounded /4^(L-1)).
    switch (level) {
        case 0: return int32_t(int64_t(v) * 2);
        case 1: return v;
        case 2: return int32_t((int64_t(v) + 2) >> 2);
        case 3: return int32_t((int64_t(v) + 8) >> 4);
        default: return int32_t((int64_t(v) + 32) >> 6);
    }
}

// FUN_0848: tile type at (x, y) on a level's grid (0 outside it).
int WorldCapture::tile_type(int level, int x, int y) const {
    if (level == 4) {
        x += 2;
        y += 2;
    }
    int size = int16_t(ds16(uint16_t(sizes_ + 2 * level)));
    if (x < 0 || y < 0 || x >= size || y >= size) return 0;
    auto sub = [&](uint16_t tab, int parent) { return ds8(uint16_t(tab + (x & 3) + (y & 3) * 4 + parent * 16)); };
    switch (level) {
        case 4: return ds8(uint16_t(map4_ + x + y * 8));
        case 3: return ds8(uint16_t(map3_ + x + y * 16));
        case 2: return sub(exp3_, tile_type(3, x >> 2, y >> 2));
        case 1: return sub(exp2_, tile_type(2, x >> 2, y >> 2));
        default: return sub(exp1_, tile_type(1, x >> 2, y >> 2));
    }
}

std::shared_ptr<const SceneShape> WorldCapture::shape_at(uint32_t linear) {
    ShapeCache::Shared sh;
    sh.ds = m_.cpu.regs.s[DS];
    sh.xi = shared_[0];
    sh.yi = shared_[1];
    sh.zi = shared_[2];
    sh.xv = shared_[3];
    sh.yv = shared_[4];
    sh.zv = shared_[5];
    return shapes_->get(m_.mem, linear, sh);
}

// Terrain loop entry: the main view's camera. Enumerate terrain objects in
// every direction (the engine itself only draws a 3x3 block of tiles ahead
// per level).
void WorldCapture::on_terrain() {
    pending_shift_ = -1000;
    HiresProj pj = proj_state();
    if (!(pj.ox == 0 && pj.oy == 0 && pj.vp_w >= 320)) return;
    const Regs& r = m_.cpu.regs;
    auto arg = [&](int i) { return m_.mem.read16(r.s[SS], uint16_t(r.r[SP] + 2 + 2 * i)); };
    for (int k = 0; k < 3; k++) cam_world_[k] = int32_t(arg(2 + 2 * k) | (uint32_t(arg(3 + 2 * k)) << 16));
    auto sc = std::make_shared<Scene>();
    for (int i = 0; i < 9; i++) sc->view[i] = float(int16_t(ds16(uint16_t(matrix_ + 2 * i)))) / 32768.0f;
    for (int i = 0; i < 16; i++) sc->remap[i] = ds8(uint16_t(color_table_ + i));
    for (int i = 0; i < 8; i++) sc->lod_table[i] = int16_t(ds16(uint16_t(lod_table_ + 2 * i)));
    sc->night = ds8(night_) != 0;
    sc->anim = int16_t(ds16(anim_));
    uint16_t shape_seg = uint16_t(m_.mem.read8(shape_seg_lin_) | (m_.mem.read8(shape_seg_lin_ + 1) << 8));
    auto shape_of = [&](uint8_t s) { return Memory::linear(shape_seg, ds16(uint16_t(shape_table_ + 2 * (s & 0x7F)))); };
    int start = int16_t(ds16(view_mode_)) != 0 ? 4 : 3;
    size_t count = 0;
    for (int L = start; L >= 1; L--) {
        if (ds16(uint16_t(enable_ + 2 * L)) == 0) continue;
        int32_t cx = level_coord(L, cam_world_[0]), cy = level_coord(L, cam_world_[1]), cz = level_coord(L, cam_world_[2]);
        if (cz < 2) cz = 2;
        int32_t tx = cx >> 12, ty = cy >> 12;
        int R = terrain_radius[L];
        float scale = float(1 << (2 * (L - 1)));
        for (int dy = -R; dy <= R; dy++)
            for (int dx = -R; dx <= R; dx++) {
                int x = tx + dx, y = ty + dy;
                // Level 4 (base map): beyond the 8x8 map repeat its border.
                int type = L == 4 ? tile_type(4, std::clamp(x, -2, 5), std::clamp(y, -2, 5)) : tile_type(L, x, y);
                uint16_t list = ds16(uint16_t(lists_ + L * 0x40 + type * 2));
                int n = ds16(uint16_t(counts_ + L * 0x40 + type * 2));
                int32_t ccx = cx - (x * 0x1000 + 0x800), ccy = cy - (y * 0x1000 + 0x800);
                for (int i = 0; i < n && i < 256; i++) {
                    uint16_t rec = uint16_t(list + 7 * i);
                    uint8_t s = ds8(uint16_t(rec + 6));
                    uint32_t lin = shape_of(s);
                    if (s & 0x80) {
                        auto it = overrides_.find({L, x, y, i});
                        if (it != overrides_.end()) lin = it->second;
                    }
                    SceneInstance in{};
                    in.shape = shape_at(lin);
                    in.pos[0] = int16_t(ds16(rec));
                    in.pos[1] = int16_t(ds16(uint16_t(rec + 2)));
                    in.pos[2] = int16_t(ds16(uint16_t(rec + 4)));
                    in.cam[0] = ccx;
                    in.cam[1] = ccy;
                    in.cam[2] = cz;
                    in.scale = scale;
                    in.level = int8_t(L);
                    in.order = float(dx * dx + dy * dy);
                    sc->instances.push_back(std::move(in));
                    count++;
                }
            }
    }
    scene_stats[1] += count;
    if (objects_found_) add_object_list(*sc, shape_seg);
    scene_ = std::move(sc);
    dynamic_.clear();
}

uint32_t WorldCapture::shape_code(uint16_t code, uint16_t seg) const {
    // FUN_cf84.
    uint16_t off = (code & 0x100) ? ds16(uint16_t(code_theatre_ + 2 * (code & 0x7F)))
                                  : uint16_t(ds16(uint16_t(code_stflt_ + 2 * code)) + code_add_);
    return Memory::linear(seg, off);
}

// The main object list (aircraft, ships, vehicles), placed as the object
// routine (FUN_ca58) places them but without the engine's forward cone
// test, so they also appear beside and behind the view.
void WorldCapture::add_object_list(Scene& sc, uint16_t shape_seg) {
    bool external = ds8(oc_view_) & 0x80;
    auto d32 = [&](uint16_t a) { return int32_t(ds16(a) | (uint32_t(ds16(uint16_t(a + 2))) << 16)); };
    int32_t cx = d32(external ? oc_x2_ : oc_x1_), cy = d32(external ? oc_y2_ : oc_y1_);
    int16_t calt = int16_t(ds16(external ? oc_a2_ : oc_a1_));
    float size = std::ldexp(1.0f, 3 - list_p10_);
    int n = int16_t(ds16(list_count_));
    size_t added = 0;
    for (int i = 0; i < n && i < 256; i++) {
        uint16_t rec = uint16_t(list_base_ + 0x24 * i);
        if (!(ds8(uint16_t(rec + 0x16)) & 2)) continue;
        int16_t alt = int16_t(ds16(uint16_t(rec + 4)));
        int32_t a = d32(uint16_t(rec + 6)) - cx;
        int32_t e = int32_t(uint32_t(d32(uint16_t(rec + 10))) + uint32_t(cy) - 0x1000000u);
        int32_t z = alt - calt;
        // The engine's limit is 0x7FFF instance units; allow four times that.
        float lim = 4.0f * 32767.0f * size;
        if (std::fabs(float(a)) > lim || std::fabs(float(e)) > lim) continue;
        uint16_t code = ds16(uint16_t(list_codes_ + int16_t(ds16(uint16_t(rec + 0x14))) * 0x20));
        SceneInstance in{};
        in.shape = shape_at(shape_code(code, shape_seg));
        in.pos[0] = a;
        in.pos[1] = -e;
        in.pos[2] = alt != 0;
        in.cam[0] = 0;
        in.cam[1] = 0;
        in.cam[2] = -z;
        in.angle[0] = uint16_t(-int16_t(ds16(uint16_t(rec + 0x0E))));
        in.angle[1] = ds16(uint16_t(rec + 0x10));
        in.angle[2] = ds16(uint16_t(rec + 0x12));
        in.scale = 1;
        in.size = size;
        in.level = 0;
        sc.instances.push_back(std::move(in));
        added++;
    }
    scene_stats[2] += added;
}

// Shape draw entry. Terrain calls: remember state-dependent shapes
// (destroyed objects). Dynamic objects (after the object-draw routine) in
// the main view become scene instances.
void WorldCapture::on_shape_draw() {
    const Regs& r = m_.cpu.regs;
    auto st = [&](int off) { return m_.mem.read16(r.s[SS], uint16_t(r.r[SP] + off)); };
    uint32_t ret = Memory::linear(st(2), st(0));
    uint16_t soff = st(4), sseg = st(6);
    int shift = pending_shift_;
    pending_shift_ = -1000;
    if (!soff) return;
    uint32_t lin = Memory::linear(sseg, soff);
    int16_t x = int16_t(st(14)), y = int16_t(st(16)), z = int16_t(st(18));
    if (ret >= terrain_lo_ && ret < terrain_lo_ + 0x400) {
        int L = int16_t(ds16(level_));
        if (L < 1 || L > 4) return;
        int32_t cx = level_coord(L, cam_world_[0]), cy = level_coord(L, cam_world_[1]);
        int tx = (cx - int16_t(ds16(cam_off_[0])) - 0x800) >> 12, ty = (cy - int16_t(ds16(cam_off_[1])) - 0x800) >> 12;
        int type = tile_type(L, tx, ty);
        uint16_t list = ds16(uint16_t(lists_ + L * 0x40 + type * 2));
        int n = ds16(uint16_t(counts_ + L * 0x40 + type * 2));
        for (int i = 0; i < n && i < 256; i++) {
            uint16_t rec = uint16_t(list + 7 * i);
            if ((ds8(uint16_t(rec + 6)) & 0x80) && int16_t(ds16(rec)) == x && int16_t(ds16(uint16_t(rec + 2))) == y &&
                int16_t(ds16(uint16_t(rec + 4))) == z) {
                auto& o = overrides_[{L, tx, ty, i}];
                if (o != lin) scene_stats[3]++;
                o = lin;
                break;
            }
        }
        return;
    }
    bool skip = pending_skip_;
    pending_skip_ = false;
    if (shift == -1000 || !scene_ || skip) return;
    HiresProj pj = proj_state();
    if (!(pj.ox == 0 && pj.oy == 0 && pj.vp_w >= 320)) return;
    SceneInstance in{};
    in.shape = shape_at(lin);
    in.pos[0] = x;
    in.pos[1] = y;
    in.pos[2] = z;
    for (int k = 0; k < 3; k++) in.cam[k] = int16_t(ds16(cam_off_[k]));
    in.angle[0] = st(8);
    in.angle[1] = st(10);
    in.angle[2] = st(12);
    // Positions are shifted by (arg - 3), or (arg - 2) when zoomed; we do
    // not zoom, so keep the unzoomed enlargement as a shape size.
    bool zoom = ds8(oc_zoom_) != 0;
    int k = shift - (zoom ? 2 : 3);
    in.scale = std::ldexp(1.0f, -k);
    in.size = zoom ? 2.0f : 1.0f;
    in.level = 0;
    dynamic_.push_back(std::move(in));
}

void WorldCapture::flip() {
    auto f = std::make_shared<HiresFrame>();
    if (scene_) {
        scene_->instances.insert(scene_->instances.end(), dynamic_.begin(), dynamic_.end());
        scene_stats[0]++;
        scene_stats[2] += dynamic_.size();
        f->scene = std::move(scene_);
    }
    scene_.reset();
    dynamic_.clear();
    uint16_t back = gfx_.page_seg(1);
    f->page.assign(m_.mem.data() + Memory::linear(back, 0), m_.mem.data() + Memory::linear(back, 0) + 64000);
    f->mask = mask_;
    std::memcpy(f->dac, m_.dac, sizeof f->dac);
    f->prims = std::move(prims_);
    f->time_us = m_.now_us();
    if (found_) latest_ = std::move(f);
    prims_.clear();
    // Watch the back page for the next frame.
    std::fill(mask_.begin(), mask_.end(), 0);
    m_.mem.watch_lo = Memory::linear(back, 0);
    m_.mem.watch_len = 64000;
    m_.mem.watch_mask = mask_.data();
}

}  // namespace f19
