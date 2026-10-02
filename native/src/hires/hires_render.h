// Renders a captured HiresFrame at any resolution with SDL3's geometry API:
// world primitives (in the engine's painter's order) first, then the 320x200
// page on top with world pixels made transparent.
#pragma once

#include <SDL3/SDL.h>

#include "hires/world_capture.h"

namespace f19 {

class HiresRenderer {
public:
    explicit HiresRenderer(SDL_Renderer* r) : r_(r) {}
    ~HiresRenderer();
    // Draws `f` into an internal w x h target texture and returns it.
    SDL_Texture* render(const HiresFrame& f, int w, int h);
    float line_width = 0.75f;   // in 320x200 pixels
    bool draw_overlay = true;   // false: world only (debugging)

private:
    SDL_Renderer* r_;
    SDL_Texture* target_ = nullptr;
    SDL_Texture* overlay_ = nullptr;
    int tw_ = 0, th_ = 0;
};

}  // namespace f19
