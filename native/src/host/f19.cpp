// F-19 native host (SDL3): runs the original game in the interpreter and
// presents mode 13h / text mode in a window.
//
// Usage: f19 [GAMEDIR] [--scale N] [--mips N] [--trace]
//   GAMEDIR defaults to the current directory; it must be writable (the
//   game saves its roster there). Use a copy of the original files.
#include <SDL3/SDL.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/dos.h"
#include "core/machine.h"

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

// ------------------------------------------------------------ keyboard

struct KeyCode { uint8_t scan, ascii, shift_ascii; };

// SDL scancode -> PC set-1 scan code and ASCII (unshifted / shifted).
bool map_key(SDL_Scancode sc, KeyCode& k) {
    static const struct { SDL_Scancode sdl; KeyCode k; } table[] = {
        {SDL_SCANCODE_ESCAPE, {0x01, 0x1B, 0x1B}}, {SDL_SCANCODE_1, {0x02, '1', '!'}}, {SDL_SCANCODE_2, {0x03, '2', '@'}},
        {SDL_SCANCODE_3, {0x04, '3', '#'}}, {SDL_SCANCODE_4, {0x05, '4', '$'}}, {SDL_SCANCODE_5, {0x06, '5', '%'}},
        {SDL_SCANCODE_6, {0x07, '6', '^'}}, {SDL_SCANCODE_7, {0x08, '7', '&'}}, {SDL_SCANCODE_8, {0x09, '8', '*'}},
        {SDL_SCANCODE_9, {0x0A, '9', '('}}, {SDL_SCANCODE_0, {0x0B, '0', ')'}}, {SDL_SCANCODE_MINUS, {0x0C, '-', '_'}},
        {SDL_SCANCODE_EQUALS, {0x0D, '=', '+'}}, {SDL_SCANCODE_BACKSPACE, {0x0E, 0x08, 0x08}}, {SDL_SCANCODE_TAB, {0x0F, 0x09, 0x00}},
        {SDL_SCANCODE_Q, {0x10, 'q', 'Q'}}, {SDL_SCANCODE_W, {0x11, 'w', 'W'}}, {SDL_SCANCODE_E, {0x12, 'e', 'E'}},
        {SDL_SCANCODE_R, {0x13, 'r', 'R'}}, {SDL_SCANCODE_T, {0x14, 't', 'T'}}, {SDL_SCANCODE_Y, {0x15, 'y', 'Y'}},
        {SDL_SCANCODE_U, {0x16, 'u', 'U'}}, {SDL_SCANCODE_I, {0x17, 'i', 'I'}}, {SDL_SCANCODE_O, {0x18, 'o', 'O'}},
        {SDL_SCANCODE_P, {0x19, 'p', 'P'}}, {SDL_SCANCODE_LEFTBRACKET, {0x1A, '[', '{'}}, {SDL_SCANCODE_RIGHTBRACKET, {0x1B, ']', '}'}},
        {SDL_SCANCODE_RETURN, {0x1C, 0x0D, 0x0D}}, {SDL_SCANCODE_A, {0x1E, 'a', 'A'}}, {SDL_SCANCODE_S, {0x1F, 's', 'S'}},
        {SDL_SCANCODE_D, {0x20, 'd', 'D'}}, {SDL_SCANCODE_F, {0x21, 'f', 'F'}}, {SDL_SCANCODE_G, {0x22, 'g', 'G'}},
        {SDL_SCANCODE_H, {0x23, 'h', 'H'}}, {SDL_SCANCODE_J, {0x24, 'j', 'J'}}, {SDL_SCANCODE_K, {0x25, 'k', 'K'}},
        {SDL_SCANCODE_L, {0x26, 'l', 'L'}}, {SDL_SCANCODE_SEMICOLON, {0x27, ';', ':'}}, {SDL_SCANCODE_APOSTROPHE, {0x28, '\'', '"'}},
        {SDL_SCANCODE_GRAVE, {0x29, '`', '~'}}, {SDL_SCANCODE_BACKSLASH, {0x2B, '\\', '|'}}, {SDL_SCANCODE_Z, {0x2C, 'z', 'Z'}},
        {SDL_SCANCODE_X, {0x2D, 'x', 'X'}}, {SDL_SCANCODE_C, {0x2E, 'c', 'C'}}, {SDL_SCANCODE_V, {0x2F, 'v', 'V'}},
        {SDL_SCANCODE_B, {0x30, 'b', 'B'}}, {SDL_SCANCODE_N, {0x31, 'n', 'N'}}, {SDL_SCANCODE_M, {0x32, 'm', 'M'}},
        {SDL_SCANCODE_COMMA, {0x33, ',', '<'}}, {SDL_SCANCODE_PERIOD, {0x34, '.', '>'}}, {SDL_SCANCODE_SLASH, {0x35, '/', '?'}},
        {SDL_SCANCODE_KP_MULTIPLY, {0x37, '*', '*'}}, {SDL_SCANCODE_SPACE, {0x39, ' ', ' '}},
        {SDL_SCANCODE_F1, {0x3B, 0, 0}}, {SDL_SCANCODE_F2, {0x3C, 0, 0}}, {SDL_SCANCODE_F3, {0x3D, 0, 0}},
        {SDL_SCANCODE_F4, {0x3E, 0, 0}}, {SDL_SCANCODE_F5, {0x3F, 0, 0}}, {SDL_SCANCODE_F6, {0x40, 0, 0}},
        {SDL_SCANCODE_F7, {0x41, 0, 0}}, {SDL_SCANCODE_F8, {0x42, 0, 0}}, {SDL_SCANCODE_F9, {0x43, 0, 0}},
        {SDL_SCANCODE_F10, {0x44, 0, 0}},
        // Keypad with NumLock on (the demo recording uses keypad digits).
        {SDL_SCANCODE_KP_7, {0x47, '7', '7'}}, {SDL_SCANCODE_KP_8, {0x48, '8', '8'}}, {SDL_SCANCODE_KP_9, {0x49, '9', '9'}},
        {SDL_SCANCODE_KP_MINUS, {0x4A, '-', '-'}}, {SDL_SCANCODE_KP_4, {0x4B, '4', '4'}}, {SDL_SCANCODE_KP_5, {0x4C, '5', '5'}},
        {SDL_SCANCODE_KP_6, {0x4D, '6', '6'}}, {SDL_SCANCODE_KP_PLUS, {0x4E, '+', '+'}}, {SDL_SCANCODE_KP_1, {0x4F, '1', '1'}},
        {SDL_SCANCODE_KP_2, {0x50, '2', '2'}}, {SDL_SCANCODE_KP_3, {0x51, '3', '3'}}, {SDL_SCANCODE_KP_0, {0x52, '0', '0'}},
        {SDL_SCANCODE_KP_PERIOD, {0x53, '.', '.'}}, {SDL_SCANCODE_KP_ENTER, {0x1C, 0x0D, 0x0D}},
        {SDL_SCANCODE_KP_DIVIDE, {0x35, '/', '/'}},
        // Cursor keys and friends (no ASCII).
        {SDL_SCANCODE_HOME, {0x47, 0, 0}}, {SDL_SCANCODE_UP, {0x48, 0, 0}}, {SDL_SCANCODE_PAGEUP, {0x49, 0, 0}},
        {SDL_SCANCODE_LEFT, {0x4B, 0, 0}}, {SDL_SCANCODE_RIGHT, {0x4D, 0, 0}}, {SDL_SCANCODE_END, {0x4F, 0, 0}},
        {SDL_SCANCODE_DOWN, {0x50, 0, 0}}, {SDL_SCANCODE_PAGEDOWN, {0x51, 0, 0}}, {SDL_SCANCODE_INSERT, {0x52, 0, 0}},
        {SDL_SCANCODE_DELETE, {0x53, 0, 0}},
    };
    for (auto& e : table)
        if (e.sdl == sc) { k = e.k; return true; }
    return false;
}

// BIOS extended codes for function keys with modifiers.
uint8_t fkey_scan(uint8_t scan, bool shift, bool ctrl, bool alt) {
    if (scan < 0x3B || scan > 0x44) return scan;
    int n = scan - 0x3B;
    if (alt) return uint8_t(0x68 + n);
    if (ctrl) return uint8_t(0x5E + n);
    if (shift) return uint8_t(0x54 + n);
    return scan;
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir = ".";
    int scale = 4;
    double mips = 4.0;
    bool trace = false;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--scale") && i + 1 < argc) scale = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--mips") && i + 1 < argc) mips = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--trace")) trace = true;
        else dir = argv[i];
    }

    Font font;
    if (!load_psf("/usr/share/kbd/consolefonts/default8x16.psfu.gz", font))
        std::fprintf(stderr, "warning: no console font found; text mode will be blank\n");

    Machine m(dir);
    m.trace = trace;
    m.ips_per_ms = uint32_t(mips * 1000);
    if (!m.dos->start_program("F19.COM", "")) {
        std::fprintf(stderr,
                     "cannot start F19.COM in '%s'\n"
                     "usage: f19 GAMEDIR [--scale N] [--mips N] [--trace]\n"
                     "GAMEDIR is a writable copy of the game files, e.g.:\n"
                     "  mkdir -p out/run/native && cp fullgame/* out/run/native/ && chmod -R u+w out/run/native\n"
                     "  build/native/f19 out/run/native\n",
                     dir.c_str());
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("F-19 Stealth Fighter (native)", 320 * scale, 240 * scale, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = SDL_CreateRenderer(win, nullptr);
    SDL_SetRenderVSync(ren, 1);
    SDL_Texture* gfx = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 200);
    SDL_Texture* txt = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 400);
    SDL_SetTextureScaleMode(gfx, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureScaleMode(txt, SDL_SCALEMODE_NEAREST);
    // 4:3 display, letterboxed in the window.
    SDL_SetRenderLogicalPresentation(ren, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);

    uint64_t last = SDL_GetTicksNS();
    bool running = true;
    while (running && !m.exited) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) {
                // Keep the BIOS shift-state byte in sync.
                SDL_Keymod mod = ev.key.mod;
                uint8_t flags = m.mem.read8(0x417) & 0xD0;
                if (mod & SDL_KMOD_RSHIFT) flags |= 0x01;
                if (mod & SDL_KMOD_LSHIFT) flags |= 0x02;
                if (mod & SDL_KMOD_CTRL) flags |= 0x04;
                if (mod & SDL_KMOD_ALT) flags |= 0x08;
                flags |= 0x20;  // NumLock on
                m.mem.write8(0x417, flags);
            }
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                KeyCode k;
                if (!map_key(ev.key.scancode, k)) continue;
                bool shift = ev.key.mod & SDL_KMOD_SHIFT, ctrl = ev.key.mod & SDL_KMOD_CTRL, alt = ev.key.mod & SDL_KMOD_ALT;
                uint8_t ascii = shift ? k.shift_ascii : k.ascii;
                uint8_t scan = fkey_scan(k.scan, shift, ctrl, alt);
                if (alt) ascii = 0;
                else if (ctrl && std::isalpha(ascii)) ascii = uint8_t(std::tolower(ascii) & 0x1F);
                m.key_press(scan, ascii);
            }
        }

        // Run emulated time matching real time (capped to avoid spirals).
        uint64_t now = SDL_GetTicksNS();
        uint64_t elapsed_ms = std::min<uint64_t>((now - last) / 1000000, 50);
        last += elapsed_ms * 1000000;
        if (now - last > 100000000) last = now;
        m.run(uint64_t(elapsed_ms) * m.ips_per_ms);

        uint8_t mode = m.mem.read8(0x449);
        void* pixels;
        int pitch;
        SDL_RenderClear(ren);
        if (mode == 0x13) {
            SDL_LockTexture(gfx, nullptr, &pixels, &pitch);
            for (int y = 0; y < 200; y++) {
                uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pixels) + y * pitch);
                for (int x = 0; x < 320; x++) {
                    const uint8_t* c = m.dac[m.mem.read8(0xA0000 + y * 320 + x)];
                    row[x] = uint32_t(c[0] * 255 / 63) << 16 | uint32_t(c[1] * 255 / 63) << 8 | uint32_t(c[2] * 255 / 63);
                }
            }
            SDL_UnlockTexture(gfx);
            SDL_RenderTexture(ren, gfx, nullptr, nullptr);
        } else {
            SDL_LockTexture(txt, nullptr, &pixels, &pitch);
            for (int r = 0; r < 25; r++)
                for (int c = 0; c < 80; c++) {
                    uint8_t ch = m.mem.read8(0xB8000 + (r * 80 + c) * 2);
                    uint8_t at = m.mem.read8(0xB8000 + (r * 80 + c) * 2 + 1);
                    uint32_t fg = kTextPalette[at & 15], bg = kTextPalette[(at >> 4) & 7];
                    for (int y = 0; y < 16; y++) {
                        uint8_t bits = font.glyphs.empty() ? 0 : font.glyphs[ch * font.h + std::min(y, font.h - 1)];
                        uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pixels) + (r * 16 + y) * pitch) + c * 8;
                        for (int x = 0; x < 8; x++) row[x] = (bits & (0x80 >> x)) ? fg : bg;
                    }
                }
            SDL_UnlockTexture(txt);
            SDL_RenderTexture(ren, txt, nullptr, nullptr);
        }
        SDL_RenderPresent(ren);
    }
    if (m.exited) std::fprintf(stderr, "stopped: %s\n", m.stop_reason.c_str());
    SDL_Quit();
    return 0;
}
