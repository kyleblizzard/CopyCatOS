// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase_chrome.c — paints Aqua chrome for MoonBase windows into a
// CPU-side Cairo image surface. mb_host_render uploads the resulting
// pixels to a GL texture and stamps the chrome behind the client's
// content rect every frame.
//
// Every gradient / color / inset constant here matches decor.c so
// MoonBase windows are visually indistinguishable from X-client windows
// framed by CCWM. The only architectural difference is the destination:
// decor.c draws into an Xlib-backed Cairo surface wrapping an X frame
// window; here we draw into a private cairo_image_surface the
// compositor owns.
//
// Scope (slice 3c.2b):
//   * Title bar gradient (active + inactive palettes)
//   * Traffic lights (close / minimize / zoom) — flat-dot version
//     pulled from the inactive path of decor.c. Real PNG buttons + hover
//     glyphs come later once focus and pointer events route to MoonBase
//     windows.
//   * Centered title text (Lucida Grande via Pango, same as decor.c)
//   * 1 px side/bottom borders
//   * Rounded top corners, clipped
//
// Not yet (slice 3c.3+): drop shadow, resize handle, unsaved-dot,
// hover/pressed button states.

#include "moonbase_chrome.h"

#include "wm.h"  // TITLEBAR_HEIGHT, BORDER_WIDTH, BUTTON_*

#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------
// Layout helpers
// ---------------------------------------------------------------------

// Round a scaled dimension to the nearest integer pixel, never less
// than 1. Keeps edges crisp under fractional scale factors like 1.5×.
static inline int px(float v) {
    int r = (int)(v + 0.5f);
    return r < 1 ? 1 : r;
}

// ---------------------------------------------------------------------
// Public: repaint
// ---------------------------------------------------------------------

bool mb_chrome_repaint(mb_chrome_t *chrome,
                       uint32_t content_w, uint32_t content_h,
                       float    scale,
                       const char *title,
                       bool     active)
{
    if (!chrome) return false;
    if (content_w == 0 || content_h == 0 || scale <= 0.0f) return false;

    if (!title || !*title) title = "(Untitled)";

    // Scaled chrome geometry, in physical pixels.
    int border_px   = px(BORDER_WIDTH    * scale);
    int titlebar_px = px(TITLEBAR_HEIGHT * scale);

    uint32_t chrome_w = content_w + 2u * (uint32_t)border_px;
    uint32_t chrome_h = content_h + (uint32_t)titlebar_px + (uint32_t)border_px;

    // Re-allocate the Cairo surface when the outer size changes. This
    // happens on the first paint and whenever the client commits at a
    // new pixel size. The old surface is destroyed synchronously — it
    // has no GL state, it's pure RAM — and the GL texture is retained
    // so the next upload can still glTexSubImage2D into it if the new
    // dimensions happen to match (they won't here, but the logic keeps
    // the state machine simple).
    if (chrome->cairo_surface == NULL
            || chrome->chrome_w != chrome_w
            || chrome->chrome_h != chrome_h) {
        if (chrome->cairo_surface) {
            cairo_surface_destroy((cairo_surface_t *)chrome->cairo_surface);
            chrome->cairo_surface = NULL;
            chrome->pixels        = NULL;
            chrome->stride        = 0;
        }
        cairo_surface_t *s = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, (int)chrome_w, (int)chrome_h);
        if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(s);
            return false;
        }
        chrome->cairo_surface = s;
        chrome->chrome_w      = chrome_w;
        chrome->chrome_h      = chrome_h;
    }

    chrome->content_x_inset = (uint32_t)border_px;
    chrome->content_y_inset = (uint32_t)titlebar_px;

    cairo_surface_t *surface = (cairo_surface_t *)chrome->cairo_surface;
    cairo_t *cr = cairo_create(surface);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(cr);
        return false;
    }

    // ── Clear to transparent ──
    // Every pixel starts at RGBA (0,0,0,0) so any region the chrome
    // leaves untouched (specifically, the content-rect hole) composes
    // as fully transparent over the client's content quad.
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // ── Rounded-top clip ──
    // Snow Leopard windows have ~5-point rounded top corners; the
    // bottom is square. Clip the whole chrome draw to this path so the
    // titlebar gradient and borders respect the rounded profile.
    double corner_r = 5.0 * (double)scale;
    cairo_new_path(cr);
    cairo_arc(cr, corner_r, corner_r, corner_r, M_PI, 3 * M_PI / 2);
    cairo_arc(cr, (double)chrome_w - corner_r, corner_r, corner_r,
              -M_PI / 2, 0);
    cairo_line_to(cr, (double)chrome_w, (double)chrome_h);
    cairo_line_to(cr, 0, (double)chrome_h);
    cairo_close_path(cr);
    cairo_clip(cr);

    // ── Title-bar gradient ──
    // Exact palette copied from decor.c so MoonBase windows match
    // CCWM-framed X windows pixel-for-pixel.
    if (active) {
        cairo_set_source_rgb(cr, 243/255.0, 243/255.0, 243/255.0);
        cairo_rectangle(cr, 0, 0, (double)chrome_w, 1);
        cairo_fill(cr);

        cairo_pattern_t *grad = cairo_pattern_create_linear(
            0, 1, 0, (double)titlebar_px);
        cairo_pattern_add_color_stop_rgb(grad, 0.0, 212/255.0, 212/255.0, 212/255.0);
        cairo_pattern_add_color_stop_rgb(grad, 0.5, 196/255.0, 196/255.0, 196/255.0);
        cairo_pattern_add_color_stop_rgb(grad, 1.0, 172/255.0, 172/255.0, 172/255.0);
        cairo_set_source(cr, grad);
        cairo_rectangle(cr, 0, 0, (double)chrome_w, (double)titlebar_px);
        cairo_fill(cr);
        cairo_pattern_destroy(grad);
    } else {
        cairo_pattern_t *grad = cairo_pattern_create_linear(
            0, 0, 0, (double)titlebar_px);
        cairo_pattern_add_color_stop_rgb(grad, 0.0, 238/255.0, 238/255.0, 238/255.0);
        cairo_pattern_add_color_stop_rgb(grad, 1.0, 220/255.0, 220/255.0, 220/255.0);
        cairo_set_source(cr, grad);
        cairo_rectangle(cr, 0, 0, (double)chrome_w, (double)titlebar_px);
        cairo_fill(cr);
        cairo_pattern_destroy(grad);
    }

    // Titlebar / content divider (1 px line across the bottom of the bar).
    cairo_set_source_rgb(cr,
        active ? 140/255.0 : 185/255.0,
        active ? 140/255.0 : 185/255.0,
        active ? 140/255.0 : 185/255.0);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, (double)titlebar_px - 0.5);
    cairo_line_to(cr, (double)chrome_w, (double)titlebar_px - 0.5);
    cairo_stroke(cr);

    // ── Traffic lights ──
    // Dot-only rendering for now; the PNG-asset + hover-glyph path from
    // decor.c comes online once pointer events route to MoonBase
    // windows. The colors match Snow Leopard's inactive state when
    // !active and its active circle fills when active (close red,
    // minimize amber, zoom green) — close enough to scan as a window
    // in the Mission Control thumbnail grid.
    int btn_d = px(BUTTON_DIAMETER * scale);
    int btn_sp = px(BUTTON_SPACING * scale);
    int btn_lx = px(BUTTON_LEFT_PAD * scale);
    int btn_ty = px(BUTTON_TOP_PAD  * scale);

    const double colors[3][3] = {
        { 247/255.0,  72/255.0,  73/255.0 }, // close
        { 237/255.0, 152/255.0,  82/255.0 }, // minimize
        { 108/255.0, 177/255.0,  87/255.0 }, // zoom
    };
    const double inactive_rgb[3] = { 176/255.0, 176/255.0, 176/255.0 };
    const double inactive_stroke[3] = { 150/255.0, 150/255.0, 150/255.0 };

    int bx = btn_lx;
    int by = btn_ty;
    for (int i = 0; i < 3; i++) {
        double cx = bx + btn_d / 2.0;
        double cy = by + btn_d / 2.0;
        double r  = btn_d / 2.0;

        cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
        if (active) {
            cairo_set_source_rgb(cr, colors[i][0], colors[i][1], colors[i][2]);
            cairo_fill(cr);
        } else {
            cairo_set_source_rgb(cr,
                inactive_rgb[0], inactive_rgb[1], inactive_rgb[2]);
            cairo_fill_preserve(cr);
            cairo_set_source_rgb(cr,
                inactive_stroke[0], inactive_stroke[1], inactive_stroke[2]);
            cairo_set_line_width(cr, 0.5);
            cairo_stroke(cr);
        }
        bx += btn_d + btn_sp;
    }

    // ── Title text (centered in the title bar) ──
    // Pango gives us Lucida Grande with the project's font config. We
    // scale the point size by the backing scale so text is crisp at
    // 1.5× / 2.0× without relying on Pango's own DPI setting.
    PangoLayout *layout = pango_cairo_create_layout(cr);
    char font_spec[64];
    // Bold on focused windows, regular on inactive — matches decor.c.
    // 11pt base, scaled for backing scale.
    int pt_size = (int)(11.0f * scale + 0.5f);
    if (pt_size < 8) pt_size = 8;
    snprintf(font_spec, sizeof(font_spec), "%s %d",
             active ? "Lucida Grande Bold" : "Lucida Grande", pt_size);
    PangoFontDescription *font = pango_font_description_from_string(font_spec);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, title, -1);

    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);
    double tx = ((double)chrome_w - (double)tw) / 2.0;
    double ty = ((double)titlebar_px - (double)th) / 2.0;

    // 1 pt white drop shadow below the text (embossed / engraved look).
    // Offset scales with backing scale — a flat 1 px offset would vanish
    // visually at 2×/1.75× and stop reading as an engraved edge.
    cairo_move_to(cr, tx, ty + (double)px(scale));
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, active ? 0.7 : 0.3);
    pango_cairo_show_layout(cr, layout);

    cairo_move_to(cr, tx, ty);
    cairo_set_source_rgb(cr,
        active ?  40/255.0 : 140/255.0,
        active ?  40/255.0 : 140/255.0,
        active ?  40/255.0 : 140/255.0);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(font);
    g_object_unref(layout);

    // ── Side + bottom borders ──
    // 1 px outer frame. Keeps the content rect visually bounded once
    // the app paints its own background.
    double bc = active ? 138/255.0 : 180/255.0;
    cairo_set_source_rgb(cr, bc, bc, bc);
    cairo_set_line_width(cr, 1.0);

    cairo_move_to(cr, 0.5, (double)titlebar_px);
    cairo_line_to(cr, 0.5, (double)chrome_h - 0.5);
    cairo_stroke(cr);

    cairo_move_to(cr, (double)chrome_w - 0.5, (double)titlebar_px);
    cairo_line_to(cr, (double)chrome_w - 0.5, (double)chrome_h - 0.5);
    cairo_stroke(cr);

    cairo_move_to(cr, 0, (double)chrome_h - 0.5);
    cairo_line_to(cr, (double)chrome_w, (double)chrome_h - 0.5);
    cairo_stroke(cr);

    cairo_destroy(cr);

    // Expose the Cairo pixels to the uploader. cairo_image_surface_flush
    // guarantees every pending draw op has landed in RAM before we let
    // GL read it.
    cairo_surface_flush(surface);
    chrome->pixels = cairo_image_surface_get_data(surface);
    chrome->stride = (uint32_t)cairo_image_surface_get_stride(surface);
    chrome->revision++;
    return true;
}

// ---------------------------------------------------------------------
// Public: release
// ---------------------------------------------------------------------

void mb_chrome_release(mb_chrome_t *chrome,
                       void (*defer_gl_delete)(GLuint tex))
{
    if (!chrome) return;
    if (chrome->cairo_surface) {
        cairo_surface_destroy((cairo_surface_t *)chrome->cairo_surface);
    }
    if (defer_gl_delete && chrome->tex) {
        defer_gl_delete(chrome->tex);
    }
    memset(chrome, 0, sizeof(*chrome));
}
