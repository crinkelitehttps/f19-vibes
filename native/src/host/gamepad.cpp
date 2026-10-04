#include "host/gamepad.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "host/keyboard.h"

namespace f19 {

namespace {

// Typematic repeat of a held key, like a PC keyboard's defaults.
constexpr uint64_t kRepeatDelayNs = 500'000'000, kRepeatPeriodNs = 50'000'000;

bool is_trigger(int axis) { return axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER; }

// Motion controller input names, after "left-" / "right-".
const struct { const char* name; int input; } kMotionNames[] = {
    {"trigger", 0}, {"squeeze", 1}, {"menu", 2}, {"thumbstick-x", 3}, {"thumbstick-y", 4}, {"thumbstick", 5},
    {"trackpad-x", 6}, {"trackpad-y", 7}, {"trackpad", 8}, {"trackpad-touch", 9}, {"trackpad-up", 10},
    {"trackpad-down", 11}, {"trackpad-left", 12}, {"trackpad-right", 13}, {"trackpad-center", 14},
};

SDL_Scancode parse_key_name(std::string name) {
    static const struct { const char* alias; const char* sdl; } aliases[] = {
        {"enter", "return"}, {"esc", "escape"}, {"pgup", "pageup"}, {"pgdn", "pagedown"},
        {"ins", "insert"}, {"del", "delete"},
    };
    for (auto& a : aliases)
        if (!SDL_strcasecmp(name.c_str(), a.alias)) name = a.sdl;
    return SDL_GetScancodeFromName(name.c_str());
}

}  // namespace

bool Gamepad::Control::full_axis() const {
    if (half) return false;
    if (source == Source::Axis) return !is_trigger(index);
    return source == Source::Motion && (index == MStickX || index == MStickY || index == MPadX || index == MPadY);
}

Gamepad::~Gamepad() {
    if (pad_) SDL_CloseGamepad(pad_);
}

bool Gamepad::load(const std::string& text, const std::string& source) {
    bindings_.clear();
    bool ok = true;
    std::istringstream in(text);
    std::string line;
    for (int lineno = 1; std::getline(in, line); lineno++) {
        if (auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        std::string control, action, arg, extra;
        if (!(ls >> control)) continue;
        ls >> action >> arg >> extra;
        auto fail = [&](const char* why) {
            std::fprintf(stderr, "%s:%d: %s\n", source.c_str(), lineno, why);
            ok = false;
        };
        if (control == "deadzone" || control == "threshold") {
            char* end = nullptr;
            float v = std::strtof(action.c_str(), &end);
            if (action.empty() || *end || v < 0 || v >= 1 || !arg.empty()) { fail("expected a value from 0 to 1"); continue; }
            (control == "deadzone" ? deadzone_ : threshold_) = v;
            continue;
        }

        Binding b;
        std::string name = control;
        if (name.size() > 1 && (name.back() == '-' || name.back() == '+')) {
            b.control.half = name.back() == '-' ? -1 : 1;
            name.pop_back();
        }
        int motion = -1;
        for (const char* hand : {"left-", "right-"})
            if (name.rfind(hand, 0) == 0)
                for (auto& m : kMotionNames)
                    if (name.substr(std::strlen(hand)) == m.name) {
                        motion = m.input;
                        b.control.hand = hand[0] == 'r';
                    }
        if (motion >= 0) {
            b.control.source = Source::Motion;
            b.control.index = motion;
        } else if (auto btn = SDL_GetGamepadButtonFromString(name.c_str()); btn != SDL_GAMEPAD_BUTTON_INVALID) {
            b.control.index = btn;
        } else if (auto ax = SDL_GetGamepadAxisFromString(name.c_str()); ax != SDL_GAMEPAD_AXIS_INVALID) {
            b.control.source = Source::Axis;
            b.control.index = ax;
        } else {
            fail(("unknown control '" + control + "'").c_str());
            continue;
        }
        if (b.control.half) {
            Control whole = b.control;
            whole.half = 0;
            if (!whole.full_axis()) { fail("only stick and trackpad axes have halves"); continue; }
        }
        const bool full_axis = b.control.full_axis();

        if (action == "stick-x" || action == "stick-y" || action == "look-x" || action == "look-y") {
            b.action = action == "stick-x" ? Action::StickX : action == "stick-y" ? Action::StickY
                     : action == "look-x" ? Action::LookX : Action::LookY;
            if (!full_axis) { fail((action + " needs a stick or trackpad axis (e.g. leftx, right-thumbstick-y)").c_str()); continue; }
            if (arg == "invert") b.invert = true;
            else if (!arg.empty()) { fail(("unexpected '" + arg + "'").c_str()); continue; }
        } else if (action == "button1" || action == "button2" || action == "key") {
            if (full_axis) { fail(("use " + control + "- or " + control + "+ to act as a button").c_str()); continue; }
            if (action == "key") {
                b.action = Action::Key;
                if (arg.empty() || !extra.empty()) { fail("expected one key, e.g. 'key shift+f1'"); continue; }
                std::string k = arg;
                bool bad = false;
                for (size_t plus; !bad && k.size() > 1 && (plus = k.find('+')) != std::string::npos;) {
                    std::string m = k.substr(0, plus);
                    if (!SDL_strcasecmp(m.c_str(), "shift")) b.mod |= SDL_KMOD_LSHIFT;
                    else if (!SDL_strcasecmp(m.c_str(), "ctrl")) b.mod |= SDL_KMOD_LCTRL;
                    else if (!SDL_strcasecmp(m.c_str(), "alt")) b.mod |= SDL_KMOD_LALT;
                    else bad = true;
                    k.erase(0, plus + 1);
                }
                b.key = parse_key_name(k);
                if (bad || b.key == SDL_SCANCODE_UNKNOWN || !key_supported(b.key)) {
                    fail(("unknown key '" + arg + "'").c_str());
                    continue;
                }
            } else {
                b.action = action == "button1" ? Action::Button1 : Action::Button2;
                if (!arg.empty()) { fail(("unexpected '" + arg + "'").c_str()); continue; }
            }
        } else {
            fail(action.empty() ? "missing action" : ("unknown action '" + action + "'").c_str());
            continue;
        }
        bindings_.push_back(b);
    }
    return ok;
}

void Gamepad::handle_event(const SDL_Event& ev) {
    if (ev.type == SDL_EVENT_GAMEPAD_ADDED && !pad_) {
        pad_ = SDL_OpenGamepad(ev.gdevice.which);
        if (pad_) std::fprintf(stderr, "gamepad: %s\n", SDL_GetGamepadName(pad_));
        else std::fprintf(stderr, "warning: gamepad: %s\n", SDL_GetError());
    } else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED && pad_ && ev.gdevice.which == SDL_GetGamepadID(pad_)) {
        std::fprintf(stderr, "gamepad: disconnected\n");
        release_all();
        SDL_CloseGamepad(pad_);
        pad_ = nullptr;
        // Fall back to another controller, if one is plugged in.
        int n = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
            for (int i = 0; i < n && !pad_; i++) {
                pad_ = SDL_OpenGamepad(ids[i]);
                if (pad_) std::fprintf(stderr, "gamepad: %s\n", SDL_GetGamepadName(pad_));
            }
            SDL_free(ids);
        }
    }
}

float Gamepad::axis(const Control& c) const {
    float v = 0;
    if (c.source == Source::Motion) {
        if (!vr_ || !vr_->active[c.hand]) return 0;
        switch (c.index) {
            case MTrigger: v = vr_->trigger[c.hand]; break;
            case MStickX: v = vr_->stick[c.hand][0]; break;
            case MStickY: v = vr_->stick[c.hand][1]; break;
            case MPadX: v = vr_->pad_touch[c.hand] ? vr_->pad[c.hand][0] : 0; break;
            case MPadY: v = vr_->pad_touch[c.hand] ? vr_->pad[c.hand][1] : 0; break;
        }
        v = std::clamp(v, -1.0f, 1.0f);
    } else {
        if (!pad_) return 0;
        v = std::clamp(SDL_GetGamepadAxis(pad_, SDL_GamepadAxis(c.index)) / 32767.0f, -1.0f, 1.0f);
    }
    float mag = std::fabs(v);
    if (mag <= deadzone_) return 0;
    return std::copysign((mag - deadzone_) / (1 - deadzone_), v);
}

bool Gamepad::pressed(const Control& c) const {
    if (c.source == Source::Button) return pad_ && SDL_GetGamepadButton(pad_, SDL_GamepadButton(c.index));
    if (c.source == Source::Motion && c.index != MTrigger && !c.full_axis() && !c.half) {
        const int h = c.hand;
        if (!vr_ || !vr_->active[h]) return false;
        if (c.index == MSqueeze) return vr_->squeeze[h];
        if (c.index == MMenu) return vr_->menu[h];
        if (c.index == MStickClick) return vr_->stick_click[h];
        if (c.index == MPadTouch) return vr_->pad_touch[h];
        if (!vr_->pad_click[h]) return false;
        if (c.index == MPadClick) return true;
        // Trackpad as a four-way pad with a centre button.
        const float x = vr_->pad[h][0], y = vr_->pad[h][1];
        const bool centre = x * x + y * y < 0.4f * 0.4f;
        if (c.index == MPadCenter) return centre;
        if (centre) return false;
        if (std::fabs(x) > std::fabs(y)) return c.index == (x < 0 ? MPadLeft : MPadRight);
        return c.index == (y < 0 ? MPadUp : MPadDown);
    }
    float v = axis(c);
    return (c.half ? v * c.half : v) > threshold_;
}

void Gamepad::key(const Binding& b, bool down) {
    if (!send_key) return;
    static const struct { SDL_Keymod mod; SDL_Scancode sc; } mods[] = {
        {SDL_KMOD_LCTRL, SDL_SCANCODE_LCTRL}, {SDL_KMOD_LALT, SDL_SCANCODE_LALT}, {SDL_KMOD_LSHIFT, SDL_SCANCODE_LSHIFT},
    };
    if (down)
        for (auto& m : mods)
            if (b.mod & m.mod) send_key(m.sc, b.mod, true);
    send_key(b.key, b.mod, down);
    if (!down)
        for (auto& m : mods)
            if (b.mod & m.mod) send_key(m.sc, SDL_KMOD_NONE, false);
}

void Gamepad::release_all() {
    for (auto& b : bindings_)
        if (b.held) {
            if (b.action == Action::Key) key(b, false);
            b.held = false;
        }
}

Gamepad::State Gamepad::update(uint64_t now_ns, const MotionControllers* vr) {
    State s;
    vr_ = vr;
    s.connected = pad_ || (vr && (vr->active[0] || vr->active[1]));
    for (auto& b : bindings_) {
        switch (b.action) {
            case Action::StickX: s.stick_x += b.invert ? -axis(b.control) : axis(b.control); break;
            case Action::StickY: s.stick_y += b.invert ? -axis(b.control) : axis(b.control); break;
            case Action::LookX: s.look_x += b.invert ? -axis(b.control) : axis(b.control); break;
            case Action::LookY: s.look_y -= b.invert ? -axis(b.control) : axis(b.control); break;  // stick up looks up
            case Action::Button1: s.button[0] |= pressed(b.control); break;
            case Action::Button2: s.button[1] |= pressed(b.control); break;
            case Action::Key: {
                bool p = pressed(b.control);
                if (p && !b.held) {
                    key(b, true);
                    b.next_repeat_ns = now_ns + kRepeatDelayNs;
                } else if (p && now_ns >= b.next_repeat_ns) {
                    if (send_key) send_key(b.key, b.mod, true);
                    b.next_repeat_ns = now_ns + kRepeatPeriodNs;
                } else if (!p && b.held) {
                    key(b, false);
                }
                b.held = p;
                break;
            }
        }
    }
    s.stick_x = std::clamp(s.stick_x, -1.0f, 1.0f);
    s.stick_y = std::clamp(s.stick_y, -1.0f, 1.0f);
    s.look_x = std::clamp(s.look_x, -1.0f, 1.0f);
    s.look_y = std::clamp(s.look_y, -1.0f, 1.0f);
    vr_ = nullptr;
    return s;
}

}  // namespace f19
