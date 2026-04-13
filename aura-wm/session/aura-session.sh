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

# Cursor theme
export XCURSOR_THEME=Breeze_Light
export XCURSOR_SIZE=24

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

# ─── Theme configuration ───
# Force light theme for Qt and GTK apps (Snow Leopard was always light)

# KDE/Qt color scheme — force light
export KDE_COLOR_SCHEME=SnowLeopard
# Tell Qt apps to use light mode
export QT_QUICK_CONTROLS_STYLE=org.kde.desktop

# GTK theme — use Adwaita (light) as fallback for any GTK apps
export GTK_THEME=Adwaita
export GTK2_RC_FILES=/usr/share/themes/Adwaita/gtk-2.0/gtkrc

# Icon theme — use the Snow Leopard icon set
export ICON_THEME=AquaKDE-icons

# Set the icon theme for Qt/KDE apps via kdeglobals
# This writes to the KDE config so all Qt apps pick it up
mkdir -p ~/.config
if ! grep -q "AquaKDE-icons" ~/.config/kdeglobals 2>/dev/null; then
    kwriteconfig6 --file kdeglobals --group Icons --key Theme AquaKDE-icons 2>/dev/null || true
fi

# Set light color scheme for KDE
kwriteconfig6 --file kdeglobals --group General --key ColorScheme "BreezeLight" 2>/dev/null || true
kwriteconfig6 --file kdeglobals --group KDE --key LookAndFeelPackage "org.kde.breeze.desktop" 2>/dev/null || true

# Font configuration for Lucida Grande
kwriteconfig6 --file kdeglobals --group General --key font "Lucida Grande,11,-1,5,50,0,0,0,0,0" 2>/dev/null || true
kwriteconfig6 --file kdeglobals --group General --key fixed "Monaco,10,-1,5,50,0,0,0,0,0" 2>/dev/null || true

# GTK icon theme
mkdir -p ~/.config/gtk-3.0
cat > ~/.config/gtk-3.0/settings.ini << 'GTKEOF'
[Settings]
gtk-icon-theme-name=AquaKDE-icons
gtk-theme-name=Adwaita
gtk-application-prefer-dark-theme=false
gtk-font-name=Lucida Grande 11
GTKEOF

# GTK4
mkdir -p ~/.config/gtk-4.0
cp ~/.config/gtk-3.0/settings.ini ~/.config/gtk-4.0/settings.ini

# ─── Set the X root cursor before any windows appear ───
# Without this, the root window shows the ugly X cursor until something else
# overrides it. This forces the left_ptr from our chosen XCURSOR_THEME.
xsetroot -cursor_name left_ptr 2>/dev/null

# ─── Start the window manager first ───
# The WM must claim SubstructureRedirect before any other windows appear
WM=${AURA_WM:-aura-wm}
$WM &
WM_PID=$!
sleep 0.5

# ─── Compositor for ARGB transparency ───
# Crystal Compositor is now handling compositing in manual mode.
# picom is disabled — Crystal draws all windows via OpenGL.
# picom --config ~/AuraOS/aura-wm/session/picom.conf -b
sleep 0.3

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
