#!/bin/bash
# Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
# AuraOS Session Script
# Starts the custom WM and all shell components in the correct order.
# Set AURA_WM=kwin_x11 to fall back to kwin for testing.

# Tell X11 and Qt what kind of session this is
export XDG_SESSION_TYPE=x11
export XDG_SESSION_DESKTOP=AuraOS
export XDG_CURRENT_DESKTOP=AuraOS

# Qt styling — force AquaStyle (the Snow Leopard QStyle plugin)
export QT_QPA_PLATFORMTHEME=kde
export QT_STYLE_OVERRIDE=AquaStyle

# Icon theme — use the Snow Leopard icon set
export XCURSOR_THEME=capitaine-cursors

# Font config — make sure Lucida Grande is available
if [ -d "$HOME/.local/share/fonts" ]; then
    export FONTCONFIG_PATH="$HOME/.local/share/fonts"
fi

# DBus session bus (needed by Qt apps and system tray)
if [ -z "$DBUS_SESSION_BUS_ADDRESS" ]; then
    eval $(dbus-launch --sh-syntax)
    export DBUS_SESSION_BUS_ADDRESS
fi

# Display rotation for Lenovo Legion Go (uncomment if needed)
# xrandr --output eDP-1 --rotate right

# ─── Start the window manager first ───
# The WM must claim SubstructureRedirect before any other windows appear
WM=${AURA_WM:-aura-wm}
$WM &
WM_PID=$!
sleep 0.5

# ─── Desktop surface (wallpaper + icons) ───
# Must come before dock/menubar so it sits at the bottom of the stack
aura-desktop &
sleep 0.2

# ─── Shell components (can start in parallel) ───
aura-menubar &
aura-dock &
aura-spotlight &

# ─── Wait for the WM to exit ───
# When the WM process ends (user logged out or crashed), clean up everything
wait $WM_PID

# Kill all shell components on exit
kill $(jobs -p) 2>/dev/null
wait 2>/dev/null
