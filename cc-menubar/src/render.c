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

    // The menubar_bg.png texture has a seam artifact that creates a visible
    // dark band at y=4-5 when tiled. The Cairo gradient produces a more
    // accurate match to real Snow Leopard, so we skip the texture entirely.
    // If a corrected texture is ever produced, this can be re-enabled.
    bg_texture = NULL;
}

// ── Background ──────────────────────────────────────────────────────

void render_background(MenuBar *mb, cairo_t *cr)
{
    // ── Translucent gradient (rows y=0 through y=20) ────────────────
    //
    // Measured from a real Snow Leopard 10.6.8 machine on 2026-04-14.
    // The menubar is 22px tall. Rows 0-20 are a smooth translucent
    // gradient; row 21 is a 1px opaque dark border.
    //
    // SL pixel values (x=100, away from wallpaper tint):
    //   y=0:  ~231    y=5:  ~191    y=10: ~176
    //   y=15: ~164    y=20: ~148
    //
    // The bar uses alpha ~0.88, so the wallpaper (Aurora) bleeds through
    // at ~12%, giving the characteristic warm purple tint over dark areas
    // and a slightly cool tint over bright areas. The gradient itself is
    // neutral gray — the wallpaper provides all the color.
    //
    // To produce those displayed values, the gradient RGB at alpha 0.88:
    //   displayed = gradient * 0.88 + wallpaper * 0.12
    // Solving for gradient = (displayed - wallpaper * 0.12) / 0.88
    // With a mid-tone wallpaper average of ~80:
    //   y=0:  gradient = (231 - 9.6) / 0.88 = 252  → 0.988
    //   y=10: gradient = (176 - 9.6) / 0.88 = 189  → 0.741
    //   y=20: gradient = (148 - 9.6) / 0.88 = 157  → 0.616

    cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, MENUBAR_HEIGHT - 1);

    // SL menubar gradient measured at a neutral area (x=100):
    //   y=0: 231  y=1: 204  y=5: 191  y=10: 176  y=15: 164  y=20: 148
    // At a wallpaper-tinted area (x=800):
    //   y=0: 245  y=10: 202  y=20: 171
    //
    // The gradient source values at alpha 0.88, adjusted for compositing
    // against a typical wallpaper brightness of ~60-80:
    cairo_pattern_add_color_stop_rgba(grad, 0.00,  1.00, 1.00, 1.00, 0.88);
    cairo_pattern_add_color_stop_rgba(grad, 0.05,  0.96, 0.96, 0.96, 0.88);
    cairo_pattern_add_color_stop_rgba(grad, 0.50,  0.84, 0.84, 0.84, 0.88);
    cairo_pattern_add_color_stop_rgba(grad, 1.00,  0.68, 0.68, 0.68, 0.88);

    cairo_set_source(cr, grad);
    cairo_rectangle(cr, 0, 0, mb->screen_w, MENUBAR_HEIGHT - 1);
    cairo_fill(cr);
    cairo_pattern_destroy(grad);

    // ── 1px bottom border at y=21 ──────────────────────────────────
    //
    // Measured: RGB(30, 10, 30) — nearly black with a slight warm tint.
    // Fully opaque so no wallpaper bleeds through the border line.
    cairo_set_source_rgba(cr, 30/255.0, 10/255.0, 30/255.0, 1.0);
    cairo_rectangle(cr, 0, MENUBAR_HEIGHT - 1, mb->screen_w, 1);
    cairo_fill(cr);
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
