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
// 3. Draw the shelf image at full opacity (its alpha channel handles transparency)
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
        // The scurve-xl.png is an RGBA image with built-in alpha (141-201).
        // When double-buffered through an ARGB surface and composited by picom,
        // the premultiplied alpha path makes it darker than intended.
        //
        // Real Snow Leopard shelf pixel profile (measured from real machine):
        //   Front of shelf: brightness 140-143 (blue-grey silver)
        //   Middle: brightness 130-166 (icon reflections vary)
        //   Back: brightness 164-245 (very bright, nearly white reflection zone)
        //
        // Strategy: Paint the scurve texture, then add a strong semi-opaque
        // grey overlay to reach the real Snow Leopard brightness levels.
        int img_w = cairo_image_surface_get_width(state->shelf_img);
        int img_h = cairo_image_surface_get_height(state->shelf_img);

        // Pass 1: The scurve texture at native alpha
        cairo_save(cr);
        cairo_translate(cr, shelf_x, shelf_y);
        cairo_scale(cr,
                    (double)shelf_width / img_w,
                    (double)SHELF_HEIGHT / img_h);
        cairo_set_source_surface(cr, state->shelf_img, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);

        // Pass 2: Strong grey overlay to match real SL brightness.
        // The real shelf has a grey-silver base tone around RGB(150-180).
        // We paint a semi-opaque grey to bring our brightness up to match.
        cairo_pattern_t *shelf_base = cairo_pattern_create_linear(
            0, shelf_y, 0, state->win_h);
        // Back of shelf (top): very bright, almost white — real SL measures brightness 245 here
        cairo_pattern_add_color_stop_rgba(shelf_base, 0.0,  0.85, 0.85, 0.88, 0.65);
        // Middle: silver tone matching SL measured brightness ~131
        cairo_pattern_add_color_stop_rgba(shelf_base, 0.4,  0.65, 0.67, 0.72, 0.55);
        // Front of shelf: blue-grey silver — real SL is R=113-136, G=142-147, B=147-170, brightness 141
        // Pushed higher to close the -21 brightness gap
        cairo_pattern_add_color_stop_rgba(shelf_base, 0.8,  0.52, 0.60, 0.70, 0.65);
        // Very bottom: brighter to approach SL's 209 brightness at the edge
        cairo_pattern_add_color_stop_rgba(shelf_base, 1.0,  0.58, 0.62, 0.70, 0.65);
        cairo_set_source(cr, shelf_base);
        cairo_paint(cr);
        cairo_pattern_destroy(shelf_base);

        // Pass 3: Subtle white highlight at the front to create the reflection band
        // Real SL has brightness 200+ in the back half of the shelf (y 34-48)
        cairo_pattern_t *highlight = cairo_pattern_create_linear(
            0, shelf_y, 0, state->win_h);
        cairo_pattern_add_color_stop_rgba(highlight, 0.0,  1.0, 1.0, 1.0, 0.15);  // back: bright
        cairo_pattern_add_color_stop_rgba(highlight, 0.3,  1.0, 1.0, 1.0, 0.08);  // dip in middle
        cairo_pattern_add_color_stop_rgba(highlight, 0.7,  1.0, 1.0, 1.0, 0.05);  // front is darker
        cairo_pattern_add_color_stop_rgba(highlight, 1.0,  1.0, 1.0, 1.0, 0.10);  // bottom edge glow
        cairo_set_source(cr, highlight);
        cairo_paint(cr);
        cairo_pattern_destroy(highlight);
    } else {
        // Fallback: draw a simple gradient if the image didn't load
        cairo_pattern_t *grad = cairo_pattern_create_linear(
            0, shelf_y, 0, state->win_h);

        // Semi-transparent dark gradient as a stand-in for the glass shelf
        // Higher alpha values here so the fallback is actually visible
        cairo_pattern_add_color_stop_rgba(grad, 0.0,  0.15, 0.15, 0.18, 0.70);
        cairo_pattern_add_color_stop_rgba(grad, 0.3,  0.10, 0.10, 0.12, 0.75);
        cairo_pattern_add_color_stop_rgba(grad, 1.0,  0.05, 0.05, 0.08, 0.80);

        cairo_set_source(cr, grad);
        cairo_paint(cr);
        cairo_pattern_destroy(grad);
    }

    cairo_restore(cr);  // Remove the trapezoid clip

    // Strong bright line at the very bottom of the shelf (the "front edge" of the glass).
    // Real SL bottom edge pixel brightness is ~209 — nearly pure white.
    // This is the single most important visual cue that says "glass shelf."
    cairo_set_source_rgba(cr, 0.82, 0.83, 0.85, 0.75);
    cairo_set_line_width(cr, 3.0);
    cairo_move_to(cr, shelf_x, state->win_h - 1.5);
    cairo_line_to(cr, shelf_x + shelf_width, state->win_h - 1.5);
    cairo_stroke(cr);
    // Thinner white highlight on top of that
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.4);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, shelf_x, state->win_h - 0.5);
    cairo_line_to(cr, shelf_x + shelf_width, state->win_h - 0.5);
    cairo_stroke(cr);

    // --- Frontline highlight ---
    // A 1px bright line along the top edge of the shelf. This makes it look
    // like light is reflecting off the glass edge.
    if (state->frontline_img) {
        int fl_w = cairo_image_surface_get_width(state->frontline_img);
        int fl_h = cairo_image_surface_get_height(state->frontline_img);

        // Paint the real frontline asset at full opacity for maximum visibility
        cairo_save(cr);
        cairo_translate(cr, top_left, shelf_y);
        cairo_scale(cr,
                    (top_right - top_left) / (double)fl_w,
                    1.0 / (double)(fl_h > 0 ? fl_h : 1));
        cairo_set_source_surface(cr, state->frontline_img, 0, 0);
        cairo_paint(cr);  // Full opacity — let the asset's own alpha do the work
        cairo_restore(cr);

        // Also draw a white highlight line on top of the asset for extra pop.
        // Real Snow Leopard has a very visible bright white/silver line at the
        // top edge of the shelf — this reinforces that effect.
        cairo_save(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.5);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, top_left, shelf_y + 0.5);
        cairo_line_to(cr, top_right, shelf_y + 0.5);
        cairo_stroke(cr);
        cairo_restore(cr);
    } else {
        // Fallback: brighter white line at the top of the shelf
        cairo_save(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.75);  // Boosted from 0.65 for visibility
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
    // 1. A dark translucent line (shadow/groove)
    // 2. A white highlight line offset 1px to the right (bright edge)
    // Together they create the classic Snow Leopard etched groove effect.
    // The separator spans the full shelf height for clear visibility.

    // Dark groove line (left side of the etched pair)
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.35);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x + 0.5, shelf_y + 4);
    cairo_line_to(cr, x + 0.5, state->win_h - 4);
    cairo_stroke(cr);

    // White highlight line (right side, catches the "light")
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.45);
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
