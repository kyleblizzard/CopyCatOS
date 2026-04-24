// CopyCatOS — by Kyle Blizzard at Blizzard.show

// render.c — Drawing utilities for the menu bar
//
// This module provides the low-level rendering primitives that the rest
// of the menu bar uses: background painting, text rendering, and hover
// highlights. All text is drawn using Pango with Lucida Grande, the
// signature font of macOS Snow Leopard's interface.
//
// Cairo is used for all 2D drawing. It works like a painter's canvas:
// you set colors, define paths (rectangles, arcs, lines), then fill
// or stroke them. Pango handles the complex job of text shaping and
// font rendering on top of Cairo.

#define _GNU_SOURCE  // For M_PI in math.h under strict C11

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>

#include "menubar.h"
#include "render.h"

// ── Font scaling helper ─────────────────────────────────────────────

// Build a Pango font description string with a proportionally scaled size.
// The base_size is the size at 22px menubar height (scale 1.0). At larger
// heights, the font grows proportionally so text remains visually correct.
// The returned pointer is a static buffer — only valid until the next call.
static char *scaled_font(const char *base_name, int base_size)
{
    static char buf[128];
    int scaled_size = (int)(base_size * menubar_scale + 0.5);
    if (scaled_size < base_size) scaled_size = base_size; // never go below base
    snprintf(buf, sizeof(buf), "%s %d", base_name, scaled_size);
    return buf;
}

// ── Module state ────────────────────────────────────────────────────

// Optional background texture. If loaded, it's tiled across the bar
// instead of using the gradient. NULL means "use gradient fallback."
static cairo_surface_t *bg_texture = NULL;

// ── Initialization ──────────────────────────────────────────────────

void render_init(MenuBar *mb)
{
    (void)mb;

    // Load the real Snow Leopard menubar background texture.
    // menubar_bg.png is 400x22 RGBA — the actual asset from Mac OS X 10.6.
    // It contains the exact gradient (245→170) with a 1px dark border at
    // the bottom. We tile it horizontally to fill the screen width.
    const char *home = getenv("HOME");
    if (home) {
        char path[512];
        snprintf(path, sizeof(path),
                 "%s/.local/share/aqua-widgets/menubar/menubar_bg.png", home);

        bg_texture = cairo_image_surface_create_from_png(path);
        if (cairo_surface_status(bg_texture) != CAIRO_STATUS_SUCCESS) {
            fprintf(stderr, "[menubar] WARNING: Could not load %s\n", path);
            cairo_surface_destroy(bg_texture);
            bg_texture = NULL;
        } else {
            fprintf(stderr, "[menubar] Loaded menubar_bg.png (%dx%d)\n",
                    cairo_image_surface_get_width(bg_texture),
                    cairo_image_surface_get_height(bg_texture));
        }
    }
}

// ── Background ──────────────────────────────────────────────────────

void render_background(MenuBar *mb, MenuBarPane *pane, cairo_t *cr)
{
    (void)mb;
    if (bg_texture) {
        // Render the real Snow Leopard menubar_bg.png (400x22).
        // Tile horizontally, scale vertically to match current height.
        // Use a pattern matrix to scale the texture: the matrix transforms
        // from user coords to pattern coords, so we scale Y by tex_h/height
        // to stretch the 22px texture to fill the full menubar height.
        int tex_h = cairo_image_surface_get_height(bg_texture);

        cairo_pattern_t *pattern = cairo_pattern_create_for_surface(bg_texture);
        cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);

        // Scale the pattern vertically to fill the menubar height
        cairo_matrix_t matrix;
        cairo_matrix_init_scale(&matrix, 1.0, (double)tex_h / MENUBAR_HEIGHT);
        cairo_pattern_set_matrix(pattern, &matrix);

        cairo_set_source(cr, pattern);
        cairo_rectangle(cr, 0, 0, pane->screen_w, MENUBAR_HEIGHT);
        cairo_fill(cr);
        cairo_pattern_destroy(pattern);
    } else {
        // Fallback gradient if the real asset is missing.
        // Matches the Snow Leopard menubar: #F2F2F2 -> #E8E8E8 -> #D7D7D7 -> #D2D2D2
        // with a 1px bottom border at #A8A8A8 (from CLAUDE.md color values).
        cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, MENUBAR_HEIGHT);
        cairo_pattern_add_color_stop_rgb(grad, 0.0, 242/255.0, 242/255.0, 242/255.0);
        cairo_pattern_add_color_stop_rgb(grad, 0.3, 232/255.0, 232/255.0, 232/255.0);
        cairo_pattern_add_color_stop_rgb(grad, 0.8, 215/255.0, 215/255.0, 215/255.0);
        cairo_pattern_add_color_stop_rgb(grad, 1.0, 210/255.0, 210/255.0, 210/255.0);
        cairo_set_source(cr, grad);
        cairo_rectangle(cr, 0, 0, pane->screen_w, MENUBAR_HEIGHT);
        cairo_fill(cr);
        cairo_pattern_destroy(grad);

        // 1px bottom border
        cairo_set_source_rgb(cr, 168/255.0, 168/255.0, 168/255.0);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, 0, MENUBAR_HEIGHT - 0.5);
        cairo_line_to(cr, pane->screen_w, MENUBAR_HEIGHT - 0.5);
        cairo_stroke(cr);
    }
}

// ── Text Rendering ──────────────────────────────────────────────────

// Internal helper: create a PangoLayout with our standard font settings.
// A PangoLayout is Pango's main object — it holds the text, font info,
// and computed glyph positions.
static PangoLayout *create_text_layout(cairo_t *cr, const char *text, bool bold)
{
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text, -1); // -1 = use full string

    // Build the font description. Lucida Grande is the classic macOS
    // system font. If it's not installed, Pango will fall back to
    // a similar sans-serif font. Font size scales proportionally with
    // the menubar height via scaled_font().
    PangoFontDescription *desc = pango_font_description_from_string(
        bold ? scaled_font("Lucida Grande Bold", 13)
             : scaled_font("Lucida Grande", 13)
    );
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    return layout;
}

double render_text(cairo_t *cr, const char *text, double x, double y,
                   bool bold, double r, double g, double b)
{
    // Create the layout with our font settings
    PangoLayout *layout = create_text_layout(cr, text, bold);

    // Set the text color
    cairo_set_source_rgb(cr, r, g, b);

    // Move to the drawing position and render the text
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);

    // Get the pixel width of the rendered text so the caller can
    // figure out where to place the next item
    int width, height;
    pango_layout_get_pixel_size(layout, &width, &height);

    g_object_unref(layout);

    return (double)width;
}

double render_measure_text(const char *text, bool bold)
{
    // We need a Cairo context to create a Pango layout, even though
    // we're not actually drawing anything. Create a tiny image surface
    // just for measurement purposes.
    cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(tmp);

    PangoLayout *layout = create_text_layout(cr, text, bold);

    int width, height;
    pango_layout_get_pixel_size(layout, &width, &height);

    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(tmp);

    return (double)width;
}

int render_text_center_y(const char *text, bool bold)
{
    // Ask Pango what the real layout height is for this text at the
    // current scale, then split the leftover vertical space evenly.
    // Using the measured height keeps the baseline on the bar's midline
    // at non-integer scales (1.25×, 1.5×, 1.75×) where a hardcoded
    // font-size constant would drift and push text toward the top edge.
    cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(tmp);

    PangoLayout *layout = create_text_layout(cr, text, bold);

    int width, height;
    pango_layout_get_pixel_size(layout, &width, &height);

    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(tmp);

    int y = (MENUBAR_HEIGHT - height) / 2;
    if (y < 0) y = 0;
    return y;
}

// ── Hover Highlight ─────────────────────────────────────────────────

// Helper: draw a rounded rectangle path. This is used for both hover
// highlights and dropdown menu items. Cairo doesn't have a built-in
// rounded-rect function, so we build it from arcs and lines.
static void rounded_rect(cairo_t *cr, double x, double y,
                         double w, double h, double radius)
{
    // Start at top-left, just past the rounded corner
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - radius, y + radius,     radius, -M_PI / 2, 0);         // top-right
    cairo_arc(cr, x + w - radius, y + h - radius, radius, 0,          M_PI / 2); // bottom-right
    cairo_arc(cr, x + radius,     y + h - radius, radius, M_PI / 2,   M_PI);     // bottom-left
    cairo_arc(cr, x + radius,     y + radius,     radius, M_PI,       3 * M_PI / 2); // top-left
    cairo_close_path(cr);
}

void render_hover_highlight(cairo_t *cr, int x, int y, int w, int h)
{
    // Draw a semi-transparent dark overlay. RGBA(0, 0, 0, 0.1) gives
    // a very subtle darkening effect that looks natural on the gradient.
    // Corner radius scales proportionally with the menubar height.
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.1);
    rounded_rect(cr, x, y, w, h, SF(3.0));
    cairo_fill(cr);
}

// ── Cleanup ─────────────────────────────────────────────────────────

void render_cleanup(void)
{
    if (bg_texture) {
        cairo_surface_destroy(bg_texture);
        bg_texture = NULL;
    }
}
