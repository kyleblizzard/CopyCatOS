// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// panes/appearance.c — Appearance preferences pane
// ============================================================================
//
// Today this pane carries the Modern/Classic menu-bar toggle. Long term it's
// where Snow Leopard's Appearance preferences live (highlight color,
// scroll-bar style, recent items, font smoothing) — those add here, not in
// dock.c.
//
// The toggle writes the _COPYCATOS_MENUBAR_MODE root-window atom as XA_STRING
// with exactly "modern" or "classic" and no trailing newline. menubar
// subscribes via PropertyNotify on that atom and reconciles its pane set —
// no SIGHUP, no restart.
// ============================================================================

#include "appearance.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

// Atom name must match menubar/src/menubar.h COPYCATOS_MENUBAR_MODE_ATOM_NAME.
// Kept as a literal here so systemcontrol doesn't have to pull the menubar
// header tree into its include path just for a single string.
#define COPYCATOS_MENUBAR_MODE_ATOM_NAME "_COPYCATOS_MENUBAR_MODE"

// ============================================================================
//  Layout constants (mirror controller.c's segmented-pill geometry)
// ============================================================================

#define LABEL_X            30
#define CONTENT_Y_START    (TOOLBAR_HEIGHT + 20)

#define PILL_X             120
#define PILL_W_TOTAL       260
#define PILL_H              24
#define PILL_SEGMENTS        2
#define PILL_SEG_W         (PILL_W_TOTAL / PILL_SEGMENTS)
#define PILL_CORNER_R       4.0

// ============================================================================
//  Module state
// ============================================================================

// Menu-bar mode as a local enum so the pane doesn't depend on menubar's
// header. Values are mapped to the exact "classic"/"modern" byte payloads
// when we write the atom.
typedef enum {
    APPEARANCE_MENUBAR_MODERN  = 0,
    APPEARANCE_MENUBAR_CLASSIC = 1,
} AppearanceMenuBarMode;

static AppearanceMenuBarMode current_mode = APPEARANCE_MENUBAR_MODERN;
static bool state_initialized = false;

// ============================================================================
//  Atom read / write
// ============================================================================

// Read _COPYCATOS_MENUBAR_MODE from the root window. Exactly "classic" (7
// bytes, XA_STRING) selects Classic; anything else — including an unset
// atom — means Modern, matching menubar.c:read_menubar_mode.
static AppearanceMenuBarMode read_menubar_mode_atom(Display *dpy, Window root)
{
    Atom atom = XInternAtom(dpy, COPYCATOS_MENUBAR_MODE_ATOM_NAME, False);
    if (atom == None) return APPEARANCE_MENUBAR_MODERN;

    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;

    int st = XGetWindowProperty(dpy, root, atom,
                                0, 64, False, XA_STRING,
                                &actual_type, &actual_format,
                                &nitems, &bytes_after, &data);

    AppearanceMenuBarMode mode = APPEARANCE_MENUBAR_MODERN;
    if (st == Success && data) {
        if (actual_type == XA_STRING && actual_format == 8 &&
            nitems == 7 && memcmp(data, "classic", 7) == 0) {
            mode = APPEARANCE_MENUBAR_CLASSIC;
        }
        XFree(data);
    }
    return mode;
}

// Publish the current mode on the root window. XA_STRING format 8, exact
// byte count, no trailing newline — menubar's read_menubar_mode does an
// exact memcmp so any extra byte silently flips the bar to Modern.
static void write_menubar_mode_atom(Display *dpy, Window root,
                                    AppearanceMenuBarMode mode)
{
    Atom atom = XInternAtom(dpy, COPYCATOS_MENUBAR_MODE_ATOM_NAME, False);
    if (atom == None) return;

    const unsigned char *payload;
    int nbytes;
    if (mode == APPEARANCE_MENUBAR_CLASSIC) {
        payload = (const unsigned char *)"classic";
        nbytes  = 7;
    } else {
        payload = (const unsigned char *)"modern";
        nbytes  = 6;
    }

    XChangeProperty(dpy, root, atom, XA_STRING, 8,
                    PropModeReplace, payload, nbytes);
    XFlush(dpy);
}

// ============================================================================
//  Pill drawing — 2-segment Modern/Classic toggle
// ============================================================================

// Build the closed path for one segment of a rounded pill. The leftmost
// segment rounds its left corners, the rightmost segment rounds its right
// corners; middle segments (unused here) would stay square.
static void pill_segment_path(cairo_t *cr, double x, double y, double w,
                              double h, double r, bool first, bool last)
{
    cairo_new_path(cr);
    if (first && last) {
        // Fully rounded single-segment — not the case here but safe.
        cairo_arc(cr, x + r,     y + r,     r, M_PI,      3 * M_PI / 2);
        cairo_arc(cr, x + w - r, y + r,     r, -M_PI / 2, 0);
        cairo_arc(cr, x + w - r, y + h - r, r, 0,         M_PI / 2);
        cairo_arc(cr, x + r,     y + h - r, r, M_PI / 2,  M_PI);
    } else if (first) {
        cairo_arc(cr, x + r, y + r,     r, M_PI,     3 * M_PI / 2);
        cairo_line_to(cr, x + w, y);
        cairo_line_to(cr, x + w, y + h);
        cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    } else if (last) {
        cairo_move_to(cr, x, y);
        cairo_arc(cr, x + w - r, y + r,     r, -M_PI / 2, 0);
        cairo_arc(cr, x + w - r, y + h - r, r, 0,         M_PI / 2);
        cairo_line_to(cr, x, y + h);
    } else {
        cairo_rectangle(cr, x, y, w, h);
    }
    cairo_close_path(cr);
}

// Draw one segment of the pill with its label. Selected segment gets the
// dark/recessed fill + bold white text (same treatment as controller.c's
// Desktop Mode / Desktop Gaming tabs).
static void draw_pill_segment(cairo_t *cr, double x, double y,
                              const char *label, bool selected,
                              bool first, bool last)
{
    pill_segment_path(cr, x, y, PILL_SEG_W, PILL_H, PILL_CORNER_R,
                      first, last);

    if (selected) {
        cairo_set_source_rgb(cr, 0.72, 0.72, 0.72);
    } else {
        cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
    }
    cairo_fill_preserve(cr);

    cairo_set_source_rgb(cr, 0.62, 0.62, 0.62);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, label, -1);
    PangoFontDescription *font = pango_font_description_from_string(
        selected ? "Lucida Grande Bold 11" : "Lucida Grande 11");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    int tw, th;
    pango_layout_get_pixel_size(layout, &tw, &th);

    if (selected) {
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    } else {
        cairo_set_source_rgb(cr, 0.20, 0.20, 0.20);
    }
    cairo_move_to(cr, x + (PILL_SEG_W - tw) / 2.0,
                      y + (PILL_H - th) / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

// ============================================================================
//  Hit testing — which segment is under (x, y)?
// ============================================================================

// Returns the segment index (0 = Modern, 1 = Classic) or -1 if outside
// the pill bounds.
static int hit_test_pill(int x, int y, double pill_y)
{
    if (y < pill_y || y > pill_y + PILL_H) return -1;
    if (x < PILL_X || x > PILL_X + PILL_W_TOTAL) return -1;
    int seg = (x - PILL_X) / PILL_SEG_W;
    if (seg < 0) seg = 0;
    if (seg >= PILL_SEGMENTS) seg = PILL_SEGMENTS - 1;
    return seg;
}

// Vertical offsets used to place the pill row below the title + separator.
// Kept as named constants so paint and hit-test read the same numbers —
// change one and both paths track.
#define TITLE_TO_SEPARATOR   30   // title baseline → separator y
#define SEPARATOR_TO_PILL    35   // separator y → pill top

// Y of the pill top inside the content rect. Shared by paint and hit-test
// so they can't drift.
static double pill_y_offset(void)
{
    return CONTENT_Y_START + TITLE_TO_SEPARATOR + SEPARATOR_TO_PILL;
}

// ============================================================================
//  Public API
// ============================================================================

void appearance_pane_paint(SysPrefsState *state)
{
    cairo_t *cr = state->cr;

    // First-paint: seed current_mode from the atom so the pill reflects
    // live state (Classic survives a relaunch of systemcontrol).
    if (!state_initialized) {
        current_mode = read_menubar_mode_atom(state->dpy, state->root);
        state_initialized = true;
    }

    // ── Title ──────────────────────────────────────────────────────
    PangoLayout *title = pango_cairo_create_layout(cr);
    pango_layout_set_text(title, "Appearance", -1);
    PangoFontDescription *title_font =
        pango_font_description_from_string("Lucida Grande Bold 15");
    pango_layout_set_font_description(title, title_font);
    pango_font_description_free(title_font);

    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_move_to(cr, LABEL_X, CONTENT_Y_START);
    pango_cairo_show_layout(cr, title);
    g_object_unref(title);

    // ── Separator line ─────────────────────────────────────────────
    double separator_y = CONTENT_Y_START + TITLE_TO_SEPARATOR;
    cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, LABEL_X, separator_y + 0.5);
    cairo_line_to(cr, LABEL_X + 540, separator_y + 0.5);
    cairo_stroke(cr);

    // ── "Menu Bar:" label + Modern/Classic pill ────────────────────
    double pill_y = pill_y_offset();
    PangoLayout *lbl = pango_cairo_create_layout(cr);
    pango_layout_set_text(lbl, "Menu Bar:", -1);
    PangoFontDescription *lbl_font =
        pango_font_description_from_string("Lucida Grande Bold 12");
    pango_layout_set_font_description(lbl, lbl_font);
    pango_font_description_free(lbl_font);

    int lbl_w, lbl_h;
    pango_layout_get_pixel_size(lbl, &lbl_w, &lbl_h);
    (void)lbl_w;
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_move_to(cr, LABEL_X, pill_y + (PILL_H - lbl_h) / 2.0);
    pango_cairo_show_layout(cr, lbl);
    g_object_unref(lbl);

    draw_pill_segment(cr, PILL_X,              pill_y, "Modern",
                      current_mode == APPEARANCE_MENUBAR_MODERN,
                      /* first */ true,  /* last */ false);
    draw_pill_segment(cr, PILL_X + PILL_SEG_W, pill_y, "Classic",
                      current_mode == APPEARANCE_MENUBAR_CLASSIC,
                      /* first */ false, /* last */ true);

    // ── Description text ───────────────────────────────────────────
    double desc_y = pill_y + PILL_H + 18;
    PangoLayout *desc = pango_cairo_create_layout(cr);
    pango_layout_set_text(desc,
        "Modern shows a menu bar on every connected display.\n"
        "Classic keeps a single menu bar on the primary display.",
        -1);
    PangoFontDescription *desc_font =
        pango_font_description_from_string("Lucida Grande 11");
    pango_layout_set_font_description(desc, desc_font);
    pango_font_description_free(desc_font);

    cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
    cairo_move_to(cr, LABEL_X, desc_y);
    pango_cairo_show_layout(cr, desc);
    g_object_unref(desc);
}

bool appearance_pane_click(SysPrefsState *state, int x, int y)
{
    int seg = hit_test_pill(x, y, pill_y_offset());
    if (seg < 0) return false;

    AppearanceMenuBarMode new_mode =
        (seg == 0) ? APPEARANCE_MENUBAR_MODERN : APPEARANCE_MENUBAR_CLASSIC;

    if (new_mode == current_mode) {
        // Clicked the already-selected segment — still a valid hit; repaint
        // is harmless and keeps behaviour uniform.
        return true;
    }

    current_mode = new_mode;
    write_menubar_mode_atom(state->dpy, state->root, current_mode);
    return true;
}

bool appearance_pane_motion(SysPrefsState *state, int x, int y)
{
    (void)state; (void)x; (void)y;
    return false;
}

void appearance_pane_release(SysPrefsState *state)
{
    (void)state;
}
