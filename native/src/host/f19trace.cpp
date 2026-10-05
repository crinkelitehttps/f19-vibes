// Headless runner for bring-up: boots a DOS program from the game directory,
// logs DOS/BIOS service calls, and dumps the VGA (mode 13h) screen.
//
// Usage: f19trace GAMEDIR [-p PROGRAM] [-a ARGS] [-n MILLIONS] [-t] [-s OUT.ppm] [-k KEYS] [-w OUT.wav]
//   -k KEYS: keys to type, one per emulated second ('\n' = Enter).
//   -w OUT.wav: record the PC speaker (48 kHz mono).
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>
#include <vector>
#include <cstdio>
#include <tuple>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/dos.h"
#include "core/machine.h"
#include "drivers/mgraphic.h"
#include "hires/gl_render.h"
#include "hires/scene.h"
#include "hires/world_capture.h"
#include <SDL3/SDL.h>

using namespace f19;

static void screenshot(Machine& m, const char* path) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n320 200\n255\n");
    for (int i = 0; i < 64000; i++) {
        uint8_t c = m.mem.read8(0xA0000 + i);
        uint8_t rgb[3] = {uint8_t(m.dac[c][0] * 255 / 63), uint8_t(m.dac[c][1] * 255 / 63), uint8_t(m.dac[c][2] * 255 / 63)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

// US keyboard scan codes for printable ASCII (set 1).
static uint8_t scan_for(char c) {
    static const char* rows[] = {"1234567890-=", "qwertyuiop[]", "asdfghjkl;'`", "zxcvbnm,./"};
    static const uint8_t base[] = {0x02, 0x10, 0x1E, 0x2C};
    char l = char(std::tolower(uint8_t(c)));
    for (int r = 0; r < 4; r++)
        for (int i = 0; rows[r][i]; i++)
            if (rows[r][i] == l) return uint8_t(base[r] + i);
    if (c == ' ') return 0x39;
    if (c == '\n') return 0x1C;
    return 0;
}

static void write_wav(const char* path, const std::vector<float>& samples) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    uint32_t bytes = uint32_t(samples.size() * 2);
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(1); u32(PcSpeaker::kRate); u32(PcSpeaker::kRate * 2); u16(2); u16(16);
    std::fwrite("data", 1, 4, f); u32(bytes);
    for (float s : samples) u16(uint16_t(int16_t(std::lround(s * 32767))));
    std::fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s GAMEDIR [-p PROGRAM] [-a ARGS] [-n MILLIONS] [-t] [-s OUT.ppm] [-k KEYS] [-w OUT.wav]\n", argv[0]);
        return 2;
    }
    std::string program = "F19.COM", args, shot, keys, hires_shot, wav;
    double millions = 50;
    double mips_opt = 4.0, refresh_opt = 70.086;
    bool trace = false, prof = false, drv_trace = false, native_drv = false, verify_drv = false;
    for (int i = 2; i < argc; i++) {
        if (!std::strcmp(argv[i], "-p") && i + 1 < argc) program = argv[++i];
        else if (!std::strcmp(argv[i], "-a") && i + 1 < argc) args = argv[++i];
        else if (!std::strcmp(argv[i], "-n") && i + 1 < argc) millions = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "-t")) trace = true;
        else if (!std::strcmp(argv[i], "-P")) prof = true;
        else if (!std::strcmp(argv[i], "-D")) drv_trace = true;
        else if (!std::strcmp(argv[i], "-m") && i + 1 < argc) mips_opt = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "-r") && i + 1 < argc) refresh_opt = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "-N")) native_drv = true;
        else if (!std::strcmp(argv[i], "-V")) native_drv = verify_drv = true;
        else if (!std::strcmp(argv[i], "-H") && i + 1 < argc) { hires_shot = argv[++i]; native_drv = true; }
        else if (!std::strcmp(argv[i], "-s") && i + 1 < argc) shot = argv[++i];
        else if (!std::strcmp(argv[i], "-k") && i + 1 < argc) keys = argv[++i];
        else if (!std::strcmp(argv[i], "-w") && i + 1 < argc) wav = argv[++i];
    }
    Machine m(argv[1]);
    m.trace = trace;
    m.ips_per_ms = uint32_t(mips_opt * 1000);
    m.vga_refresh_hz = refresh_opt;
    m.speaker.enabled = !wav.empty();

    // -D: trace calls into the graphics driver's exported entry points.
    struct EntryStats { std::vector<int> slots; uint64_t calls = 0; std::map<uint32_t, uint64_t> callers; std::vector<std::string> samples; };
    std::map<uint32_t, EntryStats> entries;
    auto record = [&](uint32_t lin) {
        auto& e = entries[lin];
        e.calls++;
        uint16_t ss = m.cpu.regs.s[SS], sp = m.cpu.regs.r[SP];
        uint16_t rip = m.mem.read16(ss, sp), rcs = m.mem.read16(ss, uint16_t(sp + 2));
        e.callers[(uint32_t(rcs) << 16) | rip]++;
        if (e.samples.size() < 4) {
            char b[160];
            std::snprintf(b, sizeof b, "args %04X %04X %04X %04X %04X %04X  AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X ES=%04X",
                          m.mem.read16(ss, uint16_t(sp + 4)), m.mem.read16(ss, uint16_t(sp + 6)), m.mem.read16(ss, uint16_t(sp + 8)),
                          m.mem.read16(ss, uint16_t(sp + 10)), m.mem.read16(ss, uint16_t(sp + 12)), m.mem.read16(ss, uint16_t(sp + 14)),
                          m.cpu.regs.r[AX], m.cpu.regs.r[BX], m.cpu.regs.r[CX], m.cpu.regs.r[DX], m.cpu.regs.r[SI], m.cpu.regs.r[DI], m.cpu.regs.s[ES]);
            if (std::find(e.samples.begin(), e.samples.end(), b) == e.samples.end()) e.samples.push_back(b);
        }
    };
    if (drv_trace) {
        m.overlay_listeners.push_back([&](const std::string& name, uint16_t seg) {
            if (name.find("GRAPHIC") == std::string::npos) return;
            uint16_t adj = m.mem.read16(seg, 0x18), n = m.mem.read16(seg, 0x22);
            uint8_t first = m.mem.read8(Memory::linear(seg, 0x1C));
            entries.clear();
            for (int i = 0; i < n; i++) {
                uint16_t off = m.mem.read16(seg, uint16_t(0x24 + 2 * i));
                uint32_t lin = Memory::linear(adj, off);   // +0x18 is relocated at load: already a segment
                if (entries[lin].slots.empty())
                    m.add_breakpoint(lin, [&record, lin] { record(lin); return false; });
                entries[lin].slots.push_back(first + i);
            }
            std::fprintf(stderr, "driver %s at %04X: %u entries\n", name.c_str(), seg, n);
        });
    }
    std::unique_ptr<MGraphicNative> gfx;
    std::unique_ptr<WorldCapture> capture;
    if (native_drv) {
        gfx = std::make_unique<MGraphicNative>(m);
        gfx->verify = verify_drv;
        if (!hires_shot.empty()) capture = std::make_unique<WorldCapture>(m, *gfx);
    }
    if (!m.dos->start_program(program, args)) {
        std::fprintf(stderr, "cannot start %s\n", program.c_str());
        return 1;
    }
    uint64_t total = uint64_t(millions * 1e6);
    uint64_t slice = uint64_t(m.ips_per_ms) * 1000;  // one emulated second
    size_t k = 0;
    std::map<uint32_t, uint64_t> profile;   // CS:IP>>4 buckets, sampled
    while (!m.exited && m.cpu.instructions < total) {
        for (uint64_t done = 0; done < slice && !m.exited; done += 997) {
            m.run(997);
            profile[(uint32_t(m.cpu.regs.s[CS]) << 16) | (m.cpu.regs.ip & 0xFFF0)]++;
        }
        if (k < keys.size()) {
            char c = keys[k++];
            m.key_press(scan_for(c), c == '\n' ? 0x0D : uint8_t(c));
        }
    }
    std::fprintf(stderr, "stopped after %llu instructions (%.1f s emulated): %s\n",
                 (unsigned long long)m.cpu.instructions, m.now_us() / 1e6,
                 m.stop_reason.empty() ? "budget reached" : m.stop_reason.c_str());
    std::fprintf(stderr, "CPU at %04X:%04X\n", m.cpu.regs.s[CS], m.cpu.regs.ip);
    if (!shot.empty()) screenshot(m, shot.c_str());
    if (!wav.empty()) {
        m.speaker.advance(m.pit_ticks());
        write_wav(wav.c_str(), m.speaker.samples);
        std::fprintf(stderr, "wrote %s: %.1f s\n", wav.c_str(), m.speaker.samples.size() / double(PcSpeaker::kRate));
    }
    if (gfx && verify_drv) std::fprintf(stderr, "%s", gfx->report().c_str());
    if (gfx) {
        std::fprintf(stderr, "flips %llu in %.1f emulated s\n", (unsigned long long)gfx->stats[44].calls, m.now_us() / 1e6);
        if (std::getenv("F19_FLIP_LOG")) {
            FILE* fl = std::fopen(std::getenv("F19_FLIP_LOG"), "w");
            for (uint64_t t : gfx->flip_times_us) std::fprintf(fl, "%llu\n", (unsigned long long)t);
            std::fclose(fl);
        }
    }
    if (capture) {
        std::fprintf(stderr, "hires: %s\n", capture->status.c_str());
        std::fprintf(stderr, "hires dbg: poly %llu wrongseg %llu few-edges %llu short-loop %llu | line %llu wrongseg %llu | draw seg %04X page1 %04X\n",
                     (unsigned long long)capture->dbg[0], (unsigned long long)capture->dbg[1], (unsigned long long)capture->dbg[2],
                     (unsigned long long)capture->dbg[3], (unsigned long long)capture->dbg[4], (unsigned long long)capture->dbg[5],
                     gfx->current_draw_seg(), gfx->page_seg(1));
        std::fprintf(stderr, "hires dbg: edge_setup %llu; poly hook at %05X bytes:", (unsigned long long)capture->dbg[6], capture->poly_lin_);
        for (int k = -10; k < 6; k++) std::fprintf(stderr, " %02X", m.mem.read8(capture->poly_lin_ + k));
        std::fprintf(stderr, "\n");
        auto frame = capture->latest();
        if (!frame) std::fprintf(stderr, "hires: no frame captured\n");
        else {
            size_t counts[4] = {};
            for (auto& p : frame->prims) counts[p.kind]++;
            if (const char* dump = std::getenv("F19_DUMP_PRIMS")) {
                FILE* fd = std::fopen(dump, "w");
                for (auto& p : frame->prims) {
                    const uint8_t* d = frame->dac[p.color];
                    std::fprintf(fd, "%d col %3d rgb %02d%02d%02d c2 %3d cx %.0f cy %.0f zdiv %.3f vp %.0fx%.0f org %.0f,%.0f",
                                 p.kind, p.color, d[0], d[1], d[2], p.color2, p.proj.cx, p.proj.cy, p.proj.zdiv, p.proj.vp_w, p.proj.vp_h, p.proj.ox, p.proj.oy);
                    if (p.kind == HiresPrim::Horizon)
                        std::fprintf(fd, " M %.1f,%.1f U %.3f,%.3f uniform %d skyonly %d", p.hx, p.hy, p.ux, p.uy, p.uniform, p.sky_only);
                    for (auto& v : p.v) std::fprintf(fd, " (%.0f %.0f %.0f)", v[0], v[1], v[2]);
                    std::fprintf(fd, "\n");
                }
                std::fclose(fd);
            }
            std::fprintf(stderr, "hires: frame with %zu polys, %zu lines, %zu dots, %zu horizon\n", counts[0], counts[1], counts[2], counts[3]);
            SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
            SDL_Init(SDL_INIT_VIDEO);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
            SDL_Window* win = SDL_CreateWindow("f19trace", 64, 64, SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);
            SDL_GLContext glc = SDL_GL_CreateContext(win);
            if (!glc || !gl::load()) { std::fprintf(stderr, "hires: no OpenGL: %s\n", SDL_GetError()); return 1; }
            GlRenderer gr;
            gr.init();
            auto save = [&](const std::string& path, bool overlay) {
                gr.draw_overlay = overlay;
                GLuint tex = gr.render_frame(*frame, 1280, 960);
                auto rgb = gr.read_rgb(tex, 1280, 960);
                FILE* fo = std::fopen(path.c_str(), "wb");
                std::fprintf(fo, "P6\n1280 960\n255\n");
                std::fwrite(rgb.data(), 1, rgb.size(), fo);
                std::fclose(fo);
            };
            save(hires_shot, true);
            save(hires_shot + ".world.ppm", false);
            gr.draw_overlay = true;
            if (frame->scene)
                std::fprintf(stderr, "scene: %zu instances in the last frame; %llu frames, avg terrain %.0f dynamic %.1f, %llu destroyed-state changes\n",
                             frame->scene->instances.size(), (unsigned long long)capture->scene_stats[0],
                             capture->scene_stats[0] ? double(capture->scene_stats[1]) / double(capture->scene_stats[0]) : 0.0,
                             capture->scene_stats[0] ? double(capture->scene_stats[2]) / double(capture->scene_stats[0]) : 0.0,
                             (unsigned long long)capture->scene_stats[3]);
            else
                std::fprintf(stderr, "scene: none in the last frame\n");
            if (frame->scene) {
                std::vector<HiresPrim> sp;
                SceneBuildParams bp;
                bp.lod_scale = 3.0f;
                auto t0 = std::chrono::steady_clock::now();
                for (int i = 0; i < 20; i++) build_scene_prims(*frame->scene, bp, sp);
                double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20;
                size_t kinds[4] = {};
                for (auto& p : sp) kinds[p.kind]++;
                size_t lv[5] = {};
                for (auto& in : frame->scene->instances) lv[std::clamp<int>(in.level, 0, 4)]++;
                std::fprintf(stderr, "scene: instances per level: dynamic %zu, L1 %zu, L2 %zu, L3 %zu, L4 %zu\n", lv[0], lv[1], lv[2], lv[3], lv[4]);
                std::fprintf(stderr, "scene: build %.2f ms -> %zu polys, %zu lines, %zu dots\n", ms, kinds[0], kinds[1], kinds[2]);
            }
            struct Shot { const char* suffix; HeadPose head; bool native; };
            for (auto [suffix, head, native] : {Shot{".cockpit3d.ppm", HeadPose{}, true},
                                                Shot{".cockpit3d-captured.ppm", HeadPose{}, false},
                                                Shot{".cockpit3d-look.ppm", HeadPose{0.35f, -0.15f}, true},
                                                Shot{".cockpit3d-left.ppm", HeadPose{-1.4f, -0.1f}, true},
                                                Shot{".cockpit3d-behind.ppm", HeadPose{2.6f, 0.1f}, true},
                                                Shot{".cockpit3d-lean.ppm", HeadPose{0.2f, -0.3f, 0.25f, 8, 0, -15}, true}, Shot{".eyeL.ppm", HeadPose{0, 0, 0, -3.2f, 0, 0}, true}, Shot{".eyeR.ppm", HeadPose{0, 0, 0, 3.2f, 0, 0}, true}}) {
                GLuint tex;
                gr.native_world = native;
                if (gr.render_cockpit3d(*frame, 1280, 720, head, &tex)) {
                    auto rgb = gr.read_rgb(tex, 1280, 720);
                    FILE* fo = std::fopen((hires_shot + suffix).c_str(), "wb");
                    std::fprintf(fo, "P6\n1280 720\n255\n");
                    std::fwrite(rgb.data(), 1, rgb.size(), fo);
                    std::fclose(fo);
                }
            }
            gr.depth = false;
            save(hires_shot + ".nodepth.ppm", true);
            gr.depth = true;
            // The engine's own 320x200 output for the same frame, and its world mask.
            FILE* fo = std::fopen((hires_shot + ".page.ppm").c_str(), "wb");
            std::fprintf(fo, "P6\n320 200\n255\n");
            for (int i = 0; i < 64000; i++) {
                const uint8_t* d = frame->dac[frame->page[i]];
                uint8_t rgb[3] = {uint8_t(d[0] * 255 / 63), uint8_t(d[1] * 255 / 63), uint8_t(d[2] * 255 / 63)};
                std::fwrite(rgb, 1, 3, fo);
            }
            std::fclose(fo);
            fo = std::fopen((hires_shot + ".mask.ppm").c_str(), "wb");
            std::fprintf(fo, "P6\n320 200\n255\n");
            for (int i = 0; i < 64000; i++) { uint8_t v = frame->mask[i] ? 255 : 0; uint8_t rgb[3] = {v, v, v}; std::fwrite(rgb, 1, 3, fo); }
            std::fclose(fo);
            SDL_Quit();
        }
    }
    if (drv_trace) {
        std::vector<std::pair<uint64_t, uint32_t>> order;
        for (auto& [lin, e] : entries) order.emplace_back(e.calls, lin);
        std::sort(order.rbegin(), order.rend());
        for (auto [calls, lin] : order) {
            auto& e = entries[lin];
            std::string slots;
            for (int s2 : e.slots) slots += (slots.empty() ? "" : ",") + std::to_string(s2);
            std::fprintf(stderr, "slot %-6s lin %05X calls %8llu callers %zu\n", slots.c_str(), lin, (unsigned long long)calls, e.callers.size());
            for (auto& smp : e.samples) std::fprintf(stderr, "        %s\n", smp.c_str());
            std::vector<std::pair<uint64_t, uint32_t>> cs;
            for (auto [c, n] : e.callers) cs.emplace_back(n, c);
            std::sort(cs.rbegin(), cs.rend());
            for (size_t i = 0; i < cs.size() && i < 8; i++)
                std::fprintf(stderr, "        caller %04X:%04X x%llu\n", cs[i].second >> 16, cs[i].second & 0xFFFF, (unsigned long long)cs[i].first);
        }
    }
    if (prof) {
        std::vector<std::pair<uint64_t, uint32_t>> top;
        for (auto [k2, n] : profile) top.emplace_back(n, k2);
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 12; i++)
            std::fprintf(stderr, "  %04X:%04Xx %6.2f%%\n", top[i].second >> 16, top[i].second & 0xFFFF,
                         100.0 * top[i].first / (m.cpu.instructions / 997.0));
    }
    uint8_t mode = m.mem.read8(0x449);
    std::fprintf(stderr, "video mode %02X\n", mode);
    if (mode <= 3) std::fprintf(stderr, "%s", m.text_screen().c_str());
    return 0;
}
