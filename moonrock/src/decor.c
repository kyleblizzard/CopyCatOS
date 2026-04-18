// CopyCatOS — by Kyle Blizzard at Blizzard.show

// CopyCatOS Window Manager — Snow Leopard decoration rendering
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

// M_PI requires _GNU_SOURCE (provided by meson via -D_GNU_SOURCE).
#include "decor.h"
#include "assets.h"
#include "moonrock.h"
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

// Global flag set by compositor_init() — declared in wm.h
bool compositor_active = false;

void decor_init(CCWM *wm)
{
    assets_load(wm);
}

void decor_paint(CCWM *wm, Client *c)
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

    // Get the frame window's actual visual (might be 32-bit ARGB if
    // compositor is active). Using DefaultVisual() here would return the
    // 24-bit visual, causing a mismatch: Cairo would write RGB data into
    // an ARGB window, leaving the alpha channel as garbage (often 0 =
    // fully transparent), which makes the title bar invisible/black.
    XWindowAttributes wa;
    XGetWindowAttributes(wm->dpy, c->frame, &wa);
    Visual *visual = wa.visual;

    cairo_surface_t *surface = cairo_xlib_surface_create(
        wm->dpy, c->frame, visual, frame_w, frame_h);
    cairo_t *cr = cairo_create(surface);

    // ── Compositor-aware frame clearing ──
    if (compositor_active) {
        // Clear frame to transparent for ARGB visual
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        // NOTE: Shadow rendering moved to MoonRock Compositor.
        // mr_composite() draws GL shadows in the compositing pass,
        // so we don't paint Cairo shadows here anymore.
    }

    // All chrome painting is offset by (sl, st) to account for shadow padding.
    // Use cairo_translate so we can keep the same coordinate system as before.
    cairo_save(cr);
    cairo_translate(cr, sl, st);

    // ── Rounded window shape ──
    // Snow Leopard windows have ~5px rounded corners at the top. The bottom
    // corners are square (content area meets the bottom bar flush).
    // We clip to this shape so all painting respects the rounded corners.
    double corner_r = 5.0;
    cairo_new_path(cr);
    cairo_arc(cr, corner_r, corner_r, corner_r, M_PI, 3*M_PI/2);           // top-left
    cairo_arc(cr, chrome_w - corner_r, corner_r, corner_r, -M_PI/2, 0);    // top-right
    cairo_line_to(cr, chrome_w, chrome_h);                                   // right side
    cairo_line_to(cr, 0, chrome_h);                                          // bottom
    cairo_close_path(cr);
    cairo_clip(cr);

    // ── Title bar gradient ──
    // Snow Leopard standard window title bar: smooth grey gradient with a
    // 1px bright highlight at the very top edge where light catches the chrome.
    if (active) {
        // 1px bright highlight at the very top edge
        cairo_set_source_rgb(cr, 243/255.0, 243/255.0, 243/255.0);
        cairo_rectangle(cr, 0, 0, chrome_w, 1);
        cairo_fill(cr);

        // Main gradient: warm grey from light to medium
        cairo_pattern_t *grad = cairo_pattern_create_linear(0, 1, 0, TITLEBAR_HEIGHT);
        cairo_pattern_add_color_stop_rgb(grad, 0.0,  212/255.0, 212/255.0, 212/255.0);
        cairo_pattern_add_color_stop_rgb(grad, 0.5,  196/255.0, 196/255.0, 196/255.0);
        cairo_pattern_add_color_stop_rgb(grad, 1.0,  172/255.0, 172/255.0, 172/255.0);
        cairo_set_source(cr, grad);
        cairo_rectangle(cr, 0, 0, chrome_w, TITLEBAR_HEIGHT);
        cairo_fill(cr);
        cairo_pattern_destroy(grad);
    } else {
        // Inactive: lighter, flatter gradient (window recedes)
        cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, TITLEBAR_HEIGHT);
        cairo_pattern_add_color_stop_rgb(grad, 0.0, 238/255.0, 238/255.0, 238/255.0);
        cairo_pattern_add_color_stop_rgb(grad, 1.0, 220/255.0, 220/255.0, 220/255.0);
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
    // Active windows get real Snow Leopard PNG buttons. When the mouse
    // hovers over the button region, all three buttons show their glyphs
    // (x for close, - for minimize, + for zoom). Pressing a button
    // darkens it to give click feedback. Inactive windows get gray dots.
    int bx = BUTTON_LEFT_PAD;
    int by = BUTTON_TOP_PAD;

    // Check hover/pressed state from the WM
    bool hovering = (wm->buttons_hover && wm->hover_client == c && active);
    int pressed = (wm->hover_client == c) ? wm->pressed_button : 0;

    if (active) {
        cairo_surface_t *btn_imgs[3] = {
            assets_get_close_button(),
            assets_get_minimize_button(),
            assets_get_zoom_button()
        };

        for (int i = 0; i < 3; i++) {
            cairo_surface_t *img = btn_imgs[i];
            double cx = bx + BUTTON_DIAMETER / 2.0;
            double cy = by + BUTTON_DIAMETER / 2.0;

            // Paint the button circle (from PNG asset or fallback)
            if (img) {
                int img_w = cairo_image_surface_get_width(img);
                int img_h = cairo_image_surface_get_height(img);
                cairo_save(cr);
                cairo_translate(cr, bx, by);
                cairo_scale(cr, (double)BUTTON_DIAMETER / img_w,
                                (double)BUTTON_DIAMETER / img_h);
                cairo_set_source_surface(cr, img, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
                cairo_paint(cr);
                cairo_restore(cr);
            }

            // Unsaved changes dot — when a document has unsaved changes,
            // the close button displays a dark dot in its center (HIG
            // Figure 14-8). This is the standard macOS indicator that
            // closing will prompt to save. Detected from title prefix.
            if (i == 0 && c->unsaved && !hovering && pressed == 0) {
                cairo_save(cr);
                cairo_arc(cr, cx, cy, 2.0, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, 0.22, 0.0, 0.0, 0.85);
                cairo_fill(cr);
                cairo_restore(cr);
            }

            // Pressed state — darken the button circle to show feedback.
            // Real Snow Leopard darkens the pressed button by overlaying
            // a semi-transparent black on the circle.
            if (pressed == (i + 1)) {
                double r = BUTTON_DIAMETER / 2.0;
                cairo_save(cr);
                cairo_arc(cr, cx, cy, r - 0.5, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, 0, 0, 0, 0.25);
                cairo_fill(cr);
                cairo_restore(cr);
            }

            // Hover glyphs — when mouse enters the button region, ALL
            // three buttons show their glyph simultaneously. The glyphs
            // are rendered as dark marks with a subtle shadow, matching
            // the real Snow Leopard appearance.
            if (hovering || pressed > 0) {
                double r = BUTTON_DIAMETER / 2.0;
                double glyph_size = r * 0.55;  // Glyph fits inside circle

                cairo_save(cr);
                cairo_set_line_width(cr, 1.2);
                cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

                if (i == 0) {
                    // Close glyph: × (two diagonal lines crossing)
                    // Dark mark with slight transparency for depth
                    cairo_set_source_rgba(cr, 0.25, 0.0, 0.0, 0.9);
                    cairo_move_to(cr, cx - glyph_size, cy - glyph_size);
                    cairo_line_to(cr, cx + glyph_size, cy + glyph_size);
                    cairo_stroke(cr);
                    cairo_move_to(cr, cx + glyph_size, cy - glyph_size);
                    cairo_line_to(cr, cx - glyph_size, cy + glyph_size);
                    cairo_stroke(cr);
                } else if (i == 1) {
                    // Minimize glyph: − (horizontal line)
                    cairo_set_source_rgba(cr, 0.25, 0.12, 0.0, 0.9);
                    cairo_move_to(cr, cx - glyph_size, cy);
                    cairo_line_to(cr, cx + glyph_size, cy);
                    cairo_stroke(cr);
                } else {
                    // Zoom glyph: + (horizontal + vertical lines)
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

            bx += BUTTON_DIAMETER + BUTTON_SPACING;
        }
    } else {
        // Inactive: identical gray dots (#B0B0B0) for all three buttons.
        // No glyphs shown on inactive windows (matches real Snow Leopard).
        for (int i = 0; i < 3; i++) {
            double cx = bx + BUTTON_DIAMETER / 2.0;
            double cy = by + BUTTON_DIAMETER / 2.0;
            double r = BUTTON_DIAMETER / 2.0;

            cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
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
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, active ? 0.7 : 0.3);
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
    // Real Snow Leopard outer border: ~RGB(138,138,138) active, ~RGB(180,180,180) inactive
    double bc = active ? 138/255.0 : 180/255.0;
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
