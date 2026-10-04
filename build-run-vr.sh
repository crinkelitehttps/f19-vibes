# VR: (re)start Monado for the Acer AH101 (IMU-only tracking, per-eye
# vertical display correction; see docs/vr.md), then build and run.
# Switch the motion controllers on first: Monado only finds them at startup.
log="${XDG_RUNTIME_DIR:-/tmp}/monado.log"
if pgrep -x f19 >/dev/null; then
    echo "f19 is already running (pid $(pgrep -x f19 | tr '\n' ' ')); quit it first" >&2
    exit 1
fi
# Stop the old service and wait until it has really gone (a second
# instance refuses to start while the first is still shutting down).
pkill -x monado-service
for _ in $(seq 50); do pgrep -x monado-service >/dev/null || break; sleep 0.1; done
if pgrep -x monado-service >/dev/null; then
    pkill -9 -x monado-service; sleep 0.5
fi
# Monado polls stdin (Enter quits): give it a pipe nobody writes to.
sleep infinity | WMR_SLAM=0 WMR_HANDTRACKING=0 \
    WMR_LEFT_DISPLAY_VIEW_Y_OFFSET=50 WMR_RIGHT_DISPLAY_VIEW_Y_OFFSET=-50 \
    monado-service >"$log" 2>&1 &
sleep 3
if ! pgrep -x monado-service >/dev/null; then
    echo "monado-service failed to start; see $log" >&2
    tail -5 "$log" >&2
    exit 1
fi
grep -E "controller config" "$log" | sed 's/.*Reading /monado: /' >&2
ninja -C build/native && build/native/f19 out/run/native --vr
