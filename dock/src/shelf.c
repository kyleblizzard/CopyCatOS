// CopyCatOS — by Kyle Blizzard at Blizzard.show

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

    // Load the pre-composited opaque shelf image.
    // scurve-xl-opaque.png is the real Snow Leopard scurve-xl.png composited
    // over gray(50) to produce fully opaque pixels at the correct brightness.
    // This avoids the alpha-transparency problem where MoonRock's compositing
    // makes the shelf see-through on dark wallpapers.
    snprintf(path, sizeof(path),
             "%s/.local/share/aqua-widgets/dock/scurve-xl-opaque.png", home);

    state->shelf_img = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(state->shelf_img) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Warning: Could not load shelf image: %s\n", path);
        cairo_surface_destroy(state->shelf_img);
        state->shelf_img = NULL;
        // We'll draw a fallback gradient instead
    }

    // Load the frontline highlight (top edge of shelf)
    snprintf(path, sizeof(path),
             "%s/.local/share/aqua-widgets/dock/frontline.png", home);
    state->frontline_img = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(state->frontline_img) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(state->frontline_img);
        state->frontline_img = NULL;
    }

    // Load the separator (dashed divider between app and doc sections)
    snprintf(path, sizeof(path),
             "%s/.local/share/aqua-widgets/dock/separator.png", home);
    state->separator_img = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(state->separator_img) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(state->separator_img);
        state->separator_img = NULL;
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

    // ── Render the shelf texture ──────────────────────────────────
    // scurve-xl-opaque.png is the real Snow Leopard shelf asset pre-composited
    // to fully opaque pixels. No backing fill needed — just render it directly.
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

// shelf_draw_bottom_band — Bottom edge features.
// Called AFTER icons and reflections so these override any bleeding content.
//
// Real Snow Leopard bottom edge (measured):
//   yfb 3: brightness  88 (dark contact line)
//   yfb 2: brightness 231 (bright bottom highlight)
//   yfb 1: brightness 232 (bright bottom highlight)
//
// The bright highlight at yfb 1-2 is the signature "glass sitting on a lit
// surface" effect — ambient light reflecting off the screen edge underneath
// the shelf. This is clearly visible in every real SL screenshot.
void shelf_draw_bottom_band(DockState *state, int shelf_width)
{
    cairo_t *cr = state->cr;
    double shelf_x = (state->win_w - shelf_width) / 2.0;

    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

    // Scale the 1-pt dark line and 2-pt bright highlight to match the output's
    // backing scale so the bottom band reads the same thickness at any HiDPI
    // factor. Stacked from the bottom up to avoid overlap at fractional scales.
    int bright_h = (int)(2 * state->hidpi_scale + 0.5);
    int dark_h   = (int)(1 * state->hidpi_scale + 0.5);
    int bright_y = state->win_h - bright_h;
    int dark_y   = bright_y - dark_h;

    // Dark contact line (1 pt above bright) — RGB(91,84,91)
    cairo_set_source_rgba(cr, 91/255.0, 84/255.0, 91/255.0, 1.0);
    cairo_rectangle(cr, shelf_x, dark_y, shelf_width, dark_h);
    cairo_fill(cr);

    // Bright bottom highlight (2 pt from bottom) — RGB(232,231,232)
    cairo_set_source_rgba(cr, 232/255.0, 231/255.0, 232/255.0, 1.0);
    cairo_rectangle(cr, shelf_x, bright_y, shelf_width, bright_h);
    cairo_fill(cr);

    cairo_restore(cr);
}

void shelf_draw_separator(DockState *state, double x)
{
    cairo_t *cr = state->cr;
    double shelf_y = state->win_h - SHELF_HEIGHT;

    // Render the real Snow Leopard separator.png asset.
    // It's 64x128 RGBA — a dashed stippled white line pattern.
    // We scale it to fit the shelf height and center it at x.
    if (state->separator_img) {
        int sep_w = cairo_image_surface_get_width(state->separator_img);
        int sep_h = cairo_image_surface_get_height(state->separator_img);

        cairo_save(cr);
        // Center the separator image at the given x position
        double draw_x = x - (double)sep_w / 2.0;
        cairo_translate(cr, draw_x, shelf_y);
        cairo_scale(cr, 1.0, (double)SHELF_HEIGHT / sep_h);
        cairo_set_source_surface(cr, state->separator_img, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    }
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
    if (state->separator_img) {
        cairo_surface_destroy(state->separator_img);
        state->separator_img = NULL;
    }
}
