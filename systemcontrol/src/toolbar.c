// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// toolbar.c — Unified toolbar rendering
// ============================================================================
//
// Draws the toolbar at the top of the System Preferences content area.
// The toolbar has a subtle gradient that matches Snow Leopard's unified
// title bar + toolbar appearance. It contains:
//   - "Show All" button with a 4-dot grid icon (always visible)
//   - Search field (right side, pill-shaped input)
//
// In pane view, back/forward arrows appear to the right of Show All.
// ============================================================================

#include "toolbar.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

// Toolbar layout constants
#define TB_GRADIENT_TOP      0.91    // #E8E8E8
#define TB_GRADIENT_BOT      0.82    // #D0D0D0
#define TB_BORDER_COLOR      0.69    // #B0B0B0

#define SHOW_ALL_X           12
#define SHOW_ALL_Y            4
#define SHOW_ALL_W           58
#define SHOW_ALL_H           30

#define SEARCH_W            180
#define SEARCH_H             22
#define SEARCH_RIGHT_PAD     12
#define SEARCH_RADIUS        11

// ============================================================================
// Helper — draw a rounded rectangle path (reused across the project)
// ============================================================================
static void rounded_rect(cairo_t *cr,
                          double x, double y,
                          double w, double h,
                          double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -M_PI / 2.0, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0,            M_PI / 2.0);
    cairo_arc(cr, x + r,     y + h - r, r, M_PI / 2.0,   M_PI);
    cairo_arc(cr, x + r,     y + r,     r, M_PI,         3.0 * M_PI / 2.0);
    cairo_close_path(cr);
}

// ============================================================================
// draw_show_all_icon — Paint a 4x4 grid of small squares (the "Show All" icon)
// ============================================================================
static void draw_show_all_icon(cairo_t *cr, double cx, double cy)
{
    // 4 squares in a 2x2 grid, representing the overview grid
    double size = 3.0;
    double gap = 2.0;
    double total = size * 2 + gap;
    double start_x = cx - total / 2.0;
    double start_y = cy - total / 2.0;

    cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);

    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < 2; col++) {
            cairo_rectangle(cr,
                start_x + col * (size + gap),
                start_y + row * (size + gap),
                size, size);
            cairo_fill(cr);
        }
    }
}

// ============================================================================
// toolbar_paint — Render the full toolbar
// ============================================================================
void toolbar_paint(SysPrefsState *state)
{
    cairo_t *cr = state->cr;
    int w = state->win_w;

    // ── Background gradient ──────────────────────────────────────────────
    // Subtle gray gradient matching Snow Leopard's unified toolbar
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, TOOLBAR_HEIGHT);
    cairo_pattern_add_color_stop_rgb(grad, 0.0, TB_GRADIENT_TOP, TB_GRADIENT_TOP, TB_GRADIENT_TOP);
    cairo_pattern_add_color_stop_rgb(grad, 1.0, TB_GRADIENT_BOT, TB_GRADIENT_BOT, TB_GRADIENT_BOT);
    cairo_set_source(cr, grad);
    cairo_rectangle(cr, 0, 0, w, TOOLBAR_HEIGHT);
    cairo_fill(cr);
    cairo_pattern_destroy(grad);

    // ── Bottom border line ──────────────────────────────────────────────
    cairo_set_source_rgb(cr, TB_BORDER_COLOR, TB_BORDER_COLOR, TB_BORDER_COLOR);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, TOOLBAR_HEIGHT - 0.5);
    cairo_line_to(cr, w, TOOLBAR_HEIGHT - 0.5);
    cairo_stroke(cr);

    // ── "Show All" button ───────────────────────────────────────────────
    double btn_x = SHOW_ALL_X;
    double btn_y = SHOW_ALL_Y;
    double btn_w = SHOW_ALL_W;
    double btn_h = SHOW_ALL_H;

    // Hover highlight
    if (state->toolbar_hover == 0) {
        rounded_rect(cr, btn_x, btn_y, btn_w, btn_h, 4.0);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.08);
        cairo_fill(cr);
    }

    // Grid icon (centered in the button area, offset upward for text below)
    draw_show_all_icon(cr, btn_x + btn_w / 2.0, btn_y + 10);

    // "Show All" text below the icon
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, "Show All", -1);
    PangoFontDescription *font = pango_font_description_from_string("Lucida Grande 9");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    int text_w, text_h;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);

    cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
    cairo_move_to(cr, btn_x + (btn_w - text_w) / 2.0, btn_y + 18);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);

    // ── Search field ────────────────────────────────────────────────────
    double search_x = w - SEARCH_W - SEARCH_RIGHT_PAD;
    double search_y = (TOOLBAR_HEIGHT - SEARCH_H) / 2.0;

    // Field background (white with gray border, pill-shaped)
    rounded_rect(cr, search_x, search_y, SEARCH_W, SEARCH_H, SEARCH_RADIUS);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.72, 0.72, 0.72);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    // Magnifying glass icon (simple circle + line)
    double mag_x = search_x + 14;
    double mag_y = search_y + SEARCH_H / 2.0;
    double mag_r = 4.5;

    cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
    cairo_arc(cr, mag_x, mag_y - 1, mag_r, 0, 2 * M_PI);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);

    // Handle of the magnifying glass
    cairo_move_to(cr, mag_x + mag_r * 0.7, mag_y - 1 + mag_r * 0.7);
    cairo_line_to(cr, mag_x + mag_r + 3, mag_y - 1 + mag_r + 3);
    cairo_stroke(cr);

    // Placeholder text
    if (state->search_query[0] == '\0') {
        PangoLayout *search_layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(search_layout, "Search", -1);
        PangoFontDescription *search_font = pango_font_description_from_string("Lucida Grande 11");
        pango_layout_set_font_description(search_layout, search_font);
        pango_font_description_free(search_font);

        cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
        cairo_move_to(cr, search_x + 26, search_y + 3);
        pango_cairo_show_layout(cr, search_layout);
        g_object_unref(search_layout);
    }
}

// ============================================================================
// toolbar_handle_click — Process a click inside the toolbar area
// ============================================================================
bool toolbar_handle_click(SysPrefsState *state, int x, int y)
{
    (void)y;

    // Check "Show All" button
    if (x >= SHOW_ALL_X && x <= SHOW_ALL_X + SHOW_ALL_W &&
        y >= SHOW_ALL_Y && y <= SHOW_ALL_Y + SHOW_ALL_H) {
        sysprefs_show_all(state);
        return true;
    }

    return false;
}

// ============================================================================
// toolbar_handle_motion — Update toolbar hover state
// ============================================================================
bool toolbar_handle_motion(SysPrefsState *state, int x, int y)
{
    int old_hover = state->toolbar_hover;

    // Check "Show All" button bounds
    if (x >= SHOW_ALL_X && x <= SHOW_ALL_X + SHOW_ALL_W &&
        y >= SHOW_ALL_Y && y <= SHOW_ALL_Y + SHOW_ALL_H) {
        state->toolbar_hover = 0;
    } else {
        state->toolbar_hover = -1;
    }

    return state->toolbar_hover != old_hover;
}
