// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

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

void render_background(MenuBar *mb, cairo_t *cr)
{
    if (bg_texture) {
        // Tile the real Snow Leopard menubar texture across the full width.
        // The texture is 400x22 and contains the complete gradient + border.
        // CAIRO_EXTEND_REPEAT tiles it seamlessly.
        cairo_pattern_t *pattern = cairo_pattern_create_for_surface(bg_texture);
        cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
        cairo_set_source(cr, pattern);
        cairo_rectangle(cr, 0, 0, mb->screen_w, MENUBAR_HEIGHT);
        cairo_fill(cr);
        cairo_pattern_destroy(pattern);
    } else {
        // Fallback only if the real asset is missing
        cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
        cairo_rectangle(cr, 0, 0, mb->screen_w, MENUBAR_HEIGHT);
        cairo_fill(cr);
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
    // a similar sans-serif font.
    PangoFontDescription *desc = pango_font_description_from_string(
        bold ? "Lucida Grande Bold 13" : "Lucida Grande 13"
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
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.1);
    rounded_rect(cr, x, y, w, h, 3.0);
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
