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
#include <X11/keysym.h>

#include "menubar.h"
#include "render.h"
#include "apple.h"
#include "appmenu.h"
#include "systray.h"

// ── Forward declarations ────────────────────────────────────────────
static void dismiss_open_menu(MenuBar *mb);
static int  hit_test_menu(MenuBar *mb, int mx);
static void grab_pointer(MenuBar *mb);
static void ungrab_pointer(MenuBar *mb);

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
        fprintf(stderr, "cc-menubar: cannot open X display\n");
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
    mb->atom_net_active_window       = XInternAtom(mb->dpy, "_NET_ACTIVE_WINDOW", False);
    mb->atom_net_wm_window_type      = XInternAtom(mb->dpy, "_NET_WM_WINDOW_TYPE", False);
    mb->atom_net_wm_window_type_dock = XInternAtom(mb->dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    mb->atom_net_wm_strut            = XInternAtom(mb->dpy, "_NET_WM_STRUT", False);
    mb->atom_net_wm_strut_partial    = XInternAtom(mb->dpy, "_NET_WM_STRUT_PARTIAL", False);
    mb->atom_wm_class                = XInternAtom(mb->dpy, "WM_CLASS", False);
    mb->atom_utf8_string             = XInternAtom(mb->dpy, "UTF8_STRING", False);

    // ── Find 32-bit ARGB visual for translucency ─────────────────
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

    if (!visual) {
        visual = DefaultVisual(mb->dpy, mb->screen);
        depth = DefaultDepth(mb->dpy, mb->screen);
        colormap = DefaultColormap(mb->dpy, mb->screen);
    }

    // ── Create the menu bar window ──────────────────────────────
    XSetWindowAttributes attrs;
    attrs.override_redirect = False;
    attrs.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask
                     | LeaveWindowMask | StructureNotifyMask | KeyPressMask;
    attrs.background_pixel = 0;
    attrs.colormap = colormap;
    attrs.border_pixel = 0;

    mb->win = XCreateWindow(
        mb->dpy, mb->root,
        0, 0,
        (unsigned int)mb->screen_w,
        MENUBAR_HEIGHT,
        0,
        depth,
        InputOutput,
        visual,
        CWOverrideRedirect | CWEventMask | CWBackPixel | CWColormap | CWBorderPixel,
        &attrs
    );

    XSetWindowBackgroundPixmap(mb->dpy, mb->win, None);

    // ── Set window type to DOCK ─────────────────────────────────
    Atom dock_type = mb->atom_net_wm_window_type_dock;
    XChangeProperty(mb->dpy, mb->win,
                    mb->atom_net_wm_window_type, XA_ATOM,
                    32, PropModeReplace,
                    (unsigned char *)&dock_type, 1);

    // ── Reserve screen space with struts ────────────────────────
    long strut_partial[12] = {
        0, 0, MENUBAR_HEIGHT, 0,
        0, 0, 0, 0,
        0, mb->screen_w, 0, 0
    };
    XChangeProperty(mb->dpy, mb->win,
                    mb->atom_net_wm_strut_partial, XA_CARDINAL,
                    32, PropModeReplace,
                    (unsigned char *)strut_partial, 12);

    long strut[4] = { 0, 0, MENUBAR_HEIGHT, 0 };
    XChangeProperty(mb->dpy, mb->win,
                    mb->atom_net_wm_strut, XA_CARDINAL,
                    32, PropModeReplace,
                    (unsigned char *)strut, 4);

    // ── Watch root window for active window changes ─────────────
    XSelectInput(mb->dpy, mb->root, PropertyChangeMask);

    // ── Map (show) the window ───────────────────────────────────
    XMapWindow(mb->dpy, mb->win);
    XFlush(mb->dpy);

    // ── Compute layout regions ──────────────────────────────────
    mb->apple_x = 0;
    mb->apple_w = 50;

    mb->appname_x = 58;
    mb->appname_w = 0;

    mb->menus_x = 0;

    // ── Initialize subsystems ───────────────────────────────────
    render_init(mb);
    apple_init(mb);
    appmenu_init(mb);
    systray_init(mb);

    // ── Set initial state ───────────────────────────────────────
    strncpy(mb->active_app, "Finder", sizeof(mb->active_app) - 1);
    strncpy(mb->active_class, "dolphin", sizeof(mb->active_class) - 1);

    mb->running = true;

    fprintf(stdout, "cc-menubar: initialized (%dx%d screen)\n",
            mb->screen_w, mb->screen_h);

    return true;
}

// ── Pointer grab helpers ────────────────────────────────────────────
// We grab the pointer when a dropdown is open so we can detect clicks
// outside the menu bar to dismiss the dropdown. The grab is on the
// root window so we get events in screen (root) coordinates.

static void grab_pointer(MenuBar *mb)
{
    XGrabPointer(mb->dpy, mb->root, True,
                 ButtonPressMask | PointerMotionMask | ButtonReleaseMask,
                 GrabModeAsync, GrabModeAsync,
                 None, None, CurrentTime);
}

static void ungrab_pointer(MenuBar *mb)
{
    XUngrabPointer(mb->dpy, CurrentTime);
    XFlush(mb->dpy);
}

// ── Helper: dismiss whichever menu is currently open ────────────────

static void dismiss_open_menu(MenuBar *mb)
{
    if (mb->open_menu == 0) {
        apple_dismiss(mb);
    } else if (mb->open_menu > 0) {
        appmenu_dismiss(mb);
    }
    mb->open_menu = -1;
    ungrab_pointer(mb);
}

// ── Helper: figure out which menu title was clicked ─────────────────
// Returns: -1 = nothing, 0 = Apple logo, 1+ = menu title index

static int hit_test_menu(MenuBar *mb, int mx)
{
    // Check Apple logo region
    if (mx >= mb->apple_x && mx < mb->apple_x + mb->apple_w) {
        return 0;
    }

    // Check each menu title region
    const char **menus;
    int menu_count;
    appmenu_get_menus(mb->active_class, &menus, &menu_count);

    int item_x = mb->menus_x;
    for (int i = 0; i < menu_count; i++) {
        double w = render_measure_text(menus[i], false);
        int item_w = (int)w + 20;
        if (mx >= item_x && mx < item_x + item_w) {
            return i + 1;
        }
        item_x += item_w;
    }

    return -1;
}

// ── Helper: open a specific menu by index ───────────────────────────
// index 0 = Apple, 1+ = app menus.

static void open_menu_at(MenuBar *mb, int index)
{
    mb->open_menu = index;

    if (index == 0) {
        apple_show_menu(mb);
    } else {
        // Compute the x position for this menu's dropdown
        const char **menus;
        int count;
        appmenu_get_menus(mb->active_class, &menus, &count);

        int dx = mb->menus_x;
        for (int j = 0; j < index - 1 && j < count; j++) {
            dx += (int)render_measure_text(menus[j], false) + 20;
        }
        appmenu_show_dropdown(mb, index - 1, dx);
    }

    menubar_paint(mb);
}

// ── Event Loop ──────────────────────────────────────────────────────

void menubar_run(MenuBar *mb)
{
    int x11_fd = ConnectionNumber(mb->dpy);
    time_t last_clock_check = 0;
    time_t last_systray_update = 0;

    while (mb->running) {
        // ── Handle all pending X events ─────────────────────────
        while (XPending(mb->dpy)) {
            XEvent ev;
            XNextEvent(mb->dpy, &ev);

            // ── Route events to the dropdown if it's open ───────
            if (mb->open_menu > 0) {
                bool should_dismiss = false;
                if (appmenu_handle_dropdown_event(mb, &ev, &should_dismiss)) {
                    if (should_dismiss) {
                        dismiss_open_menu(mb);
                        menubar_paint(mb);
                    }
                    continue;
                }
            }

            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) {
                    menubar_paint(mb);
                }
                break;

            case MotionNotify: {
                // When a menu is open, the pointer is grabbed on root,
                // so we get root coordinates. Convert to menubar-relative.
                int mx, my;
                if (mb->open_menu >= 0) {
                    // Grabbed on root — coordinates are screen/root coords
                    mx = ev.xmotion.x_root;
                    my = ev.xmotion.y_root;
                } else {
                    // Not grabbed — coordinates are relative to mb->win
                    mx = ev.xmotion.x;
                    my = ev.xmotion.y;
                }

                // Only process hover changes when mouse is in the bar
                if (my < 0 || my >= MENUBAR_HEIGHT) {
                    // Mouse is below the bar. If we had a hover, clear it
                    // but don't close the dropdown (user might be in it).
                    if (mb->hover_index != -1) {
                        mb->hover_index = -1;
                        menubar_paint(mb);
                    }
                    break;
                }

                int old_hover = mb->hover_index;
                int new_hover = hit_test_menu(mb, mx);

                if (new_hover != old_hover) {
                    mb->hover_index = new_hover;

                    // If a menu is already open and we hover a different
                    // title, switch to that menu (menu bar scrubbing).
                    if (mb->open_menu >= 0 && new_hover >= 0 &&
                        new_hover != mb->open_menu) {
                        // Dismiss the old dropdown (keep the grab!)
                        if (mb->open_menu == 0) {
                            apple_dismiss(mb);
                        } else {
                            appmenu_dismiss(mb);
                        }

                        // Open the new one (without re-grabbing)
                        open_menu_at(mb, new_hover);
                    }

                    menubar_paint(mb);
                }
                break;
            }

            case ButtonPress: {
                // When pointer is grabbed on root, ButtonPress coords
                // are in root/screen coordinates.
                int mx, my;
                if (mb->open_menu >= 0) {
                    mx = ev.xbutton.x_root;
                    my = ev.xbutton.y_root;
                } else {
                    mx = ev.xbutton.x;
                    my = ev.xbutton.y;
                }


                if (mb->open_menu >= 0) {
                    // A menu is currently open.

                    // Check if click is within the menu bar
                    if (my >= 0 && my < MENUBAR_HEIGHT) {
                        int clicked = hit_test_menu(mb, mx);

                        if (clicked == mb->open_menu) {
                            // Clicked the same menu title — toggle it closed
                            dismiss_open_menu(mb);
                            menubar_paint(mb);
                        } else if (clicked >= 0) {
                            // Clicked a different menu title — switch to it.
                            // Dismiss old dropdown but keep the pointer grab.
                            if (mb->open_menu == 0) {
                                apple_dismiss(mb);
                            } else {
                                appmenu_dismiss(mb);
                            }
                            open_menu_at(mb, clicked);
                        } else {
                            // Clicked empty space in the menu bar — dismiss
                            dismiss_open_menu(mb);
                            menubar_paint(mb);
                        }
                    } else {
                        // Clicked outside the menu bar entirely.
                        // Check if click is in the dropdown window.
                        // If not, dismiss everything.
                        Window dropdown = appmenu_get_dropdown_win();
                        if (dropdown != None) {
                            // Get dropdown geometry to check if click is inside
                            XWindowAttributes dwa;
                            XGetWindowAttributes(mb->dpy, dropdown, &dwa);
                            int dx = dwa.x, dy = dwa.y;
                            int dw = dwa.width, dh = dwa.height;

                            if (mx >= dx && mx < dx + dw &&
                                my >= dy && my < dy + dh) {
                                // Click is inside the dropdown — let it handle it.
                                // Re-dispatch as a synthetic event to the dropdown.
                                XEvent synth = ev;
                                synth.xbutton.window = dropdown;
                                synth.xbutton.x = mx - dx;
                                synth.xbutton.y = my - dy;
                                XSendEvent(mb->dpy, dropdown, True, ButtonPressMask, &synth);
                                XFlush(mb->dpy);
                                break;
                            }
                        }
                        // Click is truly outside — dismiss
                        dismiss_open_menu(mb);
                        menubar_paint(mb);
                    }
                    break;
                }

                // No menu is open — check if a menu title was clicked
                int clicked = hit_test_menu(mb, mx);
                if (clicked >= 0) {
                    grab_pointer(mb);
                    open_menu_at(mb, clicked);
                }
                break;
            }

            case LeaveNotify:
                // Mouse left the menu bar window — clear hover
                if (mb->open_menu < 0 && mb->hover_index != -1) {
                    mb->hover_index = -1;
                    menubar_paint(mb);
                }
                break;

            case PropertyNotify:
                if (ev.xproperty.atom == mb->atom_net_active_window) {
                    if (mb->open_menu >= 0) {
                        dismiss_open_menu(mb);
                    }
                    appmenu_update_active(mb);
                    menubar_paint(mb);
                }
                break;

            case KeyPress: {
                KeySym sym = XLookupKeysym(&ev.xkey, 0);
                if (sym == XK_Escape && mb->open_menu >= 0) {
                    dismiss_open_menu(mb);
                    menubar_paint(mb);
                }
                break;
            }

            default:
                break;
            }
        }

        // ── Periodic updates (clock, systray) ───────────────────
        time_t now = time(NULL);

        if (now != last_clock_check) {
            last_clock_check = now;
            menubar_paint(mb);
        }

        if (now - last_systray_update >= 10) {
            last_systray_update = now;
            systray_update(mb);
        }

        // ── Wait for next event or timeout ──────────────────────
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(x11_fd, &fds);

        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 500000;

        select(x11_fd + 1, &fds, NULL, NULL, &tv);
    }
}

// ── Painting ────────────────────────────────────────────────────────

void menubar_paint(MenuBar *mb)
{
    XWindowAttributes wa;
    XGetWindowAttributes(mb->dpy, mb->win, &wa);
    cairo_surface_t *surface = cairo_xlib_surface_create(
        mb->dpy, mb->win,
        wa.visual,
        mb->screen_w, MENUBAR_HEIGHT
    );
    cairo_t *cr = cairo_create(surface);

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // ── Background ──────────────────────────────────────────────
    render_background(mb, cr);

    // ── Apple logo (far left) ───────────────────────────────────
    apple_paint(mb, cr);

    // ── Bold app name ───────────────────────────────────────────
    double appname_w = render_text(cr, mb->active_app,
                                   mb->appname_x, 3,
                                   true, 0.1, 0.1, 0.1);

    // ── Menu titles ─────────────────────────────────────────────
    const char **menus;
    int menu_count;
    appmenu_get_menus(mb->active_class, &menus, &menu_count);

    mb->menus_x = mb->appname_x + (int)appname_w + 16;

    int item_x = mb->menus_x;
    for (int i = 0; i < menu_count; i++) {
        double w = render_measure_text(menus[i], false);
        int item_w = (int)w + 20;

        if (mb->hover_index == i + 1 || mb->open_menu == i + 1) {
            render_hover_highlight(cr, item_x, 1, item_w, MENUBAR_HEIGHT - 2);
        }

        render_text(cr, menus[i],
                    item_x + 10, 3,
                    false, 0.1, 0.1, 0.1);

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
    XFlush(mb->dpy);
}

// ── Shutdown ────────────────────────────────────────────────────────

void menubar_shutdown(MenuBar *mb)
{
    systray_cleanup();
    appmenu_cleanup();
    apple_cleanup();
    render_cleanup();

    if (mb->win) {
        XDestroyWindow(mb->dpy, mb->win);
    }
    if (mb->dpy) {
        XCloseDisplay(mb->dpy);
    }

    fprintf(stdout, "cc-menubar: shut down cleanly\n");
}
