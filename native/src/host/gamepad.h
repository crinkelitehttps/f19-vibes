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
        bool recenter = false;           // a recenter binding was just pressed
        bool manual = false;             // a manual binding was just pressed (show / hide it)
        int manual_page = 0;             // pages to turn (manual-page bindings)
    };
    // `manual_shown`: the manual is up, so `manual` layer bindings act (and
    // their controls' other bindings don't).
    State update(uint64_t now_ns, const MotionControllers* vr = nullptr, bool manual_shown = false);

    // The loaded bindings in readable form, in file order (for the
    // clipboard's controls pages).
    struct Help {
        bool vr = false;          // a motion controller binding
        bool gap = false;         // a blank line before it in the file
        std::string control;      // e.g. "S2 L pad up", "M D-pad left"
        std::string action;       // e.g. "Shift+Up", "button 1"
        std::string note;         // the line's comment
    };
    const std::vector<Help>& help() const { return help_; }

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
    enum class Action { StickX, StickY, LookX, LookY, Button1, Button2, Key, Cycle, Recenter, Shift, Manual, ManualPage };
    struct Chord {
        SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
        SDL_Keymod mod = SDL_KMOD_NONE;
    };
    struct Binding {
        Control control;
        Action action;
        bool invert = false;
        std::vector<Chord> keys;  // Key: one; Cycle: pressed in turn
        size_t step = 0;          // Cycle: the key the next press sends
        // Key, cycle, recenter, manual, stick, look: the shift layer it acts
        // in (0 none, 1, 2).
        // Shift: the shift it holds (1, 2).
        int layer = 0;
        int pages = 0;            // ManualPage: pages per press (- back)
        // In the `manual` layer: acts only while the manual is shown.
        bool manual_layer = false;
        // Its control has a `manual` layer binding, which takes over while
        // the manual is shown.
        bool shadowed = false;
        // Stick, look: the shift layers (bit n: shift n) in which its axis
        // has a shifted stick or look binding that takes over.
        int axis_shadowed = 0;
        bool axis_action() const {
            return action == Action::StickX || action == Action::StickY || action == Action::LookX || action == Action::LookY;
        }
        bool held = false;
        bool was_down = false;    // the control, last update
        uint64_t next_repeat_ns = 0;
    };
    std::vector<Binding> bindings_;
    std::vector<Help> help_;
    float deadzone_ = 0.12f, threshold_ = 0.5f;
    SDL_Gamepad* pad_ = nullptr;
    const MotionControllers* vr_ = nullptr;  // during update()
    // Per hand: the trackpad region a click started in (MPadUp..MPadCenter),
    // kept until the click is released; -1 while not clicked.
    int pad_region_[2] = {-1, -1};

    float axis(const Control& c) const;
    bool pressed(const Control& c) const;
    void key(const Binding& b, bool down);
    void release_all();
};

}  // namespace f19
