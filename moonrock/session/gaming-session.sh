#!/bin/bash
# CopyCatOS — by Kyle Blizzard at Blizzard.show

# CopyCatOS Gaming Session
# Pure Gamescope compositor. No moonrock. No cc-shell.
# Steam / emulators own the full screen.
#
# inputd runs in the background, auto-detects this session via
# XDG_SESSION_DESKTOP=CopyCatOS-Gaming, and switches to the GAME profile
# (raw passthrough to the virtual gamepad — Steam Input takes over).

export XDG_SESSION_TYPE=x11
export XDG_SESSION_DESKTOP=CopyCatOS-Gaming
export XDG_CURRENT_DESKTOP=CopyCatOS-Gaming

# DBus session bus
if [ -z "$DBUS_SESSION_BUS_ADDRESS" ]; then
    eval $(dbus-launch --sh-syntax)
    export DBUS_SESSION_BUS_ADDRESS
fi

# inputd (system daemon) should already be running via systemd.
# If not, fail loudly — controller won't work in game mode without it.
if ! pgrep -x inputd > /dev/null; then
    echo "[gaming-session] WARNING: inputd not running. Controller may not work." >&2
fi

# Legion Go S native resolution.
# Gamescope will scale game output to fit this.
NATIVE_W=1920
NATIVE_H=1200
NATIVE_REFRESH=120

# Launch Gamescope with Steam Big Picture as the main surface.
# -W / -H: output size (native)
# -w / -h: internal render size (scaled by -W/-H if game requests different)
# -r:     refresh rate
# -e:     steam integration
# -f:     fullscreen
exec gamescope \
    -W "$NATIVE_W" -H "$NATIVE_H" \
    -w "$NATIVE_W" -h "$NATIVE_H" \
    -r "$NATIVE_REFRESH" \
    -e -f \
    -- \
    steam -gamepadui -steamos3 -steampal -steamdeck
