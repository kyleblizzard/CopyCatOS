// CopyCatOS — by Kyle Blizzard at Blizzard.show

// CopyCatOS Window Manager — X11 event dispatch
// Uses function pointer array for O(1) dispatch (dwm pattern)

#include "events.h"
#include "frame.h"
#include "ewmh.h"
#include "input.h"
#include "moonrock.h"
#include "moonbase_host.h"
#include "moonbase_xdnd.h"
#include "moonbase.h"     // MB_MOD_*
#include "moonrock_display.h"   // display_scale_request_atom + handler
#include "struts.h"
#include "resize.h"
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>   // strcasecmp
#include <stdlib.h>    // free
#include <time.h>
#include <X11/Xutil.h>

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
static void on_key_release(CCWM *wm, XEvent *e);
static void on_expose(CCWM *wm, XEvent *e);
static void on_leave_notify(CCWM *wm, XEvent *e);
static void on_selection_notify(CCWM *wm, XEvent *e);

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
    handlers[KeyRelease]       = on_key_release;
    handlers[Expose]           = on_expose;
    handlers[LeaveNotify]      = on_leave_notify;
    handlers[SelectionNotify]  = on_selection_notify;
}

void events_run(CCWM *wm)
{
    init_handlers();

    // Frame any pre-existing windows
    frame_existing_windows(wm);

    fprintf(stderr, "[moonrock] Entering event loop\n");

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
            } else if (display_handle_event(wm->dpy, &ev)) {
                // XRandR hotplug / screen-change event — display module
                // consumed it, auto-enabled any new output, and
                // re-published the scale table.
            } else if (ev.type < LASTEvent && handlers[ev.type]) {
                handlers[ev.type](wm, &ev);
            }
        }

        // Run the coalesced output re-enumeration exactly once per
        // iteration, after the X queue drain has finished. A single
        // primary toggle / mode change / hotplug emits multiple RR
        // events in one burst; display_handle_event() latches a flag
        // while draining and this call does the full display_init()
        // walk once. Must run before mr_composite so the renderer
        // sees fresh per-output geometry this same frame.
        display_flush_deferred_hotplug(wm->dpy);

        // Composite all windows onto the screen. This is the MoonRock
        // compositor's main render pass — it draws every window with
        // shadows and effects via OpenGL.
        if (mr_is_active()) {
            mr_composite(wm);
        }

        // Unified wait. Poll X + MoonBase every iteration so a busy
        // compositor can't starve the moonbase listener — previous
        // code only polled when XPending()==0, and under compositor
        // redraw pressure (damage notifies generated by every
        // composite pass) that condition never held, leaving
        // accept() blocked and new clients queued at the kernel.
        //
        // Timeout is 0 when X still has buffered work (drain it on
        // the next iteration without added latency), 16ms ≈ 60Hz
        // when fully idle. mb_host_tick always runs so accepts,
        // frame reads, and send-queue drains happen every pass
        // regardless of X state.
        struct pollfd fds[32];
        size_t n = 0;

        fds[n].fd      = ConnectionNumber(wm->dpy);
        fds[n].events  = POLLIN;
        fds[n].revents = 0;
        n++;

        size_t mb_first = n;
        size_t mb_cap   = (sizeof(fds)/sizeof(fds[0])) - n;
        size_t mb_n     = mb_host_collect_pollfds(fds + mb_first, mb_cap);
        n += mb_n;

        int timeout_ms = (XPending(wm->dpy) > 0) ? 0 : 16;
        (void)poll(fds, (nfds_t)n, timeout_ms);

        mb_host_tick(fds + mb_first, mb_n);
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

    // Ignore unmaps we caused by reparenting (event comes from root because
    // we selected SubstructureNotify on root when we reparented).
    if (e->xunmap.event == wm->root) return;

    Client *c = wm_find_client(wm, w);
    if (c) {
        // If this window is minimized, the unmap came from us (we called
        // XUnmapWindow on the frame when genie minimize completed, which
        // also auto-unmaps the client). We must NOT unframe it — the frame
        // needs to survive so the user can restore it from the dock.
        if (c->minimized) return;

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
        // Fullscreen windows ignore configure requests — they must stay
        // covering the entire screen until explicitly exiting fullscreen.
        if (c->fullscreen) return;

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

// ── Dock icon position lookup ──
//
// Reads _MOONROCK_DOCK_ICON_POSITIONS from the root window (published by dock
// after every paint) and returns the screen-absolute center of the dock icon
// whose process_name or exec_base matches the client's WM_CLASS.
//
// The property format (set by dock_publish_icon_positions in dock.c) is
// newline-separated entries: "process_name:exec_base:x:y\n"
//
// Matching strategy:
//   1. Try exact case-insensitive match: wm_class == process_name
//   2. Try exact case-insensitive match: wm_class == exec_base
//   3. Try substring: process_name or exec_base starts with wm_class, or vice versa
// This handles variations like "firefox-bin" matching "Firefox".
//
// On no match, or if the property doesn't exist (dock not running), falls
// back to center-of-screen at the dock's estimated Y so the genie still
// plays — it just aims at the middle of the dock rather than a specific icon.
static void get_dock_icon_pos(CCWM *wm, Client *c, int *out_x, int *out_y)
{
    // Sensible fallback: horizontal center of screen, near dock bottom.
    *out_x = wm->root_w / 2;
    *out_y = wm->root_h - 48;

    Atom actual_type;
    int  actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    int rc = XGetWindowProperty(
        wm->dpy, wm->root,
        wm->atom_cc_dock_icon_positions,
        0,           // Offset (0 = start from beginning)
        4096 / 4,    // Max length in 32-bit units (4096 bytes / 4 = 1024 longs)
        False,       // Don't delete after read
        XA_STRING,   // Expected type
        &actual_type, &actual_format, &nitems, &bytes_after,
        &data);

    if (rc != Success || !data || nitems == 0) {
        if (data) XFree(data);
        return; // Dock not running or property not set — use fallback
    }

    // Parse each newline-separated entry.
    // We operate on a NUL-terminated copy so strtok is safe.
    char *text = strndup((char *)data, nitems);
    XFree(data);
    if (!text) return;

    char *line = strtok(text, "\n");
    while (line) {
        // Each line: "process_name:exec_base:x:y"
        char proc[128], exec_base[256];
        int  ix, iy;

        if (sscanf(line, "%127[^:]:%255[^:]:%d:%d",
                   proc, exec_base, &ix, &iy) == 4) {

            // Try to match against the client's instance name (wm_class)
            // and class name (wm_class_name). We try exact match first, then
            // a "starts-with" match for names like "firefox-esr" vs "Firefox".
            const char *candidates[2] = { c->wm_class, c->wm_class_name };

            for (int ci = 0; ci < 2; ci++) {
                const char *cand = candidates[ci];
                if (!cand[0]) continue;

                // Check against both process_name and exec_base
                const char *keys[2] = { proc, exec_base };
                for (int ki = 0; ki < 2; ki++) {
                    const char *key = keys[ki];
                    if (!key[0]) continue;

                    // Exact match (case-insensitive)
                    if (strcasecmp(cand, key) == 0) {
                        *out_x = ix;
                        *out_y = iy;
                        free(text);
                        return;
                    }

                    // Prefix match — handles "firefox" matching "firefox-bin"
                    size_t clen = strlen(cand);
                    size_t klen = strlen(key);
                    size_t minlen = clen < klen ? clen : klen;
                    if (strncasecmp(cand, key, minlen) == 0) {
                        *out_x = ix;
                        *out_y = iy;
                        // Don't return immediately — keep looking for exact match
                    }
                }
            }
        }

        line = strtok(NULL, "\n");
    }

    free(text);
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

    // MoonBase surfaces have an InputOnly proxy at the chrome rect.
    // If this click is on one, MoonBase handles it (focus-on-click,
    // close button, etc.) and we must not fall through to the X-client
    // dispatch below.
    if (mb_host_handle_button_press(w, e->xbutton.x, e->xbutton.y,
                                    e->xbutton.button)) {
        return;
    }

    Client *c = wm_find_client_by_frame(wm, w);

    if (!c) return;

    // Focus on click
    wm_focus_client(wm, c);

    // Fullscreen windows have no decorations — no drag, no buttons, no resize
    if (c->fullscreen) return;

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
    // MoonBase proxies take priority — if the release lands on one,
    // let the MoonBase host fire its close/minimize/zoom action and
    // drop its grab before the X-client path runs.
    if (mb_host_handle_button_release(e->xbutton.window,
                                      e->xbutton.x, e->xbutton.y,
                                      e->xbutton.button)) {
        return;
    }

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
                    // Look up where this window's dock icon is so the genie
                    // animation pours toward the real icon, not a hardcoded
                    // center-of-screen fallback.
                    int dock_x, dock_y;
                    get_dock_icon_pos(wm, c, &dock_x, &dock_y);
                    mr_animate_minimize(wm, c, dock_x, dock_y);
                } else {
                    // No compositor — hide immediately with no animation.
                    XUnmapWindow(wm->dpy, c->frame);
                    c->mapped    = false;
                    c->minimized = true;
                    ewmh_update_client_list(wm);
                }
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
    // MoonBase proxies first — if the motion is on one, route it so the
    // traffic-light hover glyphs follow the pointer, and don't fall
    // through to the X-client drag/resize logic below.
    if (mb_host_handle_motion(e->xmotion.window,
                              e->xmotion.x, e->xmotion.y)) {
        return;
    }

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

    // Reverse scale-request atom: the systemcontrol Displays pane writes
    // _MOONROCK_SET_OUTPUT_SCALE on the root window. Handle this before
    // any client-window logic so we don't accidentally match the root
    // against a client lookup.
    if (w == wm->root && e->xproperty.state == PropertyNewValue &&
        e->xproperty.atom == display_scale_request_atom(wm->dpy)) {
        display_handle_scale_request(wm->dpy, wm->root);
        return;
    }

    // Reverse primary-request atom: the Displays pane writes
    // _MOONROCK_SET_PRIMARY_OUTPUT on the root window. Same dispatch
    // shape as the scale-request atom above.
    if (w == wm->root && e->xproperty.state == PropertyNewValue &&
        e->xproperty.atom == display_primary_request_atom(wm->dpy)) {
        display_handle_primary_request(wm->dpy, wm->root);
        return;
    }

    // Reverse rotation-request atom: the Displays pane writes
    // _MOONROCK_SET_OUTPUT_ROTATION on the root window. Same pattern.
    if (w == wm->root && e->xproperty.state == PropertyNewValue &&
        e->xproperty.atom == display_rotation_request_atom(wm->dpy)) {
        display_handle_rotation_request(wm->dpy, wm->root);
        return;
    }

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

    // XDND ClientMessages that target a MoonBase InputOnly proxy get
    // consumed here and never touch the EWMH / _NET_* path below —
    // the XDND module sends its own XdndStatus / XdndFinished replies
    // and the EWMH handlers would otherwise never match (wrong atom).
    // Returning early keeps the dispatch short and avoids false
    // matches if someone adds an atom clash later.
    if (mb_xdnd_handle_client_message(cm)) {
        return;
    }

    // Check for _NET_WM_PING response (pong) first — these are sent
    // to the root window by apps that received our ping. If we get one,
    // the app is responsive and we can clear the beach ball.
    if (ewmh_handle_pong(wm, cm)) {
        return; // Pong handled
    }

    if (cm->message_type == wm->atom_net_active_window) {
        // Request to activate a window — restore it if minimized and focus it
        Client *c = wm_find_client(wm, cm->window);
        if (c) {
            if (c->frame) XMapWindow(wm->dpy, c->frame);
            c->mapped     = true;
            c->minimized  = false;   // Clear minimized state on restore
            wm_focus_client(wm, c);
        }
    } else if (cm->message_type == wm->atom_net_close_window) {
        ewmh_send_delete(wm, cm->window);
    } else if (cm->message_type == wm->atom_net_wm_state) {
        // _NET_WM_STATE ClientMessage — apps request state changes.
        // data.l[0] = action: 0=remove, 1=add, 2=toggle
        // data.l[1] and data.l[2] = state atoms to change
        // gamescope uses this to request fullscreen via _NET_WM_STATE_FULLSCREEN.
        Client *c = wm_find_client(wm, cm->window);
        if (!c) c = wm_find_client_by_frame(wm, cm->window);
        if (c) {
            int action = (int)cm->data.l[0];
            Atom states[2] = { (Atom)cm->data.l[1], (Atom)cm->data.l[2] };

            for (int i = 0; i < 2; i++) {
                if (states[i] == wm->atom_net_wm_state_fullscreen) {
                    bool want_fs;
                    if (action == 0)      want_fs = false;  // _NET_WM_STATE_REMOVE
                    else if (action == 1)  want_fs = true;   // _NET_WM_STATE_ADD
                    else                   want_fs = !c->fullscreen; // _NET_WM_STATE_TOGGLE
                    wm_set_fullscreen(wm, c, want_fs);
                } else if (states[i] == wm->atom_net_wm_state_hidden) {
                    // Hidden state is managed by minimize — ignore explicit requests
                }
            }
        }
    } else if (cm->message_type == wm->atom_wm_change_state) {
        // ICCCM WM_CHANGE_STATE: apps or shell components request an iconic
        // (minimized) state transition. IconicState = 3.
        // This is how menubar's "Window > Minimize" fires the genie effect.
        if (cm->data.l[0] == IconicState) {
            Client *c = wm_find_client(wm, cm->window);
            if (!c) {
                // Try finding by frame too (some senders use the frame XID)
                c = wm_find_client_by_frame(wm, cm->window);
            }
            if (c && c->mapped && !c->minimized) {
                if (mr_is_active()) {
                    // Use the genie animation — same path as clicking the
                    // yellow minimize button in the title bar.
                    int dock_x, dock_y;
                    get_dock_icon_pos(wm, c, &dock_x, &dock_y);
                    mr_animate_minimize(wm, c, dock_x, dock_y);
                } else {
                    // No compositor — hide immediately with no animation.
                    XUnmapWindow(wm->dpy, c->frame);
                    c->mapped    = false;
                    c->minimized = true;
                    ewmh_update_client_list(wm);
                }
            }
        }
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

// Translate X11 event-state bits to MoonBase modifier flags. The MB
// namespace is Apple-flavored (Option = Alt, Command = Super), so we
// map Mod1 → OPTION and Mod4 → COMMAND; the rest are 1:1.
static uint32_t translate_x_mods(unsigned int state)
{
    uint32_t mods = 0;
    if (state & ShiftMask)   mods |= MB_MOD_SHIFT;
    if (state & ControlMask) mods |= MB_MOD_CONTROL;
    if (state & Mod1Mask)    mods |= MB_MOD_OPTION;
    if (state & Mod4Mask)    mods |= MB_MOD_COMMAND;
    if (state & LockMask)    mods |= MB_MOD_CAPSLOCK;
    return mods;
}

static void on_key_press(CCWM *wm, XEvent *e)
{
    // If a MoonBase surface owns the compositor focus, route the key
    // event to it as an MB_IPC_KEY_DOWN frame and swallow it here.
    // Otherwise, fall through to the normal X shortcut path.
    if (mb_host_has_focus()) {
        KeySym sym = XLookupKeysym(&e->xkey, 0);
        uint32_t mods = translate_x_mods(e->xkey.state);
        if (mb_host_route_key((uint32_t)sym, mods,
                              /*is_down*/ true, /*is_repeat*/ false)) {
            // Tier 1 text input: printable ASCII only, no IME, no
            // compose. Real IME lands when inputd gains an fcitx/IBus
            // client. Suppress when Command is held — those key presses
            // are menu shortcuts, not text entry.
            if (!(mods & MB_MOD_COMMAND)) {
                char buf[8];
                int n = XLookupString(&e->xkey, buf, sizeof(buf) - 1,
                                      NULL, NULL);
                // XLookupString returns Latin-1 bytes; the Tier 1 gate
                // below restricts to printable ASCII (0x20..0x7E),
                // which is a strict subset of UTF-8. Ctrl-letter
                // combinations come back as sub-0x20 control codes and
                // are dropped here — apps that want them still see the
                // KEY_DOWN event with the keysym + modifiers.
                if (n > 0 && n < (int)sizeof(buf)) {
                    bool printable_ascii = true;
                    for (int i = 0; i < n; i++) {
                        unsigned char c = (unsigned char)buf[i];
                        if (c < 0x20 || c > 0x7E) {
                            printable_ascii = false;
                            break;
                        }
                    }
                    if (printable_ascii) {
                        buf[n] = '\0';
                        mb_host_route_text_input(buf);
                    }
                }
            }
            return;
        }
    }
    input_handle_key(wm, &e->xkey);
}

static void on_key_release(CCWM *wm, XEvent *e)
{
    // KeyRelease has no legacy dispatch path — it exists solely so
    // MoonBase apps see matched down/up pairs. If no MoonBase surface
    // has focus, the release is silently ignored by the WM.
    (void)wm;
    if (mb_host_has_focus()) {
        KeySym sym = XLookupKeysym(&e->xkey, 0);
        uint32_t mods = translate_x_mods(e->xkey.state);
        (void)mb_host_route_key((uint32_t)sym, mods,
                                /*is_down*/ false, /*is_repeat*/ false);
    }
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
    // MoonBase proxies first — clearing hover there is disjoint from
    // the X-client frame's hover state.
    if (mb_host_handle_leave(e->xcrossing.window)) {
        return;
    }

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

// SelectionNotify is the X reply to an XConvertSelection request. We
// only make those requests from the XDND path (text/uri-list on a
// MoonBase proxy), so hand every SelectionNotify there. Non-XDND
// selections go unhandled — the WM doesn't own any selection today.
static void on_selection_notify(CCWM *wm, XEvent *e)
{
    (void)wm;
    (void)mb_xdnd_handle_selection_notify(&e->xselection);
}
