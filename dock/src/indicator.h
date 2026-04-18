// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// indicator.h — Running-app indicator dots
//
// When an app in the dock is currently running, a small glowing dot appears
// centered beneath its icon. This is the classic macOS dock indicator.
//
// The indicator is either loaded from an image file (indicator_medium.png)
// or drawn procedurally as a radial gradient (white center -> aqua blue ->
// transparent). It's 8px in diameter and positioned 4px from the dock bottom.
// ============================================================================

#ifndef INDICATOR_H
#define INDICATOR_H

#include "dock.h"

// Load the indicator image from disk, or create a procedural one if the
// image file is not found. Must be called during dock initialization.
// Returns true on success.
bool indicator_load(DockState *state);

// Draw an indicator dot centered at the given X position, near the bottom
// of the dock window. Only call this for items where item->running is true.
//
// Parameters:
//   state    — the dock's global state (for the Cairo context)
//   center_x — the X coordinate to center the dot on
void indicator_draw(DockState *state, double center_x);

// Free the indicator surface.
void indicator_cleanup(DockState *state);

#endif // INDICATOR_H
