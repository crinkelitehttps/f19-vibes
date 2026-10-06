// Native world scene: the 3D engine's shapes and object placements, rendered
// by us instead of captured as the engine's own (culled, 320x200-tuned)
// primitives.
//
// Shapes are parsed from the game's memory (.3D3 data as loaded; format in
// docs/formats.md). Each frame records the camera (world position and the
// engine's view matrix) and a list of shape instances: terrain objects
// enumerated natively from the engine's tile tables in every direction out
// to a radius per level, plus the dynamic objects (aircraft, vehicles,
// weapons) the engine asked to draw. The renderer picks each instance's
// level of detail for its own resolution and transforms the geometry with
// the engine's conventions (see the transform notes in scene.cpp).
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "hires/world_capture.h"

namespace f19 {

class Memory;

struct SceneBody {
    enum Kind : uint8_t { Mesh, Point, Lights } kind = Mesh;
    uint8_t first = 0;        // first body byte (0x40 = always depth-sorted)
    int anim_axis = -1;       // prefix byte 0x6x: 0 = z offset, 1..3 = angle
    bool flat = false;        // all vertices at z = 0 (ground decal)
    std::vector<std::array<int16_t, 4>> planes;   // nx, ny, nz, d
    std::vector<std::array<float, 3>> verts;      // x east, y north, z up
    struct Prim {
        bool poly;
        uint8_t plane;        // poly: plane index
        uint32_t mask;        // line: visible if mask & visible planes
        bool ground = false;  // line: both ends at z = 0
        bool outline = false; // line: along a polygon edge of the body
        uint8_t color;        // 0-15 (remapped), 255 = not drawn
        std::vector<uint8_t> v;   // poly: vertex loop; line: 2 vertices
    };
    std::vector<Prim> prims;
    // Plane-sorted bodies: prims come in one group per plane, drawn in an
    // order the engine picks per view by walking a BSP tree of the planes
    // (see draw_order). sort_root < 0: draw prims in their stored order.
    int sort_root = -1;
    std::vector<std::array<uint8_t, 2>> sort_tree;      // per plane: child 0, child 1 (0xFF = none)
    std::vector<std::array<uint16_t, 2>> sort_groups;   // per plane: first prim, count
    std::vector<uint16_t> draw_order(uint32_t visible) const;
    uint8_t point_color = 0;  // Point
};

// A shape entry point: either a LOD switch (farther than threshold[level]
// -> far) or a body.
struct SceneNode {
    int lod_level = -1;
    std::shared_ptr<const SceneNode> near_node, far_node;
    std::shared_ptr<const SceneBody> body;
};

struct SceneShape {
    uint8_t size_class = 0;
    std::shared_ptr<const SceneNode> node;
    // From the most detailed body: a ground decal (all vertices at z = 0),
    // or terrain relief (a hill or ridge: few faces, raised, wide; its lines
    // are ridges, faded with distance). `extent` in shape units.
    bool flat = false;
    bool relief = false;
    float extent = 0;
};

struct SceneInstance {
    std::shared_ptr<const SceneShape> shape;
    int32_t pos[3];           // object position (instance units)
    int32_t cam[3];           // camera position in the same frame
    uint16_t angle[3];        // engine angles (0x988, 0x98A, 0x98C order)
    float scale;              // world units per instance unit
    float size = 1;           // shape vertices per instance unit (the engine enlarges aircraft)
    int8_t level;             // terrain level 1-4, 0 = dynamic object
    float order;              // painter key within the level (larger = earlier)
};

struct Scene {
    float view[9];            // engine view matrix (0x9B8, Q15 -> float)
    std::vector<SceneInstance> instances;
    uint8_t remap[16] = {};   // colour index -> palette
    bool night = false;
    int16_t anim = 0;         // rotating-part angle (0x9FC)
    int16_t lod_table[8] = {};
};

// Shapes parsed from emulated memory, cached by linear address.
class ShapeCache {
public:
    struct Shared { uint16_t ds = 0, xi = 0, yi = 0, zi = 0, xv = 0, yv = 0, zv = 0; };
    std::shared_ptr<const SceneShape> get(const Memory& mem, uint32_t linear, const Shared& sh);
    void clear() { shapes_.clear(); nodes_.clear(); bodies_.clear(); }

private:
    std::map<uint32_t, std::shared_ptr<const SceneShape>> shapes_;
    std::map<uint32_t, std::shared_ptr<const SceneNode>> nodes_;
    std::map<uint32_t, std::shared_ptr<const SceneBody>> bodies_;
    std::shared_ptr<const SceneNode> node(const Memory& mem, uint32_t linear, const Shared& sh, int depth);
    std::shared_ptr<const SceneBody> body(const Memory& mem, uint32_t linear, const Shared& sh);
};

struct SceneBuildParams {
    float lod_scale = 1.0f;   // > 1: keep detailed bodies farther away
    float ground_lod = 8.0f;  // extra lod_scale for ground decals (runways, roads, fields)
    // Line widths (see HiresPrim::LineStyle); classic: all Screen.
    bool classic_lines = false;
    float road_width = 40.0f;   // world units (feet): free lines on ground decals
    float edge_width = 1.0f;    // shape units: outlines and object edges
    float relief_extent = 2048; // world units: wider raised terrain shapes are relief
    float lights_range = 0x2400;  // ground lights beyond this depth are dropped (engine: ~2 tiles)
};

// Camera-space primitives (x right, y up, z forward; 65536 = one world
// unit) in draw order: terrain on the ground in the engine's painter order
// (level 4 -> 1, far tiles first), then everything else far to near.
// `object` numbers instances; `flat` marks ground decals (no depth write).
void build_scene_prims(const Scene& s, const SceneBuildParams& p, std::vector<HiresPrim>& out);

}  // namespace f19
