// F-19 native host (SDL3): runs the original game in the interpreter and
// presents mode 13h / text mode in a window.
//
// In the cockpit view the world is rendered in 3D with the instrument panel
// as a surface in the cockpit (F11 toggles; --flat-cockpit starts with the
// original 2D layout). Hold the right mouse button and drag to look around.
// The 3D world is drawn from the game's own shape and terrain data in every
// direction (Shift+F11 switches to the engine's captured primitives, which
// only cover its forward view).
// Head tracking: OpenTrack's "UDP over network" output, received on
// 127.0.0.1:4242 by default (--headtrack-port 0 disables, --headtrack-bind
// ADDR to accept it from another machine); mouse look adds to it.
// F12 saves a screenshot (BMP) in the current directory, plus debug data
// for the high-resolution renderer (captured primitives, the engine's own
// 320x200 frame).
//
// VR (--vr): OpenXR output, e.g. Monado with a WMR headset. The 3D view is
// rendered per eye with the headset's pose (head tracking and mouse look
// are ignored); menus and flat views appear on a virtual screen ahead of
// the seat. Shift+F12 recentres. --vr-scale F scales the per-eye
// resolution the runtime recommends (default 0.7: Monado recommends 1.4x
// supersampling, too much for an integrated GPU); MSAA defaults to 4x in VR.
//
// Gamepad (e.g. Xbox controller): the left stick and A/B are the PC
// joystick (answer Y to "Do you have a joystick?" in setup), the right
// stick looks around, other buttons press keys. In VR the motion
// controllers can do the same (right thumbstick, trigger and grip fly). Bindings: native/gamepad.cfg
// (built in), overridden by ~/.config/f19/gamepad.cfg or --gamepad FILE.
//
// Set F19_PERF=1 for a once-a-second timing summary.
//
// Usage: f19 [GAMEDIR] [--scale N] [--mips N] [--msaa N] [--vga-hz HZ] [--headtrack-port N] [--headtrack-bind ADDR]
//            [--trace] [--original-driver] [--verify-driver] [--lowres] [--lod-detail F] [--terrain-radius N]
//            [--vr] [--vr-scale F] [--gamepad FILE]
//   --lod-detail F: > 1 keeps detailed models farther away (default 1 =
//   switch at the same on-screen size as the original). --terrain-radius N:
//   terrain tiles drawn in every direction per level (default 6).
//   The 3D world is rendered at the window's resolution unless --lowres
//   (needs the native driver).
//   GAMEDIR defaults to the current directory; it must be writable (the
//   game saves its roster there). Use a copy of the original files.
#include <SDL3/SDL.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/dos.h"
#include "core/machine.h"
#include "drivers/mgraphic.h"
#include "hires/gl_render.h"
#include "hires/world_capture.h"
#include "host/gamepad.h"
#include "host/headtrack.h"
#include "host/keyboard.h"
#include "host/xr.h"
#include "gamepad_cfg.h"

using namespace f19;

namespace {

// ------------------------------------------------------------ text font

struct Font {
    int w = 8, h = 16;
    std::vector<uint8_t> glyphs;  // 256 * h bytes
};

// Load a PSF1/PSF2 console font (gzip or plain) from the system.
bool load_psf(const char* path, Font& font) {
    gzFile gz = gzopen(path, "rb");
    if (!gz) return false;
    std::vector<uint8_t> d;
    uint8_t buf[4096];
    int n;
    while ((n = gzread(gz, buf, sizeof buf)) > 0) d.insert(d.end(), buf, buf + n);
    gzclose(gz);
    if (d.size() > 4 && d[0] == 0x36 && d[1] == 0x04) {
        font.h = d[3];
        font.glyphs.assign(d.begin() + 4, d.begin() + 4 + 256 * font.h);
        return true;
    }
    if (d.size() > 32 && d[0] == 0x72 && d[1] == 0xB5 && d[2] == 0x4A && d[3] == 0x86) {
        auto u32 = [&](int o) { return uint32_t(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | d[o + 3] << 24); };
        uint32_t hdr = u32(8), bytes = u32(20);
        font.h = int(u32(24));
        font.w = int(u32(28));
        if (font.w != 8) return false;
        font.glyphs.resize(256 * font.h);
        for (int g = 0; g < 256; g++)
            std::memcpy(&font.glyphs[g * font.h], &d[hdr + g * bytes], font.h);
        return true;
    }
    return false;
}

// CGA/EGA text attribute colours.
const uint32_t kTextPalette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

void save_bmp(const std::string& path, const uint8_t* rgb, int w, int h) {
    SDL_Surface* s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGB24, const_cast<uint8_t*>(rgb), w * 3);
    if (s) {
        SDL_SaveBMP(s, path.c_str());
        SDL_DestroySurface(s);
    }
}

void save_screenshot(GlRenderer& r, int w, int h, const HiresFrame* frame) {
    char stamp[64];
    std::time_t t = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "f19-%Y%m%d-%H%M%S", std::localtime(&t));
    std::string base = stamp;
    auto px = r.read_window(w, h);
    save_bmp(base + ".bmp", px.data(), w, h);
    if (frame) {
        // The engine's own 320x200 frame and the captured primitives.
        std::vector<uint8_t> page(64000 * 3);
        for (int i = 0; i < 64000; i++)
            for (int k = 0; k < 3; k++) page[i * 3 + k] = uint8_t(frame->dac[frame->page[i]][k] * 255 / 63);
        save_bmp(base + "-engine.bmp", page.data(), 320, 200);
        if (FILE* f = std::fopen((base + "-prims.txt").c_str(), "w")) {
            for (auto& p : frame->prims) {
                std::fprintf(f, "%d obj %u col %3d c2 %3d cx %.1f cy %.1f zdiv %.3f vp %.0fx%.0f org %.0f,%.0f", p.kind, p.object,
                             p.color, p.color2, p.proj.cx, p.proj.cy, p.proj.zdiv, p.proj.vp_w, p.proj.vp_h, p.proj.ox, p.proj.oy);
                if (p.kind == HiresPrim::Horizon)
                    std::fprintf(f, " M %.2f,%.2f U %.4f,%.4f uniform %d skyonly %d", p.hx, p.hy, p.ux, p.uy, p.uniform, p.sky_only);
                for (auto& v : p.v) std::fprintf(f, " (%.0f %.0f %.0f)", v[0], v[1], v[2]);
                std::fprintf(f, "\n");
            }
            std::fclose(f);
        }
    }
    std::fprintf(stderr, "screenshot: %s.bmp\n", base.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir = ".";
    int scale = 4;
    double mips = 25.0;   // emulated CPU speed; the game renders as fast as it allows
    int msaa = 0;         // 0 = default (8, or 4 in VR)
    bool vr = false;
    float vr_scale = 0.7f;  // of the runtime's recommended eye size (supersampled); 0.7 ~ the AH101's panels
    double vga_hz = 0;    // 0 = match the display
    bool flat_cockpit = false;
    int headtrack_port = 4242;
    std::string headtrack_bind = "127.0.0.1";
    bool trace = false, original_driver = false, verify_driver = false, lowres = false, no_depth = false;
    float lod_detail = 1.0f;
    int terrain_radius = 6;
    std::string gamepad_cfg;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--scale") && i + 1 < argc) scale = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--mips") && i + 1 < argc) mips = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--trace")) trace = true;
        else if (!std::strcmp(argv[i], "--original-driver")) original_driver = true;
        else if (!std::strcmp(argv[i], "--verify-driver")) verify_driver = true;
        else if (!std::strcmp(argv[i], "--lowres")) lowres = true;
        else if (!std::strcmp(argv[i], "--msaa") && i + 1 < argc) msaa = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--no-depth")) no_depth = true;
        else if (!std::strcmp(argv[i], "--vga-hz") && i + 1 < argc) vga_hz = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--flat-cockpit")) flat_cockpit = true;
        else if (!std::strcmp(argv[i], "--headtrack-port") && i + 1 < argc) headtrack_port = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--headtrack-bind") && i + 1 < argc) headtrack_bind = argv[++i];
        else if (!std::strcmp(argv[i], "--lod-detail") && i + 1 < argc) lod_detail = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--terrain-radius") && i + 1 < argc) terrain_radius = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--vr")) vr = true;
        else if (!std::strcmp(argv[i], "--vr-scale") && i + 1 < argc) vr_scale = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--gamepad") && i + 1 < argc) gamepad_cfg = argv[++i];
        else dir = argv[i];
    }

    Font font;
    if (!load_psf("/usr/share/kbd/consolefonts/default8x16.psfu.gz", font))
        std::fprintf(stderr, "warning: no console font found; text mode will be blank\n");

    Machine m(dir);
    m.trace = trace;
    m.ips_per_ms = uint32_t(mips * 1000);
    // Native replacement for MGRAPHIC.EXE (verify mode checks every call
    // against the original and reports at exit).
    MGraphicNative native_gfx(m);
    native_gfx.enabled = !original_driver;
    native_gfx.verify = verify_driver;
    // High-resolution world rendering: capture the engine's 3D geometry.
    WorldCapture capture(m, native_gfx);
    capture.enabled = !lowres && !original_driver;
    for (int l = 1; l <= 4; l++) capture.terrain_radius[l] = terrain_radius;
    if (!m.dos->start_program("F19.COM", "")) {
        std::fprintf(stderr,
                     "cannot start F19.COM in '%s'\n"
                     "usage: f19 GAMEDIR [--scale N] [--mips N] [--trace] [--original-driver] [--verify-driver]\n"
                     "GAMEDIR is a writable copy of the game files, e.g.:\n"
                     "  mkdir -p out/run/native && cp fullgame/* out/run/native/ && chmod -R u+w out/run/native\n"
                     "  build/native/f19 out/run/native\n",
                     dir.c_str());
        return 1;
    }

    // Under WSL, Mesa defaults to software rendering; use the GPU through
    // the D3D12 bridge when it is available (overridable).
    if (!std::getenv("GALLIUM_DRIVER") && SDL_GetPathInfo("/usr/lib/wsl/lib/libd3d12.so", nullptr))
        setenv("GALLIUM_DRIVER", "d3d12", 0);
    // Gamepad input keeps working while the window is unfocused (in VR).
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window* win = SDL_CreateWindow("F-19 Stealth Fighter (native)", 320 * scale, 240 * scale,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_GLContext glc = win ? SDL_GL_CreateContext(win) : nullptr;
    if (!glc || !gl::load()) {
        std::fprintf(stderr, "OpenGL 3.3 unavailable: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(1);
    std::fprintf(stderr, "OpenGL: %s\n", reinterpret_cast<const char*>(gl::GetString(GL_RENDERER)));
    GlRenderer renderer;
    renderer.msaa = msaa > 0 ? msaa : vr ? 4 : 8;
    renderer.lod_detail = lod_detail;
    renderer.depth = !no_depth;
    if (!renderer.init()) std::fprintf(stderr, "warning: renderer initialisation reported a GL error\n");
    XrOutput xr;
    bool xr_on = vr && xr.init(vr_scale), xr_vsync_off = false;
    if (vr && !xr_on) std::fprintf(stderr, "warning: VR unavailable; running on the desktop only\n");
    std::shared_ptr<const HiresFrame> shown_frame;
    GLuint hires_tex = 0;
    int hires_w = 0, hires_h = 0;
    std::vector<uint32_t> screen(640 * 400);

    bool want_screenshot = false;
    bool cockpit3d = !flat_cockpit, looking = false;
    float head_yaw = 0, head_pitch = 0;  // mouse look
    Gamepad gamepad;
    {
        std::string path = gamepad_cfg, text = kDefaultGamepadCfg, source = "built-in gamepad.cfg";
        if (path.empty()) {
            const char* xdg = std::getenv("XDG_CONFIG_HOME");
            const char* home = std::getenv("HOME");
            std::string user = xdg && *xdg ? std::string(xdg) + "/f19/gamepad.cfg"
                             : home ? std::string(home) + "/.config/f19/gamepad.cfg" : "";
            if (!user.empty() && SDL_GetPathInfo(user.c_str(), nullptr)) path = user;
        }
        if (!path.empty()) {
            size_t size = 0;
            void* data = SDL_LoadFile(path.c_str(), &size);
            if (!data) {
                std::fprintf(stderr, "cannot read %s: %s\n", path.c_str(), SDL_GetError());
                return 1;
            }
            text.assign(static_cast<const char*>(data), size);
            SDL_free(data);
            source = path;
            std::fprintf(stderr, "gamepad bindings: %s\n", path.c_str());
        }
        gamepad.load(text, source);
    }
    HeadPose shown_head;
    HeadTracker tracker;
    if (headtrack_port > 0) {
        if (tracker.open(headtrack_bind.c_str(), headtrack_port))
            std::fprintf(stderr, "head tracking: listening for OpenTrack UDP on %s:%d\n", headtrack_bind.c_str(), headtrack_port);
        else
            std::fprintf(stderr, "warning: head tracking: cannot bind %s:%d\n", headtrack_bind.c_str(), headtrack_port);
    }
    bool shown_3d = false;

    // Match the emulated VGA refresh to the display, so each game frame
    // (the game syncs to vertical retrace) lines up with one display frame.
    if (vga_hz <= 0) {
        const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
        vga_hz = dm && dm->refresh_rate > 0 ? dm->refresh_rate : 60.0;
    }
    m.vga_refresh_hz = vga_hz;
    std::fprintf(stderr, "emulated CPU %.0f MIPS, VGA refresh %.2f Hz\n", mips, vga_hz);

    // Emulation runs on its own thread against a real-time clock, in slices
    // of about 1 ms of emulated time; the main thread handles input and
    // renders snapshots. `mtx` guards the machine.
    std::mutex mtx;
    gamepad.send_key = [&](SDL_Scancode sc, SDL_Keymod mod, bool down) {
        std::lock_guard<std::mutex> lk(mtx);
        send_key(m, sc, mod, down);
    };
    std::atomic<bool> quit{false};
    std::atomic<uint64_t> perf_drops{0}, perf_busy_ns{0};
    bool perf = std::getenv("F19_PERF") != nullptr;
    std::thread emu([&] {
        using clk = std::chrono::steady_clock;
        const uint64_t slice = m.ips_per_ms;               // 1 ms emulated
        const uint64_t max_lag = uint64_t(m.ips_per_ms) * 250;
        auto base_t = clk::now();
        uint64_t base_i = m.cpu.instructions;
        while (!quit) {
            auto now = clk::now();
            double ms = std::chrono::duration<double, std::milli>(now - base_t).count();
            uint64_t target = base_i + uint64_t(ms * m.ips_per_ms);
            bool behind = false;
            {
                std::lock_guard<std::mutex> lk(mtx);
                if (m.exited) break;
                if (target > m.cpu.instructions + max_lag) {  // fell far behind: drop time
                    base_t = now;
                    base_i = m.cpu.instructions;
                    target = base_i;
                    perf_drops++;
                }
                if (m.cpu.instructions < target) {
                    auto t0 = clk::now();
                    m.run(std::min(slice, target - m.cpu.instructions));
                    perf_busy_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count());
                    behind = m.cpu.instructions < target;
                }
            }
            if (!behind) std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });

    struct Snapshot {
        uint8_t mode = 3;
        std::vector<uint8_t> vram = std::vector<uint8_t>(64000), text = std::vector<uint8_t>(4000);
        uint8_t dac[256][3] = {};
        std::shared_ptr<const HiresFrame> frame;
        uint64_t now_us = 0;
    } snap;
    uint64_t perf_wait_ns = 0, perf_render_ns = 0, perf_submit_ns = 0;
    uint64_t perf_t0 = SDL_GetTicksNS(), perf_frames = 0, perf_emu_us0 = 0, next_frame_ns = 0;
    size_t perf_flips0 = 0;
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_F12 && !ev.key.repeat) {
                // F12: screenshot; Shift+F12: recentre VR.
                if ((ev.key.mod & SDL_KMOD_SHIFT) && xr_on) xr.recenter();
                else want_screenshot = true;
                continue;
            }
            if (ev.type == SDL_EVENT_KEY_UP && ev.key.scancode == SDL_SCANCODE_F12) continue;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_F11 && !ev.key.repeat) {
                // F11: 3D / flat; Shift+F11: native world / engine's primitives.
                if (ev.key.mod & SDL_KMOD_SHIFT) {
                    renderer.native_world = !renderer.native_world;
                    std::fprintf(stderr, "world: %s\n", renderer.native_world ? "native scene" : "engine primitives");
                } else {
                    cockpit3d = !cockpit3d;
                }
                continue;
            }
            if (ev.type == SDL_EVENT_KEY_UP && ev.key.scancode == SDL_SCANCODE_F11) continue;
            // Freelook (adds to head tracking): hold the right mouse
            // button and drag (dragging up looks down, like a stick);
            // releasing recentres.
            // (Plain motion deltas, not relative mode: pointer warping in
            // relative mode misbehaves over VNC/remote sessions.)
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_RIGHT) looking = true;
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_RIGHT) {
                looking = false;
                head_yaw = head_pitch = 0;
            }
            if (ev.type == SDL_EVENT_MOUSE_MOTION && looking) {
                head_yaw = std::clamp(head_yaw + std::clamp(ev.motion.xrel, -50.0f, 50.0f) * 0.004f, -2.6f, 2.6f);
                head_pitch = std::clamp(head_pitch + std::clamp(ev.motion.yrel, -50.0f, 50.0f) * 0.004f, -1.2f, 1.3f);
            }
            gamepad.handle_event(ev);
            if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) {
                std::lock_guard<std::mutex> lk(mtx);
                send_key(m, ev.key.scancode, ev.key.mod, ev.type == SDL_EVENT_KEY_DOWN);
            }
        }

        // VR: the runtime paces the loop (xrWaitFrame) instead of vsync.
        if (xr_on && !xr.poll()) {
            xr_on = false;
            std::fprintf(stderr, "openxr: session ended; desktop only\n");
        }
        const bool xr_frame = xr_on && xr.running();
        if (xr_frame != xr_vsync_off) {
            SDL_GL_SetSwapInterval(xr_frame ? 0 : 1);
            xr_vsync_off = xr_frame;
        }
        const uint64_t t_wait0 = SDL_GetTicksNS();
        const bool xr_render = xr_frame && xr.begin_frame();
        const uint64_t t_wait1 = SDL_GetTicksNS();
        perf_wait_ns += t_wait1 - t_wait0;

        // Snapshot what the renderer needs.
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (m.exited) running = false;
            snap.mode = m.mem.read8(0x449);
            snap.frame = capture.latest();
            snap.now_us = m.now_us();
            bool use_hires = snap.mode == 0x13 && snap.frame && snap.now_us - snap.frame->time_us < 300000;
            if (!use_hires) {
                if (snap.mode == 0x13) std::memcpy(snap.vram.data(), m.mem.data() + 0xA0000, 64000);
                else std::memcpy(snap.text.data(), m.mem.data() + 0xB8000, 4000);
                std::memcpy(snap.dac, m.dac, sizeof snap.dac);
            }
        }

        // Gamepad: joystick and keys into the machine.
        MotionControllers vr_pads;
        const bool vr_pads_on = xr_on && xr.controllers(vr_pads);
        const Gamepad::State pad = gamepad.update(SDL_GetTicksNS(), vr_pads_on ? &vr_pads : nullptr);
        if (pad.recenter && xr_on) xr.recenter();
        {
            std::lock_guard<std::mutex> lk(mtx);
            m.joystick.connected = pad.connected;
            m.joystick.x = pad.stick_x;
            m.joystick.y = pad.stick_y;
            m.joystick.button[0] = pad.button[0];
            m.joystick.button[1] = pad.button[1];
        }

        // Head pose: tracker plus mouse look and the gamepad's look stick.
        HeadPose head = tracker.poll();
        head.yaw = std::clamp(head.yaw + head_yaw + pad.look_x * 2.5f, -2.6f, 2.6f);
        head.pitch = std::clamp(head.pitch + head_pitch + pad.look_y * 1.2f, -1.2f, 1.3f);

        // 4:3 letterbox in the window's pixels.
        int ow, oh;
        SDL_GetWindowSizeInPixels(win, &ow, &oh);
        int vw = ow, vh = ow * 3 / 4;
        if (vh > oh) { vh = oh; vw = oh * 4 / 3; }
        float vx = float((ow - vw) / 2), vy = float((oh - vh) / 2);
        auto& frame = snap.frame;
        bool use_hires = snap.mode == 0x13 && frame && snap.now_us - frame->time_us < 300000;
        GLuint tex;
        bool full_window = false;
        // VR: the 3D view in stereo, mirrored in the window (right eye).
        const bool vr_stereo = xr_render && use_hires && cockpit3d && xr.tracking();
        if (vr_stereo) {
            const int ew = xr.eye_width(), eh = xr.eye_height();
            GLuint t = 0;
            for (int i = 0; i < 2; i++) {
                GLuint fb = xr.acquire_eye(i);
                t = renderer.render_view(*frame, ew, eh, xr.eye(i), fb);
                xr.release_eye(i);
            }
            float s = std::min(float(ow) / ew, float(oh) / eh);
            renderer.present(t, ow, oh, (ow - ew * s) / 2, (oh - eh * s) / 2, ew * s, eh * s);
            hires_tex = 0;  // the desktop paths must render afresh
        } else if (use_hires && cockpit3d && !xr_frame) {
            // 3D view (cockpit and external views): fills the whole window.
            if (frame != shown_frame || !hires_tex || !shown_3d || hires_w != ow || hires_h != oh || head != shown_head) {
                GLuint t3;
                if (renderer.render_cockpit3d(*frame, ow, oh, head, &t3)) {
                    hires_tex = t3;
                    shown_frame = frame;
                    shown_3d = true;
                    hires_w = ow;
                    hires_h = oh;
                    shown_head = head;
                } else {
                    shown_3d = false;
                }
            }
            full_window = shown_3d;
        }
        if (vr_stereo) {
            tex = 0;
        } else if (full_window) {
            tex = hires_tex;
        } else if (use_hires) {
            if (frame != shown_frame || !hires_tex || shown_3d || hires_w != vw || hires_h != vh) {
                shown_3d = false;
                hires_tex = renderer.render_frame(*frame, vw, vh);
                shown_frame = frame;
                hires_w = vw;
                hires_h = vh;
            }
            tex = hires_tex;
        } else if (snap.mode == 0x13) {
            for (int i = 0; i < 64000; i++) {
                const uint8_t* c = snap.dac[snap.vram[i]];
                screen[i] = 0xFF000000u | uint32_t(c[0] * 255 / 63) << 16 | uint32_t(c[1] * 255 / 63) << 8 | uint32_t(c[2] * 255 / 63);
            }
            tex = renderer.upload(screen.data(), 320, 200);
            shown_frame = nullptr;
        } else {
            for (int r = 0; r < 25; r++)
                for (int c = 0; c < 80; c++) {
                    uint8_t ch = snap.text[(r * 80 + c) * 2];
                    uint8_t at = snap.text[(r * 80 + c) * 2 + 1];
                    uint32_t fg = 0xFF000000u | kTextPalette[at & 15], bg = 0xFF000000u | kTextPalette[(at >> 4) & 7];
                    for (int y = 0; y < 16; y++) {
                        uint8_t bits = font.glyphs.empty() ? 0 : font.glyphs[ch * font.h + std::min(y, font.h - 1)];
                        uint32_t* row = &screen[(r * 16 + y) * 640 + c * 8];
                        for (int x = 0; x < 8; x++) row[x] = (bits & (0x80 >> x)) ? fg : bg;
                    }
                }
            tex = renderer.upload(screen.data(), 640, 400);
            shown_frame = nullptr;
        }
        if (vr_stereo) {
            // drawn above
        } else {
            if (xr_render) {
                // Virtual screen in VR.
                GLuint fb = xr.acquire_screen();
                renderer.present_to(fb, tex, XrOutput::kScreenW, XrOutput::kScreenH, 0, 0, XrOutput::kScreenW, XrOutput::kScreenH);
                xr.release_screen();
            }
            if (full_window) renderer.present(tex, ow, oh, 0, 0, float(ow), float(oh));
            else renderer.present(tex, ow, oh, vx, vy, float(vw), float(vh));
        }
        const uint64_t t_submit0 = SDL_GetTicksNS();
        perf_render_ns += t_submit0 - t_wait1;
        if (xr_frame) xr.end_frame();
        if (want_screenshot) {
            want_screenshot = false;
            save_screenshot(renderer, ow, oh, use_hires ? frame.get() : nullptr);
        }
        SDL_GL_SwapWindow(win);
        perf_submit_ns += SDL_GetTicksNS() - t_submit0;

        // Pace to the display refresh in case the driver ignores vsync
        // (seen under WSLg): sleep until the next frame slot.
        if (!xr_frame) {
            uint64_t period = uint64_t(1e9 / vga_hz);
            uint64_t t = SDL_GetTicksNS();
            if (next_frame_ns == 0 || t > next_frame_ns + period) next_frame_ns = t;
            next_frame_ns += period;
            if (next_frame_ns > t) SDL_DelayPrecise(next_frame_ns - t);
        }

        perf_frames++;
        uint64_t pnow = SDL_GetTicksNS();
        if (perf && pnow - perf_t0 >= 1000000000) {
            size_t flips;
            {
                std::lock_guard<std::mutex> lk(mtx);
                flips = native_gfx.flip_times_us.size();
            }
            double secs = (pnow - perf_t0) / 1e9;
            std::fprintf(stderr, "perf: display %.0f fps, game %.0f fps, emulated %.0f ms per s, emulation busy %.0f%%, dropped %llu\n",
                         perf_frames / secs, (flips - perf_flips0) / secs, (snap.now_us - perf_emu_us0) / 1000.0 / secs,
                         perf_busy_ns.exchange(0) / 1e7 / secs, (unsigned long long)perf_drops.exchange(0));
            if (xr_frame)
                std::fprintf(stderr, "perf: vr per frame: wait %.1f ms, render %.1f ms, submit+swap %.1f ms\n",
                             perf_wait_ns / 1e6 / perf_frames, perf_render_ns / 1e6 / perf_frames, perf_submit_ns / 1e6 / perf_frames);
            perf_wait_ns = perf_render_ns = perf_submit_ns = 0;
            perf_t0 = pnow;
            perf_frames = 0;
            perf_flips0 = flips;
            perf_emu_us0 = snap.now_us;
        }
    }
    quit = true;
    emu.join();
    if (m.exited) std::fprintf(stderr, "stopped: %s\n", m.stop_reason.c_str());
    if (verify_driver) std::fprintf(stderr, "%s", native_gfx.report().c_str());
    SDL_Quit();
    return 0;
}
