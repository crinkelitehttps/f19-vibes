#include "host/keyboard.h"

#include <cctype>

#include "core/machine.h"

namespace f19 {

namespace {

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
        // Keypad with NumLock off: cursor codes, as F-19 expects for flying.
        {SDL_SCANCODE_KP_7, {0x47, 0, 0}}, {SDL_SCANCODE_KP_8, {0x48, 0, 0}}, {SDL_SCANCODE_KP_9, {0x49, 0, 0}},
        {SDL_SCANCODE_KP_MINUS, {0x4A, '-', '-'}}, {SDL_SCANCODE_KP_4, {0x4B, 0, 0}}, {SDL_SCANCODE_KP_5, {0x4C, 0, 0}},
        {SDL_SCANCODE_KP_6, {0x4D, 0, 0}}, {SDL_SCANCODE_KP_PLUS, {0x4E, '+', '+'}}, {SDL_SCANCODE_KP_1, {0x4F, 0, 0}},
        {SDL_SCANCODE_KP_2, {0x50, 0, 0}}, {SDL_SCANCODE_KP_3, {0x51, 0, 0}}, {SDL_SCANCODE_KP_0, {0x52, 0, 0}},
        {SDL_SCANCODE_KP_PERIOD, {0x53, 0, 0}}, {SDL_SCANCODE_KP_ENTER, {0x1C, 0x0D, 0x0D}},
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

// Keys a 101-key keyboard sends with an E0 prefix.
bool is_extended(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_UP: case SDL_SCANCODE_DOWN: case SDL_SCANCODE_LEFT: case SDL_SCANCODE_RIGHT:
        case SDL_SCANCODE_HOME: case SDL_SCANCODE_END: case SDL_SCANCODE_PAGEUP: case SDL_SCANCODE_PAGEDOWN:
        case SDL_SCANCODE_INSERT: case SDL_SCANCODE_DELETE: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_KP_DIVIDE:
        case SDL_SCANCODE_RCTRL: case SDL_SCANCODE_RALT:
            return true;
        default:
            return false;
    }
}

uint8_t modifier_scan(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_LSHIFT: return 0x2A;
        case SDL_SCANCODE_RSHIFT: return 0x36;
        case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x1D;
        case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x38;
        default: return 0;
    }
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

bool key_supported(SDL_Scancode sc) {
    KeyCode k;
    return modifier_scan(sc) || map_key(sc, k);
}

void send_key(Machine& m, SDL_Scancode sc, SDL_Keymod mod, bool down) {
    uint8_t code = modifier_scan(sc);
    uint16_t bios = 0;
    KeyCode k;
    if (!code) {
        if (!map_key(sc, k)) return;
        code = k.scan;
        bool shift = mod & SDL_KMOD_SHIFT, ctrl = mod & SDL_KMOD_CTRL, alt = mod & SDL_KMOD_ALT;
        uint8_t ascii = shift ? k.shift_ascii : k.ascii;
        uint8_t scan = fkey_scan(k.scan, shift, ctrl, alt);
        if (alt) ascii = 0;
        else if (ctrl && std::isalpha(ascii)) ascii = uint8_t(std::tolower(ascii) & 0x1F);
        bios = uint16_t(ascii | (scan << 8));
    }
    if (is_extended(sc)) m.key_event(0xE0, 0);
    m.key_event(down ? code : uint8_t(code | 0x80), down ? bios : 0);
}

}  // namespace f19
