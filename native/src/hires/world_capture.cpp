#include "hires/world_capture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>

#include "core/machine.h"
#include "drivers/mgraphic.h"

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
const char* kCLine = "e8 0d 00 cb 55 56 57 06 e8 05 00 07 5f 5e 5d cb";
// Rectangle fill called from C (2D, e.g. the TrackCam background).
const char* kRectFill = "55 8b ec 57 56 55 1e 07 9a ?? ?? ?? ?? 50 8b 5e 06 8b 07 9a";

}  // namespace

WorldCapture::WorldCapture(Machine& m, MGraphicNative& gfx) : m_(m), gfx_(gfx) {
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
        if (ret_cs != engine_cs_) {
            world_flag_ = false;
            return false;
        }
        return world_flag_;
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
    if (!proj || !projy || !eloop || !esetup || !poly || !line || !hor || !hcos || !hmode || !hsky || !hgnd || !vp ||
        !cline || dots.empty()) {
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
        p.ux = -s;
        p.uy = -0.75f * c;
    }
    prims_.push_back(std::move(p));
}

void WorldCapture::flip() {
    auto f = std::make_shared<HiresFrame>();
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
