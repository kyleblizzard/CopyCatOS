// CopyCatOS — by Kyle Blizzard at Blizzard.show

// aqua-demo — the canonical visual reference for Snow Leopard Aqua
// fidelity on CopyCatOS. Single scrollable window, every Aqua widget,
// real Snow Leopard PNGs from snowleopardaura/MacAssets/SnowReverseOutput
// shipped inside the bundle's Contents/Resources/widgets/.
//
// Widget drawing code lives only in this demo. libmoonbase stays
// minimal — windows + render mode + events + lifecycle. No widget API
// enters the framework until 4+ apps actually need the same shapes.
// This demo is the proving ground.
//
// Slice α — push button only. One regular push button at (40, 40),
// 120×24, hit-tested against MB_EV_POINTER_DOWN / MB_EV_POINTER_UP,
// painted via 3-slice (left cap | stretched center | right cap) from
// the real SL HITheme assets. States: normal / pressed / disabled.
// Future slices add toggles, sliders, progress, text fields, popups,
// segmented controls, tabs, polish.

#include <moonbase.h>

#include <cairo/cairo.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------
// Visual constants. Points, not pixels — MoonBase scales the Cairo
// surface for us.
// ---------------------------------------------------------------------
#define WINDOW_W       640
#define WINDOW_H       420
#define CONTENT_BG_R   0xEC
#define CONTENT_BG_G   0xEC
#define CONTENT_BG_B   0xEC

// Push button geometry. Snow Leopard's regular-size push button has a
// fixed height; the source PNG is 32 px tall but the visible button is
// 22 pt — Apple bakes a 5px shadow above and 5px below into the asset.
// We blit the full 32 pt and let the surrounding background show
// through the alpha so the shadow lands naturally.
//
// Cap width is the rounded-pillow region on either side of the source
// PNG that must NOT be stretched. Inspecting push_active_regular.png
// (120×32 RGBA): the rounded curve resolves at roughly x=8, so the
// flat center begins at x=8 and the right cap starts at x=112. Tune
// here if a future visual review on Legion suggests a different value.
#define PUSH_CAP_W     8
#define PUSH_SRC_H     32

// One-button slice α layout. The clickable target is the full source
// frame including the alpha shadow — matches how AppKit hit-tests
// push buttons in SL.
#define BUTTON_X       40
#define BUTTON_Y       40
#define BUTTON_W       120
#define BUTTON_H       PUSH_SRC_H

// ---------------------------------------------------------------------
// Asset cache — one cairo_image_surface_t per state, loaded once on
// first paint. We keep them around for the process lifetime; the demo
// is short-lived enough that lazy-eviction isn't worth the code.
// ---------------------------------------------------------------------
typedef enum {
    PUSH_NORMAL = 0,
    PUSH_PRESSED,
    PUSH_DISABLED,
    PUSH_STATE_COUNT,
} push_state_t;

static const char *PUSH_ASSET_NAMES[PUSH_STATE_COUNT] = {
    [PUSH_NORMAL]   = "widgets/buttons/push_active_regular.png",
    [PUSH_PRESSED]  = "widgets/buttons/push_pressed_regular.png",
    [PUSH_DISABLED] = "widgets/buttons/push_disabled_regular.png",
};

static cairo_surface_t *g_push_surfaces[PUSH_STATE_COUNT];

// Load one PNG from the bundle's Resources/. Caches it in the
// per-state slot. Returns NULL on failure (caller falls back to a
// flat-rect placeholder so a missing asset is visible, not crashy).
static cairo_surface_t *load_push_asset(push_state_t s) {
    if (g_push_surfaces[s]) return g_push_surfaces[s];

    char *path = moonbase_bundle_resource_path(PUSH_ASSET_NAMES[s]);
    if (!path) {
        fprintf(stderr, "aqua-demo: asset missing: %s\n",
                PUSH_ASSET_NAMES[s]);
        return NULL;
    }
    cairo_surface_t *surf = cairo_image_surface_create_from_png(path);
    cairo_status_t status = cairo_surface_status(surf);
    if (status != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "aqua-demo: cairo failed to load %s: %s\n",
                path, cairo_status_to_string(status));
        cairo_surface_destroy(surf);
        free(path);
        return NULL;
    }
    free(path);
    g_push_surfaces[s] = surf;
    return surf;
}

// ---------------------------------------------------------------------
// 3-slice horizontal blit. Source is laid out [LL CCCC RR]; left cap
// and right cap are pixel-copied at native size to preserve the
// rounded-corner curvature, the center is scaled to fill whatever
// width remains. CAIRO_FILTER_BEST on the stretched center keeps
// gradients smooth across fractional scales.
// ---------------------------------------------------------------------
static void blit_three_slice(cairo_t *cr, cairo_surface_t *src,
                             int dst_x, int dst_y, int dst_w,
                             int cap_w) {
    if (!src) {
        // Asset-missing fallback: draw a magenta placeholder so the
        // problem is impossible to miss.
        cairo_save(cr);
        cairo_set_source_rgb(cr, 1.0, 0.0, 1.0);
        cairo_rectangle(cr, dst_x, dst_y, dst_w, BUTTON_H);
        cairo_fill(cr);
        cairo_restore(cr);
        return;
    }
    int src_w = cairo_image_surface_get_width(src);
    int src_h = cairo_image_surface_get_height(src);

    // Left cap — native-size copy from src(0..cap_w) to dst(0..cap_w).
    cairo_save(cr);
    cairo_rectangle(cr, dst_x, dst_y, cap_w, src_h);
    cairo_clip(cr);
    cairo_set_source_surface(cr, src, dst_x, dst_y);
    cairo_paint(cr);
    cairo_restore(cr);

    // Center — stretched. Source slice is x=cap_w..src_w-cap_w
    // (width = src_w - 2*cap_w), drawn into dst x=cap_w..dst_w-cap_w
    // (width = dst_w - 2*cap_w). Scale the device-coord transform
    // before setting the source so cairo applies BILINEAR/BEST filter
    // on a single pattern instead of a manual sub-rect copy.
    int center_src_w = src_w - 2 * cap_w;
    int center_dst_w = dst_w - 2 * cap_w;
    if (center_src_w > 0 && center_dst_w > 0) {
        cairo_save(cr);
        cairo_rectangle(cr,
                        dst_x + cap_w, dst_y,
                        center_dst_w, src_h);
        cairo_clip(cr);
        double sx = (double)center_dst_w / (double)center_src_w;
        cairo_translate(cr, dst_x + cap_w, dst_y);
        cairo_scale(cr, sx, 1.0);
        // After scale, source-image origin needs to land at -cap_w
        // in source-space so the center slice aligns under the
        // clipped rectangle.
        cairo_set_source_surface(cr, src, -cap_w, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
        cairo_paint(cr);
        cairo_restore(cr);
    }

    // Right cap — native-size copy from src(src_w-cap_w..src_w) to
    // dst(dst_w-cap_w..dst_w). Same clip-and-paint trick as the left.
    cairo_save(cr);
    cairo_rectangle(cr,
                    dst_x + dst_w - cap_w, dst_y,
                    cap_w, src_h);
    cairo_clip(cr);
    cairo_set_source_surface(cr,
                             src,
                             dst_x + dst_w - cap_w - (src_w - cap_w),
                             dst_y);
    cairo_paint(cr);
    cairo_restore(cr);
}

// ---------------------------------------------------------------------
// Push button — full draw including the centered Lucida Grande label.
// state selects which asset family to blit; `enabled` falls back to
// the disabled asset and a dimmer label color when false.
// ---------------------------------------------------------------------
static void draw_push_button(cairo_t *cr,
                             int x, int y, int w,
                             const char *label,
                             push_state_t state,
                             bool enabled) {
    push_state_t s = enabled ? state : PUSH_DISABLED;
    cairo_surface_t *surf = load_push_asset(s);
    blit_three_slice(cr, surf, x, y, w, PUSH_CAP_W);

    // Label — Lucida Grande 13 pt, vertically centered in the visible
    // pillow (y_top + ~PUSH_SRC_H/2 + 4 puts the baseline at the
    // optical center for SL push buttons).
    cairo_select_font_face(cr, "Lucida Grande",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    if (enabled) {
        cairo_set_source_rgb(cr,
                             0x1A / 255.0, 0x1A / 255.0, 0x1A / 255.0);
    } else {
        cairo_set_source_rgb(cr,
                             0xA6 / 255.0, 0xA6 / 255.0, 0xA6 / 255.0);
    }
    cairo_text_extents_t ext;
    cairo_text_extents(cr, label, &ext);
    double tx = x + (w - ext.x_advance) / 2.0 - ext.x_bearing;
    // PUSH_SRC_H 32 = 5 top shadow + 22 pillow + 5 bottom shadow.
    // Baseline at y + 5 + 15 = y + 20 lands the cap-height of "Click"
    // visually in the middle of the pillow.
    double ty = y + 20;
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, label);
}

// ---------------------------------------------------------------------
// Demo state — minimal: one button, with pressed and enabled flags.
// pressed flips on MB_EV_POINTER_DOWN inside the hit rect, off on
// MB_EV_POINTER_UP. A "click" only fires when the up lands inside the
// same rect — matches AppKit's semantics. Disabled toggle is wired to
// a key shortcut so we can prove both states paint without needing a
// second widget yet.
// ---------------------------------------------------------------------
typedef struct {
    bool pressed;       // mouse currently held down inside the button
    bool enabled;       // false renders the disabled asset
    int  click_count;   // cumulative; printed on each click
} button_t;

static button_t g_button = { .enabled = true };

static bool point_in_button(int px, int py) {
    return px >= BUTTON_X
        && px <  BUTTON_X + BUTTON_W
        && py >= BUTTON_Y
        && py <  BUTTON_Y + BUTTON_H;
}

// ---------------------------------------------------------------------
// Paint the whole window. v0.1 has one push button + a status string;
// future slices append more widgets here (or refactor into a
// per-section paint table once 4+ widgets share the layout pain).
// ---------------------------------------------------------------------
static void paint(mb_window_t *w) {
    cairo_t *cr = (cairo_t *)moonbase_window_cairo(w);
    if (!cr) return;

    int width = 0, height = 0;
    moonbase_window_size(w, &width, &height);

    // Window background — Snow Leopard window-content gray #ECECEC.
    cairo_set_source_rgb(cr,
                         CONTENT_BG_R / 255.0,
                         CONTENT_BG_G / 255.0,
                         CONTENT_BG_B / 255.0);
    cairo_paint(cr);

    // The one button.
    push_state_t state = g_button.pressed ? PUSH_PRESSED : PUSH_NORMAL;
    draw_push_button(cr,
                     BUTTON_X, BUTTON_Y, BUTTON_W,
                     "Click Me",
                     state,
                     g_button.enabled);

    // Status line — confirms click events round-trip without needing
    // a console open. Updated on each click; sits below the button.
    cairo_select_font_face(cr, "Lucida Grande",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    cairo_set_source_rgb(cr,
                         0x73 / 255.0, 0x73 / 255.0, 0x73 / 255.0);
    char status[128];
    snprintf(status, sizeof(status),
             "clicks: %d   |   press space to toggle enabled (currently %s)",
             g_button.click_count,
             g_button.enabled ? "on" : "off");
    cairo_move_to(cr, BUTTON_X, BUTTON_Y + BUTTON_H + 24);
    cairo_show_text(cr, status);

    // Slice α scope marker — silences -Wunused if width/height aren't
    // consumed yet. Future slices use these for layout.
    (void)width;
    (void)height;

    moonbase_window_commit(w);
}

// ---------------------------------------------------------------------
// main — app-owns-loop pattern, same shape as textedit. Paint after
// any state change so the surface mirrors logical state immediately.
// ---------------------------------------------------------------------
int main(int argc, char **argv) {
    int rc = moonbase_init(argc, argv);
    if (rc != MB_EOK) {
        fprintf(stderr,
                "aqua-demo: moonbase_init failed: %s\n",
                moonbase_error_string((mb_error_t)rc));
        return 1;
    }

    mb_window_desc_t desc = {
        .version       = MOONBASE_WINDOW_DESC_VERSION,
        .title         = "Aqua Demo",
        .width_points  = WINDOW_W,
        .height_points = WINDOW_H,
        .render_mode   = MOONBASE_RENDER_CAIRO,
        .flags         = MB_WINDOW_FLAG_CENTER,
    };
    mb_window_t *win = moonbase_window_create(&desc);
    if (!win) {
        fprintf(stderr,
                "aqua-demo: moonbase_window_create failed: %s\n",
                moonbase_error_string(moonbase_last_error()));
        return 1;
    }

    paint(win);

    int running   = 1;
    int exit_code = 0;
    while (running) {
        mb_event_t ev;
        int wr = moonbase_wait_event(&ev, -1);
        if (wr < 0) {
            fprintf(stderr,
                    "aqua-demo: wait_event error: %s\n",
                    moonbase_error_string((mb_error_t)wr));
            exit_code = 1;
            break;
        }
        if (wr == 0) continue;

        bool dirty = false;

        switch (ev.kind) {
        case MB_EV_WINDOW_REDRAW:
            dirty = true;
            break;

        case MB_EV_WINDOW_CLOSED:
        case MB_EV_APP_WILL_QUIT:
            running = 0;
            break;

        case MB_EV_POINTER_DOWN:
            if (ev.pointer.button == MB_BUTTON_LEFT
                    && g_button.enabled
                    && point_in_button(ev.pointer.x, ev.pointer.y)) {
                g_button.pressed = true;
                dirty = true;
            }
            break;

        case MB_EV_POINTER_UP:
            if (ev.pointer.button == MB_BUTTON_LEFT && g_button.pressed) {
                bool inside = point_in_button(ev.pointer.x, ev.pointer.y);
                g_button.pressed = false;
                if (inside && g_button.enabled) {
                    g_button.click_count++;
                    fprintf(stderr,
                            "aqua-demo: button clicked (count=%d)\n",
                            g_button.click_count);
                }
                dirty = true;
            }
            break;

        case MB_EV_KEY_DOWN:
            // Cmd-Q quits — same hotkey TextEdit uses.
            if ((ev.key.modifiers & MB_MOD_COMMAND)
                    && (ev.key.keycode == 'q' || ev.key.keycode == 'Q')) {
                running = 0;
                break;
            }
            // Space toggles enabled, so we can see the disabled
            // asset paint without needing a second widget yet.
            if (ev.key.keycode == ' ') {
                g_button.enabled = !g_button.enabled;
                if (!g_button.enabled) g_button.pressed = false;
                dirty = true;
            }
            break;

        default:
            break;
        }

        if (dirty) paint(win);
    }

    // Drop cached surfaces — process is exiting but it's the polite
    // thing to do, and it makes valgrind happy.
    for (int i = 0; i < PUSH_STATE_COUNT; i++) {
        if (g_push_surfaces[i]) {
            cairo_surface_destroy(g_push_surfaces[i]);
            g_push_surfaces[i] = NULL;
        }
    }

    moonbase_window_close(win);
    return exit_code;
}
