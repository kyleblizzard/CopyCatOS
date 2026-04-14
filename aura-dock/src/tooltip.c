// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// tooltip.c — Dock icon tooltips (help tags)
//
// Shows a small yellow tooltip above a dock icon after the user hovers over
// it for 500ms. This replicates the classic macOS Snow Leopard "help tag"
// behavior — the warm #FFFFCC yellow background with a thin gray border.
//
// Key design decisions:
//   - Override-redirect window: just like the context menu in menu.c, we
//     create a borderless popup that the window manager leaves alone.
//   - No pointer grab: unlike menus, tooltips are passive. They appear and
//     disappear without stealing input focus.
//   - 500ms hover delay: prevents tooltips from flickering on/off as the
//     user quickly moves the mouse across the dock.
//   - Module-static state: only one tooltip can exist at a time, stored in
//     a file-scoped struct (same pattern as menu.c).
//
// How to wire this into dock.c (changes needed in that file):
//
//   In dock_run() MotionNotify handler:
//     int hit = dock_hit_test(state, state->mouse_x, state->mouse_y);
//     tooltip_update_hover(state, hit);
//
//   In dock_run() LeaveNotify handler:
//     tooltip_hide(state);
//
//   In dock_run() ButtonPress handler (before menu/launch logic):
//     tooltip_hide(state);
//
//   In dock_run() timer/select section:
//     If tooltip timer is active, ensure select() timeout <= 100ms so
//     we check the hover timer frequently enough.
//
//   In dock_init():
//     tooltip_init();
//
//   In dock_cleanup():
//     tooltip_cleanup(state);
// ============================================================================

#define _GNU_SOURCE  // Needed for M_PI in math.h under strict C11

#include "tooltip.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>

// ---------------------------------------------------------------------------
// Tooltip appearance constants
//
// These mirror the classic Snow Leopard "help tag" style:
//   - Warm yellow background (#FFFFCC)
//   - Thin gray border (#999999)
//   - Small rounded corners (4px)
//   - Lucida Grande 12pt text in near-black
// ---------------------------------------------------------------------------
#define TOOLTIP_FONT          "Lucida Grande 12"  // Classic macOS system font
#define TOOLTIP_H_PADDING     8      // Horizontal padding on each side of the text
#define TOOLTIP_V_PADDING     4      // Vertical padding above and below the text
#define TOOLTIP_CORNER_RADIUS 4.0    // Rounded corner radius in pixels
#define TOOLTIP_GAP           8      // Pixels of space between tooltip and dock top
#define TOOLTIP_HOVER_DELAY   0.5    // Seconds to wait before showing (500ms)

// Shadow parameters — subtle drop shadow beneath the tooltip
#define TOOLTIP_SHADOW_OFFSET_X  0    // Horizontal shadow offset (centered)
#define TOOLTIP_SHADOW_OFFSET_Y  1    // Shadow falls 1px below the tooltip
#define TOOLTIP_SHADOW_BLUR      2.0  // Approximate blur radius
#define TOOLTIP_SHADOW_ALPHA     0.2  // 20% black

// ---------------------------------------------------------------------------
// Module-static state — only one tooltip at a time
//
// This struct tracks everything about the current tooltip: which icon it
// belongs to, whether we're waiting for the 500ms delay, and the X window
// used to display it.
// ---------------------------------------------------------------------------
static struct {
    Window win;               // The tooltip popup window (None if not created yet)
    cairo_surface_t *surface; // Cairo surface for drawing on the tooltip window
    cairo_t *cr;              // Cairo drawing context
    int target_idx;           // Which dock icon this tooltip is for (-1 = none)
    double hover_start_time;  // Monotonic timestamp when mouse entered the icon
    bool visible;             // True if the tooltip window is currently mapped (shown)
    bool timer_active;        // True if we're counting down the 500ms delay
    int win_w;                // Current tooltip window width
    int win_h;                // Current tooltip window height
} tip = {
    .win = None,
    .target_idx = -1,
    .visible = false,
    .timer_active = false
};

// ---------------------------------------------------------------------------
// Helper: get current monotonic time in seconds.
//
// CLOCK_MONOTONIC is a timer that only goes forward — it isn't affected by
// the user changing their system clock or NTP adjustments. This is the same
// approach used in bounce.c and dock.c for animation timing.
// ---------------------------------------------------------------------------
static double tooltip_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

// ---------------------------------------------------------------------------
// Helper: draw a rounded rectangle path in Cairo.
//
// A rounded rectangle is built from four arcs (one per corner) connected
// by straight lines. We start at the top-left and go clockwise.
// This is the same approach used in menu.c for the context menu background.
// ---------------------------------------------------------------------------
static void tooltip_rounded_rect(cairo_t *cr, double x, double y,
                                  double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);          // Top-right corner
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);       // Bottom-right corner
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);        // Bottom-left corner
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);        // Top-left corner
    cairo_close_path(cr);
}

// ---------------------------------------------------------------------------
// Helper: paint the tooltip contents (background, border, text).
//
// This draws the classic Snow Leopard help tag:
//   1. A subtle drop shadow (slightly offset, semi-transparent)
//   2. Yellow (#FFFFCC) filled rounded rectangle
//   3. Gray (#999999) 1px border stroke
//   4. Near-black text centered inside
// ---------------------------------------------------------------------------
static void tooltip_paint(DockState *state, const char *label)
{
    cairo_t *cr = tip.cr;

    // Clear the entire surface to fully transparent.
    // CAIRO_OPERATOR_SOURCE replaces pixels instead of blending, so this
    // wipes everything clean before we start drawing.
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // --- Drop shadow ---
    // Draw a slightly offset, semi-transparent dark rounded rect behind
    // the tooltip to give it a subtle floating appearance.
    tooltip_rounded_rect(cr,
                         TOOLTIP_SHADOW_OFFSET_X,
                         TOOLTIP_SHADOW_OFFSET_Y,
                         tip.win_w, tip.win_h,
                         TOOLTIP_CORNER_RADIUS);
    cairo_set_source_rgba(cr, 0, 0, 0, TOOLTIP_SHADOW_ALPHA);
    cairo_fill(cr);

    // --- Yellow background ---
    // #FFFFCC = RGB(255, 255, 204) — the classic Apple tooltip yellow
    tooltip_rounded_rect(cr, 0, 0, tip.win_w, tip.win_h, TOOLTIP_CORNER_RADIUS);
    cairo_set_source_rgb(cr, 255.0 / 255.0, 255.0 / 255.0, 204.0 / 255.0);
    cairo_fill_preserve(cr);  // Fill but keep the path for the border stroke

    // --- Gray border ---
    // #999999 = RGB(153, 153, 153) — thin 1px outline
    cairo_set_source_rgb(cr, 153.0 / 255.0, 153.0 / 255.0, 153.0 / 255.0);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    // --- Text label ---
    // Use Pango for text rendering so we get proper font shaping and metrics.
    // Pango is a text layout library used with Cairo — it handles font
    // selection, sizing, and rendering across platforms.
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_from_string(TOOLTIP_FONT);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, label, -1);

    // Get the rendered text dimensions so we can center it in the tooltip
    int text_w, text_h;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);

    // Center the text horizontally and vertically within the tooltip
    double text_x = (tip.win_w - text_w) / 2.0;
    double text_y = (tip.win_h - text_h) / 2.0;

    // Near-black text color: #1A1A1A = RGB(26, 26, 26)
    cairo_set_source_rgb(cr, 26.0 / 255.0, 26.0 / 255.0, 26.0 / 255.0);
    cairo_move_to(cr, text_x, text_y);
    pango_cairo_show_layout(cr, layout);

    // Clean up Pango objects
    pango_font_description_free(font);
    g_object_unref(layout);

    // Tell Cairo to push all pending drawing operations to the X server
    cairo_surface_flush(tip.surface);
}

// ---------------------------------------------------------------------------
// tooltip_init — One-time setup
//
// Currently there's no global state to initialize, but this function exists
// so dock_init() has a clean place to call if we add setup logic later.
// ---------------------------------------------------------------------------
void tooltip_init(void)
{
    tip.win = None;
    tip.target_idx = -1;
    tip.visible = false;
    tip.timer_active = false;
}

// ---------------------------------------------------------------------------
// tooltip_show — Display a tooltip above the specified dock icon
//
// This creates (or repositions) an override-redirect popup window showing
// the app's name. The window is positioned centered above the icon, with
// a small gap between the tooltip and the dock's top edge.
//
// Parameters:
//   state    — the global dock state (for X display, visual, screen info)
//   icon_idx — index into state->items[] for the icon to label
// ---------------------------------------------------------------------------
void tooltip_show(DockState *state, int icon_idx)
{
    // Safety check: make sure the icon index is valid
    if (icon_idx < 0 || icon_idx >= state->item_count) return;

    // Get the app name from the dock item
    const char *label = state->items[icon_idx].name;

    // --- Measure the text to determine tooltip window size ---
    // We need a temporary Cairo surface just for text measurement, because
    // we haven't created the tooltip window yet (or it might be the wrong size).
    cairo_surface_t *tmp_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *tmp_cr = cairo_create(tmp_surf);

    PangoLayout *measure_layout = pango_cairo_create_layout(tmp_cr);
    PangoFontDescription *font = pango_font_description_from_string(TOOLTIP_FONT);
    pango_layout_set_font_description(measure_layout, font);
    pango_layout_set_text(measure_layout, label, -1);

    int text_w, text_h;
    pango_layout_get_pixel_size(measure_layout, &text_w, &text_h);

    pango_font_description_free(font);
    g_object_unref(measure_layout);
    cairo_destroy(tmp_cr);
    cairo_surface_destroy(tmp_surf);

    // Total tooltip size = text size + padding on all sides
    // We add extra width for the shadow offset so it doesn't get clipped
    int tooltip_w = text_w + 2 * TOOLTIP_H_PADDING;
    int tooltip_h = text_h + 2 * TOOLTIP_V_PADDING;

    // --- Calculate position: centered above the icon ---
    // dock_get_icon_center_x gives us the X coordinate of the icon's center
    // relative to the dock window. We convert to screen coordinates by adding
    // the dock window's X position on screen.
    double icon_center_x = dock_get_icon_center_x(state, icon_idx);
    int screen_icon_cx = state->win_x + (int)icon_center_x;

    // Center the tooltip horizontally over the icon
    int tooltip_x = screen_icon_cx - tooltip_w / 2;

    // Place the tooltip above the dock window with a small gap
    int tooltip_y = state->win_y - tooltip_h - TOOLTIP_GAP;

    // --- Clamp to screen edges so the tooltip doesn't go off-screen ---
    if (tooltip_x < 0) {
        tooltip_x = 0;
    }
    if (tooltip_x + tooltip_w > state->screen_w) {
        tooltip_x = state->screen_w - tooltip_w;
    }
    if (tooltip_y < 0) {
        tooltip_y = 0;
    }

    // --- Create or reuse the tooltip window ---
    // If we already have a window from a previous tooltip, destroy it and
    // create a fresh one. This is simpler than resizing/repositioning an
    // existing window and avoids stale surface issues.
    if (tip.win != None) {
        // Clean up the old window's Cairo resources
        if (tip.cr) {
            cairo_destroy(tip.cr);
            tip.cr = NULL;
        }
        if (tip.surface) {
            cairo_surface_destroy(tip.surface);
            tip.surface = NULL;
        }
        XDestroyWindow(state->dpy, tip.win);
        tip.win = None;
    }

    // Store the new dimensions
    tip.win_w = tooltip_w;
    tip.win_h = tooltip_h;

    // Create an override-redirect window.
    // Override-redirect tells the window manager: "don't touch this window."
    // No title bar, no borders, no auto-positioning. This is standard for
    // tooltips, popup menus, and dropdowns.
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;        // Bypass the window manager
    attrs.colormap = state->colormap;      // Use our ARGB colormap for transparency
    attrs.border_pixel = 0;                // No X11 border
    attrs.background_pixel = 0;            // Transparent background

    tip.win = XCreateWindow(
        state->dpy, state->root,
        tooltip_x, tooltip_y, tooltip_w, tooltip_h,
        0,                                 // Border width: none
        32,                                // Color depth: 32-bit ARGB for transparency
        InputOutput,
        state->visual,                     // Use the dock's ARGB visual
        CWOverrideRedirect | CWColormap | CWBorderPixel | CWBackPixel,
        &attrs
    );

    // We don't select any input events on the tooltip window.
    // Tooltips are completely passive — they don't respond to clicks or
    // mouse movement. All input handling stays on the dock window.

    // Create a Cairo surface for drawing on the tooltip window
    tip.surface = cairo_xlib_surface_create(
        state->dpy, tip.win, state->visual, tooltip_w, tooltip_h);
    tip.cr = cairo_create(tip.surface);

    // Show the tooltip window on screen.
    // XMapRaised puts it above all other windows.
    XMapRaised(state->dpy, tip.win);

    // Paint the tooltip contents (background, border, text)
    tooltip_paint(state, label);
    XFlush(state->dpy);

    fprintf(stderr, "[tooltip] Showed tooltip for '%s' at (%d, %d) size %dx%d win=0x%lx\n",
            label, tooltip_x, tooltip_y, tooltip_w, tooltip_h, tip.win);

    // Update tracking state
    tip.target_idx = icon_idx;
    tip.visible = true;
    tip.timer_active = false;  // Timer is done — tooltip is now showing
}

// ---------------------------------------------------------------------------
// tooltip_hide — Remove the tooltip from screen and reset timer state
//
// This unmaps (hides) the tooltip window but doesn't destroy it. The window
// gets destroyed and recreated in tooltip_show() when needed, or in
// tooltip_cleanup() when the dock shuts down.
// ---------------------------------------------------------------------------
void tooltip_hide(DockState *state)
{
    // If the tooltip is currently visible, unmap it (make it invisible)
    if (tip.visible && tip.win != None) {
        XUnmapWindow(state->dpy, tip.win);
        XFlush(state->dpy);
    }

    // Reset all state so we start fresh on the next hover
    tip.visible = false;
    tip.timer_active = false;
    tip.target_idx = -1;
}

// ---------------------------------------------------------------------------
// tooltip_update_hover — Called on every MotionNotify to manage tooltip timing
//
// This is the main logic that decides when to show or hide the tooltip.
// It implements the 500ms hover delay:
//
//   1. Mouse not on any icon (hover_idx == -1): hide the tooltip
//   2. Mouse moved to a different icon: hide existing tooltip, start new timer
//   3. Mouse still on the same icon, timer running: check if 500ms has passed
//   4. Mouse still on the same icon, tooltip already visible: do nothing
//
// Parameters:
//   state         — the global dock state
//   hover_icon_idx — which icon the mouse is over, or -1 if none
// ---------------------------------------------------------------------------
void tooltip_update_hover(DockState *state, int hover_icon_idx)
{
    // --- Case 1: Mouse is not over any icon ---
    // Hide the tooltip and stop any pending timer.
    if (hover_icon_idx == -1) {
        if (tip.visible || tip.timer_active) {
            tooltip_hide(state);
        }
        return;
    }

    // --- Case 2: Mouse moved to a different icon ---
    // Hide the current tooltip (if any) and start a fresh 500ms timer
    // for the new icon.
    if (hover_icon_idx != tip.target_idx) {
        // Hide any existing tooltip first
        if (tip.visible || tip.timer_active) {
            tooltip_hide(state);
        }

        // Start the hover timer for the new icon
        tip.target_idx = hover_icon_idx;
        tip.hover_start_time = tooltip_get_time();
        tip.timer_active = true;
        fprintf(stderr, "[tooltip] Timer started for icon %d '%s'\n",
                hover_icon_idx, state->items[hover_icon_idx].name);
        return;
    }

    // --- Case 3: Still on the same icon, timer is counting down ---
    // Check if 500ms have elapsed since the mouse entered this icon.
    if (tip.timer_active) {
        double elapsed = tooltip_get_time() - tip.hover_start_time;
        if (elapsed >= TOOLTIP_HOVER_DELAY) {
            fprintf(stderr, "[tooltip] Timer fired for icon %d, calling tooltip_show\n",
                    hover_icon_idx);
            // Time's up — show the tooltip!
            tooltip_show(state, hover_icon_idx);
        }
        // If not enough time has passed, just return and wait for the
        // next MotionNotify (or the select() timeout in the event loop).
        return;
    }

    // --- Case 4: Tooltip is already visible for this icon ---
    // Nothing to do. The tooltip stays where it is.
}

// ---------------------------------------------------------------------------
// tooltip_is_visible — Check if a tooltip is currently showing
//
// This can be used by the event loop to know whether a tooltip timer is
// active and the select() timeout should be shortened.
// ---------------------------------------------------------------------------
bool tooltip_is_visible(void)
{
    // Return true if either the tooltip is showing OR the 500ms hover timer
    // is counting down. The event loop uses this to shorten the select()
    // timeout so the timer check fires frequently enough. Without this,
    // select() sleeps for the full 3-second process check interval, and
    // the hover timer never triggers because no MotionNotify events arrive.
    return tip.visible || tip.timer_active;
}

// ---------------------------------------------------------------------------
// tooltip_cleanup — Destroy the tooltip window and free all resources
//
// Called once during dock shutdown to make sure we don't leak X windows
// or Cairo objects.
// ---------------------------------------------------------------------------
void tooltip_cleanup(DockState *state)
{
    // Destroy Cairo drawing resources
    if (tip.cr) {
        cairo_destroy(tip.cr);
        tip.cr = NULL;
    }
    if (tip.surface) {
        cairo_surface_destroy(tip.surface);
        tip.surface = NULL;
    }

    // Destroy the X window if it exists
    if (tip.win != None) {
        XDestroyWindow(state->dpy, tip.win);
        tip.win = None;
    }

    // Reset all state
    tip.visible = false;
    tip.timer_active = false;
    tip.target_idx = -1;
}
