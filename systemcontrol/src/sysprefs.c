// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// sysprefs.c — Core window management and event loop
// ============================================================================
//
// Creates the System Preferences window as a normal managed window (moonrock
// provides the title bar, traffic lights, and shadow). This module handles:
//   - X11 window creation with ARGB visual
//   - Cairo surface setup
//   - The select()-based event loop
//   - Dispatching events to toolbar, icon grid, and pane view
//   - Full-window repainting
// ============================================================================

#include "sysprefs.h"
#include "toolbar.h"
#include "icongrid.h"
#include "paneview.h"
#include "panes/displays.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

// ============================================================================
// find_argb_visual — Find a 32-bit visual with alpha channel
// ============================================================================
//
// Scans available visuals to find one with 32-bit depth and a TrueColor class.
// This lets us have a transparent window background so the WM shadow shows
// through correctly. Falls back to the default visual if no ARGB visual exists.
// ============================================================================
static Visual *find_argb_visual(Display *dpy, int screen)
{
    XVisualInfo vinfo_template;
    vinfo_template.screen = screen;
    vinfo_template.depth = 32;
    vinfo_template.class = TrueColor;

    int num_visuals = 0;
    XVisualInfo *visuals = XGetVisualInfo(dpy,
        VisualScreenMask | VisualDepthMask | VisualClassMask,
        &vinfo_template, &num_visuals);

    Visual *result = NULL;
    if (visuals && num_visuals > 0) {
        result = visuals[0].visual;
    }
    if (visuals) {
        XFree(visuals);
    }

    return result;
}

// ============================================================================
// sysprefs_init — Create the window and set up Cairo
// ============================================================================
bool sysprefs_init(SysPrefsState *state)
{
    // Open connection to the X server
    state->dpy = XOpenDisplay(NULL);
    if (!state->dpy) {
        fprintf(stderr, "[systemcontrol] Cannot open X display\n");
        return false;
    }

    state->screen = DefaultScreen(state->dpy);
    state->root = RootWindow(state->dpy, state->screen);
    state->screen_w = DisplayWidth(state->dpy, state->screen);
    state->screen_h = DisplayHeight(state->dpy, state->screen);

    // Window dimensions
    state->win_w = SYSPREFS_WIN_W;
    state->win_h = SYSPREFS_WIN_H;

    // Use the default visual — System Preferences is a regular opaque window.
    // Only shell components like dock/searchsystem need ARGB for transparency.
    state->visual = DefaultVisual(state->dpy, state->screen);
    state->colormap = DefaultColormap(state->dpy, state->screen);
    int depth = DefaultDepth(state->dpy, state->screen);

    // Center the window on screen
    int win_x = (state->screen_w - state->win_w) / 2;
    int win_y = (state->screen_h - state->win_h) / 3;  // Upper third, like macOS

    // Create the window — a normal managed window, not override_redirect.
    // The WM provides the title bar, traffic lights, and drop shadow.
    XSetWindowAttributes attrs;
    attrs.colormap = state->colormap;
    attrs.background_pixel = WhitePixel(state->dpy, state->screen);
    attrs.border_pixel = 0;
    attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask
                     | PointerMotionMask | KeyPressMask | KeyReleaseMask
                     | StructureNotifyMask | EnterWindowMask | LeaveWindowMask;

    state->win = XCreateWindow(
        state->dpy, state->root,
        win_x, win_y, state->win_w, state->win_h,
        0,                          // Border width
        depth,                      // Default depth (24-bit, opaque)
        InputOutput,
        state->visual,
        CWColormap | CWBackPixel | CWBorderPixel | CWEventMask,
        &attrs
    );

    // Set window properties so the WM knows what kind of window this is
    // WM_NAME — shown in the title bar
    XStoreName(state->dpy, state->win, "System Preferences");

    // WM_CLASS — used for taskbar grouping and icon lookup
    XClassHint class_hint;
    class_hint.res_name = "systemcontrol";
    class_hint.res_class = "System Preferences";
    XSetClassHint(state->dpy, state->win, &class_hint);

    // _NET_WM_WINDOW_TYPE = _NET_WM_WINDOW_TYPE_NORMAL
    Atom type_atom = XInternAtom(state->dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom normal_atom = XInternAtom(state->dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    XChangeProperty(state->dpy, state->win, type_atom, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&normal_atom, 1);

    // Size hints — set a minimum and initial size
    XSizeHints *size_hints = XAllocSizeHints();
    size_hints->flags = PMinSize | PBaseSize;
    size_hints->min_width = SYSPREFS_WIN_W;
    size_hints->min_height = SYSPREFS_WIN_H;
    size_hints->base_width = SYSPREFS_WIN_W;
    size_hints->base_height = SYSPREFS_WIN_H;
    XSetWMNormalHints(state->dpy, state->win, size_hints);
    XFree(size_hints);

    // WM_DELETE_WINDOW — handle the close button properly
    Atom wm_delete = XInternAtom(state->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(state->dpy, state->win, &wm_delete, 1);

    // Create Cairo drawing surface
    state->surface = cairo_xlib_surface_create(
        state->dpy, state->win, state->visual,
        state->win_w, state->win_h);
    state->cr = cairo_create(state->surface);

    // Initialize view state
    state->current_view = VIEW_GRID;
    state->current_pane = -1;
    state->hover_pane = -1;
    state->toolbar_hover = -1;
    state->history_pos = -1;
    state->history_len = 0;
    state->search_query[0] = '\0';
    state->search_focused = false;
    state->running = true;

    // Show the window
    XMapWindow(state->dpy, state->win);
    XFlush(state->dpy);

    fprintf(stderr, "[systemcontrol] Window created: %dx%d at (%d, %d)\n",
            state->win_w, state->win_h, win_x, win_y);

    return true;
}

// ============================================================================
// sysprefs_paint — Full repaint of the window
// ============================================================================
void sysprefs_paint(SysPrefsState *state)
{
    cairo_t *cr = state->cr;

    // Clear to the Snow Leopard window background color
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(cr, 0xEC / 255.0, 0xEC / 255.0, 0xEC / 255.0);
    cairo_paint(cr);

    // Switch to normal compositing for the rest of the UI
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // Paint the toolbar (gradient background, Show All, search field)
    toolbar_paint(state);

    // Paint the appropriate view below the toolbar
    if (state->current_view == VIEW_GRID) {
        icongrid_paint(state);
    } else {
        paneview_paint(state);
    }

    // Flush to screen
    cairo_surface_flush(state->surface);
    XFlush(state->dpy);
}

// ============================================================================
// sysprefs_open_pane — Switch to viewing a specific preference pane
// ============================================================================
void sysprefs_open_pane(SysPrefsState *state, int pane_index)
{
    if (pane_index < 0 || pane_index >= state->pane_count) return;

    // Add to navigation history
    state->history_pos++;
    state->history[state->history_pos] = pane_index;
    state->history_len = state->history_pos + 1;

    state->current_view = VIEW_PANE;
    state->current_pane = pane_index;
    state->hover_pane = -1;

    // Per-pane on-entry hooks — each pane with live state it needs to
    // refresh when re-opened signals that here.
    if (strcmp(state->panes[pane_index].id, "displays") == 0) {
        displays_pane_mark_dirty();
    }

    // Update window title
    char title[256];
    snprintf(title, sizeof(title), "%s", state->panes[pane_index].name);
    XStoreName(state->dpy, state->win, title);

    sysprefs_paint(state);
}

// ============================================================================
// sysprefs_show_all — Return to the icon grid view
// ============================================================================
void sysprefs_show_all(SysPrefsState *state)
{
    state->current_view = VIEW_GRID;
    state->current_pane = -1;
    state->hover_pane = -1;

    XStoreName(state->dpy, state->win, "System Preferences");
    sysprefs_paint(state);
}

// ============================================================================
// sysprefs_run — Main event loop
// ============================================================================
//
// Uses select() to wait for X events efficiently. Dynamic timeout:
//   - 100ms when idle (responsive enough for hover updates)
//   - Repaints only when needed (Expose events or state changes)
// ============================================================================
void sysprefs_run(SysPrefsState *state)
{
    int x_fd = ConnectionNumber(state->dpy);
    Atom wm_delete = XInternAtom(state->dpy, "WM_DELETE_WINDOW", False);

    while (state->running) {
        // Process all pending X events
        while (XPending(state->dpy) > 0) {
            XEvent evt;
            XNextEvent(state->dpy, &evt);

            switch (evt.type) {
                case Expose:
                    // Only repaint on the last Expose in a burst
                    if (evt.xexpose.count == 0) {
                        sysprefs_paint(state);
                    }
                    break;

                case ConfigureNotify:
                    // Window was resized — update Cairo surface
                    if (evt.xconfigure.width != state->win_w ||
                        evt.xconfigure.height != state->win_h) {
                        state->win_w = evt.xconfigure.width;
                        state->win_h = evt.xconfigure.height;
                        cairo_xlib_surface_set_size(state->surface,
                                                    state->win_w, state->win_h);
                        sysprefs_paint(state);
                    }
                    break;

                case MotionNotify: {
                    state->mouse_x = evt.xmotion.x;
                    state->mouse_y = evt.xmotion.y;

                    bool needs_repaint = false;

                    // Check toolbar hover
                    if (evt.xmotion.y < TOOLBAR_HEIGHT) {
                        needs_repaint = toolbar_handle_motion(state,
                                            evt.xmotion.x, evt.xmotion.y);
                        // Clear grid hover
                        if (state->hover_pane != -1) {
                            state->hover_pane = -1;
                            needs_repaint = true;
                        }
                    } else if (state->current_view == VIEW_GRID) {
                        needs_repaint = icongrid_handle_motion(state,
                                            evt.xmotion.x, evt.xmotion.y);
                        if (state->toolbar_hover != -1) {
                            state->toolbar_hover = -1;
                            needs_repaint = true;
                        }
                    } else if (state->current_view == VIEW_PANE) {
                        // Handle slider dragging in pane views
                        needs_repaint = paneview_handle_motion(state,
                                            evt.xmotion.x, evt.xmotion.y);
                    }

                    if (needs_repaint) {
                        sysprefs_paint(state);
                    }
                    break;
                }

                case ButtonPress:
                    // Check toolbar first
                    if (evt.xbutton.y < TOOLBAR_HEIGHT) {
                        toolbar_handle_click(state,
                                             evt.xbutton.x, evt.xbutton.y);
                    } else if (state->current_view == VIEW_GRID) {
                        icongrid_handle_click(state,
                                              evt.xbutton.x, evt.xbutton.y);
                    } else {
                        if (paneview_handle_click(state,
                                                  evt.xbutton.x, evt.xbutton.y)) {
                            sysprefs_paint(state);
                        }
                    }
                    break;

                case ButtonRelease:
                    // Commit slider values on mouse release
                    if (state->current_view == VIEW_PANE) {
                        paneview_handle_release(state);
                        sysprefs_paint(state);
                    }
                    break;

                case LeaveNotify:
                    // Clear all hover states when mouse leaves the window
                    if (state->hover_pane != -1 || state->toolbar_hover != -1) {
                        state->hover_pane = -1;
                        state->toolbar_hover = -1;
                        sysprefs_paint(state);
                    }
                    break;

                case ClientMessage:
                    // Check for WM_DELETE_WINDOW (user clicked the close button)
                    if ((Atom)evt.xclient.data.l[0] == wm_delete) {
                        state->running = false;
                    }
                    break;

                default:
                    break;
            }
        }

        // Wait for the next event (100ms timeout for responsive hover)
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(x_fd, &fds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;  // 100ms

        select(x_fd + 1, &fds, NULL, NULL, &tv);
    }
}

// ============================================================================
// sysprefs_cleanup — Release all resources
// ============================================================================
void sysprefs_cleanup(SysPrefsState *state)
{
    // Free pane icon surfaces
    for (int i = 0; i < state->pane_count; i++) {
        if (state->panes[i].icon_32) {
            cairo_surface_destroy(state->panes[i].icon_32);
        }
        if (state->panes[i].icon_128) {
            cairo_surface_destroy(state->panes[i].icon_128);
        }
    }

    // Destroy Cairo context and surface
    if (state->cr) {
        cairo_destroy(state->cr);
    }
    if (state->surface) {
        cairo_surface_destroy(state->surface);
    }

    // Close X11 connection
    if (state->dpy) {
        XDestroyWindow(state->dpy, state->win);
        XCloseDisplay(state->dpy);
    }

    fprintf(stderr, "[systemcontrol] Resources cleaned up\n");
}
