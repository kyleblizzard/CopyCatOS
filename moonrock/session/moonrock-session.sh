#!/bin/bash
# CopyCatOS — by Kyle Blizzard at Blizzard.show

# CopyCatOS Session Script
# Starts the custom WM and all shell components in the correct order.
# Set MOONROCK_WM=kwin_x11 to fall back to kwin for testing.

# Tell X11 and Qt what kind of session this is
export XDG_SESSION_TYPE=x11
export XDG_SESSION_DESKTOP=CopyCatOS
export XDG_CURRENT_DESKTOP=CopyCatOS

# Clean session-type marker for inputd. A single file at a stable path
# per user is the only signal inputd reads to pick DESKTOP vs GAME profile.
# /run/user/$UID/ is tmpfs, owned by the session user, and goes away on
# logout — exactly the semantics we want.
if [ -n "$XDG_RUNTIME_DIR" ]; then
    echo "desktop" > "$XDG_RUNTIME_DIR/copycatos-session-type"
fi

# Qt styling — force AquaStyle (the Snow Leopard QStyle plugin)
export QT_QPA_PLATFORMTHEME=kde
export QT_STYLE_OVERRIDE=AquaStyle

# Cursor theme — real Snow Leopard cursors extracted from Mac OS X 10.6
# Built from snowleopardaura/MacAssets/Cursors/ via scripts/build-cursor-theme.sh
export XCURSOR_THEME=SnowLeopard
export XCURSOR_SIZE=32

# Font config — make sure Lucida Grande is available
if [ -d "$HOME/.local/share/fonts" ]; then
    export FONTCONFIG_PATH="$HOME/.local/share/fonts"
fi

# DBus session bus (needed by Qt apps and system tray)
if [ -z "$DBUS_SESSION_BUS_ADDRESS" ]; then
    eval $(dbus-launch --sh-syntax)
    export DBUS_SESSION_BUS_ADDRESS
fi

# Set the display to native resolution before anything starts.
# XLibre / the display manager leaves the screen at 640x480 by default.
# This must run before moonrock starts so the WM initializes at the right size.
#
# The --primary flag is deliberately omitted. MoonRock restores the user's
# persisted primary-output choice from ~/.local/share/moonrock/display-config.conf
# during display_init(), so forcing eDP-1 primary here would clobber that
# state on every login (and regress mirror-mode / external-primary setups
# for users who dock the Legion to an HDMI display).
xrandr --output eDP-1 --mode 1920x1200 --rate 120 2>/dev/null || \
    xrandr --output eDP-1 --mode 1920x1200 2>/dev/null || true

# Display rotation for Lenovo Legion Go (uncomment if needed)
# xrandr --output eDP-1 --rotate right

# ─── Mirror to external display when docked ───
#
# This is the TRANSITIONAL arrangement store for docked-with-external
# sessions. display-config.conf persists the primary-output EDID only —
# it does NOT persist mirror-vs-extend or per-output geometry. Until the
# Displays pane in systemcontrol (slice 55) owns that state per-EDID,
# we default docked-with-external to mirror so the user sees the shell
# immediately on the external screen on every login.
#
# Behavior:
#   - Walk DP-* / HDMI-* outputs; pick the first connected external.
#   - Apply --same-as eDP-1 --primary on that output.
#   - If modes don't match exactly, xrandr will pick the external's
#     preferred mode and align at 0,0. eDP-1 keeps its own mode; the
#     user can tune via xrandr or the Displays pane once it lands.
#   - Silent no-op when undocked (no externals connected).
EXT=""
for o in $(xrandr 2>/dev/null | awk '/^(DP|HDMI)-[0-9]+ connected/ {print $1}'); do
    EXT="$o"
    break
done
if [ -n "$EXT" ]; then
    xrandr --output "$EXT" --auto --same-as eDP-1 --primary 2>/dev/null || true
fi

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
export ICON_THEME=Aqua

# Set the icon theme for Qt/KDE apps via kdeglobals
# This writes to the KDE config so all Qt apps pick it up
mkdir -p ~/.config
if ! grep -q "Aqua" ~/.config/kdeglobals 2>/dev/null; then
    kwriteconfig6 --file kdeglobals --group Icons --key Theme Aqua 2>/dev/null || true
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
gtk-icon-theme-name=Aqua
gtk-theme-name=Adwaita
gtk-application-prefer-dark-theme=false
gtk-font-name=Lucida Grande 11
GTKEOF

# GTK4
mkdir -p ~/.config/gtk-4.0
cp ~/.config/gtk-3.0/settings.ini ~/.config/gtk-4.0/settings.ini

# ─── Start the window manager first ───
# The WM must claim SubstructureRedirect before any other windows appear
WM=${MOONROCK_WM:-moonrock}
$WM &
WM_PID=$!
sleep 0.5

# ─── Compositor for ARGB transparency ───
# MoonRock Compositor is now handling compositing in manual mode.
# picom is disabled — MoonRock draws all windows via OpenGL.
# picom --config ~/CopyCatOS/moonrock/session/picom.conf -b
sleep 0.3

# ─── Desktop surface (wallpaper + icons) ───
# Must come before dock/menubar so it sits at the bottom of the stack
desktop &
sleep 0.2

# ─── Shell components (can start in parallel) ───
menubar &
dock &
searchsystem &

# ─── Input session bridge (talks to inputd, dispatches X11 actions) ───
inputsession &

# ─── Framework host (multi-language app runtime) ───
moonbase &

# ─── Default file viewer window ───
fileviewer ~ &

# ─── Wait for the WM to exit ───
# When the WM process ends (user logged out or crashed), clean up everything
wait $WM_PID

# Kill all shell components on exit
kill $(jobs -p) 2>/dev/null
wait 2>/dev/null
