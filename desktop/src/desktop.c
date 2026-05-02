// CopyCatOS — by Kyle Blizzard at Blizzard.show

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
#include "layout.h"
#include "contextmenu.h"
#include "labels.h"
#include "dnd.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

// MoonRock -> shell scale bridge. MoonRock publishes per-output HiDPI scale
// on the root window; subscribing lets the desktop's icon grid track the
// scale of the output hosting the top-right anchor point.
#include "moonrock_scale.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>

// ── HiDPI scale ─────────────────────────────────────────────────────

// Definition of the extern declared in desktop.h. Starts at 1.0; updated
// from the _MOONROCK_OUTPUT_SCALES root-window property at startup and on
// every subsequent PropertyNotify. Tracks the *primary* output's scale
// because the icon grid anchors top-right of primary.
float desktop_hidpi_scale = 1.0f;

// Live snapshot of every connected output's scale as published by MoonRock.
// Refreshed at startup and on every PropertyNotify for that atom.
static MoonRockScaleTable g_output_scales;

// ── Forward declarations ────────────────────────────────────────────

static void desktop_repaint(Desktop *d);
static void desktop_repaint_one(Desktop *d, DesktopOutput *out);
static void reconcile_outputs(Desktop *d, bool initial);
static int  output_index_for_window(Desktop *d, Window w);
static void log_union_sentinel(Desktop *d);

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
        fprintf(stderr, "[desktop] Cannot open X display\n");
        return false;
    }

    d->screen      = DefaultScreen(d->dpy);
    d->root        = RootWindow(d->dpy, d->screen);
    d->running     = true;
    d->output_count = 0;
    d->primary_idx  = 0;

    // Subscribe to MoonRock's per-output scale property before any other
    // setup that depends on size — moonrock_scale_init's XSelectInput is
    // additive, so it ORs PropertyChangeMask onto whatever the root window
    // already has selected without clobbering anything.
    moonrock_scale_init(d->dpy);
    moonrock_scale_refresh(d->dpy, &g_output_scales);

    // Try to get a 32-bit ARGB visual for transparency effects.
    // If that fails, use the default visual (usually 24-bit RGB).
    d->visual = find_argb_visual(d->dpy, d->screen, &d->depth);
    if (d->visual) {
        fprintf(stderr, "[desktop] Using 32-bit ARGB visual\n");
        // ARGB visuals need their own colormap because they use a
        // different pixel format than the default visual.
        d->colormap = XCreateColormap(d->dpy, d->root, d->visual, AllocNone);
    } else {
        fprintf(stderr, "[desktop] Falling back to default visual\n");
        d->visual   = DefaultVisual(d->dpy, d->screen);
        d->depth    = DefaultDepth(d->dpy, d->screen);
        d->colormap = DefaultColormap(d->dpy, d->screen);
    }

    // Load the unscaled wallpaper source. Per-output scaled copies are
    // produced lazily by wallpaper_paint when each output's window is
    // painted for the first time.
    if (!wallpaper_init(wallpaper_path)) {
        fprintf(stderr, "[desktop] Warning: wallpaper init failed, using fallback\n");
    }

    // Build per-output windows from MoonRock's scale table. Falls back to
    // a single virtual-screen-union window if MoonRock isn't running or
    // hasn't published a table yet.
    reconcile_outputs(d, /*initial=*/true);

    // Pick the primary output's scale and geometry as the icon-grid anchor.
    // The icon grid lives top-right of primary regardless of how many
    // outputs are connected.
    DesktopOutput *primary = &d->outputs[d->primary_idx];
    desktop_hidpi_scale = primary->scale;
    fprintf(stderr, "[desktop] hidpi: icon-grid scale = %.2f on '%s' "
                    "(%d output%s known)\n",
            (double)desktop_hidpi_scale,
            primary->name,
            d->output_count,
            d->output_count == 1 ? "" : "s");

    // Initialize the icon grid (scan ~/Desktop, load icons, set up inotify).
    // Anchored to primary's pixel dimensions, not the virtual-screen union.
    icons_init(d->dpy, primary->width, primary->height);

    // Initialize XDND. The drag-source window and XdndProxy target are
    // primary's window — drag operations originate from icons on primary.
    dnd_init(d->dpy, primary->win, d->root);

    // Do an initial paint so the desktop isn't blank.
    desktop_repaint(d);

    log_union_sentinel(d);

    fprintf(stderr, "[desktop] Initialized successfully\n");
    return true;
}

// ── Painting ────────────────────────────────────────────────────────

// Paint one output: its wallpaper, plus icons if it's the primary output
// (the icon grid lives on primary only, anchored top-right of primary).
static void desktop_repaint_one(Desktop *d, DesktopOutput *out)
{
    if (!out || out->win == None) return;

    cairo_surface_t *xsurf = cairo_xlib_surface_create(
        d->dpy, out->win, d->visual, out->width, out->height);
    cairo_t *cr = cairo_create(xsurf);

    // Wallpaper — wallpaper.c caches a scaled copy keyed by (w, h), so a
    // hotplug that just moves an output reuses the cached surface; only
    // an output whose pixel size actually changed forces a recompute.
    wallpaper_paint(cr, out->width, out->height);

    // Icons: only primary draws them. Coordinates are pane-local
    // (origin = primary's top-left), which matches what icons.c stores.
    if (out == &d->outputs[d->primary_idx]) {
        icons_paint(cr, out->width, out->height);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(xsurf);
}

// Repaint every output's window.
static void desktop_repaint(Desktop *d)
{
    for (int i = 0; i < d->output_count; i++) {
        desktop_repaint_one(d, &d->outputs[i]);
    }
    XFlush(d->dpy);
}

// Find which output owns the given X window. Returns -1 if w isn't one
// of our windows (events on the root window etc.).
static int output_index_for_window(Desktop *d, Window w)
{
    for (int i = 0; i < d->output_count; i++) {
        if (d->outputs[i].win == w) return i;
    }
    return -1;
}

// Sentinel: warn loudly if DisplayWidth/Height (the legacy single-screen
// virtual-screen union) ever disagrees with the union of MoonRock's
// per-output rectangles. They are the same surface in X11 — this caught
// the original phantom of "DisplayWidth covers union but the desktop
// window only covers part of it." Now the windows are per-output, so
// this is a sanity check that the table we built our windows from
// actually fills the visible screen.
static void log_union_sentinel(Desktop *d)
{
    int dw = DisplayWidth(d->dpy, d->screen);
    int dh = DisplayHeight(d->dpy, d->screen);

    int max_x = 0, max_y = 0;
    for (int i = 0; i < d->output_count; i++) {
        DesktopOutput *o = &d->outputs[i];
        if (o->x + o->width  > max_x) max_x = o->x + o->width;
        if (o->y + o->height > max_y) max_y = o->y + o->height;
    }
    if (max_x != dw || max_y != dh) {
        fprintf(stderr,
                "[desktop] WARNING: output union (%dx%d) != "
                "DisplayWidth/Height (%dx%d) — outputs may not cover the "
                "full virtual screen.\n",
                max_x, max_y, dw, dh);
    }
}

// ── Output reconcile ────────────────────────────────────────────────
//
// Single source of truth for "what does the desktop component think the
// connected outputs look like right now?" — driven entirely by MoonRock's
// _MOONROCK_OUTPUT_SCALES root-window property. Called once at startup
// (initial=true creates first windows, fallback to single-screen if no
// table is published yet) and again on every PropertyNotify for that
// atom (initial=false keeps stable identity by name where possible).
//
// We deliberately do not subscribe to XRandR events here — MoonRock
// already republishes _MOONROCK_OUTPUT_SCALES on every hotplug and on
// every Displays-pane change, so a single PropertyNotify path covers
// both cases without a second event source to keep in sync. (See
// MoonRock's display.c — it owns the persistent EDID-keyed config and
// the rewrite cadence.)
static void reconcile_outputs(Desktop *d, bool initial)
{
    DesktopOutput old[MOONROCK_SCALE_MAX_OUTPUTS];
    int old_count = d->output_count;
    memcpy(old, d->outputs, sizeof(DesktopOutput) * old_count);

    DesktopOutput next[MOONROCK_SCALE_MAX_OUTPUTS];
    int next_count = 0;
    int next_primary = 0;

    if (g_output_scales.valid && g_output_scales.count > 0) {
        // Build the new list from MoonRock's table, in the same row order
        // (stable across publishes when no hotplug happened).
        for (int i = 0; i < g_output_scales.count
             && next_count < MOONROCK_SCALE_MAX_OUTPUTS; i++) {
            const MoonRockOutputScale *s = &g_output_scales.outputs[i];
            DesktopOutput *o = &next[next_count];
            memset(o, 0, sizeof(*o));
            strncpy(o->name, s->name, sizeof(o->name) - 1);
            o->x       = s->x;
            o->y       = s->y;
            o->width   = s->width;
            o->height  = s->height;
            o->scale   = s->scale > 0 ? s->scale : 1.0f;
            o->primary = s->primary;
            o->win     = None;
            if (s->primary) next_primary = next_count;
            next_count++;
        }
    } else {
        // MoonRock not running yet — fall back to a single virtual-screen
        // window so the desktop still draws on dev environments that boot
        // a desktop component without a compositor.
        DesktopOutput *o = &next[0];
        memset(o, 0, sizeof(*o));
        strncpy(o->name, "virt-screen", sizeof(o->name) - 1);
        o->x       = 0;
        o->y       = 0;
        o->width   = DisplayWidth(d->dpy, d->screen);
        o->height  = DisplayHeight(d->dpy, d->screen);
        o->scale   = 1.0f;
        o->primary = true;
        o->win     = None;
        next_count = 1;
    }

    // For each entry in `next`, reuse an existing window from `old` if a
    // same-named output exists (preserves WID identity across resizes /
    // primary swaps). Resize/move it if its rect changed; otherwise leave
    // it alone. Anything in `old` that doesn't appear in `next` gets its
    // window destroyed afterward.
    XSetWindowAttributes attrs;
    attrs.colormap         = d->colormap;
    attrs.border_pixel     = 0;
    attrs.background_pixel = 0;
    attrs.event_mask =
        ExposureMask | ButtonPressMask | ButtonReleaseMask |
        PointerMotionMask | StructureNotifyMask;
    unsigned long attr_mask =
        CWColormap | CWBorderPixel | CWBackPixel | CWEventMask;

    Atom wm_type    = XInternAtom(d->dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom wm_desktop = XInternAtom(d->dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);

    bool any_size_changed = false;

    for (int i = 0; i < next_count; i++) {
        DesktopOutput *o = &next[i];

        // Find a same-named entry in old.
        int reuse = -1;
        for (int j = 0; j < old_count; j++) {
            if (old[j].win != None &&
                strcmp(old[j].name, o->name) == 0) {
                reuse = j;
                break;
            }
        }

        if (reuse >= 0) {
            o->win = old[reuse].win;
            old[reuse].win = None;  // claimed — don't destroy below

            bool size_changed =
                (old[reuse].width  != o->width) ||
                (old[reuse].height != o->height);
            bool moved =
                (old[reuse].x != o->x) || (old[reuse].y != o->y);

            if (size_changed || moved) {
                XMoveResizeWindow(d->dpy, o->win,
                                  o->x, o->y, o->width, o->height);
                if (size_changed) any_size_changed = true;
            }
        } else {
            // Fresh window for a new (or initially-built) output.
            o->win = XCreateWindow(d->dpy, d->root,
                o->x, o->y, o->width, o->height,
                0, d->depth, InputOutput, d->visual,
                attr_mask, &attrs);

            XChangeProperty(d->dpy, o->win, wm_type, XA_ATOM, 32,
                            PropModeReplace,
                            (unsigned char *)&wm_desktop, 1);

            char title[128];
            snprintf(title, sizeof(title), "CopyCatOS Desktop (%s)", o->name);
            XStoreName(d->dpy, o->win, title);

            // WM_CLASS = "desktop" lets menubar map this pane to the
            // "Finder" app entry in app_names[] when moonrock surfaces
            // the desktop window as the frontmost-per-output. Without
            // it, an empty-space click would have no app identity for
            // menubar to look up. Both res_name and res_class are set
            // so future sniffers (window list, accessibility) get the
            // same answer regardless of which one they read.
            XClassHint ch = { (char *)"desktop", (char *)"desktop" };
            XSetClassHint(d->dpy, o->win, &ch);

            XMapWindow(d->dpy, o->win);
            any_size_changed = true;
        }
    }

    // Destroy any old windows whose outputs no longer exist.
    for (int j = 0; j < old_count; j++) {
        if (old[j].win != None) {
            fprintf(stderr,
                    "[desktop] Output '%s' gone — destroying its window\n",
                    old[j].name);
            XDestroyWindow(d->dpy, old[j].win);
            any_size_changed = true;
        }
    }

    // Commit.
    memcpy(d->outputs, next, sizeof(DesktopOutput) * next_count);
    d->output_count = next_count;
    d->primary_idx  = next_primary;

    // Drop stale wallpaper cache entries if any output's pixel size
    // changed; the cache is keyed by (w, h) so old entries that no
    // longer match any output simply waste memory until invalidated.
    if (any_size_changed) {
        wallpaper_invalidate_cache();
    }

    if (!initial) {
        log_union_sentinel(d);
    }

    fprintf(stderr,
            "[desktop] Reconciled %d output%s (primary='%s' %dx%d @ %.2fx)\n",
            d->output_count,
            d->output_count == 1 ? "" : "s",
            d->outputs[d->primary_idx].name,
            d->outputs[d->primary_idx].width,
            d->outputs[d->primary_idx].height,
            (double)d->outputs[d->primary_idx].scale);
}

// Re-layout icons and repaint after desktop_hidpi_scale changes. Called
// on the MoonRock scale-change PropertyNotify path. Layout constants in
// icons.h are in points, so layout_apply() naturally recomputes every
// pixel position from the new scaled values.
//
// Icon source surfaces stay loaded at their original pixel sizes — they
// are scaled per-paint via cairo_scale(cr, S(ICON_SIZE)/src_w, ...) so
// the new scale factor automatically changes their apparent size.
static void apply_desktop_scale(Desktop *d)
{
    icons_rescale();
    desktop_repaint(d);
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

    // XDND drag tracking.
    // We don't start an XDND drag immediately on click — we wait until the
    // mouse has moved at least DND_DRAG_THRESHOLD pixels from the click point.
    // This prevents accidental drags on a sloppy click.
    int drag_start_x = 0;       // Root-X at the moment the click registered
    int drag_start_y = 0;       // Root-Y at the moment the click registered
    bool xdnd_started = false;  // True after we've called dnd_source_begin()

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

        // Check for inotify events (files changed in ~/Desktop).
        // Call every iteration, not just when the fd is readable: the
        // debounce-fire path runs on a *follow-up* call with an empty
        // read, after INOTIFY_DEBOUNCE_MS has elapsed since the last
        // event. The 500 ms select timeout guarantees that follow-up.
        if (inotify_fd >= 0) {
            if (icons_check_inotify()) {
                // Icons were refreshed, repaint everything
                desktop_repaint(d);
            }
        }

        // Idle tick: check if a pending XdndDrop has gone unanswered for 3s.
        // Some apps never send XdndFinished; we time out and snap the icon back.
        if (ready == 0) {  // select() timed out (no events)
            if (dnd_tick()) {
                // Drag timed out — snap the dragged icon back to its grid position
                if (dragging && drag_active) {
                    DesktopOutput *p = &d->outputs[d->primary_idx];
                    icons_drag_end(p->width, p->height);
                    dragging    = NULL;
                    drag_active = false;
                    xdnd_started = false;
                }
                desktop_repaint(d);
            }
        }

        // Process all pending X events.
        // XPending() returns the number of events in the queue.
        while (XPending(d->dpy) > 0) {
            XEvent ev;
            XNextEvent(d->dpy, &ev);

            // For dispatch we frequently need the click's host output and
            // the primary output. Compute once per event.
            DesktopOutput *primary = &d->outputs[d->primary_idx];

            switch (ev.type) {
            case Expose:
                // Repaint just the output whose window got exposed.
                // count == 0 means "this is the last expose in the batch
                // for that window," which keeps us from repainting
                // multiple times for one logical exposure.
                if (ev.xexpose.count == 0) {
                    int oi = output_index_for_window(d, ev.xexpose.window);
                    if (oi >= 0) {
                        desktop_repaint_one(d, &d->outputs[oi]);
                        XFlush(d->dpy);
                    } else {
                        desktop_repaint(d);
                    }
                }
                break;

            case ButtonPress:
                if (ev.xbutton.button == Button1) {
                    // Click hit-tests against the icon grid only when the
                    // click landed on the primary's window — icons live on
                    // primary alone. Translate root coords into primary's
                    // pane-local space so icons_handle_click works on any
                    // _MOONROCK_OUTPUT_SCALES layout (primary at non-zero
                    // origin is normal once an external is plugged in to
                    // the left of the laptop panel).
                    int local_x = ev.xbutton.x_root - primary->x;
                    int local_y = ev.xbutton.y_root - primary->y;
                    bool on_primary = (ev.xbutton.window == primary->win);

                    DesktopIcon *hit = NULL;
                    if (on_primary) {
                        hit = icons_handle_click(local_x, local_y);
                    }

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
                            // the mouse moves DND_DRAG_THRESHOLD pixels
                            // away from the click point (in MotionNotify).
                            dragging     = hit;
                            drag_start_x = ev.xbutton.x_root;
                            drag_start_y = ev.xbutton.y_root;
                            xdnd_started = false;
                            icons_drag_begin(hit, local_x, local_y);
                        }
                    } else {
                        // Clicked on empty space: deselect all icons and
                        // ask moonrock to make this desktop pane "active"
                        // for its output. Snow Leopard parity — clicking
                        // the desktop surfaces Finder's menu bar without
                        // opening any Finder window. moonrock recognizes
                        // the _NET_WM_WINDOW_TYPE_DESKTOP target, clears
                        // its focused-client slot for that output, and
                        // republishes _MOONROCK_FRONTMOST_PER_OUTPUT
                        // pointing at this pane. menubar then maps the
                        // pane's WM_CLASS ("desktop") to "Finder".
                        icons_deselect_all();
                        last_clicked = NULL;

                        Atom net_active = XInternAtom(
                            d->dpy, "_NET_ACTIVE_WINDOW", False);
                        XEvent ax;
                        memset(&ax, 0, sizeof(ax));
                        ax.xclient.type         = ClientMessage;
                        ax.xclient.window       = ev.xbutton.window;
                        ax.xclient.message_type = net_active;
                        ax.xclient.format       = 32;
                        ax.xclient.data.l[0]    = 1; // source = app
                        ax.xclient.data.l[1]    = (long)ev.xbutton.time;
                        ax.xclient.data.l[2]    = 0;
                        XSendEvent(d->dpy, d->root, False,
                                   SubstructureNotifyMask
                                       | SubstructureRedirectMask,
                                   &ax);
                    }

                    desktop_repaint(d);
                }
                else if (ev.xbutton.button == Button3) {
                    // Right-click: two cases —
                    //   Hit an icon  → show icon context menu (Open, Label, Trash)
                    //   Hit desktop  → show desktop context menu (New Folder, etc.)
                    int rlocal_x = ev.xbutton.x_root - primary->x;
                    int rlocal_y = ev.xbutton.y_root - primary->y;
                    bool ron_primary = (ev.xbutton.window == primary->win);

                    DesktopIcon *rhit = NULL;
                    if (ron_primary) {
                        rhit = icons_handle_click(rlocal_x, rlocal_y);
                    }

                    // Context menus clamp themselves against virtual-screen
                    // bounds. Build the union from the output table so a
                    // menu on a non-primary output doesn't get clipped to
                    // primary's geometry.
                    int union_w = 0, union_h = 0;
                    for (int i = 0; i < d->output_count; i++) {
                        int rx = d->outputs[i].x + d->outputs[i].width;
                        int ry = d->outputs[i].y + d->outputs[i].height;
                        if (rx > union_w) union_w = rx;
                        if (ry > union_h) union_h = ry;
                    }

                    if (rhit) {
                        // Select the right-clicked icon so the user can see
                        // which icon the menu applies to.
                        icons_select(rhit);
                        desktop_repaint(d);

                        int action = contextmenu_show_icon(d->dpy, d->root,
                            ev.xbutton.x_root, ev.xbutton.y_root,
                            union_w, union_h, rhit);

                        switch (action) {
                        case ICON_ACTION_OPEN:
                            // Open file with its default application
                            icons_handle_double_click(rhit);
                            break;

                        case ICON_ACTION_INFO:
                            // Get Info — placeholder for now
                            fprintf(stderr, "[desktop] TODO: Get Info for '%s'\n",
                                    rhit->name);
                            break;

                        case ICON_ACTION_TRASH:
                            // Move to Trash — placeholder for now
                            fprintf(stderr, "[desktop] TODO: Move to Trash '%s'\n",
                                    rhit->name);
                            break;

                        default:
                            if (action >= ICON_ACTION_LABEL_BASE) {
                                // Label selected (base+0 = none, base+1 = Red, etc.)
                                int new_label = action - ICON_ACTION_LABEL_BASE;
                                label_set(rhit->path, new_label);
                                rhit->label = new_label;
                                fprintf(stderr, "[desktop] Label '%s' → %s\n",
                                        rhit->name, label_names[new_label]);
                            }
                            break;
                        }

                        desktop_repaint(d);

                    } else {
                        // Right-click on empty desktop space
                        icons_deselect_all();
                        desktop_repaint(d);

                        // contextmenu_show() runs its own mini event loop
                        // and returns the index of the selected item.
                        int choice = contextmenu_show(d->dpy, d->root,
                            ev.xbutton.x_root, ev.xbutton.y_root,
                            union_w, union_h);

                        // Handle the selected menu action
                        switch (choice) {
                        case 0:  // "New Folder"
                        {
                            // Create a new folder in ~/Desktop.
                            // If "untitled folder" already exists, append a number.
                            char folder[1024];
                            const char *home = getenv("HOME");
                            snprintf(folder, sizeof(folder),
                                     "%s/Desktop/untitled folder", home);

                            int n = 1;
                            while (access(folder, F_OK) == 0) {
                                snprintf(folder, sizeof(folder),
                                         "%s/Desktop/untitled folder %d", home, ++n);
                            }
                            mkdir(folder, 0755);
                            // inotify will pick up the change automatically
                            break;
                        }
                        case 3:  // "Clean Up"
                            icons_relayout(primary->width, primary->height);
                            desktop_repaint(d);
                            break;
                        case 5:  // "Change Desktop Background..."
                            fprintf(stderr, "[desktop] TODO: desktop background picker\n");
                            break;
                        case 7:  // "Open Terminal Here"
                        {
                            pid_t pid = fork();
                            if (pid == 0) {
                                const char *home = getenv("HOME");
                                char workdir[1024];
                                snprintf(workdir, sizeof(workdir),
                                         "%s/Desktop", home);
                                execlp("konsole", "konsole",
                                       "--workdir", workdir, NULL);
                                _exit(127);
                            }
                            break;
                        }
                        default:
                            break;
                        }

                        desktop_repaint(d);
                    }
                }
                break;

            case MotionNotify:
                // Mouse moved while a button is held down.
                // If we're tracking a potential drag, update the icon position.
                if (dragging && (ev.xmotion.state & Button1Mask)) {
                    // Motion-event coalescing — drain every queued MotionNotify
                    // and keep only the latest position. X delivers many motion
                    // events per frame on touch / fast cursor movement; without
                    // this drain the queue grows faster than the per-event
                    // repaint can run and the dragged icon visibly lags behind
                    // the cursor. After XGrabPointer (started inside
                    // dnd_source_begin) all motion routes through the grab so
                    // there's no risk of pulling motion meant for another
                    // window. We keep the latest event's coordinates and
                    // button-state mask; everything else (time, modifiers
                    // beyond Button1) is fungible during a drag.
                    //
                    // Lag instrumentation: while DESKTOP_DRAG_TRACE=1, log
                    // per-motion-batch timings — drain depth, drain wall, and
                    // icons_drag_update wall. Lets us measure where motion
                    // time is actually going on the Legion's 2880×1800 panel
                    // instead of guessing. Off by default; enable from the
                    // session script when reproducing lag.
                    static int trace_inited = 0;
                    static int trace_drag = 0;
                    if (!trace_inited) {
                        const char *t = getenv("DESKTOP_DRAG_TRACE");
                        trace_drag = (t && t[0] && t[0] != '0');
                        trace_inited = 1;
                    }
                    struct timespec t_drain_start;
                    if (trace_drag) clock_gettime(CLOCK_MONOTONIC, &t_drain_start);

                    XEvent peek;
                    int drain_count = 0;
                    while (XCheckTypedEvent(d->dpy, MotionNotify, &peek)) {
                        ev = peek;
                        drain_count++;
                    }

                    struct timespec t_drain_end;
                    if (trace_drag) clock_gettime(CLOCK_MONOTONIC, &t_drain_end);

                    // Check whether we've moved past the drag threshold.
                    // This prevents accidental drags from sloppy single-clicks.
                    int dx = ev.xmotion.x_root - drag_start_x;
                    int dy = ev.xmotion.y_root - drag_start_y;
                    int dist = dx*dx + dy*dy;  // Squared distance (no sqrt needed)
                    int thresh_sq = DND_DRAG_THRESHOLD * DND_DRAG_THRESHOLD;

                    if (dist >= thresh_sq) {
                        // Past the threshold — activate the visual drag
                        drag_active = true;

                        // Move the visual drag forward. icons_drag_update
                        // advances the icon's logical position (so the
                        // eventual ButtonRelease clamp lands at the
                        // cursor's release point) and either creates +
                        // maps the override-redirect ghost popup (first
                        // call after threshold) or XMoveWindows it
                        // (every subsequent call). Local coords keep
                        // the icon math in primary-pane space; root
                        // coords position the ghost in the virtual
                        // screen frame. We do this BEFORE dispatching
                        // XDND so the ghost is mapped on the same
                        // round-trip that XDND begins.
                        int local_x = ev.xmotion.x_root - primary->x;
                        int local_y = ev.xmotion.y_root - primary->y;

                        struct timespec t_upd_start;
                        if (trace_drag) clock_gettime(CLOCK_MONOTONIC, &t_upd_start);
                        icons_drag_update(local_x, local_y,
                                          ev.xmotion.x_root,
                                          ev.xmotion.y_root);
                        if (trace_drag) {
                            struct timespec t_upd_end;
                            clock_gettime(CLOCK_MONOTONIC, &t_upd_end);
                            long drain_us = (t_drain_end.tv_sec - t_drain_start.tv_sec) * 1000000L +
                                            (t_drain_end.tv_nsec - t_drain_start.tv_nsec) / 1000L;
                            long upd_us   = (t_upd_end.tv_sec - t_upd_start.tv_sec) * 1000000L +
                                            (t_upd_end.tv_nsec - t_upd_start.tv_nsec) / 1000L;
                            fprintf(stderr,
                                "[drag-trace] drain=%d depth=%ld us  drag_update=%ld us  pos=(%d,%d)\n",
                                drain_count, drain_us, upd_us,
                                ev.xmotion.x_root, ev.xmotion.y_root);
                        }

                        // Start XDND on the first frame that crosses the threshold.
                        // Source window is primary's desktop window, since the
                        // dragged icon lives there.
                        if (!xdnd_started) {
                            xdnd_started = true;
                            dnd_source_begin(d->dpy, primary->win, d->root,
                                             dragging->path,
                                             ev.xmotion.x_root,
                                             ev.xmotion.y_root);

                            // One-shot repaint at threshold cross so the
                            // dragged icon's "origin" cell renders at
                            // 50% alpha (Snow Leopard parity). After
                            // this point neither the original nor the
                            // ghost moves on the primary pane — the
                            // ghost is its own override-redirect window
                            // and just gets XMoveWindow'd on each
                            // motion event, so no per-frame full
                            // wallpaper + icon-grid repaint is needed.
                            desktop_repaint_one(d, primary);
                            XFlush(d->dpy);
                        } else {
                            // Update XDND position once per coalesced batch.
                            // Sending an XdndPosition per raw motion event was
                            // also part of the lag — same drain logic above
                            // collapses that to one ClientMessage per frame.
                            dnd_source_motion(d->dpy,
                                              ev.xmotion.x_root,
                                              ev.xmotion.y_root);
                        }
                    }
                }
                break;

            case ButtonRelease:
                if (ev.xbutton.button == Button1 && dragging) {
                    if (drag_active) {
                        // Try to complete as an XDND drop onto an external window.
                        // dnd_source_drop() returns true if XDND handled it —
                        // in that case, don't grid-snap (the file went to another app).
                        // It returns false when the cursor is over our own desktop,
                        // so we do the normal grid-snap.
                        if (!xdnd_started || !dnd_source_drop(d->dpy)) {
                            // No XDND target — clamp icon and persist xattr.
                            // icons_drag_end internally tears down the ghost
                            // and clears icons.c's drag pointer.
                            icons_drag_end(primary->width, primary->height);
                        } else {
                            // XDND took it — file moves to another app, our
                            // copy gets removed by inotify rescan. Don't
                            // clamp/save, but still cancel so the ghost
                            // window is destroyed and icons.c's drag
                            // pointer doesn't dangle into the next rescan.
                            icons_drag_cancel();
                        }
                        desktop_repaint(d);
                    } else if (xdnd_started) {
                        // Threshold was never crossed but we did call source_begin.
                        // Cancel cleanly.
                        dnd_source_cancel(d->dpy);
                        icons_drag_cancel();
                    } else {
                        // Simple click+release — never crossed threshold,
                        // no XDND, no ghost. icons_drag_begin still ran on
                        // ButtonPress, so icons.c is holding a drag_icon
                        // pointer that needs clearing before the next
                        // click — otherwise the next icons_paint would
                        // briefly fade this icon at 50% alpha if a drag
                        // started and exposed drag_visible. Always cancel.
                        icons_drag_cancel();
                    }
                    dragging     = NULL;
                    drag_active  = false;
                    xdnd_started = false;
                }
                break;

            case PropertyNotify:
                // MoonRock rewrites _MOONROCK_OUTPUT_SCALES on hotplug and
                // on any Displays-pane change. One PropertyNotify drives
                // both the per-output window reconcile (geometry) and the
                // icon-grid rescale (primary scale change). Ignore every
                // other root-window property change — there are a lot of
                // them.
                if (ev.xproperty.window == d->root &&
                    ev.xproperty.atom == moonrock_scale_atom(d->dpy)) {
                    moonrock_scale_refresh(d->dpy, &g_output_scales);

                    int    old_primary_idx = d->primary_idx;
                    int    old_primary_w   = d->outputs[old_primary_idx].width;
                    int    old_primary_h   = d->outputs[old_primary_idx].height;
                    char   old_primary_name[MOONROCK_SCALE_NAME_MAX];
                    strncpy(old_primary_name,
                            d->outputs[old_primary_idx].name,
                            sizeof(old_primary_name));
                    float  old_scale       = desktop_hidpi_scale;

                    reconcile_outputs(d, /*initial=*/false);

                    DesktopOutput *p = &d->outputs[d->primary_idx];
                    desktop_hidpi_scale = p->scale;

                    bool primary_changed =
                        strcmp(old_primary_name, p->name) != 0 ||
                        p->width  != old_primary_w        ||
                        p->height != old_primary_h;

                    if (primary_changed) {
                        // Icons need to be re-laid against the new primary
                        // geometry so they still sit top-right of *that*
                        // output. icons_relayout uses the saved logical
                        // grid positions (col/row), so positions remain
                        // stable under primary swap/resize.
                        icons_relayout(p->width, p->height);
                    } else if (desktop_hidpi_scale != old_scale) {
                        // Same primary, scale flipped (e.g. user dragged
                        // the Interface Scale slider). Re-apply layout to
                        // pick up the new S() scale factor.
                        apply_desktop_scale(d);
                    }

                    // Always repaint — even unchanged outputs benefit from
                    // a fresh paint after the wallpaper cache may have
                    // been invalidated for another output's resize.
                    desktop_repaint(d);
                }
                break;

            case ClientMessage:
                // XDND uses ClientMessage events for the entire protocol handshake.
                // This handles: XdndStatus, XdndFinished (source side) and
                // XdndEnter, XdndPosition, XdndLeave, XdndDrop (target side).
                dnd_handle_client_message(d->dpy, &ev.xclient);
                break;

            case SelectionRequest:
                // A drop target called XConvertSelection to fetch the file URI
                // from us (we're acting as the source/selection owner).
                // We write the URI into their requested property and confirm.
                dnd_handle_selection_request(d->dpy, &ev.xselectionrequest);
                break;

            case SelectionNotify:
                // We called XConvertSelection (as target) and the source has
                // written the file URI data into our requested property.
                // Read it, decode the path, copy the file to ~/Desktop.
                {
                    const char *dest = dnd_handle_selection_notify(
                        d->dpy, &ev.xselection);
                    if (dest) {
                        // A new file arrived on the desktop — inotify will
                        // catch it, but trigger a repaint now for instant feedback.
                        fprintf(stderr, "[desktop] File dropped onto desktop: %s\n",
                                dest);
                        desktop_repaint(d);
                    }
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
    // Remove XdndProxy from root so apps don't try to drop onto our dead window
    dnd_shutdown(d->dpy, d->root);

    icons_shutdown();
    wallpaper_shutdown();

    for (int i = 0; i < d->output_count; i++) {
        if (d->outputs[i].win != None) {
            XDestroyWindow(d->dpy, d->outputs[i].win);
            d->outputs[i].win = None;
        }
    }
    d->output_count = 0;

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
