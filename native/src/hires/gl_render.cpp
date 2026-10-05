#include "hires/gl_render.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <map>

#include "hires/scene.h"

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

const char* kSkyVS = R"(#version 330 core
void main() {
    vec2 p = vec2((gl_VertexID & 1) * 4.0 - 1.0, (gl_VertexID >> 1) * 4.0 - 1.0);
    gl_Position = vec4(p, 0.0, 1.0);
})";
// Per-pixel sky/ground: ray in view space -> aircraft space (u_head) ->
// sign against world up.
const char* kSkyFS = R"(#version 330 core
uniform vec2 u_center;  // principal point (window pixels, GL orientation)
uniform vec2 u_focal;
uniform mat3 u_head;
uniform vec3 u_up;
uniform vec3 u_sky;
uniform vec3 u_ground;
out vec4 o_col;
void main() {
    vec3 d = vec3((gl_FragCoord.xy - u_center) / u_focal, 1.0);
    vec3 a = u_head * d;
    o_col = vec4(dot(a, u_up) > 0.0 ? u_sky : u_ground, 1.0);
})";
const char* kPanelVS = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
uniform mat4 u_mvp;
out vec2 v_uv;
void main() {
    gl_Position = u_mvp * vec4(a_pos, 1.0);
    v_uv = a_uv;
})";
const char* kPanelFS = R"(#version 330 core
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

    // 3D cockpit.
    sky_prog_ = link(kSkyVS, kSkyFS);
    const char* sky_names[6] = {"u_center", "u_focal", "u_head", "u_up", "u_sky", "u_ground"};
    for (int i = 0; i < 6; i++) sky_loc_[i] = GetUniformLocation(sky_prog_, sky_names[i]);
    panel_prog_ = link(kPanelVS, kPanelFS);
    panel_mvp_loc_ = GetUniformLocation(panel_prog_, "u_mvp");
    panel_tex_loc_ = GetUniformLocation(panel_prog_, "u_tex");
    GenVertexArrays(1, &panel_vao_);
    GenBuffers(1, &panel_vbo_);
    BindVertexArray(panel_vao_);
    BindBuffer(GL_ARRAY_BUFFER, panel_vbo_);
    VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void*>(0));
    EnableVertexAttribArray(0);
    VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
    EnableVertexAttribArray(1);
    BindVertexArray(0);
    GenTextures(1, &panel_tex_);
    BindTexture(GL_TEXTURE_2D, panel_tex_);
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 320, 200, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
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

namespace {
bool object_is_flat(const std::vector<const HiresPrim*>& prims);
}
// An object is flat if all its vertices lie on one plane (ground tiles,
// decals such as runway markings and roads): those keep the engine's
// painter's order instead of depth testing among themselves.
namespace {
bool object_is_flat(const std::vector<const HiresPrim*>& prims) {
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
}  // namespace

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
    draw_groups();
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

void GlRenderer::draw_groups() {
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
}

namespace {
struct M3 {
    float m[3][3];
    std::array<float, 3> mul(const std::array<float, 3>& v) const {
        return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
                m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
    }
};
M3 mul(const M3& a, const M3& b) {
    M3 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}
// Head orientation in the aircraft frame: yaw (right +) about y, then pitch
// (up +) about x, then roll (right ear down +) about z.
M3 head_matrix(float yaw, float pitch, float roll) {
    float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    float cr = std::cos(roll), sr = std::sin(roll);
    // H = Ry(yaw) * Rx(-pitch) * Rz(-roll): columns are the view axes in aircraft space.
    M3 yp{{{cy, -sy * sp, sy * cp}, {0, cp, sp}, {-sy, -cy * sp, cy * cp}}};
    return mul(yp, M3{{{cr, sr, 0}, {-sr, cr, 0}, {0, 0, 1}}});
}
M3 transpose(const M3& a) {
    M3 t;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) t.m[i][j] = a.m[j][i];
    return t;
}
// The first primitive of the main view (the full-width viewport at the page
// origin), or null: its projection state is the engine's camera.
const HiresPrim* main_view_prim(const HiresFrame& f) {
    for (auto& p : f.prims)
        if (p.proj.ox == 0 && p.proj.oy == 0 && p.proj.vp_w >= 320) return &p;
    return nullptr;
}
}  // namespace

bool GlRenderer::render_cockpit3d(const HiresFrame& f, int w, int h, const HeadPose& head, GLuint* out) {
    // Keep the original horizontal FOV at 4:3; wider windows see more.
    const float tan_v = std::tan(hfov_4x3_deg * 0.5f * 3.14159265f / 180.0f) * 0.75f;
    const float tan_h = tan_v * float(w) / float(h);
    EyeView eye;
    M3 H = head_matrix(head.yaw, head.pitch, head.roll);
    std::copy_n(&H.m[0][0], 9, &eye.rot[0][0]);
    eye.pos[0] = head.x;
    eye.pos[1] = head.y;
    eye.pos[2] = -head.z;  // HeadPose z is back +
    eye.tan_left = -tan_h;
    eye.tan_right = tan_h;
    eye.tan_down = -tan_v;
    eye.tan_up = tan_v;
    // Cockpit view: frame it as the original screen, the engine's
    // projection centre (the HUD crosshair) above the window centre, so the
    // HUD and the panel below it are both in view.
    if (const HiresPrim* m = main_view_prim(f); m && m->proj.vp_h < 200) {
        eye.tan_up = std::clamp(m->proj.cy / 192.0f, 0.0f, 2 * tan_v);
        eye.tan_down = eye.tan_up - 2 * tan_v;
    }
    *out = render_view(f, w, h, eye, 0);
    return true;
}

GLuint GlRenderer::render_view(const HiresFrame& f, int w, int h, const EyeView& eye, GLuint dst) {
    // The main view is the full-width viewport at the page origin; in the
    // cockpit view it stops above the instrument panel.
    const HiresPrim* main = main_view_prim(f);
    const HiresPrim* hor = main && main->kind == HiresPrim::Horizon ? main : nullptr;
    const float vp_h = main ? main->proj.vp_h : 200;
    const bool external = vp_h >= 200;  // external views: no panel; 2D layer centred 4:3

    ensure_framebuffers(w, h);
    // Pinhole camera in pixels (top-left origin) from the frustum.
    const float fx = w / (eye.tan_right - eye.tan_left), fy = h / (eye.tan_up - eye.tan_down);
    const float cx = -eye.tan_left * fx, cy = eye.tan_up * fy;
    M3 H;
    std::copy_n(&eye.rot[0][0], 9, &H.m[0][0]);
    M3 V = transpose(H);  // V: aircraft -> view
    // The eye's offset in the world (camera-space units: 65536 per world unit).
    const float wk = world_units_per_cm * 65536.0f;
    const std::array<float, 3> eye_world = {eye.pos[0] * wk, eye.pos[1] * wk, eye.pos[2] * wk};

    // World primitives of the main view, projected by our camera: the
    // native scene, or the engine's captured primitives.
    verts_.clear();
    runs_.clear();
    groups_.clear();
    const std::vector<HiresPrim>* prims = &f.prims;
    if (native_world && f.scene) {
        SceneBuildParams sp;
        // The engine's LOD distances suit its 256-pixel focal length; scale
        // them to ours so models switch at the same on-screen size.
        sp.lod_scale = std::max(1.0f, fx / 256.0f) * lod_detail;
        // Both eyes of a stereo frame share the build.
        if (&f != scene_frame_ || f.time_us != scene_time_ || sp.lod_scale != scene_lod_) {
            build_scene_prims(*f.scene, sp, scene_prims_);
            scene_frame_ = &f;
            scene_time_ = f.time_us;
            scene_lod_ = sp.lod_scale;
        }
        prims = &scene_prims_;
    }
    std::map<uint32_t, std::vector<const HiresPrim*>> by_object;
    for (const HiresPrim& p : *prims) by_object[p.object].push_back(&p);
    std::map<uint32_t, bool> flat;
    for (auto& [obj, list] : by_object) flat[obj] = list.front()->flat >= 0 ? list.front()->flat == 1 : object_is_flat(list);
    runs_.push_back(Run{0, 0, 0, 0, w, h});
    auto color = [&](uint8_t c) { return std::array<float, 4>{f.dac[c][0] / 63.0f, f.dac[c][1] / 63.0f, f.dac[c][2] / 63.0f, 1.0f}; };
    auto tri_fan = [&](const std::vector<P2>& pts, std::array<float, 4> c) {
        for (size_t i = 1; i + 1 < pts.size(); i++)
            for (const P2& p : {pts[0], pts[i], pts[i + 1]}) verts_.push_back({p.x, p.y, p.d, c[0], c[1], c[2], c[3]});
    };
    auto view = [&](const std::array<float, 3>& v) {
        return V.mul({v[0] - eye_world[0], v[1] - eye_world[1], v[2] - eye_world[2]});
    };
    auto project = [&](const std::array<float, 3>& v) {  // view space, z >= near
        return P2{cx + fx * v[0] / v[2], cy - fy * v[1] / v[2], kNearZ / v[2]};
    };
    const float lw = std::max(1.0f, line_width * float(w) / 320.0f);
    for (const HiresPrim& p : *prims) {
        if (!(p.proj.ox == 0 && p.proj.oy == 0 && p.proj.vp_w >= 320)) continue;  // sub-views live on the panel
        if (p.kind == HiresPrim::Horizon) continue;                              // sky shader instead
        bool is_flat = p.flat >= 0 ? p.flat == 1 : (p.kind == HiresPrim::Dot || flat[p.object]);
        if (groups_.empty() || groups_.back().object != p.object || groups_.back().flat != is_flat)
            groups_.push_back(Group{int(verts_.size()), 0, 0, p.object, is_flat, false});
        std::vector<std::array<float, 3>> vv;
        for (auto& v : p.v) vv.push_back(view(v));
        if (p.kind == HiresPrim::Poly) {
            auto clipped = clip_near(vv);
            if (clipped.size() >= 3) {
                std::vector<P2> pts;
                for (auto& v : clipped) pts.push_back(project(v));
                tri_fan(pts, color(p.color));
            }
        } else if (p.kind == HiresPrim::Line) {
            auto a = vv[0], b = vv[1];
            bool ain = a[2] >= kNearZ, bin = b[2] >= kNearZ;
            if (ain || bin) {
                if (ain != bin) {
                    float t = (kNearZ - a[2]) / (b[2] - a[2]);
                    std::array<float, 3> m = {a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), kNearZ};
                    (ain ? b : a) = m;
                }
                P2 pa = project(a), pb = project(b);
                float dx = pb.x - pa.x, dy = pb.y - pa.y, len = std::sqrt(dx * dx + dy * dy), hw = lw * 0.5f;
                float tx, ty, nx, ny;
                if (len < 1e-3f) { tx = hw; ty = 0; nx = 0; ny = hw; }
                else { tx = dx / len * hw; ty = dy / len * hw; nx = -ty; ny = tx; }
                tri_fan({{pa.x - tx + nx, pa.y - ty + ny, pa.d}, {pb.x + tx + nx, pb.y + ty + ny, pb.d},
                         {pb.x + tx - nx, pb.y + ty - ny, pb.d}, {pa.x - tx - nx, pa.y - ty - ny, pa.d}}, color(p.color));
            }
        } else if (p.kind == HiresPrim::Dot) {
            if (vv[0][2] >= kNearZ) {
                P2 q = project(vv[0]);
                float s = std::max(1.0f, float(w) / 320.0f) * 0.5f;
                tri_fan({{q.x - s, q.y - s, q.d}, {q.x + s, q.y - s, q.d}, {q.x + s, q.y + s, q.d}, {q.x - s, q.y + s, q.d}},
                        color(p.color));
            }
        }
        groups_.back().count = int(verts_.size()) - groups_.back().first;
    }
    runs_.back().count = int(verts_.size());

    BindFramebuffer(GL_FRAMEBUFFER, ms_fbo_);
    Viewport(0, 0, w, h);
    Disable(GL_SCISSOR_TEST);
    ClearColor(0, 0, 0, 1);
    ClearDepth(0.0);
    DepthMask(GL_TRUE);
    Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    Disable(GL_DEPTH_TEST);
    Disable(GL_BLEND);

    // Sky and ground.
    if (hor) {
        UseProgram(sky_prog_);
        Uniform2f(sky_loc_[0], cx, h - cy);
        Uniform2f(sky_loc_[1], fx, fy);
        float hm[9];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) hm[j * 3 + i] = H.m[i][j];  // column-major
        UniformMatrix3fv(sky_loc_[2], 1, GL_FALSE, hm);
        Uniform3f(sky_loc_[3], hor->up[0], hor->up[1], hor->up[2]);
        auto sky = color(hor->color), ground = color(hor->color2);
        Uniform3f(sky_loc_[4], sky[0], sky[1], sky[2]);
        Uniform3f(sky_loc_[5], ground[0], ground[1], ground[2]);
        BindVertexArray(quad_vao_);
        DrawArrays(GL_TRIANGLES, 0, 3);
    }

    // World.
    UseProgram(world_prog_);
    Uniform2f(world_size_loc_, float(w), float(h));
    BindVertexArray(vao_);
    BindBuffer(GL_ARRAY_BUFFER, vbo_);
    BufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts_.size() * sizeof(Vert)), verts_.data(), GL_STREAM_DRAW);
    draw_groups();

    // Cockpit surfaces' transform: perspective (same focal as the world) *
    // aircraft->view rotation * eye offset (head position; z back +,
    // aircraft z forward). The eye stays behind z = max_z.
    auto mul4 = [](const float* a, const float* b, float* r) {  // column-major r = a b
        for (int col = 0; col < 4; col++)
            for (int row = 0; row < 4; row++) {
                float acc = 0;
                for (int k = 0; k < 4; k++) acc += a[k * 4 + row] * b[col * 4 + k];
                r[col * 4 + row] = acc;
            }
    };
    auto cockpit_mvp = [&](float max_z, float* P, float* R, float* mvp) {
        float ex = eye.pos[0] * panel_units_per_cm, ey = eye.pos[1] * panel_units_per_cm;
        float ez = std::min(eye.pos[2] * panel_units_per_cm, max_z);
        float tv[3];
        for (int i = 0; i < 3; i++) tv[i] = -(V.m[i][0] * ex + V.m[i][1] * ey + V.m[i][2] * ez);
        float sx = 2 * fx / w, sy = 2 * fy / h, n = 0.01f, fa = 100.0f;
        float ox = 2 * cx / w - 1, oy = 1 - 2 * cy / h;  // off-centre (asymmetric) frustum
        const float p[16] = {sx, 0, 0, 0, 0, sy, 0, 0, ox, oy, (fa + n) / (fa - n), 1, 0, 0, -2 * fa * n / (fa - n), 0};
        const float r[16] = {V.m[0][0], V.m[1][0], V.m[2][0], 0, V.m[0][1], V.m[1][1], V.m[2][1], 0,
                             V.m[0][2], V.m[1][2], V.m[2][2], 0, tv[0], tv[1], tv[2], 1};
        std::copy_n(p, 16, P);
        std::copy_n(r, 16, R);
        mul4(P, R, mvp);
    };
    auto draw_quad = [&](const float (&q)[4][5]) {  // corners TL, TR, BR, BL: x, y, z, u, v
        float tri[6][5];
        int order[6] = {0, 1, 2, 0, 2, 3};
        for (int i = 0; i < 6; i++) std::copy_n(q[order[i]], 5, tri[i]);
        BindVertexArray(panel_vao_);
        BindBuffer(GL_ARRAY_BUFFER, panel_vbo_);
        BufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STREAM_DRAW);
        DrawArrays(GL_TRIANGLES, 0, 6);
    };
    // The manual's clipboard, in front of everything in the cockpit.
    auto draw_clipboard = [&] {
        if (!show_manual || !manual_w_) return;
        const float* mc = manual_center;
        float mvp[16], P[16], R[16];
        cockpit_mvp(mc[2] - 0.2f, P, R, mvp);
        UseProgram(panel_prog_);
        UniformMatrix4fv(panel_mvp_loc_, 1, GL_FALSE, mvp);
        Uniform1i(panel_tex_loc_, 0);
        ActiveTexture(GL_TEXTURE0);
        BindTexture(GL_TEXTURE_2D, manual_tex_);
        Disable(GL_DEPTH_TEST);
        Disable(GL_BLEND);
        float mw = manual_width, mh = mw * manual_h_ / manual_w_;
        float t = manual_tilt_deg * 3.14159265f / 180.0f, uy = std::cos(t), uz = std::sin(t);
        float q[4][5] = {
            {mc[0] - mw / 2, mc[1] + uy * mh / 2, mc[2] + uz * mh / 2, 0, 0},
            {mc[0] + mw / 2, mc[1] + uy * mh / 2, mc[2] + uz * mh / 2, 1, 0},
            {mc[0] + mw / 2, mc[1] - uy * mh / 2, mc[2] - uz * mh / 2, 1, 1},
            {mc[0] - mw / 2, mc[1] - uy * mh / 2, mc[2] - uz * mh / 2, 0, 1},
        };
        draw_quad(q);
    };

    // Cockpit surfaces, fixed in the aircraft frame: the HUD (the page's 2D
    // layer above the 3D viewport) on an upright quad ahead of the panel, and
    // the instrument panel (page rows below the viewport) on a tilted quad.
    if (!external) {
        // One texture for both: HUD rows keep only non-world pixels
        // (premultiplied alpha), panel rows are opaque.
        static std::vector<uint32_t> px(64000);
        const int hud_rows = int(vp_h);
        for (int i = 0; i < 64000; i++) {
            const uint8_t* d = f.dac[f.page[i]];
            px[i] = (i < hud_rows * 320 && f.mask[i])
                        ? 0u
                        : 0xFF000000u | uint32_t(d[0] * 255 / 63) << 16 | uint32_t(d[1] * 255 / 63) << 8 | uint32_t(d[2] * 255 / 63);
        }
        BindTexture(GL_TEXTURE_2D, panel_tex_);
        PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 320, 200, GL_BGRA, GL_UNSIGNED_BYTE, px.data());

        const float* c = panel_center;
        float mvp[16], P[16], R[16];
        cockpit_mvp(c[2] - 0.25f, P, R, mvp);
        UseProgram(panel_prog_);
        UniformMatrix4fv(panel_mvp_loc_, 1, GL_FALSE, mvp);
        Uniform1i(panel_tex_loc_, 0);
        ActiveTexture(GL_TEXTURE0);
        BindTexture(GL_TEXTURE_2D, panel_tex_);
        Disable(GL_DEPTH_TEST);

        // HUD first (the panel may cover its lower edge). Collimated, as
        // a real HUD: drawn at infinity (rotation only, no eye offset), so
        // both eyes see it in the same direction as the distant world and
        // head movement does not shift it. Each page pixel is placed in the
        // direction the engine's projection gives it (x = cx + 256 X/Z,
        // y = cy - 192 Y/Z), so the game's symbology (target boxes,
        // gun cross, tracers) overlays the world where the engine aims it.
        if (draw_overlay) {
            float pcx = main ? main->proj.cx : hud_cross[0], pcy = main ? main->proj.cy : hud_cross[1];
            float zd = main ? main->proj.zdiv : 1.0f;
            // The viewport's last row is the panel's top bevel, not HUD
            // (half a texel more, so filtering does not pull it in).
            float x0 = hud_cols[0], x1 = hud_cols[1] + 1.0f, y1 = float(hud_rows) - 1.5f;
            float hx0 = (x0 - pcx) * zd / 256.0f, hx1 = (x1 - pcx) * zd / 256.0f;
            float hy0 = (pcy - 0) * zd / 192.0f, hy1 = (pcy - y1) * zd / 192.0f;
            float q[4][5] = {{hx0, hy0, 1, x0 / 320, 0},
                             {hx1, hy0, 1, x1 / 320, 0},
                             {hx1, hy1, 1, x1 / 320, y1 / 200},
                             {hx0, hy1, 1, x0 / 320, y1 / 200}};
            float R0[16];
            std::copy_n(R, 16, R0);
            R0[12] = R0[13] = R0[14] = 0;
            float mvp0[16];
            mul4(P, R0, mvp0);
            UniformMatrix4fv(panel_mvp_loc_, 1, GL_FALSE, mvp0);
            Enable(GL_BLEND);
            BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            draw_quad(q);
            Disable(GL_BLEND);
            UniformMatrix4fv(panel_mvp_loc_, 1, GL_FALSE, mvp);
        }

        float rows = 200.0f - vp_h;
        float pw = panel_width, ph = pw * rows * 1.2f / 320.0f;  // 4:3 pixel aspect
        float t = panel_tilt_deg * 3.14159265f / 180.0f;
        float uy = std::cos(t), uz = std::sin(t);  // panel "up" leans away
        float v0 = vp_h / 200.0f;
        float q[4][5] = {
            {c[0] - pw / 2, c[1] + uy * ph / 2, c[2] + uz * ph / 2, 0, v0},
            {c[0] + pw / 2, c[1] + uy * ph / 2, c[2] + uz * ph / 2, 1, v0},
            {c[0] + pw / 2, c[1] - uy * ph / 2, c[2] - uz * ph / 2, 1, 1},
            {c[0] - pw / 2, c[1] - uy * ph / 2, c[2] - uz * ph / 2, 0, 1},
        };
        draw_quad(q);
        draw_clipboard();
    }

    BindFramebuffer(GL_READ_FRAMEBUFFER, ms_fbo_);
    BindFramebuffer(GL_DRAW_FRAMEBUFFER, out_fbo_);
    BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (external && draw_overlay) {
        // The game's 2D elements (text etc.) at original proportions, centred
        // 4:3 at full height; world pixels transparent.
        BindFramebuffer(GL_FRAMEBUFFER, out_fbo_);
        Viewport(0, 0, w, h);
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
        float half = std::min(1.0f, (h * 4.0f / 3.0f) / w);  // NDC half-width of the 4:3 area
        draw_textured(overlay_tex_, -half, -1, half, 1);
        Disable(GL_BLEND);
    }
    if (external) {
        BindFramebuffer(GL_FRAMEBUFFER, out_fbo_);
        Viewport(0, 0, w, h);
        draw_clipboard();
    }
    if (dst) {
        BindFramebuffer(GL_READ_FRAMEBUFFER, out_fbo_);
        BindFramebuffer(GL_DRAW_FRAMEBUFFER, dst);
        BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    BindFramebuffer(GL_FRAMEBUFFER, 0);
    return out_tex_;
}

void GlRenderer::set_manual(const uint32_t* argb, int w, int h) {
    if (!manual_tex_) {
        GenTextures(1, &manual_tex_);
        BindTexture(GL_TEXTURE_2D, manual_tex_);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Anisotropic filtering keeps text sharp on the tilted board
        // (EXT_texture_filter_anisotropic; ignored where unsupported).
        TexParameterf(GL_TEXTURE_2D, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY */, 8.0f);
        GetError();
    }
    BindTexture(GL_TEXTURE_2D, manual_tex_);
    PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (w != manual_w_ || h != manual_h_) {
        TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, argb);
        manual_w_ = w;
        manual_h_ = h;
    } else {
        TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, argb);
    }
    GenerateMipmap(GL_TEXTURE_2D);
}

void GlRenderer::draw_manual_2d(GLuint fbo, int fb_w, int fb_h) {
    if (!show_manual || !manual_w_) return;
    BindFramebuffer(GL_FRAMEBUFFER, fbo);
    Viewport(0, 0, fb_w, fb_h);
    Disable(GL_BLEND);
    // Fit 96% of the framebuffer, keeping the image's aspect.
    float s = 0.96f * std::min(float(fb_w) / manual_w_, float(fb_h) / manual_h_);
    float hw = manual_w_ * s / fb_w, hh = manual_h_ * s / fb_h;  // NDC half sizes
    draw_textured(manual_tex_, -hw, -hh, hw, hh);  // uploads: y0 is the bottom
    BindFramebuffer(GL_FRAMEBUFFER, 0);
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

void GlRenderer::present_to(GLuint fbo, GLuint tex, int win_w, int win_h, float x, float y, float w, float h) {
    BindFramebuffer(GL_FRAMEBUFFER, fbo);
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

std::vector<uint8_t> GlRenderer::read_window(int w, int h) {
    std::vector<uint8_t> px(size_t(w) * h * 3), out(px.size());
    BindFramebuffer(GL_FRAMEBUFFER, 0);
    PixelStorei(GL_PACK_ALIGNMENT, 1);
    ReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < h; y++) std::copy_n(&px[size_t(h - 1 - y) * w * 3], w * 3, &out[size_t(y) * w * 3]);
    return out;
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
