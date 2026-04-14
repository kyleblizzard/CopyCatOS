// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// shelf.h — Glass shelf rendering
//
// The shelf is the translucent glass platform that sits at the bottom of the
// dock, beneath the icons. It's the signature visual element of the Snow
// Leopard dock — a 3D-looking trapezoid with a glossy highlight on top.
//
// The shelf image (scurve-xl.png) is loaded once and drawn every frame,
// stretched to fit the current dock width. A frontline highlight is drawn
// along the top edge, and vertical separators mark section boundaries.
// ============================================================================

#ifndef SHELF_H
#define SHELF_H

#include "dock.h"

// Load the shelf and frontline images from disk.
// Must be called once during dock initialization.
// Returns true if assets loaded successfully.
bool shelf_load_assets(DockState *state);

// Draw the glass shelf at the bottom of the dock.
// Parameters:
//   state       — the dock's global state (for Cairo context and dimensions)
//   shelf_width — how wide the shelf should be (based on icon row width + padding)
void shelf_draw(DockState *state, int shelf_width);

// Draw separator lines between dock sections.
// Called after drawing icons for items that have separator_after = true.
// Parameters:
//   state — the dock's global state
//   x     — the X coordinate where the separator should be drawn
void shelf_draw_bottom_band(DockState *state, int shelf_width);
void shelf_draw_separator(DockState *state, double x);

// Free the loaded shelf image surfaces.
void shelf_cleanup(DockState *state);

#endif // SHELF_H
