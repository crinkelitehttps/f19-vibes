# F-19 native on Windows

`f19.exe` is the same program as the Linux build: the original game runs
in the interpreter, and the window, sound, gamepads and VR go through SDL3
and OpenXR. You need your own copy of the game files.

## Running

1. Copy the full game's files into a folder you can write to, e.g.
   `C:\Games\F19`.
2. Run, from a command prompt (it logs to the console):

   ```
   f19.exe C:\Games\F19
   f19.exe C:\Games\F19 --vr
   ```

The options are the same as on Linux (see the top of
`native/src/host/f19.cpp`). Your own gamepad bindings go in
`%APPDATA%\f19\gamepad.cfg` (start from `gamepad.cfg`), and the clipboard
remembers its page in `%APPDATA%\f19\manual-page`.

The in-game manual (PDF on the clipboard) needs poppler, which the Windows
build doesn't include yet. The controls pages still work.

## VR

`--vr` uses whichever OpenXR runtime is active. On Windows, f19 hands the
runtime Direct3D 11 images (the GL renderer draws into them through
`WGL_NV_DX_interop2`), because the Windows Mixed Reality runtime supports no
OpenGL. Direct3D 11 works with every Windows runtime:

| Headset | Runtime |
|---|---|
| WMR (Acer AH101, HP Reverb, ...), Windows 10 or Windows 11 23H2 and earlier | Mixed Reality Portal (its OpenXR runtime) |
| WMR on Windows 11 24H2 and later (WMR was removed) | SteamVR with the Oasis driver (NVIDIA GPUs only) |
| Valve Index, HTC Vive, Pimax, ... | SteamVR |
| Quest | Meta Quest Link, or Virtual Desktop (VDXR), or SteamVR |

With SteamVR, make it the active OpenXR runtime (SteamVR settings >
OpenXR). On laptops, run f19 on the GPU the headset is connected to; it
asks for the high-performance GPU itself.

### Controllers

The bindings in `gamepad.cfg` are written for WMR motion controllers
(thumbstick plus trackpad). Other controllers are bound to the same
controls where they have them:

| Controller | Notes |
|---|---|
| WMR motion controllers | everything |
| HP Reverb G2, Quest Touch | Y/B = trackpad up, X/A = trackpad down. No trackpad left/right/centre. On Touch the right menu button is the system button, so right `menu` is missing too. |
| Valve Index | trackpad (press = click); B = menu; A = trackpad down |
| HTC Vive wands | trackpad and menu; no thumbstick, so no flying stick |

## Building

### On Linux (cross-compiling)

Install `mingw-w64-gcc` (Arch), then run `./build-windows.sh`. The first run
downloads SDL3, the OpenXR loader and zlib, and builds them in. The result,
`build/windows/f19.exe`, needs no DLLs. The desktop (non-VR) build runs
under Wine for a quick check: `wine build/windows/f19.exe out/run/native`.

### With Visual Studio

From a "x64 Native Tools Command Prompt":

```
cmake -S native -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target f19
```

The MSVC build needs the Visual C++ runtime, which most PCs have
installed.

### GitHub Actions

`.github/workflows/windows.yml` builds with MSVC on every push to `master`.
`f19-windows-x64` is attached to the run as a downloadable artifact.
