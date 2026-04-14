#!/bin/bash
# Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
# This code is publicly visible for portfolio purposes only.
# Unauthorized copying, forking, or distribution of this file,
# via any medium, is strictly prohibited.
#
# build-appimage.sh — Package a Qt6 application as an AppImage with CopiCatOS
# theming baked in (AquaStyle plugin, Lucida Grande font, Aqua widget assets).
#
# Usage:
#   ./build-appimage.sh <app-name> <path-to-binary>
#
# Example:
#   ./build-appimage.sh kate /usr/bin/kate
#
# This is a TEMPLATE script. Not every Qt6 app will work out of the box —
# some may need additional plugins, libraries, or data files. But this handles
# the common case: a single binary that uses Qt6 widgets and needs CopiCatOS styling.

set -euo pipefail

# ── Color helpers for terminal output ──────────────────────────────────────

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# ── Argument parsing ──────────────────────────────────────────────────────

if [ $# -lt 2 ]; then
    echo -e "${RED}Usage:${NC} $0 <app-name> <path-to-binary>"
    echo ""
    echo "  app-name       Friendly name for the AppImage (e.g. kate, dolphin)"
    echo "  path-to-binary Full path to the installed binary (e.g. /usr/bin/kate)"
    exit 1
fi

APP_NAME="$1"        # Human-readable name, used for the .desktop file and output filename
APP_BINARY="$2"      # Absolute path to the binary we're packaging
APP_BIN_NAME="$(basename "$APP_BINARY")"  # Just the filename (e.g. "kate")

# ── Locate the CopiCatOS project root ────────────────────────────────────────
# We assume this script lives in <project-root>/scripts/ and derive paths
# to fonts, icons, and style assets relative to the project root.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Validate inputs ──────────────────────────────────────────────────────

if [ ! -f "$APP_BINARY" ]; then
    echo -e "${RED}[ERROR]${NC} Binary not found: $APP_BINARY"
    exit 1
fi

# ── Locate the AquaStyle plugin (.so) ────────────────────────────────────
# First check the local breeze-aqua build directory, then fall back to the
# system-wide Qt6 plugin path. The plugin file is named breezeaqua6.so.

AQUA_STYLE_SO=""

# Check local build output first
if [ -f "$PROJECT_ROOT/breeze-aqua/build/kstyle/breezeaqua6.so" ]; then
    AQUA_STYLE_SO="$PROJECT_ROOT/breeze-aqua/build/kstyle/breezeaqua6.so"
# Check system Qt6 plugin directory
elif [ -f "/usr/lib/qt6/plugins/styles/breezeaqua6.so" ]; then
    AQUA_STYLE_SO="/usr/lib/qt6/plugins/styles/breezeaqua6.so"
elif [ -f "/usr/lib64/qt6/plugins/styles/breezeaqua6.so" ]; then
    AQUA_STYLE_SO="/usr/lib64/qt6/plugins/styles/breezeaqua6.so"
else
    echo -e "${YELLOW}[WARN]${NC} Could not find breezeaqua6.so — AppImage will lack AquaStyle."
    echo "       Build breeze-aqua first: cd breeze-aqua && mkdir -p build && cd build && cmake .. && make"
fi

# ── Set up the AppDir structure ──────────────────────────────────────────
# AppDir is the staging directory that appimagetool will pack into the final
# .AppImage file. It mirrors a minimal Linux filesystem with our app,
# libraries, fonts, and theme assets.

APPDIR="$(pwd)/${APP_NAME}.AppDir"

echo -e "${GREEN}[INFO]${NC} Creating AppDir at $APPDIR"

# Clean any previous build
rm -rf "$APPDIR"

# Create the directory skeleton
mkdir -p "$APPDIR/usr/bin"
mkdir -p "$APPDIR/usr/lib/qt6/plugins/styles"
mkdir -p "$APPDIR/usr/share/fonts"
mkdir -p "$APPDIR/usr/share/aqua-scrollbar"
mkdir -p "$APPDIR/usr/share/aqua-controls"
mkdir -p "$APPDIR/usr/share/icons"
mkdir -p "$APPDIR/usr/etc/fonts"

# ── Step 1: Copy the application binary ──────────────────────────────────

echo -e "${GREEN}[COPY]${NC} Application binary: $APP_BINARY"
cp "$APP_BINARY" "$APPDIR/usr/bin/$APP_BIN_NAME"
chmod +x "$APPDIR/usr/bin/$APP_BIN_NAME"

# ── Step 2: Analyze shared library dependencies ─────────────────────────
# Use ldd to find which Qt6 libraries the binary links against. We copy
# these into the AppImage so it doesn't depend on the host's Qt6 install.
#
# NOTE: This is a best-effort approach. Some apps load plugins at runtime
# (not visible to ldd). You may need to add extra libraries manually.

echo -e "${GREEN}[INFO]${NC} Checking shared library dependencies with ldd..."
echo ""

# Extract all Qt6 and KF6 library paths that the binary depends on
QT_LIBS=$(ldd "$APP_BINARY" 2>/dev/null | grep -E '(libQt6|libKF6)' | awk '{print $3}' | sort -u || true)

if [ -n "$QT_LIBS" ]; then
    echo -e "${GREEN}[COPY]${NC} Bundling Qt6/KF6 libraries:"
    while IFS= read -r lib; do
        if [ -f "$lib" ]; then
            echo "       -> $(basename "$lib")"
            cp "$lib" "$APPDIR/usr/lib/"
        fi
    done <<< "$QT_LIBS"
    echo ""
else
    echo -e "${YELLOW}[WARN]${NC} No Qt6/KF6 libraries detected via ldd."
    echo "       The binary may be statically linked, or ldd may not have access."
    echo "       You may need to manually copy required .so files into $APPDIR/usr/lib/"
    echo ""
fi

# ── Step 3: Copy the AquaStyle plugin ───────────────────────────────────
# This is the Qt6 style plugin that makes apps look like Snow Leopard.

if [ -n "$AQUA_STYLE_SO" ]; then
    echo -e "${GREEN}[COPY]${NC} AquaStyle plugin: $AQUA_STYLE_SO"
    cp "$AQUA_STYLE_SO" "$APPDIR/usr/lib/qt6/plugins/styles/breezeaqua6.so"
fi

# ── Step 4: Copy Lucida Grande font ─────────────────────────────────────
# Snow Leopard's system font. We bundle it so apps render with the correct
# typeface regardless of what's installed on the host.

FONT_SRC="$PROJECT_ROOT/fonts/LucidaGrande.ttc"
if [ -f "$FONT_SRC" ]; then
    echo -e "${GREEN}[COPY]${NC} Lucida Grande font"
    cp "$FONT_SRC" "$APPDIR/usr/share/fonts/LucidaGrande.ttc"
else
    echo -e "${YELLOW}[WARN]${NC} Lucida Grande font not found at $FONT_SRC"
fi

# ── Step 5: Copy Aqua scrollbar assets ──────────────────────────────────
# These are the pixel-perfect scrollbar track and thumb images used by the
# AquaStyle plugin to draw Snow Leopard-style scrollbars.

SCROLLBAR_SRC="$PROJECT_ROOT/breeze-aqua/kstyle/aqua-assets"
if [ -d "$SCROLLBAR_SRC" ]; then
    echo -e "${GREEN}[COPY]${NC} Aqua scrollbar assets"
    cp -r "$SCROLLBAR_SRC"/* "$APPDIR/usr/share/aqua-scrollbar/"
else
    echo -e "${YELLOW}[WARN]${NC} Aqua scrollbar assets not found at $SCROLLBAR_SRC"
fi

# ── Step 6: Copy Aqua control assets from SnowReverseOutput ────────────
# Additional widget textures (buttons, checkboxes, etc.) reverse-engineered
# from Snow Leopard's Aqua interface.

CONTROLS_SRC="$PROJECT_ROOT/snowleopardaura/MacAssets"
if [ -d "$CONTROLS_SRC" ]; then
    echo -e "${GREEN}[COPY]${NC} Aqua control assets (MacAssets)"
    cp -r "$CONTROLS_SRC"/* "$APPDIR/usr/share/aqua-controls/"
else
    echo -e "${YELLOW}[WARN]${NC} Aqua control assets not found at $CONTROLS_SRC"
fi

# ── Step 7: Copy AquaKDE icon theme ────────────────────────────────────
# The icon theme styled to match Snow Leopard's Aqua look.

ICONS_SRC="$PROJECT_ROOT/icons/AquaKDE-icons"
if [ -d "$ICONS_SRC" ]; then
    echo -e "${GREEN}[COPY]${NC} AquaKDE icon theme"
    cp -r "$ICONS_SRC" "$APPDIR/usr/share/icons/"
else
    echo -e "${YELLOW}[WARN]${NC} AquaKDE-icons not found at $ICONS_SRC"
fi

# ── Step 8: Generate minimal fonts.conf ─────────────────────────────────
# This fontconfig configuration tells the app to look for fonts inside the
# AppImage first, and to prefer Lucida Grande as the default sans-serif font.

echo -e "${GREEN}[GEN ]${NC} Generating fonts.conf"
cat > "$APPDIR/usr/etc/fonts/fonts.conf" << 'FONTCONF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<!--
  Minimal fontconfig for CopiCatOS AppImage.
  Prioritizes the bundled Lucida Grande font so apps render with the correct
  Snow Leopard typeface, even if the host system doesn't have it installed.
-->
<fontconfig>
  <!-- Only search our bundled fonts directory -->
  <dir prefix="relative">../../share/fonts</dir>

  <!-- Also fall back to system fonts so we don't break non-Latin text -->
  <dir>/usr/share/fonts</dir>
  <dir>/usr/local/share/fonts</dir>
  <dir prefix="xdg">fonts</dir>

  <!-- Make Lucida Grande the preferred sans-serif font -->
  <alias>
    <family>sans-serif</family>
    <prefer>
      <family>Lucida Grande</family>
    </prefer>
  </alias>

  <!-- Also map "system-ui" and "Helvetica" to Lucida Grande, since many -->
  <!-- apps use those as fallback font family names -->
  <alias>
    <family>Helvetica</family>
    <prefer>
      <family>Lucida Grande</family>
    </prefer>
  </alias>
  <alias>
    <family>system-ui</family>
    <prefer>
      <family>Lucida Grande</family>
    </prefer>
  </alias>
</fontconfig>
FONTCONF

# ── Step 9: Generate the .desktop file ──────────────────────────────────
# Every AppImage needs a .desktop file at the root of AppDir. This tells
# the system (and AppImage tools) what the app is called and how to run it.

echo -e "${GREEN}[GEN ]${NC} Generating app.desktop"
cat > "$APPDIR/app.desktop" << DESKTOP
[Desktop Entry]
Name=${APP_NAME}
Exec=${APP_BIN_NAME}
Icon=app
Type=Application
Categories=Utility;
Comment=CopiCatOS-themed ${APP_NAME}
DESKTOP

# ── Step 10: Generate a placeholder icon ────────────────────────────────
# AppImage requires an icon at the AppDir root. If the app ships its own
# icon, replace this placeholder with the real one.

if [ ! -f "$APPDIR/app.png" ]; then
    echo -e "${YELLOW}[NOTE]${NC} No app icon provided — creating a 1x1 placeholder PNG."
    echo "       Replace $APPDIR/app.png with a real icon for production use."
    # Minimal valid 1x1 red PNG (67 bytes) — just enough to satisfy appimagetool
    printf '\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x02\x00\x00\x00\x90wS\xde\x00\x00\x00\x0cIDATx\x9cc\xf8\x0f\x00\x00\x01\x01\x00\x05\x18\xd8N\x00\x00\x00\x00IEND\xaeB`\x82' > "$APPDIR/app.png"
fi

# ── Step 11: Generate the AppRun entry point ────────────────────────────
# AppRun is the script that runs when the user launches the .AppImage.
# It sets up environment variables so the bundled Qt plugins, fonts, icons,
# and libraries take priority over whatever is installed on the host.

echo -e "${GREEN}[GEN ]${NC} Generating AppRun"
cat > "$APPDIR/AppRun" << APPRUN
#!/bin/bash
# AppRun — Entry point for the CopiCatOS-themed ${APP_NAME} AppImage.
# Sets up paths to bundled Qt plugins, fonts, icons, and libraries
# so the app uses AquaStyle and Lucida Grande regardless of the host system.

HERE="\$(dirname "\$(readlink -f "\$0")")"

# Tell Qt6 to find our bundled style plugin (AquaStyle / breezeaqua6.so)
export QT_PLUGIN_PATH="\$HERE/usr/lib/qt6/plugins"

# Force the AquaStyle plugin as the active widget style
export QT_STYLE_OVERRIDE=AquaStyle

# Add our share directory to the data search path so apps can find
# bundled icons, themes, and other resources
export XDG_DATA_DIRS="\$HERE/usr/share:\$XDG_DATA_DIRS"

# Point fontconfig at our minimal config that prioritizes Lucida Grande
export FONTCONFIG_FILE="\$HERE/usr/etc/fonts/fonts.conf"

# Ensure bundled shared libraries are found before system ones
export LD_LIBRARY_PATH="\$HERE/usr/lib:\$LD_LIBRARY_PATH"

# Launch the actual application, passing through any command-line arguments
exec "\$HERE/usr/bin/${APP_BIN_NAME}" "\$@"
APPRUN
chmod +x "$APPDIR/AppRun"

# ── Step 12: Build the AppImage ─────────────────────────────────────────
# appimagetool takes the AppDir and packs it into a single self-mounting
# executable .AppImage file. If the tool isn't installed, we print
# instructions instead of failing hard.

echo ""
echo -e "${GREEN}════════════════════════════════════════${NC}"
echo -e "${GREEN}  AppDir created: $APPDIR${NC}"
echo -e "${GREEN}════════════════════════════════════════${NC}"
echo ""

if command -v appimagetool &> /dev/null; then
    OUTPUT_FILE="${APP_NAME}-CopiCatOS-$(uname -m).AppImage"
    echo -e "${GREEN}[BUILD]${NC} Running appimagetool..."
    appimagetool "$APPDIR" "$OUTPUT_FILE"
    echo ""
    echo -e "${GREEN}[DONE]${NC} AppImage created: $OUTPUT_FILE"
    echo "       Run it with: ./$OUTPUT_FILE"
else
    echo -e "${YELLOW}[NOTE]${NC} appimagetool is not installed."
    echo ""
    echo "  To install it:"
    echo "    wget https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$(uname -m).AppImage"
    echo "    chmod +x appimagetool-$(uname -m).AppImage"
    echo "    sudo mv appimagetool-$(uname -m).AppImage /usr/local/bin/appimagetool"
    echo ""
    echo "  Then re-run this script, or manually run:"
    echo "    appimagetool $APPDIR ${APP_NAME}-CopiCatOS-$(uname -m).AppImage"
fi
