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
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace f19 {

class Machine;
class MGraphicNative;
class ShapeCache;
struct Scene;
struct SceneInstance;
struct SceneShape;

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
    int8_t flat = -1;        // 1: ground decal (no depth write), 0: solid, -1: decide from geometry
    // Line: how its width is chosen (native scene; captured lines are
    // Screen). Screen: fixed width in 320x200 pixels, as the engine draws.
    // Ground: a strip `width` wide (camera-space units) lying in the plane
    // with normal `normal` (roads, markings). Edge: `width` wide facing the
    // camera (object outlines). Relief: screen width, thinning and fading
    // with distance (mountain ridges). Ground and Edge keep a minimum
    // on-screen width and fade out below it.
    enum LineStyle : uint8_t { Screen, Ground, Edge, Relief } line_style = Screen;
    float width = 0;
    std::array<float, 3> normal = {0, 0, 0};
    HiresProj proj;
    std::vector<std::array<float, 3>> v;  // camera space (x right, y up, z forward)
    // Horizon: screen-space line point M, sky direction U (local coords),
    // or `uniform` = whole viewport one colour (color).
    float hx = 0, hy = 0, ux = 0, uy = 0;
    bool uniform = false, sky_only = false;
    // Horizon: world "up" in camera space (x right, y up, z forward), for
    // rendering sky/ground from any viewing direction.
    float up[3] = {0, 1, 0};
};

struct HiresFrame {
    std::vector<HiresPrim> prims;
    std::vector<uint8_t> page;   // 320x200 back page at flip time
    std::vector<uint8_t> mask;   // 1 = pixel last written by world drawing
    uint8_t dac[256][3] = {};
    uint64_t time_us = 0;
    // Native scene for the main view (null if the engine was not found or
    // no main view was drawn this frame).
    std::shared_ptr<const Scene> scene;
};

class WorldCapture {
public:
    WorldCapture(Machine& m, MGraphicNative& gfx);
    ~WorldCapture();
    bool enabled = true;
    // Native scene: terrain tiles enumerated around the camera, per level
    // (1-4) out to this many tiles in every direction.
    int terrain_radius[5] = {0, 6, 6, 6, 6};
    uint64_t scene_stats[4] = {};   // frames, terrain instances, dynamic instances, overrides
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

    // Scene capture (see scene.h).
    std::unique_ptr<ShapeCache> shapes_;
    bool scene_found_ = false;
    uint16_t angle_[3] = {}, cam_off_[3] = {}, matrix_ = 0, lod_table_ = 0, lod_shift_ = 0, anim_ = 0, night_ = 0;
    uint16_t color_table_ = 0, level_ = 0, lists_ = 0, counts_ = 0, enable_ = 0, sizes_ = 0;
    uint16_t map4_ = 0, map3_ = 0, exp3_ = 0, exp2_ = 0, exp1_ = 0, shape_table_ = 0;
    uint16_t shared_[6] = {};   // xi, yi, zi, xv, yv, zv
    uint32_t shape_seg_lin_ = 0, terrain_lo_ = 0;
    int32_t cam_world_[3] = {};
    std::shared_ptr<Scene> scene_;
    std::vector<SceneInstance> dynamic_;
    int pending_shift_ = -1000;   // object_draw scale argument, until its shape draw
    bool pending_skip_ = false;   // that draw comes from the object list (drawn natively)
    // Object list (aircraft, ships, vehicles) and the object routine's camera.
    uint16_t oc_x1_ = 0, oc_y1_ = 0, oc_a1_ = 0, oc_view_ = 0, oc_x2_ = 0, oc_y2_ = 0, oc_a2_ = 0, oc_zoom_ = 0;
    uint16_t code_theatre_ = 0, code_stflt_ = 0, code_add_ = 0;
    uint16_t list_count_ = 0, list_base_ = 0, list_codes_ = 0, list_type_ = 0;
    uint32_t list_draw_ret_ = 0;
    int list_p10_ = 2;
    bool objects_found_ = false;
    uint32_t shape_code(uint16_t code, uint16_t seg) const;
    void add_object_list(Scene& sc, uint16_t shape_seg);
    std::map<std::tuple<int, int, int, int>, uint32_t> overrides_;   // (level, tx, ty, index) -> shape

    void on_terrain();
    void on_shape_draw();
    int tile_type(int level, int x, int y) const;
    int32_t level_coord(int level, int32_t v) const;
    std::shared_ptr<const SceneShape> shape_at(uint32_t linear);
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
