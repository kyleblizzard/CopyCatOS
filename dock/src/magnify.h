// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// magnify.h — Parabolic icon magnification
//
// When the mouse hovers over the dock, icons near the cursor grow larger
// using a smooth cosine curve. This creates the classic "fish-eye" effect
// where the icon directly under the mouse is biggest, and neighbors smoothly
// shrink back to normal size.
//
// The magnification range is defined by MAGNIFICATION_RANGE (3 icon slots).
// Icons further away than that stay at their base size.
// ============================================================================

#ifndef MAGNIFY_H
#define MAGNIFY_H

#include "dock.h"

// Recalculate the scale factor for every icon based on the current mouse
// position. If the mouse is outside the dock, all scales reset to 1.0.
//
// Parameters:
//   state — the dock's global state (reads mouse_x, mouse_in_dock;
//           writes each item's scale field)
void magnify_update(DockState *state);

#endif // MAGNIFY_H
