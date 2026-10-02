#include "hires/gl_render.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace f19 {

using namespace gl;

namespace {

constexpr float kNearZ = 65536.0f;  // engine requires z_hi >= 1

const char* kWorldVS = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_col;
uniform vec2 u_size;
out vec4 v_col;
void main() {
    gl_Position = vec4(a_pos.x / u_size.x * 2.0 - 1.0, 1.0 - a_pos.y / u_size.y * 2.0, a_pos.z, 1.0);
    v_col = a_col;
})";
const char* kWorldFS = R"(#version 330 core
in vec4 v_col;
out vec4 o_col;
void main() { o_col = v_col; })";
const char* kTexVS = R"(#version 330 core
layout(location = 0) in vec2 a_uv;
uniform vec4 u_rect;  // NDC x0, y0, x1, y1
out vec2 v_uv;
void main() {
    gl_Position = vec4(mix(u_rect.x, u_rect.z, a_uv.x), mix(u_rect.y, u_rect.w, a_uv.y), 0.0, 1.0);
    v_uv = vec2(a_uv.x, 1.0 - a_uv.y);
})";
const char* kTexFS = R"(#version 330 core
in vec2 v_uv;
uniform sampler2D u_tex;
out vec4 o_col;
void main() { o_col = texture(u_tex, v_uv); })";

GLuint compile(GLenum type, const char* src) {
    GLuint s = CreateShader(type);
    ShaderSource(s, 1, &src, nullptr);
    CompileShader(s);
    GLint ok = 0;
    GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        GetShaderInfoLog(s, sizeof log, nullptr, log);
        SDL_Log("shader: %s", log);
    }
    return s;
}

GLuint link(const char* vs, const char* fs) {
    GLuint p = CreateProgram();
    GLuint a = compile(GL_VERTEX_SHADER, vs), b = compile(GL_FRAGMENT_SHADER, fs);
    AttachShader(p, a);
    AttachShader(p, b);
    LinkProgram(p);
    GLint ok = 0;
    GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        GetProgramInfoLog(p, sizeof log, nullptr, log);
        SDL_Log("program: %s", log);
    }
    DeleteShader(a);
    DeleteShader(b);
    return p;
}

struct P2 { float x, y, d = 0.0f; };

// Sutherland-Hodgman against z >= kNearZ.
std::vector<std::array<float, 3>> clip_near(const std::vector<std::array<float, 3>>& in) {
    std::vector<std::array<float, 3>> out;
    for (size_t i = 0; i < in.size(); i++) {
        const auto& a = in[i];
        const auto& b = in[(i + 1) % in.size()];
        bool ain = a[2] >= kNearZ, bin = b[2] >= kNearZ;
        if (ain) out.push_back(a);
        if (ain != bin) {
            float t = (kNearZ - a[2]) / (b[2] - a[2]);
            out.push_back({a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), kNearZ});
        }
    }
    return out;
}

std::vector<P2> clip_half(const std::vector<P2>& in, float mx, float my, float ux, float uy, float sign) {
    std::vector<P2> out;
    auto side = [&](P2 p) { return ((p.x - mx) * ux + (p.y - my) * uy) * sign; };
    for (size_t i = 0; i < in.size(); i++) {
        P2 a = in[i], b = in[(i + 1) % in.size()];
        float sa = side(a), sb = side(b);
        if (sa >= 0) out.push_back(a);
        if ((sa >= 0) != (sb >= 0)) {
            float t = sa / (sa - sb);
            out.push_back({a.x + t * (b.x - a.x), a.y + t * (b.y - a.y)});
        }
    }
    return out;
}

}  // namespace

GlRenderer::~GlRenderer() {
    // Context may already be gone at exit; GL objects are released with it.
}

bool GlRenderer::init() {
    world_prog_ = link(kWorldVS, kWorldFS);
    world_size_loc_ = GetUniformLocation(world_prog_, "u_size");
    tex_prog_ = link(kTexVS, kTexFS);
    tex_rect_loc_ = GetUniformLocation(tex_prog_, "u_rect");
    tex_sampler_loc_ = GetUniformLocation(tex_prog_, "u_tex");

    GenVertexArrays(1, &vao_);
    GenBuffers(1, &vbo_);
    BindVertexArray(vao_);
    BindBuffer(GL_ARRAY_BUFFER, vbo_);
    VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vert), reinterpret_cast<void*>(0));
    EnableVertexAttribArray(0);
    VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vert), reinterpret_cast<void*>(3 * sizeof(float)));
    EnableVertexAttribArray(1);

    static const float quad[] = {0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1};
    GenVertexArrays(1, &quad_vao_);
    GenBuffers(1, &quad_vbo_);
    BindVertexArray(quad_vao_);
    BindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
    BufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), reinterpret_cast<void*>(0));
    EnableVertexAttribArray(0);
    BindVertexArray(0);

    auto make_tex = [](GLuint& t) {
        GenTextures(1, &t);
        BindTexture(GL_TEXTURE_2D, t);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    make_tex(overlay_tex_);
    TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 320, 200, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    make_tex(upload_tex_);
    return GetError() == GL_NO_ERROR;
}

void GlRenderer::ensure_framebuffers(int w, int h) {
    int samples = std::max(1, msaa);
    if (fb_w_ == w && fb_h_ == h && fb_samples_ == samples) return;
    if (ms_fbo_) {
        DeleteFramebuffers(1, &ms_fbo_);
        DeleteRenderbuffers(1, &ms_color_);
        DeleteRenderbuffers(1, &ms_depth_);
        DeleteFramebuffers(1, &out_fbo_);
        DeleteTextures(1, &out_tex_);
    }
    GenFramebuffers(1, &ms_fbo_);
    BindFramebuffer(GL_FRAMEBUFFER, ms_fbo_);
    GenRenderbuffers(1, &ms_color_);
    BindRenderbuffer(GL_RENDERBUFFER, ms_color_);
    RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
    FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, ms_color_);
    GenRenderbuffers(1, &ms_depth_);
    BindRenderbuffer(GL_RENDERBUFFER, ms_depth_);
    RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT32F, w, h);
    FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, ms_depth_);
    if (CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) SDL_Log("multisample framebuffer incomplete");

    GenFramebuffers(1, &out_fbo_);
    BindFramebuffer(GL_FRAMEBUFFER, out_fbo_);
    GenTextures(1, &out_tex_);
    BindTexture(GL_TEXTURE_2D, out_tex_);
    TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out_tex_, 0);
    if (CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) SDL_Log("output framebuffer incomplete");
    BindFramebuffer(GL_FRAMEBUFFER, 0);
    fb_w_ = w;
    fb_h_ = h;
    fb_samples_ = samples;
}

// An object is flat if all its vertices lie on one plane (ground tiles,
// decals such as runway markings and roads): those keep the engine's
// painter's order instead of depth testing among themselves.
static bool object_is_flat(const std::vector<const HiresPrim*>& prims) {
    std::vector<std::array<double, 3>> pts;
    for (auto* p : prims)
        if (p->kind == HiresPrim::Poly || p->kind == HiresPrim::Line)
            for (auto& v : p->v) pts.push_back({v[0], v[1], v[2]});
    if (pts.size() < 4) return true;
    double scale = 0;
    for (auto& q : pts) scale = std::max({scale, std::fabs(q[0]), std::fabs(q[1]), std::fabs(q[2])});
    // Plane through the first three non-collinear points.
    for (size_t i = 1; i < pts.size(); i++)
        for (size_t j = i + 1; j < pts.size(); j++) {
            auto& a = pts[0];
            double u[3] = {pts[i][0] - a[0], pts[i][1] - a[1], pts[i][2] - a[2]};
            double v[3] = {pts[j][0] - a[0], pts[j][1] - a[1], pts[j][2] - a[2]};
            double n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
            double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len < 1e-6 * scale * scale) continue;
            for (auto& q : pts) {
                double d = ((q[0] - a[0]) * n[0] + (q[1] - a[1]) * n[1] + (q[2] - a[2]) * n[2]) / len;
                if (std::fabs(d) > 1e-4 * scale) return false;
            }
            return true;
        }
    return true;
}

void GlRenderer::build_geometry(const HiresFrame& f, int w, int h) {
    verts_.clear();
    runs_.clear();
    groups_.clear();
    std::map<uint32_t, std::vector<const HiresPrim*>> by_object;
    for (const HiresPrim& p : f.prims) by_object[p.object].push_back(&p);
    std::map<uint32_t, bool> flat;
    for (auto& [obj, list] : by_object) flat[obj] = object_is_flat(list);
    const float sx = float(w) / 320.0f, sy = float(h) / 200.0f;
    auto color = [&](uint8_t c) { return std::array<float, 4>{f.dac[c][0] / 63.0f, f.dac[c][1] / 63.0f, f.dac[c][2] / 63.0f, 1.0f}; };
    // Depth = 65536 / Z: affine in screen space for planar polygons, so
    // interpolating it per vertex is exact. 1 at the near plane, -> 0 far.
    auto tri_fan = [&](const std::vector<P2>& pts, std::array<float, 4> c) {
        for (size_t i = 1; i + 1 < pts.size(); i++)
            for (const P2& p : {pts[0], pts[i], pts[i + 1]}) verts_.push_back({p.x, p.y, p.d, c[0], c[1], c[2], c[3]});
    };
    auto project = [&](const HiresProj& p, const std::array<float, 3>& v) {
        float z = v[2] * p.zdiv;
        return P2{(p.ox + p.cx + 256.0f * v[0] / z) * sx, (p.oy + p.cy - 192.0f * v[1] / z) * sy, kNearZ / v[2]};
    };
    auto quad_line = [&](P2 a, P2 b, float width, std::array<float, 4> c) {
        float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy), hw = width * 0.5f;
        float tx, ty, nx, ny;
        if (len < 1e-3f) { tx = hw; ty = 0; nx = 0; ny = hw; }
        else { tx = dx / len * hw; ty = dy / len * hw; nx = -ty; ny = tx; }
        tri_fan({{a.x - tx + nx, a.y - ty + ny, a.d}, {b.x + tx + nx, b.y + ty + ny, b.d}, {b.x + tx - nx, b.y + ty - ny, b.d},
                 {a.x - tx - nx, a.y - ty - ny, a.d}}, c);
    };
    const float lw = std::max(1.0f, line_width * sx);

    for (const HiresPrim& p : f.prims) {
        // Scissor to the primitive's viewport (GL origin is bottom-left).
        int x0 = int(std::floor(p.proj.ox * sx)), y0 = int(std::floor(p.proj.oy * sy));
        int x1 = int(std::ceil((p.proj.ox + p.proj.vp_w) * sx)), y1 = int(std::ceil((p.proj.oy + p.proj.vp_h) * sy));
        Run r{int(verts_.size()), 0, x0, h - y1, x1 - x0, y1 - y0};
        if (runs_.empty() || runs_.back().sx != r.sx || runs_.back().sy != r.sy || runs_.back().sw != r.sw ||
            runs_.back().sh != r.sh)
            runs_.push_back(r);
        bool background = p.kind == HiresPrim::Horizon;
        bool is_flat = p.kind == HiresPrim::Dot || flat[p.object];
        if (groups_.empty() || groups_.back().object != p.object || groups_.back().run != int(runs_.size()) - 1 ||
            groups_.back().background != background || groups_.back().flat != is_flat)
            groups_.push_back(Group{int(verts_.size()), 0, int(runs_.size()) - 1, p.object, is_flat, background});
        switch (p.kind) {
            case HiresPrim::Poly: {
                auto clipped = clip_near(p.v);
                if (clipped.size() < 3) break;
                std::vector<P2> pts;
                for (auto& v : clipped) pts.push_back(project(p.proj, v));
                tri_fan(pts, color(p.color));
                break;
            }
            case HiresPrim::Line: {
                auto a = p.v[0], b = p.v[1];
                bool ain = a[2] >= kNearZ, bin = b[2] >= kNearZ;
                if (!ain && !bin) break;
                if (ain != bin) {
                    float t = (kNearZ - a[2]) / (b[2] - a[2]);
                    std::array<float, 3> m = {a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), kNearZ};
                    (ain ? b : a) = m;
                }
                quad_line(project(p.proj, a), project(p.proj, b), lw, color(p.color));
                break;
            }
            case HiresPrim::Dot: {
                if (p.v[0][2] < kNearZ) break;
                P2 q = project(p.proj, p.v[0]);
                float s = std::max(1.0f, sx) * 0.5f;
                tri_fan({{q.x - s, q.y - s, q.d}, {q.x + s, q.y - s, q.d}, {q.x + s, q.y + s, q.d}, {q.x - s, q.y + s, q.d}},
                        color(p.color));
                break;
            }
            case HiresPrim::Horizon: {
                std::vector<P2> vp = {{0, 0}, {p.proj.vp_w, 0}, {p.proj.vp_w, p.proj.vp_h}, {0, p.proj.vp_h}};
                auto to_target = [&](std::vector<P2> pts) {
                    for (auto& q : pts) q = {(p.proj.ox + q.x) * sx, (p.proj.oy + q.y) * sy};
                    return pts;
                };
                if (p.uniform) {
                    tri_fan(to_target(vp), color(p.color));
                    break;
                }
                tri_fan(to_target(clip_half(vp, p.hx, p.hy, p.ux, p.uy, 1.0f)), color(p.color));
                if (!p.sky_only) tri_fan(to_target(clip_half(vp, p.hx, p.hy, p.ux, p.uy, -1.0f)), color(p.color2));
                break;
            }
        }
        runs_.back().count = int(verts_.size()) - runs_.back().first;
        groups_.back().count = int(verts_.size()) - groups_.back().first;
    }
}

GLuint GlRenderer::render_frame(const HiresFrame& f, int w, int h) {
    ensure_framebuffers(w, h);
    build_geometry(f, w, h);

    // World, multisampled, in the engine's order.
    BindFramebuffer(GL_FRAMEBUFFER, ms_fbo_);
    Viewport(0, 0, w, h);
    Disable(GL_SCISSOR_TEST);
    ClearColor(0, 0, 0, 1);
    ClearDepth(0.0);  // depth is "larger = nearer"
    DepthMask(GL_TRUE);
    Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    Disable(GL_DEPTH_TEST);
    Disable(GL_BLEND);
    UseProgram(world_prog_);
    Uniform2f(world_size_loc_, float(w), float(h));
    BindVertexArray(vao_);
    BindBuffer(GL_ARRAY_BUFFER, vbo_);
    BufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts_.size() * sizeof(Vert)), verts_.data(), GL_STREAM_DRAW);
    Enable(GL_SCISSOR_TEST);
    if (!depth) {
        for (const Run& r : runs_) {
            if (!r.count) continue;
            Scissor(r.sx, r.sy, r.sw, r.sh);
            DrawArrays(GL_TRIANGLES, r.first, r.count);
        }
    } else {
        // Depth holds solid objects only (larger = nearer). Background
        // (horizon) ignores it; flat objects test against it in painter's
        // order without writing; solid objects draw colour (test, no write,
        // so their own faces keep the engine's order) then write depth.
        DepthFunc(GL_GEQUAL);
        for (const Group& g : groups_) {
            if (!g.count) continue;
            const Run& r = runs_[g.run];
            Scissor(r.sx, r.sy, r.sw, r.sh);
            if (g.background) {
                Disable(GL_DEPTH_TEST);
                DrawArrays(GL_TRIANGLES, g.first, g.count);
                continue;
            }
            Enable(GL_DEPTH_TEST);
            DepthMask(GL_FALSE);
            DrawArrays(GL_TRIANGLES, g.first, g.count);
            if (!g.flat) {
                ColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                DepthMask(GL_TRUE);
                DrawArrays(GL_TRIANGLES, g.first, g.count);
                ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            }
        }
        DepthMask(GL_TRUE);
        Disable(GL_DEPTH_TEST);
    }
    Disable(GL_SCISSOR_TEST);

    // Resolve, then the 2D layer on top.
    BindFramebuffer(GL_READ_FRAMEBUFFER, ms_fbo_);
    BindFramebuffer(GL_DRAW_FRAMEBUFFER, out_fbo_);
    BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    BindFramebuffer(GL_FRAMEBUFFER, out_fbo_);
    if (draw_overlay) {
        static std::vector<uint32_t> px(64000);
        for (int i = 0; i < 64000; i++) {
            const uint8_t* d = f.dac[f.page[i]];
            uint32_t a = f.mask[i] ? 0u : 255u;
            px[i] = a << 24 | uint32_t(d[0] * 255 / 63) << 16 | uint32_t(d[1] * 255 / 63) << 8 | uint32_t(d[2] * 255 / 63);
        }
        BindTexture(GL_TEXTURE_2D, overlay_tex_);
        PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 320, 200, GL_BGRA, GL_UNSIGNED_BYTE, px.data());
        Enable(GL_BLEND);
        BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        // Uploaded images are top row first; the framebuffer has NDC +1 at
        // the image top, so map the quad bottom-up.
        draw_textured(overlay_tex_, -1, -1, 1, 1);
        Disable(GL_BLEND);
    }
    BindFramebuffer(GL_FRAMEBUFFER, 0);
    return out_tex_;
}

GLuint GlRenderer::upload(const uint32_t* argb, int w, int h) {
    BindTexture(GL_TEXTURE_2D, upload_tex_);
    PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (w != upload_w_ || h != upload_h_) {
        TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, argb);
        upload_w_ = w;
        upload_h_ = h;
    } else {
        TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, argb);
    }
    return upload_tex_;
}

void GlRenderer::draw_textured(GLuint tex, float x0, float y0, float x1, float y1) {
    UseProgram(tex_prog_);
    Uniform4f(tex_rect_loc_, x0, y0, x1, y1);
    Uniform1i(tex_sampler_loc_, 0);
    ActiveTexture(GL_TEXTURE0);
    BindTexture(GL_TEXTURE_2D, tex);
    BindVertexArray(quad_vao_);
    DrawArrays(GL_TRIANGLES, 0, 6);
}

void GlRenderer::present(GLuint tex, int win_w, int win_h, float x, float y, float w, float h) {
    BindFramebuffer(GL_FRAMEBUFFER, 0);
    Viewport(0, 0, win_w, win_h);
    ClearColor(0, 0, 0, 1);
    Clear(GL_COLOR_BUFFER_BIT);
    float x0 = x / win_w * 2 - 1, x1 = (x + w) / win_w * 2 - 1;
    float y0 = 1 - y / win_h * 2, y1 = 1 - (y + h) / win_h * 2;
    // Uploads are top row first (t = 0 at the top); render_frame output has
    // the image top at t = 1.
    if (tex != out_tex_) std::swap(y0, y1);
    draw_textured(tex, x0, y0, x1, y1);
}

std::vector<uint8_t> GlRenderer::read_rgb(GLuint tex, int w, int h) {
    std::vector<uint8_t> px(size_t(w) * h * 3);
    if (tex != out_tex_) return px;
    BindFramebuffer(GL_FRAMEBUFFER, out_fbo_);
    PixelStorei(GL_PACK_ALIGNMENT, 1);
    ReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    BindFramebuffer(GL_FRAMEBUFFER, 0);
    // Flip to top row first.
    std::vector<uint8_t> out(px.size());
    for (int y = 0; y < h; y++) std::copy_n(&px[size_t(h - 1 - y) * w * 3], w * 3, &out[size_t(y) * w * 3]);
    return out;
}

}  // namespace f19
