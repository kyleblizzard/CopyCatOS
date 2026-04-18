// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ─── render.c ───
// All visual rendering for the Spotlight overlay.
//
// This file uses Cairo for vector graphics and Pango (via
// PangoCairo) for high-quality text layout.  Every frame is
// drawn from scratch onto a 32-bit ARGB surface — there is
// no retained-mode scene graph, just immediate-mode painting.
//
// The visual design mimics macOS Spotlight: a frosted-glass
// style rounded rectangle with a search bar at the top, a
// list of results below, and a highlighted selection row.

#define _GNU_SOURCE  // For M_PI in math.h under strict C11

#include <math.h>
#include <string.h>
#include <pango/pangocairo.h>

#include "render.h"
#include "spotlight.h"

// ──────────────────────────────────────────────
// Colour constants
// ──────────────────────────────────────────────

// Main overlay background — light gray, slightly translucent.
#define BG_R  (232.0 / 255.0)
#define BG_G  (232.0 / 255.0)
#define BG_B  (232.0 / 255.0)
#define BG_A  (225.0 / 255.0)

// Border around the overlay.
#define BORDER_R  (180.0 / 255.0)
#define BORDER_G  (180.0 / 255.0)
#define BORDER_B  (180.0 / 255.0)
#define BORDER_A  (200.0 / 255.0)

// Search field background — white, nearly opaque.
#define FIELD_R  (255.0 / 255.0)
#define FIELD_G  (255.0 / 255.0)
#define FIELD_B  (255.0 / 255.0)
#define FIELD_A  (240.0 / 255.0)

// Selected result row — a blue highlight.
#define SEL_R  (56.0  / 255.0)
#define SEL_G  (117.0 / 255.0)
#define SEL_B  (246.0 / 255.0)
#define SEL_A  (220.0 / 255.0)

// Row separator line.
#define SEP_R  (200.0 / 255.0)
#define SEP_G  (200.0 / 255.0)
#define SEP_B  (200.0 / 255.0)
#define SEP_A  (160.0 / 255.0)

// ──────────────────────────────────────────────
// Rounded rectangle helper
// ──────────────────────────────────────────────

// Add a rounded-rectangle sub-path to the current Cairo path.
// (x, y) is the top-left corner; w and h are width and height;
// r is the corner radius.
static void rounded_rect(cairo_t *cr,
                          double x, double y,
                          double w, double h,
                          double r) {
    // Four arcs, one per corner, connected by implicit straight
    // lines.  We start at the top-right corner and go clockwise.
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -M_PI / 2.0, 0);          // top-right
    cairo_arc(cr, x + w - r, y + h - r, r, 0,            M_PI / 2.0); // bottom-right
    cairo_arc(cr, x + r,     y + h - r, r, M_PI / 2.0,   M_PI);      // bottom-left
    cairo_arc(cr, x + r,     y + r,     r, M_PI,         3.0 * M_PI / 2.0); // top-left
    cairo_close_path(cr);
}

// ──────────────────────────────────────────────
// Drop shadow
// ──────────────────────────────────────────────

// Draw a fake drop shadow by painting several concentric rounded
// rectangles, each slightly larger and more transparent than the
// last.  This is cheaper than a real Gaussian blur and looks
// convincing enough for a UI shadow.
static void draw_shadow(cairo_t *cr,
                         double x, double y,
                         double w, double h) {
    // Number of shadow layers.  More layers = smoother gradient.
    int layers = 8;

    // Total shadow spread in pixels.
    int blur_radius = 24;

    // Vertical offset — the shadow is shifted down slightly to
    // simulate a light source above the overlay.
    double y_offset = 2.0;

    for (int i = 0; i < layers; i++) {
        // Alpha decreases with each outer layer, creating a
        // smooth fade-out.
        double alpha = 0.3 * (1.0 - (double)i / (double)layers) * 0.3;

        // Each layer expands outward from the overlay bounds.
        double expand = (double)(blur_radius - i * 3);

        rounded_rect(cr,
                     x - expand,
                     y - expand + y_offset,
                     w + 2.0 * expand,
                     h + 2.0 * expand,
                     CORNER_RADIUS + expand * 0.5);

        cairo_set_source_rgba(cr, 0, 0, 0, alpha);
        cairo_fill(cr);
    }
}

// ──────────────────────────────────────────────
// Magnifying glass icon
// ──────────────────────────────────────────────

// Draw a small magnifying-glass icon using Cairo vector
// primitives — a circle (the lens) with a diagonal line
// (the handle) extending from it.
static void draw_search_icon(cairo_t *cr, double cx, double cy) {
    // Lens: a circle centred at (cx, cy) with radius 7px.
    double r = 7.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2.0 * M_PI);

    cairo_set_source_rgb(cr, 0x88 / 255.0, 0x88 / 255.0, 0x88 / 255.0);
    cairo_set_line_width(cr, 2.5);
    cairo_stroke(cr);

    // Handle: a line from the bottom-right of the lens,
    // extending diagonally down-right.
    double angle = M_PI / 4.0; // 45 degrees
    double hx = cx + r * cos(angle);
    double hy = cy + r * sin(angle);

    cairo_move_to(cr, hx, hy);
    cairo_line_to(cr, hx + 6.0, hy + 6.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, 2.5);
    cairo_stroke(cr);
}

// ──────────────────────────────────────────────
// Text rendering with Pango
// ──────────────────────────────────────────────

// Draw a single line of text using Pango for proper font
// shaping and rendering.
//
// font_desc — a Pango font description string (e.g. "Lucida Grande 17")
// text      — the UTF-8 string to render
// x, y      — baseline position (left edge, top of the text area)
// r, g, b   — text colour (0.0 .. 1.0)
static void draw_text(cairo_t *cr,
                       const char *font_desc,
                       const char *text,
                       double x, double y,
                       double r, double g, double b) {
    // Create a Pango layout — this object holds the shaped text
    // and the font information needed to render it.
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text, -1);

    // Parse the font description string into a PangoFontDescription.
    PangoFontDescription *desc = pango_font_description_from_string(font_desc);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    // Position the text and paint it.
    cairo_set_source_rgb(cr, r, g, b);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
}

// ──────────────────────────────────────────────
// Search field
// ──────────────────────────────────────────────

// Draw the search text field at the top of the overlay.
// It includes a white background, the magnifying glass icon,
// and either the user's query or a gray placeholder string.
static void draw_search_field(cairo_t *cr,
                                double x, double y,
                                double w,
                                const char *query) {
    double padding = 8.0;

    // White background with smaller corner radius.
    rounded_rect(cr, x + padding, y + padding,
                 w - 2.0 * padding, SEARCH_HEIGHT - 2.0 * padding, 8.0);
    cairo_set_source_rgba(cr, FIELD_R, FIELD_G, FIELD_B, FIELD_A);
    cairo_fill(cr);

    // Magnifying glass on the left.
    draw_search_icon(cr, x + 28.0, y + SEARCH_HEIGHT / 2.0);

    // Text content.
    double text_x = x + 48.0;
    double text_y = y + 12.0;

    if (query[0] == '\0') {
        // Placeholder text when the field is empty.
        draw_text(cr, "Lucida Grande 17", "Spotlight Search",
                  text_x, text_y,
                  0xA0 / 255.0, 0xA0 / 255.0, 0xA0 / 255.0);
    } else {
        // The user's current query.
        draw_text(cr, "Lucida Grande 17", query,
                  text_x, text_y,
                  0x1E / 255.0, 0x1E / 255.0, 0x1E / 255.0);
    }
}

// ──────────────────────────────────────────────
// Result rows
// ──────────────────────────────────────────────

// Draw one result row.  `is_selected` controls whether it gets
// the blue highlight background or the default appearance.
static void draw_result_row(cairo_t *cr,
                              double x, double y,
                              double w,
                              SearchEntry *entry,
                              int is_selected) {
    double row_h = RESULT_HEIGHT;
    double hmargin = 4.0; // horizontal margin for the selection highlight

    // ── Selection highlight ──
    if (is_selected) {
        rounded_rect(cr, x + hmargin, y, w - 2.0 * hmargin, row_h, 4.0);
        cairo_set_source_rgba(cr, SEL_R, SEL_G, SEL_B, SEL_A);
        cairo_fill(cr);
    }

    // ── App icon (32×32, 12px from the left edge) ──
    double icon_x = x + 12.0;
    double icon_y = y + (row_h - 32.0) / 2.0;

    if (entry->icon) {
        cairo_set_source_surface(cr, entry->icon, icon_x, icon_y);
        cairo_paint(cr);
    }

    // ── Text colours change based on selection state ──
    double name_r, name_g, name_b;
    double desc_r, desc_g, desc_b;

    if (is_selected) {
        // White text on the blue highlight.
        name_r = name_g = name_b = 1.0;
        desc_r = desc_g = desc_b = 0.92;
    } else {
        // Dark text on the light background.
        name_r = name_g = name_b = 0x1E / 255.0;
        desc_r = desc_g = desc_b = 0x78 / 255.0;
    }

    // ── App name (bold, 15pt) ──
    double text_x = x + 56.0;

    if (entry->generic_name[0] != '\0') {
        // Two lines: name on top, generic name below.
        draw_text(cr, "Lucida Grande Bold 15", entry->name,
                  text_x, y + 3.0, name_r, name_g, name_b);
        draw_text(cr, "Lucida Grande 11", entry->generic_name,
                  text_x, y + 24.0, desc_r, desc_g, desc_b);
    } else {
        // Single line centred vertically.
        draw_text(cr, "Lucida Grande Bold 15", entry->name,
                  text_x, y + 12.0, name_r, name_g, name_b);
    }
}

// ──────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────

void render_frame(cairo_t *cr,
                  int width, int height,
                  const char *query,
                  SearchEntry **results,
                  int result_count,
                  int selected) {
    // ── Clear the entire surface to fully transparent ──
    // This is important because we're drawing on a 32-bit ARGB
    // window — any pixel we don't paint will be see-through.
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);

    // Switch back to normal "over" compositing for all drawing.
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // The content area is inset from the window edges to leave
    // room for the drop shadow.  The shadow extends ~24px out
    // from the overlay in every direction.
    double shadow_pad = 28.0;
    double cx = shadow_pad;             // content x
    double cy = shadow_pad;             // content y
    double cw = width - 2.0 * shadow_pad;  // content width
    double ch = height - 2.0 * shadow_pad; // content height

    // ── Drop shadow ──
    draw_shadow(cr, cx, cy, cw, ch);

    // ── Main background ──
    rounded_rect(cr, cx, cy, cw, ch, CORNER_RADIUS);
    cairo_set_source_rgba(cr, BG_R, BG_G, BG_B, BG_A);
    cairo_fill(cr);

    // ── Border ──
    rounded_rect(cr, cx, cy, cw, ch, CORNER_RADIUS);
    cairo_set_source_rgba(cr, BORDER_R, BORDER_G, BORDER_B, BORDER_A);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    // ── Search field ──
    draw_search_field(cr, cx, cy, cw, query);

    // ── Result list ──
    // Determine how many rows to show (capped at MAX_VISIBLE_RESULTS).
    int visible = result_count;
    if (visible > MAX_VISIBLE_RESULTS) visible = MAX_VISIBLE_RESULTS;

    double row_y = cy + SEARCH_HEIGHT;

    for (int i = 0; i < visible; i++) {
        // Draw a separator line between rows (but not above the first).
        if (i > 0) {
            cairo_set_source_rgba(cr, SEP_R, SEP_G, SEP_B, SEP_A);
            cairo_move_to(cr, cx + 12.0, row_y);
            cairo_line_to(cr, cx + cw - 12.0, row_y);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
        }

        draw_result_row(cr, cx, row_y, cw, results[i], (i == selected));
        row_y += RESULT_HEIGHT;
    }
}
