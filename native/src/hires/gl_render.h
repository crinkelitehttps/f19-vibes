// OpenGL 3.3 renderer for the native host.
//
// render_frame() draws a captured HiresFrame: the engine's world primitives
// (projected exactly as the engine does, near-plane clipped) into a
// multisampled framebuffer in the engine's order, resolves it, then
// composites the 320x200 page with world pixels transparent. Other screens
// (menus, text mode) are uploaded as plain textures. present() letterboxes a
// texture into the window.
#pragma once

#include <cstdint>
#include <vector>

#include "hires/gl.h"
#include "hires/world_capture.h"

namespace f19 {

// Head pose in the aircraft frame: rotation (radians; yaw right +, pitch up +,
// roll right ear down +) and position (cm from the neutral eye point; x right,
// y up, z back).
struct HeadPose {
    float yaw = 0, pitch = 0, roll = 0;
    float x = 0, y = 0, z = 0;
    bool operator==(const HeadPose&) const = default;
};

// One eye's camera for render_view: orientation, position and an
// arbitrary (asymmetric) frustum, as a VR runtime supplies per eye.
struct EyeView {
    // View -> aircraft rotation: columns are the view's right, up and
    // forward axes in the aircraft frame (x right, y up, z forward).
    float rot[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    float pos[3] = {};  // eye position, cm from the neutral eye point (aircraft frame)
    // Frustum edges as tangents of the angles from the view axis (left and
    // down negative).
    float tan_left = -1, tan_right = 1, tan_down = -1, tan_up = 1;
};

class GlRenderer {
public:
    ~GlRenderer();
    bool init();                 // needs a current GL 3.3 core context
    int msaa = 8;                // samples for the 3D world (1 = off)
    float line_width = 0.75f;    // in 320x200 pixels
    bool draw_overlay = true;    // false: world only (debugging)
    bool depth = true;           // per-pixel occlusion between solid objects
    // 3D view: draw the world from the native scene (all directions, our
    // own level of detail) instead of the engine's captured primitives.
    bool native_world = true;
    float lod_detail = 1.0f;     // > 1 keeps detailed models farther away
    // Native scene lines (HiresPrim::LineStyle). classic_lines: all at
    // line_width, as the original. Otherwise roads and markings are strips
    // on the ground and object edges have a width in the world; both stay at
    // least line_min_px wide (window pixels at 1080 rows) and fade out in
    // proportion below it, to no less than line_fade_floor. Mountain ridges
    // keep line_width up to relief_near (world units), then thin to the
    // minimum and fade to relief_alpha by relief_far.
    bool classic_lines = false;
    float line_min_px = 1.25f, line_fade_floor = 0.25f;
    float relief_near = 25000.0f, relief_far = 200000.0f, relief_alpha = 0.5f;
    float road_width = 40.0f;    // world units (feet)

    // Composited frame at w x h; returns a texture valid until the next call.
    GLuint render_frame(const HiresFrame& f, int w, int h);
    // 3D view at the window's aspect ratio. Cockpit view: the instrument
    // panel and the HUD become surfaces in the cockpit.
    // External views: the game's 2D elements are composited centred 4:3.
    // The world is rendered through
    // our own perspective camera rotated by the head, sky and ground per
    // pixel, and the instrument panel (the page below the 3D viewport) as a
    // textured quad in the cockpit. Head position only moves the eye
    // relative to the panel (the world is far away).
    // Returns a w x h texture.
    bool render_cockpit3d(const HiresFrame& f, int w, int h, const HeadPose& head, GLuint* out);
    // The same scene through an arbitrary eye camera, resolved into the
    // framebuffer `dst` (w x h, GL orientation; 0 = only the internal output
    // texture, which is returned). For stereo, call once per eye.
    GLuint render_view(const HiresFrame& f, int w, int h, const EyeView& eye, GLuint dst);
    // Panel placement in the aircraft frame (eye at origin, x right, y up,
    // z forward; units arbitrary, only proportions matter).
    // Top edge just below the original 3D viewport (~15 degrees down), as
    // on the original screen, so it does not hide the HUD's bottom rows.
    float panel_width = 1.0f, panel_center[3] = {0.0f, -0.43f, 0.90f};
    float panel_tilt_deg = 25.0f;   // top edge leans away from the viewer
    // HUD: the non-world pixels of page columns hud_cols in the rows of the
    // 3D viewport, collimated (at infinity) at the engine's projection scale
    // around its projection centre (hud_cross if the frame has none).
    float hud_cols[2] = {0, 319}, hud_cross[2] = {159, 56};
    float hfov_4x3_deg = 64.0f;     // the original's horizontal field of view
    float panel_units_per_cm = 1.0f / 60.0f;  // head position scale (panel width ~60 cm)
    // Eye position also moves the eye in the world (parallax on nearby
    // objects in stereo). World units are assumed to be feet.
    float world_units_per_cm = 1.0f / 30.48f;

    // The manual on a clipboard: in the 3D views a quad in the cockpit,
    // facing the neutral eye in front of the panel; elsewhere drawn flat
    // over the screen (draw_manual_2d).
    bool show_manual = false;
    float manual_width = 0.70f, manual_center[3] = {0.0f, -0.11f, 0.69f};
    float manual_tilt_deg = 9.0f;  // top edge leans away from the viewer
    // The clipboard image (0xAARRGGBB words, top row first; mipmapped).
    void set_manual(const uint32_t* argb, int w, int h);
    bool has_manual() const { return manual_w_ > 0; }
    // Draw the clipboard over framebuffer `fbo` (fb_w x fb_h), centred.
    void draw_manual_2d(GLuint fbo, int fb_w, int fb_h);

    // A w x h RGBA8 image (0xAARRGGBB words) as a texture (`nearest` filtering).
    GLuint upload(const uint32_t* argb, int w, int h);
    // Draw `tex` into the window's default framebuffer at `rect` (pixels,
    // top-left origin); clears the rest to black.
    void present(GLuint tex, int win_w, int win_h, float x, float y, float w, float h) {
        present_to(0, tex, win_w, win_h, x, y, w, h);
    }
    // The same into framebuffer `fbo` (fb_w x fb_h).
    void present_to(GLuint fbo, GLuint tex, int fb_w, int fb_h, float x, float y, float w, float h);
    // Read the window's default framebuffer (RGB, top row first).
    std::vector<uint8_t> read_window(int w, int h);
    // Read back a texture produced by render_frame (RGB, top row first).
    std::vector<uint8_t> read_rgb(GLuint tex, int w, int h);

private:
    struct Vert { float x, y, z, r, g, b, a; };
    struct Run { int first, count; int sx, sy, sw, sh; };
    // Consecutive primitives of one object within one viewport.
    struct Group { int first, count; int run; uint32_t object; bool flat, background; };
    std::vector<Group> groups_;
    std::vector<HiresPrim> scene_prims_;
    const HiresFrame* scene_frame_ = nullptr;  // scene_prims_ built for (frame, time, lod)
    uint64_t scene_time_ = 0;
    float scene_lod_ = 0;
    bool scene_classic_ = false;

    GLuint world_prog_ = 0, tex_prog_ = 0;
    GLint world_size_loc_ = -1, tex_rect_loc_ = -1, tex_sampler_loc_ = -1;
    GLuint vao_ = 0, vbo_ = 0, quad_vao_ = 0, quad_vbo_ = 0;
    GLuint ms_fbo_ = 0, ms_color_ = 0, ms_depth_ = 0;
    GLuint out_fbo_ = 0, out_tex_ = 0;
    int fb_w_ = 0, fb_h_ = 0, fb_samples_ = 0;
    GLuint overlay_tex_ = 0, upload_tex_ = 0;
    int upload_w_ = 0, upload_h_ = 0;
    std::vector<Vert> verts_;
    std::vector<Run> runs_;

    void ensure_framebuffers(int w, int h);
    void build_geometry(const HiresFrame& f, int w, int h);
    void draw_textured(GLuint tex, float x0, float y0, float x1, float y1);  // NDC
    GLuint sky_prog_ = 0, panel_prog_ = 0, panel_vao_ = 0, panel_vbo_ = 0, panel_tex_ = 0;
    GLint sky_loc_[6] = {}, panel_mvp_loc_ = -1, panel_tex_loc_ = -1;
    GLuint manual_tex_ = 0;
    int manual_w_ = 0, manual_h_ = 0;
    void draw_groups();
};

}  // namespace f19
