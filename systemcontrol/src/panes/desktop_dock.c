// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// panes/desktop_dock.c — Desktop & Dock preferences pane (multi-display)
// ============================================================================
//
// Two checkboxes, both defaulting ON. Toggling one writes
// ~/.config/copycatos/shell.conf and publishes the matching atom:
//   displays_separate_menu_bars → _COPYCATOS_MENUBAR_MODE ("modern"/"classic")
//   displays_separate_spaces    → _COPYCATOS_SPACES_MODE  ("per_display"/"global")
// menubar and moonrock subscribe via PropertyNotify and reconcile live.
// ============================================================================

#include "desktop_dock.h"
#include "../shellconf.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
//  Layout constants
// ============================================================================

#define LABEL_X           30
#define CONTENT_Y_START   (TOOLBAR_HEIGHT + 20)

// Title → separator → first checkbox
#define TITLE_TO_SEPARATOR   30
#define SEPARATOR_TO_SECTION 22
#define SECTION_TO_CHECKBOX  14
#define CHECKBOX_SPACING     28   // vertical gap between the two checkboxes

#define CHECKBOX_SIZE        14
#define CHECKBOX_HIT_W      360   // wide hit-rect so the label text is clickable

// ============================================================================
//  Module state — mirrors shell.conf
// ============================================================================
//
// Seeded lazily from disk on first paint so the pane reflects the actual
// persisted values (and whatever main.c already published to the atoms at
// startup). Re-reading the atoms on every open is wasteful and would defeat
// the point of the on-disk source of truth.
// ============================================================================

static ShellConf current = { true, true };
static bool      current_loaded = false;

// ============================================================================
//  Checkbox drawing (mirrors controller.c's Snow Leopard style)
// ============================================================================

static void draw_checkbox(cairo_t *cr, double x, double y,
                          bool checked, const char *label)
{
    double sz = CHECKBOX_SIZE;
    double r  = 2.0;

    // Box background — checked = SL selection blue, unchecked = near-white.
    cairo_set_source_rgb(cr, checked ? 0.22 : 0.96,
                             checked ? 0.47 : 0.96,
                             checked ? 0.84 : 0.96);
    cairo_new_path(cr);
    cairo_arc(cr, x + sz - r, y + r,      r, -M_PI / 2, 0);
    cairo_arc(cr, x + sz - r, y + sz - r, r,  0,         M_PI / 2);
    cairo_arc(cr, x + r,      y + sz - r, r,  M_PI / 2,  M_PI);
    cairo_arc(cr, x + r,      y + r,      r,  M_PI,      3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_fill(cr);

    // Box border
    cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
    cairo_set_line_width(cr, 1.0);
    cairo_new_path(cr);
    cairo_arc(cr, x + sz - r, y + r,      r, -M_PI / 2, 0);
    cairo_arc(cr, x + sz - r, y + sz - r, r,  0,         M_PI / 2);
    cairo_arc(cr, x + r,      y + sz - r, r,  M_PI / 2,  M_PI);
    cairo_arc(cr, x + r,      y + r,      r,  M_PI,      3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_stroke(cr);

    // Checkmark
    if (checked) {
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_set_line_width(cr, 2.0);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(cr, x + 3,  y + 7);
        cairo_line_to(cr, x + 6,  y + 11);
        cairo_line_to(cr, x + 11, y + 3);
        cairo_stroke(cr);
    }

    // Label to the right
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, label, -1);
    PangoFontDescription *font =
        pango_font_description_from_string("Lucida Grande 12");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_move_to(cr, x + sz + 8, y - 1);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

// ============================================================================
//  Layout math — single source of truth for paint and hit-test
// ============================================================================

static double checkbox_y(int index /* 0 or 1 */)
{
    double base = CONTENT_Y_START
                + TITLE_TO_SEPARATOR
                + SEPARATOR_TO_SECTION
                + SECTION_TO_CHECKBOX;
    return base + index * CHECKBOX_SPACING;
}

// Hit-test: checkboxes 0 and 1. Label is clickable too (generous hit rect
// on the x axis). Returns -1 if the click is outside both rows.
static int hit_test(int x, int y)
{
    if (x < LABEL_X || x > LABEL_X + CHECKBOX_HIT_W) return -1;
    for (int i = 0; i < 2; i++) {
        double top = checkbox_y(i);
        if (y >= top - 2 && y <= top + CHECKBOX_SIZE + 2) return i;
    }
    return -1;
}

// ============================================================================
//  Public API
// ============================================================================

void desktop_dock_pane_paint(SysPrefsState *state)
{
    cairo_t *cr = state->cr;

    // Lazy-seed from shell.conf the first time this pane paints.
    if (!current_loaded) {
        shellconf_load(&current);
        current_loaded = true;
    }

    // ── Title ───────────────────────────────────────────────────────
    PangoLayout *title = pango_cairo_create_layout(cr);
    pango_layout_set_text(title, "Desktop & Dock", -1);
    PangoFontDescription *title_font =
        pango_font_description_from_string("Lucida Grande Bold 15");
    pango_layout_set_font_description(title, title_font);
    pango_font_description_free(title_font);

    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_move_to(cr, LABEL_X, CONTENT_Y_START);
    pango_cairo_show_layout(cr, title);
    g_object_unref(title);

    // ── Separator ───────────────────────────────────────────────────
    double separator_y = CONTENT_Y_START + TITLE_TO_SEPARATOR;
    cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, LABEL_X, separator_y + 0.5);
    cairo_line_to(cr, LABEL_X + 540, separator_y + 0.5);
    cairo_stroke(cr);

    // ── Section label "Displays:" ───────────────────────────────────
    double section_y = separator_y + SEPARATOR_TO_SECTION - 16;
    PangoLayout *sec = pango_cairo_create_layout(cr);
    pango_layout_set_text(sec, "Displays:", -1);
    PangoFontDescription *sec_font =
        pango_font_description_from_string("Lucida Grande Bold 12");
    pango_layout_set_font_description(sec, sec_font);
    pango_font_description_free(sec_font);
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_move_to(cr, LABEL_X, section_y);
    pango_cairo_show_layout(cr, sec);
    g_object_unref(sec);

    // ── Checkboxes ──────────────────────────────────────────────────
    draw_checkbox(cr, LABEL_X, checkbox_y(0),
                  current.displays_separate_menu_bars,
                  "Displays have separate menu bars");
    draw_checkbox(cr, LABEL_X, checkbox_y(1),
                  current.displays_separate_spaces,
                  "Displays have separate Spaces");

    // ── Footer description ──────────────────────────────────────────
    double desc_y = checkbox_y(1) + CHECKBOX_SIZE + 22;
    PangoLayout *desc = pango_cairo_create_layout(cr);
    pango_layout_set_text(desc,
        "When on, each connected display shows its own menu bar and its "
        "own Spaces.\n"
        "When off, the primary display owns a single menu bar and a single "
        "Spaces set.",
        -1);
    PangoFontDescription *desc_font =
        pango_font_description_from_string("Lucida Grande 11");
    pango_layout_set_font_description(desc, desc_font);
    pango_font_description_free(desc_font);
    pango_layout_set_width(desc, (540) * PANGO_SCALE);
    pango_layout_set_wrap(desc, PANGO_WRAP_WORD);

    cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
    cairo_move_to(cr, LABEL_X, desc_y);
    pango_cairo_show_layout(cr, desc);
    g_object_unref(desc);
}

bool desktop_dock_pane_click(SysPrefsState *state, int x, int y)
{
    int which = hit_test(x, y);
    if (which < 0) return false;

    if (which == 0) {
        current.displays_separate_menu_bars = !current.displays_separate_menu_bars;
    } else {
        current.displays_separate_spaces = !current.displays_separate_spaces;
    }

    // Persist + publish. Even if one side of the pair fails we still want
    // the other to fire, so no short-circuit here.
    shellconf_save(&current);
    shellconf_publish_atoms(state->dpy, state->root, &current);
    return true;
}

bool desktop_dock_pane_motion(SysPrefsState *state, int x, int y)
{
    (void)state; (void)x; (void)y;
    return false;
}

void desktop_dock_pane_release(SysPrefsState *state)
{
    (void)state;
}
