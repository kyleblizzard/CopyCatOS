// CopyCatOS — by Kyle Blizzard at Blizzard.show

// host_chrome.c — paints Aqua chrome for MoonBase windows into a
// CPU-side Cairo image surface. The caller uploads or blits the pixels
// (moonrock → GL texture, moonrock-lite → X drawable).
//
// Every gradient / color / inset constant here matches decor.c so
// MoonBase windows are visually indistinguishable from X-client windows
// framed by CCWM. The only architectural difference is the destination:
// decor.c draws into an Xlib-backed Cairo surface wrapping an X frame
// window; here we draw into a private cairo_image_surface the consumer
// owns.
//
// Title-bar palette (SL 10.6, measured from
// snowleopardaura/example photos/finderexample.png averaged across
// three column samples at x=200, 500, 900):
//
//   Active:
//     tb_y=0       rgb(226, 226, 226)  #E2E2E2   1 px top highlight
//     tb_y=1..20   linear #D0D0D0 -> #C2C2C2     body gradient
//     tb_y=21      rgb(191, 191, 191)  #BFBFBF   divider line
//
//   Inactive (from example.png, Finder window behind Sharing sheet):
//     tb_y=0       rgb(244, 244, 244)  #F4F4F4   1 px top highlight
//     tb_y=1..20   linear #EDEDED -> #E4E4E4     body gradient
//     tb_y=21      rgb(208, 208, 208)  #D0D0D0   divider (derived)
//
// Traffic-light buttons are passed in as cairo_surface_t* by the caller
// (host_chrome doesn't depend on moonrock's asset cache) — same source
// surfaces decor.c paints, so MoonBase and X-client chrome stay
// pixel-identical.

#include "host_chrome.h"

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
// Static helper: paint title strip
// ---------------------------------------------------------------------
//
// Renders gradient + traffic lights + centered title at the cairo_t's
// current origin, into a strip (width_px × titlebar_px). No clip, no
// hairlines — the caller is responsible for any rounded-top clip and
// for drawing side/bottom borders if the strip is part of a larger
// chrome rectangle. This is the single source of truth shared between
// `mb_chrome_repaint` (compositor-internal MoonBase windows) and
// `mb_chrome_paint_title_strip` (foreign-distro chrome via moonrock-
// lite blitting into an X drawable).

static void paint_title_strip(cairo_t *cr,
                              int      width_px,
                              int      titlebar_px,
                              float    scale,
                              const char *title,
                              bool     active,
                              bool     buttons_hover,
                              int      pressed_button,
                              cairo_surface_t *imgs[3])
{
    if (!title || !*title) title = "(Untitled)";

    double w = (double)width_px;
    double h = (double)titlebar_px;

    // ── Title-bar gradient ──
    {
        double hi_r, hi_g, hi_b;
        double g0_r, g0_g, g0_b;
        double g1_r, g1_g, g1_b;
        double dv_r, dv_g, dv_b;

        if (active) {
            hi_r = hi_g = hi_b = 226/255.0;
            g0_r = g0_g = g0_b = 208/255.0;
            g1_r = g1_g = g1_b = 194/255.0;
            dv_r = dv_g = dv_b = 191/255.0;
        } else {
            hi_r = hi_g = hi_b = 244/255.0;
            g0_r = g0_g = g0_b = 237/255.0;
            g1_r = g1_g = g1_b = 228/255.0;
            dv_r = dv_g = dv_b = 208/255.0;
        }

        cairo_set_source_rgb(cr, hi_r, hi_g, hi_b);
        cairo_rectangle(cr, 0, 0, w, 1);
        cairo_fill(cr);

        cairo_pattern_t *grad = cairo_pattern_create_linear(0, 1, 0, h - 1);
        cairo_pattern_add_color_stop_rgb(grad, 0.0, g0_r, g0_g, g0_b);
        cairo_pattern_add_color_stop_rgb(grad, 1.0, g1_r, g1_g, g1_b);
        cairo_set_source(cr, grad);
        cairo_rectangle(cr, 0, 1, w, h - 1);
        cairo_fill(cr);
        cairo_pattern_destroy(grad);

        cairo_set_source_rgb(cr, dv_r, dv_g, dv_b);
        cairo_rectangle(cr, 0, h - 1, w, 1);
        cairo_fill(cr);
    }

    // ── Traffic lights ──
    int btn_d  = px(MB_CHROME_BUTTON_DIAMETER  * scale);
    int btn_sp = px(MB_CHROME_BUTTON_SPACING   * scale);
    int btn_lx = px(MB_CHROME_BUTTON_LEFT_PAD  * scale);
    int btn_ty = px(MB_CHROME_BUTTON_TOP_PAD   * scale);

    bool show_glyphs = active && (buttons_hover || pressed_button > 0);
    int bx = btn_lx;
    int by = btn_ty;
    for (int i = 0; i < 3; i++) {
        double cx = bx + btn_d / 2.0;
        double cy = by + btn_d / 2.0;
        double r  = btn_d / 2.0;

        if (active && imgs[i]) {
            int img_w = cairo_image_surface_get_width (imgs[i]);
            int img_h = cairo_image_surface_get_height(imgs[i]);
            cairo_save(cr);
            cairo_translate(cr, bx, by);
            cairo_scale(cr, (double)btn_d / img_w, (double)btn_d / img_h);
            cairo_set_source_surface(cr, imgs[i], 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);
            cairo_restore(cr);
        } else {
            cairo_arc(cr, cx, cy, r - 0.5, 0, 2 * M_PI);
            cairo_set_source_rgb(cr, 176/255.0, 176/255.0, 176/255.0);
            cairo_fill_preserve(cr);
            cairo_set_source_rgb(cr, 150/255.0, 150/255.0, 150/255.0);
            cairo_set_line_width(cr, 0.5);
            cairo_stroke(cr);
        }

        if (pressed_button == (i + 1) && active) {
            cairo_save(cr);
            cairo_arc(cr, cx, cy, r - 0.5, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.25);
            cairo_fill(cr);
            cairo_restore(cr);
        }

        if (show_glyphs) {
            double glyph_size = r * 0.55;
            cairo_save(cr);
            cairo_set_line_width(cr, 1.2);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            if (i == 0) {
                cairo_set_source_rgba(cr, 0.25, 0.0, 0.0, 0.9);
                cairo_move_to(cr, cx - glyph_size, cy - glyph_size);
                cairo_line_to(cr, cx + glyph_size, cy + glyph_size);
                cairo_stroke(cr);
                cairo_move_to(cr, cx + glyph_size, cy - glyph_size);
                cairo_line_to(cr, cx - glyph_size, cy + glyph_size);
                cairo_stroke(cr);
            } else if (i == 1) {
                cairo_set_source_rgba(cr, 0.25, 0.12, 0.0, 0.9);
                cairo_move_to(cr, cx - glyph_size, cy);
                cairo_line_to(cr, cx + glyph_size, cy);
                cairo_stroke(cr);
            } else {
                cairo_set_source_rgba(cr, 0.0, 0.18, 0.0, 0.9);
                cairo_move_to(cr, cx - glyph_size, cy);
                cairo_line_to(cr, cx + glyph_size, cy);
                cairo_stroke(cr);
                cairo_move_to(cr, cx, cy - glyph_size);
                cairo_line_to(cr, cx, cy + glyph_size);
                cairo_stroke(cr);
            }
            cairo_restore(cr);
        }

        bx += btn_d + btn_sp;
    }

    // ── Title text (centered in the strip) ──
    PangoLayout *layout = pango_cairo_create_layout(cr);
    char font_spec[64];
    int pt_size = (int)(11.0f * scale + 0.5f);
    if (pt_size < 8) pt_size = 8;
    snprintf(font_spec, sizeof(font_spec), "%s %d",
             active ? "Lucida Grande Bold" : "Lucida Grande", pt_size);
    PangoFontDescription *font = pango_font_description_from_string(font_spec);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, title, -1);

    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);
    double tx = (w - (double)tw) / 2.0;
    double ty = (h - (double)th) / 2.0;

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
}

// ---------------------------------------------------------------------
// Public: repaint
// ---------------------------------------------------------------------

bool mb_chrome_repaint(mb_chrome_t *chrome,
                       uint32_t content_w, uint32_t content_h,
                       float    scale,
                       const char *title,
                       bool     active,
                       bool     buttons_hover,
                       int      pressed_button,
                       void *const btn_imgs[3])
{
    if (!chrome) return false;
    if (content_w == 0 || content_h == 0 || scale <= 0.0f) return false;

    if (!title || !*title) title = "(Untitled)";

    // Scaled chrome geometry, in physical pixels.
    // Ground-truth in snowleopardaura/example photos/finderexample.png
    // (y=200 row, sampled at both edges): wallpaper → 1-px shadow →
    // content flush. SL 10.6 has no visible left/right/bottom gray
    // border; the shadow is what separates the window visually. So the
    // chrome here has NO side or bottom inset — chrome_w = content_w,
    // chrome_h = content_h + titlebar_px. We still paint a thin
    // 1-px edge line at the content's outer pixels (drawn as an
    // overlay, not as reserved chrome space) so the window has a
    // hairline definition against the wallpaper when the GL shadow
    // pass is disabled.
    int titlebar_px = px(MB_CHROME_TITLEBAR_HEIGHT * scale);

    uint32_t chrome_w = content_w;
    uint32_t chrome_h = content_h + (uint32_t)titlebar_px;

    // Re-allocate the Cairo surface when the outer size changes. This
    // happens on the first paint and whenever the client commits at a
    // new pixel size. The old surface is destroyed synchronously — it
    // has no GPU state, it's pure RAM.
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

    // Content quad lives flush with the chrome's outer rect on three
    // sides; the 1-px edge line below is painted OVER the content's
    // outermost pixels so the window doesn't grow to accommodate it.
    chrome->content_x_inset = 0;
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

    // ── Title strip (gradient + traffic lights + centered title) ──
    // Single source of truth shared with mb_chrome_paint_title_strip.
    cairo_surface_t *imgs[3] = {
        btn_imgs ? (cairo_surface_t *)btn_imgs[0] : NULL,
        btn_imgs ? (cairo_surface_t *)btn_imgs[1] : NULL,
        btn_imgs ? (cairo_surface_t *)btn_imgs[2] : NULL,
    };
    paint_title_strip(cr, (int)chrome_w, titlebar_px, scale,
                      title, active, buttons_hover, pressed_button, imgs);

    // ── Side + bottom hairline ──
    double bc = active ? 160/255.0 : 190/255.0;
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

    cairo_surface_flush(surface);
    chrome->pixels = cairo_image_surface_get_data(surface);
    chrome->stride = (uint32_t)cairo_image_surface_get_stride(surface);
    chrome->revision++;
    return true;
}

// ---------------------------------------------------------------------
// Public: release
// ---------------------------------------------------------------------

void mb_chrome_release(mb_chrome_t *chrome)
{
    if (!chrome) return;
    if (chrome->cairo_surface) {
        cairo_surface_destroy((cairo_surface_t *)chrome->cairo_surface);
    }
    memset(chrome, 0, sizeof(*chrome));
}

// ---------------------------------------------------------------------
// Public: paint title strip into caller's Cairo context
// ---------------------------------------------------------------------
//
// moonrock-lite owns its own cairo_xlib_surface above an X drawable
// and just needs the pixels of the SL title strip — no rounded-top
// clip (its background is whatever WM_CLASS_HINT background pixel the
// override-redirect window was created with), no side/bottom hairlines
// (those belong to the bundle's own X frame, not the chrome bar).

void mb_chrome_paint_title_strip(void *cr_void,
                                 int width_px, int height_px,
                                 float scale,
                                 const char *title,
                                 bool active,
                                 bool buttons_hover,
                                 int  pressed_button,
                                 void *const btn_imgs[3])
{
    if (!cr_void) return;
    if (width_px <= 0 || height_px <= 0 || scale <= 0.0f) return;

    cairo_surface_t *imgs[3] = {
        btn_imgs ? (cairo_surface_t *)btn_imgs[0] : NULL,
        btn_imgs ? (cairo_surface_t *)btn_imgs[1] : NULL,
        btn_imgs ? (cairo_surface_t *)btn_imgs[2] : NULL,
    };
    paint_title_strip((cairo_t *)cr_void, width_px, height_px, scale,
                      title, active, buttons_hover, pressed_button, imgs);
}
