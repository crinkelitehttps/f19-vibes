# VR (OpenXR) — status and notes

State on 2026-10-04: **working** with the Acer AH101 under Monado, with
rotation-only tracking and a per-eye display correction:

```
WMR_SLAM=0 WMR_HANDTRACKING=0 \
WMR_LEFT_DISPLAY_VIEW_Y_OFFSET=50 WMR_RIGHT_DISPLAY_VIEW_Y_OFFSET=-50 monado-service &
sleep 3; build/native/f19 out/run/native --vr     # Shift+F12 recentres
```

`./build-run-vr.sh` does all of this (restarts Monado with these settings,
builds, runs). Brakes are `0` (toggle); full keys in `fullgame/F19.KEY`.

Camera-based 6DOF (Basalt) diverges on this setup and is left off, so
leaning is not tracked and the heading drifts slowly (recentre). The code
is runtime-agnostic and should work unchanged with any OpenXR runtime.

## Hardware and software tried

- Acer AH101 (Windows Mixed Reality, 2880x1440 at 60/90 Hz, inside-out
  tracking with two front cameras + IMU), on the Arch laptop (AMD Renoir
  iGPU, i3/Xorg).
- Monado (`monado-git` 25.1.0-883, AUR) as the OpenXR runtime, Basalt
  (`basalt-monado-git`) for camera-based 6DOF, `xr-hardware` udev rules,
  `libuvc`. No AUR helper on this machine: packages built with `makepkg -si`
  in `~/source/aur`, in that order: `xr-hardware` (needs `--nocheck`: its
  test hardcodes python3.10), `monado-git`, `libuvc`, `basalt-monado-git`.
  `paru-bin` is broken against pacman 7.1 (libalpm.so.16); Envision's
  PKGBUILD depends on AUR packages, so plain makepkg cannot build it.
- Active runtime: `~/.config/openxr/1/active_runtime.json`, a copy of
  `/usr/share/openxr/1/openxr_monado.json` with absolute library paths (a
  symlink would resolve the relative paths against `~/.config`).
- Run: `WMR_HANDTRACKING=0 monado-service` (its camera hand tracking
  aborts on missing ONNX models otherwise), then
  `build/native/f19 out/run/native --vr`. Monado drives the headset in X11
  direct mode (DRM lease); the headset is marked non-desktop and is not an
  i3 output. `hello_xr -g OpenGL` (from the `openxr` package) is the quick
  runtime check. From a non-interactive shell `monado-service` needs a
  stdin it can poll (`sleep N | monado-service`).

## What went wrong

- **Basalt 6DOF diverges.** Logged with `F19_XR_LOG=1`, headset still: the
  reported position ran off at ~8.5 km/s within half a second of starting
  and kept going, while the runtime still flagged it valid; orientation
  wandered as well (sky below, drift). `hello_xr` looked fine in a brief
  check. Not investigated further; usual suspects are lighting/texture in
  the room, camera exposure (`WMR_AUTOEXPOSURE`, `WMR_UNIFY_EXPGAIN`), the
  headset's factory calibration, or a Monado/Basalt version mismatch.
- **IMU-only 3DOF (`WMR_SLAM=0`)** is stable: the game runs well with it.
  (A first test looked unusable, but the game had connected to a still
  running Monado with Basalt: a second `monado-service` exits at once with
  "already running", so check `pgrep monado-service` when changing
  settings.)
- Performance was acceptable: at `--vr-scale 0.7` (1410x1410 per eye) the
  demo holds ~57-60 fps of the headset's 60 Hz; at Monado's recommended
  2015x2015 it falls to 30-50.

## Eye alignment

With `WMR_SLAM=0` tracking is stable (the game runs well), but the left
eye's image sits higher than the right, seen as doubled images (also in
`hello_xr`): Monado applies the AH101's factory display calibration
imperfectly. OpenXR has no calibration tool; display correction is the
runtime's job. Monado's workaround is `WMR_LEFT_DISPLAY_VIEW_Y_OFFSET` /
`WMR_RIGHT_DISPLAY_VIEW_Y_OFFSET` (panel pixels, default 0, read at
startup, applied in the distortion mesh); there is no horizontal
equivalent. `tools/vr-offsets.sh` restarts Monado + `hello_xr` with
offsets typed at a prompt, to find them by eye. For this AH101 the
misalignment was purely vertical and `+50` (left 50, right -50) merges
the two eyes' images.

## Options

- **Another headset on Linux.** Anything with a solid OpenXR runtime should
  just work with `--vr`: SteamVR on Linux (Valve's own headsets), Monado
  with lighthouse-tracked hardware (Index/Vive via the `lighthouse` or
  `steamvr` builders, which track far better than WMR inside-out), or
  WiVRn/ALVR streaming to a standalone headset. Valve's new headset (Steam
  Frame) is the obvious candidate given SteamOS/Linux; check its Linux PC
  support and OpenXR runtime before buying.
- **Windows port.** WMR tracks properly with Microsoft's own runtime, but
  Microsoft removed Windows Mixed Reality from Windows 11 24H2 onwards, so
  this headset only works on Windows 10 or Windows 11 23H2 or older. The
  port itself would be moderate: the game core is portable C++, SDL3 and
  OpenGL work on Windows, and the OpenXR code would need the WGL graphics
  binding (`XrGraphicsBindingOpenGLWin32KHR`: HDC + HGLRC from SDL) instead
  of GLX. The host's POSIX bits (head-tracking UDP socket, `setenv`) need
  small changes.
- **Basalt tuning**, if worth the time: Monado's docs and issue tracker for
  WMR SLAM; try a well-lit, textured room; compare the stable Monado
  release with `monado-git`.

## Implementation

- `native/src/host/xr.{h,cpp}` (`XrOutput`): instance with
  `XR_KHR_opengl_enable`, session on the GLX context SDL creates (X11
  only; display, context, drawable, FBConfig and visual from GLX), stereo
  view configuration, one sRGB swapchain per eye plus one 1280x960 for the
  virtual screen, a framebuffer object per swapchain image. Session state
  handled in `poll()` (READY → begin, STOPPING → end, EXITING / loss → VR
  off, the game carries on on the desktop).
- Spaces: the play space is the runtime's LOCAL space moved to the head,
  yaw only, at session start; Shift+F12 recentres (re-creates it). Head
  positions more than 1 m from the seat are treated as tracking failures:
  both eyes are moved back around the seat (eye separation kept), also in
  the poses submitted, so the compositor reprojects what was rendered.
- Per frame (`host/f19.cpp`): `xrWaitFrame` paces the main loop (vsync
  and the refresh pacing are off while the session runs); in the 3D view
  each eye is rendered with `GlRenderer::render_view` straight into the
  swapchain image and submitted as a projection layer; everything else
  (menus, text mode, F11 flat view, frames without tracking) is drawn into
  the screen swapchain and submitted as a quad layer, 1.6 m wide, 1.8 m
  ahead. The window mirrors the right eye.
- `GlRenderer::render_view(frame, w, h, EyeView, dst_fbo)`: the 3D cockpit
  renderer generalised to any eye: rotation matrix, eye position (cm) and
  an asymmetric frustum (tangents). The world projection, the per-pixel
  sky shader (principal point and separate focal lengths) and the panel/HUD
  matrix all take the off-centre frustum. `render_cockpit3d` (desktop)
  builds a symmetric `EyeView` from the head pose and calls it. Eye
  position moves the eye relative to the panel quad (stereo parallax on
  it) and in the world; the HUD is collimated (at infinity, rotation
  only), so it is single and sharp when focusing on distant targets, assuming world units are feet
  (`world_units_per_cm`, unverified). The scene build is cached per frame,
  so both eyes share it.
- OpenXR → aircraft frame: OpenXR looks down -z, the aircraft frame has z
  forward, so the rotation is `S R S` and positions `S p` with
  `S = diag(1, 1, -1)`.
- Swapchains are sRGB and written without `GL_FRAMEBUFFER_SRGB`: the
  game's colours are already sRGB-encoded and the runtime decodes them.
- Options: `--vr`, `--vr-scale F` (default 0.7 of the recommended eye
  size), MSAA 4x by default in VR (`--msaa N` overrides). `F19_PERF=1` adds
  per-frame wait / render / submit times; `F19_XR_LOG=1` prints the eye
  pose twice a second.

## Motion controllers

The AH101's original WMR controllers pair with the laptop over Bluetooth
("Motion controller - Left/Right"; hold the pairing button under the
battery cover). Monado's WMR driver talks to them; switch them on before
starting Monado. Monado main since b0b1d8b1b ("drivers: fix dead stores",
2026-08) rejects every Bluetooth WMR controller ("Failed to get WMR
Bluetooth controller string descriptor": `wmr_create_bt_controller`
checks `ret != 0`, but the prober returns the string's length); the local
AUR build carries a one-line patch (`ret <= 0`). With it, both
controllers were confirmed live through OpenXR (2026-10-04). Turn them on
before starting Monado: it only probes at startup. Mainline Monado tracks them by IMU only (no optical
constellation tracking), so f19 uses only their buttons and axes.

`xr.cpp` creates one action set ("flight") with trigger, grip, menu,
thumbstick (+click) and trackpad (+click, +touch) actions, both hands as
subaction paths, suggested for `/interaction_profiles/microsoft/motion_controller`,
and syncs them each frame while the session is focused
(`XrOutput::controllers`). The state goes through the gamepad bindings
(`native/gamepad.cfg`, `left-`/`right-` controls): by default the right
thumbstick is the joystick, right trigger / grip are fire buttons 1 / 2,
the left thumbstick's y is the throttle, the trackpads are four-way
pads with a centre click for weapons, defences and flight controls (right
trackpad: centre = HUD mode, right = `cycle` through the right CRT's
pages), and the left menu button recentres the view (`recenter` action,
same as Shift+F12).

## Not done

- External views in VR: their 2D overlay is drawn in screen space per eye,
  so it is head-locked (should be a quad in the world like the HUD).
- 90 Hz (the runtime chose 60 Hz here), controller poses (pointing,
  grabbing a stick), a
  Windows/WGL binding, Wayland/EGL binding.
- World scale (feet assumed) and cockpit scale never checked in a working
  headset.
