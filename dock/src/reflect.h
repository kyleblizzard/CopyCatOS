// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// reflect.h — Icon reflections
//
// Each icon in the dock has a faint reflection drawn below it, simulating
// the glossy surface of the glass shelf. The reflection is a vertically
// flipped copy of the icon that fades from ~35% opacity at the top to
// fully transparent at the bottom. The reflection height is 35% of the
// icon's current displayed size.
// ============================================================================

#ifndef REFLECT_H
#define REFLECT_H

#include "dock.h"

// Draw a reflection of the given icon below it on the shelf surface.
//
// Parameters:
//   cr          — the Cairo drawing context
//   icon        — the icon surface to reflect
//   x           — left edge X coordinate of the icon
//   y           — top edge Y coordinate of the icon (the reflection is drawn below)
//   icon_size   — the current rendered size of the icon (after magnification)
void reflect_draw(cairo_t *cr, cairo_surface_t *icon, double x, double y, double icon_size);

#endif // REFLECT_H
