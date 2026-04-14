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

        // Pass 2: Grey overlay matched to real Snow Leopard measurements.
        // Measured from a real SL desktop (left padding area, no icons):
        //   y_from_bottom= 3: RGB(108,103,108) brightness=106  (dark bottom edge)
        //   y_from_bottom=10: RGB(167,159,163) brightness=163  (front shelf, blue-grey)
        //   y_from_bottom=20: RGB(153,140,154) brightness=149  (middle)
        //   y_from_bottom=30: RGB(251,254,255) brightness=253  (reflection band!)
        //   y_from_bottom=40+: wallpaper (shelf ends)
        //
        // Gradient goes from shelf_y (top of shelf, t=0) to win_h (bottom, t=1).
        // Shelf is 48px tall. y_from_bottom=30 is 18px from top → t=18/48≈0.375
        // y_from_bottom=20 is 28px from top → t=28/48≈0.58
        // y_from_bottom=10 is 38px from top → t=38/48≈0.79
        // y_from_bottom= 3 is 45px from top → t=45/48≈0.94
        cairo_pattern_t *shelf_base = cairo_pattern_create_linear(
            0, shelf_y, 0, state->win_h);
        // Back of shelf (top, t=0): real SL measures brightness 253 — nearly pure white
        // This is the reflection of ambient light on the glass surface.
        // High alpha (0.92) needed to override the scurve texture and wallpaper bleed.
        cairo_pattern_add_color_stop_rgba(shelf_base, 0.0,  0.99, 0.99, 1.0, 0.95);
        // t≈0.375 (y_from_bottom=30): still very bright, reflection band
        cairo_pattern_add_color_stop_rgba(shelf_base, 0.375, 0.98, 0.98, 1.0, 0.90);
        // t≈0.58 (y_from_bottom=20): RGB(153,140,154) brightness=149, muted purple-grey
        // Higher alpha to push through the scurve texture and wallpaper
        cairo_pattern_add_color_stop_rgba(shelf_base, 0.58,  0.62, 0.57, 0.63, 0.70);
        // t≈0.79 (y_from_bottom=10): RGB(167,159,163) brightness=163, blue-grey front
        cairo_pattern_add_color_stop_rgba(shelf_base, 0.79,  0.65, 0.62, 0.65, 0.70);
        // t≈0.94 (y_from_bottom=3): RGB(108,103,108) brightness=106, dark bottom edge
        // Higher alpha to prevent wallpaper from brightening this zone
        cairo_pattern_add_color_stop_rgba(shelf_base, 0.94,  0.30, 0.28, 0.30, 0.85);
        // Very bottom pixel: darkest — the bottom band overlay will handle final values
        cairo_pattern_add_color_stop_rgba(shelf_base, 1.0,   0.25, 0.23, 0.25, 0.90);
        cairo_set_source(cr, shelf_base);
        cairo_paint(cr);
        cairo_pattern_destroy(shelf_base);

        // Pass 3: White highlight concentrated at the back (reflection band).
        // Real SL has brightness 253 at y_from_bottom=30 (t≈0.375).
        // The highlight fades to nothing toward the front/bottom of the shelf,
        // since the bottom edge should be DARK (brightness 106), not glowing.
        cairo_pattern_t *highlight = cairo_pattern_create_linear(
            0, shelf_y, 0, state->win_h);
        cairo_pattern_add_color_stop_rgba(highlight, 0.0,  1.0, 1.0, 1.0, 0.50);  // back: very bright
        cairo_pattern_add_color_stop_rgba(highlight, 0.3,  1.0, 1.0, 1.0, 0.40);  // reflection band
        cairo_pattern_add_color_stop_rgba(highlight, 0.5,  1.0, 1.0, 1.0, 0.08);  // fading slowly
        cairo_pattern_add_color_stop_rgba(highlight, 0.75, 1.0, 1.0, 1.0, 0.03);  // slight presence in front
        cairo_pattern_add_color_stop_rgba(highlight, 1.0,  1.0, 1.0, 1.0, 0.0);   // no bottom glow
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

    // NOTE: The dark bottom band is drawn AFTER icons via shelf_draw_bottom_band()
    // to prevent icon reflections from overwriting the dark contact line.

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
        // Real Snow Leopard has a very clear, bright white/silver line at the
        // top edge of the shelf — this reinforces that effect.
        cairo_save(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.7);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, top_left, shelf_y + 0.5);
        cairo_line_to(cr, top_right, shelf_y + 0.5);
        cairo_stroke(cr);
        cairo_restore(cr);
    } else {
        // Fallback: bright white line at the top of the shelf
        cairo_save(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, top_left, shelf_y + 0.5);
        cairo_line_to(cr, top_right, shelf_y + 0.5);
        cairo_stroke(cr);
        cairo_restore(cr);
    }
}

// shelf_draw_bottom_band — Dark contact line at the very bottom of the shelf.
// Called AFTER icons and reflections are drawn so it overrides any bright
// icon content that bleeds into the bottom pixels.
// Real Snow Leopard measures:
//   y_from_bottom=3: RGB(108,103,108) brightness=106
//   y_from_bottom=5: RGB(119,115,119) brightness=118
void shelf_draw_bottom_band(DockState *state, int shelf_width)
{
    cairo_t *cr = state->cr;
    double shelf_x = (state->win_w - shelf_width) / 2.0;

    // Use OPERATOR_SOURCE to REPLACE pixels rather than blend.
    // This ensures the dark band wins over any bright icon reflections
    // or scurve texture pixels underneath.
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

    // Dark bottom band: 8px tall covering y_from_bottom 1-8
    // Real Snow Leopard measurements:
    //   y_from_bottom=8: RGB(140,135,140) brightness=138
    //   y_from_bottom=5: RGB(119,115,119) brightness=118
    //   y_from_bottom=3: RGB(108,103,108) brightness=106
    //   y_from_bottom=1: even darker, blends into screen edge
    cairo_pattern_t *bottom_band = cairo_pattern_create_linear(
        0, state->win_h - 8, 0, state->win_h);
    // Top of band (y_from_bottom=8): brightness ~138
    cairo_pattern_add_color_stop_rgba(bottom_band, 0.0,
        140/255.0, 135/255.0, 140/255.0, 0.92);
    // y_from_bottom=5 → t = 3/8 = 0.375: brightness ~118
    cairo_pattern_add_color_stop_rgba(bottom_band, 0.375,
        119/255.0, 115/255.0, 119/255.0, 0.95);
    // y_from_bottom=3 → t = 5/8 = 0.625: RGB(108,103,108)
    cairo_pattern_add_color_stop_rgba(bottom_band, 0.625,
        108/255.0, 103/255.0, 108/255.0, 0.97);
    // Very bottom (y_from_bottom=1): darkest — dark contact line
    cairo_pattern_add_color_stop_rgba(bottom_band, 1.0,
        90/255.0, 85/255.0, 90/255.0, 0.98);
    cairo_set_source(cr, bottom_band);
    cairo_rectangle(cr, shelf_x, state->win_h - 8, shelf_width, 8);
    cairo_fill(cr);
    cairo_pattern_destroy(bottom_band);

    cairo_restore(cr);
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
