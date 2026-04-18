// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// icongrid.c — Category-grouped icon grid
// ============================================================================
//
// Renders the main System Preferences view: a grid of 32x32 icons organized
// into categories (Personal, Hardware, Internet & Wireless, System). Each
// category gets a gray text header with a horizontal separator line below it.
//
// Layout matches the real Snow Leopard System Preferences pixel-for-pixel:
//   - Category headers: Lucida Grande 11pt, #808080
//   - Separator: 1px #C8C8C8
//   - Icon cells: 80x76, icon 32x32 centered, label 10pt below
//   - Hover: rounded-rect highlight with selection blue tint
// ============================================================================

#include "icongrid.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

// ============================================================================
// Helper — draw a rounded rectangle path
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
// get_cell_rect — Compute the position of a specific icon cell
// ============================================================================
//
// Walks through all categories and panes to find the screen rectangle for
// pane at the given index. This is used by both painting and hit testing.
//
// Returns the y-offset of the cell's top edge, or -1 if out of bounds.
// Sets *out_x, *out_y, *out_w, *out_h to the cell rectangle.
// ============================================================================
static bool get_cell_rect(SysPrefsState *state, int pane_index,
                          double *out_x, double *out_y,
                          double *out_w, double *out_h)
{
    double y_cursor = TOOLBAR_HEIGHT + GRID_TOP_PAD;

    for (int c = 0; c < state->category_count; c++) {
        CategoryInfo *cat = &state->categories[c];

        // Category header height
        y_cursor += HEADER_HEIGHT + SEPARATOR_GAP;

        // Walk through panes in this category
        for (int p = 0; p < cat->pane_count; p++) {
            int idx = cat->first_pane + p;
            int col = p % 8;  // Up to 8 icons per row

            // Start a new row if needed
            int row = p / 8;

            double cell_x = GRID_LEFT_PAD + col * CELL_WIDTH;
            double cell_y = y_cursor + row * CELL_HEIGHT;

            if (idx == pane_index) {
                *out_x = cell_x;
                *out_y = cell_y;
                *out_w = CELL_WIDTH;
                *out_h = CELL_HEIGHT;
                return true;
            }
        }

        // Advance past all rows for this category
        int rows = (cat->pane_count + 7) / 8;
        y_cursor += rows * CELL_HEIGHT + CATEGORY_GAP;
    }

    return false;
}

// ============================================================================
// icongrid_paint — Render the full icon grid with categories
// ============================================================================
void icongrid_paint(SysPrefsState *state)
{
    cairo_t *cr = state->cr;
    double y_cursor = TOOLBAR_HEIGHT + GRID_TOP_PAD;

    for (int c = 0; c < state->category_count; c++) {
        CategoryInfo *cat = &state->categories[c];

        // ── Category header text ────────────────────────────────────────
        PangoLayout *header = pango_cairo_create_layout(cr);
        pango_layout_set_text(header, cat->label, -1);
        PangoFontDescription *header_font =
            pango_font_description_from_string("Lucida Grande Bold 11");
        pango_layout_set_font_description(header, header_font);
        pango_font_description_free(header_font);

        cairo_set_source_rgb(cr, 0x80 / 255.0, 0x80 / 255.0, 0x80 / 255.0);
        cairo_move_to(cr, GRID_LEFT_PAD, y_cursor);
        pango_cairo_show_layout(cr, header);

        int header_text_h;
        pango_layout_get_pixel_size(header, NULL, &header_text_h);
        g_object_unref(header);

        // ── Separator line ──────────────────────────────────────────────
        double sep_y = y_cursor + header_text_h + 2;
        cairo_set_source_rgb(cr, 0xC8 / 255.0, 0xC8 / 255.0, 0xC8 / 255.0);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, GRID_LEFT_PAD, sep_y + 0.5);
        cairo_line_to(cr, state->win_w - GRID_LEFT_PAD, sep_y + 0.5);
        cairo_stroke(cr);

        y_cursor += HEADER_HEIGHT + SEPARATOR_GAP;

        // ── Icon cells ──────────────────────────────────────────────────
        for (int p = 0; p < cat->pane_count; p++) {
            int idx = cat->first_pane + p;
            PaneInfo *pane = &state->panes[idx];

            int col = p % 8;
            int row = p / 8;

            double cell_x = GRID_LEFT_PAD + col * CELL_WIDTH;
            double cell_y = y_cursor + row * CELL_HEIGHT;

            // ── Hover highlight ─────────────────────────────────────────
            if (idx == state->hover_pane) {
                rounded_rect(cr, cell_x + 2, cell_y + 1,
                             CELL_WIDTH - 4, CELL_HEIGHT - 3, 6.0);
                // Subtle blue highlight (matches Snow Leopard selection tint)
                cairo_set_source_rgba(cr,
                    0x38 / 255.0, 0x75 / 255.0, 0xD7 / 255.0, 0.15);
                cairo_fill_preserve(cr);
                cairo_set_source_rgba(cr,
                    0x38 / 255.0, 0x75 / 255.0, 0xD7 / 255.0, 0.30);
                cairo_set_line_width(cr, 1.0);
                cairo_stroke(cr);
            }

            // ── Icon (32x32, centered in cell) ──────────────────────────
            if (pane->icon_32) {
                double icon_x = cell_x + (CELL_WIDTH - ICON_SIZE) / 2.0;
                double icon_y = cell_y + 6;

                // Paint the icon within a clipped rectangle so it doesn't
                // fill the entire window surface
                cairo_save(cr);
                cairo_rectangle(cr, icon_x, icon_y, ICON_SIZE, ICON_SIZE);
                cairo_clip(cr);
                cairo_set_source_surface(cr, pane->icon_32, icon_x, icon_y);
                cairo_paint(cr);
                cairo_restore(cr);
            }

            // ── Label (centered below icon) ─────────────────────────────
            PangoLayout *label = pango_cairo_create_layout(cr);
            pango_layout_set_text(label, pane->name, -1);
            PangoFontDescription *label_font =
                pango_font_description_from_string("Lucida Grande 10");
            pango_layout_set_font_description(label, label_font);
            pango_font_description_free(label_font);

            // Center the text and allow wrapping for multi-line names
            pango_layout_set_width(label, (CELL_WIDTH - 4) * PANGO_SCALE);
            pango_layout_set_alignment(label, PANGO_ALIGN_CENTER);

            int label_w, label_h;
            pango_layout_get_pixel_size(label, &label_w, &label_h);

            cairo_set_source_rgb(cr, 0x33 / 255.0, 0x33 / 255.0, 0x33 / 255.0);
            cairo_move_to(cr, cell_x + 2, cell_y + 6 + ICON_SIZE + 4);
            pango_cairo_show_layout(cr, label);
            g_object_unref(label);
        }

        // Advance past all rows for this category
        int rows = (cat->pane_count + 7) / 8;
        y_cursor += rows * CELL_HEIGHT + CATEGORY_GAP;
    }
}

// ============================================================================
// icongrid_hit_test — Find which pane the cursor is over
// ============================================================================
int icongrid_hit_test(SysPrefsState *state, int x, int y)
{
    for (int i = 0; i < state->pane_count; i++) {
        double cx, cy, cw, ch;
        if (get_cell_rect(state, i, &cx, &cy, &cw, &ch)) {
            if (x >= cx && x < cx + cw && y >= cy && y < cy + ch) {
                return i;
            }
        }
    }
    return -1;
}

// ============================================================================
// icongrid_handle_click — Process a click in the icon grid
// ============================================================================
bool icongrid_handle_click(SysPrefsState *state, int x, int y)
{
    int hit = icongrid_hit_test(state, x, y);
    if (hit >= 0) {
        sysprefs_open_pane(state, hit);
        return true;
    }
    return false;
}

// ============================================================================
// icongrid_handle_motion — Update hover state for mouse movement
// ============================================================================
bool icongrid_handle_motion(SysPrefsState *state, int x, int y)
{
    int old_hover = state->hover_pane;
    state->hover_pane = icongrid_hit_test(state, x, y);
    return state->hover_pane != old_hover;
}
