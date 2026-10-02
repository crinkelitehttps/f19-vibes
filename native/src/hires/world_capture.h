// Captures the 3D engine's world geometry in camera space, for rendering at
// high resolution.
//
// When the flight program (EGAME.EXE / DGAME.EXE) loads, the engine's hook
// points are located by byte signature (see tools/engine_sigs.py) and the
// data-segment offsets they use are read from the matched code, so one build
// handles both the demo and the full game. Each drawn polygon, line and dot
// is recorded with its camera-space vertices and the projection state at the
// time (screen centre, zoom, viewport, driver draw origin); the horizon is
// recorded from the camera's up vector. The engine still rasterises at
// 320x200; memory write tagging marks which back-page pixels came from world
// drawing, so the 2D layer can be composited over the high-res render.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace f19 {

class Machine;
class MGraphicNative;

struct HiresProj {
    float cx, cy;        // projection centre (viewport-local, 320x200 units)
    float zdiv;          // camera Z is divided by this (engine zoom/shift)
    float vp_w, vp_h;    // viewport size (local)
    float ox, oy;        // driver draw origin (page coordinates)
};

struct HiresPrim {
    enum Kind : uint8_t { Poly, Line, Dot, Horizon } kind;
    uint8_t color = 0;       // palette index
    uint8_t color2 = 0;      // horizon: ground colour
    uint32_t object = 0;     // shape instance the primitive belongs to
    HiresProj proj;
    std::vector<std::array<float, 3>> v;  // camera space (x right, y up, z forward)
    // Horizon: screen-space line point M, sky direction U (local coords),
    // or `uniform` = whole viewport one colour (color).
    float hx = 0, hy = 0, ux = 0, uy = 0;
    bool uniform = false, sky_only = false;
};

struct HiresFrame {
    std::vector<HiresPrim> prims;
    std::vector<uint8_t> page;   // 320x200 back page at flip time
    std::vector<uint8_t> mask;   // 1 = pixel last written by world drawing
    uint8_t dac[256][3] = {};
    uint64_t time_us = 0;
};

class WorldCapture {
public:
    WorldCapture(Machine& m, MGraphicNative& gfx);
    bool enabled = true;
    bool active() const { return found_; }
    std::shared_ptr<const HiresFrame> latest() const { return latest_; }
    std::string status;  // what was found, for logging
    uint64_t dbg[8] = {};
    uint32_t poly_lin_ = 0;

private:
    Machine& m_;
    MGraphicNative& gfx_;
    bool found_ = false;
    struct Pending { std::string name; uint16_t seg = 0; uint32_t size = 0; } pending_;
    bool has_pending_ = false;
    uint16_t engine_cs_ = 0;
    // Data-segment offsets read from the engine code.
    uint16_t x_tab_ = 0, y_tab_ = 0, z_tab_ = 0, zoom_ = 0, zshift_ = 0, cx_ = 0, cy_ = 0;
    uint16_t edge_base_ = 0, vp_x_ = 0, vp_y_ = 0;
    uint16_t up_s_ = 0, up_n_ = 0, up_d_ = 0, cos_ = 0, view_mode_ = 0, alt_ = 0, sky_ = 0, ground_ = 0;
    uint32_t c_line_lin_ = 0;   // far entries for plain 2D lines (+0, +4)

    std::vector<std::array<uint8_t, 2>> edges_;   // edge record index -> vertex indices
    std::vector<HiresPrim> prims_;
    std::vector<uint8_t> mask_;
    std::shared_ptr<HiresFrame> latest_;
    bool world_flag_ = false;
    uint32_t object_ = 0;

    void on_program(const std::string& name, uint16_t seg, uint32_t size);
    HiresProj proj_state() const;
    std::array<float, 3> vertex(int v) const;
    void capture_poly();
    void capture_line();
    void capture_dot();
    void capture_horizon();
    void edge_setup();
    void flip();
    uint16_t ds16(uint16_t off) const;
    uint8_t ds8(uint16_t off) const;
};

}  // namespace f19
