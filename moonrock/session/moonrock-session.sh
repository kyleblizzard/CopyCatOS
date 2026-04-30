#!/bin/bash
# CopyCatOS — by Kyle Blizzard at Blizzard.show

# CopyCatOS Session Script
# Starts the custom WM and all shell components in the correct order.
# Set MOONROCK_WM=kwin_x11 to fall back to kwin for testing.

# Tell X11 and Qt what kind of session this is
export XDG_SESSION_TYPE=x11
export XDG_SESSION_DESKTOP=CopyCatOS
export XDG_CURRENT_DESKTOP=CopyCatOS

# Prefer user-local binaries over /usr/local/bin. Every shell component
# (moonrock, menubar, dock, desktop, searchsystem, moonbase, fileviewer,
# inputsession) is resolved via PATH below — prepending ~/.local/bin lets
# `meson setup build --prefix=$HOME/.local && meson install -C build`
# roll a new binary into the session without sudo. A /usr/local/bin
# install still works as a system-wide fallback when ~/.local is empty.
export PATH="$HOME/.local/bin:$PATH"

# ─── Stale-shadow probe ───
# The PATH prepend above is the dev-loop convenience: a fresh
# `meson install --prefix=$HOME/.local` rolls into the session without
# sudo. The flip side: a leftover shadow under ~/.local/bin that lags
# the /usr/local/bin copy will silently run old code — regressions
# hide for hours behind a binary that nobody knew was running. Probe
# every shell component at boot and warn loudly when the user-local
# copy is older than the system copy.
# Non-fatal — the user is informed, the session continues, and the
# warning sticks in logout-trace.log across logins until cleared.
SHADOW_TRACE_FILE="${XDG_STATE_HOME:-$HOME/.local/state}/copycatos/logout-trace.log"
mkdir -p "$(dirname "$SHADOW_TRACE_FILE")"
for b in moonrock desktop menubar dock searchsystem inputsession moonbase fileviewer moonbase-launch; do
    shadow="$HOME/.local/bin/$b"
    system="/usr/local/bin/$b"
    if [ -f "$shadow" ] && [ -f "$system" ] && [ "$system" -nt "$shadow" ]; then
        msg="[shadow-warn] ~/.local/bin/$b is OLDER than /usr/local/bin/$b — session will run the stale shadow"
        echo "$msg" >&2
        echo "$msg at $(date +%s.%N)" >> "$SHADOW_TRACE_FILE"
    fi
done

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
#   - Apply --same-as eDP-1 on that output (mirror at shared origin).
#   - If modes don't match exactly, xrandr will pick the external's
#     preferred mode and align at 0,0. eDP-1 keeps its own mode; the
#     user can tune via xrandr or the Displays pane once it lands.
#   - Silent no-op when undocked (no externals connected).
#
# --primary is deliberately omitted here for the same reason as the eDP-1
# line above: MoonRock restores the user's persisted primary-output choice
# from ~/.local/share/moonrock/display-config.conf during display_init(),
# and slice 55's Displays pane lets the user pick which output owns the
# menu bar. Forcing --primary on the external here would clobber that
# choice on every docked login.
#
# TODO(slice-55 mirror/extend): the --same-as eDP-1 below will stomp any
# future "extend" choice the user makes in the Displays pane until
# display-config.conf persists per-output geometry and moonrock restores
# it on login. Drop this whole block once that lands.
EXT=""
for o in $(xrandr 2>/dev/null | awk '/^(DP|HDMI)-[0-9]+ connected/ {print $1}'); do
    EXT="$o"
    break
done
if [ -n "$EXT" ]; then
    xrandr --output "$EXT" --auto --same-as eDP-1 2>/dev/null || true
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

# ─── Track every shell-component PID we spawn ───
# We don't trust `jobs -p` for cleanup — in a non-interactive bash script
# with job control off, jobs -p has missed children that get re-parented
# (e.g. searchsystem when something else re-spawns it via setsid), and a
# missed child blocks the final `wait` forever, leaving the user staring
# at a black screen after Log Out. An explicit array of every PID we
# launched is the only reliable target list for shutdown.
SHELL_PIDS=()

# ─── Start the window manager first ───
# The WM must claim SubstructureRedirect before any other windows appear
WM=${MOONROCK_WM:-moonrock}
$WM &
WM_PID=$!
SHELL_PIDS+=($WM_PID)
sleep 0.5

# ─── Compositor for ARGB transparency ───
# MoonRock Compositor is now handling compositing in manual mode.
# picom is disabled — MoonRock draws all windows via OpenGL.
# picom --config ~/CopyCatOS/moonrock/session/picom.conf -b
sleep 0.3

# ─── Publish persisted shell-mode atoms before any shell component ───
# systemcontrol owns the ~/.config/copycatos/shell.conf → root-atom mapping
# (_COPYCATOS_MENUBAR_MODE, _COPYCATOS_SPACES_MODE). Without this call the
# atoms stay unset until the user opens System Preferences, and menubar /
# desktop fall through to defaults that may not match the persisted choice.
# --publish-atoms is a headless mode: load conf, write atoms, exit. Failing
# is non-fatal — components default to Modern / per_display when the atoms
# are unset.
systemcontrol --publish-atoms 2>&1 || true

# ─── Desktop surface (wallpaper + icons) ───
# Must come before dock/menubar so it sits at the bottom of the stack
desktop &
SHELL_PIDS+=($!)
sleep 0.2

# ─── Shell components (can start in parallel) ───
menubar &        SHELL_PIDS+=($!)
dock &           SHELL_PIDS+=($!)
searchsystem &   SHELL_PIDS+=($!)

# ─── Input session bridge (talks to inputd, dispatches X11 actions) ───
inputsession &   SHELL_PIDS+=($!)

# ─── Framework host (multi-language app runtime) ───
moonbase &       SHELL_PIDS+=($!)

# ─── Default file viewer window ───
fileviewer ~ &
SHELL_PIDS+=($!)

# ─── Persistent logout trace ───
# xsession-errors is truncated on every new X session, so we mirror
# the trace lines into a stable file under XDG state. Preserved across
# logout/login so we can read what happened on the *previous* session.
TRACE_FILE="${XDG_STATE_HOME:-$HOME/.local/state}/copycatos/logout-trace.log"
mkdir -p "$(dirname "$TRACE_FILE")"
trace() {
    local line="[logout-trace] $* at $(date +%s.%N)"
    echo "$line" >&2
    echo "$line" >> "$TRACE_FILE"
}
trace "----- new session boot $(date -Iseconds) -----"

# ─── Wait for the WM to exit ───
# When the WM process ends (user logged out or crashed), clean up everything
wait $WM_PID
trace "WM_PID exited"

# ─── Cleanup — guaranteed exit so SDDM gets the session back ───
# Send SIGTERM to every PID we launched. Then wait up to ~3 seconds
# for graceful exits, polling every 0.5s. Anything still alive after
# that gets SIGKILL. Finally `exit 0` so this script always returns
# control to the display manager — never block on a misbehaving child.
kill "${SHELL_PIDS[@]}" 2>/dev/null
trace "SIGTERM sent to shell components"
# Poll at 50ms granularity for up to ~2.5s. Half-second polling masked
# fast component exits behind a perceived UI hang during logout —
# components that died at +30ms were still detected as alive until +500ms.
for i in $(seq 1 50); do
    alive_pids=""
    for p in "${SHELL_PIDS[@]}"; do
        if kill -0 "$p" 2>/dev/null; then
            alive_pids="$alive_pids $p"
        fi
    done
    if [ -z "$alive_pids" ]; then
        trace "all shell components exited cleanly at iter $i"
        break
    fi
    [ $((i % 10)) -eq 0 ] && trace "iter $i still alive:$alive_pids"
    sleep 0.05
done
kill -9 "${SHELL_PIDS[@]}" 2>/dev/null
trace "cleanup done — exit 0"
exit 0
