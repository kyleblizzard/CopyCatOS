// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// menubar.c — Core menu bar lifecycle and event handling
//
// This is the heart of the menu bar. It manages:
//   - The X11 dock-type window pinned to the top of the screen
//   - The main event loop (mouse, expose, property changes)
//   - Layout computation (where each clickable region is)
//   - Coordination between all subsystems
//
// The window uses _NET_WM_WINDOW_TYPE_DOCK so the window manager knows
// to keep it always on top and not give it decorations. The _NET_WM_STRUT
// properties reserve screen space so other windows don't overlap the bar.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/select.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <X11/Xutil.h>

#include "menubar.h"
#include "render.h"
#include "apple.h"
#include "appmenu.h"
#include "systray.h"

// ── Initialization ──────────────────────────────────────────────────

bool menubar_init(MenuBar *mb)
{
    // Zero out the entire struct so all pointers start as NULL
    // and all numbers start as 0.
    memset(mb, 0, sizeof(MenuBar));
    mb->hover_index = -1;
    mb->open_menu   = -1;

    // Connect to the X server. NULL means use the DISPLAY environment
    // variable, which is the standard way to find the X server.
    mb->dpy = XOpenDisplay(NULL);
    if (!mb->dpy) {
        fprintf(stderr, "aura-menubar: cannot open X display\n");
        return false;
    }

    // Get basic screen info — we need the dimensions to make a
    // full-width window and the root window to watch for active
    // window changes.
    mb->screen   = DefaultScreen(mb->dpy);
    mb->root     = RootWindow(mb->dpy, mb->screen);
    mb->screen_w = DisplayWidth(mb->dpy, mb->screen);
    mb->screen_h = DisplayHeight(mb->dpy, mb->screen);

    // ── Intern atoms ────────────────────────────────────────────
    // Atoms are X11's way of naming properties. We look them up once
    // here and reuse the numeric IDs throughout the program.
    mb->atom_net_active_window       = XInternAtom(mb->dpy, "_NET_ACTIVE_WINDOW", False);
    mb->atom_net_wm_window_type      = XInternAtom(mb->dpy, "_NET_WM_WINDOW_TYPE", False);
    mb->atom_net_wm_window_type_dock = XInternAtom(mb->dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    mb->atom_net_wm_strut            = XInternAtom(mb->dpy, "_NET_WM_STRUT", False);
    mb->atom_net_wm_strut_partial    = XInternAtom(mb->dpy, "_NET_WM_STRUT_PARTIAL", False);
    mb->atom_wm_class                = XInternAtom(mb->dpy, "WM_CLASS", False);
    mb->atom_utf8_string             = XInternAtom(mb->dpy, "UTF8_STRING", False);

    // ── Find 32-bit ARGB visual for translucency ─────────────────
    // A 32-bit visual includes an alpha channel, which lets the menubar
    // be slightly transparent so the wallpaper bleeds through — matching
    // the Snow Leopard translucent menubar effect. Without this, the bar
    // is forced to be fully opaque.
    Visual *visual = NULL;
    Colormap colormap = 0;
    int depth = CopyFromParent;
    XVisualInfo tpl;
    tpl.screen = mb->screen;
    tpl.depth = 32;
    tpl.class = TrueColor;
    int n_visuals = 0;
    XVisualInfo *vis_list = XGetVisualInfo(mb->dpy,
        VisualScreenMask | VisualDepthMask | VisualClassMask,
        &tpl, &n_visuals);
    for (int i = 0; i < n_visuals; i++) {
        // Look for a visual with the standard ARGB channel layout
        if (vis_list[i].red_mask == 0x00FF0000 &&
            vis_list[i].green_mask == 0x0000FF00 &&
            vis_list[i].blue_mask == 0x000000FF) {
            visual = vis_list[i].visual;
            depth = 32;
            colormap = XCreateColormap(mb->dpy, mb->root, visual, AllocNone);
            break;
        }
    }
    if (vis_list) XFree(vis_list);

    // If no ARGB visual was found, fall back to the default visual.
    // The bar will be opaque but otherwise functional.
    if (!visual) {
        visual = DefaultVisual(mb->dpy, mb->screen);
        depth = DefaultDepth(mb->dpy, mb->screen);
        colormap = DefaultColormap(mb->dpy, mb->screen);
    }

    // ── Create the menu bar window ──────────────────────────────
    // It spans the full screen width and is MENUBAR_HEIGHT pixels tall,
    // positioned at the very top of the screen. We use the ARGB visual
    // and its matching colormap so Cairo can paint with alpha.
    XSetWindowAttributes attrs;
    attrs.override_redirect = False;  // Let the WM manage us (as a dock)
    attrs.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask
                     | LeaveWindowMask | StructureNotifyMask;
    attrs.background_pixel = 0;       // Transparent black (alpha=0)
    attrs.colormap = colormap;
    attrs.border_pixel = 0;           // Required when using non-default visual

    mb->win = XCreateWindow(
        mb->dpy, mb->root,
        0, 0,                              // Position: top-left corner
        (unsigned int)mb->screen_w,        // Full screen width
        MENUBAR_HEIGHT,                    // 22 pixels tall
        0,                                 // No border
        depth,                             // 32-bit for ARGB translucency
        InputOutput,                       // Normal window (not InputOnly)
        visual,                            // ARGB visual for alpha support
        CWOverrideRedirect | CWEventMask | CWBackPixel | CWColormap | CWBorderPixel,
        &attrs
    );

    // Tell X not to use a background pixmap — we paint every pixel ourselves.
    // Without this, the window manager may fill with a solid color before our
    // paint runs, causing flicker.
    XSetWindowBackgroundPixmap(mb->dpy, mb->win, None);

    // ── Set window type to DOCK ─────────────────────────────────
    // This tells the window manager "I'm a panel/dock, not a regular
    // app window." The WM will keep us on top and skip us in Alt+Tab.
    Atom dock_type = mb->atom_net_wm_window_type_dock;
    XChangeProperty(mb->dpy, mb->win,
                    mb->atom_net_wm_window_type, XA_ATOM,
                    32, PropModeReplace,
                    (unsigned char *)&dock_type, 1);

    // ── Reserve screen space with struts ────────────────────────
    // Struts tell the WM to keep a strip of the screen clear for us.
    // Other windows will maximize below the menu bar, not behind it.

    // _NET_WM_STRUT_PARTIAL has 12 values:
    //   left, right, top, bottom,
    //   left_start_y, left_end_y,
    //   right_start_y, right_end_y,
    //   top_start_x, top_end_x,
    //   bottom_start_x, bottom_end_x
    long strut_partial[12] = {
        0, 0, MENUBAR_HEIGHT, 0,    // Reserve 22px at the top
        0, 0,                        // left start/end (unused)
        0, 0,                        // right start/end (unused)
        0, mb->screen_w,            // top strut spans full width
        0, 0                         // bottom start/end (unused)
    };
    XChangeProperty(mb->dpy, mb->win,
                    mb->atom_net_wm_strut_partial, XA_CARDINAL,
                    32, PropModeReplace,
                    (unsigned char *)strut_partial, 12);

    // _NET_WM_STRUT is the simpler 4-value version. Some older WMs
    // only support this one, so we set both for compatibility.
    long strut[4] = { 0, 0, MENUBAR_HEIGHT, 0 };
    XChangeProperty(mb->dpy, mb->win,
                    mb->atom_net_wm_strut, XA_CARDINAL,
                    32, PropModeReplace,
                    (unsigned char *)strut, 4);

    // ── Watch root window for active window changes ─────────────
    // When the user clicks a different window, the WM updates
    // _NET_ACTIVE_WINDOW on the root. We watch for that so we can
    // update which app name and menus are shown.
    XSelectInput(mb->dpy, mb->root, PropertyChangeMask);

    // ── Map (show) the window ───────────────────────────────────
    XMapWindow(mb->dpy, mb->win);
    XFlush(mb->dpy);

    // ── Compute layout regions ──────────────────────────────────
    // The Apple logo sits at x=10 and is about 24px wide (14px icon + padding).
    mb->apple_x = 0;
    mb->apple_w = 34;  // Clickable region width

    // The app name starts right after the Apple region, with a small gap.
    mb->appname_x = mb->apple_w + 8;
    mb->appname_w = 0;  // Will be computed dynamically based on text width

    // Menu items start after the app name (computed dynamically in paint).
    mb->menus_x = 0;  // Set during paint

    // ── Initialize subsystems ───────────────────────────────────
    render_init(mb);
    apple_init(mb);
    appmenu_init(mb);
    systray_init(mb);

    // ── Set initial state ───────────────────────────────────────
    // Start with "Finder" as the default app, since that's what macOS
    // shows when no other app is focused.
    strncpy(mb->active_app, "Finder", sizeof(mb->active_app) - 1);
    strncpy(mb->active_class, "dolphin", sizeof(mb->active_class) - 1);

    mb->running = true;

    fprintf(stdout, "aura-menubar: initialized (%dx%d screen)\n",
            mb->screen_w, mb->screen_h);

    return true;
}

// ── Event Loop ──────────────────────────────────────────────────────

void menubar_run(MenuBar *mb)
{
    // We need the X11 file descriptor for select(). This lets us wait
    // for X events with a timeout, so we can also update the clock
    // without blocking forever.
    int x11_fd = ConnectionNumber(mb->dpy);

    // Track the last time we checked the clock, so we only repaint
    // when the minute changes.
    time_t last_clock_check = 0;

    // Track when we last polled volume/battery, so we don't do it
    // every frame (that would be wasteful).
    time_t last_systray_update = 0;

    while (mb->running) {
        // ── Handle all pending X events ─────────────────────────
        while (XPending(mb->dpy)) {
            XEvent ev;
            XNextEvent(mb->dpy, &ev);

            switch (ev.type) {
            case Expose:
                // The window (or part of it) needs to be redrawn.
                // We only repaint on the last Expose in a burst
                // (count == 0 means no more Expose events queued).
                if (ev.xexpose.count == 0) {
                    menubar_paint(mb);
                }
                break;

            case MotionNotify: {
                // Mouse moved over the menu bar — figure out which
                // region the pointer is in and update the hover state.
                int mx = ev.xmotion.x;
                int old_hover = mb->hover_index;
                int new_hover = -1;

                // Check if mouse is over the Apple logo region
                if (mx >= mb->apple_x && mx < mb->apple_x + mb->apple_w) {
                    new_hover = 0;
                }
                // Check if mouse is over one of the menu titles.
                // We need to get the current menu list and check each region.
                else {
                    const char **menus;
                    int menu_count;
                    appmenu_get_menus(mb->active_class, &menus, &menu_count);

                    int item_x = mb->menus_x;
                    for (int i = 0; i < menu_count; i++) {
                        double w = render_measure_text(menus[i], false);
                        int item_w = (int)w + 20; // 10px padding each side
                        if (mx >= item_x && mx < item_x + item_w) {
                            // Menu items are indexed starting at 1
                            // (0 is the Apple logo)
                            new_hover = i + 1;
                            break;
                        }
                        item_x += item_w;
                    }
                }

                // Only repaint if the hover state actually changed
                if (new_hover != old_hover) {
                    mb->hover_index = new_hover;

                    // If a menu is already open and we hover a different
                    // title, switch to that menu (menu bar scrubbing).
                    if (mb->open_menu >= 0 && new_hover >= 0 &&
                        new_hover != mb->open_menu) {
                        // Dismiss the old dropdown
                        if (mb->open_menu == 0) {
                            apple_dismiss(mb);
                        } else {
                            appmenu_dismiss(mb);
                        }

                        // Open the new one
                        if (new_hover == 0) {
                            mb->open_menu = 0;
                            apple_show_menu(mb);
                        } else {
                            mb->open_menu = new_hover;
                            // Compute x position for this menu's dropdown
                            const char **menus2;
                            int count2;
                            appmenu_get_menus(mb->active_class, &menus2, &count2);
                            int dx = mb->menus_x;
                            for (int j = 0; j < new_hover - 1 && j < count2; j++) {
                                dx += (int)render_measure_text(menus2[j], false) + 20;
                            }
                            appmenu_show_dropdown(mb, new_hover - 1, dx);
                        }
                    }

                    menubar_paint(mb);
                }
                break;
            }

            case ButtonPress: {
                // Mouse button was clicked on the menu bar.
                int mx = ev.xbutton.x;

                // If a menu is already open, dismiss it first
                if (mb->open_menu >= 0) {
                    if (mb->open_menu == 0) {
                        apple_dismiss(mb);
                    } else {
                        appmenu_dismiss(mb);
                    }
                    mb->open_menu = -1;
                    menubar_paint(mb);
                    break;
                }

                // Check if click is on the Apple logo
                if (mx >= mb->apple_x && mx < mb->apple_x + mb->apple_w) {
                    mb->open_menu = 0;
                    apple_show_menu(mb);
                    menubar_paint(mb);
                    break;
                }

                // Check if click is on a menu title
                const char **menus;
                int menu_count;
                appmenu_get_menus(mb->active_class, &menus, &menu_count);

                int item_x = mb->menus_x;
                for (int i = 0; i < menu_count; i++) {
                    double w = render_measure_text(menus[i], false);
                    int item_w = (int)w + 20;
                    if (mx >= item_x && mx < item_x + item_w) {
                        mb->open_menu = i + 1;
                        appmenu_show_dropdown(mb, i, item_x);
                        menubar_paint(mb);
                        break;
                    }
                    item_x += item_w;
                }
                break;
            }

            case LeaveNotify:
                // Mouse left the menu bar window — clear the hover state,
                // but don't close any open dropdown (user might be moving
                // the mouse down into the dropdown).
                if (mb->hover_index != -1) {
                    mb->hover_index = -1;
                    menubar_paint(mb);
                }
                break;

            case PropertyNotify:
                // A property changed on the root window. We only care about
                // _NET_ACTIVE_WINDOW — it means the user switched to a
                // different application window.
                if (ev.xproperty.atom == mb->atom_net_active_window) {
                    appmenu_update_active(mb);
                    menubar_paint(mb);
                }
                break;

            default:
                break;
            }
        }

        // ── Periodic updates (clock, systray) ───────────────────
        time_t now = time(NULL);

        // Update clock display every second (check if time changed)
        if (now != last_clock_check) {
            last_clock_check = now;
            menubar_paint(mb);
        }

        // Update battery and volume readings every 10 seconds
        if (now - last_systray_update >= 10) {
            last_systray_update = now;
            systray_update(mb);
        }

        // ── Wait for next event or timeout ──────────────────────
        // Use select() with a 500ms timeout so we wake up periodically
        // to check the clock, even if no X events arrive.
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(x11_fd, &fds);

        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 500000;  // 500 milliseconds

        select(x11_fd + 1, &fds, NULL, NULL, &tv);
    }
}

// ── Painting ────────────────────────────────────────────────────────

void menubar_paint(MenuBar *mb)
{
    // Create a Cairo surface that draws directly onto our X window.
    // We use XGetWindowAttributes to get the actual visual assigned to
    // this window (which is our 32-bit ARGB visual), rather than the
    // default screen visual. This is critical for translucency to work.
    XWindowAttributes wa;
    XGetWindowAttributes(mb->dpy, mb->win, &wa);
    cairo_surface_t *surface = cairo_xlib_surface_create(
        mb->dpy, mb->win,
        wa.visual,
        mb->screen_w, MENUBAR_HEIGHT
    );
    cairo_t *cr = cairo_create(surface);

    // Clear to fully transparent before painting. With an ARGB visual,
    // any pixels we don't paint would show stale data. CAIRO_OPERATOR_SOURCE
    // replaces the destination rather than blending, so this wipes the
    // entire surface to transparent black.
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // ── Background ──────────────────────────────────────────────
    render_background(mb, cr);

    // ── Apple logo (far left) ───────────────────────────────────
    apple_paint(mb, cr);

    // ── Bold app name ───────────────────────────────────────────
    // The active application's name is drawn in bold, matching macOS.
    double appname_w = render_text(cr, mb->active_app,
                                   mb->appname_x, 3,
                                   true,          // Bold
                                   0.1, 0.1, 0.1); // Nearly black

    // ── Menu titles ─────────────────────────────────────────────
    // Get the list of menu titles for the active application
    const char **menus;
    int menu_count;
    appmenu_get_menus(mb->active_class, &menus, &menu_count);

    // Menu items start after the bold app name with a gap
    mb->menus_x = mb->appname_x + (int)appname_w + 16;

    int item_x = mb->menus_x;
    for (int i = 0; i < menu_count; i++) {
        double w = render_measure_text(menus[i], false);
        int item_w = (int)w + 20;  // 10px padding on each side

        // Draw hover highlight if this item is hovered or its menu is open
        if (mb->hover_index == i + 1 || mb->open_menu == i + 1) {
            render_hover_highlight(cr, item_x, 1, item_w, MENUBAR_HEIGHT - 2);
        }

        // Draw the menu title text, centered within its padded region
        render_text(cr, menus[i],
                    item_x + 10, 3,  // 10px left padding, 3px from top
                    false,            // Regular weight (not bold)
                    0.1, 0.1, 0.1);  // Nearly black

        item_x += item_w;
    }

    // ── Hover highlight for Apple logo ──────────────────────────
    if (mb->hover_index == 0 || mb->open_menu == 0) {
        render_hover_highlight(cr, mb->apple_x, 1, mb->apple_w, MENUBAR_HEIGHT - 2);
    }

    // ── System tray (right side) ────────────────────────────────
    systray_paint(mb, cr, mb->screen_w);

    // ── Clean up Cairo resources ────────────────────────────────
    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    // Flush all pending X drawing commands to the server
    XFlush(mb->dpy);
}

// ── Shutdown ────────────────────────────────────────────────────────

void menubar_shutdown(MenuBar *mb)
{
    // Shut down subsystems in reverse order of initialization
    systray_cleanup();
    appmenu_cleanup();
    apple_cleanup();
    render_cleanup();

    // Destroy our window and close the X display connection
    if (mb->win) {
        XDestroyWindow(mb->dpy, mb->win);
    }
    if (mb->dpy) {
        XCloseDisplay(mb->dpy);
    }

    fprintf(stdout, "aura-menubar: shut down cleanly\n");
}
