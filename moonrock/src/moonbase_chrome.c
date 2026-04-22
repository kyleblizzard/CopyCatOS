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
// Traffic-light buttons use the extracted real-screenshot PNGs under
// $HOME/.local/share/aqua-widgets/sl_{close,minimize,zoom}_button.png
// via assets_get_*_button() — same source surfaces decor.c paints, so
// MoonBase and X-client chrome stay pixel-identical.

#include "moonbase_chrome.h"

#include "assets.h"  // assets_get_*_button
#include "wm.h"      // TITLEBAR_HEIGHT, BORDER_WIDTH, BUTTON_*

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
                       bool     active,
                       bool     buttons_hover,
                       int      pressed_button)
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
    int titlebar_px = px(TITLEBAR_HEIGHT * scale);

    uint32_t chrome_w = content_w;
    uint32_t chrome_h = content_h + (uint32_t)titlebar_px;

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

    // ── Title-bar gradient ──
    // Values measured from real SL 10.6 reference screenshots (see
    // comment block at top of this file). Both active and inactive
    // follow the same three-part recipe: 1-px highlight row, body
    // gradient, 1-px divider at the bottom edge.
    //
    // At fractional scale we stretch the three regions proportionally:
    //   highlight = 1 px (always)
    //   divider   = 1 px (always, drawn as a stroke below)
    //   body      = titlebar_px - 1 px  (the remainder)
    // so the first and last rows remain crisp at every scale while the
    // gradient takes the slack.
    {
        double w = (double)chrome_w;
        double h = (double)titlebar_px;

        // Colour stops.
        double hi_r, hi_g, hi_b;     // 1-px highlight
        double g0_r, g0_g, g0_b;     // body gradient top
        double g1_r, g1_g, g1_b;     // body gradient bottom
        double dv_r, dv_g, dv_b;     // 1-px divider

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

        // 1-px highlight row at the very top.
        cairo_set_source_rgb(cr, hi_r, hi_g, hi_b);
        cairo_rectangle(cr, 0, 0, w, 1);
        cairo_fill(cr);

        // Body gradient, spanning y=1 to y=titlebar_px-1 (divider sits
        // on the last row drawn as a separate stroke below).
        cairo_pattern_t *grad = cairo_pattern_create_linear(0, 1, 0, h - 1);
        cairo_pattern_add_color_stop_rgb(grad, 0.0, g0_r, g0_g, g0_b);
        cairo_pattern_add_color_stop_rgb(grad, 1.0, g1_r, g1_g, g1_b);
        cairo_set_source(cr, grad);
        cairo_rectangle(cr, 0, 1, w, h - 1);
        cairo_fill(cr);
        cairo_pattern_destroy(grad);

        // 1-px divider at the very bottom of the titlebar.
        cairo_set_source_rgb(cr, dv_r, dv_g, dv_b);
        cairo_rectangle(cr, 0, h - 1, w, 1);
        cairo_fill(cr);
    }

    // ── Traffic lights ──
    // Active: real SL 10.6 PNG assets via assets_get_*_button(), same
    // surfaces decor.c uses for CCWM-framed X windows, so MoonBase
    // chrome is pixel-identical. Each PNG is a 14x14 disc cropped from
    // finderexample.png with a circular alpha mask so the titlebar
    // gradient shows around the button.
    //
    // Inactive: uniform gray dots with a thin outline — matches real
    // Snow Leopard, which replaces the three coloured lights with
    // indistinguishable circles when the window loses focus.
    int btn_d  = px(BUTTON_DIAMETER  * scale);
    int btn_sp = px(BUTTON_SPACING   * scale);
    int btn_lx = px(BUTTON_LEFT_PAD  * scale);
    int btn_ty = px(BUTTON_TOP_PAD   * scale);

    cairo_surface_t *btn_imgs[3] = {
        assets_get_close_button(),
        assets_get_minimize_button(),
        assets_get_zoom_button(),
    };

    // A hover OR press on any one button reveals all three glyphs
    // (SL 10.6 behaviour) and keeps the PNG discs in their coloured
    // state — inactive paint only happens when !active.
    bool show_glyphs = active && (buttons_hover || pressed_button > 0);
    int bx = btn_lx;
    int by = btn_ty;
    for (int i = 0; i < 3; i++) {
        double cx = bx + btn_d / 2.0;
        double cy = by + btn_d / 2.0;
        double r  = btn_d / 2.0;

        if (active && btn_imgs[i]) {
            // Paint the PNG asset scaled to the current button diameter.
            // BILINEAR is a deliberate compromise: fractional scales
            // (e.g. 1.75x -> 13 px native upscaled to ~23 px) introduce
            // mild softness vs. the 1x reference, but sharper filters
            // would alias the circular alpha edge. Cheapest path that
            // keeps the disc's anti-aliased border clean.
            int img_w = cairo_image_surface_get_width (btn_imgs[i]);
            int img_h = cairo_image_surface_get_height(btn_imgs[i]);
            cairo_save(cr);
            cairo_translate(cr, bx, by);
            cairo_scale(cr, (double)btn_d / img_w, (double)btn_d / img_h);
            cairo_set_source_surface(cr, btn_imgs[i], 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);
            cairo_restore(cr);
        } else {
            // Inactive (or fallback when a PNG failed to load): solid
            // gray dot with a slightly darker stroke. Colors measured
            // from the inactive Finder window behind the Sharing sheet
            // in example.png — roughly #B0B0B0 fill, #969696 stroke.
            cairo_arc(cr, cx, cy, r - 0.5, 0, 2 * M_PI);
            cairo_set_source_rgb(cr, 176/255.0, 176/255.0, 176/255.0);
            cairo_fill_preserve(cr);
            cairo_set_source_rgb(cr, 150/255.0, 150/255.0, 150/255.0);
            cairo_set_line_width(cr, 0.5);
            cairo_stroke(cr);
        }

        // Pressed overlay — darken the currently-pressed disc. Mirrors
        // decor.c's 25% black over the PNG, which is the SL feedback.
        if (pressed_button == (i + 1) && active) {
            cairo_save(cr);
            cairo_arc(cr, cx, cy, r - 0.5, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.25);
            cairo_fill(cr);
            cairo_restore(cr);
        }

        // Glyphs: ×, −, + rendered as short strokes across ~55% of the
        // disc. Copied from decor.c so X-client and MoonBase windows
        // paint identical pixels. Only appears on active + hover/press.
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

    // ── Side + bottom hairline ──
    // 1-px edge line overlaid on the content's outermost pixels so the
    // window has a faint definition against the wallpaper when the GL
    // shadow pass isn't drawing. Not reserved chrome space — the
    // content quad spans the full chrome width and this line paints
    // over its edge after content has already blitted. Colour is the
    // CLAUDE.md canonical #A0A0A0 active / #BEBEBE inactive, far
    // lighter than the prior #8A8A8A / #B4B4B4 so the window no
    // longer reads as "a pixel or two inset."
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
