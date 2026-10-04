// Gamepad input (SDL3, e.g. an Xbox controller) and VR motion controllers
// (OpenXR, read by XrOutput), mapped by a binding file (native/gamepad.cfg
// is the built-in default) onto the emulated PC joystick, look-around and
// keyboard keys.
#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <string>
#include <vector>

namespace f19 {

// VR motion controllers (Windows Mixed Reality layout), per hand (0 left,
// 1 right). Stick and trackpad y are down + like a gamepad's.
struct MotionControllers {
    bool active[2] = {};
    float trigger[2] = {};
    bool squeeze[2] = {}, menu[2] = {};
    float stick[2][2] = {}, pad[2][2] = {};
    bool stick_click[2] = {}, pad_click[2] = {}, pad_touch[2] = {};
};

class Gamepad {
public:
    ~Gamepad();

    // Parse bindings; errors are reported on stderr against `source` and
    // the offending lines skipped. Returns false if any line was bad.
    bool load(const std::string& text, const std::string& source);

    // Opens / closes controllers on SDL_EVENT_GAMEPAD_ADDED / _REMOVED.
    void handle_event(const SDL_Event& ev);

    // Synthesised keyboard input: a key going down or up, with the
    // modifiers held for it (the modifier keys arrive as their own calls).
    std::function<void(SDL_Scancode sc, SDL_Keymod mod, bool down)> send_key;

    // Sample the controllers (once per frame) and fire key events. `vr` is
    // the motion controllers' state, if any.
    struct State {
        bool connected = false;
        float stick_x = 0, stick_y = 0;  // -1..1, y down +
        bool button[2] = {};
        float look_x = 0, look_y = 0;    // -1..1, y up +
    };
    State update(uint64_t now_ns, const MotionControllers* vr = nullptr);

private:
    enum class Source { Button, Axis, Motion };
    // Motion controller inputs; the trackpad directions are clicks in that
    // part of the pad.
    enum Motion {
        MTrigger, MSqueeze, MMenu, MStickX, MStickY, MStickClick, MPadX, MPadY, MPadClick, MPadTouch,
        MPadUp, MPadDown, MPadLeft, MPadRight, MPadCenter,
    };
    struct Control {
        Source source = Source::Button;
        int index = 0;  // SDL_GamepadButton, SDL_GamepadAxis or Motion
        int hand = 0;   // Motion: 0 left, 1 right
        int half = 0;   // axis as button: -1 / +1 (0: full axis, or trigger)
        bool full_axis() const;
    };
    enum class Action { StickX, StickY, LookX, LookY, Button1, Button2, Key };
    struct Binding {
        Control control;
        Action action;
        bool invert = false;
        SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
        SDL_Keymod mod = SDL_KMOD_NONE;
        bool held = false;
        uint64_t next_repeat_ns = 0;
    };
    std::vector<Binding> bindings_;
    float deadzone_ = 0.12f, threshold_ = 0.5f;
    SDL_Gamepad* pad_ = nullptr;
    const MotionControllers* vr_ = nullptr;  // during update()

    float axis(const Control& c) const;
    bool pressed(const Control& c) const;
    void key(const Binding& b, bool down);
    void release_all();
};

}  // namespace f19
