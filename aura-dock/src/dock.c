// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// dock.c — Core dock state, initialization, event loop, and rendering
//
// This is the heart of the dock. It handles:
//   - Creating the X11 window with 32-bit ARGB transparency
//   - Loading icons from the freedesktop icon theme
//   - Setting _NET_WM_WINDOW_TYPE_DOCK and partial struts so the WM
//     reserves space at the bottom of the screen
//   - The main event loop using select() for both X events and timers
//   - Painting every frame: clear, draw shelf, draw icons with magnification
//     and bounce offsets, draw reflections, draw indicators
//   - Handling mouse motion, clicks, and enter/leave events
// ============================================================================

#define _GNU_SOURCE  // For M_PI in math.h under strict C11

#include "dock.h"
#include "config.h"
#include "shelf.h"
#include "magnify.h"
#include "bounce.h"
#include "reflect.h"
#include "indicator.h"
#include "launch.h"
#include "menu.h"
#include "dnd.h"
#include "poof.h"
#include "tooltip.h"
#include "stacks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

// NOTE: The default dock items list and icon resolution logic have been moved
// to config.c. See config_set_defaults() for the default item list and
// config_resolve_and_load_icon() for the icon loading pipeline.

// ---------------------------------------------------------------------------
// Helper: get current monotonic time in seconds.
// CLOCK_MONOTONIC is a timer that only goes forward — it's not affected by
// the user changing their system clock or NTP adjustments.
// ---------------------------------------------------------------------------
static double get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

// NOTE: resolve_icon_path() and create_fallback_icon() have been moved to
// config.c. They are accessed through config_resolve_and_load_icon() which
// is declared in config.h.

// ---------------------------------------------------------------------------
// Find a 32-bit ARGB visual for transparent windows.
//
// X11 windows are normally opaque. To get real transparency (not just
// pseudo-transparency with the desktop wallpaper), we need a visual that
// has an alpha channel. This is a 32-bit TrueColor visual with depth 32.
// Most modern X servers and compositors support this.
// ---------------------------------------------------------------------------
static Visual *find_argb_visual(Display *dpy, int screen)
{
    XVisualInfo tpl;
    tpl.screen = screen;
    tpl.depth = 32;
    tpl.class = TrueColor;

    int count = 0;
    XVisualInfo *infos = XGetVisualInfo(dpy,
        VisualScreenMask | VisualDepthMask | VisualClassMask,
        &tpl, &count);

    Visual *visual = NULL;
    if (infos && count > 0) {
        visual = infos[0].visual;
    }
    if (infos) XFree(infos);
    return visual;
}

// ---------------------------------------------------------------------------
// Set the _NET_WM_STRUT_PARTIAL property to reserve screen space.
//
// Struts tell the window manager "don't put other windows here." This is
// how panels, taskbars, and docks reserve space. We reserve SHELF_HEIGHT
// pixels at the bottom of the screen so maximized windows don't overlap us.
// ---------------------------------------------------------------------------
static void set_struts(DockState *state)
{
    // _NET_WM_STRUT_PARTIAL has 12 values:
    // [left, right, top, bottom, left_start_y, left_end_y,
    //  right_start_y, right_end_y, top_start_x, top_end_x,
    //  bottom_start_x, bottom_end_x]
    long struts[12] = {0};
    struts[3] = SHELF_HEIGHT;         // bottom strut height
    struts[10] = 0;                    // bottom strut starts at left edge
    struts[11] = state->screen_w;      // bottom strut extends to right edge

    Atom strut_atom = XInternAtom(state->dpy, "_NET_WM_STRUT_PARTIAL", False);
    XChangeProperty(state->dpy, state->win, strut_atom, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)struts, 12);

    // Also set the simpler _NET_WM_STRUT for WMs that don't support PARTIAL
    Atom strut_simple = XInternAtom(state->dpy, "_NET_WM_STRUT", False);
    XChangeProperty(state->dpy, state->win, strut_simple, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)struts, 4);
}

// ---------------------------------------------------------------------------
// Set the window type to _NET_WM_WINDOW_TYPE_DOCK.
//
// This tells the window manager that our window is a dock/panel, not a
// regular application window. The WM will then:
//   - Keep it on all desktops
//   - Not add decorations (title bar, borders)
//   - Keep it above other windows
//   - Not include it in Alt+Tab
// ---------------------------------------------------------------------------
static void set_window_type_dock(DockState *state)
{
    Atom type_atom = XInternAtom(state->dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom dock_atom = XInternAtom(state->dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);

    XChangeProperty(state->dpy, state->win, type_atom, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&dock_atom, 1);
}

bool dock_init(DockState *state)
{
    // Clear the entire state struct to zero/NULL/false
    memset(state, 0, sizeof(DockState));

    // --- Connect to the X server ---
    state->dpy = XOpenDisplay(NULL);
    if (!state->dpy) {
        fprintf(stderr, "Error: Cannot open X display\n");
        return false;
    }

    state->screen = DefaultScreen(state->dpy);
    state->root = RootWindow(state->dpy, state->screen);
    state->screen_w = DisplayWidth(state->dpy, state->screen);
    state->screen_h = DisplayHeight(state->dpy, state->screen);

    // --- Find a 32-bit ARGB visual for transparency ---
    state->visual = find_argb_visual(state->dpy, state->screen);
    if (!state->visual) {
        fprintf(stderr, "Error: No 32-bit ARGB visual found. "
                "Is a compositor running?\n");
        return false;
    }

    state->colormap = XCreateColormap(state->dpy, state->root,
                                       state->visual, AllocNone);

    // --- Load dock items from config file, or use defaults on first launch ---
    // config_load() reads ~/.config/aura-dock/dock.conf. If the file doesn't
    // exist yet (first launch), it returns false and we fall back to the
    // hardcoded default item list via config_set_defaults().
    if (!config_load(state)) {
        config_set_defaults(state);
    }

    // --- Calculate initial dock window size ---
    // Width is based on all icons at base size plus spacing and separators
    state->win_w = dock_calculate_total_width(state) + 2 * SHELF_PADDING;
    state->win_h = DOCK_HEIGHT;
    state->win_x = (state->screen_w - state->win_w) / 2;
    state->win_y = state->screen_h - state->win_h;

    // --- Create the dock window ---
    XSetWindowAttributes attrs;
    attrs.override_redirect = False;  // Let the WM manage positioning
    attrs.colormap = state->colormap;
    attrs.border_pixel = 0;
    attrs.background_pixel = 0;     // Transparent background
    attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                       PointerMotionMask | EnterWindowMask | LeaveWindowMask |
                       StructureNotifyMask;

    state->win = XCreateWindow(
        state->dpy, state->root,
        state->win_x, state->win_y,
        state->win_w, state->win_h,
        0,              // Border width
        32,             // Depth (32-bit ARGB)
        InputOutput,
        state->visual,
        CWOverrideRedirect | CWColormap | CWBorderPixel |
        CWBackPixel | CWEventMask,
        &attrs
    );

    // Explicitly set background to None so X doesn't fill it with garbage.
    // With a 32-bit ARGB visual, background_pixel=0 should be transparent,
    // but some X servers still fill with the default background. Setting
    // BackPixmap to None tells X "don't paint anything — I'll handle it."
    XSetWindowBackgroundPixmap(state->dpy, state->win, None);

    // Set the window title (shows up in window lists and debugging tools)
    XStoreName(state->dpy, state->win, "AuraDock");

    // Tell the WM this is a dock window
    set_window_type_dock(state);

    // Reserve screen space at the bottom
    set_struts(state);

    // --- Create the Cairo drawing surface ---
    state->surface = cairo_xlib_surface_create(
        state->dpy, state->win, state->visual,
        state->win_w, state->win_h);
    state->cr = cairo_create(state->surface);

    // --- Load assets ---
    shelf_load_assets(state);
    indicator_load(state);
    poof_load(state);
    tooltip_init();
    stacks_load_assets(state);

    // --- Show the window ---
    XMapWindow(state->dpy, state->win);
    XFlush(state->dpy);

    // Do an initial check for running processes
    launch_check_running(state->items, state->item_count);
    state->last_process_check = get_time();

    state->running_loop = true;
    return true;
}

int dock_calculate_total_width(DockState *state)
{
    // Add up the width of all icons at their current scale, plus spacing
    double total = 0;
    for (int i = 0; i < state->item_count; i++) {
        total += BASE_ICON_SIZE * state->items[i].scale;
        if (i < state->item_count - 1) {
            total += state->items[i].separator_after ? SEPARATOR_WIDTH : ICON_SPACING;
        }
    }
    return (int)ceil(total);
}

double dock_get_icon_center_x(DockState *state, int index)
{
    // Calculate the X coordinate of a specific icon's center.
    // We need to walk through all previous icons to find where this one starts.
    int total_w = dock_calculate_total_width(state);
    double x = (state->win_w - total_w) / 2.0;

    for (int i = 0; i < index; i++) {
        x += BASE_ICON_SIZE * state->items[i].scale;
        x += state->items[i].separator_after ? SEPARATOR_WIDTH : ICON_SPACING;
    }

    // Return the center of this icon
    return x + (BASE_ICON_SIZE * state->items[index].scale) / 2.0;
}

// ---------------------------------------------------------------------------
// Figure out which dock item the mouse is over (or -1 if none)
// ---------------------------------------------------------------------------
int dock_hit_test(DockState *state, int mx, int my)
{
    int total_w = dock_calculate_total_width(state);
    double x = (state->win_w - total_w) / 2.0;

    for (int i = 0; i < state->item_count; i++) {
        double icon_size = BASE_ICON_SIZE * state->items[i].scale;

        // The icon's Y position: bottom at 10px above the dock window bottom,
        // matching the rendering in dock_paint().
        double icon_bottom = state->win_h - 12;
        double icon_y = icon_bottom - icon_size + state->items[i].bounce_offset;

        if (mx >= x && mx < x + icon_size &&
            my >= icon_y && my < icon_y + icon_size) {
            return i;
        }

        x += icon_size;
        if (i < state->item_count - 1) {
            x += state->items[i].separator_after ? SEPARATOR_WIDTH : ICON_SPACING;
        }
    }

    return -1;
}

void dock_paint(DockState *state)
{
    // --- Double-buffering: paint to an off-screen image first ---
    // Drawing directly to the X window surface causes flicker because each
    // individual draw operation (clear, shelf, icons) becomes visible to the
    // compositor as a separate frame. Instead, we:
    //   1. Create an off-screen image buffer (in CPU memory)
    //   2. Paint everything to that buffer
    //   3. Blit the finished frame to the window in one operation
    // This eliminates flicker completely.
    cairo_surface_t *buffer = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, state->win_w, state->win_h);
    cairo_t *cr = cairo_create(buffer);

    // --- Clear the entire buffer to fully transparent ---
    // CAIRO_OPERATOR_SOURCE replaces pixels instead of blending,
    // so painting with alpha=0 makes everything invisible.
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);

    // Switch back to normal alpha blending for all subsequent drawing
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // --- Draw the glass shelf ---
    // Temporarily swap the state's cairo context so shelf_draw and friends
    // render into our off-screen buffer instead of the window surface.
    cairo_t *orig_cr = state->cr;
    state->cr = cr;

    int total_w = dock_calculate_total_width(state);
    int shelf_width = total_w + 2 * SHELF_PADDING;
    shelf_draw(state, shelf_width);

    // --- Draw reflections, icons, separators, and indicators ---
    //
    // Draw order matters for correct Snow Leopard appearance:
    //   1. Shelf (already drawn above)
    //   2. Reflections — painted ON the shelf surface, behind icons
    //   3. Icons — sit on top of the shelf, partially overlapping it
    //   4. Indicators — running-app dots on the shelf, below icons
    //   5. Separators — divider lines on the shelf
    //
    // We do two passes over the icon list: first reflections, then icons
    // and overlays. This ensures reflections are behind the icons.

    double x = (state->win_w - total_w) / 2.0;

    // --- Pass 1: Draw reflections on the shelf surface ---
    for (int i = 0; i < state->item_count; i++) {
        DockItem *item = &state->items[i];
        double icon_size = BASE_ICON_SIZE * item->scale;

        double icon_bottom = state->win_h - 12;
        double icon_y = icon_bottom - icon_size + item->bounce_offset;
        double icon_x = x;

        // Only draw reflections when the icon isn't bouncing (looks odd mid-bounce)
        if (item->icon && !item->bouncing) {
            reflect_draw(cr, item->icon, icon_x, icon_y, icon_size);
        }

        x += icon_size;
        if (i < state->item_count - 1) {
            x += item->separator_after ? SEPARATOR_WIDTH : ICON_SPACING;
        }
    }

    // --- Pass 2: Draw icons, indicators, and separators on top ---
    x = (state->win_w - total_w) / 2.0;

    for (int i = 0; i < state->item_count; i++) {
        DockItem *item = &state->items[i];
        double icon_size = BASE_ICON_SIZE * item->scale;

        // Icons rest ON the shelf surface. In real Snow Leopard, the
        // icon bottom is about 12px above the dock window's bottom edge,
        // placing icons firmly on the glass with the bottom ~20% overlapping
        // the shelf area. bounce_offset is negative when bouncing up.
        double icon_bottom = state->win_h - 12;  // 12px above dock bottom
        double icon_y = icon_bottom - icon_size + item->bounce_offset;
        double icon_x = x;

        // --- Draw the icon ---
        if (item->icon) {
            cairo_save(cr);

            // Scale the loaded icon (128x128) down to the current display size
            int src_w = cairo_image_surface_get_width(item->icon);
            int src_h = cairo_image_surface_get_height(item->icon);

            cairo_translate(cr, icon_x, icon_y);
            cairo_scale(cr, icon_size / src_w, icon_size / src_h);
            cairo_set_source_surface(cr, item->icon, 0, 0);

            // Use CAIRO_FILTER_BILINEAR for smooth scaling
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);

            cairo_restore(cr);
        }

        // --- Draw running indicator ---
        if (item->running) {
            double center_x = icon_x + icon_size / 2.0;
            indicator_draw(state, center_x);
        }

        // --- Draw separator after this icon if needed ---
        if (item->separator_after && i < state->item_count - 1) {
            double sep_x = x + icon_size + (SEPARATOR_WIDTH - 2) / 2.0;
            shelf_draw_separator(state, sep_x);
        }

        // Advance to the next icon position
        x += icon_size;
        if (i < state->item_count - 1) {
            x += item->separator_after ? SEPARATOR_WIDTH : ICON_SPACING;
        }
    }

    // Restore the original cairo context on the state
    state->cr = orig_cr;

    // --- Blit the finished buffer to the window in one operation ---
    // This is the key to eliminating flicker: the compositor only ever sees
    // one complete frame, never a half-drawn intermediate state.
    cairo_set_operator(orig_cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(orig_cr, buffer, 0, 0);
    cairo_paint(orig_cr);

    // Clean up the off-screen buffer
    cairo_destroy(cr);
    cairo_surface_destroy(buffer);

    // Flush the Cairo surface so X11 sees the updated pixels
    cairo_surface_flush(state->surface);
    XFlush(state->dpy);
}

// Module-level drag-and-drop state (used across event handlers)
static DndState _dnd;

void dock_run(DockState *state)
{
    int x_fd = ConnectionNumber(state->dpy);
    bool needs_repaint = false;

    // Initialize drag-and-drop state
    dnd_init(&_dnd);

    while (state->running_loop) {
        needs_repaint = false;

        // --- Process all pending X events ---
        while (XPending(state->dpy)) {
            XEvent ev;
            XNextEvent(state->dpy, &ev);

            // Let the menu handler try first — it may consume the event
            if (menu_handle_event(state, &ev)) continue;

            switch (ev.type) {
            case Expose:
                // The window needs to be redrawn (e.g., was uncovered)
                if (ev.xexpose.count == 0) {
                    needs_repaint = true;
                }
                break;

            case MotionNotify:
                state->mouse_x = ev.xmotion.x;
                state->mouse_y = ev.xmotion.y;

                // Check if a drag is in progress
                {
                    DndState *dnd = &_dnd;
                    if (dnd_handle_motion(state, dnd, ev.xmotion.x_root, ev.xmotion.y_root)) {
                        // Drag is active — skip magnification
                        needs_repaint = true;
                        break;
                    }
                }

                // Normal mouse motion — update magnification and tooltip
                magnify_update(state);
                {
                    int hit = dock_hit_test(state, state->mouse_x, state->mouse_y);
                    tooltip_update_hover(state, hit);
                }

                // Resize the dock window if magnification changed the total width
                {
                    int new_w = dock_calculate_total_width(state) + 2 * SHELF_PADDING;
                    if (new_w != state->win_w) {
                        state->win_w = new_w;
                        state->win_x = (state->screen_w - new_w) / 2;
                        XMoveResizeWindow(state->dpy, state->win,
                                          state->win_x, state->win_y,
                                          state->win_w, state->win_h);
                        cairo_xlib_surface_set_size(state->surface,
                                                     state->win_w, state->win_h);
                    }
                }
                needs_repaint = true;
                break;

            case EnterNotify:
                // Mouse entered the dock window
                state->mouse_in_dock = true;
                state->mouse_x = ev.xcrossing.x;
                state->mouse_y = ev.xcrossing.y;
                magnify_update(state);
                needs_repaint = true;
                break;

            case LeaveNotify:
                // Mouse left the dock window — reset magnification and hide tooltip
                state->mouse_in_dock = false;
                magnify_update(state);
                tooltip_hide(state);

                // Shrink the dock window back to base width
                {
                    int base_w = dock_calculate_total_width(state) + 2 * SHELF_PADDING;
                    if (base_w != state->win_w) {
                        state->win_w = base_w;
                        state->win_x = (state->screen_w - base_w) / 2;
                        XMoveResizeWindow(state->dpy, state->win,
                                          state->win_x, state->win_y,
                                          state->win_w, state->win_h);
                        cairo_xlib_surface_set_size(state->surface,
                                                     state->win_w, state->win_h);
                    }
                }
                needs_repaint = true;
                break;

            case ButtonPress:
                tooltip_hide(state);  // Dismiss tooltip on any click

                // If stacks popup is open, let it handle events first
                if (stacks_is_open() && stacks_handle_event(state, &ev)) {
                    needs_repaint = true;
                    break;
                }

                if (ev.xbutton.button == 1) {
                    // Left click — check if it's a folder (opens stack popup)
                    int click_idx = dock_hit_test(state, ev.xbutton.x, ev.xbutton.y);
                    if (click_idx >= 0 && state->items[click_idx].is_folder) {
                        double cx = state->win_x + dock_get_icon_center_x(state, click_idx);
                        stacks_show(state, &state->items[click_idx], cx);
                        needs_repaint = true;
                        break;
                    }

                    // Not a folder — start potential drag, or launch on release
                    DndState *dnd = &_dnd;
                    if (!dnd_handle_button_press(state, dnd, ev.xbutton.button,
                                                 ev.xbutton.x_root, ev.xbutton.y_root)) {
                        // Not on an icon — ignore
                    }
                    // Don't launch here — wait for ButtonRelease
                } else if (ev.xbutton.button == 3) {
                    // Right click — show context menu
                    int idx = dock_hit_test(state, ev.xbutton.x, ev.xbutton.y);
                    if (idx >= 0) {
                        // Convert local coords to screen coords for the popup
                        int screen_x = state->win_x + ev.xbutton.x;
                        int screen_y = state->win_y + ev.xbutton.y;
                        menu_show(state, &state->items[idx], screen_x, screen_y);
                    }
                }
                break;

            case ButtonRelease:
                if (ev.xbutton.button == 1) {
                    DndState *dnd = &_dnd;
                    if (dnd_handle_button_release(state, dnd, ev.xbutton.x_root, ev.xbutton.y_root)) {
                        // Drop completed (reorder or removal) — don't launch
                        needs_repaint = true;
                    } else if (dnd->pending) {
                        // Was a normal click (no drag) — launch the app now
                        int idx = dnd->icon_idx;
                        dnd_cleanup(dnd);
                        dnd_init(dnd);
                        if (idx >= 0 && idx < state->item_count) {
                            launch_app(state, &state->items[idx]);
                            needs_repaint = true;
                        }
                    }
                }
                break;

            case ConfigureNotify:
                // Window was resized or moved (by the WM)
                state->win_w = ev.xconfigure.width;
                state->win_h = ev.xconfigure.height;
                state->win_x = ev.xconfigure.x;
                state->win_y = ev.xconfigure.y;
                cairo_xlib_surface_set_size(state->surface,
                                             state->win_w, state->win_h);
                break;

            case DestroyNotify:
                // Our window was destroyed — exit the loop
                state->running_loop = false;
                break;

            default:
                break;
            }
        }

        // --- Timer-based tasks ---
        double now = get_time();

        // Check running processes periodically
        if (now - state->last_process_check >= PROCESS_CHECK_INTERVAL) {
            launch_check_running(state->items, state->item_count);
            state->last_process_check = now;
            needs_repaint = true;  // Indicators may have changed
        }

        // Debug: count running apps so we can verify process detection works
        {
            int n_running = 0;
            for (int i = 0; i < state->item_count; i++) {
                if (state->items[i].running) n_running++;
            }
            if (n_running > 0) {
                // Only print when the count changes to avoid log spam
                static int last_count = -1;
                if (n_running != last_count) {
                    fprintf(stderr, "[aura-dock] %d apps detected as running\n", n_running);
                    last_count = n_running;
                }
            }
        }

        // Check tooltip hover timer — the mouse may be stationary while the
        // 500ms delay counts down. tooltip_update_hover() only runs during
        // MotionNotify events, so we must also call it here on each select()
        // wakeup. We re-hit-test at the last known mouse position to see if
        // the cursor is still on the same icon.
        if (tooltip_is_visible() && state->mouse_in_dock) {
            int hit = dock_hit_test(state, state->mouse_x, state->mouse_y);
            tooltip_update_hover(state, hit);
        }

        // Advance bounce animations — only call when something is bouncing
        if (state->any_bouncing) {
            if (bounce_update_all(state)) {
                needs_repaint = true;
            }
        }

        // Advance poof animation if active
        if (poof_is_active()) {
            poof_update(state);
        }

        // --- Only repaint when something actually changed ---
        if (needs_repaint) {
            dock_paint(state);
        }

        // --- Wait for next event or timeout ---
        // select() lets us wait for either:
        //   - An X event arriving on the X file descriptor
        //   - A timeout expiration
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(x_fd, &fds);

        // If animations are running, use a short timeout for smooth frames.
        // Otherwise, use a long timeout — we only need to wake up for the
        // periodic process check (every 3 seconds). The old 500ms timeout
        // caused ~2 repaints/sec even when idle, wasting CPU cycles.
        struct timeval tv;
        if (state->any_bouncing || poof_is_active()) {
            // ~60fps for smooth animation (bounce or poof)
            tv.tv_sec = 0;
            tv.tv_usec = BOUNCE_FRAME_MS * 1000;
        } else if (tooltip_is_visible() || _dnd.pending) {
            // Responsive timer for tooltip hover delay and drag threshold
            tv.tv_sec = 0;
            tv.tv_usec = 100000;  // 100ms
        } else {
            // No animations — sleep until the next process check.
            // X events will wake us immediately via the file descriptor.
            tv.tv_sec = (int)PROCESS_CHECK_INTERVAL;
            tv.tv_usec = 0;
        }

        select(x_fd + 1, &fds, NULL, NULL, &tv);
    }
}

void dock_cleanup(DockState *state)
{
    // Close the context menu if it's open
    menu_close(state);

    // Free all icon surfaces
    for (int i = 0; i < state->item_count; i++) {
        if (state->items[i].icon) {
            cairo_surface_destroy(state->items[i].icon);
            state->items[i].icon = NULL;
        }
    }

    // Free shelf, indicator, poof, tooltip, and DnD resources
    shelf_cleanup(state);
    indicator_cleanup(state);
    poof_cleanup(state);
    tooltip_cleanup(state);
    stacks_cleanup(state);
    dnd_cleanup(&_dnd);

    // Free Cairo resources
    if (state->cr) {
        cairo_destroy(state->cr);
        state->cr = NULL;
    }
    if (state->surface) {
        cairo_surface_destroy(state->surface);
        state->surface = NULL;
    }

    // Close the X connection (this also destroys the window)
    if (state->dpy) {
        XDestroyWindow(state->dpy, state->win);
        XFreeColormap(state->dpy, state->colormap);
        XCloseDisplay(state->dpy);
        state->dpy = NULL;
    }
}
