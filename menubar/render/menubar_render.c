// CopyCatOS — by Kyle Blizzard at Blizzard.show

// menubar_render.c — Shared Aqua chrome rendering bodies.
//
// Slice 19.B: extracted verbatim from menubar/src/render.c. The daemon's
// render.c is now a thin forwarder that plugs the global `menubar_scale`
// and MENUBAR_HEIGHT into the explicit-scale signatures here. Behavior
// is identical to the pre-19.B daemon — same texture path, same Pango
// font name + size formula, same gradient stops, same hover-pill alpha,
// same vertical-centering math.
//
// Two extra surfaces live here that the daemon never used:
// menubar_render_layout_menus / paint_title_bar / paint_menu_bar are
// for moonbase-launcher's foreign-distro chrome stub (slice 19.D). The
// daemon's existing paint flow stays in the daemon — no refactor of
// paint_pane / appmenu / dropdowns is in this slice.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // M_PI in math.h under strict C11
#endif

#include "menubar_render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

// ---------------------------------------------------------------------------
// Module state — single texture surface shared by every caller in-process
// ---------------------------------------------------------------------------

static cairo_surface_t *bg_texture = NULL;

// Per-item horizontal padding for the high-level menu-bar layout.
// Split evenly between left and right of the title — layout records
// the slot left edge plus half-pad inset, paint redoes the same
// half-pad inset so the two stay consistent without re-measuring text.
static const int MENU_ITEM_PAD_POINTS = 16;

// ---------------------------------------------------------------------------
// Heights in points
// ---------------------------------------------------------------------------

int menubar_render_title_bar_height_pts(void)
{
    return MENUBAR_RENDER_TITLE_BAR_POINTS;
}

int menubar_render_menu_bar_height_pts(void)
{
    return MENUBAR_RENDER_MENU_BAR_POINTS;
}

// ---------------------------------------------------------------------------
// Font scaling — proportional to the effective scale, never below base
// ---------------------------------------------------------------------------

// Build a Pango font description string with a proportionally-scaled
// size. Identical formula to the daemon's old scaled_font(). Returned
// pointer is a static buffer — only valid until the next call. Pango
// parses + copies the string immediately, so the buffer only needs to
// live for the duration of pango_font_description_from_string().
static char *scaled_font(const char *base_name, int base_size, double scale)
{
    static char buf[128];
    int scaled_size = (int)(base_size * scale + 0.5);
    if (scaled_size < base_size) scaled_size = base_size; // never below base
    snprintf(buf, sizeof(buf), "%s %d", base_name, scaled_size);
    return buf;
}

// Variant that lets the caller pick the base point size (11 / 12 / 13).
// 13 is the menu bar / dropdown label baseline. Submenu arrows use 11
// and shortcut glyphs use 12 — those callers live inside this file
// (menubar_render_paint_menu_item) so the size knob stays internal.
static PangoLayout *create_text_layout_sized(cairo_t *cr, const char *text,
                                             bool bold, int base_size,
                                             double scale)
{
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text, -1);

    PangoFontDescription *desc = pango_font_description_from_string(
        bold ? scaled_font("Lucida Grande Bold", base_size, scale)
             : scaled_font("Lucida Grande",      base_size, scale)
    );
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    return layout;
}

static PangoLayout *create_text_layout(cairo_t *cr, const char *text,
                                       bool bold, double scale)
{
    return create_text_layout_sized(cr, text, bold, 13, scale);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void menubar_render_init(void)
{
    if (bg_texture) return;  // idempotent across multiple callers

    const char *home = getenv("HOME");
    if (!home) return;

    char path[512];
    snprintf(path, sizeof(path),
             "%s/.local/share/aqua-widgets/menubar/menubar_bg.png", home);

    bg_texture = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(bg_texture) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "[menubar-render] WARNING: could not load %s\n", path);
        cairo_surface_destroy(bg_texture);
        bg_texture = NULL;
    } else {
        fprintf(stderr, "[menubar-render] loaded menubar_bg.png (%dx%d)\n",
                cairo_image_surface_get_width(bg_texture),
                cairo_image_surface_get_height(bg_texture));
    }
}

void menubar_render_cleanup(void)
{
    if (bg_texture) {
        cairo_surface_destroy(bg_texture);
        bg_texture = NULL;
    }
}

// ---------------------------------------------------------------------------
// Background
// ---------------------------------------------------------------------------

void menubar_render_background(cairo_t *cr, int width_px, int height_px,
                               menubar_render_theme_t theme)
{
    // TODO(19.F): host-theme variants tint the gradient stops to match
    // KDE Breeze / GNOME Adwaita. For now every theme renders Aqua.
    (void)theme;

    if (bg_texture) {
        // Real Snow Leopard menubar_bg.png (400×22 RGBA). Tile X,
        // scale Y so the 22px texture fills the requested height_px.
        int tex_h = cairo_image_surface_get_height(bg_texture);

        cairo_pattern_t *pattern = cairo_pattern_create_for_surface(bg_texture);
        cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);

        cairo_matrix_t matrix;
        cairo_matrix_init_scale(&matrix, 1.0, (double)tex_h / height_px);
        cairo_pattern_set_matrix(pattern, &matrix);

        cairo_set_source(cr, pattern);
        cairo_rectangle(cr, 0, 0, width_px, height_px);
        cairo_fill(cr);
        cairo_pattern_destroy(pattern);
        return;
    }

    // Fallback: canonical 4-stop gradient from CLAUDE.md
    // (#F2F2F2 → #E8E8E8 → #D7D7D7 → #D2D2D2) + 1px bottom border #A8A8A8.
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, height_px);
    cairo_pattern_add_color_stop_rgb(grad, 0.0, 242/255.0, 242/255.0, 242/255.0);
    cairo_pattern_add_color_stop_rgb(grad, 0.3, 232/255.0, 232/255.0, 232/255.0);
    cairo_pattern_add_color_stop_rgb(grad, 0.8, 215/255.0, 215/255.0, 215/255.0);
    cairo_pattern_add_color_stop_rgb(grad, 1.0, 210/255.0, 210/255.0, 210/255.0);
    cairo_set_source(cr, grad);
    cairo_rectangle(cr, 0, 0, width_px, height_px);
    cairo_fill(cr);
    cairo_pattern_destroy(grad);

    cairo_set_source_rgb(cr, 168/255.0, 168/255.0, 168/255.0);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, height_px - 0.5);
    cairo_line_to(cr, width_px, height_px - 0.5);
    cairo_stroke(cr);
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

double menubar_render_text(cairo_t *cr, const char *text,
                           double x, double y, bool bold,
                           double r, double g, double b, double scale)
{
    PangoLayout *layout = create_text_layout(cr, text, bold, scale);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);

    int width, height;
    pango_layout_get_pixel_size(layout, &width, &height);
    g_object_unref(layout);
    return (double)width;
}

double menubar_render_measure_text(const char *text, bool bold, double scale)
{
    cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(tmp);

    PangoLayout *layout = create_text_layout(cr, text, bold, scale);

    int width, height;
    pango_layout_get_pixel_size(layout, &width, &height);

    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(tmp);
    return (double)width;
}

int menubar_render_text_center_y(const char *text, bool bold,
                                 int bar_height_px, double scale)
{
    cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(tmp);

    PangoLayout *layout = create_text_layout(cr, text, bold, scale);

    int width, height;
    pango_layout_get_pixel_size(layout, &width, &height);

    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(tmp);

    int y = (bar_height_px - height) / 2;
    if (y < 0) y = 0;
    return y;
}

// ---------------------------------------------------------------------------
// Hover pill — rounded dark overlay
// ---------------------------------------------------------------------------

static void rounded_rect(cairo_t *cr, double x, double y,
                         double w, double h, double radius)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - radius, y + radius,     radius, -M_PI / 2, 0);
    cairo_arc(cr, x + w - radius, y + h - radius, radius, 0,          M_PI / 2);
    cairo_arc(cr, x + radius,     y + h - radius, radius, M_PI / 2,   M_PI);
    cairo_arc(cr, x + radius,     y + radius,     radius, M_PI,       3 * M_PI / 2);
    cairo_close_path(cr);
}

void menubar_render_hover_highlight(cairo_t *cr, int x, int y, int w, int h,
                                    double scale)
{
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.1);
    rounded_rect(cr, x, y, w, h, 3.0 * scale);
    cairo_fill(cr);
}

// ---------------------------------------------------------------------------
// Layout — used only by the foreign-distro chrome stub
// ---------------------------------------------------------------------------

void menubar_render_layout_menus(menubar_render_item_t *items, size_t n,
                                 int origin_x, double scale)
{
    int pad_px = (int)(MENU_ITEM_PAD_POINTS * scale + 0.5);
    int x = origin_x;
    for (size_t i = 0; i < n; i++) {
        double w = menubar_render_measure_text(items[i].title,
                                               items[i].is_app_name_bold,
                                               scale);
        items[i].x     = x;
        items[i].width = (int)w + pad_px;
        x += items[i].width;
    }
}

// ---------------------------------------------------------------------------
// High-level paint — foreign-distro chrome stub callers only
// ---------------------------------------------------------------------------

void menubar_render_paint_title_bar(cairo_t *cr, int width_px, int height_px,
                                    const char *window_title, bool active,
                                    menubar_render_theme_t theme, double scale)
{
    // 19.D-prep: traffic-light glyphs and the canonical SL title-bar
    // gradient live in moonbase/src/chrome/moonbase_chrome.c. Extract
    // those into the shared chrome layer when 19.D lands. For now we
    // paint the menubar background + a centered window title, so the
    // call site is exercisable end-to-end.
    (void)active;
    menubar_render_background(cr, width_px, height_px, theme);

    if (window_title && *window_title) {
        double tw = menubar_render_measure_text(window_title, false, scale);
        int    ty = menubar_render_text_center_y(window_title, false,
                                                 height_px, scale);
        double tx = (width_px - tw) / 2.0;
        if (tx < 0) tx = 0;
        // Snow Leopard text primary: #1A1A1A.
        menubar_render_text(cr, window_title, tx, ty, false,
                            26/255.0, 26/255.0, 26/255.0, scale);
    }
}

void menubar_render_paint_menu_bar(cairo_t *cr, int width_px, int height_px,
                                   const menubar_render_item_t *items, size_t n,
                                   int hover_index,
                                   menubar_render_theme_t theme, double scale)
{
    menubar_render_background(cr, width_px, height_px, theme);

    int half_pad_px = (int)((MENU_ITEM_PAD_POINTS / 2) * scale + 0.5);

    for (size_t i = 0; i < n; i++) {
        if ((int)i == hover_index) {
            menubar_render_hover_highlight(cr, items[i].x, 0,
                                           items[i].width, height_px, scale);
        }
        int ty = menubar_render_text_center_y(items[i].title,
                                              items[i].is_app_name_bold,
                                              height_px, scale);
        menubar_render_text(cr, items[i].title,
                            items[i].x + half_pad_px, ty,
                            items[i].is_app_name_bold,
                            26/255.0, 26/255.0, 26/255.0, scale);
    }
}

// ---------------------------------------------------------------------------
// Dropdown row — bit-identical mirror of menubar/src/appmenu.c lines 730-820
// ---------------------------------------------------------------------------

void menubar_render_paint_menu_item(cairo_t *cr,
                                    int x, int y, int w, int h,
                                    const menubar_render_menu_item_t *item,
                                    bool hovered,
                                    menubar_render_theme_t theme, double scale)
{
    // TODO(19.F): host theme variants tint the selection pill / text.
    // For now every theme renders Aqua.
    (void)theme;
    if (!item || !item->label) return;

    // Scale-translated pixel constants. Same formula as menubar.h's S():
    //   S(p) = (int)(p * scale + 0.5).
    // SF(p) used for sub-pixel radii — scaled but kept as double.
    int  s2  = (int)(2  * scale + 0.5);
    int  s3  = (int)(3  * scale + 0.5);
    int  s4  = (int)(4  * scale + 0.5);
    int  s6  = (int)(6  * scale + 0.5);
    int  s8  = (int)(8  * scale + 0.5);
    int  s14 = (int)(14 * scale + 0.5);
    int  s18 = (int)(18 * scale + 0.5);
    double sf3 = 3.0 * scale;

    // A row is "selected" only when both hovered AND enabled — matches
    // appmenu.c's `selected && enabled` check at line 730.
    bool selected = hovered && item->enabled;

    // Selection pill (#3875D7). Same geometry as appmenu.c:732 —
    // dropdown_rounded_rect(cr, S(4), y, L->w - S(8), S(ROW_H_ITEM), SF(3.0)).
    if (selected) {
        cairo_set_source_rgb(cr, 56.0/255.0, 117.0/255.0, 215.0/255.0);
        rounded_rect(cr, x + s4, y, w - s8, h, sf3);
        cairo_fill(cr);
    }

    // Text colour tier — matches appmenu.c:746-752.
    //   selected+enabled → white
    //   disabled         → 0.65 gray
    //   enabled normal   → 0.1 gray (~#1A1A1A)
    double tr, tg, tb;
    if (selected) {
        tr = tg = tb = 1.0;
    } else if (!item->enabled) {
        tr = tg = tb = 0.65;
    } else {
        tr = tg = tb = 0.1;
    }

    // Label at (x + S(18), y + S(2)) — appmenu.c:753.
    menubar_render_text(cr, item->label,
                        x + s18, y + s2, false,
                        tr, tg, tb, scale);

    // Toggle glyph at left margin (x + S(6), y + S(2)) — mirrors
    // appmenu.c:761-774. CHECKMARK paints ✓ U+2713 (checkbox on),
    // RADIO paints • U+2022 (radio selected), NONE skips the column.
    // The caller passes the *displayed* state, so an unchecked checkbox
    // is MENUBAR_TOGGLE_NONE and the column is empty — same pixels as
    // appmenu's `n->toggle != MENU_TOGGLE_NONE && n->toggle_state == 1`
    // gate.
    const char *toggle_glyph = NULL;
    switch (item->toggle) {
        case MENUBAR_TOGGLE_CHECKMARK: toggle_glyph = "\xe2\x9c\x93"; break;  // ✓ U+2713
        case MENUBAR_TOGGLE_RADIO:     toggle_glyph = "\xe2\x80\xa2"; break;  // • U+2022
        case MENUBAR_TOGGLE_NONE:      break;
    }
    if (toggle_glyph) {
        menubar_render_text(cr, toggle_glyph,
                            x + s6, y + s2, false,
                            tr, tg, tb, scale);
    }

    // Submenu arrow (▸ U+25B8) right-aligned, 11pt — appmenu.c:778-791.
    // Drawn before the shortcut so the shortcut text reserves the
    // appropriate gap to the left of the arrow.
    int right_margin = s14;
    if (item->is_submenu) {
        PangoLayout *al = create_text_layout_sized(cr, "\xe2\x96\xb8",
                                                   false, 11, scale);
        int arrow_w, arrow_h;
        pango_layout_get_pixel_size(al, &arrow_w, &arrow_h);
        cairo_set_source_rgb(cr, tr, tg, tb);
        cairo_move_to(cr, x + w - arrow_w - right_margin, y + s3);
        pango_cairo_show_layout(cr, al);
        g_object_unref(al);
        right_margin += arrow_w + s4;
    }

    // Shortcut text right-aligned, 12pt — appmenu.c:794-818. Text colour
    // matches appmenu's separate shortcut tier: 0.35 normal (vs 0.1 for
    // labels), 0.65 disabled, 1.0 selected. The label tier above already
    // computed `tr/tg/tb`; we override it just for shortcut paint.
    if (item->shortcut && *item->shortcut) {
        PangoLayout *sc = create_text_layout_sized(cr, item->shortcut,
                                                   false, 12, scale);
        int sw, sh;
        pango_layout_get_pixel_size(sc, &sw, &sh);

        if (selected) {
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        } else if (!item->enabled) {
            cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
        } else {
            cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
        }
        cairo_move_to(cr, x + w - sw - right_margin, y + s2);
        pango_cairo_show_layout(cr, sc);
        g_object_unref(sc);
    }
}
