#!/bin/bash
# Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
# This code is publicly visible for portfolio purposes only.
# Unauthorized copying, forking, or distribution of this file,
# via any medium, is strictly prohibited.
#
# sync-moonrock.sh — Copy MoonRock compositor sources from CopiCatOS to the
# standalone MoonRock repo. No git operations — just the file copy.
#
# Usage: ./scripts/sync-moonrock.sh
#
# CopiCatOS/cc-wm/src/ is the authoritative source for all moonrock_* files.
# The standalone MoonRock repo at /Volumes/Development/MoonRock/ gets synced
# from here whenever changes are ready to publish.

set -euo pipefail

# Paths
SRC_DIR="$(cd "$(dirname "$0")/../cc-wm/src" && pwd)"
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
echo "Remember to update copyright headers and commit in the MoonRock repo."
