// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// systray.c — System tray rendering (right side of menu bar)
// ============================================================================
//
// Renders the status icons on the right side of the menu bar using REAL
// Snow Leopard MenuExtras icon assets (PNG files converted from the original
// PDF vectors). No Cairo-drawn approximations — every icon is the actual
// asset from Mac OS X 10.6.
//
// Items are drawn right-to-left:
//   [ ... menu items ... ]  [bat] [vol] [wifi] [bt] [Tue 3:58 PM] [🔍]
//
// Data sources:
//   - Clock: standard C time functions
//   - Volume: PulseAudio via `pactl`
//   - Battery: Linux sysfs (/sys/class/power_supply/)
//   - WiFi: static (full signal) — dynamic detection planned
//   - Bluetooth: static (idle) — dynamic detection planned
// ============================================================================

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include "systray.h"
#include "render.h"

// ── Font scaling helper (mirrors the one in render.c) ──────────────
// Builds a Pango font description string scaled to the current menubar size.
static char *systray_scaled_font(const char *base_name, int base_size)
{
    static char buf[128];
    int scaled_size = (int)(base_size * menubar_scale + 0.5);
    if (scaled_size < base_size) scaled_size = base_size;
    snprintf(buf, sizeof(buf), "%s %d", base_name, scaled_size);
    return buf;
}

// ============================================================================
//  Icon assets — loaded once from real Snow Leopard MenuExtras PNGs
// ============================================================================

static cairo_surface_t *airport_icons[5];    // Signal levels 0-4
static cairo_surface_t *volume_icons[4];     // Levels 1-4 (1=mute, 4=full)
static cairo_surface_t *bluetooth_icons[3];  // 0=off, 1=idle, 2=connected
static cairo_surface_t *battery_icons[3];    // 0=charging, 1=charged, 2=empty
static cairo_surface_t *spotlight_icon;

// Spotlight bbox state moved to MenuBarPane (per-pane). The static
// globals here would track only the last-painted pane, so a click on
// a secondary bar would either miss (its bbox was overwritten) or
// fire from the wrong pane's coordinates. systray_paint now writes
// pane->spotlight_hit_*; systray_hit_spotlight reads from the pane
// the click landed on.

// ============================================================================
//  Cached system readings
// ============================================================================

static int  volume_percent   = 50;
static bool volume_muted     = false;
static int  battery_percent  = -1;
static bool battery_present  = false;
static time_t last_volume_poll  = 0;
static time_t last_battery_poll = 0;

// ============================================================================
//  Asset loading helper
// ============================================================================

// Load a PNG from the menubar extras directory. Returns NULL on failure.
static cairo_surface_t *load_extra(const char *home, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path),
             "%s/.local/share/aqua-widgets/menubar/extras/%s", home, name);

    cairo_surface_t *s = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return NULL;
    }
    return s;
}

// ============================================================================
//  Icon rendering helper
// ============================================================================

// Paint an icon surface at (x, y), vertically centered in the menubar.
// Icons are scaled proportionally with the menubar height using cairo
// transforms so they stay crisp at any size. Returns the scaled icon
// width in pixels (for cursor advancement).
static int paint_icon(cairo_t *cr, cairo_surface_t *icon, double x)
{
    if (!icon) return 0;

    int w = cairo_image_surface_get_width(icon);
    int h = cairo_image_surface_get_height(icon);
    // Scale the icon proportionally with the menubar
    double icon_scale = menubar_scale;
    double scaled_w = w * icon_scale;
    double scaled_h = h * icon_scale;
    double y = (MENUBAR_HEIGHT - scaled_h) / 2.0;

    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, icon_scale, icon_scale);
    cairo_set_source_surface(cr, icon, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
    cairo_paint(cr);
    cairo_restore(cr);

    return (int)(scaled_w + 0.5);
}

// ============================================================================
//  System state readers
// ============================================================================

static void read_volume(void)
{
    FILE *fp = popen("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null", "r");
    if (!fp) return;

    char buf[256];
    while (fgets(buf, sizeof(buf), fp)) {
        char *pct = strstr(buf, "%");
        if (pct) {
            char *num_start = pct - 1;
            while (num_start > buf && (num_start[-1] >= '0' && num_start[-1] <= '9'))
                num_start--;
            volume_percent = atoi(num_start);
            if (volume_percent < 0) volume_percent = 0;
            if (volume_percent > 100) volume_percent = 100;
            break;
        }
    }
    pclose(fp);

    fp = popen("pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null", "r");
    if (fp) {
        char buf2[128];
        if (fgets(buf2, sizeof(buf2), fp))
            volume_muted = (strstr(buf2, "yes") != NULL);
        pclose(fp);
    }
}

static void read_battery(void)
{
    const char *paths[] = {
        "/sys/class/power_supply/BAT0/capacity",
        "/sys/class/power_supply/BAT1/capacity",
        NULL
    };

    battery_present = false;
    for (int i = 0; paths[i]; i++) {
        FILE *fp = fopen(paths[i], "r");
        if (fp) {
            char buf[32];
            if (fgets(buf, sizeof(buf), fp)) {
                battery_percent = atoi(buf);
                battery_present = true;
                if (battery_percent < 0) battery_percent = 0;
                if (battery_percent > 100) battery_percent = 100;
            }
            fclose(fp);
            break;
        }
    }
}

// ============================================================================
//  Public API
// ============================================================================

void systray_init(MenuBar *mb)
{
    (void)mb;

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    // Load all real Snow Leopard MenuExtras icons
    fprintf(stderr, "[systray] Loading menu extra icons...\n");

    // WiFi signal levels (0 = no signal, 4 = full)
    airport_icons[0] = load_extra(home, "airport-0.png");
    airport_icons[1] = load_extra(home, "airport-1.png");
    airport_icons[2] = load_extra(home, "airport-2.png");
    airport_icons[3] = load_extra(home, "airport-3.png");
    airport_icons[4] = load_extra(home, "airport-4.png");

    // Volume levels (1 = mute/off, 2 = low, 3 = mid, 4 = full)
    volume_icons[0] = load_extra(home, "volume-1.png");
    volume_icons[1] = load_extra(home, "volume-2.png");
    volume_icons[2] = load_extra(home, "volume-3.png");
    volume_icons[3] = load_extra(home, "volume-4.png");

    // Bluetooth states
    bluetooth_icons[0] = load_extra(home, "bluetooth-off.png");
    bluetooth_icons[1] = load_extra(home, "bluetooth-idle.png");
    bluetooth_icons[2] = load_extra(home, "bluetooth-connected.png");

    // Battery states
    battery_icons[0] = load_extra(home, "battery-charging.png");
    battery_icons[1] = load_extra(home, "battery-charged.png");
    battery_icons[2] = load_extra(home, "battery-empty.png");

    // Spotlight
    spotlight_icon = load_extra(home, "spotlight.png");

    // Count loaded
    int loaded = 0;
    for (int i = 0; i < 5; i++) if (airport_icons[i]) loaded++;
    for (int i = 0; i < 4; i++) if (volume_icons[i]) loaded++;
    for (int i = 0; i < 3; i++) if (bluetooth_icons[i]) loaded++;
    for (int i = 0; i < 3; i++) if (battery_icons[i]) loaded++;
    if (spotlight_icon) loaded++;
    fprintf(stderr, "[systray] Loaded %d/16 menu extra icons\n", loaded);

    // Read initial system state
    read_volume();
    read_battery();
    last_volume_poll = time(NULL);
    last_battery_poll = time(NULL);
}

int systray_paint(MenuBar *mb, MenuBarPane *pane, cairo_t *cr)
{
    (void)mb;

    // Paint right-to-left from the pane's right edge with scaled margin.
    // The spotlight glyph hit bbox is cached per-paint; in Classic mode
    // only pane 0 paints, so the cache maps to that pane. A.2.3 needs
    // per-pane spotlight bboxes so clicks on a secondary bar activate
    // searchsystem there too.
    int cursor = pane->screen_w - S(8);

    // ── Spotlight icon (rightmost) ──────────────────────────────────
    int spot_x = 0, spot_w = 0;
    if (spotlight_icon) {
        int w = cairo_image_surface_get_width(spotlight_icon);
        int h = cairo_image_surface_get_height(spotlight_icon);
        // Spotlight TIF may be larger — scale to S(14) x S(14)
        double target = SF(14.0);
        double scale = target / (w > h ? w : h);
        int scaled_w = (int)(w * scale + 0.5);
        cursor -= scaled_w;
        double sx = cursor + (target - w * scale) / 2.0;
        double sy = (MENUBAR_HEIGHT - h * scale) / 2.0;

        cairo_save(cr);
        cairo_translate(cr, sx, sy);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, spotlight_icon, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
        cairo_paint(cr);
        cairo_restore(cr);

        spot_x = cursor;
        spot_w = scaled_w;
    } else {
        // Fallback: simple magnifying glass with Cairo
        cursor -= S(14);
        double ix = cursor, iy = (MENUBAR_HEIGHT - SF(14.0)) / 2.0;
        cairo_set_source_rgb(cr, 0.29, 0.29, 0.29);
        cairo_set_line_width(cr, SF(1.6));
        cairo_arc(cr, ix + SF(5.5), iy + SF(5.5), SF(4.0), 0, 2 * M_PI);
        cairo_stroke(cr);
        cairo_move_to(cr, ix + SF(8.3), iy + SF(8.3));
        cairo_line_to(cr, ix + SF(13.0), iy + SF(13.0));
        cairo_stroke(cr);

        spot_x = cursor;
        spot_w = S(14);
    }

    // Record a generous hit-rect on this pane: the full menubar height
    // vertically (so flicks near the top/bottom edge still land), and a
    // small horizontal pad so the 14-point glyph feels like a comfortable
    // button target under fingertip / cursor alike. Per-pane, so a click
    // on a secondary bar tests against that pane's own geometry — not a
    // last-painted-pane snapshot in module-static state.
    int pad = S(4);
    pane->spotlight_hit_x0 = spot_x - pad;
    pane->spotlight_hit_x1 = spot_x + spot_w + pad;
    pane->spotlight_hit_y0 = 0;
    pane->spotlight_hit_y1 = MENUBAR_HEIGHT;

    cursor -= S(12); // Gap

    // ── Clock ──────────────────────────────────────────────────────
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char clock_buf[64];
    strftime(clock_buf, sizeof(clock_buf), "%a %-I:%M %p", tm_info);

    double clock_w = render_measure_text(clock_buf, false);
    cursor -= (int)clock_w;
    // Center on the actual measured line height, not a hardcoded font-size
    // constant. At fractional scales the constant under-estimates Pango's
    // real layout height and pushes the text off the bar's midline.
    int clock_y = render_text_center_y(clock_buf, false);
    render_text(cr, clock_buf, cursor, clock_y, false, 0.1, 0.1, 0.1);

    cursor -= S(12); // Gap

    // ── Battery (if present) ───────────────────────────────────────
    // Real SL shows battery icon + percentage text like "(100%)"
    if (battery_present) {
        // Battery percentage text in parentheses like Snow Leopard
        char batt_buf[16];
        snprintf(batt_buf, sizeof(batt_buf), "(%d%%)", battery_percent);

        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, batt_buf, -1);
        PangoFontDescription *desc =
            pango_font_description_from_string(systray_scaled_font("Lucida Grande", 11));
        pango_layout_set_font_description(layout, desc);
        pango_font_description_free(desc);

        int batt_text_w, batt_text_h;
        pango_layout_get_pixel_size(layout, &batt_text_w, &batt_text_h);

        cursor -= batt_text_w;
        cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        // Center on the real 11pt layout height rather than guessing — the
        // battery string uses a smaller size than the rest of the bar, so
        // the centered-y here is independent from the clock's.
        int batt_y = (MENUBAR_HEIGHT - batt_text_h) / 2;
        if (batt_y < 0) batt_y = 0;
        cairo_move_to(cr, cursor, batt_y);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);

        cursor -= S(4); // Small gap between text and icon

        // Choose battery icon based on state
        cairo_surface_t *batt_icon = NULL;
        if (battery_percent >= 95) {
            batt_icon = battery_icons[1]; // charged
        } else if (battery_percent <= 5) {
            batt_icon = battery_icons[2]; // empty
        } else {
            batt_icon = battery_icons[0]; // charging (default frame)
        }

        if (batt_icon) {
            int bw = (int)(cairo_image_surface_get_width(batt_icon) * menubar_scale + 0.5);
            cursor -= bw;
            paint_icon(cr, batt_icon, cursor);
        }

        cursor -= S(12); // Gap
    }

    // ── Volume icon ────────────────────────────────────────────────
    {
        // Choose volume icon based on level
        cairo_surface_t *vol_icon = NULL;
        if (volume_muted || volume_percent == 0) {
            vol_icon = volume_icons[0]; // volume-1: mute/off
        } else if (volume_percent < 34) {
            vol_icon = volume_icons[1]; // volume-2: low
        } else if (volume_percent < 67) {
            vol_icon = volume_icons[2]; // volume-3: medium
        } else {
            vol_icon = volume_icons[3]; // volume-4: full
        }

        if (vol_icon) {
            int vw = (int)(cairo_image_surface_get_width(vol_icon) * menubar_scale + 0.5);
            cursor -= vw;
            paint_icon(cr, vol_icon, cursor);
        } else {
            cursor -= S(17); // Fallback width
        }

        cursor -= S(12); // Gap
    }

    // ── WiFi / AirPort icon ────────────────────────────────────────
    {
        // Full signal for now — dynamic detection later
        cairo_surface_t *wifi_icon = airport_icons[4];
        if (wifi_icon) {
            int ww = (int)(cairo_image_surface_get_width(wifi_icon) * menubar_scale + 0.5);
            cursor -= ww;
            paint_icon(cr, wifi_icon, cursor);
        } else {
            cursor -= S(22); // Fallback width
        }

        cursor -= S(12); // Gap
    }

    // ── Bluetooth icon ─────────────────────────────────────────────
    {
        // Idle state for now — dynamic detection later
        cairo_surface_t *bt_icon = bluetooth_icons[1];
        if (bt_icon) {
            int bw = (int)(cairo_image_surface_get_width(bt_icon) * menubar_scale + 0.5);
            cursor -= bw;
            paint_icon(cr, bt_icon, cursor);
        } else {
            cursor -= S(14); // Fallback width
        }

        cursor -= S(10); // Final gap
    }

    return pane->screen_w - cursor;
}

void systray_update(MenuBar *mb)
{
    (void)mb;
    time_t now = time(NULL);

    if (now - last_volume_poll >= 10) {
        read_volume();
        last_volume_poll = now;
    }

    if (now - last_battery_poll >= 30) {
        read_battery();
        last_battery_poll = now;
    }
}

bool systray_hit_spotlight(const MenuBarPane *pane, int mx, int my)
{
    // Degenerate rect (memset-zero init or no paint yet) reads as
    // false naturally: mx >= 0 && mx < 0 is false for any mx.
    if (!pane) return false;
    if (pane->spotlight_hit_x0 >= pane->spotlight_hit_x1) return false;
    return mx >= pane->spotlight_hit_x0 && mx < pane->spotlight_hit_x1 &&
           my >= pane->spotlight_hit_y0 && my < pane->spotlight_hit_y1;
}

void systray_cleanup(void)
{
    for (int i = 0; i < 5; i++) {
        if (airport_icons[i]) { cairo_surface_destroy(airport_icons[i]); airport_icons[i] = NULL; }
    }
    for (int i = 0; i < 4; i++) {
        if (volume_icons[i]) { cairo_surface_destroy(volume_icons[i]); volume_icons[i] = NULL; }
    }
    for (int i = 0; i < 3; i++) {
        if (bluetooth_icons[i]) { cairo_surface_destroy(bluetooth_icons[i]); bluetooth_icons[i] = NULL; }
    }
    for (int i = 0; i < 3; i++) {
        if (battery_icons[i]) { cairo_surface_destroy(battery_icons[i]); battery_icons[i] = NULL; }
    }
    if (spotlight_icon) { cairo_surface_destroy(spotlight_icon); spotlight_icon = NULL; }
}
