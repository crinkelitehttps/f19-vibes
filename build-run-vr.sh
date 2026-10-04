# VR: (re)start Monado for the Acer AH101 (IMU-only tracking, per-eye
# vertical display correction; see docs/vr.md), then build and run.
pkill -x monado-service; sleep 1
WMR_SLAM=0 WMR_HANDTRACKING=0 \
WMR_LEFT_DISPLAY_VIEW_Y_OFFSET=50 WMR_RIGHT_DISPLAY_VIEW_Y_OFFSET=-50 \
    monado-service </dev/zero >"${XDG_RUNTIME_DIR:-/tmp}/monado.log" 2>&1 &
sleep 3
ninja -C build/native && build/native/f19 out/run/native --vr
