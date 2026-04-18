#!/bin/bash
# CopyCatOS Auto-Rotation — polls iio-sensor-proxy and rotates via xrandr
DISPLAY_OUTPUT="eDP-1"

# Claim the accelerometer
gdbus call --system --dest net.hadess.SensorProxy \
  --object-path /net/hadess/SensorProxy \
  --method net.hadess.SensorProxy.ClaimAccelerometer &>/dev/null

rotate() {
    local orient="$1"
    case "$orient" in
        normal)    xr="normal";    matrix="1 0 0 0 1 0 0 0 1" ;;
        left-up)   xr="left";      matrix="0 -1 1 1 0 0 0 0 1" ;;
        right-up)  xr="right";     matrix="0 1 0 -1 0 1 0 0 1" ;;
        bottom-up) xr="inverted";  matrix="-1 0 1 0 -1 1 0 0 1" ;;
        *) return ;;
    esac

    xrandr --output "$DISPLAY_OUTPUT" --rotate "$xr"

    # Rotate touch input to match
    for id in $(xinput list --id-only 2>/dev/null); do
        if xinput list-props "$id" 2>/dev/null | grep -q "Coordinate Transformation Matrix"; then
            xinput set-prop "$id" "Coordinate Transformation Matrix" $matrix 2>/dev/null
        fi
    done
}

PREV=""
while true; do
    ORIENT=$(gdbus call --system --dest net.hadess.SensorProxy \
        --object-path /net/hadess/SensorProxy \
        --method org.freedesktop.DBus.Properties.Get \
        net.hadess.SensorProxy AccelerometerOrientation 2>/dev/null \
        | grep -oP "'[^']+'" | tail -1 | tr -d "'")

    if [[ -n "$ORIENT" && "$ORIENT" != "$PREV" ]]; then
        rotate "$ORIENT"
        PREV="$ORIENT"
    fi
    sleep 1
done
