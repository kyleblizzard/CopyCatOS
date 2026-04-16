#!/bin/bash
# Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
# Build all CopyCatOS components in order.
# Run from the project root: bash scripts/build-all.sh

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

build_component() {
    local name="$1"
    local dir="$ROOT/$name"

    if [ ! -f "$dir/meson.build" ]; then
        echo -e "${YELLOW}[SKIP]${NC} $name — no meson.build found"
        return 0
    fi

    echo -e "${GREEN}[BUILD]${NC} $name"

    # Set up build directory if it doesn't exist
    if [ ! -d "$dir/build" ]; then
        meson setup "$dir/build" "$dir" || {
            echo -e "${RED}[FAIL]${NC} $name — meson setup failed"
            return 1
        }
    fi

    # Compile
    meson compile -C "$dir/build" || {
        echo -e "${RED}[FAIL]${NC} $name — compilation failed"
        return 1
    }

    echo -e "${GREEN}[DONE]${NC} $name"
}

echo "════════════════════════════════════════"
echo "  CopyCatOS — Building All Components"
echo "════════════════════════════════════════"
echo ""

# Build order: WM first, then shell components
COMPONENTS=(
    "cc-wm"
    "cc-desktop"
    "cc-menubar"
    "cc-dock"
    "cc-spotlight"
    "cc-inputd"
    "cc-input-session"
    "cc-sysprefs"
)

FAILED=0
for comp in "${COMPONENTS[@]}"; do
    build_component "$comp" || FAILED=$((FAILED + 1))
    echo ""
done

# Summary
echo "════════════════════════════════════════"
if [ $FAILED -eq 0 ]; then
    echo -e "  ${GREEN}All ${#COMPONENTS[@]} components built successfully${NC}"
else
    echo -e "  ${RED}$FAILED of ${#COMPONENTS[@]} components failed${NC}"
fi
echo "════════════════════════════════════════"

# Install helper scripts to ~/.local/bin so they're on PATH alongside
# the cc-* binaries. This makes cc-game-mode available for the Apple menu
# action in cc-menubar to launch without needing an absolute path.
echo ""
echo "Installing scripts to ~/.local/bin..."
mkdir -p "$HOME/.local/bin"
cp "$ROOT/scripts/game-mode.sh" "$HOME/.local/bin/cc-game-mode"
chmod +x "$HOME/.local/bin/cc-game-mode"
echo -e "${GREEN}[DONE]${NC} cc-game-mode installed"

exit $FAILED
