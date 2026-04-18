// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// layout.h — Persistent desktop icon layout
//
// Spatial memory: icons stay exactly where you put them across reboots.
// This is one of the key behavioral differences between Snow Leopard's
// Finder and every modern Linux file manager, which either resets icon
// positions on every login or doesn't support free positioning at all.
//
// Storage format (~/.local/share/copycatos/desktop-layout.ini):
//
//   # CopyCatOS desktop icon layout
//   example.pdf=0:0
//   Projects=0:1
//   README.txt=1:0
//
// Keys are filenames only (not full paths), so the layout survives home
// directory changes. Values are col:row in the icon grid (not pixels),
// so the layout adapts correctly when the screen resolution changes.
//
// Usage:
//   1. layout_load()                        — call once at startup
//   2. layout_apply(icons, count, sw, sh)   — after scan_desktop()
//   3. layout_save_all(icons, count)        — after any position change

#ifndef CC_LAYOUT_H
#define CC_LAYOUT_H

#include "icons.h"

// Load the saved layout from disk into an internal lookup table.
// If the file doesn't exist yet, this is a no-op (all icons will be
// auto-placed by layout_apply on first run).
// Call once at startup before layout_apply().
void layout_load(void);

// Apply saved grid positions to the icon array.
//
// For each icon, we look up its filename in the loaded layout table:
//   - Found: restore its grid_col/grid_row and compute pixel x/y
//   - Not found (new file): auto-place in the next free grid cell,
//     ordered top-right → down → left (Snow Leopard's default order)
//
// screen_w/screen_h are needed to compute pixel positions from grid coords.
void layout_apply(DesktopIcon *icons, int count, int screen_w, int screen_h);

// Save all current icon positions to disk.
// Written atomically: we write to a .tmp file first, then rename()
// it over the real file — so a crash never leaves a corrupted layout.
// Call this after any position change (drag end, clean up, relayout).
void layout_save_all(const DesktopIcon *icons, int count);

#endif // CC_LAYOUT_H
