#!/bin/bash
# CopyCatOS — by Kyle Blizzard at Blizzard.show

#
# sync-moonrock.sh — Copy MoonRock compositor sources from CopyCatOS to the
# standalone MoonRock repo. No git operations — just the file copy.
#
# Usage: ./scripts/sync-moonrock.sh
#
# CopyCatOS/moonrock/src/ is the authoritative source for all moonrock_* files.
# The standalone MoonRock repo at /Volumes/Development/MoonRock/ gets synced
# from here whenever changes are ready to publish.

set -euo pipefail

# Paths
SRC_DIR="$(cd "$(dirname "$0")/../moonrock/src" && pwd)"
DST_DIR="/Volumes/Development/MoonRock/src"

if [ ! -d "$SRC_DIR" ]; then
    echo "ERROR: Source directory not found: $SRC_DIR"
    exit 1
fi

if [ ! -d "$DST_DIR" ]; then
    echo "ERROR: MoonRock repo not found: $DST_DIR"
    echo "       Expected at /Volumes/Development/MoonRock/"
    exit 1
fi

echo "Syncing MoonRock sources:"
echo "  From: $SRC_DIR"
echo "  To:   $DST_DIR"
echo ""

count=0
for f in "$SRC_DIR"/moonrock_*.c "$SRC_DIR"/moonrock_*.h; do
    if [ -f "$f" ]; then
        basename="$(basename "$f")"
        cp "$f" "$DST_DIR/$basename"
        echo "  Copied: $basename"
        count=$((count + 1))
    fi
done

# Also copy the main moonrock.c and moonrock.h
for f in moonrock.c moonrock.h wm_compat.h; do
    if [ -f "$SRC_DIR/$f" ]; then
        cp "$SRC_DIR/$f" "$DST_DIR/$f"
        echo "  Copied: $f"
        count=$((count + 1))
    fi
done

echo ""
echo "Done: $count files copied."
echo ""

# shell-api/ — the tiny subscriber helpers shell components (menubar, dock,
# desktop, systemcontrol) link against to read MoonRock's per-output scale
# atom. Canonical source lives in CopyCatOS; mirror it into the standalone
# MoonRock repo so the pair of files is discoverable next to the compositor.
SHELL_SRC="$(cd "$(dirname "$0")/../moonrock/shell-api" && pwd)"
SHELL_DST="/Volumes/Development/MoonRock/shell-api"

if [ -d "$SHELL_SRC" ]; then
    mkdir -p "$SHELL_DST"
    shell_count=0
    echo "Syncing shell-api:"
    echo "  From: $SHELL_SRC"
    echo "  To:   $SHELL_DST"
    for f in "$SHELL_SRC"/*.c "$SHELL_SRC"/*.h; do
        if [ -f "$f" ]; then
            basename="$(basename "$f")"
            cp "$f" "$SHELL_DST/$basename"
            echo "  Copied: $basename"
            shell_count=$((shell_count + 1))
        fi
    done
    echo "  shell-api files copied: $shell_count"
    echo ""
fi

# MoonRock is BSD-3-Clause (open source). Source files in CopyCatOS use either
# the old ARR block (legacy moonrock core files) or the one-line per-file
# header "// CopyCatOS — by Kyle Blizzard at Blizzard.show" (newer files,
# including shell-api). Convert both into the BSD-3 block used by the
# standalone MoonRock repo.
echo "Fixing copyright headers (CopyCatOS → MoonRock BSD-3-Clause)..."

python3 - "$DST_DIR" "$SHELL_DST" <<'PYEOF'
import sys, os

dirs = [d for d in sys.argv[1:] if d and os.path.isdir(d)]

OLD_ARR = (
    "// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.\n"
    "// This code is publicly visible for portfolio purposes only.\n"
    "// Unauthorized copying, forking, or distribution of this file,\n"
    "// via any medium, is strictly prohibited."
)

# Consume the blank line that always follows the one-liner in CopyCatOS,
# so the MoonRock BSD-3 block sits flush against the next comment block —
# matching the layout every legacy ARR file already had after conversion.
OLD_ONELINE = "// CopyCatOS — by Kyle Blizzard at Blizzard.show\n\n"
ONELINE_REPLACEMENT_SUFFIX = "\n"

NEW = (
    "// Copyright (c) 2026 Kyle Blizzard\n"
    "// SPDX-License-Identifier: BSD-3-Clause\n"
    "//\n"
    "// MoonRock was created by Kyle Blizzard. Feel free to use it and improve it!\n"
    "// www.blizzard.show/moonrock/"
)

for src_dir in dirs:
    for fname in sorted(os.listdir(src_dir)):
        if not fname.endswith(('.c', '.h')):
            continue
        path = os.path.join(src_dir, fname)
        content = open(path).read()
        changed = False
        if OLD_ARR in content:
            content = content.replace(OLD_ARR, NEW)
            changed = True
        elif OLD_ONELINE in content:
            content = content.replace(OLD_ONELINE, NEW + ONELINE_REPLACEMENT_SUFFIX)
            changed = True
        # Remove CopyCatOS-specific branding that doesn't belong in standalone MoonRock
        content = content.replace(
            "MoonRock Compositor — CopyCatOS's custom OpenGL X11 compositor",
            "MoonRock Compositor — custom OpenGL X11 compositor"
        )
        content = content.replace(
            "CopyCatOS aesthetic of recreating the Snow Leopard look",
            "MoonRock aesthetic of recreating the Snow Leopard look"
        )
        if changed:
            open(path, 'w').write(content)
            print(f"  Fixed: {os.path.relpath(path, os.path.dirname(src_dir))}")

PYEOF

echo ""
echo "Headers fixed. Commit in the MoonRock repo when ready."
