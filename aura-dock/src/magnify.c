// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// magnify.c — Parabolic icon magnification
//
// Implements the classic macOS dock "fish-eye" effect. As the mouse moves
// along the dock, icons near the cursor grow larger. The size boost follows
// a cosine curve, which gives a smooth, natural-looking falloff:
//   - Icon directly under mouse: maximum size (MAX_ICON_SIZE)
//   - Icons further away: smoothly shrink back to BASE_ICON_SIZE
//   - Icons beyond MAGNIFICATION_RANGE slots away: normal size
//
// The cosine function is key here — it creates a bell-shaped curve that
// looks much better than a linear falloff. The formula:
//   boost = max_boost * (1 + cos(pi * distance_ratio)) / 2
// gives 100% boost at distance=0 and 0% boost at distance=range.
// ============================================================================

#include "magnify.h"
#include <math.h>

void magnify_update(DockState *state)
{
    // If the mouse isn't inside the dock window, reset everything to normal
    if (!state->mouse_in_dock) {
        for (int i = 0; i < state->item_count; i++) {
            state->items[i].scale = 1.0;
        }
        return;
    }

    // The magnification range in pixels. This is how far from the mouse
    // the magnification effect reaches. MAGNIFICATION_RANGE is in "icon slots",
    // so we multiply by the width of one slot (icon + spacing).
    double range_px = MAGNIFICATION_RANGE * (BASE_ICON_SIZE + ICON_SPACING);

    // The maximum scale boost. When the mouse is directly on an icon,
    // it scales up by this much. For BASE=54 and MAX=82, this is about 0.519.
    double max_boost = (MAX_ICON_SIZE / (double)BASE_ICON_SIZE) - 1.0;

    // Calculate each icon's center X position and apply magnification.
    // We need to do this in two passes because magnification changes icon
    // sizes, which shifts all positions. But for the magnification calculation
    // itself, we use the UN-magnified positions (base size) so the effect
    // feels consistent regardless of current state.
    //
    // First, calculate icon centers at base size (no magnification).
    double base_total_width = 0;
    for (int i = 0; i < state->item_count; i++) {
        base_total_width += BASE_ICON_SIZE;
        if (i < state->item_count - 1) {
            // Add spacing, or separator width if there's a separator after this icon
            base_total_width += state->items[i].separator_after ? SEPARATOR_WIDTH : ICON_SPACING;
        }
    }

    // Starting X position: center the base-size icon row in the dock window
    double start_x = (state->win_w - base_total_width) / 2.0;

    // Now calculate scale for each icon based on distance from mouse
    double x = start_x;
    for (int i = 0; i < state->item_count; i++) {
        // Center of this icon at base size
        double center_x = x + BASE_ICON_SIZE / 2.0;

        // Distance from mouse to this icon's center
        double dist = fabs(state->mouse_x - center_x);

        if (dist < range_px) {
            // Icon is within magnification range — apply cosine falloff
            //
            // ratio: 0.0 when mouse is directly on icon, 1.0 at edge of range
            double ratio = dist / range_px;

            // Cosine falloff: cos(0) = 1 (full boost), cos(pi) = -1 (no boost)
            // Adding 1 and dividing by 2 maps this to the range [0, 1]
            double boost = max_boost * (1.0 + cos(M_PI * ratio)) / 2.0;

            state->items[i].scale = 1.0 + boost;
        } else {
            // Icon is too far from mouse — keep it at normal size
            state->items[i].scale = 1.0;
        }

        // Advance X to the next icon position (using base size for calculation)
        x += BASE_ICON_SIZE;
        if (i < state->item_count - 1) {
            x += state->items[i].separator_after ? SEPARATOR_WIDTH : ICON_SPACING;
        }
    }
}
