// CopyCatOS — by Kyle Blizzard at Blizzard.show

// finder.c — Core Finder window creation, event loop, and painting
//
// This module is the heart of CCFinder. It handles:
//
//   1. Opening the X display and creating a normal (non-override-redirect)
//      window. The window manager (moonrock) will frame it with Snow
//      Leopard title bar chrome automatically.
//
//   2. Setting up Cairo for rendering. All drawing happens through a
//      single Cairo context that covers the entire window.
//
//   3. Running the main event loop — dispatching Expose, ButtonPress,
//      ConfigureNotify, and other events to the appropriate module
//      (toolbar, sidebar, or content).
//
//   4. Coordinating repaints by calling toolbar_paint, sidebar_paint,
//      and content_paint in order.

#define _GNU_SOURCE  // For M_PI and other POSIX extensions

#include "finder.h"
#include "toolbar.h"
#include "sidebar.h"
#include "content.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include <pango/pangocairo.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <sys/statvfs.h>

// ── Helper: Recreate Cairo surface ──────────────────────────────────
//
// Called on init and whenever the window resizes (ConfigureNotify).
// We destroy the old surface/context and create a new one matching
// the current window dimensions.

static void recreate_cairo(FinderState *fs)
{
    // Destroy existing Cairo resources if they exist
    if (fs->cr) {
        cairo_destroy(fs->cr);
        fs->cr = NULL;
    }
    if (fs->surface) {
        cairo_surface_destroy(fs->surface);
        fs->surface = NULL;
    }

    // Create a new Cairo surface that draws directly onto the X window.
    // This is the bridge between Cairo's 2D API and the X11 window system.
    fs->surface = cairo_xlib_surface_create(
        fs->dpy, fs->win, fs->visual, fs->win_w, fs->win_h);

    fs->cr = cairo_create(fs->surface);
}

// ── Helper: Build the window title ──────────────────────────────────
//
// Snow Leopard Finder titles look like "Home — Finder" or
// "Documents — Finder". We extract the last component of the path
// and append " — Finder".

static void update_window_title(FinderState *fs)
{
    // Find the last component of the path (e.g., "Documents" from
    // "/home/user/Documents"). If the path is "/", use "Macintosh HD".
    const char *name = NULL;

    if (strcmp(fs->path, "/") == 0) {
        name = "Macintosh HD";
    } else {
        // Find the last '/' and take what comes after it
        name = strrchr(fs->path, '/');
        if (name && name[1] != '\0') {
            name++;  // Skip the '/'
        } else {
            name = fs->path;
        }

        // Special case: if navigating to $HOME, show "Home"
        const char *home = getenv("HOME");
        if (home && strcmp(fs->path, home) == 0) {
            name = "Home";
        }
    }

    // Build the title string: "Name — Finder"
    char title[512];
    snprintf(title, sizeof(title), "%s — Finder", name);

    // Set the X11 window title. The WM reads this and displays it
    // in the title bar chrome.
    XStoreName(fs->dpy, fs->win, title);

    // Also set _NET_WM_NAME for modern WM compatibility (UTF-8 titles).
    Atom net_wm_name = XInternAtom(fs->dpy, "_NET_WM_NAME", False);
    Atom utf8_string = XInternAtom(fs->dpy, "UTF8_STRING", False);
    XChangeProperty(fs->dpy, fs->win, net_wm_name, utf8_string, 8,
                    PropModeReplace, (unsigned char *)title, strlen(title));
}

// ── Initialization ──────────────────────────────────────────────────

bool finder_init(FinderState *fs, const char *initial_path)
{
    // Zero out everything to start clean
    memset(fs, 0, sizeof(FinderState));

    // Set layout constants — stored in the struct so every module
    // can access them via the FinderState pointer
    fs->toolbar_h   = FINDER_TOOLBAR_H;
    fs->sidebar_w   = FINDER_SIDEBAR_W;
    fs->statusbar_h = FINDER_STATUSBAR_H;
    fs->pathbar_h   = FINDER_PATHBAR_H;

    // Set the initial path. Default to $HOME if nothing specified.
    if (initial_path) {
        strncpy(fs->path, initial_path, sizeof(fs->path) - 1);
    } else {
        const char *home = getenv("HOME");
        if (home) {
            strncpy(fs->path, home, sizeof(fs->path) - 1);
        } else {
            strncpy(fs->path, "/", sizeof(fs->path) - 1);
        }
    }

    // Open the X display. NULL means use $DISPLAY env variable,
    // which is usually ":0" for a local X server.
    fs->dpy = XOpenDisplay(NULL);
    if (!fs->dpy) {
        fprintf(stderr, "[finder] Cannot open X display\n");
        return false;
    }

    fs->screen = DefaultScreen(fs->dpy);
    fs->visual = DefaultVisual(fs->dpy, fs->screen);
    fs->depth  = DefaultDepth(fs->dpy, fs->screen);

    // Window dimensions
    fs->win_w = FINDER_DEFAULT_W;
    fs->win_h = FINDER_DEFAULT_H;

    // Create the window. We use a normal window (NOT override-redirect)
    // because we want the WM to frame it with Snow Leopard title bar
    // chrome. The WM detects our WM_CLASS and adds the brushed-metal
    // title bar, close/minimize/zoom buttons, etc.
    XSetWindowAttributes attrs;
    attrs.background_pixel = WhitePixel(fs->dpy, fs->screen);
    attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                       PointerMotionMask | KeyPressMask | StructureNotifyMask;

    fs->win = XCreateWindow(
        fs->dpy,
        RootWindow(fs->dpy, fs->screen),
        100, 100,                          // Initial position (WM may override)
        fs->win_w, fs->win_h,             // Size
        0,                                  // Border width (WM adds its own)
        fs->depth,
        InputOutput,
        fs->visual,
        CWBackPixel | CWEventMask,
        &attrs
    );

    // Set WM_CLASS so the window manager and taskbar can identify us.
    // instance = "finder", class = "Finder"
    XClassHint class_hint;
    class_hint.res_name  = "finder";
    class_hint.res_class = "Finder";
    XSetClassHint(fs->dpy, fs->win, &class_hint);

    // Set the window title (e.g., "Home — Finder")
    update_window_title(fs);

    // Set WM_DELETE_WINDOW so we get a ClientMessage when the user
    // clicks the close button, instead of the WM just killing us.
    Atom wm_delete = XInternAtom(fs->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(fs->dpy, fs->win, &wm_delete, 1);

    // Set size hints so the WM knows our preferred and minimum size
    XSizeHints *size_hints = XAllocSizeHints();
    size_hints->flags = PMinSize | PBaseSize;
    size_hints->min_width  = 400;   // Minimum width to keep layout usable
    size_hints->min_height = 300;   // Minimum height
    size_hints->base_width  = FINDER_DEFAULT_W;
    size_hints->base_height = FINDER_DEFAULT_H;
    XSetWMNormalHints(fs->dpy, fs->win, size_hints);
    XFree(size_hints);

    // Map (show) the window
    XMapWindow(fs->dpy, fs->win);

    // Create the Cairo rendering surface
    recreate_cairo(fs);

    // Initialize sub-modules
    sidebar_init();
    content_scan(fs->path);

    fs->running = true;

    fprintf(stderr, "[finder] Initialized: %dx%d, path=%s\n",
            fs->win_w, fs->win_h, fs->path);

    return true;
}

// ── Status Bar Painting ─────────────────────────────────────────────
//
// The status bar sits between the content area and the path bar at the
// bottom of the window. It shows the item count and available disk
// space, matching Snow Leopard Finder's "X items, Y GB available" text.

static void statusbar_paint(FinderState *fs)
{
    cairo_t *cr = fs->cr;
    int win_w = fs->win_w;
    int sidebar_w = fs->sidebar_w;
    int content_w = win_w - sidebar_w;

    // The status bar sits directly below the content area.
    // content_h = total height minus toolbar, status bar, and path bar.
    int content_h = fs->win_h - fs->toolbar_h - fs->statusbar_h - fs->pathbar_h;
    int status_y = fs->toolbar_h + content_h;

    // ── Light grey background (#ECECEC) ─────────────────────────
    cairo_set_source_rgb(cr, 236 / 255.0, 236 / 255.0, 236 / 255.0);
    cairo_rectangle(cr, sidebar_w, status_y, content_w, fs->statusbar_h);
    cairo_fill(cr);

    // ── 1px top border to separate from content ─────────────────
    cairo_set_source_rgb(cr, 180 / 255.0, 180 / 255.0, 180 / 255.0);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, sidebar_w, status_y + 0.5);
    cairo_line_to(cr, win_w, status_y + 0.5);
    cairo_stroke(cr);

    // ── Build status text: "X items, Y GB available" ────────────
    //
    // Get the number of items from the content module and the
    // available disk space from statvfs on the current path.
    int item_count = content_get_count();
    char status_text[128];

    struct statvfs vfs;
    if (statvfs(fs->path, &vfs) == 0) {
        // Calculate available space in GB (using f_bavail for non-root)
        double avail_gb = (double)vfs.f_bavail * vfs.f_frsize / (1024.0 * 1024.0 * 1024.0);
        if (avail_gb >= 1.0) {
            snprintf(status_text, sizeof(status_text),
                     "%d items, %.1f GB available", item_count, avail_gb);
        } else {
            // Show MB if less than 1 GB
            double avail_mb = (double)vfs.f_bavail * vfs.f_frsize / (1024.0 * 1024.0);
            snprintf(status_text, sizeof(status_text),
                     "%d items, %.0f MB available", item_count, avail_mb);
        }
    } else {
        snprintf(status_text, sizeof(status_text), "%d items", item_count);
    }

    // ── Draw the status text centered in the status bar ─────────
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, status_text, -1);

    PangoFontDescription *font = pango_font_description_from_string(
        "Lucida Grande 11");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    int text_w, text_h;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);

    // Center the text horizontally within the content area, vertically in the bar
    int text_x = sidebar_w + (content_w - text_w) / 2;
    int text_y = status_y + (fs->statusbar_h - text_h) / 2;

    cairo_move_to(cr, text_x, text_y);
    cairo_set_source_rgb(cr, 0x66 / 255.0, 0x66 / 255.0, 0x66 / 255.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

// ── Path Bar Painting ───────────────────────────────────────────────
//
// The path bar is the bottommost strip of the Finder window. It shows
// a breadcrumb trail of the current directory path, with each component
// rendered as a clickable-looking pill. A ">" separator sits between
// components. Matches the real Snow Leopard Finder path bar appearance.

static void pathbar_paint(FinderState *fs)
{
    cairo_t *cr = fs->cr;
    int win_w = fs->win_w;

    // The path bar sits at the very bottom of the window, spanning full width
    int path_y = fs->win_h - fs->pathbar_h;

    // ── Gradient background (light grey, like the toolbar) ──────
    cairo_pattern_t *path_grad = cairo_pattern_create_linear(
        0, path_y, 0, path_y + fs->pathbar_h);
    cairo_pattern_add_color_stop_rgb(path_grad, 0.0,
        210 / 255.0, 210 / 255.0, 210 / 255.0);
    cairo_pattern_add_color_stop_rgb(path_grad, 1.0,
        190 / 255.0, 190 / 255.0, 190 / 255.0);
    cairo_set_source(cr, path_grad);
    cairo_rectangle(cr, 0, path_y, win_w, fs->pathbar_h);
    cairo_fill(cr);
    cairo_pattern_destroy(path_grad);

    // ── 1px top border ──────────────────────────────────────────
    cairo_set_source_rgb(cr, 165 / 255.0, 165 / 255.0, 165 / 255.0);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, path_y + 0.5);
    cairo_line_to(cr, win_w, path_y + 0.5);
    cairo_stroke(cr);

    // ── Split the current path into components ──────────────────
    //
    // For path "/home/user/Documents", we produce:
    //   ["Macintosh HD", "home", "user", "Documents"]
    //
    // The root "/" is displayed as "Macintosh HD" to match the Mac style.
    // The user's home directory component is displayed as "Home".
    char path_copy[1024];
    strncpy(path_copy, fs->path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    // Collect path components into an array
    const char *components[64];
    int comp_count = 0;

    // Always start with "Macintosh HD" representing the root
    components[comp_count++] = "Macintosh HD";

    // Tokenize the rest of the path (skip empty tokens from leading '/')
    if (strcmp(path_copy, "/") != 0) {
        char *token = strtok(path_copy, "/");
        while (token && comp_count < 64) {
            components[comp_count++] = token;
            token = strtok(NULL, "/");
        }
    }

    // Check if the user's home directory is in the path so we can
    // replace that component with "Home" for friendlier display.
    const char *home = getenv("HOME");
    char home_basename[256] = "";
    if (home) {
        const char *hb = strrchr(home, '/');
        if (hb && hb[1]) {
            strncpy(home_basename, hb + 1, sizeof(home_basename) - 1);
        }
    }

    // ── Draw each component as a pill with ">" separators ───────
    int draw_x = 12;  // Starting X offset (left padding)
    int text_y_offset = (fs->pathbar_h - 12) / 2;  // Vertically center ~12px text

    for (int i = 0; i < comp_count; i++) {
        // Determine display name — replace home dir username with "Home"
        const char *display = components[i];
        if (i > 0 && home_basename[0] && strcmp(display, home_basename) == 0) {
            display = "Home";
        }

        // Draw the ">" separator before each component except the first
        if (i > 0) {
            PangoLayout *sep_layout = pango_cairo_create_layout(cr);
            pango_layout_set_text(sep_layout, " \xE2\x96\xB8 ", -1);  // Unicode right-pointing small triangle

            PangoFontDescription *sep_font = pango_font_description_from_string(
                "Lucida Grande 9");
            pango_layout_set_font_description(sep_layout, sep_font);
            pango_font_description_free(sep_font);

            int sep_w, sep_h;
            pango_layout_get_pixel_size(sep_layout, &sep_w, &sep_h);
            (void)sep_h;

            cairo_move_to(cr, draw_x, path_y + text_y_offset);
            cairo_set_source_rgb(cr, 0x88 / 255.0, 0x88 / 255.0, 0x88 / 255.0);
            pango_cairo_show_layout(cr, sep_layout);
            g_object_unref(sep_layout);

            draw_x += sep_w;
        }

        // Measure the component text
        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, display, -1);

        PangoFontDescription *font = pango_font_description_from_string(
            "Lucida Grande 11");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);

        int text_w, text_h;
        pango_layout_get_pixel_size(layout, &text_w, &text_h);
        (void)text_h;

        // ── Draw the pill background behind the component ───────
        //
        // Each path component gets a subtle rounded pill shape,
        // slightly darker than the path bar background.
        double pill_x = draw_x - 4;
        double pill_y = path_y + 2;
        double pill_w = text_w + 8;
        double pill_h = fs->pathbar_h - 4;
        double pill_r = 3;

        cairo_new_sub_path(cr);
        cairo_arc(cr, pill_x + pill_w - pill_r, pill_y + pill_r,
                  pill_r, -M_PI / 2, 0);
        cairo_arc(cr, pill_x + pill_w - pill_r, pill_y + pill_h - pill_r,
                  pill_r, 0, M_PI / 2);
        cairo_arc(cr, pill_x + pill_r, pill_y + pill_h - pill_r,
                  pill_r, M_PI / 2, M_PI);
        cairo_arc(cr, pill_x + pill_r, pill_y + pill_r,
                  pill_r, M_PI, 3 * M_PI / 2);
        cairo_close_path(cr);

        // Subtle fill: slightly lighter than the gradient behind it
        cairo_set_source_rgba(cr, 230 / 255.0, 230 / 255.0, 230 / 255.0, 0.6);
        cairo_fill_preserve(cr);
        // Subtle border
        cairo_set_source_rgba(cr, 160 / 255.0, 160 / 255.0, 160 / 255.0, 0.5);
        cairo_set_line_width(cr, 0.5);
        cairo_stroke(cr);

        // ── Draw the component text ─────────────────────────────
        cairo_move_to(cr, draw_x, path_y + text_y_offset);
        cairo_set_source_rgb(cr, 0x33 / 255.0, 0x33 / 255.0, 0x33 / 255.0);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);

        draw_x += text_w + 2;

        // Stop drawing if we've run past the window edge
        if (draw_x > win_w - 20) break;
    }
}

// ── Painting ────────────────────────────────────────────────────────
//
// Paint the entire Finder window by calling each zone's paint function
// in order: toolbar (top), sidebar (left), content (remaining area),
// then the status bar and path bar at the bottom.

void finder_paint(FinderState *fs)
{
    if (!fs->cr) return;

    // Clear the entire window to white first.
    // This prevents artifacts when resizing.
    cairo_set_source_rgb(fs->cr, 1.0, 1.0, 1.0);
    cairo_paint(fs->cr);

    // Paint each zone in order. Each painter clips to its own area
    // so they don't overlap.
    toolbar_paint(fs);
    sidebar_paint(fs);
    content_paint(fs);

    // Paint the bottom bars after content so they draw on top.
    // Status bar shows item count and disk space; path bar shows
    // the breadcrumb navigation trail.
    statusbar_paint(fs);
    pathbar_paint(fs);

    // Flush Cairo's drawing commands to the X server so they
    // actually appear on screen.
    cairo_surface_flush(fs->surface);
    XFlush(fs->dpy);
}

// ── Navigation ──────────────────────────────────────────────────────

void finder_navigate(FinderState *fs, const char *new_path)
{
    // Update the stored path
    strncpy(fs->path, new_path, sizeof(fs->path) - 1);
    fs->path[sizeof(fs->path) - 1] = '\0';

    // Update the window title to reflect the new location
    update_window_title(fs);

    // Rescan the directory and repaint everything
    content_scan(fs->path);
    finder_paint(fs);

    fprintf(stderr, "[finder] Navigated to: %s\n", fs->path);
}

// ── Event Loop ──────────────────────────────────────────────────────
//
// The main event loop handles all X11 events and dispatches them to
// the appropriate module. It runs until fs->running is set to false
// (by a signal handler or the WM_DELETE_WINDOW close button).

void finder_run(FinderState *fs)
{
    // Atom for detecting the WM close button
    Atom wm_delete = XInternAtom(fs->dpy, "WM_DELETE_WINDOW", False);

    // Track double-click timing. If two clicks happen within 300ms
    // at roughly the same position, treat it as a double-click.
    struct timespec last_click_time = {0};
    int last_click_x = -1;
    int last_click_y = -1;

    XEvent ev;

    while (fs->running) {
        // Block until an event arrives. This is efficient — we don't
        // burn CPU spinning.
        XNextEvent(fs->dpy, &ev);

        switch (ev.type) {

        // ── Expose: the window needs repainting ─────────────────
        // This happens when the window is first shown, uncovered,
        // or the WM asks us to repaint.
        case Expose:
            // Only repaint on the last Expose in a batch (count == 0)
            // to avoid redundant repaints.
            if (ev.xexpose.count == 0) {
                finder_paint(fs);
            }
            break;

        // ── ConfigureNotify: the window was resized or moved ────
        case ConfigureNotify:
            // Check if the size actually changed (ignore move-only events)
            if (ev.xconfigure.width != fs->win_w ||
                ev.xconfigure.height != fs->win_h) {
                fs->win_w = ev.xconfigure.width;
                fs->win_h = ev.xconfigure.height;

                // Recreate Cairo surface at the new size
                recreate_cairo(fs);

                // Repaint everything at the new dimensions
                finder_paint(fs);
            }
            break;

        // ── ButtonPress: mouse click ────────────────────────────
        case ButtonPress: {
            int x = ev.xbutton.x;
            int y = ev.xbutton.y;

            fprintf(stderr, "[finder] ButtonPress at (%d, %d) button=%d window=0x%lx subwindow=0x%lx\n",
                    x, y, ev.xbutton.button, ev.xbutton.window, ev.xbutton.subwindow);

            // Only handle left-click (Button1) for now
            if (ev.xbutton.button != Button1) break;

            // Check for double-click: same area within 300ms
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long ms = (now.tv_sec - last_click_time.tv_sec) * 1000 +
                      (now.tv_nsec - last_click_time.tv_nsec) / 1000000;

            bool is_double = (ms < 300 &&
                              abs(x - last_click_x) < 5 &&
                              abs(y - last_click_y) < 5);

            last_click_time = now;
            last_click_x = x;
            last_click_y = y;

            // Determine which zone was clicked and dispatch.
            // Zone boundaries:
            //   Toolbar:  y < toolbar_h
            //   Sidebar:  y >= toolbar_h && x < sidebar_w
            //   Content:  y >= toolbar_h && x >= sidebar_w

            if (y < fs->toolbar_h) {
                // Click in the toolbar area
                toolbar_handle_click(fs, x, y);
            } else if (x < fs->sidebar_w) {
                // Click in the sidebar area
                sidebar_handle_click(fs, x, y);
            } else {
                // Click in the content area
                if (is_double) {
                    content_handle_double_click(fs, x, y);
                } else {
                    content_handle_click(fs, x, y);
                }
            }

            // Repaint to show selection changes
            finder_paint(fs);
            break;
        }

        // ── ClientMessage: WM close button ──────────────────────
        case ClientMessage:
            // Check if this is the WM asking us to close
            if ((Atom)ev.xclient.data.l[0] == wm_delete) {
                fprintf(stderr, "[finder] Close requested by WM\n");
                fs->running = false;
            }
            break;

        // ── KeyPress: keyboard input ────────────────────────────
        case KeyPress: {
            // Check for common keyboard shortcuts
            KeySym key = XLookupKeysym(&ev.xkey, 0);

            // Escape or Q to quit (for convenience during development)
            if (key == XK_Escape || key == XK_q) {
                fs->running = false;
            }
            break;
        }

        default:
            // Ignore all other events (MotionNotify, etc.)
            break;
        }
    }
}

// ── Shutdown ────────────────────────────────────────────────────────

void finder_shutdown(FinderState *fs)
{
    // Shut down sub-modules first (they may hold Cairo surfaces)
    content_shutdown();
    sidebar_shutdown();

    // Destroy Cairo resources
    if (fs->cr) {
        cairo_destroy(fs->cr);
        fs->cr = NULL;
    }
    if (fs->surface) {
        cairo_surface_destroy(fs->surface);
        fs->surface = NULL;
    }

    // Destroy the X window and close the display connection
    if (fs->dpy) {
        if (fs->win) {
            XDestroyWindow(fs->dpy, fs->win);
        }
        XCloseDisplay(fs->dpy);
        fs->dpy = NULL;
    }

    fprintf(stderr, "[finder] Shutdown complete\n");
}
