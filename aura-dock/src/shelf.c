// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// shelf.c — Glass shelf rendering
//
// The glass shelf is the translucent platform at the bottom of the dock.
// It's the most distinctive visual element of the Snow Leopard dock style.
//
// How it's drawn:
// 1. Load scurve-xl.png — the pre-rendered shelf image with glass gradients
// 2. Clip to a trapezoid shape (narrower at top, wider at bottom)
//    - Top edge is 97.2% of bottom width (1.4% inset on each side)
//    - This creates the subtle 3D perspective effect
// 3. Draw the shelf image at 65% opacity
// 4. Draw frontline.png — a 1px bright highlight along the top edge
// 5. Draw separators between dock sections (white line + shadow line)
// ============================================================================

#include "shelf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool shelf_load_assets(DockState *state)
{
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    char path[1024];

    // Load the main shelf image (the glass gradient)
    snprintf(path, sizeof(path),
             "%s/.local/share/aqua-widgets/dock/scurve-xl.png", home);

    state->shelf_img = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(state->shelf_img) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Warning: Could not load shelf image: %s\n", path);
        cairo_surface_destroy(state->shelf_img);
        state->shelf_img = NULL;
        // We'll draw a fallback gradient instead
    }

    // Load the frontline highlight (1px bright line along the shelf top)
    snprintf(path, sizeof(path),
             "%s/.local/share/aqua-widgets/dock/frontline.png", home);

    state->frontline_img = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(state->frontline_img) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(state->frontline_img);
        state->frontline_img = NULL;
    }

    return true;
}

void shelf_draw(DockState *state, int shelf_width)
{
    cairo_t *cr = state->cr;

    // The shelf sits at the bottom of the dock window
    double shelf_y = state->win_h - SHELF_HEIGHT;

    // Center the shelf horizontally in the dock window
    double shelf_x = (state->win_w - shelf_width) / 2.0;

    // --- Trapezoid clip path ---
    // The top edge is slightly narrower than the bottom, creating a
    // subtle 3D perspective effect. The inset is 1.4% on each side,
    // so the top width is 97.2% of the bottom width.
    double inset = shelf_width * 0.014;  // 1.4% per side
    double top_left  = shelf_x + inset;
    double top_right = shelf_x + shelf_width - inset;

    cairo_save(cr);

    // Define the trapezoid as a clip path
    // Start at top-left, go clockwise
    cairo_new_path(cr);
    cairo_move_to(cr, top_left, shelf_y);                      // Top-left
    cairo_line_to(cr, top_right, shelf_y);                     // Top-right
    cairo_line_to(cr, shelf_x + shelf_width, state->win_h);   // Bottom-right
    cairo_line_to(cr, shelf_x, state->win_h);                  // Bottom-left
    cairo_close_path(cr);
    cairo_clip(cr);

    if (state->shelf_img) {
        // Scale the shelf image to fill the entire shelf area
        int img_w = cairo_image_surface_get_width(state->shelf_img);
        int img_h = cairo_image_surface_get_height(state->shelf_img);

        cairo_save(cr);
        cairo_translate(cr, shelf_x, shelf_y);
        cairo_scale(cr,
                    (double)shelf_width / img_w,
                    (double)SHELF_HEIGHT / img_h);
        cairo_set_source_surface(cr, state->shelf_img, 0, 0);

        // Draw at 65% opacity for the semi-transparent glass look
        cairo_paint_with_alpha(cr, 0.65);
        cairo_restore(cr);
    } else {
        // Fallback: draw a simple gradient if the image didn't load
        cairo_pattern_t *grad = cairo_pattern_create_linear(
            0, shelf_y, 0, state->win_h);

        // Semi-transparent dark gradient as a stand-in for the glass shelf
        cairo_pattern_add_color_stop_rgba(grad, 0.0, 0.3, 0.3, 0.35, 0.5);
        cairo_pattern_add_color_stop_rgba(grad, 0.3, 0.15, 0.15, 0.2, 0.55);
        cairo_pattern_add_color_stop_rgba(grad, 1.0, 0.05, 0.05, 0.1, 0.6);

        cairo_set_source(cr, grad);
        cairo_paint(cr);
        cairo_pattern_destroy(grad);
    }

    cairo_restore(cr);  // Remove the trapezoid clip

    // --- Frontline highlight ---
    // A 1px bright line along the top edge of the shelf. This makes it look
    // like light is reflecting off the glass edge.
    if (state->frontline_img) {
        int fl_w = cairo_image_surface_get_width(state->frontline_img);
        int fl_h = cairo_image_surface_get_height(state->frontline_img);

        cairo_save(cr);
        cairo_translate(cr, top_left, shelf_y);
        cairo_scale(cr,
                    (top_right - top_left) / (double)fl_w,
                    1.0 / (double)(fl_h > 0 ? fl_h : 1));
        cairo_set_source_surface(cr, state->frontline_img, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        // Fallback: just draw a thin white line at the top of the shelf
        cairo_save(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.4);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, top_left, shelf_y + 0.5);
        cairo_line_to(cr, top_right, shelf_y + 0.5);
        cairo_stroke(cr);
        cairo_restore(cr);
    }
}

void shelf_draw_separator(DockState *state, double x)
{
    cairo_t *cr = state->cr;
    double shelf_y = state->win_h - SHELF_HEIGHT;

    cairo_save(cr);

    // The separator is two thin vertical lines side by side:
    // 1. A white line at alpha 80/255 (bright edge)
    // 2. A black line at alpha 40/255 (shadow), offset 1px to the right
    // Together they create a subtle etched groove effect.

    // White highlight line
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 80.0 / 255.0);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x + 0.5, shelf_y + 4);
    cairo_line_to(cr, x + 0.5, state->win_h - 4);
    cairo_stroke(cr);

    // Black shadow line (1px to the right)
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 40.0 / 255.0);
    cairo_move_to(cr, x + 1.5, shelf_y + 4);
    cairo_line_to(cr, x + 1.5, state->win_h - 4);
    cairo_stroke(cr);

    cairo_restore(cr);
}

void shelf_cleanup(DockState *state)
{
    if (state->shelf_img) {
        cairo_surface_destroy(state->shelf_img);
        state->shelf_img = NULL;
    }
    if (state->frontline_img) {
        cairo_surface_destroy(state->frontline_img);
        state->frontline_img = NULL;
    }
}
