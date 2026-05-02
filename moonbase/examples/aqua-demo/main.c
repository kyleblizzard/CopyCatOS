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
// Slice α — push button (3-slice horizontal blit, regular size, 3 states).
// Slice β — checkbox + radio. Both share a "toggle with value" shape:
// asset chosen by (state, value), drawn as a fixed-size sub-rect blit
// from the SL frame onto the window, label drawn in our own Cairo text
// to the right. Introduces a minimal widget_t record + a per-family
// draw / hit-test dispatch so future slices (bevel, popup, slider, …)
// can be added by extending the family enum and the asset selectors.
// Future slices add push variants (bevel/round/help/disclosure/popup),
// sliders, progress, text fields, segmented controls, tabs, polish.

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
#define WINDOW_H       400
#define CONTENT_BG_R   0xEC
#define CONTENT_BG_G   0xEC
#define CONTENT_BG_B   0xEC

// Push button source frame: 100×24 RGBA. The visible button fills the
// whole frame — Apple did not pad with shadow for the regular push asset
// in this render set. Cap width is the rounded-pillow region on either
// side that must NOT be stretched; the curve resolves at roughly x=8.
#define PUSH_CAP_W     8
#define PUSH_SRC_H     24

// Checkbox / radio source frames are 120×32, with the visible widget at
// the top-left of the frame and a baked window-color background filling
// the rest (where AppKit would draw the label). We blit only the visible
// box rect and draw the label ourselves so the asset's slightly-lighter
// background doesn't bleed into the window content area. Box rects below
// were measured directly from the PNGs — see the widget_bbox script in
// the slice-β commit history.
#define CHECKBOX_SRC_X 4
#define CHECKBOX_SRC_Y 6
#define CHECKBOX_SRC_W 18
#define CHECKBOX_SRC_H 19

#define RADIO_SRC_X    3
#define RADIO_SRC_Y    7
#define RADIO_SRC_W    20
#define RADIO_SRC_H    20

// Gap between a toggle's visible box and its label.
#define TOGGLE_LABEL_GAP 6

// ---------------------------------------------------------------------
// Widget model. A `widget_t` carries enough to draw and hit-test any
// SL widget the demo currently knows. The set is intentionally tiny —
// every new slice may grow this struct, but only when a widget actually
// needs the new field.
// ---------------------------------------------------------------------
typedef enum {
    FAM_PUSH,
    FAM_CHECKBOX,
    FAM_RADIO,
} widget_family_t;

typedef enum {
    STATE_NORMAL,
    STATE_PRESSED,
    STATE_DISABLED,
} widget_state_t;

typedef struct {
    widget_family_t family;
    int             x, y;        // top-left of the widget's visible region
    int             w;            // push only — button width in pt
    const char     *label;
    widget_state_t  state;        // transient: NORMAL or PRESSED
    int             value;        // checkbox: 0=off 1=on 2=mixed; radio: 0|1
    bool            enabled;
    int             click_count;  // push only

    // Filled in by the per-family draw routine on every paint, then
    // read by hit_test on every pointer event. Storing it here means
    // the event loop never recomputes label widths.
    int             hit_x, hit_y, hit_w, hit_h;
} widget_t;

// Slice β layout — one push, two checkboxes, four radios. All radios
// share an implicit single mutex group; clicking one selects it and
// clears the others. Disabled toggles are independent.
//
// y-coords are picked so each row has ~10pt of breathing space. Bumping
// WINDOW_H is the right move when adding a row beyond y≈300.
static widget_t g_widgets[] = {
    { .family = FAM_PUSH,     .x = 40, .y = 40,
      .w = 100, .label = "Click Me",
      .enabled = true },

    { .family = FAM_CHECKBOX, .x = 40, .y = 90,
      .label = "Show Hidden Files (cycle: off / on / mixed)",
      .value = 0, .enabled = true },

    // SL only ships checkbox_disabled_regular.png — no disabled-on or
    // disabled-mixed variant — so a disabled checkbox always paints
    // empty. Demo it as such; "disabled-on" would render misleadingly.
    { .family = FAM_CHECKBOX, .x = 40, .y = 120,
      .label = "Disabled checkbox",
      .value = 0, .enabled = false },

    { .family = FAM_RADIO,    .x = 40, .y = 170,
      .label = "Option A",
      .value = 1, .enabled = true },

    { .family = FAM_RADIO,    .x = 40, .y = 200,
      .label = "Option B",
      .value = 0, .enabled = true },

    { .family = FAM_RADIO,    .x = 40, .y = 230,
      .label = "Option C",
      .value = 0, .enabled = true },

    { .family = FAM_RADIO,    .x = 40, .y = 260,
      .label = "Disabled option",
      .value = 0, .enabled = false },
};

#define WIDGET_COUNT ((int)(sizeof(g_widgets) / sizeof(g_widgets[0])))

// ---------------------------------------------------------------------
// Asset cache — small flat table keyed by relative path. The full
// per-family × per-state × per-value matrix is only ~14 entries today,
// so linear search is the right answer. Swap to a hashmap if we ever
// blow past ~50.
// ---------------------------------------------------------------------
typedef struct {
    const char      *path;   // pointer-equal key (string literal from selector)
    cairo_surface_t *surf;
} asset_entry_t;

static asset_entry_t g_assets[64];
static int           g_asset_count;

static cairo_surface_t *load_asset(const char *rel) {
    for (int i = 0; i < g_asset_count; i++) {
        if (g_assets[i].path == rel) return g_assets[i].surf;
    }
    cairo_surface_t *surf = NULL;
    char *path = moonbase_bundle_resource_path(rel);
    if (!path) {
        fprintf(stderr, "aqua-demo: asset missing: %s\n", rel);
    } else {
        surf = cairo_image_surface_create_from_png(path);
        cairo_status_t st = cairo_surface_status(surf);
        if (st != CAIRO_STATUS_SUCCESS) {
            fprintf(stderr, "aqua-demo: cairo failed to load %s: %s\n",
                    path, cairo_status_to_string(st));
            cairo_surface_destroy(surf);
            surf = NULL;
        }
        free(path);
    }
    if (g_asset_count < (int)(sizeof(g_assets) / sizeof(g_assets[0]))) {
        g_assets[g_asset_count++] = (asset_entry_t){ rel, surf };
    }
    return surf;
}

// ---------------------------------------------------------------------
// Per-family asset selectors. Each returns a string literal so the
// asset cache can use pointer-equality as its key.
// ---------------------------------------------------------------------
static const char *push_asset_path(widget_state_t s) {
    switch (s) {
    case STATE_PRESSED:  return "widgets/buttons/push_pressed_regular.png";
    case STATE_DISABLED: return "widgets/buttons/push_disabled_regular.png";
    default:             return "widgets/buttons/push_active_regular.png";
    }
}

static const char *checkbox_asset_path(widget_state_t s, int value) {
    if (s == STATE_DISABLED) {
        return "widgets/buttons/checkbox_disabled_regular.png";
    }
    if (s == STATE_PRESSED) {
        // SL ships pressed-off and pressed-on; mixed-pressed has no
        // dedicated asset, so we reuse pressed-on (closest visual match
        // — a partially-filled box still gets darker on press).
        return value == 0
            ? "widgets/buttons/checkbox_pressed_regular.png"
            : "widgets/buttons/checkbox_pressed_on_regular.png";
    }
    switch (value) {
    case 1:  return "widgets/buttons/checkbox_active_on_regular.png";
    case 2:  return "widgets/buttons/checkbox_active_mixed_regular.png";
    default: return "widgets/buttons/checkbox_active_regular.png";
    }
}

static const char *radio_asset_path(widget_state_t s, int value) {
    if (s == STATE_DISABLED) {
        return "widgets/buttons/radio_disabled_regular.png";
    }
    if (s == STATE_PRESSED) {
        return value == 0
            ? "widgets/buttons/radio_pressed_regular.png"
            : "widgets/buttons/radio_pressed_on_regular.png";
    }
    return value == 0
        ? "widgets/buttons/radio_active_regular.png"
        : "widgets/buttons/radio_active_on_regular.png";
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
        // Asset-missing fallback: magenta placeholder so the problem
        // is impossible to miss.
        cairo_save(cr);
        cairo_set_source_rgb(cr, 1.0, 0.0, 1.0);
        cairo_rectangle(cr, dst_x, dst_y, dst_w, PUSH_SRC_H);
        cairo_fill(cr);
        cairo_restore(cr);
        return;
    }
    int src_w = cairo_image_surface_get_width(src);
    int src_h = cairo_image_surface_get_height(src);

    // Left cap.
    cairo_save(cr);
    cairo_rectangle(cr, dst_x, dst_y, cap_w, src_h);
    cairo_clip(cr);
    cairo_set_source_surface(cr, src, dst_x, dst_y);
    cairo_paint(cr);
    cairo_restore(cr);

    // Center — stretched.
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
        cairo_set_source_surface(cr, src, -cap_w, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
        cairo_paint(cr);
        cairo_restore(cr);
    }

    // Right cap.
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

// Sub-rect blit — copy a fixed (sw × sh) source rect at (sx, sy) in
// `src` onto (dx, dy) in the target. Used by checkbox/radio so we don't
// paint the asset's baked label-area background over the window.
static void blit_subrect(cairo_t *cr, cairo_surface_t *src,
                         int sx, int sy, int sw, int sh,
                         int dx, int dy) {
    if (!src) {
        cairo_save(cr);
        cairo_set_source_rgb(cr, 1.0, 0.0, 1.0);
        cairo_rectangle(cr, dx, dy, sw, sh);
        cairo_fill(cr);
        cairo_restore(cr);
        return;
    }
    cairo_save(cr);
    cairo_rectangle(cr, dx, dy, sw, sh);
    cairo_clip(cr);
    cairo_set_source_surface(cr, src, dx - sx, dy - sy);
    cairo_paint(cr);
    cairo_restore(cr);
}

// ---------------------------------------------------------------------
// Label drawing helper — one place that knows about Lucida Grande 13pt
// and the SL text-on-content / text-disabled colors.
// ---------------------------------------------------------------------
static void set_label_font(cairo_t *cr) {
    cairo_select_font_face(cr, "Lucida Grande",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
}

static void set_label_color(cairo_t *cr, bool enabled) {
    if (enabled) {
        cairo_set_source_rgb(cr,
                             0x1A / 255.0, 0x1A / 255.0, 0x1A / 255.0);
    } else {
        cairo_set_source_rgb(cr,
                             0xA6 / 255.0, 0xA6 / 255.0, 0xA6 / 255.0);
    }
}

// ---------------------------------------------------------------------
// Per-family draw routines. Each writes its `hit_*` fields into the
// widget so the event loop can hit-test without re-running layout.
// ---------------------------------------------------------------------
static void draw_push(cairo_t *cr, widget_t *w) {
    widget_state_t s = w->enabled ? w->state : STATE_DISABLED;
    cairo_surface_t *surf = load_asset(push_asset_path(s));
    blit_three_slice(cr, surf, w->x, w->y, w->w, PUSH_CAP_W);

    // Label centered in the button — Lucida Grande 13pt. Baseline at
    // y + 16 lands cap-height visually in the middle of the 24pt frame.
    set_label_font(cr);
    set_label_color(cr, w->enabled);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, w->label, &ext);
    double tx = w->x + (w->w - ext.x_advance) / 2.0 - ext.x_bearing;
    double ty = w->y + 16;
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, w->label);

    w->hit_x = w->x;
    w->hit_y = w->y;
    w->hit_w = w->w;
    w->hit_h = PUSH_SRC_H;
}

// Shared toggle (checkbox + radio) draw — same shape, only the source
// rect and asset selector differ.
static void draw_toggle(cairo_t *cr, widget_t *w,
                        int sx, int sy, int sw, int sh,
                        const char *asset_path) {
    cairo_surface_t *surf = load_asset(asset_path);
    blit_subrect(cr, surf, sx, sy, sw, sh, w->x, w->y);

    set_label_font(cr);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, w->label, &ext);
    double label_x = w->x + sw + TOGGLE_LABEL_GAP;
    // Center label vertically against the box. Lucida Grande 13pt
    // cap-height ~9pt; baseline at box_mid + ~4 puts the cap optically
    // centered.
    double baseline_y = w->y + sh / 2.0 + 4;

    set_label_color(cr, w->enabled);
    cairo_move_to(cr, label_x, baseline_y);
    cairo_show_text(cr, w->label);

    int label_w = (int)(ext.x_advance + 0.5);
    w->hit_x = w->x;
    w->hit_y = w->y;
    w->hit_w = sw + TOGGLE_LABEL_GAP + label_w;
    w->hit_h = sh;
}

static void draw_checkbox(cairo_t *cr, widget_t *w) {
    widget_state_t s = w->enabled ? w->state : STATE_DISABLED;
    draw_toggle(cr, w,
                CHECKBOX_SRC_X, CHECKBOX_SRC_Y,
                CHECKBOX_SRC_W, CHECKBOX_SRC_H,
                checkbox_asset_path(s, w->value));
}

static void draw_radio(cairo_t *cr, widget_t *w) {
    widget_state_t s = w->enabled ? w->state : STATE_DISABLED;
    draw_toggle(cr, w,
                RADIO_SRC_X, RADIO_SRC_Y,
                RADIO_SRC_W, RADIO_SRC_H,
                radio_asset_path(s, w->value));
}

static void draw_widget(cairo_t *cr, widget_t *w) {
    switch (w->family) {
    case FAM_PUSH:     draw_push(cr, w);     break;
    case FAM_CHECKBOX: draw_checkbox(cr, w); break;
    case FAM_RADIO:    draw_radio(cr, w);    break;
    }
}

// ---------------------------------------------------------------------
// Hit-test + click semantics.
// ---------------------------------------------------------------------
static bool widget_hit(const widget_t *w, int px, int py) {
    return px >= w->hit_x && px < w->hit_x + w->hit_w
        && py >= w->hit_y && py < w->hit_y + w->hit_h;
}

static widget_t *widget_at(int px, int py) {
    for (int i = 0; i < WIDGET_COUNT; i++) {
        if (widget_hit(&g_widgets[i], px, py)) return &g_widgets[i];
    }
    return NULL;
}

// Apply the click semantics for whichever family was clicked. Called
// only when pointer-up landed inside the widget that received the
// pointer-down — same gesture rule AppKit enforces.
static void widget_click(widget_t *w) {
    switch (w->family) {
    case FAM_PUSH:
        w->click_count++;
        fprintf(stderr,
                "aqua-demo: '%s' clicked (count=%d)\n",
                w->label, w->click_count);
        break;
    case FAM_CHECKBOX:
        // Cycle off → on → mixed → off so a single demo widget exercises
        // every value asset in the SL set.
        w->value = (w->value + 1) % 3;
        break;
    case FAM_RADIO:
        // Single implicit group: clearing every other enabled radio is
        // the simplest correct behavior. Disabled radios are excluded
        // from the group — they keep whatever value they were given.
        for (int i = 0; i < WIDGET_COUNT; i++) {
            widget_t *o = &g_widgets[i];
            if (o != w && o->family == FAM_RADIO && o->enabled) {
                o->value = 0;
            }
        }
        w->value = 1;
        break;
    }
}

// ---------------------------------------------------------------------
// Paint — clear background, then walk the widget list. Status footer
// gives a quick read of which widget last clicked / which radio is
// selected so the demo's state is legible without a debugger.
// ---------------------------------------------------------------------
static void paint(mb_window_t *win) {
    cairo_t *cr = (cairo_t *)moonbase_window_cairo(win);
    if (!cr) return;

    cairo_set_source_rgb(cr,
                         CONTENT_BG_R / 255.0,
                         CONTENT_BG_G / 255.0,
                         CONTENT_BG_B / 255.0);
    cairo_paint(cr);

    for (int i = 0; i < WIDGET_COUNT; i++) {
        draw_widget(cr, &g_widgets[i]);
    }

    // Footer — tiny help line at the bottom.
    set_label_font(cr);
    cairo_set_font_size(cr, 11);
    cairo_set_source_rgb(cr,
                         0x73 / 255.0, 0x73 / 255.0, 0x73 / 255.0);
    cairo_move_to(cr, 40, WINDOW_H - 24);
    cairo_show_text(cr,
                    "Click a widget to interact   |   Cmd-Q to quit");

    moonbase_window_commit(win);
}

// ---------------------------------------------------------------------
// main — app-owns-loop pattern, same shape as textedit. Tracks the
// widget that received pointer-down so the up-event can decide whether
// to fire a click (release-inside) or just clear the pressed state
// (release-outside, classic AppKit cancel).
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

    widget_t *pressed = NULL;  // widget currently in pressed state
    int       running = 1;
    int       exit_code = 0;

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
            if (ev.pointer.button == MB_BUTTON_LEFT) {
                widget_t *w = widget_at(ev.pointer.x, ev.pointer.y);
                if (w && w->enabled) {
                    pressed     = w;
                    w->state    = STATE_PRESSED;
                    dirty       = true;
                }
            }
            break;

        case MB_EV_POINTER_UP:
            if (ev.pointer.button == MB_BUTTON_LEFT && pressed) {
                bool inside = widget_hit(pressed,
                                         ev.pointer.x, ev.pointer.y);
                pressed->state = STATE_NORMAL;
                if (inside && pressed->enabled) {
                    widget_click(pressed);
                }
                pressed = NULL;
                dirty = true;
            }
            break;

        case MB_EV_KEY_DOWN:
            if ((ev.key.modifiers & MB_MOD_COMMAND)
                    && (ev.key.keycode == 'q' || ev.key.keycode == 'Q')) {
                running = 0;
                break;
            }
            break;

        default:
            break;
        }

        if (dirty) paint(win);
    }

    // Drop cached surfaces. Process is exiting either way; this just
    // keeps valgrind quiet.
    for (int i = 0; i < g_asset_count; i++) {
        if (g_assets[i].surf) {
            cairo_surface_destroy(g_assets[i].surf);
            g_assets[i].surf = NULL;
        }
    }

    moonbase_window_close(win);
    return exit_code;
}
