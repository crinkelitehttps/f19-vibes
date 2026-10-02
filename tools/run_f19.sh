#!/usr/bin/env bash
# Run F-19 in DOSBox from a disposable copy of the originals.
#
#   tools/run_f19.sh [full|demo] [--fresh] [extra DOSBox args...]
#
# The game runs from out/run/<which>/ (created on first use; --fresh
# recreates it), so saves, rosters and recordings never touch fullgame/ or
# gamefiles/. Screenshots (Ctrl+F5) and recordings (Ctrl+Alt+F5) land in
# out/run/capture/. Speed: Ctrl+F11 / Ctrl+F12 lower / raise CPU cycles.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
which="${1:-full}"; shift || true
case "$which" in
  full) src="$root/fullgame" ;;
  demo) src="$root/gamefiles" ;;
  *) echo "usage: $0 [full|demo] [--fresh] [dosbox args...]" >&2; exit 2 ;;
esac

run="$root/out/run/$which"
if [[ "${1:-}" == "--fresh" ]]; then rm -rf "$run"; shift; fi
if [[ ! -d "$run" ]]; then
  mkdir -p "$run"
  # Copy without the demo's archive; make the copy writable.
  find "$src" -maxdepth 1 -type f ! -name '*.zip' -exec cp -p {} "$run/" \;
  chmod -R u+w "$run"
fi
mkdir -p "$root/out/run/capture"

conf="$root/out/run/$which.conf"
cat > "$conf" <<EOF
[sdl]
fullscreen=false
output=opengl
windowresolution=1280x800
autolock=true

[dosbox]
machine=svga_s3
memsize=4
captures=$root/out/run/capture

[render]
aspect=true
scaler=normal2x

[cpu]
core=normal
cputype=auto
cycles=fixed 6000

[speaker]
pcspeaker=true
tandy=auto

[autoexec]
mount c "$run"
c:
F19
exit
EOF

exec dosbox -conf "$conf" "$@"
