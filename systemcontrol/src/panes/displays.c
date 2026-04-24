// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// panes/displays.c — Displays preferences pane (v1-minimum: scale picker +
//                    primary toggle)
// ============================================================================
//
// Enumerates XRandR outputs directly (the _MOONROCK_OUTPUT_SCALES atom only
// carries name + geometry + effective scale, not mm dimensions), reads each
// output's current effective scale + primary flag from the atom, and renders
// a row per output with:
//   - a segmented pill control showing the 11 scale choices 0.50 → 3.00 in
//     0.25 steps,
//   - a right-aligned "Primary display" radio that picks which output owns
//     the menu bar (and anchors dock/desktop placement in later slices).
//
// Clicking a pill sends moonrock_request_scale() over _MOONROCK_SET_OUTPUT_SCALE.
// Clicking a primary radio sends moonrock_request_primary() over
// _MOONROCK_SET_PRIMARY_OUTPUT. Both updates are optimistic locally —
// MoonRock re-publishes on _MOONROCK_OUTPUT_SCALES and anyone who subscribed
// (menubar, dock, desktop) gets the change via PropertyNotify; the pane
// refreshes from the atom on next entry.
// ============================================================================

#include "displays.h"
#include "moonrock_scale.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <X11/extensions/Xrandr.h>
#include <pango/pangocairo.h>

// ── Layout constants (in pixels inside the fixed 668×500 pane window) ──

#define PANE_LEFT          24
#define PANE_RIGHT         644
#define ROW_TOP_MARGIN     60   // below the toolbar (TOOLBAR_HEIGHT=38)
#define ROW_HEIGHT         146  // grows to hold the Interface Scale slider
#define ROW_V_GAP          8
#define ROW_TITLE_Y         6   // Y offset inside row for "eDP-1 — 1920×1200"
#define ROW_META_Y         26   // Y offset for "14" panel · 323 PPI" etc.
#define PILL_Y             52   // Y offset for the scale pills
#define PILL_H             28
#define PILL_RADIUS         6.0

// ── Interface Scale slider — per-output multiplier (0.5 – 4.0×) ────────
// Sits below the backing-scale pill strip. Continuous knob with tick
// marks at the canonical 0.5 / 0.75 / 1 / 1.25 / 1.5 / 2 / 2.5 / 3 / 4
// positions; drag is free-form, tick marks are visual only for now.
#define ISCALE_LABEL_Y    90    // "Interface Scale" caption above track
#define ISCALE_TRACK_Y   108    // track center (tick marks hang below)
#define ISCALE_TICK_H      4    // tick length below the track
#define ISCALE_TICK_LBL_Y 118   // Y of the tick value labels
#define ISCALE_KNOB_R      7    // knob radius
#define ISCALE_VAL_GAP    12    // gap between track end and current value
#define ISCALE_MIN       0.5f
#define ISCALE_MAX       4.0f
#define ISCALE_HIT_PAD    10    // extra vertical slop for grabbing the knob

// Canonical tick positions — these are the values Kyle called out in the
// slice A spec. Drawn as short vertical marks on the track; their labels
// appear below. The knob moves continuously between them, so a user can
// pick any value in [0.5, 4.0] and MoonRock will store it verbatim.
#define ISCALE_TICK_COUNT 9
static const float ISCALE_TICKS[ISCALE_TICK_COUNT] = {
    0.50f, 0.75f, 1.00f, 1.25f, 1.50f, 2.00f, 2.50f, 3.00f, 4.00f,
};

// Primary radio — right-aligned on the title row. The whole 120px block
// (circle + label) is the clickable target; clicking anywhere in it flips
// that output to primary.
#define PRIMARY_CTRL_W    128   // reserved width at the right edge
#define PRIMARY_RADIO_D    14   // outer-circle diameter
#define PRIMARY_RADIO_IN    5   // inner-dot diameter when selected
#define PRIMARY_GAP         6   // space between circle and label
#define PRIMARY_HIT_PAD_Y   4   // vertical slop for hit-testing the radio

// Rotation pills — right-aligned on the meta row. Four compact pills:
// "0°", "90°", "180°", "270°". Total strip width 4*ROT_PILL_W + 3*ROT_GAP.
#define ROT_COUNT           4
#define ROT_PILL_W         36
#define ROT_PILL_H         20
#define ROT_GAP             4
#define ROT_PILL_RADIUS     4.0
#define ROT_PILL_Y_ADJ     (-2)   // nudge up so pills center on meta text

// ── Scale choices ───────────────────────────────────────────────────────

#define STEP_COUNT         11
static const float SCALE_STEPS[STEP_COUNT] = {
    0.50f, 0.75f, 1.00f, 1.25f, 1.50f,
    1.75f, 2.00f, 2.25f, 2.50f, 2.75f, 3.00f,
};

// ── Per-output row state ───────────────────────────────────────────────

#define MAX_ROWS  8

static const int ROT_DEGREES[ROT_COUNT] = { 0, 90, 180, 270 };

typedef struct {
    char  name[MOONROCK_SCALE_NAME_MAX];
    int   width, height;          // native resolution (pixels)
    int   mm_width, mm_height;    // physical size from EDID
    float current_scale;          // effective scale MoonRock is using
                                  // (= backing × multiplier)
    float multiplier;             // Interface Scale multiplier from table;
                                  // 1.0 when no user override exists
    float drag_multiplier;        // live knob value during a drag, before
                                  // the ButtonRelease atom write commits;
                                  // 0.0 when no drag is in progress
    int   picked_step;            // last clicked step; -1 for "match current"
    bool  is_primary;             // reflected from _MOONROCK_OUTPUT_SCALES
    int   rotation;               // 0 / 90 / 180 / 270 — from scale table
} DisplayRow;

static DisplayRow g_rows[MAX_ROWS];
static int        g_row_count = 0;
static int        g_hover_row = -1;
static int        g_hover_step = -1;
static bool       g_subscribed = false;   // moonrock_scale_init once-only
static bool       g_needs_refresh = true; // re-enumerate on pane entry

// Row currently driving an Interface Scale drag, or -1 if nothing is being
// dragged. The drag starts on ButtonPress inside the knob / track, keeps the
// local `drag_multiplier` in sync on MotionNotify, and commits the final
// value over _MOONROCK_SET_OUTPUT_INTERFACE_SCALE on ButtonRelease. We
// never hit the atom mid-drag — that'd re-publish the scale table on every
// pointer tick and flood PropertyNotify traffic.
static int  g_iscale_drag_row = -1;

// ── Helpers ─────────────────────────────────────────────────────────────

// Nearest step to the given scale — used to highlight the active pill when
// MoonRock's value doesn't exactly hit one of our discrete steps (e.g. an
// EDID-derived default of 1.75 for the Legion Go S panel matches exactly;
// a future 1.40 custom scale from the config file will be shown as 1.50).
static int nearest_step(float scale)
{
    int best = 0;
    float best_d = 99.0f;
    for (int i = 0; i < STEP_COUNT; i++) {
        float d = scale - SCALE_STEPS[i];
        if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

// ── XRandR enumeration ─────────────────────────────────────────────────

// Walk the XRandR outputs, keeping only those that are currently connected
// and have a CRTC (i.e. actively driving a monitor). mm_width / mm_height
// come straight from EDID — 0 on adapters that don't report it, which we
// treat as "unknown" in the UI.
static void enumerate_outputs(Display *dpy, Window root)
{
    // Drop any in-flight drag — after a hotplug the old row indices may
    // point at a different output (or none at all), so committing the
    // drag buffer would send the wrong output's atom.
    g_iscale_drag_row = -1;
    g_row_count = 0;

    XRRScreenResources *res = XRRGetScreenResources(dpy, root);
    if (!res) return;

    for (int i = 0; i < res->noutput && g_row_count < MAX_ROWS; i++) {
        XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
        if (!oi) continue;

        if (oi->connection == RR_Connected && oi->crtc) {
            XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
            if (ci) {
                DisplayRow *r = &g_rows[g_row_count++];
                strncpy(r->name, oi->name, sizeof(r->name) - 1);
                r->name[sizeof(r->name) - 1] = '\0';
                r->width     = ci->width;
                r->height    = ci->height;
                r->mm_width  = oi->mm_width;
                r->mm_height = oi->mm_height;
                r->current_scale    = 1.0f;  // filled in by refresh_scales()
                r->multiplier       = 1.0f;  // filled in by refresh_scales()
                r->drag_multiplier  = 0.0f;  // no drag in progress
                r->picked_step      = -1;
                r->is_primary       = false; // filled in by refresh_scales()
                r->rotation         = 0;     // filled in by refresh_scales()
                XRRFreeCrtcInfo(ci);
            }
        }

        XRRFreeOutputInfo(oi);
    }

    XRRFreeScreenResources(res);
}

// Read _MOONROCK_OUTPUT_SCALES and copy the effective scale + primary flag
// into each row. Rows whose name isn't in the table (e.g. MoonRock hasn't
// started yet) stay at scale 1.0 and primary=false.
static void refresh_scales(Display *dpy)
{
    MoonRockScaleTable table;
    moonrock_scale_refresh(dpy, &table);

    for (int i = 0; i < g_row_count; i++) {
        g_rows[i].current_scale = 1.0f;
        g_rows[i].multiplier    = 1.0f;
        g_rows[i].is_primary    = false;
        g_rows[i].rotation      = 0;
        if (!table.valid) continue;
        for (int j = 0; j < table.count; j++) {
            if (strcmp(table.outputs[j].name, g_rows[i].name) == 0) {
                const MoonRockOutputScale *o = &table.outputs[j];
                g_rows[i].current_scale = o->scale;
                g_rows[i].multiplier    = (o->multiplier > 0.0f)
                                            ? o->multiplier
                                            : 1.0f;
                g_rows[i].is_primary    = o->primary;
                g_rows[i].rotation      = o->rotation;
                break;
            }
        }
    }
}

// Effective multiplier to show on the slider. During a drag this is the
// live knob position (not yet committed to MoonRock); otherwise it's
// whatever MoonRock last published in the scale table.
static float row_display_multiplier(const DisplayRow *r)
{
    return (r->drag_multiplier > 0.0f) ? r->drag_multiplier : r->multiplier;
}

// Backing scale the pill picker should highlight. The published
// current_scale field is the effective value (backing × multiplier after
// A.1); divide out the multiplier so clicking a 1× pill still means
// "render at 1× density" regardless of the Interface Scale slider.
static float row_backing_scale(const DisplayRow *r)
{
    float mult = (r->multiplier > 0.0f) ? r->multiplier : 1.0f;
    return r->current_scale / mult;
}

// Map a multiplier value to an X position on the slider track. The track
// spans [PANE_LEFT, PANE_RIGHT] across [ISCALE_MIN, ISCALE_MAX]. Clamping
// is at call sites — this helper trusts the caller to stay in-range.
static double iscale_multiplier_to_x(float m)
{
    double frac = (double)(m - ISCALE_MIN) / (ISCALE_MAX - ISCALE_MIN);
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    return PANE_LEFT + frac * (PANE_RIGHT - PANE_LEFT);
}

// Map a pixel X on the slider track back to a multiplier. Values outside
// the track clamp to the endpoints so dragging off the edges still pins
// the knob at 0.5 / 4.0 rather than leaving it stuck.
static float iscale_x_to_multiplier(int x)
{
    double frac = (double)(x - PANE_LEFT) / (double)(PANE_RIGHT - PANE_LEFT);
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    return ISCALE_MIN + (float)frac * (ISCALE_MAX - ISCALE_MIN);
}

// Snap the raw multiplier to the nearest canonical tick if the pointer is
// close enough. Keeps the user from having to hit a pixel-exact 1.0× —
// near-misses round to the tick. Slop is 0.03 on either side, chosen so
// neighbours (1.00 / 1.25) don't overlap.
static float iscale_maybe_snap(float raw)
{
    const float SLOP = 0.03f;
    for (int i = 0; i < ISCALE_TICK_COUNT; i++) {
        float d = raw - ISCALE_TICKS[i];
        if (d < 0.0f) d = -d;
        if (d <= SLOP) return ISCALE_TICKS[i];
    }
    return raw;
}

// ── Drawing helpers ────────────────────────────────────────────────────

// Draw the right-aligned "Primary display" radio + label for one row.
// The circle's left edge sits at (PANE_RIGHT - PRIMARY_CTRL_W), vertically
// centered with the title text. Label is Lucida Grande 12; text + circle
// both shift to Aqua selection blue when primary, gray otherwise, so state
// is readable even without color cues.
static void draw_primary_radio(cairo_t *cr, double title_y, bool is_primary)
{
    const double cx = PANE_RIGHT - PRIMARY_CTRL_W + PRIMARY_RADIO_D / 2.0;
    const double cy = title_y + 9.0; // roughly cap-height center of Bold 13

    if (is_primary) {
        // Filled blue outer circle + white inner dot
        cairo_set_source_rgb(cr, 0.220, 0.459, 0.843); // #3875D7
        cairo_arc(cr, cx, cy, PRIMARY_RADIO_D / 2.0, 0, 2 * 3.14159265358979);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_arc(cr, cx, cy, PRIMARY_RADIO_IN / 2.0, 0, 2 * 3.14159265358979);
        cairo_fill(cr);
    } else {
        // White fill, gray outline
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_arc(cr, cx, cy, PRIMARY_RADIO_D / 2.0, 0, 2 * 3.14159265358979);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
        cairo_set_line_width(cr, 1.0);
        cairo_arc(cr, cx, cy, PRIMARY_RADIO_D / 2.0 - 0.5,
                  0, 2 * 3.14159265358979);
        cairo_stroke(cr);
    }

    // Label — "Primary display" in blue when active, secondary-gray when not
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, "Primary display", -1);
    PangoFontDescription *font =
        pango_font_description_from_string("Lucida Grande 12");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    int lw, lh;
    pango_layout_get_pixel_size(layout, &lw, &lh);

    if (is_primary) {
        cairo_set_source_rgb(cr, 0.220, 0.459, 0.843);
    } else {
        cairo_set_source_rgb(cr, 0.40, 0.40, 0.40);
    }
    cairo_move_to(cr,
                  PANE_RIGHT - PRIMARY_CTRL_W + PRIMARY_RADIO_D + PRIMARY_GAP,
                  title_y + (PRIMARY_RADIO_D + 2 - lh) / 2.0 + 1);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);

    (void)lw;
}

// Filled rounded rectangle — used for pill buttons and the row background.
static void rounded_rect(cairo_t *cr, double x, double y,
                         double w, double h, double r)
{
    const double pi = 3.14159265358979323846;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -pi / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r,  0,      pi / 2);
    cairo_arc(cr, x + r,     y + h - r, r,  pi / 2, pi);
    cairo_arc(cr, x + r,     y + r,     r,  pi,     3 * pi / 2);
    cairo_close_path(cr);
}

// Draw the pill strip for one row. Pills share gaps so the strip reads as
// a single segmented control. Active pill is filled Aqua blue; hovered
// pill gets a soft highlight.
static void draw_pill_strip(cairo_t *cr, int row_index,
                            double x0, double y0, double strip_w,
                            int active_step, int hover_step)
{
    double pill_w = strip_w / STEP_COUNT;

    for (int i = 0; i < STEP_COUNT; i++) {
        double x = x0 + i * pill_w;

        // Background — active pill is the selection blue, idle pill is
        // a light gray, hover pill is a softer blue tint.
        if (i == active_step) {
            cairo_set_source_rgb(cr, 0.220, 0.459, 0.843); // #3875D7
        } else if (i == hover_step) {
            cairo_set_source_rgba(cr, 0.220, 0.459, 0.843, 0.20);
        } else {
            cairo_set_source_rgb(cr, 0.93, 0.93, 0.93);
        }
        rounded_rect(cr, x + 2, y0, pill_w - 4, PILL_H, PILL_RADIUS);
        cairo_fill(cr);

        // Border — 1px darker edge so the segments read cleanly
        cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
        cairo_set_line_width(cr, 1.0);
        rounded_rect(cr, x + 2, y0, pill_w - 4, PILL_H, PILL_RADIUS);
        cairo_stroke(cr);

        // Label: "0.5×", "1×", "1.75×", "3×". %g suppresses trailing zeros
        // so whole scales read as "1×" instead of "1.00×".
        char label[16];
        snprintf(label, sizeof(label), "%g×", (double)SCALE_STEPS[i]);

        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, label, -1);
        PangoFontDescription *font =
            pango_font_description_from_string("Lucida Grande 11");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);

        int lw, lh;
        pango_layout_get_pixel_size(layout, &lw, &lh);

        if (i == active_step) {
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        } else {
            cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
        }
        cairo_move_to(cr, x + (pill_w - lw) / 2.0,
                      y0 + (PILL_H - lh) / 2.0);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }

    (void)row_index;
}

// X of the leftmost rotation pill on the meta row — right-aligned so the
// rightmost pill's right edge sits at PANE_RIGHT. Shared between draw and
// hit-test so the two stay in lockstep even if the constants move.
static double rotation_strip_x0(void)
{
    double strip_w = ROT_COUNT * ROT_PILL_W + (ROT_COUNT - 1) * ROT_GAP;
    return (double)PANE_RIGHT - strip_w;
}

// Y of the rotation pill strip relative to the row's top — anchored to the
// meta-row baseline, then nudged up a few px so the pills visually center
// on the meta text. Kept separate from the pill's y-inside-row constant so
// adjustments don't accidentally shift row geometry.
static double rotation_strip_y(double row_y)
{
    return row_y + ROW_META_Y + ROT_PILL_Y_ADJ;
}

// Draw the 4-pill rotation strip at the right end of the meta row.
// active_deg is the rotation from the scale table (0/90/180/270); the
// matching pill renders filled-blue, the rest render idle-gray.
static void draw_rotation_pills(cairo_t *cr, double x0, double y0,
                                int active_deg, int hover_index)
{
    for (int i = 0; i < ROT_COUNT; i++) {
        double x = x0 + i * (ROT_PILL_W + ROT_GAP);
        bool is_active = (active_deg == ROT_DEGREES[i]);

        // Pill fill
        if (is_active) {
            cairo_set_source_rgb(cr, 0.220, 0.459, 0.843);
        } else if (i == hover_index) {
            cairo_set_source_rgba(cr, 0.220, 0.459, 0.843, 0.18);
        } else {
            cairo_set_source_rgb(cr, 0.93, 0.93, 0.93);
        }
        rounded_rect(cr, x, y0, ROT_PILL_W, ROT_PILL_H, ROT_PILL_RADIUS);
        cairo_fill(cr);

        // Edge
        cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
        cairo_set_line_width(cr, 1.0);
        rounded_rect(cr, x, y0, ROT_PILL_W, ROT_PILL_H, ROT_PILL_RADIUS);
        cairo_stroke(cr);

        // Label — "0°" / "90°" / "180°" / "270°"
        char label[8];
        snprintf(label, sizeof(label), "%d°", ROT_DEGREES[i]);

        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, label, -1);
        PangoFontDescription *font =
            pango_font_description_from_string("Lucida Grande 10");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);

        int lw, lh;
        pango_layout_get_pixel_size(layout, &lw, &lh);

        if (is_active) {
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        } else {
            cairo_set_source_rgb(cr, 0.20, 0.20, 0.20);
        }
        cairo_move_to(cr, x + (ROT_PILL_W - lw) / 2.0,
                      y0 + (ROT_PILL_H - lh) / 2.0);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }
}

// Draw the Interface Scale slider for one row. `multiplier` is whatever
// we want the knob to reflect right now — either MoonRock's published
// value or the in-flight drag position. `is_dragging` subtly highlights
// the knob so the user can see which row they're adjusting.
static void draw_interface_scale_slider(cairo_t *cr, int row_index,
                                        double row_y, float multiplier,
                                        bool is_dragging)
{
    (void)row_index;

    // ── Caption: "Interface Scale" (bold, matches other pane labels)
    {
        PangoLayout *lbl = pango_cairo_create_layout(cr);
        pango_layout_set_text(lbl, "Interface Scale", -1);
        PangoFontDescription *f =
            pango_font_description_from_string("Lucida Grande Bold 11");
        pango_layout_set_font_description(lbl, f);
        pango_font_description_free(f);
        cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
        cairo_move_to(cr, PANE_LEFT, row_y + ISCALE_LABEL_Y);
        pango_cairo_show_layout(cr, lbl);
        g_object_unref(lbl);
    }

    // ── Track — 2px gray line across the row
    double track_y = row_y + ISCALE_TRACK_Y;
    cairo_set_source_rgb(cr, 0.72, 0.72, 0.72);
    cairo_set_line_width(cr, 2.0);
    cairo_move_to(cr, PANE_LEFT, track_y);
    cairo_line_to(cr, PANE_RIGHT, track_y);
    cairo_stroke(cr);

    // ── Tick marks + labels at canonical multipliers
    PangoFontDescription *tick_font =
        pango_font_description_from_string("Lucida Grande 9");
    for (int t = 0; t < ISCALE_TICK_COUNT; t++) {
        double tx = iscale_multiplier_to_x(ISCALE_TICKS[t]);

        cairo_set_source_rgb(cr, 0.60, 0.60, 0.60);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, tx, track_y + 2);
        cairo_line_to(cr, tx, track_y + 2 + ISCALE_TICK_H);
        cairo_stroke(cr);

        // Skip the label when two ticks would visually collide (1.25/1.5
        // at 0.25-spacing are fine; the jump to 2.0 leaves plenty of
        // room). Drawing every label keeps the control self-documenting.
        char buf[16];
        snprintf(buf, sizeof(buf), "%g×", (double)ISCALE_TICKS[t]);

        PangoLayout *lay = pango_cairo_create_layout(cr);
        pango_layout_set_text(lay, buf, -1);
        pango_layout_set_font_description(lay, tick_font);
        int lw, lh;
        pango_layout_get_pixel_size(lay, &lw, &lh);
        cairo_set_source_rgb(cr, 0.40, 0.40, 0.40);
        cairo_move_to(cr, tx - lw / 2.0, row_y + ISCALE_TICK_LBL_Y);
        pango_cairo_show_layout(cr, lay);
        g_object_unref(lay);

        (void)lh;
    }
    pango_font_description_free(tick_font);

    // ── Knob — white fill, gray border; shadow underneath. Matches
    // dock/power slider knobs.
    float mult = multiplier;
    if (mult <= 0.0f) mult = 1.0f;
    if (mult < ISCALE_MIN) mult = ISCALE_MIN;
    if (mult > ISCALE_MAX) mult = ISCALE_MAX;
    double knob_x = iscale_multiplier_to_x(mult);

    cairo_set_source_rgba(cr, 0, 0, 0, 0.15);
    cairo_arc(cr, knob_x, track_y + 1, ISCALE_KNOB_R + 1, 0, 2 * M_PI);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, is_dragging ? 1.0 : 0.98,
                             is_dragging ? 1.0 : 0.98,
                             is_dragging ? 1.0 : 0.98);
    cairo_arc(cr, knob_x, track_y, ISCALE_KNOB_R, 0, 2 * M_PI);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, is_dragging ? 0.22 : 0.65,
                             is_dragging ? 0.46 : 0.65,
                             is_dragging ? 0.84 : 0.65);
    cairo_set_line_width(cr, is_dragging ? 1.5 : 1.0);
    cairo_arc(cr, knob_x, track_y, ISCALE_KNOB_R, 0, 2 * M_PI);
    cairo_stroke(cr);

    // ── Current multiplier label — right of the track
    char val_buf[16];
    snprintf(val_buf, sizeof(val_buf), "%.2f×", (double)mult);

    PangoLayout *val_lay = pango_cairo_create_layout(cr);
    pango_layout_set_text(val_lay, val_buf, -1);
    PangoFontDescription *val_font =
        pango_font_description_from_string("Lucida Grande 10");
    pango_layout_set_font_description(val_lay, val_font);
    pango_font_description_free(val_font);
    cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
    cairo_move_to(cr, PANE_LEFT, row_y + ISCALE_LABEL_Y);
    // The readout sits on the label row, right-aligned inside the pane.
    int vw, vh;
    pango_layout_get_pixel_size(val_lay, &vw, &vh);
    cairo_move_to(cr, PANE_RIGHT - vw, row_y + ISCALE_LABEL_Y);
    pango_cairo_show_layout(cr, val_lay);
    g_object_unref(val_lay);
    (void)vh;
}

// Hit-test for the Interface Scale slider. A click anywhere on the track
// band (or its vertical slop) grabs the drag and jumps the knob to the
// pointer's X — matches the live-slider behaviour of macOS controls. The
// caller gets the row index plus the (already clamped / snapped)
// multiplier the caller should apply as the starting drag value.
static bool hit_test_interface_scale(int x, int y, int *row_out,
                                     float *mult_out)
{
    double row_y = ROW_TOP_MARGIN + 20;
    for (int i = 0; i < g_row_count; i++) {
        double track_y = row_y + ISCALE_TRACK_Y;
        if (y >= track_y - ISCALE_HIT_PAD &&
            y <= track_y + ISCALE_HIT_PAD &&
            x >= PANE_LEFT - ISCALE_KNOB_R &&
            x <= PANE_RIGHT + ISCALE_KNOB_R) {
            *row_out  = i;
            *mult_out = iscale_maybe_snap(iscale_x_to_multiplier(x));
            return true;
        }
        row_y += ROW_HEIGHT + ROW_V_GAP;
    }
    return false;
}

// ── Public API — paint, click, motion, release ─────────────────────────

void displays_pane_paint(SysPrefsState *state)
{
    if (!g_subscribed) {
        // Kick off PropertyChangeMask on the root window so subsequent
        // refresh_scales() calls see up-to-date atom values. Safe to call
        // multiple times; only the first does work.
        moonrock_scale_init(state->dpy);
        g_subscribed = true;
    }

    // Re-enumerate and refresh only on pane entry (or after an external
    // refresh trigger). Doing it on every paint would clobber the optimistic
    // click update before MoonRock's atom round-trip lands.
    if (g_needs_refresh) {
        enumerate_outputs(state->dpy, state->root);
        refresh_scales(state->dpy);
        g_needs_refresh = false;
    }

    cairo_t *cr = state->cr;

    // Pane heading
    PangoLayout *head = pango_cairo_create_layout(cr);
    pango_layout_set_text(head, "Displays", -1);
    PangoFontDescription *head_font =
        pango_font_description_from_string("Lucida Grande Bold 16");
    pango_layout_set_font_description(head, head_font);
    pango_font_description_free(head_font);
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_move_to(cr, PANE_LEFT, 48);
    pango_cairo_show_layout(cr, head);
    g_object_unref(head);

    if (g_row_count == 0) {
        PangoLayout *empty = pango_cairo_create_layout(cr);
        pango_layout_set_text(empty,
            "No connected displays were detected.", -1);
        PangoFontDescription *ef =
            pango_font_description_from_string("Lucida Grande 12");
        pango_layout_set_font_description(empty, ef);
        pango_font_description_free(ef);
        cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
        cairo_move_to(cr, PANE_LEFT, ROW_TOP_MARGIN + 30);
        pango_cairo_show_layout(cr, empty);
        g_object_unref(empty);
        return;
    }

    // Rows
    double row_y = ROW_TOP_MARGIN + 20;
    double row_w = PANE_RIGHT - PANE_LEFT;

    for (int i = 0; i < g_row_count; i++) {
        DisplayRow *r = &g_rows[i];

        // Title: "eDP-1 — 1920 × 1200"
        char title[128];
        snprintf(title, sizeof(title), "%s — %d × %d",
                 r->name, r->width, r->height);

        PangoLayout *tl = pango_cairo_create_layout(cr);
        pango_layout_set_text(tl, title, -1);
        PangoFontDescription *tf =
            pango_font_description_from_string("Lucida Grande Bold 13");
        pango_layout_set_font_description(tl, tf);
        pango_font_description_free(tf);
        // Clamp title width so a long "HDMI-1 — 3840 × 2160" can't spill
        // into the right-side primary control's region.
        pango_layout_set_width(tl,
            (PANE_RIGHT - PANE_LEFT - PRIMARY_CTRL_W - 12) * PANGO_SCALE);
        pango_layout_set_ellipsize(tl, PANGO_ELLIPSIZE_END);
        cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
        cairo_move_to(cr, PANE_LEFT, row_y + ROW_TITLE_Y);
        pango_cairo_show_layout(cr, tl);
        g_object_unref(tl);

        // Primary radio + "Primary display" label, right-aligned on the
        // title row.
        draw_primary_radio(cr, row_y + ROW_TITLE_Y, r->is_primary);

        // Meta: "14" panel · 323 PPI" if EDID reports mm, otherwise just
        // the current effective scale so the row is never empty.
        char meta[128];
        if (r->mm_width > 0 && r->mm_height > 0) {
            double diag_sq = (double)r->mm_width * r->mm_width +
                             (double)r->mm_height * r->mm_height;
            double inches = sqrt(diag_sq) / 25.4;
            double ppi = (double)r->width /
                         ((double)r->mm_width / 25.4);
            snprintf(meta, sizeof(meta),
                     "%.1f\" panel · %.0f PPI · active scale %.2f×",
                     inches, ppi, (double)r->current_scale);
        } else {
            snprintf(meta, sizeof(meta),
                     "active scale %.2f×",
                     (double)r->current_scale);
        }

        PangoLayout *ml = pango_cairo_create_layout(cr);
        pango_layout_set_text(ml, meta, -1);
        PangoFontDescription *mf =
            pango_font_description_from_string("Lucida Grande 11");
        pango_layout_set_font_description(ml, mf);
        pango_font_description_free(mf);
        // Clamp so meta text can't bleed into the rotation pill strip.
        pango_layout_set_width(ml,
            (int)((rotation_strip_x0() - PANE_LEFT - 12) * PANGO_SCALE));
        pango_layout_set_ellipsize(ml, PANGO_ELLIPSIZE_END);
        cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
        cairo_move_to(cr, PANE_LEFT, row_y + ROW_META_Y);
        pango_cairo_show_layout(cr, ml);
        g_object_unref(ml);

        // Rotation pills — right-aligned on the meta row. Only offered
        // when the built-in panel is the sole connected output: rotating
        // one output of a multi-display setup implies geometry decisions
        // (break mirror, re-pack positions) that the pane doesn't yet
        // make, and the externals themselves don't physically rotate.
        // "Rotate my tablet when I'm undocked" is the intended UX.
        if (g_row_count == 1) {
            draw_rotation_pills(cr,
                                rotation_strip_x0(),
                                rotation_strip_y(row_y),
                                r->rotation,
                                -1);
        }

        // Pills reflect the BACKING scale (density MoonRock picked from
        // EDID or a user override). Because the published table carries
        // effective = backing × multiplier, divide out the multiplier so
        // the 1× pill stays lit when a user slides Interface Scale to 1.5×
        // but hasn't touched density.
        int active = (r->picked_step >= 0)
                       ? r->picked_step
                       : nearest_step(row_backing_scale(r));
        int hover = (g_hover_row == i) ? g_hover_step : -1;
        draw_pill_strip(cr, i, PANE_LEFT, row_y + PILL_Y,
                        row_w, active, hover);

        // Interface Scale slider sits below the pill strip. Continuous
        // 0.5 – 4.0× knob with canonical tick marks; separate from the
        // density pills above — this is the UI-zoom multiplier, folded
        // into effective_scale downstream.
        draw_interface_scale_slider(cr, i, row_y,
                                    row_display_multiplier(r),
                                    g_iscale_drag_row == i);

        row_y += ROW_HEIGHT + ROW_V_GAP;
    }
}

// Hit-test: which (row, step) does (x, y) fall in? Returns true if the
// point hit any pill; fills *row_out and *step_out.
static bool hit_test(int x, int y, int *row_out, int *step_out)
{
    double row_y = ROW_TOP_MARGIN + 20;
    double row_w = PANE_RIGHT - PANE_LEFT;
    double pill_w = row_w / STEP_COUNT;

    for (int i = 0; i < g_row_count; i++) {
        double py = row_y + PILL_Y;
        if (y >= py && y <= py + PILL_H &&
            x >= PANE_LEFT && x <= PANE_LEFT + row_w) {
            int step = (int)((x - PANE_LEFT) / pill_w);
            if (step < 0) step = 0;
            if (step >= STEP_COUNT) step = STEP_COUNT - 1;
            *row_out = i;
            *step_out = step;
            return true;
        }
        row_y += ROW_HEIGHT + ROW_V_GAP;
    }
    return false;
}

// Hit-test for the rotation pills on the meta row. Returns true on hit and
// fills *row_out + *rot_index_out (0..3 mapping to 0°/90°/180°/270°).
static bool hit_test_rotation(int x, int y, int *row_out, int *rot_index_out)
{
    // Rotation pills are only drawn when the built-in panel is the sole
    // connected output; refuse hits otherwise so an invisible strip can't
    // be clicked by a touch that happens to land on the meta row.
    if (g_row_count != 1) return false;

    double row_y = ROW_TOP_MARGIN + 20;
    const double x0 = rotation_strip_x0();

    for (int i = 0; i < g_row_count; i++) {
        double py = rotation_strip_y(row_y);
        if (y >= py && y <= py + ROT_PILL_H &&
            x >= x0 && x <= PANE_RIGHT) {
            // Quantize to one of the 4 pills. Clicks that fall in the
            // ROT_GAP strips between pills round to the nearer pill by
            // construction (integer division).
            int step = (int)((x - x0) / (ROT_PILL_W + ROT_GAP));
            if (step < 0) step = 0;
            if (step >= ROT_COUNT) step = ROT_COUNT - 1;
            // Check we're actually inside the pill rectangle (not in a
            // gap between pills) — treat gap-clicks as "no hit" so we
            // don't accidentally rotate from a stray click.
            double pill_x = x0 + step * (ROT_PILL_W + ROT_GAP);
            if (x < pill_x || x > pill_x + ROT_PILL_W) return false;
            *row_out = i;
            *rot_index_out = step;
            return true;
        }
        row_y += ROW_HEIGHT + ROW_V_GAP;
    }
    return false;
}

// Hit-test for the primary radio on the title row. Matches the full
// 128px control block (circle + label) so clicks feel forgiving. Returns
// the row index on hit, -1 otherwise.
static int hit_test_primary(int x, int y)
{
    double row_y = ROW_TOP_MARGIN + 20;
    const double ctrl_x0 = PANE_RIGHT - PRIMARY_CTRL_W;
    const double ctrl_x1 = PANE_RIGHT;

    for (int i = 0; i < g_row_count; i++) {
        double ty = row_y + ROW_TITLE_Y;
        double y0 = ty - PRIMARY_HIT_PAD_Y;
        double y1 = ty + PRIMARY_RADIO_D + PRIMARY_HIT_PAD_Y;
        if (y >= y0 && y <= y1 && x >= ctrl_x0 && x <= ctrl_x1) {
            return i;
        }
        row_y += ROW_HEIGHT + ROW_V_GAP;
    }
    return -1;
}

bool displays_pane_click(SysPrefsState *state, int x, int y)
{
    // Interface Scale slider — check first so a click on the slider
    // track isn't shadowed by the pill strip row's vertical padding.
    // Press grabs the drag and snaps the knob to the pointer's X; the
    // ButtonRelease handler commits the final value to MoonRock.
    int sr = -1;
    float smult = 1.0f;
    if (hit_test_interface_scale(x, y, &sr, &smult)) {
        g_iscale_drag_row = sr;
        g_rows[sr].drag_multiplier = smult;
        return true; // repaint to show the knob at the new position
    }

    // Rotation pills — their strip sits on the meta row above the scale
    // strip, so they're checked first so a click on the 0°/90°/180°/270°
    // pill isn't shadowed by the primary radio or scale strip below.
    int rrow = -1, rstep = -1;
    if (hit_test_rotation(x, y, &rrow, &rstep)) {
        DisplayRow *r = &g_rows[rrow];
        int degrees = ROT_DEGREES[rstep];
        if (r->rotation == degrees) {
            // No-op — don't spam MoonRock for a same-value click.
            return false;
        }
        fprintf(stderr,
                "[sysprefs:displays] Requesting %s → %d°\n",
                r->name, degrees);
        if (!moonrock_request_rotation(state->dpy, r->name, degrees)) {
            fprintf(stderr,
                    "[sysprefs:displays] moonrock_request_rotation() failed — "
                    "request dropped\n");
            return false;
        }
        // Optimistic local flip — MoonRock will re-publish the scale
        // table with the new rotation; next pane entry will confirm.
        r->rotation = degrees;
        return true;
    }

    // Primary radio takes priority — it sits on the title row, well
    // separated from the pill strip, but check it first for clarity.
    int prow = hit_test_primary(x, y);
    if (prow >= 0) {
        DisplayRow *r = &g_rows[prow];
        if (r->is_primary) {
            // Already primary — no-op, no atom traffic. Return false so
            // caller doesn't repaint a frame that wouldn't change.
            return false;
        }
        fprintf(stderr,
                "[sysprefs:displays] Requesting primary = %s\n", r->name);
        if (!moonrock_request_primary(state->dpy, r->name)) {
            fprintf(stderr,
                    "[sysprefs:displays] moonrock_request_primary() "
                    "failed — request dropped\n");
            return false;
        }
        // Optimistic local flip — exactly one row primary. MoonRock
        // re-publishes on _MOONROCK_OUTPUT_SCALES; next pane entry will
        // confirm or revert via refresh_scales().
        for (int i = 0; i < g_row_count; i++) {
            g_rows[i].is_primary = (i == prow);
        }
        return true;
    }

    int row, step;
    if (!hit_test(x, y, &row, &step)) return false;

    DisplayRow *r = &g_rows[row];
    float scale = SCALE_STEPS[step];

    fprintf(stderr,
            "[sysprefs:displays] Requesting %s → %.2f×\n",
            r->name, (double)scale);

    if (!moonrock_request_scale(state->dpy, r->name, scale)) {
        fprintf(stderr,
                "[sysprefs:displays] moonrock_request_scale() failed — "
                "request dropped\n");
        return false;
    }

    // Optimistic update: show the new step immediately so the UI feels
    // responsive. MoonRock will re-publish within one event round-trip
    // and the next refresh_scales() will either confirm or revert.
    r->picked_step   = step;
    r->current_scale = scale;
    return true;  // caller will repaint
}

bool displays_pane_motion(SysPrefsState *state, int x, int y)
{
    (void)state;

    // Active Interface Scale drag — update the local knob position on
    // every motion sample. The atom write waits for ButtonRelease so we
    // don't flood MoonRock mid-drag; the knob still glides live because
    // draw_interface_scale_slider reads row_display_multiplier().
    if (g_iscale_drag_row >= 0 && g_iscale_drag_row < g_row_count) {
        float m = iscale_maybe_snap(iscale_x_to_multiplier(x));
        DisplayRow *r = &g_rows[g_iscale_drag_row];
        if (r->drag_multiplier == m) return false;
        r->drag_multiplier = m;
        return true;
    }

    int row = -1, step = -1;
    hit_test(x, y, &row, &step);
    if (row == g_hover_row && step == g_hover_step) return false;
    g_hover_row  = row;
    g_hover_step = step;
    return true;
}

void displays_pane_release(SysPrefsState *state)
{
    // Commit the Interface Scale drag, if one's active. The drag buffer
    // carries the already-snapped value, so we send it verbatim;
    // MoonRock clamps to [0.5, 4.0] on its side as a second defence.
    if (g_iscale_drag_row >= 0 && g_iscale_drag_row < g_row_count) {
        DisplayRow *r = &g_rows[g_iscale_drag_row];
        float m = r->drag_multiplier;

        // Optimistic local state — stash the committed value and clear
        // the drag buffer so row_display_multiplier() stops preferring
        // it. MoonRock's re-publish on the scale table will confirm or
        // (if clamped) correct on the next PropertyNotify.
        r->multiplier      = m;
        r->drag_multiplier = 0.0f;
        int drag_row = g_iscale_drag_row;
        g_iscale_drag_row = -1;

        if (m > 0.0f) {
            fprintf(stderr,
                    "[sysprefs:displays] Requesting %s interface scale "
                    "→ %.2f×\n", r->name, (double)m);
            if (!moonrock_request_interface_scale(state->dpy, r->name, m)) {
                fprintf(stderr,
                        "[sysprefs:displays] "
                        "moonrock_request_interface_scale() failed — "
                        "request dropped\n");
            }
        }
        (void)drag_row;
        return;
    }
}

void displays_pane_mark_dirty(void)
{
    g_needs_refresh = true;
}
