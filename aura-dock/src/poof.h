// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// poof.h — "Poof" cloud animation for removing dock icons
//
// When a user drags an icon out of the dock and releases it, a small cloud
// explosion plays at the release point — this is the classic macOS "poof"
// effect. The animation uses a 5-frame sprite sheet (poof.png) and plays
// over ~150ms total.
//
// Usage from other modules:
//   1. Call poof_load() once at startup to load the sprite sheet.
//   2. Call poof_start() when a drag ends outside the dock.
//   3. Call poof_update() each frame in the event loop to advance the animation.
//   4. Call poof_is_active() to check if the animation is still playing
//      (used to keep the event loop's select() timeout short).
//   5. Call poof_cleanup() on shutdown to free resources.
// ============================================================================

#ifndef DOCK_POOF_H
#define DOCK_POOF_H

#include "dock.h"

// Load the poof sprite sheet from assets. Call once at startup.
// Returns true if the sprite sheet was found and loaded successfully.
bool poof_load(DockState *state);

// Start the poof animation at the given screen coordinates.
// Creates a temporary override-redirect window and begins frame playback.
// The animation will be centered on (screen_x, screen_y).
void poof_start(DockState *state, int screen_x, int screen_y);

// Advance the animation by one frame if enough time has elapsed.
// Returns true if the animation is still running, false if finished.
// Call this every iteration of the event loop.
bool poof_update(DockState *state);

// Check if a poof animation is currently playing.
// Use this to decide whether select() should use a short timeout.
bool poof_is_active(void);

// Clean up all poof resources (sprite sheet, temporary window, Cairo state).
// Call this during dock shutdown.
void poof_cleanup(DockState *state);

#endif // DOCK_POOF_H
