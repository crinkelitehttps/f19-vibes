#include "hires/hires_render.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace f19 {

namespace {

constexpr float kNearZ = 65536.0f;  // engine requires z_hi >= 1

struct Ctx {
    float sx, sy;  // target pixels per 320x200 unit
    const HiresFrame* f;
};

SDL_FColor color_of(const HiresFrame& f, uint8_t c) {
    return SDL_FColor{f.dac[c][0] / 63.0f, f.dac[c][1] / 63.0f, f.dac[c][2] / 63.0f, 1.0f};
}

// Camera space -> target pixels.
SDL_FPoint project(const Ctx& c, const HiresProj& p, const std::array<float, 3>& v) {
    float z = v[2] * p.zdiv;
    float lx = p.cx + 256.0f * v[0] / z;
    float ly = p.cy - 192.0f * v[1] / z;
    return SDL_FPoint{(p.ox + lx) * c.sx, (p.oy + ly) * c.sy};
}

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

void set_clip(SDL_Renderer* r, const Ctx& c, const HiresProj& p) {
    SDL_Rect rc;
    rc.x = int(std::floor(p.ox * c.sx));
    rc.y = int(std::floor(p.oy * c.sy));
    rc.w = int(std::ceil((p.ox + p.vp_w) * c.sx)) - rc.x;
    rc.h = int(std::ceil((p.oy + p.vp_h) * c.sy)) - rc.y;
    SDL_SetRenderClipRect(r, &rc);
}

void fill_polygon(SDL_Renderer* r, const std::vector<SDL_FPoint>& pts, SDL_FColor col) {
    if (pts.size() < 3) return;
    std::vector<SDL_Vertex> v(pts.size());
    for (size_t i = 0; i < pts.size(); i++) v[i] = SDL_Vertex{pts[i], col, {0, 0}};
    std::vector<int> idx;
    for (size_t i = 1; i + 1 < pts.size(); i++) {
        idx.push_back(0);
        idx.push_back(int(i));
        idx.push_back(int(i + 1));
    }
    SDL_RenderGeometry(r, nullptr, v.data(), int(v.size()), idx.data(), int(idx.size()));
}

void thick_line(SDL_Renderer* r, SDL_FPoint a, SDL_FPoint b, float w, SDL_FColor col) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = std::sqrt(dx * dx + dy * dy);
    float nx, ny;
    if (len < 1e-3f) { nx = w * 0.5f; ny = 0; dx = 0; dy = w * 0.5f; }
    else { nx = -dy / len * w * 0.5f; ny = dx / len * w * 0.5f; }
    // Extend by half a width at both ends so short segments stay visible.
    float ex = len < 1e-3f ? 0 : dx / len * w * 0.5f, ey = len < 1e-3f ? 0 : dy / len * w * 0.5f;
    if (len < 1e-3f) { ex = 0; ey = w * 0.5f; nx = w * 0.5f; ny = 0; }
    std::vector<SDL_FPoint> q = {{a.x - ex + nx, a.y - ey + ny}, {b.x + ex + nx, b.y + ey + ny},
                                 {b.x + ex - nx, b.y + ey - ny}, {a.x - ex - nx, a.y - ey - ny}};
    fill_polygon(r, q, col);
}

// Clip a convex polygon (local coords) by the half-plane dot(p - m, u) * sign >= 0.
std::vector<SDL_FPoint> clip_half(const std::vector<SDL_FPoint>& in, float mx, float my, float ux, float uy, float sign) {
    std::vector<SDL_FPoint> out;
    auto side = [&](SDL_FPoint p) { return ((p.x - mx) * ux + (p.y - my) * uy) * sign; };
    for (size_t i = 0; i < in.size(); i++) {
        SDL_FPoint a = in[i], b = in[(i + 1) % in.size()];
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

HiresRenderer::~HiresRenderer() {
    if (target_) SDL_DestroyTexture(target_);
    if (overlay_) SDL_DestroyTexture(overlay_);
}

SDL_Texture* HiresRenderer::render(const HiresFrame& f, int w, int h) {
    if (!target_ || tw_ != w || th_ != h) {
        if (target_) SDL_DestroyTexture(target_);
        target_ = SDL_CreateTexture(r_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
        tw_ = w;
        th_ = h;
    }
    if (!overlay_) {
        overlay_ = SDL_CreateTexture(r_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 200);
        SDL_SetTextureScaleMode(overlay_, SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(overlay_, SDL_BLENDMODE_BLEND);
    }
    SDL_Texture* prev = SDL_GetRenderTarget(r_);
    SDL_SetRenderTarget(r_, target_);
    SDL_SetRenderDrawColor(r_, 0, 0, 0, 255);
    SDL_SetRenderClipRect(r_, nullptr);
    SDL_RenderClear(r_);

    Ctx c{float(w) / 320.0f, float(h) / 200.0f, &f};
    float lw = std::max(1.0f, line_width * c.sx);
    for (const HiresPrim& p : f.prims) {
        set_clip(r_, c, p.proj);
        switch (p.kind) {
            case HiresPrim::Poly: {
                auto clipped = clip_near(p.v);
                if (clipped.size() < 3) break;
                std::vector<SDL_FPoint> pts;
                for (auto& v : clipped) pts.push_back(project(c, p.proj, v));
                fill_polygon(r_, pts, color_of(f, p.color));
                break;
            }
            case HiresPrim::Line: {
                auto a = p.v[0], b = p.v[1];
                bool ain = a[2] >= kNearZ, bin = b[2] >= kNearZ;
                if (!ain && !bin) break;
                if (ain != bin) {
                    float t = (kNearZ - a[2]) / (b[2] - a[2]);
                    std::array<float, 3> m = {a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), kNearZ};
                    if (ain) b = m;
                    else a = m;
                }
                thick_line(r_, project(c, p.proj, a), project(c, p.proj, b), lw, color_of(f, p.color));
                break;
            }
            case HiresPrim::Dot: {
                if (p.v[0][2] < kNearZ) break;
                SDL_FPoint q = project(c, p.proj, p.v[0]);
                float s = std::max(1.0f, c.sx);
                SDL_FRect rc{q.x - s * 0.5f, q.y - s * 0.5f, s, s};
                SDL_FColor col = color_of(f, p.color);
                SDL_SetRenderDrawColorFloat(r_, col.r, col.g, col.b, 1.0f);
                SDL_RenderFillRect(r_, &rc);
                break;
            }
            case HiresPrim::Horizon: {
                std::vector<SDL_FPoint> vp = {{0, 0}, {p.proj.vp_w, 0}, {p.proj.vp_w, p.proj.vp_h}, {0, p.proj.vp_h}};
                auto to_target = [&](std::vector<SDL_FPoint> pts) {
                    for (auto& q : pts) q = {(p.proj.ox + q.x) * c.sx, (p.proj.oy + q.y) * c.sy};
                    return pts;
                };
                if (p.uniform) {
                    fill_polygon(r_, to_target(vp), color_of(f, p.color));
                    break;
                }
                fill_polygon(r_, to_target(clip_half(vp, p.hx, p.hy, p.ux, p.uy, 1.0f)), color_of(f, p.color));
                if (!p.sky_only)
                    fill_polygon(r_, to_target(clip_half(vp, p.hx, p.hy, p.ux, p.uy, -1.0f)), color_of(f, p.color2));
                break;
            }
        }
    }
    SDL_SetRenderClipRect(r_, nullptr);

    // 2D layer: everything not drawn by the world renderer.
    void* pixels;
    int pitch;
    SDL_LockTexture(overlay_, nullptr, &pixels, &pitch);
    for (int y = 0; y < 200; y++) {
        uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pixels) + y * pitch);
        for (int x = 0; x < 320; x++) {
            int i = y * 320 + x;
            const uint8_t* d = f.dac[f.page[i]];
            uint32_t a = f.mask[i] ? 0u : 255u;
            row[x] = a << 24 | uint32_t(d[0] * 255 / 63) << 16 | uint32_t(d[1] * 255 / 63) << 8 | uint32_t(d[2] * 255 / 63);
        }
    }
    SDL_UnlockTexture(overlay_);
    if (draw_overlay) SDL_RenderTexture(r_, overlay_, nullptr, nullptr);
    SDL_SetRenderTarget(r_, prev);
    return target_;
}

}  // namespace f19
