// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// panes/stub.c — Generic "not yet available" placeholder pane
// ============================================================================
//
// Shows a centered layout with:
//   - 128x128 pane icon (or fallback circle with first letter)
//   - Pane name in bold
//   - "This preference pane is not yet available." in gray
//
// This serves as the default view for all panes until functional
// implementations are built.
// ============================================================================

#include "stub.h"
#include "../registry.h"

#include <string.h>

// ============================================================================
// stub_paint — Render the stub pane
// ============================================================================
void stub_paint(SysPrefsState *state, int pane_index)
{
    cairo_t *cr = state->cr;
    PaneInfo *pane = &state->panes[pane_index];

    // Lazy-load the 128x128 icon if needed
    registry_load_icon_128(state, pane_index);

    // Content area starts below the toolbar
    double content_y = TOOLBAR_HEIGHT;
    double content_h = state->win_h - TOOLBAR_HEIGHT;
    double center_x = state->win_w / 2.0;
    double center_y = content_y + content_h / 2.0 - 40;

    // ── 128x128 icon ────────────────────────────────────────────────────
    if (pane->icon_128) {
        double icon_x = center_x - 64;
        double icon_y = center_y - 80;

        cairo_set_source_surface(cr, pane->icon_128, icon_x, icon_y);
        cairo_paint(cr);
    } else {
        // Fallback: draw a gray circle with the first letter of the pane name
        cairo_arc(cr, center_x, center_y - 16, 48, 0, 2 * 3.14159);
        cairo_set_source_rgb(cr, 0.82, 0.82, 0.82);
        cairo_fill(cr);

        // First letter
        char letter[2] = {pane->name[0], '\0'};
        PangoLayout *letter_layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(letter_layout, letter, -1);
        PangoFontDescription *letter_font =
            pango_font_description_from_string("Lucida Grande Bold 32");
        pango_layout_set_font_description(letter_layout, letter_font);
        pango_font_description_free(letter_font);

        int lw, lh;
        pango_layout_get_pixel_size(letter_layout, &lw, &lh);
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_move_to(cr, center_x - lw / 2.0, center_y - 16 - lh / 2.0);
        pango_cairo_show_layout(cr, letter_layout);
        g_object_unref(letter_layout);
    }

    // ── Pane name (bold, centered) ──────────────────────────────────────
    // Use only the first line of the name (strip newlines for display)
    char display_name[128];
    strncpy(display_name, pane->name, sizeof(display_name) - 1);
    display_name[sizeof(display_name) - 1] = '\0';
    // Replace newlines with spaces for the title display
    for (char *p = display_name; *p; p++) {
        if (*p == '\n') *p = ' ';
    }

    PangoLayout *name_layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(name_layout, display_name, -1);
    PangoFontDescription *name_font =
        pango_font_description_from_string("Lucida Grande Bold 17");
    pango_layout_set_font_description(name_layout, name_font);
    pango_font_description_free(name_font);
    pango_layout_set_alignment(name_layout, PANGO_ALIGN_CENTER);

    int name_w, name_h;
    pango_layout_get_pixel_size(name_layout, &name_w, &name_h);

    cairo_set_source_rgb(cr, 0x1A / 255.0, 0x1A / 255.0, 0x1A / 255.0);
    cairo_move_to(cr, center_x - name_w / 2.0, center_y + 60);
    pango_cairo_show_layout(cr, name_layout);
    g_object_unref(name_layout);

    // ── "Not yet available" text ────────────────────────────────────────
    PangoLayout *msg_layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(msg_layout,
        "This preference pane is not yet available.", -1);
    PangoFontDescription *msg_font =
        pango_font_description_from_string("Lucida Grande 13");
    pango_layout_set_font_description(msg_layout, msg_font);
    pango_font_description_free(msg_font);

    int msg_w, msg_h;
    pango_layout_get_pixel_size(msg_layout, &msg_w, &msg_h);
    (void)msg_h;

    cairo_set_source_rgb(cr, 0x73 / 255.0, 0x73 / 255.0, 0x73 / 255.0);
    cairo_move_to(cr, center_x - msg_w / 2.0, center_y + 60 + name_h + 8);
    pango_cairo_show_layout(cr, msg_layout);
    g_object_unref(msg_layout);
}
