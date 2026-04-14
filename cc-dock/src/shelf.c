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

    // ── Trapezoid clip path ─────────────────────────────────────────
    // Top edge is 97.2% of bottom width (1.4% inset per side).
    double inset = shelf_width * 0.014;
    double top_left  = shelf_x + inset;
    double top_right = shelf_x + shelf_width - inset;

    cairo_save(cr);
    cairo_new_path(cr);
    cairo_move_to(cr, top_left, shelf_y);
    cairo_line_to(cr, top_right, shelf_y);
    cairo_line_to(cr, shelf_x + shelf_width, state->win_h);
    cairo_line_to(cr, shelf_x, state->win_h);
    cairo_close_path(cr);
    cairo_clip(cr);

    // ── Opaque backing behind the glass ────────────────────────────
    // The scurve texture has alpha 56-75%. On real Snow Leopard, the dock
    // renders into its own opaque compositing layer before being placed on
    // screen. MoonRock composites the ARGB window directly over wallpaper,
    // so without an opaque backing the glass is too transparent.
    //
    // This fill provides the opaque surface the glass sits on. The scurve
    // then composites over it with OVER, giving the correct final look.
    // The color (neutral gray ~55%) matches the average shelf brightness
    // from the real Snow Leopard machine.
    cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
    cairo_rectangle(cr, shelf_x, shelf_y, shelf_width, SHELF_HEIGHT);
    cairo_fill(cr);

    // ── Render the real scurve-xl.png shelf texture ─────────────────
    // This is the actual Snow Leopard dock shelf asset (1280x86 RGBA).
    // It composites over the opaque backing to produce the glass look.
    if (state->shelf_img) {
        int img_w = cairo_image_surface_get_width(state->shelf_img);
        int img_h = cairo_image_surface_get_height(state->shelf_img);

        cairo_save(cr);
        cairo_translate(cr, shelf_x, shelf_y);
        cairo_scale(cr,
                    (double)shelf_width / img_w,
                    (double)SHELF_HEIGHT / img_h);
        cairo_set_source_surface(cr, state->shelf_img, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    }

    cairo_restore(cr);  // Remove the trapezoid clip

    // ── Frontline highlight ─────────────────────────────────────────
    // frontline.png is the real Snow Leopard asset (790x3 RGBA).
    // Row 0: black alpha 77 (shadow), rows 1-2: white alpha 209 (highlight).
    // Just render it directly — no extra white lines on top.
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
    }
}

// shelf_draw_bottom_band — Clean up the bottom edge after icons/reflections.
// The scurve texture already has the correct bottom gradient. This just
// ensures icon reflections don't bleed past the shelf bottom.
void shelf_draw_bottom_band(DockState *state, int shelf_width)
{
    (void)state;
    (void)shelf_width;
    // The scurve texture handles the bottom edge. No fake gradient needed.
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
