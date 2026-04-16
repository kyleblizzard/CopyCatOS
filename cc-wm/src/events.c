// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// CopiCatOS Window Manager — X11 event dispatch
// Uses function pointer array for O(1) dispatch (dwm pattern)

#include "events.h"
#include "frame.h"
#include "ewmh.h"
#include "input.h"
#include "moonrock.h"
#include "struts.h"
#include "resize.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

// Forward declarations for event handlers
static void on_map_request(CCWM *wm, XEvent *e);
static void on_unmap_notify(CCWM *wm, XEvent *e);
static void on_destroy_notify(CCWM *wm, XEvent *e);
static void on_configure_request(CCWM *wm, XEvent *e);
static void on_button_press(CCWM *wm, XEvent *e);
static void on_button_release(CCWM *wm, XEvent *e);
static void on_motion_notify(CCWM *wm, XEvent *e);
static void on_property_notify(CCWM *wm, XEvent *e);
static void on_client_message(CCWM *wm, XEvent *e);
static void on_focus_in(CCWM *wm, XEvent *e);
static void on_key_press(CCWM *wm, XEvent *e);
static void on_expose(CCWM *wm, XEvent *e);
static void on_leave_notify(CCWM *wm, XEvent *e);

// Event handler function type
typedef void (*EventHandler)(CCWM *wm, XEvent *e);

// Dispatch table — indexed by event type for O(1) lookup
static EventHandler handlers[LASTEvent] = {0};

static void init_handlers(void)
{
    handlers[MapRequest]       = on_map_request;
    handlers[UnmapNotify]      = on_unmap_notify;
    handlers[DestroyNotify]    = on_destroy_notify;
    handlers[ConfigureRequest] = on_configure_request;
    handlers[ButtonPress]      = on_button_press;
    handlers[ButtonRelease]    = on_button_release;
    handlers[MotionNotify]     = on_motion_notify;
    handlers[PropertyNotify]   = on_property_notify;
    handlers[ClientMessage]    = on_client_message;
    handlers[FocusIn]          = on_focus_in;
    handlers[KeyPress]         = on_key_press;
    handlers[Expose]           = on_expose;
    handlers[LeaveNotify]      = on_leave_notify;
}

void events_run(CCWM *wm)
{
    init_handlers();

    // Frame any pre-existing windows
    frame_existing_windows(wm);

    fprintf(stderr, "[cc-wm] Entering event loop\n");

    // Track when we last sent a ping and checked timeouts.
    // We use XPending + select() with a timeout instead of blocking
    // on XNextEvent, so we can periodically check ping timeouts.
    struct timespec last_ping = {0};

    while (wm->running) {
        // ── Periodic ping/timeout check ──
        // Every PING_INTERVAL_MS, send a ping to the focused window.
        // Every loop iteration, check for ping timeouts (beach ball).
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long since_ping = (now.tv_sec - last_ping.tv_sec) * 1000 +
                              (now.tv_nsec - last_ping.tv_nsec) / 1000000;

            if (since_ping >= PING_INTERVAL_MS && wm->focused) {
                ewmh_send_ping(wm, wm->focused);
                last_ping = now;
            }

            // Check all clients for ping timeouts
            ewmh_check_ping_timeouts(wm);
        }

        // Process all pending X events (non-blocking drain).
        // We use XPending instead of blocking XNextEvent so the ping
        // timeout checks above run even when no X events arrive.
        while (XPending(wm->dpy) > 0) {
            XEvent ev;
            XNextEvent(wm->dpy, &ev);

            // Let MoonRock handle extension events (XDamage, etc.) first.
            // mr_handle_event() returns true if it consumed the event.
            if (mr_handle_event(wm, &ev)) {
                // MoonRock handled this event (e.g., DamageNotify) — skip
                // normal dispatch so we don't process it twice.
            } else if (ev.type < LASTEvent && handlers[ev.type]) {
                handlers[ev.type](wm, &ev);
            }
        }

        // Composite all windows onto the screen. This is the MoonRock
        // compositor's main render pass — it draws every window with
        // shadows and effects via OpenGL.
        if (mr_is_active()) {
            mr_composite(wm);
        }

        // Brief sleep to avoid busy-spinning when no events pending.
        // 16ms ≈ 60Hz — enough to keep ping checks responsive without
        // burning CPU. The compositing pass above handles display refresh.
        if (XPending(wm->dpy) == 0) {
            struct timespec ts = {0, 16000000}; // 16ms
            nanosleep(&ts, NULL);
        }
    }
}

// ─── Event Handlers ────────────────────────────────────────────

static void on_map_request(CCWM *wm, XEvent *e)
{
    Window w = e->xmaprequest.window;

    // Check if we already manage this window
    if (wm_find_client(wm, w)) {
        XMapWindow(wm->dpy, w);
        return;
    }

    // Get window type — docks, desktops, and splash screens get special treatment
    Atom type = ewmh_get_window_type(wm, w);

    if (type == wm->atom_net_wm_type_dock ||
        type == wm->atom_net_wm_type_desktop) {
        // Don't frame dock or desktop windows — just map them
        XMapWindow(wm->dpy, w);
        // Dock windows may have struts — recalculate work area.
        // Also watch for PropertyNotify so we catch strut changes
        // (e.g., when the menubar resizes via System Preferences).
        if (type == wm->atom_net_wm_type_dock) {
            XSelectInput(wm->dpy, w, PropertyChangeMask | StructureNotifyMask);
            struts_recalculate(wm);
        }
        return;
    }

    // Frame the window and map it
    frame_window(wm, w);
}

static void on_unmap_notify(CCWM *wm, XEvent *e)
{
    Window w = e->xunmap.window;

    // Ignore unmaps from reparenting (we caused them)
    if (e->xunmap.event == wm->root) return;

    Client *c = wm_find_client(wm, w);
    if (c) {
        unframe_window(wm, c);
    }
}

static void on_destroy_notify(CCWM *wm, XEvent *e)
{
    Window w = e->xdestroywindow.window;
    Client *c = wm_find_client(wm, w);
    if (c) {
        // Tell MoonRock to release the texture and damage tracking for this
        // window BEFORE we destroy the frame. MoonRock looks up the window
        // by its frame ID, so the frame must still exist at this point.
        if (mr_is_active()) {
            mr_window_unmapped(wm, c);
        }

        // Client destroyed itself — clean up frame
        if (c->frame) {
            XDestroyWindow(wm->dpy, c->frame);
            c->frame = 0;
        }
        wm_remove_client(wm, c);
        ewmh_update_client_list(wm);
    }
}

static void on_configure_request(CCWM *wm, XEvent *e)
{
    XConfigureRequestEvent *cr = &e->xconfigurerequest;

    Client *c = wm_find_client(wm, cr->window);
    if (c && c->frame) {
        // Managed window — apply the configuration to both frame and client
        if (cr->value_mask & (CWX | CWY)) {
            c->x = cr->x;
            c->y = cr->y;
        }
        if (cr->value_mask & CWWidth) c->w = cr->width;
        if (cr->value_mask & CWHeight) c->h = cr->height;

        // Shadow padding (0 if compositor is off)
        int sl = compositor_active ? SHADOW_LEFT : 0;
        int sr = compositor_active ? SHADOW_RIGHT : 0;
        int st = compositor_active ? SHADOW_TOP : 0;
        int sb = compositor_active ? SHADOW_BOTTOM : 0;

        // Move/resize the frame (includes shadow padding)
        XMoveResizeWindow(wm->dpy, c->frame,
                          c->x, c->y,
                          c->w + 2 * BORDER_WIDTH + sl + sr,
                          c->h + TITLEBAR_HEIGHT + BORDER_WIDTH + st + sb);

        // Resize the client within the frame (offset by shadow + chrome)
        XMoveResizeWindow(wm->dpy, c->client,
                          sl + BORDER_WIDTH, st + TITLEBAR_HEIGHT,
                          c->w, c->h);
    } else {
        // Unmanaged window — pass through the configuration
        XWindowChanges changes = {
            .x = cr->x, .y = cr->y,
            .width = cr->width, .height = cr->height,
            .border_width = cr->border_width,
            .sibling = cr->above,
            .stack_mode = cr->detail,
        };
        XConfigureWindow(wm->dpy, cr->window, cr->value_mask, &changes);
    }
}

// ── Traffic light button hit testing ──
// Returns which button (1=close, 2=minimize, 3=zoom) the point (fx,fy)
// falls within, or 0 if it's not on any button. The coordinates are
// relative to the chrome origin (after shadow offset adjustment).
static int hit_test_button(int fx, int fy)
{
    // Buttons are only in the title bar region
    if (fy < BUTTON_TOP_PAD || fy > BUTTON_TOP_PAD + BUTTON_DIAMETER)
        return 0;

    int bx = BUTTON_LEFT_PAD;

    // Close button
    if (fx >= bx && fx <= bx + BUTTON_DIAMETER) return 1;
    bx += BUTTON_DIAMETER + BUTTON_SPACING;

    // Minimize button
    if (fx >= bx && fx <= bx + BUTTON_DIAMETER) return 2;
    bx += BUTTON_DIAMETER + BUTTON_SPACING;

    // Zoom button
    if (fx >= bx && fx <= bx + BUTTON_DIAMETER) return 3;

    return 0;
}

// Returns true if the point is within the broader "button region" — the
// bounding box that contains all three traffic light buttons. In real
// Snow Leopard, hovering anywhere in this region reveals glyphs on ALL
// three buttons simultaneously.
static bool in_button_region(int fx, int fy)
{
    if (fy < BUTTON_TOP_PAD || fy > BUTTON_TOP_PAD + BUTTON_DIAMETER)
        return false;

    int region_left = BUTTON_LEFT_PAD;
    int region_right = BUTTON_LEFT_PAD + 3 * BUTTON_DIAMETER + 2 * BUTTON_SPACING;

    return fx >= region_left && fx <= region_right;
}

static void on_button_press(CCWM *wm, XEvent *e)
{
    Window w = e->xbutton.window;
    Client *c = wm_find_client_by_frame(wm, w);

    if (!c) return;

    // Focus on click
    wm_focus_client(wm, c);

    int fx = e->xbutton.x;  // Click position relative to frame
    int fy = e->xbutton.y;

    // Adjust for shadow offset when compositor is active — button
    // positions in decor.c are relative to the chrome origin, not
    // the frame origin which includes shadow padding.
    int sl = compositor_active ? SHADOW_LEFT : 0;
    int st = compositor_active ? SHADOW_TOP : 0;
    int cx = fx - sl;  // Chrome-relative x
    int cy = fy - st;  // Chrome-relative y

    // Check if click is in the title bar area
    if (cy >= 0 && cy < TITLEBAR_HEIGHT) {
        int btn = hit_test_button(cx, cy);

        if (btn > 0) {
            // Set pressed state and redraw to show pressed visual
            wm->pressed_button = btn;
            wm->hover_client = c;
            wm->buttons_hover = true;
            frame_redraw_decor(wm, c);

            // Grab pointer so we get the release even if mouse moves off
            XGrabPointer(wm->dpy, c->frame, True,
                         PointerMotionMask | ButtonReleaseMask,
                         GrabModeAsync, GrabModeAsync,
                         None, None, CurrentTime);
            return;
        }

        // Click on title bar (not on a button) — begin drag
        wm->dragging = true;
        wm->drag_client = c;
        wm->drag_start_x = e->xbutton.x_root;
        wm->drag_start_y = e->xbutton.y_root;
        wm->drag_frame_x = c->x;
        wm->drag_frame_y = c->y;

        XGrabPointer(wm->dpy, c->frame, True,
                     PointerMotionMask | ButtonReleaseMask,
                     GrabModeAsync, GrabModeAsync,
                     None, None, CurrentTime);
        return;
    }

    // Click on border — detect edge/corner and begin resize
    ResizeDir dir = resize_detect_edge(c, fx, fy);
    if (dir != RESIZE_NONE) {
        resize_begin(wm, c, e->xbutton.x_root, e->xbutton.y_root, dir);
    }
}

static void on_button_release(CCWM *wm, XEvent *e)
{
    if (wm->pressed_button > 0 && wm->hover_client) {
        // A traffic light button was pressed — execute the action if the
        // mouse is still over the same button (click-and-release pattern).
        // This matches real Snow Leopard: pressing highlights the button,
        // dragging off cancels, releasing on the button fires the action.
        Client *c = wm->hover_client;
        int fx = e->xbutton.x;
        int fy = e->xbutton.y;
        int sl = compositor_active ? SHADOW_LEFT : 0;
        int st = compositor_active ? SHADOW_TOP : 0;
        int btn = hit_test_button(fx - sl, fy - st);

        int pressed = wm->pressed_button;
        wm->pressed_button = 0;

        XUngrabPointer(wm->dpy, CurrentTime);
        frame_redraw_decor(wm, c);

        // Only fire the action if released on the same button that was pressed
        if (btn == pressed) {
            switch (btn) {
            case 1: // Close
                ewmh_send_delete(wm, c->client);
                break;
            case 2: // Minimize
                if (mr_is_active()) {
                    int dock_x = wm->root_w / 2;
                    int dock_y = wm->root_h - 48;
                    mr_animate_minimize(wm, c, dock_x, dock_y);
                } else {
                    XUnmapWindow(wm->dpy, c->frame);
                    c->mapped = false;
                }
                ewmh_update_client_list(wm);
                break;
            case 3: { // Zoom
                extern void client_smart_zoom(CCWM *wm, Client *c);
                client_smart_zoom(wm, c);
                frame_redraw_decor(wm, c);
                break;
            }
            }
        }
        return;
    }

    if (wm->resizing) {
        resize_end(wm);
    } else if (wm->dragging) {
        XUngrabPointer(wm->dpy, CurrentTime);
        wm->dragging = false;
        wm->drag_client = NULL;
    }
}

static void on_motion_notify(CCWM *wm, XEvent *e)
{
    if (wm->resizing) {
        // Active resize — update window geometry based on drag delta
        resize_update(wm, e->xmotion.x_root, e->xmotion.y_root);
    } else if (wm->pressed_button > 0 && wm->hover_client) {
        // Button is pressed — track whether mouse is still over it.
        // During a button press, the glyphs stay visible on all three
        // buttons. The pressed darkening only shows when the mouse is
        // over the originally-pressed button (drag off = un-darken,
        // drag back = re-darken). This matches real Snow Leopard.
        //
        // We don't need to update pressed_button here — decor.c checks
        // hit_test_button at paint time via the pressed state. The
        // redraw will show/hide the darkening based on current mouse
        // position vs the pressed_button value. Since decor.c already
        // has the pressed_button, we just trigger a redraw.
        frame_redraw_decor(wm, wm->hover_client);
    } else if (wm->dragging && wm->drag_client) {
        // Title bar drag — move the window
        int dx = e->xmotion.x_root - wm->drag_start_x;
        int dy = e->xmotion.y_root - wm->drag_start_y;

        Client *c = wm->drag_client;
        c->x = wm->drag_frame_x + dx;
        c->y = wm->drag_frame_y + dy;

        XMoveWindow(wm->dpy, c->frame, c->x, c->y);
    } else {
        // Not dragging or resizing — check for button hover and
        // update cursor shape for resize edges
        Client *c = wm_find_client_by_frame(wm, e->xmotion.window);
        if (c) {
            int fx = e->xmotion.x;
            int fy = e->xmotion.y;
            int sl = compositor_active ? SHADOW_LEFT : 0;
            int st = compositor_active ? SHADOW_TOP : 0;
            int cx = fx - sl;
            int cy = fy - st;

            // Check if mouse is in the button region of the title bar
            bool hovering = c->focused && in_button_region(cx, cy);

            if (hovering && !wm->buttons_hover) {
                // Mouse just entered the button region — show glyphs
                wm->buttons_hover = true;
                wm->hover_client = c;
                frame_redraw_decor(wm, c);
            } else if (!hovering && wm->buttons_hover && wm->hover_client == c) {
                // Mouse left the button region — hide glyphs
                wm->buttons_hover = false;
                wm->hover_client = NULL;
                frame_redraw_decor(wm, c);
            }

            resize_update_cursor(wm, c, fx, fy);
        }
    }
}

static void on_property_notify(CCWM *wm, XEvent *e)
{
    Window w = e->xproperty.window;
    Client *c = wm_find_client(wm, w);

    if (c && (e->xproperty.atom == wm->atom_net_wm_name ||
              e->xproperty.atom == wm->atom_wm_name)) {
        // Title changed — update and redraw decoration
        ewmh_get_title(wm, c->client, c->title, sizeof(c->title));
        // Detect unsaved changes from title prefix — GTK, Qt, and most
        // Linux apps prefix the title with '*' or '•' when a document
        // has been modified. The WM shows a dot on the close button.
        c->unsaved = (c->title[0] == '*' ||
                      (unsigned char)c->title[0] == 0xE2); // UTF-8 '•' starts with 0xE2
        if (c->frame) {
            frame_redraw_decor(wm, c);
        }
    }

    // Strut properties changed on any window — recalculate work area
    if (e->xproperty.atom == wm->atom_net_wm_strut ||
        e->xproperty.atom == wm->atom_net_wm_strut_partial) {
        struts_recalculate(wm);
    }
}

static void on_client_message(CCWM *wm, XEvent *e)
{
    XClientMessageEvent *cm = &e->xclient;

    // Check for _NET_WM_PING response (pong) first — these are sent
    // to the root window by apps that received our ping. If we get one,
    // the app is responsive and we can clear the beach ball.
    if (ewmh_handle_pong(wm, cm)) {
        return; // Pong handled
    }

    if (cm->message_type == wm->atom_net_active_window) {
        // Request to activate a window
        Client *c = wm_find_client(wm, cm->window);
        if (c) {
            if (c->frame) XMapWindow(wm->dpy, c->frame);
            c->mapped = true;
            wm_focus_client(wm, c);
        }
    } else if (cm->message_type == wm->atom_net_close_window) {
        ewmh_send_delete(wm, cm->window);
    }
}

static void on_focus_in(CCWM *wm, XEvent *e)
{
    Window w = e->xfocus.window;
    Client *c = wm_find_client(wm, w);
    if (c && c != wm->focused) {
        wm_focus_client(wm, c);
        frame_redraw_decor(wm, c);
    }
}

static void on_key_press(CCWM *wm, XEvent *e)
{
    input_handle_key(wm, &e->xkey);
}

static void on_expose(CCWM *wm, XEvent *e)
{
    if (e->xexpose.count > 0) return; // Only redraw on last expose

    Client *c = wm_find_client_by_frame(wm, e->xexpose.window);
    if (c) {
        frame_redraw_decor(wm, c);

        // Tell MoonRock the window contents changed so it refreshes the
        // OpenGL texture on the next composite pass. Expose events mean
        // the window (or its decorations) were repainted.
        if (mr_is_active()) {
            mr_window_damaged(wm, c);
        }
    }
}

static void on_leave_notify(CCWM *wm, XEvent *e)
{
    // Mouse left a frame window — clear button hover state so the
    // glyphs disappear. This handles the case where the mouse moves
    // directly from the button region to outside the window entirely
    // (skipping the intermediate "still in frame but not on buttons" state).
    if (wm->buttons_hover) {
        Client *c = wm_find_client_by_frame(wm, e->xcrossing.window);
        if (c && c == wm->hover_client) {
            wm->buttons_hover = false;
            wm->hover_client = NULL;
            frame_redraw_decor(wm, c);
        }
    }
}
