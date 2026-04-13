// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// desktop.c — Core desktop surface manager
//
// This module creates the full-screen desktop window and runs the main
// event loop. It coordinates between the wallpaper, icons, and context
// menu modules.
//
// The window is created with _NET_WM_WINDOW_TYPE_DESKTOP, which tells
// the window manager to:
//   1. Place it at the very bottom of the stacking order
//   2. Not put any frame/decorations on it
//   3. Not include it in Alt+Tab or the taskbar
//
// The event loop uses select() to monitor both the X connection fd
// (for mouse/keyboard/expose events) and the inotify fd (for changes
// to ~/Desktop files).

#define _GNU_SOURCE  // For M_PI in math.h under strict C11

#include "desktop.h"
#include "wallpaper.h"
#include "icons.h"
#include "contextmenu.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>

// ── Forward declarations ────────────────────────────────────────────

static void desktop_repaint(Desktop *d);

// ── Initialization ──────────────────────────────────────────────────

// Try to find a 32-bit ARGB visual on this screen.
// ARGB visuals allow per-pixel alpha transparency, which we may use
// for effects like translucent selections or smooth edges.
// If no ARGB visual is available, we fall back to the default visual.
static Visual *find_argb_visual(Display *dpy, int screen, int *depth_out)
{
    // Ask X for all visuals on this screen
    XVisualInfo tmpl;
    tmpl.screen = screen;
    tmpl.depth = 32;               // We want 32-bit (8 bits each for R, G, B, A)
    tmpl.class = TrueColor;        // Direct color mapping, not palette-based

    int nvisuals = 0;
    XVisualInfo *visuals = XGetVisualInfo(dpy,
        VisualScreenMask | VisualDepthMask | VisualClassMask,
        &tmpl, &nvisuals);

    if (!visuals || nvisuals == 0) {
        return NULL;  // No 32-bit visual found
    }

    // Take the first one that has an alpha mask
    Visual *result = NULL;
    for (int i = 0; i < nvisuals; i++) {
        // A proper ARGB visual has a red mask in the typical position.
        // Check that it looks like a standard ARGB layout.
        if (visuals[i].red_mask   == 0x00FF0000 &&
            visuals[i].green_mask == 0x0000FF00 &&
            visuals[i].blue_mask  == 0x000000FF) {
            result = visuals[i].visual;
            *depth_out = 32;
            break;
        }
    }

    XFree(visuals);
    return result;
}

bool desktop_init(Desktop *d, const char *wallpaper_path)
{
    // Open connection to the X server.
    // NULL means use the DISPLAY environment variable (e.g., ":0").
    d->dpy = XOpenDisplay(NULL);
    if (!d->dpy) {
        fprintf(stderr, "[aura-desktop] Cannot open X display\n");
        return false;
    }

    d->screen = DefaultScreen(d->dpy);
    d->root = RootWindow(d->dpy, d->screen);
    d->width = DisplayWidth(d->dpy, d->screen);
    d->height = DisplayHeight(d->dpy, d->screen);
    d->running = true;

    fprintf(stderr, "[aura-desktop] Screen: %dx%d\n", d->width, d->height);

    // Try to get a 32-bit ARGB visual for transparency effects.
    // If that fails, use the default visual (usually 24-bit RGB).
    d->visual = find_argb_visual(d->dpy, d->screen, &d->depth);
    if (d->visual) {
        fprintf(stderr, "[aura-desktop] Using 32-bit ARGB visual\n");
        // ARGB visuals need their own colormap because they use a
        // different pixel format than the default visual.
        d->colormap = XCreateColormap(d->dpy, d->root, d->visual, AllocNone);
    } else {
        fprintf(stderr, "[aura-desktop] Falling back to default visual\n");
        d->visual = DefaultVisual(d->dpy, d->screen);
        d->depth = DefaultDepth(d->dpy, d->screen);
        d->colormap = DefaultColormap(d->dpy, d->screen);
    }

    // Create the full-screen desktop window.
    // XSetWindowAttributes lets us configure the window before creating it.
    XSetWindowAttributes attrs;
    attrs.colormap = d->colormap;
    attrs.border_pixel = 0;          // No border
    attrs.background_pixel = 0;      // Transparent/black background initially
    attrs.event_mask =
        ExposureMask         |  // Window needs repainting
        ButtonPressMask      |  // Mouse button pressed
        ButtonReleaseMask    |  // Mouse button released
        PointerMotionMask    |  // Mouse moved (for dragging)
        StructureNotifyMask;    // Window resized/moved

    // CWColormap + CWBorderPixel + CWBackPixel + CWEventMask tells X
    // which fields in 'attrs' we actually set
    unsigned long attr_mask = CWColormap | CWBorderPixel | CWBackPixel | CWEventMask;

    d->win = XCreateWindow(d->dpy, d->root,
        0, 0, d->width, d->height,   // Full screen at origin
        0,                             // No border width
        d->depth,                      // Color depth
        InputOutput,                   // We want to draw and receive input
        d->visual,                     // Visual for this window
        attr_mask, &attrs);

    // Set the window type to DESKTOP.
    // This is an EWMH hint that tells the WM to treat this window specially:
    // no frame, always at the bottom, not in the taskbar.
    Atom wm_type = XInternAtom(d->dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom wm_desktop = XInternAtom(d->dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    XChangeProperty(d->dpy, d->win, wm_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&wm_desktop, 1);

    // Give the window a name (visible in xprop/xwininfo for debugging)
    XStoreName(d->dpy, d->win, "AuraOS Desktop");

    // Show the window on screen
    XMapWindow(d->dpy, d->win);

    // Wait for the window to actually appear before painting.
    // The MapNotify event confirms the window is visible.
    XFlush(d->dpy);

    // Initialize the wallpaper (load image, scale to screen)
    if (!wallpaper_init(wallpaper_path, d->width, d->height)) {
        fprintf(stderr, "[aura-desktop] Warning: wallpaper init failed, using fallback\n");
    }

    // Initialize the icon grid (scan ~/Desktop, load icons, set up inotify)
    icons_init(d->dpy, d->width, d->height);

    // Do an initial paint so the desktop isn't blank
    desktop_repaint(d);

    fprintf(stderr, "[aura-desktop] Initialized successfully\n");
    return true;
}

// ── Painting ────────────────────────────────────────────────────────

// Repaint the entire desktop: wallpaper first, then icons on top.
// We create a Cairo context targeting the X window, paint everything,
// then destroy the context.
static void desktop_repaint(Desktop *d)
{
    // Create a Cairo surface that draws directly onto our X window.
    // This is the bridge between Cairo's drawing API and X11.
    cairo_surface_t *xsurf = cairo_xlib_surface_create(
        d->dpy, d->win, d->visual, d->width, d->height);
    cairo_t *cr = cairo_create(xsurf);

    // Layer 1: Wallpaper (fills the entire screen)
    wallpaper_paint(cr, d->width, d->height);

    // Layer 2: Desktop icons (on top of wallpaper)
    icons_paint(cr, d->width, d->height);

    // Clean up the Cairo context (the pixels are already on screen)
    cairo_destroy(cr);
    cairo_surface_destroy(xsurf);

    // Make sure everything is flushed to the X server
    XFlush(d->dpy);
}

// ── Event Loop ──────────────────────────────────────────────────────

// The main event loop uses select() to wait for activity on two file
// descriptors simultaneously:
//   1. The X connection fd — for mouse clicks, expose events, etc.
//   2. The inotify fd — for changes to files in ~/Desktop
//
// This lets us respond to both GUI events and filesystem changes
// without needing threads or busy-waiting.

void desktop_run(Desktop *d)
{
    // Get the file descriptors we need to monitor
    int x_fd = ConnectionNumber(d->dpy);  // X11 connection fd
    int inotify_fd = icons_get_inotify_fd();  // inotify fd (-1 if unavailable)
    int max_fd = x_fd;
    if (inotify_fd > max_fd) max_fd = inotify_fd;

    // State for double-click detection.
    // A double-click is two clicks within 250ms on the same icon.
    DesktopIcon *last_clicked = NULL;       // Icon from the first click
    struct timespec last_click_time = {0};  // Time of the first click

    // State for drag-and-drop
    DesktopIcon *dragging = NULL;   // Icon currently being dragged
    bool drag_active = false;       // True if we're in a drag operation

    while (d->running) {
        // Set up the file descriptor set for select().
        // select() will block until one of these fds has data to read.
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(x_fd, &fds);
        if (inotify_fd >= 0) {
            FD_SET(inotify_fd, &fds);
        }

        // Timeout: wake up every 500ms even if nothing happens.
        // This prevents us from getting stuck if we miss an event.
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 500000;  // 500ms

        int ready = select(max_fd + 1, &fds, NULL, NULL, &timeout);
        if (ready < 0) {
            // select() was interrupted by a signal (e.g., SIGINT)
            continue;
        }

        // Check for inotify events (files changed in ~/Desktop)
        if (inotify_fd >= 0 && FD_ISSET(inotify_fd, &fds)) {
            if (icons_check_inotify()) {
                // Icons were refreshed, repaint everything
                desktop_repaint(d);
            }
        }

        // Process all pending X events.
        // XPending() returns the number of events in the queue.
        while (XPending(d->dpy) > 0) {
            XEvent ev;
            XNextEvent(d->dpy, &ev);

            switch (ev.type) {
            case Expose:
                // The window (or part of it) needs to be repainted.
                // We only repaint once even if multiple Expose events
                // are queued (count == 0 means "last in this batch").
                if (ev.xexpose.count == 0) {
                    desktop_repaint(d);
                }
                break;

            case ButtonPress:
                if (ev.xbutton.button == Button1) {
                    // Left click: check if we hit an icon
                    DesktopIcon *hit = icons_handle_click(
                        ev.xbutton.x, ev.xbutton.y);

                    if (hit) {
                        // Check for double-click: same icon within 250ms
                        struct timespec now;
                        clock_gettime(CLOCK_MONOTONIC, &now);

                        // Calculate time difference in milliseconds
                        long ms = (now.tv_sec - last_click_time.tv_sec) * 1000 +
                                  (now.tv_nsec - last_click_time.tv_nsec) / 1000000;

                        if (hit == last_clicked && ms < 250) {
                            // Double-click detected! Open the file.
                            icons_handle_double_click(hit);
                            last_clicked = NULL;
                        } else {
                            // Single click: select this icon and start
                            // tracking for a potential double-click.
                            icons_select(hit);
                            last_clicked = hit;
                            last_click_time = now;

                            // Start tracking for a potential drag.
                            // We don't actually begin the drag until
                            // the mouse moves (in MotionNotify).
                            dragging = hit;
                            icons_drag_begin(hit, ev.xbutton.x, ev.xbutton.y);
                        }
                    } else {
                        // Clicked on empty space: deselect all icons
                        icons_deselect_all();
                        last_clicked = NULL;
                    }

                    desktop_repaint(d);
                }
                else if (ev.xbutton.button == Button3) {
                    // Right click on empty space: show context menu
                    icons_deselect_all();
                    desktop_repaint(d);

                    // contextmenu_show() runs its own mini event loop
                    // and returns the index of the selected item.
                    int choice = contextmenu_show(d->dpy, d->root,
                        ev.xbutton.x_root, ev.xbutton.y_root,
                        d->width, d->height);

                    // Handle the selected menu action
                    switch (choice) {
                    case 0:  // "New Folder" (menu index 0)
                    {
                        // Create a new folder in ~/Desktop.
                        // If "untitled folder" already exists, append a number.
                        char folder[1024];
                        const char *home = getenv("HOME");
                        snprintf(folder, sizeof(folder),
                                 "%s/Desktop/untitled folder", home);

                        // Check if it already exists and increment
                        int n = 1;
                        while (access(folder, F_OK) == 0) {
                            snprintf(folder, sizeof(folder),
                                     "%s/Desktop/untitled folder %d", home, ++n);
                        }
                        mkdir(folder, 0755);
                        // inotify will pick up the change automatically
                        break;
                    }
                    case 3:  // "Clean Up" (menu index 3)
                        icons_relayout(d->width, d->height);
                        desktop_repaint(d);
                        break;
                    case 5:  // "Change Desktop Background..." (menu index 5)
                        fprintf(stderr, "[aura-desktop] TODO: desktop background picker\n");
                        break;
                    case 7:  // "Open Terminal Here" (menu index 7)
                    {
                        // Fork and exec konsole in ~/Desktop
                        pid_t pid = fork();
                        if (pid == 0) {
                            const char *home = getenv("HOME");
                            char workdir[1024];
                            snprintf(workdir, sizeof(workdir),
                                     "%s/Desktop", home);
                            execlp("konsole", "konsole",
                                   "--workdir", workdir, NULL);
                            _exit(127);  // exec failed
                        }
                        break;
                    }
                    default:
                        break;  // Dismissed or unhandled item
                    }

                    desktop_repaint(d);
                }
                break;

            case MotionNotify:
                // Mouse moved while a button is held down.
                // If we're tracking a potential drag, update the icon position.
                if (dragging && (ev.xmotion.state & Button1Mask)) {
                    drag_active = true;
                    icons_drag_update(ev.xmotion.x, ev.xmotion.y);
                    desktop_repaint(d);
                }
                break;

            case ButtonRelease:
                if (ev.xbutton.button == Button1 && dragging) {
                    if (drag_active) {
                        // End the drag: snap icon to nearest free grid cell
                        icons_drag_end(d->width, d->height);
                        desktop_repaint(d);
                    }
                    dragging = NULL;
                    drag_active = false;
                }
                break;

            default:
                break;
            }
        }
    }
}

// ── Shutdown ────────────────────────────────────────────────────────

void desktop_shutdown(Desktop *d)
{
    icons_shutdown();
    wallpaper_shutdown();

    if (d->win) {
        XDestroyWindow(d->dpy, d->win);
    }

    // Only free the colormap if we created it ourselves (ARGB visual).
    // The default colormap shouldn't be freed.
    if (d->depth == 32) {
        XFreeColormap(d->dpy, d->colormap);
    }

    if (d->dpy) {
        XCloseDisplay(d->dpy);
        d->dpy = NULL;
    }
}
