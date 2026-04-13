// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// indicator.c — Running-app indicator dots
//
// A small glowing dot beneath each running app's icon. This tells the user
// at a glance which apps are currently open.
//
// We first try to load indicator_medium.png from the widget assets directory.
// If that file doesn't exist, we fall back to drawing a procedural dot using
// a radial gradient: white center -> aqua blue -> transparent edge. This
// fallback ensures the dock still looks right even without the image asset.
// ============================================================================

#include "indicator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// How far from the bottom of the dock window to place the indicator center
#define INDICATOR_BOTTOM_OFFSET 6  // Sits on the shelf surface, 6px from dock bottom

bool indicator_load(DockState *state)
{
    // Try to load the indicator image from the aqua-widgets asset directory
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    char path[1024];
    snprintf(path, sizeof(path),
             "%s/.local/share/aqua-widgets/dock/indicator_medium.png", home);

    // Attempt to load the PNG file
    state->indicator_img = cairo_image_surface_create_from_png(path);

    // Check if the load succeeded. Cairo doesn't return NULL on failure;
    // instead it returns a surface with an error status.
    if (cairo_surface_status(state->indicator_img) != CAIRO_STATUS_SUCCESS) {
        // The image file wasn't found or was invalid — clean up and create
        // a procedural indicator instead
        cairo_surface_destroy(state->indicator_img);
        state->indicator_img = NULL;

        // Create a small surface for our procedural dot
        int size = INDICATOR_SIZE * 2;  // Extra space for the glow
        state->indicator_img = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, size, size);

        cairo_t *cr = cairo_create(state->indicator_img);

        // Draw a radial gradient: bright white center, aqua blue glow, fades out
        // Radial gradient goes from a tiny center point outward to the edge
        double cx = size / 2.0;
        double cy = size / 2.0;
        double radius = size / 2.0;

        cairo_pattern_t *grad = cairo_pattern_create_radial(
            cx, cy, 0,       // Inner circle: center point with radius 0
            cx, cy, radius   // Outer circle: same center, full radius
        );

        // White hot center
        cairo_pattern_add_color_stop_rgba(grad, 0.0, 1.0, 1.0, 1.0, 1.0);
        // Aqua blue glow (the classic macOS indicator color)
        cairo_pattern_add_color_stop_rgba(grad, 0.4, 0.4, 0.75, 1.0, 0.8);
        // Fade to transparent at the edges
        cairo_pattern_add_color_stop_rgba(grad, 1.0, 0.3, 0.6, 1.0, 0.0);

        cairo_set_source(cr, grad);
        cairo_paint(cr);

        cairo_pattern_destroy(grad);
        cairo_destroy(cr);

        cairo_surface_flush(state->indicator_img);
    }

    return true;
}

void indicator_draw(DockState *state, double center_x)
{
    if (!state->indicator_img) return;

    int img_w = cairo_image_surface_get_width(state->indicator_img);
    int img_h = cairo_image_surface_get_height(state->indicator_img);

    // Position the indicator centered horizontally on center_x,
    // and near the bottom of the dock window
    double draw_x = center_x - img_w / 2.0;
    double draw_y = state->win_h - INDICATOR_BOTTOM_OFFSET - img_h / 2.0;

    cairo_save(state->cr);
    cairo_set_source_surface(state->cr, state->indicator_img, draw_x, draw_y);
    cairo_paint(state->cr);
    cairo_restore(state->cr);
}

void indicator_cleanup(DockState *state)
{
    if (state->indicator_img) {
        cairo_surface_destroy(state->indicator_img);
        state->indicator_img = NULL;
    }
}
