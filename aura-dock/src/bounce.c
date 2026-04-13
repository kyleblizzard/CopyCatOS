// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// bounce.c — Two-phase sine bounce animation
//
// The bounce animation plays when an app is launched from the dock. It gives
// the user visual feedback that something is happening while the app loads.
//
// Each bounce cycle (720ms) has three phases:
//   Phase 1 (0% - 58%):  Big hop — the icon arcs up to BOUNCE_AMPLITUDE pixels
//   Phase 2 (58% - 88%): Small rebound — a shorter bounce at 45% amplitude
//   Phase 3 (88% - 100%): Rest — the icon sits at its normal position
//
// The cycle repeats until:
//   a) The bounce timeout expires (10 seconds), or
//   b) The process is detected as running (handled in launch.c)
//
// The sine function creates a smooth arc: sin(0)=0, sin(pi/2)=1, sin(pi)=0.
// By mapping each phase to a 0-to-pi range, we get a nice up-and-back motion.
// ============================================================================

#define _GNU_SOURCE  // For M_PI in math.h under strict C11

#include "bounce.h"
#include <math.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Helper: get the current monotonic time in seconds (with fractional part).
// We use CLOCK_MONOTONIC because it's immune to system clock changes —
// wall-clock adjustments won't glitch our animations.
// ---------------------------------------------------------------------------
static double get_time_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

void bounce_start(DockItem *item)
{
    item->bouncing = true;
    item->bounce_offset = 0;
    item->bounce_start_time = get_time_seconds();
}

void bounce_update(DockItem *item, double current_time)
{
    // If this item isn't bouncing, nothing to do
    if (!item->bouncing) return;

    // How many milliseconds have elapsed since the bounce started
    double elapsed = (current_time - item->bounce_start_time) * 1000.0;

    // If we've been bouncing for too long, stop the animation
    if (elapsed > BOUNCE_TIMEOUT_MS) {
        item->bouncing = false;
        item->bounce_offset = 0;
        return;
    }

    // Where are we in the current bounce cycle?
    // cycle_pos goes from 0.0 to 1.0 over each 720ms cycle
    double cycle_pos = fmod(elapsed, BOUNCE_CYCLE_MS) / BOUNCE_CYCLE_MS;

    double offset = 0;

    if (cycle_pos < 0.58) {
        // Phase 1: Big hop
        // Map cycle_pos from [0, 0.58] to [0, 1] for the sine calculation
        double local = cycle_pos / 0.58;
        // sin(pi * local) goes: 0 -> 1 -> 0, creating a smooth arc
        offset = sin(M_PI * local) * BOUNCE_AMPLITUDE;

    } else if (cycle_pos < 0.88) {
        // Phase 2: Small rebound (45% of the big hop's height)
        // Map cycle_pos from [0.58, 0.88] to [0, 1]
        double local = (cycle_pos - 0.58) / 0.30;
        offset = sin(M_PI * local) * BOUNCE_AMPLITUDE * 0.45;

    }
    // Phase 3 (0.88 - 1.0): Rest period — offset stays at 0

    // Negative offset means the icon moves UP (toward the top of the screen)
    item->bounce_offset = -offset;
}

bool bounce_update_all(DockState *state)
{
    double now = get_time_seconds();
    bool any_active = false;

    // Update every item that's currently bouncing
    for (int i = 0; i < state->item_count; i++) {
        if (state->items[i].bouncing) {
            bounce_update(&state->items[i], now);
            if (state->items[i].bouncing) {
                any_active = true;
            }
        }
    }

    // Let the caller know if animations are still running
    state->any_bouncing = any_active;
    return any_active;
}
