// OpenXR output (e.g. Monado with a Windows Mixed Reality headset).
//
// The 3D view is rendered once per eye (GlRenderer::render_view) into the
// runtime's swapchains; everything else (menus, text mode, flat views) is
// shown on a virtual screen ahead of the seat (a quad layer). Needs an
// OpenGL context on X11 (GLX), as SDL creates under i3/Xorg.
//
// Per frame:
//   if (xr.begin_frame()) {           // waits for the runtime's frame timing
//       for each eye: render into xr.acquire_eye(i), xr.release_eye(i)
//       or: draw into xr.acquire_screen(), xr.release_screen()
//   }
//   xr.end_frame();                   // after every begin_frame()
#pragma once

#include <cstdint>
#include <memory>

#include "hires/gl_render.h"

namespace f19 {

class XrOutput {
public:
    XrOutput();
    ~XrOutput();
    // Create the instance and session for the current GL context. Returns
    // false (with a message) if there is no runtime or headset.
    bool init(float resolution_scale);
    // Handle runtime events. Returns false once the runtime has ended the
    // session for good (headset unplugged, runtime quit).
    bool poll();
    // Session running: frames must be submitted (begin_frame/end_frame).
    bool running() const;

    // Wait for the next frame and locate the eyes; false if nothing needs
    // rendering this frame (end_frame must still be called).
    bool begin_frame();
    bool tracking() const;          // the eyes were located this frame
    int eye_width() const;
    int eye_height() const;
    EyeView eye(int i) const;       // eye i (0 left, 1 right) for this frame
    GLuint acquire_eye(int i);      // framebuffer to render eye i into
    void release_eye(int i);
    static constexpr int kScreenW = 1280, kScreenH = 960;
    GLuint acquire_screen();        // framebuffer (kScreenW x kScreenH) for the virtual screen
    void release_screen();
    // Submit what was rendered this frame (the eyes or the screen).
    void end_frame();
    // Make the current head position and heading the neutral eye point.
    void recenter();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace f19
