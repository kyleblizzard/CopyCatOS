// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// AuraOS Window Manager — X11 event dispatch
// Uses function pointer array for O(1) dispatch (dwm pattern)

#include "events.h"
#include "frame.h"
#include "ewmh.h"
#include "input.h"
#include "crystal.h"
#include "struts.h"
#include "resize.h"
#include <stdio.h>
#include <string.h>

// Forward declarations for event handlers
static void on_map_request(AuraWM *wm, XEvent *e);
static void on_unmap_notify(AuraWM *wm, XEvent *e);
static void on_destroy_notify(AuraWM *wm, XEvent *e);
static void on_configure_request(AuraWM *wm, XEvent *e);
static void on_button_press(AuraWM *wm, XEvent *e);
static void on_button_release(AuraWM *wm, XEvent *e);
static void on_motion_notify(AuraWM *wm, XEvent *e);
static void on_property_notify(AuraWM *wm, XEvent *e);
static void on_client_message(AuraWM *wm, XEvent *e);
static void on_focus_in(AuraWM *wm, XEvent *e);
static void on_key_press(AuraWM *wm, XEvent *e);
static void on_expose(AuraWM *wm, XEvent *e);

// Event handler function type
typedef void (*EventHandler)(AuraWM *wm, XEvent *e);

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
}

void events_run(AuraWM *wm)
{
    init_handlers();

    // Frame any pre-existing windows
    frame_existing_windows(wm);

    fprintf(stderr, "[aura-wm] Entering event loop\n");

    while (wm->running) {
        XEvent ev;
        XNextEvent(wm->dpy, &ev);

        // Let Crystal handle extension events (XDamage, etc.) first.
        // crystal_handle_event() returns true if it consumed the event.
        if (crystal_handle_event(wm, &ev)) {
            // Crystal handled this event (e.g., DamageNotify) — skip
            // normal dispatch so we don't process it twice.
        } else if (ev.type < LASTEvent && handlers[ev.type]) {
            handlers[ev.type](wm, &ev);
        }

        // Composite all windows onto the screen. This is the Crystal
        // compositor's main render pass — it draws every window with
        // shadows and effects via OpenGL.
        if (crystal_is_active()) {
            crystal_composite(wm);
        }
    }
}

// ─── Event Handlers ────────────────────────────────────────────

static void on_map_request(AuraWM *wm, XEvent *e)
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
        // Dock windows may have struts — recalculate work area
        if (type == wm->atom_net_wm_type_dock) {
            struts_recalculate(wm);
        }
        return;
    }

    // Frame the window and map it
    frame_window(wm, w);
}

static void on_unmap_notify(AuraWM *wm, XEvent *e)
{
    Window w = e->xunmap.window;

    // Ignore unmaps from reparenting (we caused them)
    if (e->xunmap.event == wm->root) return;

    Client *c = wm_find_client(wm, w);
    if (c) {
        unframe_window(wm, c);
    }

    // A window was hidden — tell the compositor to repaint.
    crystal_mark_dirty();
}

static void on_destroy_notify(AuraWM *wm, XEvent *e)
{
    Window w = e->xdestroywindow.window;
    Client *c = wm_find_client(wm, w);
    if (c) {
        // Tell Crystal to release the texture and damage tracking for this
        // window BEFORE we destroy the frame. Crystal looks up the window
        // by its frame ID, so the frame must still exist at this point.
        if (crystal_is_active()) {
            crystal_window_unmapped(wm, c);
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

static void on_configure_request(AuraWM *wm, XEvent *e)
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

    // Window geometry changed — tell the compositor to repaint.
    // Also triggers a restack in case sibling/stacking order changed.
    crystal_mark_dirty();
}

static void on_button_press(AuraWM *wm, XEvent *e)
{
    Window w = e->xbutton.window;
    Client *c = wm_find_client_by_frame(wm, w);

    if (!c) return;

    // Focus on click
    wm_focus_client(wm, c);

    int fx = e->xbutton.x;  // Click position relative to frame
    int fy = e->xbutton.y;

    // Check if click is in the title bar area
    if (fy < TITLEBAR_HEIGHT) {
        // Check traffic light buttons
        int bx = BUTTON_LEFT_PAD;
        int by = BUTTON_TOP_PAD;

        // Close button
        if (fx >= bx && fx <= bx + BUTTON_DIAMETER &&
            fy >= by && fy <= by + BUTTON_DIAMETER) {
            ewmh_send_delete(wm, c->client);
            return;
        }
        bx += BUTTON_DIAMETER + BUTTON_SPACING;

        // Minimize button — trigger the genie animation if Crystal is active.
        // The genie effect visually warps the window into the dock icon over
        // ~500ms, creating the signature Snow Leopard "flowing into the lamp"
        // look. If Crystal is not running, fall back to an instant unmap.
        if (fx >= bx && fx <= bx + BUTTON_DIAMETER &&
            fy >= by && fy <= by + BUTTON_DIAMETER) {
            if (crystal_is_active()) {
                // Target the center-bottom of the screen where the dock lives.
                // When the dock reports its actual icon positions, we can pass
                // the exact coordinates here instead of this approximation.
                int dock_x = wm->root_w / 2;
                int dock_y = wm->root_h - 48;
                crystal_animate_minimize(wm, c, dock_x, dock_y);
            } else {
                XUnmapWindow(wm->dpy, c->frame);
                c->mapped = false;
            }
            ewmh_update_client_list(wm);
            return;
        }
        bx += BUTTON_DIAMETER + BUTTON_SPACING;

        // Zoom button (toggle between saved size and maximized)
        if (fx >= bx && fx <= bx + BUTTON_DIAMETER &&
            fy >= by && fy <= by + BUTTON_DIAMETER) {
            extern void client_smart_zoom(AuraWM *wm, Client *c);
            client_smart_zoom(wm, c);
            frame_redraw_decor(wm, c);
            return;
        }

        // Click on title bar — begin drag
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

static void on_button_release(AuraWM *wm, XEvent *e)
{
    (void)e;
    if (wm->resizing) {
        resize_end(wm);
    } else if (wm->dragging) {
        XUngrabPointer(wm->dpy, CurrentTime);
        wm->dragging = false;
        wm->drag_client = NULL;
    }
}

static void on_motion_notify(AuraWM *wm, XEvent *e)
{
    if (wm->resizing) {
        // Active resize — update window geometry based on drag delta
        resize_update(wm, e->xmotion.x_root, e->xmotion.y_root);
    } else if (wm->dragging && wm->drag_client) {
        // Title bar drag — move the window
        int dx = e->xmotion.x_root - wm->drag_start_x;
        int dy = e->xmotion.y_root - wm->drag_start_y;

        Client *c = wm->drag_client;
        c->x = wm->drag_frame_x + dx;
        c->y = wm->drag_frame_y + dy;

        XMoveWindow(wm->dpy, c->frame, c->x, c->y);
    } else {
        // Not dragging or resizing — update cursor shape based on
        // which edge/corner the mouse is hovering over
        Client *c = wm_find_client_by_frame(wm, e->xmotion.window);
        if (c) {
            resize_update_cursor(wm, c, e->xmotion.x, e->xmotion.y);
        }
    }
}

static void on_property_notify(AuraWM *wm, XEvent *e)
{
    Window w = e->xproperty.window;
    Client *c = wm_find_client(wm, w);

    if (c && (e->xproperty.atom == wm->atom_net_wm_name ||
              e->xproperty.atom == wm->atom_wm_name)) {
        // Title changed — update and redraw decoration
        ewmh_get_title(wm, c->client, c->title, sizeof(c->title));
        if (c->frame) {
            frame_redraw_decor(wm, c);
            // Title bar was repainted — compositor needs to refresh.
            crystal_mark_dirty();
        }
    }

    // Strut properties changed on any window — recalculate work area
    if (e->xproperty.atom == wm->atom_net_wm_strut ||
        e->xproperty.atom == wm->atom_net_wm_strut_partial) {
        struts_recalculate(wm);
    }
}

static void on_client_message(AuraWM *wm, XEvent *e)
{
    XClientMessageEvent *cm = &e->xclient;

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

static void on_focus_in(AuraWM *wm, XEvent *e)
{
    Window w = e->xfocus.window;
    Client *c = wm_find_client(wm, w);
    if (c && c != wm->focused) {
        wm_focus_client(wm, c);
        frame_redraw_decor(wm, c);

        // Focus changed — the shadow intensity differs between active and
        // inactive windows, so the compositor needs to repaint.
        crystal_mark_dirty();
    }
}

static void on_key_press(AuraWM *wm, XEvent *e)
{
    input_handle_key(wm, &e->xkey);
}

static void on_expose(AuraWM *wm, XEvent *e)
{
    if (e->xexpose.count > 0) return; // Only redraw on last expose

    Client *c = wm_find_client_by_frame(wm, e->xexpose.window);
    if (c) {
        frame_redraw_decor(wm, c);

        // Tell Crystal the window contents changed so it refreshes the
        // OpenGL texture on the next composite pass. Expose events mean
        // the window (or its decorations) were repainted.
        if (crystal_is_active()) {
            crystal_window_damaged(wm, c);
        }
    }
}
