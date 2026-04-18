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

# MoonRock is BSD-3-Clause (open source). The source files in moonrock use the
# CopyCatOS All Rights Reserved header — strip that and replace it with the
# correct BSD-3-Clause header so MoonRock stays openly licensed.
echo "Fixing copyright headers (CopyCatOS All Rights Reserved → MoonRock BSD-3-Clause)..."

python3 - "$DST_DIR" <<'PYEOF'
import sys, os

src_dir = sys.argv[1]

OLD = (
    "// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.\n"
    "// This code is publicly visible for portfolio purposes only.\n"
    "// Unauthorized copying, forking, or distribution of this file,\n"
    "// via any medium, is strictly prohibited."
)

NEW = (
    "// Copyright (c) 2026 Kyle Blizzard\n"
    "// SPDX-License-Identifier: BSD-3-Clause\n"
    "//\n"
    "// MoonRock was created by Kyle Blizzard. Feel free to use it and improve it!\n"
    "// www.blizzard.show/moonrock/"
)

for fname in sorted(os.listdir(src_dir)):
    if not fname.endswith(('.c', '.h')):
        continue
    path = os.path.join(src_dir, fname)
    content = open(path).read()
    if OLD not in content:
        continue
    content = content.replace(OLD, NEW)
    # Remove CopyCatOS-specific branding that doesn't belong in standalone MoonRock
    content = content.replace(
        "MoonRock Compositor — CopyCatOS's custom OpenGL X11 compositor",
        "MoonRock Compositor — custom OpenGL X11 compositor"
    )
    content = content.replace(
        "CopyCatOS aesthetic of recreating the Snow Leopard look",
        "MoonRock aesthetic of recreating the Snow Leopard look"
    )
    open(path, 'w').write(content)
    print(f"  Fixed: {fname}")

PYEOF

echo ""
echo "Headers fixed. Commit in the MoonRock repo when ready."
