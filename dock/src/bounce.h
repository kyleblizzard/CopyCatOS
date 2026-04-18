// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// bounce.h — Two-phase sine bounce animation
//
// When an app is launched from the dock, its icon bounces up and down to
// give visual feedback. The animation has two phases:
//   1. A big hop (58% of the cycle) — the icon arcs up high
//   2. A small rebound (30% of the cycle) — a shorter secondary bounce
//   3. A brief rest (12% of the cycle) — the icon sits still
//
// This cycle repeats until either the app window appears (process detected
// as running) or the bounce timeout (10 seconds) expires.
// ============================================================================

#ifndef BOUNCE_H
#define BOUNCE_H

#include "dock.h"

// Start the bounce animation for a specific dock item.
// Records the current time as the animation start time.
void bounce_start(DockItem *item);

// Update the bounce offset for a single item based on the current time.
// This is called every animation frame (~16ms). It calculates where the
// icon should be in the bounce cycle and sets item->bounce_offset.
// If the timeout has expired, it stops the animation.
//
// Parameters:
//   item         — the dock item to update
//   current_time — current monotonic time in seconds (from clock_gettime)
void bounce_update(DockState *state, DockItem *item, double current_time);

// Update all bouncing items in the dock. Returns true if any item is
// still bouncing (meaning we need to keep the animation timer running).
bool bounce_update_all(DockState *state);

#endif // BOUNCE_H
