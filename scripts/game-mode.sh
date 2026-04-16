#!/bin/bash
# Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
# This code is publicly visible for portfolio purposes only.
# Unauthorized copying, forking, or distribution of this file,
# via any medium, is strictly prohibited.

# game-mode.sh — Switch from CopyCatOS desktop into Steam Big Picture via gamescope.
#
# Flow:
#   1. Wait briefly so the Apple menu has time to close cleanly.
#   2. Kill the four CopyCatOS shell components (menubar, dock, desktop, spotlight).
#      cc-wm is intentionally left alive — killing it would trigger cc-session.sh's
#      cleanup and log out the user entirely.
#   3. Launch gamescope in embedded mode (-e) so it runs inside the existing X session,
#      with Steam in gamepadui (Big Picture) mode as the hosted application.
#   4. Block here until gamescope exits (user selects "Exit to Desktop" in Steam).
#   5. Relaunch all shell components to restore the CopyCatOS desktop.

# ── Marker file path ─────────────────────────────────────────────────
# cc-inputd reads this file to detect game mode. When it exists, a power
# button short press exits gamescope (returning to desktop) instead of
# suspending the system.
MARKER_DIR="$HOME/.local/share/copycatos"
MARKER_FILE="$MARKER_DIR/gamemode.active"

# ── Step 1: Let the Apple menu dismiss ───────────────────────────────
sleep 0.5

echo "[game-mode] Entering Game Mode — suspending CopyCatOS shell..."

# ── Step 2: Kill shell components (not cc-wm) ────────────────────────
# We use pkill -x to match the exact process name only.
# If a component isn't running (e.g. cc-spotlight not started), pkill just returns
# a non-zero exit code — the 2>/dev/null suppresses the noise.
pkill -x cc-menubar  2>/dev/null
pkill -x cc-dock     2>/dev/null
pkill -x cc-desktop  2>/dev/null
pkill -x cc-spotlight 2>/dev/null

# Give them time to exit and release their X resources (struts, dock window, etc.)
sleep 0.4

# Write the game mode marker so cc-inputd knows we're in game mode.
# The file's presence is the signal — the content doesn't matter.
mkdir -p "$MARKER_DIR"
echo "$$" > "$MARKER_FILE"   # Store our PID for debugging
echo "[game-mode] Marker written: $MARKER_FILE"

echo "[game-mode] Shell components stopped. Launching gamescope + Steam..."

# ── Step 3 & 4: Run gamescope with Steam ─────────────────────────────
#
# Flags:
#   -e            Embedded mode — gamescope runs as a window inside the current
#                 X session instead of creating its own display. This means cc-wm
#                 is still in control of the X session and we can return cleanly.
#
#   -W / -H       Output resolution (what gamescope renders to the screen).
#                 Set to the Lenovo Legion Go's native display: 2560x1600.
#
#   -w / -h       Game/content resolution. Same as output so Steam fills the screen.
#
#   -f            Start in fullscreen mode.
#
#   --             Everything after this is the command gamescope will host.
#
#   steam         The Steam client binary (must be on PATH).
#   -gamepadui    Launch Steam in Big Picture / Game Mode UI instead of the
#                 desktop client. This is the same mode used on Steam Deck.
#   -pipewire-dmabuf  Use PipeWire's DMA-BUF path for audio/video — required
#                 for correct audio on Nobara/PipeWire systems.
#
# This call blocks until gamescope exits (i.e., the user picks "Exit to Desktop"
# inside Steam's Big Picture UI, or Steam crashes).
gamescope \
    -e \
    -W 2560 -H 1600 \
    -w 2560 -h 1600 \
    -f \
    -- \
    steam -gamepadui -pipewire-dmabuf

EXIT_CODE=$?
echo "[game-mode] gamescope exited with code $EXIT_CODE. Restoring CopyCatOS desktop..."

# Remove the game mode marker so cc-inputd resumes normal power button behavior
# (short press = suspend) now that we're back at the desktop.
rm -f "$MARKER_FILE"
echo "[game-mode] Marker removed."

# ── Step 5: Restore the CopyCatOS desktop ────────────────────────────
# Restart all four shell components in the same order as cc-session.sh:
# desktop first (it sits at the bottom of the window stack), then the
# others in parallel since they don't depend on each other.
sleep 0.3

cc-desktop &
sleep 0.2

cc-menubar   &
cc-dock      &
cc-spotlight &

echo "[game-mode] Desktop restored."
