#!/usr/bin/env bash
# Find Monado's per-eye vertical display offsets for a WMR headset by eye.
#
# Restarts monado-service with WMR_LEFT/RIGHT_DISPLAY_VIEW_Y_OFFSET (panel
# pixels) and runs hello_xr in the headset; type new values and look again.
# On quit, Monado keeps running with the last values (for the game) and the
# command line to reproduce them is printed.
#
# Usage: tools/vr-offsets.sh [LEFT RIGHT]
# Commands at the prompt:
#   L R    set both offsets (e.g. "10 -10")
#   +N/-N  split change: left += N, right -= N
#   l N    left only, r N right only (absolute)
#   Enter  restart with the same values
#   q      quit, leave Monado running
# WMR_SLAM defaults to 0 (Basalt diverged on this headset); override by
# exporting it.
set -u

left=${1:-0}
right=${2:-0}
slam=${WMR_SLAM:-0}
run=${XDG_RUNTIME_DIR:-/tmp}
log=$run/vr-offsets-monado.log
fifo=$run/vr-offsets.fifo
monado_pid=
hello_pid=

# Both programs quit on input from stdin: give them a pipe nobody writes to.
rm -f "$fifo"
mkfifo "$fifo"
exec 3<>"$fifo"

stop_hello() {
    [ -n "$hello_pid" ] && kill "$hello_pid" 2>/dev/null && wait "$hello_pid" 2>/dev/null
    hello_pid=
}

stop_all() {
    stop_hello
    pkill -x hello_xr 2>/dev/null
    pkill -x monado-service 2>/dev/null
    for _ in $(seq 50); do pgrep -x monado-service >/dev/null || break; sleep 0.1; done
    monado_pid=
}

start() {
    stop_all
    WMR_SLAM=$slam WMR_HANDTRACKING=0 \
        WMR_LEFT_DISPLAY_VIEW_Y_OFFSET=$left WMR_RIGHT_DISPLAY_VIEW_Y_OFFSET=$right \
        monado-service <&3 >"$log" 2>&1 &
    monado_pid=$!
    for _ in $(seq 150); do
        grep -q 'service has started' "$log" 2>/dev/null && break
        if ! kill -0 "$monado_pid" 2>/dev/null; then
            echo "monado-service exited; last lines of $log:"
            tail -n 15 "$log"
            return 1
        fi
        sleep 0.1
    done
    sleep 1  # let it finish opening the headset
    hello_xr -g OpenGL <&3 >/dev/null 2>&1 &
    hello_pid=$!
    echo "running: left $left, right $right (Monado log: $log)"
}

trap 'stop_all; exec 3>&-; rm -f "$fifo"; exit 130' INT TERM

is_int() { [[ $1 =~ ^[-+]?[0-9]+$ ]]; }

start
while true; do
    read -r -p "offsets [L R | +N | -N | l N | r N | Enter | q]: " a b || a=q
    case "$a" in
        q) break ;;
        "") ;;
        l) is_int "${b:-x}" && left=$b || { echo "l N"; continue; } ;;
        r) is_int "${b:-x}" && right=$b || { echo "r N"; continue; } ;;
        +*|-*)
            if [ -z "${b:-}" ] && is_int "$a"; then
                left=$((left + a)); right=$((right - a))
            elif is_int "$a" && is_int "$b"; then
                left=$a; right=$b
            else
                echo "?"; continue
            fi ;;
        *)
            if is_int "$a" && is_int "${b:-x}"; then left=$a; right=$b
            else echo "?"; continue; fi ;;
    esac
    start
done

stop_hello
exec 3>&-
rm -f "$fifo"
echo
echo "Monado left running with these offsets. To start it like this again:"
echo "  WMR_SLAM=$slam WMR_HANDTRACKING=0 WMR_LEFT_DISPLAY_VIEW_Y_OFFSET=$left WMR_RIGHT_DISPLAY_VIEW_Y_OFFSET=$right monado-service"
