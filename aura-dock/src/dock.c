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

#include "dock.h"
#include "shelf.h"
#include "magnify.h"
#include "bounce.h"
#include "reflect.h"
#include "indicator.h"
#include "launch.h"
#include "menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

// ---------------------------------------------------------------------------
// Hardcoded dock items — same as the Python prototype.
// Each entry: {name, exec_path, icon_name, process_name, separator_after}
// ---------------------------------------------------------------------------
typedef struct {
    const char *name;
    const char *exec_path;
    const char *icon_name;
    const char *process_name;
    bool separator_after;
} DockItemDef;

static const DockItemDef default_items[] = {
    {"Finder",              "dolphin",        "system-file-manager",  "dolphin",        false},
    {"Brave",               "brave-browser",  "brave-browser",        "brave",          false},
    {"Kate",                "kate",           "kate",                 "kate",           false},
    {"Terminal",            "konsole",        "utilities-terminal",   "konsole",        false},
    {"Strawberry",          "strawberry",     "strawberry",           "strawberry",     true },
    {"Krita",               "krita",          "krita",                "krita",          false},
    {"GIMP",                "gimp",           "gimp",                 "gimp",           false},
    {"Inkscape",            "inkscape",       "inkscape",             "inkscape",       false},
    {"Kdenlive",            "kdenlive",       "kdenlive",             "kdenlive",       false},
    {"System Preferences",  "systemsettings", "preferences-system",   "systemsettings", false},
};

#define DEFAULT_ITEM_COUNT (sizeof(default_items) / sizeof(default_items[0]))

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

// ---------------------------------------------------------------------------
// Icon resolution — find an icon file on disk
//
// We search multiple locations in order of preference:
// 1. AquaKDE custom theme (the project's own icon set)
// 2. hicolor theme (the freedesktop standard fallback)
// 3. pixmaps directory (legacy location for app icons)
//
// We prefer larger sizes (128, 64, 48) because we load once and scale
// down for display. This gives us crisp rendering even at magnified sizes.
// ---------------------------------------------------------------------------
static bool resolve_icon_path(const char *icon_name, char *out_path, size_t out_size)
{
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    // Sizes to try, from largest to smallest
    static const int sizes[] = {128, 64, 48};
    static const int size_count = 3;

    char path[1024];

    // Search 1: AquaKDE custom icon theme
    for (int i = 0; i < size_count; i++) {
        snprintf(path, sizeof(path),
                 "%s/.local/share/icons/AquaKDE-icons/%dx%d/apps/%s.png",
                 home, sizes[i], sizes[i], icon_name);
        if (access(path, R_OK) == 0) {
            snprintf(out_path, out_size, "%s", path);
            return true;
        }
    }

    // Search 2: hicolor theme (standard freedesktop location)
    for (int i = 0; i < size_count; i++) {
        snprintf(path, sizeof(path),
                 "/usr/share/icons/hicolor/%dx%d/apps/%s.png",
                 sizes[i], sizes[i], icon_name);
        if (access(path, R_OK) == 0) {
            snprintf(out_path, out_size, "%s", path);
            return true;
        }
    }

    // Search 3: pixmaps directory (legacy fallback)
    snprintf(path, sizeof(path), "/usr/share/pixmaps/%s.png", icon_name);
    if (access(path, R_OK) == 0) {
        snprintf(out_path, out_size, "%s", path);
        return true;
    }

    // Also try .svg -> .png conversion might exist as just the name
    snprintf(path, sizeof(path), "/usr/share/pixmaps/%s.svg", icon_name);
    if (access(path, R_OK) == 0) {
        // Cairo can't load SVG directly without librsvg, so skip for now
    }

    return false;
}

// ---------------------------------------------------------------------------
// Create a fallback icon: a rounded rectangle with a gradient.
// Used when we can't find the real icon file on disk.
// ---------------------------------------------------------------------------
static cairo_surface_t *create_fallback_icon(const char *name)
{
    int size = 128;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                        size, size);
    cairo_t *cr = cairo_create(surf);

    // Draw a rounded rectangle with a blue-to-purple gradient
    double r = 24;  // Corner radius
    double x = 4, y = 4, w = size - 8, h = size - 8;

    // Rounded rectangle path
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);

    // Gradient fill
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, size);
    cairo_pattern_add_color_stop_rgb(grad, 0.0, 0.3, 0.5, 0.9);
    cairo_pattern_add_color_stop_rgb(grad, 1.0, 0.2, 0.2, 0.6);
    cairo_set_source(cr, grad);
    cairo_fill(cr);
    cairo_pattern_destroy(grad);

    // Draw the first letter of the app name in white
    cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 64);

    // Center the letter
    char letter[2] = {name[0], '\0'};
    cairo_text_extents_t ext;
    cairo_text_extents(cr, letter, &ext);
    cairo_move_to(cr, (size - ext.width) / 2 - ext.x_bearing,
                      (size - ext.height) / 2 - ext.y_bearing);
    cairo_show_text(cr, letter);

    cairo_destroy(cr);
    cairo_surface_flush(surf);
    return surf;
}

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

    // --- Initialize dock items from the hardcoded list ---
    state->item_count = (int)DEFAULT_ITEM_COUNT;
    if (state->item_count > MAX_DOCK_ITEMS) {
        state->item_count = MAX_DOCK_ITEMS;
    }

    for (int i = 0; i < state->item_count; i++) {
        DockItem *item = &state->items[i];
        const DockItemDef *def = &default_items[i];

        strncpy(item->name, def->name, sizeof(item->name) - 1);
        strncpy(item->exec_path, def->exec_path, sizeof(item->exec_path) - 1);
        strncpy(item->process_name, def->process_name, sizeof(item->process_name) - 1);
        item->separator_after = def->separator_after;
        item->scale = 1.0;
        item->bounce_offset = 0;
        item->running = false;
        item->bouncing = false;

        // Try to find the icon file on disk
        if (resolve_icon_path(def->icon_name, item->icon_path,
                              sizeof(item->icon_path))) {
            // Load the icon as a Cairo image surface
            item->icon = cairo_image_surface_create_from_png(item->icon_path);
            if (cairo_surface_status(item->icon) != CAIRO_STATUS_SUCCESS) {
                fprintf(stderr, "Warning: Failed to load icon %s\n",
                        item->icon_path);
                cairo_surface_destroy(item->icon);
                item->icon = create_fallback_icon(item->name);
            }
        } else {
            fprintf(stderr, "Warning: Icon not found for '%s', using fallback\n",
                    item->name);
            item->icon = create_fallback_icon(item->name);
        }
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
static int dock_hit_test(DockState *state, int mx, int my)
{
    int total_w = dock_calculate_total_width(state);
    double x = (state->win_w - total_w) / 2.0;

    for (int i = 0; i < state->item_count; i++) {
        double icon_size = BASE_ICON_SIZE * state->items[i].scale;

        // The icon's Y position: bottom-aligned to the shelf top
        double shelf_top = state->win_h - SHELF_HEIGHT;
        double icon_y = shelf_top - icon_size + state->items[i].bounce_offset;

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
    cairo_t *cr = state->cr;

    // --- Clear the entire surface to fully transparent ---
    // CAIRO_OPERATOR_SOURCE replaces pixels instead of blending,
    // so painting with alpha=0 makes everything invisible.
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);

    // Switch back to normal alpha blending for all subsequent drawing
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // --- Draw the glass shelf ---
    int total_w = dock_calculate_total_width(state);
    int shelf_width = total_w + 2 * SHELF_PADDING;
    shelf_draw(state, shelf_width);

    // --- Draw icons, reflections, separators, and indicators ---
    // Start position: center the icon row in the dock window
    double x = (state->win_w - total_w) / 2.0;
    double shelf_top = state->win_h - SHELF_HEIGHT;

    for (int i = 0; i < state->item_count; i++) {
        DockItem *item = &state->items[i];
        double icon_size = BASE_ICON_SIZE * item->scale;

        // Bottom-align icons to the shelf surface, with bounce offset.
        // bounce_offset is negative when the icon is bouncing up.
        double icon_y = shelf_top - icon_size + item->bounce_offset;
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

        // --- Draw the reflection ---
        // Only draw reflections when the icon isn't bouncing (looks weird mid-bounce)
        if (item->icon && !item->bouncing) {
            reflect_draw(cr, item->icon, icon_x, icon_y, icon_size);
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

    // Flush the Cairo surface so X11 sees the updated pixels
    cairo_surface_flush(state->surface);
    XFlush(state->dpy);
}

void dock_run(DockState *state)
{
    // Get the file descriptor for the X11 connection.
    // We'll use select() to wait for either X events or a timer expiration.
    int x_fd = ConnectionNumber(state->dpy);

    while (state->running_loop) {
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
                    dock_paint(state);
                }
                break;

            case MotionNotify:
                // Mouse moved inside the dock — update magnification
                state->mouse_x = ev.xmotion.x;
                state->mouse_y = ev.xmotion.y;
                magnify_update(state);

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
                dock_paint(state);
                break;

            case EnterNotify:
                // Mouse entered the dock window
                state->mouse_in_dock = true;
                state->mouse_x = ev.xcrossing.x;
                state->mouse_y = ev.xcrossing.y;
                magnify_update(state);
                dock_paint(state);
                break;

            case LeaveNotify:
                // Mouse left the dock window — reset all magnification
                state->mouse_in_dock = false;
                magnify_update(state);

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
                dock_paint(state);
                break;

            case ButtonPress:
                if (ev.xbutton.button == 1) {
                    // Left click — launch or activate the clicked app
                    int idx = dock_hit_test(state, ev.xbutton.x, ev.xbutton.y);
                    if (idx >= 0) {
                        launch_app(state, &state->items[idx]);
                        dock_paint(state);
                    }
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
            dock_paint(state);  // Indicators may have changed
        }

        // Advance bounce animations
        if (state->any_bouncing || bounce_update_all(state)) {
            dock_paint(state);
        }

        // --- Wait for next event or timeout ---
        // select() lets us wait for either:
        //   - An X event arriving on the X file descriptor
        //   - A timeout (BOUNCE_FRAME_MS = 16ms for ~60fps animation)
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(x_fd, &fds);

        // If animations are running, use a short timeout for smooth frames.
        // Otherwise, use a longer timeout to save CPU.
        struct timeval tv;
        if (state->any_bouncing) {
            // ~60fps for smooth bounce animation
            tv.tv_sec = 0;
            tv.tv_usec = BOUNCE_FRAME_MS * 1000;
        } else {
            // No animations — check every 500ms (for process monitoring)
            tv.tv_sec = 0;
            tv.tv_usec = 500000;
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

    // Free shelf and indicator assets
    shelf_cleanup(state);
    indicator_cleanup(state);

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
