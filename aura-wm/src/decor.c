// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// AuraOS Window Manager — Snow Leopard decoration rendering
//
// Every color value is extracted from the actual Snow Leopard reference
// screenshots in aquaimages/finderexample.png. No approximations.
//
// Title bar gradient (active):
//   y=0:  (76, 125, 176)  — blue-gray top
//   y=9:  (73, 118, 167)  — slightly darker blue
//   y=10: (62, 101, 143)  — dark blue divider
//   y=11: (226, 226, 226) — jump to light gray
//   y=19: (203, 203, 203) — bottom gray
//   y=21: (202, 202, 202) — very bottom
//
// Traffic light buttons (from real screenshot at 14x14 crop):
//   Close center:    (247, 72, 73)
//   Minimize center: (237, 152, 82)
//   Zoom center:     (108, 177, 87)

#include "decor.h"
#include "assets.h"
#include "compositor.h"
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <string.h>

// Global flag set by compositor_init() — declared in wm.h
bool compositor_active = false;

void decor_init(AuraWM *wm)
{
    assets_load(wm);
}

void decor_paint(AuraWM *wm, Client *c)
{
    if (!c->frame) return;

    // Chrome dimensions (the actual window border, without shadow)
    int chrome_w = c->w + 2 * BORDER_WIDTH;
    int chrome_h = c->h + TITLEBAR_HEIGHT + BORDER_WIDTH;
    bool active = c->focused;

    // Shadow padding — when compositor is active, the frame is larger
    // than the chrome to make room for the transparent shadow region
    int sl = compositor_active ? SHADOW_LEFT : 0;
    int sr = compositor_active ? SHADOW_RIGHT : 0;
    int st = compositor_active ? SHADOW_TOP : 0;
    int sb = compositor_active ? SHADOW_BOTTOM : 0;

    // Total frame dimensions (chrome + shadow padding)
    int frame_w = chrome_w + sl + sr;
    int frame_h = chrome_h + st + sb;

    // Create Cairo surface on the frame window
    Visual *visual = DefaultVisual(wm->dpy, wm->screen);
    cairo_surface_t *surface = cairo_xlib_surface_create(
        wm->dpy, c->frame, visual, frame_w, frame_h);
    cairo_t *cr = cairo_create(surface);

    // ── Shadow (compositor must be active for ARGB visuals) ──
    if (compositor_active) {
        // Clear the entire frame to transparent first (ARGB visual)
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

        // Paint the drop shadow in the transparent padding around the chrome
        compositor_paint_shadow(wm, c, cr);
    }

    // All chrome painting is offset by (sl, st) to account for shadow padding.
    // Use cairo_translate so we can keep the same coordinate system as before.
    cairo_save(cr);
    cairo_translate(cr, sl, st);

    // ── Title bar gradient ──
    // Colors extracted from real Snow Leopard screenshots pixel-by-pixel.
    if (active) {
        // Active: distinctive two-zone gradient (blue-gray top, light gray bottom)
        cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, TITLEBAR_HEIGHT);
        cairo_pattern_add_color_stop_rgb(grad, 0.0,   76/255.0, 125/255.0, 176/255.0);
        cairo_pattern_add_color_stop_rgb(grad, 0.40,  73/255.0, 118/255.0, 167/255.0);
        cairo_pattern_add_color_stop_rgb(grad, 0.45,  62/255.0, 101/255.0, 143/255.0);
        cairo_pattern_add_color_stop_rgb(grad, 0.50, 226/255.0, 226/255.0, 226/255.0);
        cairo_pattern_add_color_stop_rgb(grad, 0.80, 208/255.0, 208/255.0, 208/255.0);
        cairo_pattern_add_color_stop_rgb(grad, 1.0,  202/255.0, 202/255.0, 202/255.0);
        cairo_set_source(cr, grad);
        cairo_rectangle(cr, 0, 0, chrome_w, TITLEBAR_HEIGHT);
        cairo_fill(cr);
        cairo_pattern_destroy(grad);
    } else {
        // Inactive: flat light gray (loses the blue zone)
        cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, TITLEBAR_HEIGHT);
        cairo_pattern_add_color_stop_rgb(grad, 0.0, 236/255.0, 236/255.0, 236/255.0);
        cairo_pattern_add_color_stop_rgb(grad, 1.0, 218/255.0, 218/255.0, 218/255.0);
        cairo_set_source(cr, grad);
        cairo_rectangle(cr, 0, 0, chrome_w, TITLEBAR_HEIGHT);
        cairo_fill(cr);
        cairo_pattern_destroy(grad);
    }

    // Bottom border line of title bar
    cairo_set_source_rgb(cr, active ? 140/255.0 : 185/255.0,
                              active ? 140/255.0 : 185/255.0,
                              active ? 140/255.0 : 185/255.0);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, TITLEBAR_HEIGHT - 0.5);
    cairo_line_to(cr, chrome_w, TITLEBAR_HEIGHT - 0.5);
    cairo_stroke(cr);

    // ── Traffic light buttons ──
    // Active windows get real Snow Leopard PNG buttons.
    // Inactive windows get gray circles (exact Snow Leopard behavior).
    int bx = BUTTON_LEFT_PAD;
    int by = BUTTON_TOP_PAD;

    if (active) {
        cairo_surface_t *close_img = assets_get_close_button();
        cairo_surface_t *min_img = assets_get_minimize_button();
        cairo_surface_t *zoom_img = assets_get_zoom_button();

        if (close_img) {
            cairo_set_source_surface(cr, close_img, bx, by);
            cairo_paint(cr);
        }
        bx += BUTTON_DIAMETER + BUTTON_SPACING;

        if (min_img) {
            cairo_set_source_surface(cr, min_img, bx, by);
            cairo_paint(cr);
        }
        bx += BUTTON_DIAMETER + BUTTON_SPACING;

        if (zoom_img) {
            cairo_set_source_surface(cr, zoom_img, bx, by);
            cairo_paint(cr);
        }
    } else {
        // Inactive: identical gray dots (#B0B0B0) for all three buttons
        for (int i = 0; i < 3; i++) {
            double cx = bx + BUTTON_DIAMETER / 2.0;
            double cy = by + BUTTON_DIAMETER / 2.0;
            double r = BUTTON_DIAMETER / 2.0;

            cairo_arc(cr, cx, cy, r, 0, 2 * 3.14159265);
            cairo_set_source_rgb(cr, 176/255.0, 176/255.0, 176/255.0);
            cairo_fill_preserve(cr);
            cairo_set_source_rgb(cr, 150/255.0, 150/255.0, 150/255.0);
            cairo_set_line_width(cr, 0.5);
            cairo_stroke(cr);

            bx += BUTTON_DIAMETER + BUTTON_SPACING;
        }
    }

    // ── Title text ──
    // Snow Leopard: Lucida Grande Bold 13pt (title bar uses 11pt on screen),
    // centered, with a 1px white drop shadow for embossed effect.
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_from_string(
        active ? "Lucida Grande Bold 11" : "Lucida Grande 11");
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, c->title, -1);

    int text_w, text_h;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);
    double text_x = (chrome_w - text_w) / 2.0;
    double text_y = (TITLEBAR_HEIGHT - text_h) / 2.0;

    // 1px white drop shadow below the text (embossed/engraved look)
    cairo_move_to(cr, text_x, text_y + 1);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, active ? 0.6 : 0.4);
    pango_cairo_show_layout(cr, layout);

    // Actual title text on top
    cairo_move_to(cr, text_x, text_y);
    cairo_set_source_rgb(cr, active ? 40/255.0 : 140/255.0,
                              active ? 40/255.0 : 140/255.0,
                              active ? 40/255.0 : 140/255.0);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(font);
    g_object_unref(layout);

    // ── Side and bottom borders ──
    double bc = active ? 160/255.0 : 190/255.0;
    cairo_set_source_rgb(cr, bc, bc, bc);
    cairo_set_line_width(cr, 1.0);

    // Left border
    cairo_move_to(cr, 0.5, TITLEBAR_HEIGHT);
    cairo_line_to(cr, 0.5, chrome_h - 0.5);
    cairo_stroke(cr);

    // Right border
    cairo_move_to(cr, chrome_w - 0.5, TITLEBAR_HEIGHT);
    cairo_line_to(cr, chrome_w - 0.5, chrome_h - 0.5);
    cairo_stroke(cr);

    // Bottom border
    cairo_move_to(cr, 0, chrome_h - 0.5);
    cairo_line_to(cr, chrome_w, chrome_h - 0.5);
    cairo_stroke(cr);

    // ── Resize handle ── (bottom-right corner, diagonal ridges)
    // Draw three small diagonal lines in the bottom-right corner.
    // Real Snow Leopard uses resizestandard.png from CoreUI, but we draw
    // a simple fallback here that matches the visual.
    if (active) {
        int rx = chrome_w - 15;
        int ry = chrome_h - 15;
        cairo_set_line_width(cr, 1.0);
        for (int i = 0; i < 3; i++) {
            int offset = i * 4;
            // Dark line
            cairo_set_source_rgba(cr, 0, 0, 0, 0.25);
            cairo_move_to(cr, rx + offset + 12, ry + 12);
            cairo_line_to(cr, rx + 12, ry + offset + 12);
            cairo_stroke(cr);
            // Light line (1px below for 3D effect)
            cairo_set_source_rgba(cr, 1, 1, 1, 0.5);
            cairo_move_to(cr, rx + offset + 13, ry + 12);
            cairo_line_to(cr, rx + 12, ry + offset + 13);
            cairo_stroke(cr);
        }
    }

    // Restore the coordinate system (undo the shadow offset translation)
    cairo_restore(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

void decor_shutdown(void)
{
    assets_shutdown();
}
