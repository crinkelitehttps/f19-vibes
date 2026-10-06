#!/bin/sh
# Package release archives from the current builds (build/native/f19 and
# build/windows/f19.exe) into build/release/. No game files: players bring
# their own copy of F-19 Stealth Fighter.
#   tools/make-release.sh v0.1.0
set -e
ver=${1:?usage: tools/make-release.sh VERSION}
cd "$(dirname "$0")/.."
ninja -C build/native f19
./build-windows.sh
out=build/release
rm -rf "$out"
mkdir -p "$out"

keys='Host keys (not passed to the game):
  F11         3D cockpit / flat view      Shift+F11  native world / engine'"'"'s
  Ctrl+F11    clipboard (controls pages; PgUp/PgDn turn pages)
  Alt+F11     classic one-pixel lines
  F12         screenshot                  Shift+F12  recentre VR
  Alt+F12     building draw distance (6/9/12/18/24 tiles)
  Ctrl+F12    model detail distance (1/2/3/4/6 x)
  Right mouse button + drag: look around

Gamepad: hold LT for the shift-1 layer, LB for shift-2; LT + Start cycles
the HUD mode (NAV / air-to-air / air-to-ground; keyboard F2).'

gamenote='Copy the files of your own copy of F-19 Stealth Fighter (DOS, MicroProse
1988; the full game, e.g. version 435.01, or the demo) into this folder,
so that F19.COM is directly in it. The game saves its pilot roster here,
so the folder must be writable.'

# Windows
w=$out/windows/F19
mkdir -p "$w/game"
cp build/windows/f19.exe native/gamepad.cfg "$w/"
printf '%s\n' "$gamenote" > "$w/game/PUT-GAME-FILES-HERE.txt"
cat > "$w/F19.bat" <<'EOF'
@echo off
rem F-19 Stealth Fighter, native port. Extra options can be added after the
rem command, e.g.  F19.bat --msaa 4
cd /d "%~dp0"
f19.exe "%~dp0game" %*
if errorlevel 1 pause
EOF
cat > "$w/F19-VR.bat" <<'EOF'
@echo off
rem F-19 in VR: uses the active OpenXR runtime (Mixed Reality Portal or SteamVR).
cd /d "%~dp0"
f19.exe "%~dp0game" --vr %*
if errorlevel 1 pause
EOF
cat > "$w/F19-debug.bat" <<'EOF'
@echo off
rem Diagnostic start: logs each startup step and SDL's messages, and keeps
rem this window open. Add --no-gamepad to skip gamepad detection.
cd /d "%~dp0"
f19.exe "%~dp0game" --verbose %*
pause
EOF
cat > "$w/README.txt" <<EOF
F-19 Stealth Fighter - native port, Windows x64 ($ver)

You need your own copy of the game: see game\\PUT-GAME-FILES-HERE.txt.
Extract this folder somewhere writable (e.g. C:\\Games\\F19, not Program
Files).

  F19.bat       play on the desktop
  F19-VR.bat    play in VR (OpenXR: Mixed Reality Portal or SteamVR)
  F19-debug.bat start with a step-by-step log (if the game does not start;
                F19-debug.bat --no-gamepad skips gamepad detection)
  (older non-Xbox gamepads may need --directinput)
  gamepad.cfg   gamepad / VR controller bindings; to change them, copy it
                to %APPDATA%\\f19\\gamepad.cfg and edit that copy

The console window shows the log; if something fails, it stays open.

$keys

Known limits: the PDF manual is not shown on the in-game clipboard (the
controls pages are).

Options go after the .bat name, for example:
  F19.bat --msaa 4 --detail-radius 18 --light-size 5
EOF
sed -i 's/$/\r/' "$w"/*.bat "$w"/*.txt "$w"/game/*.txt
python3 -I - "$out/windows" "$out/F19-$ver-windows-x64.zip" <<'EOF'
import os, sys, zipfile
root, dest = sys.argv[1:]
with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as z:
    for d, _, files in os.walk(root):
        for f in sorted(files):
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, root))
EOF

# Linux
l=$out/linux/f19-$ver-linux-x86_64
mkdir -p "$l/game"
cp build/native/f19 native/gamepad.cfg "$l/"
strip "$l/f19"
printf '%s\n' "$gamenote" > "$l/game/PUT-GAME-FILES-HERE.txt"
cat > "$l/f19.sh" <<'EOF'
#!/bin/sh
# F-19 Stealth Fighter, native port. Extra options go after, e.g. ./f19.sh --msaa 4
here=$(dirname "$(readlink -f "$0")")
exec "$here/f19" "$here/game" "$@"
EOF
cat > "$l/f19-vr.sh" <<'EOF'
#!/bin/sh
# F-19 in VR: uses the active OpenXR runtime (e.g. Monado, SteamVR).
here=$(dirname "$(readlink -f "$0")")
exec "$here/f19" "$here/game" --vr "$@"
EOF
chmod +x "$l"/*.sh
cat > "$l/README.txt" <<EOF
F-19 Stealth Fighter - native port, Linux x86_64 ($ver)

You need your own copy of the game: see game/PUT-GAME-FILES-HERE.txt.

  ./f19.sh      play on the desktop
  ./f19-vr.sh   play in VR (OpenXR)
  gamepad.cfg   gamepad / VR controller bindings; to change them, copy it
                to ~/.config/f19/gamepad.cfg and edit that copy

Built on Arch Linux (glibc $(pacman -Q glibc | cut -d' ' -f2 | cut -d+ -f1)) against the system libraries; it
needs SDL3, the OpenXR loader, poppler (poppler-cpp), zlib and OpenGL/X11:
  Arch:          pacman -S sdl3 openxr poppler
Other distributions may need to build from source (see the repository's
README and native/CMakeLists.txt). Head tracking: OpenTrack "UDP over
network" to 127.0.0.1:4242.

$keys

Options go after the script name, for example:
  ./f19.sh --msaa 4 --detail-radius 18 --light-size 5
EOF
tar -C "$out/linux" -czf "$out/f19-$ver-linux-x86_64.tar.gz" "f19-$ver-linux-x86_64"

ls -l "$out"/*.zip "$out"/*.tar.gz
