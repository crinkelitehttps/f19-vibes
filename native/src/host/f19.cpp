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
// Gamepad (e.g. Xbox controller): the left stick and right trigger/bumper
// are the PC joystick (answer Y to "Do you have a joystick?" in setup), the
// right stick is throttle and look left/right, other buttons press keys,
// with the left trigger and bumper as two shift layers. In VR the motion
// controllers do the same (right thumbstick, trigger and grip fly). Bindings: native/gamepad.cfg
// (built in), overridden by ~/.config/f19/gamepad.cfg or --gamepad FILE.
//
// Sound: the PC speaker (ISOUND.EXE, chosen by the game for VGA) is
// emulated and played through the default audio device. --volume F (0-1,
// default 0.5), --no-sound, --raw-speaker (no speaker-cone response, just
// the ideal 1-bit signal band-limited).
//
// Clipboard: the controls (made from the loaded bindings), then the game's
// manual (a PDF: --manual FILE, else the first *.pdf in GAMEDIR or the
// current directory), on a clipboard in the cockpit, or over the screen
// elsewhere. Ctrl+F11 shows / hides it; while it is up, Page Up /
// Page Down (with Shift: 10 pages), Home (the controls) / End and the
// mouse wheel turn pages. Controllers: the `manual` and `manual-page` bindings. The page
// being read is remembered (~/.local/state/f19/manual-page).
//
// Set F19_PERF=1 for a once-a-second timing summary.
//
// Usage: f19 [GAMEDIR] [--scale N] [--mips N] [--msaa N] [--vga-hz HZ] [--headtrack-port N] [--headtrack-bind ADDR]
//            [--trace] [--original-driver] [--verify-driver] [--lowres] [--lod-detail F] [--terrain-radius N]
//            [--classic-lines] [--road-width FEET]
//            [--vr] [--vr-scale F] [--gamepad FILE] [--volume F] [--no-sound] [--raw-speaker] [--manual FILE]
//   --classic-lines: every line at one screen width, as the original
//   (Alt+F11 toggles). Otherwise roads and markings are drawn in perspective
//   (--road-width FEET, default 40), object edges scale with distance, and
//   mountain ridges thin and fade with distance.
//   --lod-detail F: > 1 keeps detailed models farther away (default 1 =
//   switch at the same on-screen size as the original). --terrain-radius N:
//   terrain tiles drawn in every direction per level (default 6).
//   The 3D world is rendered at the window's resolution unless --lowres
//   (needs the native driver).
//   GAMEDIR defaults to the current directory; it must be writable (the
//   game saves its roster there). Use a copy of the original files.
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "core/dos.h"
#include "core/machine.h"
#include "drivers/mgraphic.h"
#include "hires/gl_render.h"
#include "hires/world_capture.h"
#include "host/font.h"
#include "host/gamepad.h"
#include "host/headtrack.h"
#include "host/keyboard.h"
#include "host/manual.h"
#include "host/xr.h"
#include "gamepad_cfg.h"

using namespace f19;

namespace {

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

// Where the manual page being read is kept between runs.
std::string manual_state_path() {
    const char* xdg = std::getenv("XDG_STATE_HOME");
    const char* home = std::getenv("HOME");
    return xdg && *xdg ? std::string(xdg) + "/f19/manual-page" : home ? std::string(home) + "/.local/state/f19/manual-page" : "";
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
    bool classic_lines = false;
    float road_width = 40.0f;
    std::string gamepad_cfg, manual_pdf;
    bool sound = true, raw_speaker = false;
    float volume = 0.5f;
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
        else if (!std::strcmp(argv[i], "--classic-lines")) classic_lines = true;
        else if (!std::strcmp(argv[i], "--road-width") && i + 1 < argc) road_width = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--vr")) vr = true;
        else if (!std::strcmp(argv[i], "--vr-scale") && i + 1 < argc) vr_scale = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--gamepad") && i + 1 < argc) gamepad_cfg = argv[++i];
        else if (!std::strcmp(argv[i], "--volume") && i + 1 < argc) volume = std::clamp(float(std::atof(argv[++i])), 0.0f, 1.0f);
        else if (!std::strcmp(argv[i], "--no-sound")) sound = false;
        else if (!std::strcmp(argv[i], "--raw-speaker")) raw_speaker = true;
        else if (!std::strcmp(argv[i], "--manual") && i + 1 < argc) manual_pdf = argv[++i];
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
    renderer.classic_lines = classic_lines;
    renderer.road_width = road_width;
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
    // The manual: pages render in the background; the one being read is
    // uploaded to the renderer when it is ready.
    Manual manual;
    manual.set_font(font);
    {
        // Controls pages, from the bindings: the device in use first, then
        // the keyboard (host keys, and the game keys the bindings press).
        auto row = [](std::string a, const std::string& b, const std::string& c) {
            a.resize(std::max<size_t>(a.size() + 1, 20), ' ');
            std::string ab = a + b;
            ab.resize(std::max<size_t>(ab.size() + 1, 38), ' ');
            return ab + c;
        };
        Manual::Section sec[2] = {{"VR motion controllers", {}}, {"Gamepad (Xbox layout)", {}}};
        for (auto& sc : sec)
            sc.lines = {"S1 / S2: hold shift 1 / 2   M: while the clipboard is up", ""};
        std::vector<std::string> game_keys;
        for (const Gamepad::Help& h : gamepad.help()) {
            auto& lines = sec[h.vr ? 0 : 1].lines;
            if (h.gap) lines.push_back("");
            lines.push_back(row(h.control, h.action, h.note));
            if (!h.note.empty() && h.note[0] != '(' && h.action.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ=-/.,0123456789") == 0) {
                std::string r = row(h.action, h.note, "");
                if (std::find(game_keys.begin(), game_keys.end(), r) == game_keys.end()) game_keys.push_back(r);
            }
        }
        Manual::Section keys{"Keyboard", {
            row("Ctrl+F11", "show / hide the clipboard", ""),
            row("PgUp / PgDn", "turn page (Shift: 10 pages)", ""),
            row("Home / End", "these controls / last page", ""),
            row("Mouse wheel", "turn page", ""),
            "",
            row("F11", "3D cockpit / original flat screen", ""),
            row("Shift+F11", "native world / engine primitives", ""),
            row("Alt+F11", "perspective / classic lines", ""),
            row("F12", "screenshot", ""),
            row("Shift+F12", "VR: recentre the view", ""),
            row("Right drag", "look around", ""),
            "",
            "Game keys used by the controller bindings:",
        }};
        keys.lines.insert(keys.lines.end(), game_keys.begin(), game_keys.end());
        for (int k = 0; k < 2; k++)
            if (const int i = vr ? k : 1 - k; sec[i].lines.size() > 2) manual.add_section(sec[i]);
        manual.add_section(keys);
    }
    if (std::string pdf = find_manual(manual_pdf, dir); !pdf.empty()) manual.open(pdf);
    bool manual_shown = false, manual_hint = false;
    int manual_page = 0, manual_uploaded = -1;
    uint64_t manual_version = 1, shown_manual_version = 0;  // what the 3D desktop view last drew
    if (manual.ok()) {
        const std::string state = manual_state_path();
        if (FILE* f = state.empty() ? nullptr : std::fopen(state.c_str(), "r")) {
            if (std::fscanf(f, "%d", &manual_page) != 1) manual_page = 0;
            std::fclose(f);
        }
        manual_page = std::clamp(manual_page, 0, manual.pages() - 1);
        manual.request(manual_page);
    }
    auto toggle_manual = [&] {
        if (manual.ok()) {
            manual_shown = !manual_shown;
            manual_version++;
        } else if (!manual_hint) {
            manual_hint = true;
            std::fprintf(stderr, "manual: none loaded (put the PDF in the game directory or pass --manual FILE)\n");
        }
    };
    auto turn_manual = [&](int pages) {
        if (!manual.ok() || !pages) return;
        manual_page = std::clamp(manual_page + pages, 0, manual.pages() - 1);
        manual.request(manual_page);
    };
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

    // PC speaker output. Samples are made in emulated time on the emulation
    // thread; the queue is held near kAudioTargetMs by nudging the playback
    // rate (the emulation follows the host clock, the device its own).
    SDL_AudioStream* audio = nullptr;
    if (sound) {
        SDL_AudioSpec spec{SDL_AUDIO_F32, 1, PcSpeaker::kRate};
        if (SDL_InitSubSystem(SDL_INIT_AUDIO))
            audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (audio) {
            SDL_ResumeAudioStreamDevice(audio);
            m.speaker.enabled = true;
            m.speaker.volume = volume;
            m.speaker.speaker_filter = !raw_speaker;
        } else {
            std::fprintf(stderr, "warning: no audio output: %s\n", SDL_GetError());
        }
    }
    constexpr int kAudioTargetMs = 50;

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
        std::vector<float> pcm;
        const int audio_target = PcSpeaker::kRate * kAudioTargetMs / 1000;
        float ratio = 1;
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
                if (audio) {
                    m.speaker.advance(m.pit_ticks());
                    pcm.swap(m.speaker.samples);
                    m.speaker.samples.clear();
                }
            }
            if (audio && !pcm.empty()) {
                int queued = SDL_GetAudioStreamQueued(audio) / int(sizeof(float));
                if (queued == 0 || queued > 4 * audio_target) {   // start, underrun or far behind: restart at the target
                    SDL_ClearAudioStream(audio);
                    std::vector<float> silence(audio_target);
                    SDL_PutAudioStreamData(audio, silence.data(), int(silence.size() * sizeof(float)));
                    queued = audio_target;
                }
                float want = 1 + std::clamp(float(queued - audio_target) / audio_target * 0.005f, -0.005f, 0.005f);
                if (std::abs(want - ratio) > 0.0005f) SDL_SetAudioStreamFrequencyRatio(audio, ratio = want);
                SDL_PutAudioStreamData(audio, pcm.data(), int(pcm.size() * sizeof(float)));
                pcm.clear();
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
                // F11: 3D / flat; Shift+F11: native world / engine's
                // primitives; Ctrl+F11: the manual; Alt+F11: classic lines.
                if (ev.key.mod & SDL_KMOD_CTRL) {
                    toggle_manual();
                } else if (ev.key.mod & SDL_KMOD_ALT) {
                    renderer.classic_lines = !renderer.classic_lines;
                    std::fprintf(stderr, "lines: %s\n", renderer.classic_lines ? "classic" : "perspective");
                } else if (ev.key.mod & SDL_KMOD_SHIFT) {
                    renderer.native_world = !renderer.native_world;
                    std::fprintf(stderr, "world: %s\n", renderer.native_world ? "native scene" : "engine primitives");
                } else {
                    cockpit3d = !cockpit3d;
                }
                continue;
            }
            if (ev.type == SDL_EVENT_KEY_UP && ev.key.scancode == SDL_SCANCODE_F11) continue;
            // While the manual is up, its page keys and the wheel turn pages
            // (the game doesn't see them).
            if (manual_shown && (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP)) {
                const SDL_Scancode sc = ev.key.scancode;
                if (sc == SDL_SCANCODE_PAGEUP || sc == SDL_SCANCODE_PAGEDOWN || sc == SDL_SCANCODE_HOME || sc == SDL_SCANCODE_END) {
                    if (ev.type == SDL_EVENT_KEY_DOWN) {
                        const int n = (ev.key.mod & SDL_KMOD_SHIFT) ? 10 : 1;
                        turn_manual(sc == SDL_SCANCODE_PAGEUP ? -n : sc == SDL_SCANCODE_PAGEDOWN ? n
                                    : sc == SDL_SCANCODE_HOME ? -manual.pages() : manual.pages());
                    }
                    continue;
                }
            }
            if (manual_shown && ev.type == SDL_EVENT_MOUSE_WHEEL && ev.wheel.y != 0) turn_manual(ev.wheel.y > 0 ? -1 : 1);
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
        const Gamepad::State pad = gamepad.update(SDL_GetTicksNS(), vr_pads_on ? &vr_pads : nullptr, manual_shown);
        if (pad.recenter && xr_on) xr.recenter();
        if (pad.manual) toggle_manual();
        turn_manual(pad.manual_page);
        if (manual_page != manual_uploaded)
            if (auto img = manual.get(manual_page)) {
                renderer.set_manual(img->argb.data(), Manual::kWidth, Manual::kHeight);
                manual_uploaded = manual_page;
                manual_version++;
            }
        renderer.show_manual = manual_shown;
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
            if (frame != shown_frame || !hires_tex || !shown_3d || hires_w != ow || hires_h != oh || head != shown_head ||
                manual_version != shown_manual_version) {
                GLuint t3;
                if (renderer.render_cockpit3d(*frame, ow, oh, head, &t3)) {
                    hires_tex = t3;
                    shown_frame = frame;
                    shown_3d = true;
                    hires_w = ow;
                    hires_h = oh;
                    shown_head = head;
                    shown_manual_version = manual_version;
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
                renderer.draw_manual_2d(fb, XrOutput::kScreenW, XrOutput::kScreenH);
                xr.release_screen();
            }
            if (full_window) {
                renderer.present(tex, ow, oh, 0, 0, float(ow), float(oh));  // the 3D view has the clipboard
            } else {
                renderer.present(tex, ow, oh, vx, vy, float(vw), float(vh));
                renderer.draw_manual_2d(0, ow, oh);
            }
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
    if (manual.ok()) {
        const std::string state = manual_state_path();
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(state).parent_path(), ec);
        if (FILE* f = state.empty() ? nullptr : std::fopen(state.c_str(), "w")) {
            std::fprintf(f, "%d\n", manual_page);
            std::fclose(f);
        }
    }
    if (audio) SDL_DestroyAudioStream(audio);
    if (m.exited) std::fprintf(stderr, "stopped: %s\n", m.stop_reason.c_str());
    if (verify_driver) std::fprintf(stderr, "%s", native_gfx.report().c_str());
    SDL_Quit();
    return 0;
}
