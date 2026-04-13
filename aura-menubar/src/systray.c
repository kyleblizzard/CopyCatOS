// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// systray.c — System tray rendering (right side of menu bar)
//
// The system tray lives on the right side of the menu bar and shows
// status indicators. Items are drawn right-to-left:
//
//   [ ... menu items ... ]  [bat 87%] [vol] [wifi] [bt] [Tue 3:58 PM] [🔍]
//                                                                       ^
//                                                                  rightmost
//
// Each indicator is drawn using Cairo primitives (no icon images needed).
// Data sources:
//   - Clock: standard C time functions
//   - Bluetooth: static icon (visual only, no backend yet)
//   - WiFi: static icon (visual only, no backend yet)
//   - Volume: PulseAudio via `pactl` command
//   - Battery: Linux sysfs (/sys/class/power_supply/)

#define _GNU_SOURCE  // For M_PI in math.h under strict C11

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include "systray.h"
#include "render.h"

// ── Module state ────────────────────────────────────────────────────

// Cached readings so we don't poll the system on every repaint.
static int  volume_percent   = 50;   // Current volume level (0-100)
static bool volume_muted     = false;
static int  battery_percent  = -1;   // -1 means no battery detected
static bool battery_present  = false;
static time_t last_volume_poll = 0;  // When we last read volume
static time_t last_battery_poll = 0; // When we last read battery

// ── Internal: read volume from PulseAudio ───────────────────────────

// Runs `pactl get-sink-volume @DEFAULT_SINK@` and parses the percentage.
// This is a simple but effective way to get the current volume level.
// PulseAudio's `pactl` is available on most Linux desktops.
static void read_volume(void)
{
    FILE *fp = popen("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null", "r");
    if (!fp) return;

    char buf[256];
    while (fgets(buf, sizeof(buf), fp)) {
        // Output looks like: "Volume: front-left: 65536 / 100% / 0.00 dB"
        // We look for the first percentage sign and parse the number before it.
        char *pct = strstr(buf, "%");
        if (pct) {
            // Walk backward from '%' to find the start of the number
            char *num_start = pct - 1;
            while (num_start > buf && (num_start[-1] >= '0' && num_start[-1] <= '9')) {
                num_start--;
            }
            volume_percent = atoi(num_start);

            // Clamp to valid range
            if (volume_percent < 0) volume_percent = 0;
            if (volume_percent > 100) volume_percent = 100;
            break;
        }
    }
    pclose(fp);

    // Check mute status separately
    fp = popen("pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null", "r");
    if (fp) {
        char buf2[128];
        if (fgets(buf2, sizeof(buf2), fp)) {
            volume_muted = (strstr(buf2, "yes") != NULL);
        }
        pclose(fp);
    }
}

// ── Internal: read battery from sysfs ───────────────────────────────

// Linux exposes battery info through /sys/class/power_supply/.
// Common battery names are BAT0 and BAT1.
static void read_battery(void)
{
    // Try BAT0 first, then BAT1
    const char *paths[] = {
        "/sys/class/power_supply/BAT0/capacity",
        "/sys/class/power_supply/BAT1/capacity",
        NULL
    };

    battery_present = false;

    for (int i = 0; paths[i] != NULL; i++) {
        FILE *fp = fopen(paths[i], "r");
        if (fp) {
            char buf[32];
            if (fgets(buf, sizeof(buf), fp)) {
                battery_percent = atoi(buf);
                battery_present = true;

                // Clamp to valid range
                if (battery_percent < 0) battery_percent = 0;
                if (battery_percent > 100) battery_percent = 100;
            }
            fclose(fp);
            break; // Found a battery, stop looking
        }
    }
}

// ── Internal: draw speaker icon ─────────────────────────────────────

// Draws a simple speaker icon using Cairo paths:
//   - A small rectangle for the speaker body
//   - A triangle for the speaker cone
//   - Arc lines for sound waves (0, 1, or 2 arcs based on volume)
static void draw_speaker_icon(cairo_t *cr, double x, double y)
{
    // Icon is drawn at 16x16 pixels, centered vertically in the bar.
    // The speaker body + cone takes the left portion, waves on the right.

    cairo_set_source_rgb(cr, 0.227, 0.227, 0.227); // #3A3A3A
    cairo_set_line_width(cr, 1.5);

    // Speaker body: small rectangle (3x6 pixels)
    cairo_rectangle(cr, x + 1, y + 5, 3, 6);
    cairo_fill(cr);

    // Speaker cone: triangle extending from the body
    cairo_move_to(cr, x + 4, y + 5);
    cairo_line_to(cr, x + 8, y + 1);
    cairo_line_to(cr, x + 8, y + 15);
    cairo_line_to(cr, x + 4, y + 11);
    cairo_close_path(cr);
    cairo_fill(cr);

    // Sound wave arcs — the number of arcs indicates the volume level.
    // Muted: no arcs. Low (<50%): 1 arc. Normal (>=50%): 2 arcs.
    if (!volume_muted && volume_percent > 0) {
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

        // First arc (small, close to speaker) — shown when volume > 0
        if (volume_percent > 0) {
            cairo_arc(cr, x + 9, y + 8, 3.0,
                      -M_PI / 4, M_PI / 4);
            cairo_stroke(cr);
        }

        // Second arc (larger, farther out) — shown when volume >= 50%
        if (volume_percent >= 50) {
            cairo_arc(cr, x + 9, y + 8, 5.5,
                      -M_PI / 4, M_PI / 4);
            cairo_stroke(cr);
        }
    }

    // If muted, draw an X over the speaker
    if (volume_muted) {
        cairo_set_source_rgb(cr, 0.227, 0.227, 0.227);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, x + 10, y + 4);
        cairo_line_to(cr, x + 15, y + 12);
        cairo_stroke(cr);
        cairo_move_to(cr, x + 15, y + 4);
        cairo_line_to(cr, x + 10, y + 12);
        cairo_stroke(cr);
    }
}

// ── Internal: draw battery icon ─────────────────────────────────────

// Draws a battery outline with a fill level proportional to the charge.
// The icon is 20x10 pixels with a small nub on the right end.
static void draw_battery_icon(cairo_t *cr, double x, double y)
{
    double w = 20.0, h = 10.0;
    double radius = 2.0;

    // Battery outline — a rounded rectangle
    cairo_set_source_rgb(cr, 0.227, 0.227, 0.227); // #3A3A3A
    cairo_set_line_width(cr, 1.0);

    // Draw rounded rectangle for the battery body
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - radius, y + radius,     radius, -M_PI / 2, 0);
    cairo_arc(cr, x + w - radius, y + h - radius, radius, 0,          M_PI / 2);
    cairo_arc(cr, x + radius,     y + h - radius, radius, M_PI / 2,   M_PI);
    cairo_arc(cr, x + radius,     y + radius,     radius, M_PI,       3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_stroke(cr);

    // Small nub on the right side of the battery (the positive terminal)
    cairo_rectangle(cr, x + w, y + 3, 2, 4);
    cairo_fill(cr);

    // Fill proportional to charge level
    double fill_w = (w - 4.0) * (battery_percent / 100.0);
    if (fill_w < 0) fill_w = 0;

    // Green for >20% charge, red for <=20% (low battery warning)
    if (battery_percent > 20) {
        cairo_set_source_rgb(cr, 0.2, 0.78, 0.2); // Green
    } else {
        cairo_set_source_rgb(cr, 0.9, 0.2, 0.2); // Red
    }

    // Fill rectangle inside the battery outline (2px inset on each side)
    cairo_rectangle(cr, x + 2, y + 2, fill_w, h - 4);
    cairo_fill(cr);
}

// ── Public API ──────────────────────────────────────────────────────

void systray_init(MenuBar *mb)
{
    (void)mb;

    // Read initial values so the first paint has real data
    read_volume();
    read_battery();

    last_volume_poll = time(NULL);
    last_battery_poll = time(NULL);
}

int systray_paint(MenuBar *mb, cairo_t *cr, int right_edge)
{
    (void)mb; // Used for consistency but not needed for drawing

    // We paint right-to-left, starting from the right edge with
    // an 8px margin.
    int cursor = right_edge - 8;

    // ── Spotlight magnifying glass (rightmost) ──────────────────
    // Real Snow Leopard has a Spotlight search icon at the far right
    // of the menu bar. We draw a simple magnifying glass: a circle
    // (stroked, not filled) for the lens and a diagonal line for
    // the handle. Color is dark grey (#4A4A4A), size ~14x14px.
    {
        double icon_size = 14.0;
        double ix = cursor - icon_size;          // x position
        double iy = (MENUBAR_HEIGHT - icon_size) / 2.0; // vertically centered

        cairo_set_source_rgb(cr, 0.29, 0.29, 0.29); // #4A4A4A
        cairo_set_line_width(cr, 1.6);

        // Lens: a circle in the upper-left area of the 14x14 box
        double lens_cx = ix + 5.5;   // center x of the lens circle
        double lens_cy = iy + 5.5;   // center y of the lens circle
        double lens_r  = 4.0;        // radius of the lens

        cairo_arc(cr, lens_cx, lens_cy, lens_r, 0, 2 * M_PI);
        cairo_stroke(cr);

        // Handle: a diagonal line from the lower-right of the lens
        // outward toward the bottom-right corner of the icon box
        double handle_start_x = lens_cx + lens_r * cos(M_PI / 4);
        double handle_start_y = lens_cy + lens_r * sin(M_PI / 4);
        cairo_move_to(cr, handle_start_x, handle_start_y);
        cairo_line_to(cr, ix + 13.0, iy + 13.0);
        cairo_stroke(cr);

        cursor -= (int)icon_size;
    }

    // Gap between Spotlight icon and clock
    cursor -= 10;

    // ── Clock ───────────────────────────────────────────────────
    // Format the current time as "Tue 3:58 PM"
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char clock_buf[64];
    strftime(clock_buf, sizeof(clock_buf), "%a %-I:%M %p", tm_info);

    // Measure the clock text so we know how far left to place it
    double clock_w = render_measure_text(clock_buf, false);
    cursor -= (int)clock_w;

    // Draw the clock text
    render_text(cr, clock_buf, cursor, 3, false, 0.1, 0.1, 0.1); // #1A1A1A

    // Gap between clock and Bluetooth icon
    cursor -= 14;

    // ── Bluetooth icon ──────────────────────────────────────────
    // The Bluetooth "rune" is a stylized letter B made of angular
    // lines: a vertical stroke with two arrow-like chevrons pointing
    // right (one top, one bottom). This matches the standard Bluetooth
    // logo seen in macOS Snow Leopard's menu bar.
    // Static icon — always shows "connected" for now.
    {
        double icon_w = 10.0;
        double icon_h = 14.0;
        double ix = cursor - icon_w;
        double iy = (MENUBAR_HEIGHT - icon_h) / 2.0;

        cairo_set_source_rgb(cr, 0.29, 0.29, 0.29); // #4A4A4A
        cairo_set_line_width(cr, 1.4);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

        // The Bluetooth symbol is built from a vertical center line
        // with two chevron arrows pointing right.
        //
        // Coordinates (relative to ix, iy in a 10x14 box):
        //   Top of vertical line:    (5, 0)
        //   Bottom of vertical line: (5, 14)
        //   Upper-right point:       (9, 3.5)
        //   Lower-right point:       (9, 10.5)
        //   Upper-left point:        (1, 10.5)  (bottom-left of top chevron)
        //   Lower-left point:        (1, 3.5)   (top-left of bottom chevron)

        double cx = ix + 5.0;  // center x (vertical stroke)

        // Top half: line from bottom-left up to top-center, then
        // diagonally down-right to the upper-right point, then back
        // to the center crossing point.
        cairo_move_to(cr, ix + 1.0, iy + 10.5);   // lower-left
        cairo_line_to(cr, ix + 9.0, iy + 3.5);    // upper-right
        cairo_line_to(cr, cx,       iy + 0.0);     // top of vertical
        cairo_line_to(cr, cx,       iy + 14.0);    // bottom of vertical
        cairo_line_to(cr, ix + 9.0, iy + 10.5);   // lower-right
        cairo_line_to(cr, ix + 1.0, iy + 3.5);    // upper-left
        cairo_stroke(cr);

        cursor -= (int)icon_w;
    }

    // Gap between Bluetooth and WiFi icon
    cursor -= 10;

    // ── WiFi / AirPort icon ─────────────────────────────────────
    // Draws a WiFi signal fan: three concentric quarter-circle arcs
    // radiating upward from a small base dot. This replicates the
    // classic AirPort/WiFi indicator in Snow Leopard's menu bar.
    // Static icon — always shows full signal (3 bars) for now.
    {
        double icon_size = 14.0;
        double ix = cursor - icon_size;
        double iy = (MENUBAR_HEIGHT - icon_size) / 2.0;

        cairo_set_source_rgb(cr, 0.29, 0.29, 0.29); // #4A4A4A
        cairo_set_line_width(cr, 1.5);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

        // The arcs radiate upward from a center point at the
        // bottom-center of the icon box.
        double base_x = ix + icon_size / 2.0;  // horizontal center
        double base_y = iy + icon_size - 1.0;  // near the bottom

        // Small filled dot at the base (the "source" of the signal)
        cairo_arc(cr, base_x, base_y, 1.5, 0, 2 * M_PI);
        cairo_fill(cr);

        // Three concentric arcs (quarter circles opening upward).
        // Each arc spans from roughly 10-o'clock to 2-o'clock
        // (-135 degrees to -45 degrees, i.e. upper-left to upper-right).
        // Smaller radius = closer to the source = innermost bar.
        double radii[] = { 4.0, 7.0, 10.0 };
        int num_bars = 3; // Full signal: all 3 bars filled

        for (int i = 0; i < num_bars; i++) {
            cairo_arc(cr, base_x, base_y, radii[i],
                      -M_PI * 3.0 / 4.0,   // start: -135 degrees
                      -M_PI * 1.0 / 4.0);   // end:   -45 degrees
            cairo_stroke(cr);
        }

        cursor -= (int)icon_size;
    }

    // Gap between WiFi and volume icon
    cursor -= 10;

    // ── Volume icon ─────────────────────────────────────────────
    cursor -= 16; // Icon is 16px wide
    // Center the 16px icon vertically in the 22px bar
    draw_speaker_icon(cr, cursor, (MENUBAR_HEIGHT - 16) / 2.0);

    // Gap between volume and battery
    cursor -= 10;

    // ── Battery (only if hardware exists) ───────────────────────
    if (battery_present) {
        // Draw the battery percentage text first (right of the icon)
        char batt_buf[16];
        snprintf(batt_buf, sizeof(batt_buf), "%d%%", battery_percent);

        // Use a slightly smaller font description for battery percentage.
        // We render it using Pango directly for the 11pt size.
        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, batt_buf, -1);
        PangoFontDescription *desc = pango_font_description_from_string(
            "Lucida Grande 11"
        );
        pango_layout_set_font_description(layout, desc);
        pango_font_description_free(desc);

        int batt_text_w, batt_text_h;
        pango_layout_get_pixel_size(layout, &batt_text_w, &batt_text_h);

        cursor -= batt_text_w;
        cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        cairo_move_to(cr, cursor, 4);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);

        // Small gap between text and icon
        cursor -= 4;

        // Draw the battery icon (20x10px + 2px nub)
        cursor -= 22; // 20px body + 2px nub
        draw_battery_icon(cr, cursor, (MENUBAR_HEIGHT - 10) / 2.0);

        cursor -= 10; // Gap before next item
    }

    // Return the total width consumed by the system tray.
    // The caller can use this to know where available menu space ends.
    return right_edge - cursor;
}

void systray_update(MenuBar *mb)
{
    (void)mb;

    time_t now = time(NULL);

    // Poll volume every 10 seconds
    if (now - last_volume_poll >= 10) {
        read_volume();
        last_volume_poll = now;
    }

    // Poll battery every 30 seconds
    if (now - last_battery_poll >= 30) {
        read_battery();
        last_battery_poll = now;
    }
}

void systray_cleanup(void)
{
    // No dynamic resources to free — all state is in static variables
    // that will be cleaned up when the process exits.
}
