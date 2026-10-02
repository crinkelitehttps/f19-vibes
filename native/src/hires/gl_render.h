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

class GlRenderer {
public:
    ~GlRenderer();
    bool init();                 // needs a current GL 3.3 core context
    int msaa = 8;                // samples for the 3D world (1 = off)
    float line_width = 0.75f;    // in 320x200 pixels
    bool draw_overlay = true;    // false: world only (debugging)
    bool depth = true;           // per-pixel occlusion between solid objects

    // Composited frame at w x h; returns a texture valid until the next call.
    GLuint render_frame(const HiresFrame& f, int w, int h);
    // A w x h RGBA8 image (0xAARRGGBB words) as a texture (`nearest` filtering).
    GLuint upload(const uint32_t* argb, int w, int h);
    // Draw `tex` into the window's default framebuffer at `rect` (pixels,
    // top-left origin); clears the rest to black.
    void present(GLuint tex, int win_w, int win_h, float x, float y, float w, float h);
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
};

}  // namespace f19
