// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// toolbar.c — Snow Leopard Finder toolbar
//
// The toolbar is the 30px-tall strip at the top of the Finder window,
// sitting just below the WM title bar. It provides:
//
//   1. View mode buttons (Icon, List, Column, Cover Flow) on the left
//   2. Path breadcrumb in the center
//   3. Search field (rounded rect with placeholder) on the right
//
// The visual style matches the real Snow Leopard Finder toolbar:
//   - Gradient background from #D8D8D8 (top) to #C0C0C0 (bottom)
//   - 1px bottom border line (#A0A0A0)
//   - Recessed button style for view mode toggles
//   - Rounded-rect search field with magnifying glass icon

#define _GNU_SOURCE  // For M_PI

#include "toolbar.h"

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include <stdio.h>
#include <string.h>
#include <math.h>

// ── Module state ────────────────────────────────────────────────────

// The currently active view mode. Only ICON mode is functional in Phase 1,
// but the buttons for all four modes are drawn.
static ViewMode current_mode = VIEW_MODE_ICON;

// ── View mode button dimensions ─────────────────────────────────────

// Each view button is a small square in a button group on the left side
#define BTN_SIZE   22    // Width and height of each view button
#define BTN_GAP     0    // Gap between buttons (0 = touching, grouped)
#define BTN_LEFT   10    // Left margin before the first button
#define BTN_TOP     4    // Top margin within the toolbar

// ── Search field dimensions ─────────────────────────────────────────

#define SEARCH_W    180  // Width of the search field
#define SEARCH_H     20  // Height of the search field
#define SEARCH_RIGHT 10  // Right margin from window edge
#define SEARCH_RADIUS 10 // Corner radius (makes it pill-shaped)

// ── Helper: Draw a rounded rectangle path ───────────────────────────
//
// Used for the search field and button backgrounds. Creates the path
// but does NOT fill or stroke — the caller does that.

static void rounded_rect(cairo_t *cr, double x, double y,
                          double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0,          M_PI / 2);
    cairo_arc(cr, x + r,     y + h - r, r, M_PI / 2,   M_PI);
    cairo_arc(cr, x + r,     y + r,     r, M_PI,        3 * M_PI / 2);
    cairo_close_path(cr);
}

// ── Helper: Draw a single view mode button icon ─────────────────────
//
// Each button has a tiny symbol inside representing its view mode:
//   - Icon view: 4 small squares in a 2x2 grid
//   - List view: 3 horizontal lines
//   - Column view: 3 vertical columns
//   - Cover Flow: a rectangle with a horizontal line below

static void draw_view_icon(cairo_t *cr, int bx, int by, ViewMode mode)
{
    // Icon color: dark grey for the symbol
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_set_line_width(cr, 1.0);

    // Center point of the button
    double cx = bx + BTN_SIZE / 2.0;
    double cy = by + BTN_SIZE / 2.0;

    switch (mode) {
    case VIEW_MODE_ICON:
        // 2x2 grid of small squares (4 dots representing icons)
        for (int row = 0; row < 2; row++) {
            for (int col = 0; col < 2; col++) {
                double sx = cx - 5 + col * 7;
                double sy = cy - 5 + row * 7;
                cairo_rectangle(cr, sx, sy, 4, 4);
                cairo_fill(cr);
            }
        }
        break;

    case VIEW_MODE_LIST:
        // 3 horizontal lines representing list rows
        for (int i = 0; i < 3; i++) {
            double ly = cy - 5 + i * 5;
            cairo_move_to(cr, cx - 6, ly);
            cairo_line_to(cr, cx + 6, ly);
            cairo_stroke(cr);
        }
        break;

    case VIEW_MODE_COLUMN:
        // 3 vertical lines representing Miller columns
        for (int i = 0; i < 3; i++) {
            double lx = cx - 5 + i * 5;
            cairo_move_to(cr, lx, cy - 6);
            cairo_line_to(cr, lx, cy + 6);
            cairo_stroke(cr);
        }
        break;

    case VIEW_MODE_CFLOW:
        // A small rectangle (cover art) with a line below (list)
        cairo_rectangle(cr, cx - 5, cy - 6, 10, 7);
        cairo_stroke(cr);
        cairo_move_to(cr, cx - 5, cy + 4);
        cairo_line_to(cr, cx + 5, cy + 4);
        cairo_stroke(cr);
        break;

    default:
        break;
    }
}

// ── Helper: Draw the path breadcrumb ────────────────────────────────
//
// Converts "/home/user/Documents" into a breadcrumb like:
//   "Macintosh HD > Users > user > Documents"
// Drawn in the center of the toolbar between the view buttons and
// the search field.

static void draw_breadcrumb(FinderState *fs)
{
    cairo_t *cr = fs->cr;

    // Build the breadcrumb string from the path.
    // Replace "/" with " > " and replace the root with "Macintosh HD".
    char breadcrumb[1024] = {0};

    if (strcmp(fs->path, "/") == 0) {
        // Root directory
        strncpy(breadcrumb, "Macintosh HD", sizeof(breadcrumb) - 1);
    } else {
        // Start with "Macintosh HD" for the root component
        strncpy(breadcrumb, "Macintosh HD", sizeof(breadcrumb) - 1);

        // Walk through the path components and append each one
        char path_copy[1024];
        strncpy(path_copy, fs->path, sizeof(path_copy) - 1);

        char *token = strtok(path_copy, "/");
        while (token) {
            strncat(breadcrumb, " ▸ ", sizeof(breadcrumb) - strlen(breadcrumb) - 1);
            strncat(breadcrumb, token, sizeof(breadcrumb) - strlen(breadcrumb) - 1);
            token = strtok(NULL, "/");
        }
    }

    // Calculate position: after the view buttons, before the search field
    int text_x = BTN_LEFT + (BTN_SIZE * VIEW_MODE_COUNT) + 15;
    int text_y = 8;  // Vertically centered-ish in the 30px toolbar

    // Create a Pango layout for the breadcrumb text
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, breadcrumb, -1);

    PangoFontDescription *font = pango_font_description_from_string(
        "Lucida Grande 10");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    // Limit width so it doesn't overlap the search field
    int max_width = fs->win_w - text_x - SEARCH_W - SEARCH_RIGHT - 20;
    if (max_width > 0) {
        pango_layout_set_width(layout, max_width * PANGO_SCALE);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_MIDDLE);
    }

    // Draw in dark grey
    cairo_move_to(cr, text_x, text_y);
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
}

// ── Public API ──────────────────────────────────────────────────────

ViewMode toolbar_get_view_mode(void)
{
    return current_mode;
}

void toolbar_paint(FinderState *fs)
{
    cairo_t *cr = fs->cr;
    int w = fs->win_w;
    int h = fs->toolbar_h;

    // Save Cairo state so our clipping and transforms don't leak
    // into the sidebar/content painters.
    cairo_save(cr);

    // ── 1. Gradient background ──────────────────────────────────
    //
    // The Snow Leopard Finder toolbar has a subtle vertical gradient
    // from light grey at the top to slightly darker grey at the bottom.
    // This gives it the brushed-metal look.
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, h);
    cairo_pattern_add_color_stop_rgb(grad, 0.0,
        0xD8 / 255.0, 0xD8 / 255.0, 0xD8 / 255.0);  // #D8D8D8 top
    cairo_pattern_add_color_stop_rgb(grad, 1.0,
        0xC0 / 255.0, 0xC0 / 255.0, 0xC0 / 255.0);  // #C0C0C0 bottom

    cairo_rectangle(cr, 0, 0, w, h);
    cairo_set_source(cr, grad);
    cairo_fill(cr);
    cairo_pattern_destroy(grad);

    // ── 2. Bottom border line ───────────────────────────────────
    //
    // A 1px darker line at the bottom separates the toolbar from
    // the content/sidebar below it.
    cairo_set_source_rgb(cr, 0xA0 / 255.0, 0xA0 / 255.0, 0xA0 / 255.0);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, h - 0.5);
    cairo_line_to(cr, w, h - 0.5);
    cairo_stroke(cr);

    // ── 3. View mode buttons ────────────────────────────────────
    //
    // Four small buttons in a group on the left side of the toolbar.
    // The active button is drawn with a darker/pressed appearance.
    for (int i = 0; i < VIEW_MODE_COUNT; i++) {
        int bx = BTN_LEFT + i * (BTN_SIZE + BTN_GAP);
        int by = BTN_TOP;

        // Button background: pressed (active) or normal
        if (i == (int)current_mode) {
            // Active button: darker inset appearance
            cairo_set_source_rgb(cr, 0xA0 / 255.0, 0xA0 / 255.0, 0xA0 / 255.0);
        } else {
            // Inactive button: slightly lighter than toolbar
            cairo_set_source_rgb(cr, 0xC8 / 255.0, 0xC8 / 255.0, 0xC8 / 255.0);
        }
        cairo_rectangle(cr, bx, by, BTN_SIZE, BTN_SIZE);
        cairo_fill(cr);

        // Button border
        cairo_set_source_rgb(cr, 0x90 / 255.0, 0x90 / 255.0, 0x90 / 255.0);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, bx + 0.5, by + 0.5, BTN_SIZE - 1, BTN_SIZE - 1);
        cairo_stroke(cr);

        // Draw the mode icon inside the button
        draw_view_icon(cr, bx, by, (ViewMode)i);
    }

    // ── 4. Path breadcrumb ──────────────────────────────────────
    draw_breadcrumb(fs);

    // ── 5. Search field ─────────────────────────────────────────
    //
    // A rounded-rect input field on the right side with placeholder
    // text "Search". This is purely visual in Phase 1.
    {
        int sx = w - SEARCH_W - SEARCH_RIGHT;
        int sy = (h - SEARCH_H) / 2;

        // White fill with grey border
        rounded_rect(cr, sx, sy, SEARCH_W, SEARCH_H, SEARCH_RADIUS);
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0x99 / 255.0, 0x99 / 255.0, 0x99 / 255.0);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        // Magnifying glass icon (small circle + line)
        double mx = sx + 14;
        double my = sy + SEARCH_H / 2.0;
        cairo_set_source_rgb(cr, 0x88 / 255.0, 0x88 / 255.0, 0x88 / 255.0);
        cairo_set_line_width(cr, 1.5);
        cairo_arc(cr, mx, my - 1, 4, 0, 2 * M_PI);
        cairo_stroke(cr);
        cairo_move_to(cr, mx + 3, my + 2);
        cairo_line_to(cr, mx + 6, my + 5);
        cairo_stroke(cr);

        // Placeholder text "Search"
        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, "Search", -1);
        PangoFontDescription *font = pango_font_description_from_string(
            "Lucida Grande 11");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);

        cairo_move_to(cr, sx + 24, sy + 2);
        cairo_set_source_rgb(cr, 0x99 / 255.0, 0x99 / 255.0, 0x99 / 255.0);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }

    cairo_restore(cr);
}

bool toolbar_handle_click(FinderState *fs, int x, int y)
{
    (void)fs;

    // Check if the click landed on one of the view mode buttons
    for (int i = 0; i < VIEW_MODE_COUNT; i++) {
        int bx = BTN_LEFT + i * (BTN_SIZE + BTN_GAP);
        int by = BTN_TOP;

        if (x >= bx && x < bx + BTN_SIZE &&
            y >= by && y < by + BTN_SIZE) {
            // Switch to this view mode
            current_mode = (ViewMode)i;
            fprintf(stderr, "[toolbar] View mode changed to %d\n", i);
            return true;
        }
    }

    return false;  // Click wasn't on any button
}
