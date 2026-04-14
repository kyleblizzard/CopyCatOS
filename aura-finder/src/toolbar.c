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
// The visual style matches the real Snow Leopard Finder toolbar,
// measured from an actual machine:
//   - Multi-stop gradient: 191 (top) → 154 (separator dip) → 170→168 (bottom)
//   - 1px dark bottom separator (brightness 140)
//   - Rounded-rect view mode buttons (~20x18px each)
//   - Rounded-rect search field with magnifying glass icon

#define _GNU_SOURCE  // For M_PI

#include "toolbar.h"
#include "content.h"

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

// Each view button is a small rounded rect in a button group on the left side.
// Real Snow Leopard buttons are ~20x18px each, grouped tightly.
#define BTN_W      20    // Width of each view button
#define BTN_H      18    // Height of each view button
#define BTN_GAP     0    // Gap between buttons (0 = touching, grouped)
#define BTN_LEFT   10    // Left margin before the first button
#define BTN_TOP     6    // Top margin within the toolbar
#define BTN_RADIUS  3    // Corner radius for button rounded rects

// ── Search field dimensions ─────────────────────────────────────────

#define SEARCH_W    180  // Width of the search field
#define SEARCH_H     22  // Height of the search field (measured from real SL)
#define SEARCH_RIGHT 10  // Right margin from window edge
#define SEARCH_RADIUS 11 // Corner radius (pill shape, half of height)

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
    double cx = bx + BTN_W / 2.0;
    double cy = by + BTN_H / 2.0;

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
    int text_x = BTN_LEFT + (BTN_W * VIEW_MODE_COUNT) + 15;
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
    // Snow Leopard Finder toolbar gradient measured from a real machine:
    //   y=0:  brightness 191 (top of toolbar)
    //   y=2:  brightness 190
    //   y=3:  brightness 154 (1px dark separator dip)
    //   y=85%: brightness 170 (lower body)
    //   y=100%: brightness 168 (very bottom, blends into frame body)
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, h);
    cairo_pattern_add_color_stop_rgb(grad, 0.0,
        191 / 255.0, 191 / 255.0, 191 / 255.0);  // top
    cairo_pattern_add_color_stop_rgb(grad, 0.1,
        190 / 255.0, 190 / 255.0, 190 / 255.0);
    cairo_pattern_add_color_stop_rgb(grad, 0.15,
        154 / 255.0, 154 / 255.0, 154 / 255.0);  // separator dip
    cairo_pattern_add_color_stop_rgb(grad, 0.85,
        170 / 255.0, 170 / 255.0, 170 / 255.0);  // bottom body
    cairo_pattern_add_color_stop_rgb(grad, 1.0,
        168 / 255.0, 168 / 255.0, 168 / 255.0);  // very bottom

    cairo_rectangle(cr, 0, 0, w, h);
    cairo_set_source(cr, grad);
    cairo_fill(cr);
    cairo_pattern_destroy(grad);

    // ── 2. Bottom border line ───────────────────────────────────
    //
    // A 1px dark separator at the very bottom of the toolbar, measured
    // brightness ~140 from the real Snow Leopard Finder.
    cairo_set_source_rgb(cr, 140 / 255.0, 140 / 255.0, 140 / 255.0);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, h - 0.5);
    cairo_line_to(cr, w, h - 0.5);
    cairo_stroke(cr);

    // ── 3. View mode buttons ────────────────────────────────────
    //
    // Four small rounded-rect buttons (~20x18px each) grouped on the
    // left side. The active button has a darker pressed appearance.
    // Matches the real Snow Leopard Finder view toggle group.
    for (int i = 0; i < VIEW_MODE_COUNT; i++) {
        int bx = BTN_LEFT + i * (BTN_W + BTN_GAP);
        int by = BTN_TOP;

        // Button background: pressed (active) or normal
        if (i == (int)current_mode) {
            // Active button: darker inset appearance (brightness ~101)
            cairo_set_source_rgb(cr, 101 / 255.0, 101 / 255.0, 101 / 255.0);
        } else {
            // Inactive button: slightly lighter than toolbar body
            cairo_set_source_rgb(cr, 170 / 255.0, 170 / 255.0, 170 / 255.0);
        }
        rounded_rect(cr, bx, by, BTN_W, BTN_H, BTN_RADIUS);
        cairo_fill(cr);

        // Button border: subtle 1px outline
        cairo_set_source_rgb(cr, 120 / 255.0, 120 / 255.0, 120 / 255.0);
        cairo_set_line_width(cr, 1.0);
        rounded_rect(cr, bx + 0.5, by + 0.5, BTN_W - 1, BTN_H - 1, BTN_RADIUS);
        cairo_stroke(cr);

        // Draw the mode icon inside the button.
        // Use white icons on the active (dark) button, dark icons otherwise.
        if (i == (int)current_mode) {
            cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
        }
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
        int bx = BTN_LEFT + i * (BTN_W + BTN_GAP);
        int by = BTN_TOP;

        if (x >= bx && x < bx + BTN_W &&
            y >= by && y < by + BTN_H) {
            // Switch to this view mode in both toolbar and content
            current_mode = (ViewMode)i;
            content_set_view_mode((ViewMode)i);
            fprintf(stderr, "[toolbar] View mode changed to %d\n", i);
            return true;
        }
    }

    return false;  // Click wasn't on any button
}
