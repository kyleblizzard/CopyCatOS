// CopyCatOS — by Kyle Blizzard at Blizzard.show

// CopyCatOS Window Manager — EWMH/ICCCM compliance
// Aggressive early implementation per review feedback — Qt apps
// and shell components misbehave without _NET_FRAME_EXTENTS,
// _NET_CLIENT_LIST, _NET_ACTIVE_WINDOW, etc.

#include "ewmh.h"
#include "frame.h"
#include "moonrock.h"
#include "moonrock_display.h"
#include "moonrock_scale.h"
#include <X11/Xcursor/Xcursor.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ewmh_setup(CCWM *wm)
{
    // Create a child window for _NET_SUPPORTING_WM_CHECK
    Window check = XCreateSimpleWindow(wm->dpy, wm->root,
                                        -1, -1, 1, 1, 0, 0, 0);

    // Set _NET_SUPPORTING_WM_CHECK on both root and check window
    XChangeProperty(wm->dpy, wm->root, wm->atom_net_supporting_wm_check,
                    XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&check, 1);
    XChangeProperty(wm->dpy, check, wm->atom_net_supporting_wm_check,
                    XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&check, 1);

    // Set WM name
    const char *name = "CopyCatOS";
    XChangeProperty(wm->dpy, check, wm->atom_net_wm_name,
                    wm->atom_utf8_string, 8, PropModeReplace,
                    (unsigned char *)name, (int)strlen(name));

    // Declare supported atoms — be aggressive, declare everything
    // we handle or plan to handle. Apps check this list.
    // _NET_WORKAREA etc. are interned by struts_init() but we add them
    // here too so they appear in _NET_SUPPORTED before struts_init runs.
    Atom atom_workarea = XInternAtom(wm->dpy, "_NET_WORKAREA", False);
    Atom atom_desktop_geom = XInternAtom(wm->dpy, "_NET_DESKTOP_GEOMETRY", False);
    Atom atom_num_desktops = XInternAtom(wm->dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    Atom atom_cur_desktop = XInternAtom(wm->dpy, "_NET_CURRENT_DESKTOP", False);

    Atom supported[] = {
        wm->atom_net_supported,
        wm->atom_net_supporting_wm_check,
        wm->atom_net_wm_name,
        wm->atom_net_wm_type,
        wm->atom_net_wm_type_normal,
        wm->atom_net_wm_type_dock,
        wm->atom_net_wm_type_desktop,
        wm->atom_net_wm_type_dialog,
        wm->atom_net_wm_type_splash,
        wm->atom_net_wm_type_utility,
        wm->atom_net_wm_state,
        wm->atom_net_wm_state_fullscreen,
        wm->atom_net_wm_state_hidden,
        wm->atom_net_active_window,
        wm->atom_net_client_list,
        wm->atom_net_client_list_stacking,
        wm->atom_net_frame_extents,
        wm->atom_net_wm_allowed_actions,
        wm->atom_net_close_window,
        wm->atom_net_wm_strut,
        wm->atom_net_wm_strut_partial,
        atom_workarea,
        atom_desktop_geom,
        atom_num_desktops,
        atom_cur_desktop,
        wm->atom_net_wm_ping,
    };
    XChangeProperty(wm->dpy, wm->root, wm->atom_net_supported,
                    XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)supported,
                    sizeof(supported) / sizeof(Atom));

    // Initialize empty client list
    XChangeProperty(wm->dpy, wm->root, wm->atom_net_client_list,
                    XA_WINDOW, 32, PropModeReplace, NULL, 0);
    XChangeProperty(wm->dpy, wm->root, wm->atom_net_client_list_stacking,
                    XA_WINDOW, 32, PropModeReplace, NULL, 0);

    // Initialize _NET_ACTIVE_WINDOW to None
    Window none = None;
    XChangeProperty(wm->dpy, wm->root, wm->atom_net_active_window,
                    XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&none, 1);

    fprintf(stderr, "[moonrock] EWMH properties set on root\n");
}

void ewmh_update_client_list(CCWM *wm)
{
    Window clients[MAX_CLIENTS];
    int n = 0;
    for (int i = 0; i < wm->num_clients; i++) {
        if (wm->clients[i].mapped) {
            clients[n++] = wm->clients[i].client;
        }
    }

    XChangeProperty(wm->dpy, wm->root, wm->atom_net_client_list,
                    XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)clients, n);
    XChangeProperty(wm->dpy, wm->root, wm->atom_net_client_list_stacking,
                    XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)clients, n);

    // Map/unmap changes the frontmost-per-output table; republish so
    // menubar (Modern mode) picks up the new topmost app per output
    // without waiting on the next scale or focus event.
    ewmh_publish_output_focus_state(wm);
}

void ewmh_set_frame_extents(CCWM *wm, Window client)
{
    // _NET_FRAME_EXTENTS: left, right, top, bottom
    // Qt apps NEED this to position dialogs correctly.
    // When compositor is active, frame extents include shadow padding.
    int sl = compositor_active ? SHADOW_LEFT : 0;
    int sr = compositor_active ? SHADOW_RIGHT : 0;
    int st = compositor_active ? SHADOW_TOP : 0;
    int sb = compositor_active ? SHADOW_BOTTOM : 0;
    long extents[4] = {
        BORDER_WIDTH + sl,      // left
        BORDER_WIDTH + sr,      // right
        TITLEBAR_HEIGHT + st,   // top (title bar + shadow)
        BORDER_WIDTH + sb       // bottom
    };
    XChangeProperty(wm->dpy, client, wm->atom_net_frame_extents,
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)extents, 4);
}

Atom ewmh_get_window_type(CCWM *wm, Window w)
{
    Atom type_ret;
    int fmt;
    unsigned long nitems, after;
    unsigned char *data = NULL;

    if (XGetWindowProperty(wm->dpy, w, wm->atom_net_wm_type,
                           0, 1, False, XA_ATOM,
                           &type_ret, &fmt, &nitems, &after, &data) == Success
        && data && nitems > 0) {
        Atom result = *(Atom *)data;
        XFree(data);
        return result;
    }
    if (data) XFree(data);
    return wm->atom_net_wm_type_normal; // Default to normal
}

bool ewmh_supports_delete(CCWM *wm, Window w)
{
    Atom type_ret;
    int fmt;
    unsigned long nitems, after;
    unsigned char *data = NULL;

    if (XGetWindowProperty(wm->dpy, w, wm->atom_wm_protocols,
                           0, 32, False, XA_ATOM,
                           &type_ret, &fmt, &nitems, &after, &data) == Success
        && data) {
        Atom *protocols = (Atom *)data;
        for (unsigned long i = 0; i < nitems; i++) {
            if (protocols[i] == wm->atom_wm_delete) {
                XFree(data);
                return true;
            }
        }
        XFree(data);
    }
    return false;
}

void ewmh_send_delete(CCWM *wm, Window w)
{
    if (ewmh_supports_delete(wm, w)) {
        XEvent ev = {0};
        ev.type = ClientMessage;
        ev.xclient.window = w;
        ev.xclient.message_type = wm->atom_wm_protocols;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = (long)wm->atom_wm_delete;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(wm->dpy, w, False, NoEventMask, &ev);
    } else {
        // App doesn't support WM_DELETE_WINDOW — kill it
        XKillClient(wm->dpy, w);
    }
}

void ewmh_get_title(CCWM *wm, Window w, char *buf, int buflen)
{
    buf[0] = '\0';

    // Try _NET_WM_NAME first (UTF-8)
    Atom type_ret;
    int fmt;
    unsigned long nitems, after;
    unsigned char *data = NULL;

    if (XGetWindowProperty(wm->dpy, w, wm->atom_net_wm_name,
                           0, 256, False, wm->atom_utf8_string,
                           &type_ret, &fmt, &nitems, &after, &data) == Success
        && data && nitems > 0) {
        snprintf(buf, buflen, "%s", (char *)data);
        XFree(data);
        return;
    }
    if (data) XFree(data);

    // Fall back to WM_NAME (Latin-1)
    XTextProperty tp;
    if (XGetWMName(wm->dpy, w, &tp) && tp.value) {
        snprintf(buf, buflen, "%s", (char *)tp.value);
        XFree(tp.value);
    }
}

// ── _NET_WM_STATE helpers ────────────────────────────────────────

bool ewmh_has_wm_state(CCWM *wm, Window w, Atom state)
{
    Atom type_ret;
    int fmt;
    unsigned long nitems, after;
    unsigned char *data = NULL;

    if (XGetWindowProperty(wm->dpy, w, wm->atom_net_wm_state,
                           0, 32, False, XA_ATOM,
                           &type_ret, &fmt, &nitems, &after, &data) == Success
        && data) {
        Atom *atoms = (Atom *)data;
        for (unsigned long i = 0; i < nitems; i++) {
            if (atoms[i] == state) {
                XFree(data);
                return true;
            }
        }
        XFree(data);
    }
    return false;
}

void ewmh_set_wm_state(CCWM *wm, Window w, Atom state, bool set)
{
    Atom type_ret;
    int fmt;
    unsigned long nitems, after;
    unsigned char *data = NULL;

    // Read current state list
    Atom current[32];
    int count = 0;

    if (XGetWindowProperty(wm->dpy, w, wm->atom_net_wm_state,
                           0, 32, False, XA_ATOM,
                           &type_ret, &fmt, &nitems, &after, &data) == Success
        && data) {
        Atom *atoms = (Atom *)data;
        for (unsigned long i = 0; i < nitems && count < 32; i++) {
            current[count++] = atoms[i];
        }
        XFree(data);
    }

    if (set) {
        // Add the state atom if not already present
        for (int i = 0; i < count; i++) {
            if (current[i] == state) return; // Already set
        }
        if (count < 32) {
            current[count++] = state;
        }
    } else {
        // Remove the state atom
        int dst = 0;
        for (int i = 0; i < count; i++) {
            if (current[i] != state) {
                current[dst++] = current[i];
            }
        }
        count = dst;
    }

    // Write the updated list back
    XChangeProperty(wm->dpy, w, wm->atom_net_wm_state,
                    XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)current, count);
}

// ── Fullscreen management ───────────────────────────────────────

void wm_set_fullscreen(CCWM *wm, Client *c, bool enter)
{
    if (!c || !c->frame) return;
    if (enter == c->fullscreen) return; // Already in requested state

    if (enter) {
        // Save current geometry so we can restore on exit
        c->pre_fs_x = c->x;
        c->pre_fs_y = c->y;
        c->pre_fs_w = c->w;
        c->pre_fs_h = c->h;
        c->fullscreen = true;

        // Resize frame to cover the entire screen with no decorations.
        // The client fills the frame completely (no title bar, no border,
        // no shadow padding).
        c->x = 0;
        c->y = 0;
        c->w = wm->root_w;
        c->h = wm->root_h;

        XMoveResizeWindow(wm->dpy, c->frame, 0, 0, wm->root_w, wm->root_h);
        XMoveResizeWindow(wm->dpy, c->client, 0, 0, wm->root_w, wm->root_h);
        XRaiseWindow(wm->dpy, c->frame);

        // Set frame extents to zero — no chrome in fullscreen
        long extents[4] = {0, 0, 0, 0};
        XChangeProperty(wm->dpy, c->client, wm->atom_net_frame_extents,
                        XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char *)extents, 4);

        // Update the _NET_WM_STATE property on the client
        ewmh_set_wm_state(wm, c->client, wm->atom_net_wm_state_fullscreen, true);

        fprintf(stderr, "[moonrock] '%s' entered fullscreen %dx%d\n",
                c->title, wm->root_w, wm->root_h);
    } else {
        // Exit fullscreen — restore saved geometry and decorations
        c->fullscreen = false;
        c->x = c->pre_fs_x;
        c->y = c->pre_fs_y;
        c->w = c->pre_fs_w;
        c->h = c->pre_fs_h;

        // Calculate decorated frame size (same math as frame_window)
        int sl = compositor_active ? SHADOW_LEFT : 0;
        int sr = compositor_active ? SHADOW_RIGHT : 0;
        int st = compositor_active ? SHADOW_TOP : 0;
        int sb = compositor_active ? SHADOW_BOTTOM : 0;

        int frame_w = c->w + 2 * BORDER_WIDTH + sl + sr;
        int frame_h = c->h + TITLEBAR_HEIGHT + BORDER_WIDTH + st + sb;

        XMoveResizeWindow(wm->dpy, c->frame, c->x, c->y, frame_w, frame_h);
        XMoveResizeWindow(wm->dpy, c->client,
                          sl + BORDER_WIDTH, st + TITLEBAR_HEIGHT,
                          c->w, c->h);

        // Restore normal frame extents
        ewmh_set_frame_extents(wm, c->client);

        // Remove fullscreen from _NET_WM_STATE
        ewmh_set_wm_state(wm, c->client, wm->atom_net_wm_state_fullscreen, false);

        // Redraw decorations now that they're visible again
        frame_redraw_decor(wm, c);

        fprintf(stderr, "[moonrock] '%s' exited fullscreen, restored %dx%d+%d+%d\n",
                c->title, c->w, c->h, c->x, c->y);
    }
}

// ── _NET_WM_PING implementation ──────────────────────────────────

// Helper: get elapsed time in milliseconds between two timespecs
static long timespec_diff_ms(struct timespec *start, struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1000 +
           (end->tv_nsec - start->tv_nsec) / 1000000;
}

bool ewmh_supports_ping(CCWM *wm, Window w)
{
    // Check if _NET_WM_PING is listed in the window's WM_PROTOCOLS
    Atom type_ret;
    int fmt;
    unsigned long nitems, after;
    unsigned char *data = NULL;

    if (XGetWindowProperty(wm->dpy, w, wm->atom_wm_protocols,
                           0, 32, False, XA_ATOM,
                           &type_ret, &fmt, &nitems, &after, &data) == Success
        && data) {
        Atom *protocols = (Atom *)data;
        for (unsigned long i = 0; i < nitems; i++) {
            if (protocols[i] == wm->atom_net_wm_ping) {
                XFree(data);
                return true;
            }
        }
        XFree(data);
    }
    return false;
}

void ewmh_send_ping(CCWM *wm, Client *c)
{
    if (!c || !c->supports_ping) return;
    if (c->ping_pending) return; // Don't send another while one is outstanding

    // Generate a unique timestamp for this ping. We use the current time
    // in milliseconds as the serial — the app must echo it back exactly.
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    unsigned long serial = (unsigned long)(now.tv_sec * 1000 + now.tv_nsec / 1000000);

    // Send _NET_WM_PING as a WM_PROTOCOLS ClientMessage to the client.
    // Format: message_type = WM_PROTOCOLS, data.l[0] = _NET_WM_PING,
    // data.l[1] = timestamp, data.l[2] = client window
    XEvent ev = {0};
    ev.type = ClientMessage;
    ev.xclient.window = c->client;
    ev.xclient.message_type = wm->atom_wm_protocols;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (long)wm->atom_net_wm_ping;
    ev.xclient.data.l[1] = (long)serial;
    ev.xclient.data.l[2] = (long)c->client;

    XSendEvent(wm->dpy, c->client, False, NoEventMask, &ev);

    // Record ping state
    c->ping_pending = true;
    c->ping_serial = serial;
    c->ping_sent = now;
}

Client *ewmh_handle_pong(CCWM *wm, XClientMessageEvent *cm)
{
    // A pong is a ClientMessage sent to the ROOT window with:
    // message_type = WM_PROTOCOLS, data.l[0] = _NET_WM_PING,
    // data.l[2] = the client window that was pinged.
    if (cm->message_type != wm->atom_wm_protocols) return NULL;
    if ((Atom)cm->data.l[0] != wm->atom_net_wm_ping) return NULL;

    // Find the client that responded
    Window client_win = (Window)cm->data.l[2];
    Client *c = wm_find_client(wm, client_win);
    if (!c) return NULL;

    // Clear ping state
    c->ping_pending = false;

    // If the window was marked unresponsive, restore normal cursor
    if (c->unresponsive) {
        c->unresponsive = false;
        if (c->frame) {
            // Remove the beach ball cursor from the frame — this
            // restores the default cursor (inherited from root)
            XUndefineCursor(wm->dpy, c->frame);
        }
        if (getenv("AURA_DEBUG")) {
            fprintf(stderr, "[moonrock] '%s' is responsive again\n", c->title);
        }
    }

    return c;
}

void ewmh_check_ping_timeouts(CCWM *wm)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    for (int i = 0; i < wm->num_clients; i++) {
        Client *c = &wm->clients[i];
        if (!c->mapped || !c->ping_pending) continue;

        long elapsed = timespec_diff_ms(&c->ping_sent, &now);

        if (elapsed >= PING_TIMEOUT_MS && !c->unresponsive) {
            // Window didn't respond in time — show the beach ball!
            // This is the same behavior as macOS showing the spinning
            // rainbow pinwheel after 2-4 seconds of no event processing.
            c->unresponsive = true;

            if (c->frame && wm->beach_ball_cursor) {
                XDefineCursor(wm->dpy, c->frame, (Cursor)wm->beach_ball_cursor);
            }

            fprintf(stderr, "[moonrock] '%s' is unresponsive — beach ball!\n",
                    c->title);
        }
    }
}


// ── Per-output focus state publisher ───────────────────────────────────
//
// Two atoms live here rather than in moonrock_display.c because their
// value comes from the WM (focused client, managed client list) rather
// than the display table. moonrock_display.c provides the output row
// order; we provide the focus/frontmost overlay.
//
// _MOONROCK_ACTIVE_OUTPUT (CARDINAL, format 32, length 1):
//     Row index in _MOONROCK_OUTPUT_SCALES hosting the focused window.
//     MOONROCK_ACTIVE_OUTPUT_NONE (0xFFFFFFFF) when no window focused.
//
// _MOONROCK_FRONTMOST_PER_OUTPUT (WINDOW, format 32, length == output count):
//     Topmost managed client per output in the same row order as the
//     scale table. Uses XQueryTree for real X stacking order so
//     subscribers match what the user sees on screen.

static CCWM *focus_state_wm = NULL;
static Atom  active_output_atom = None;
static Atom  frontmost_atom = None;

// Thunk handed to display_set_scales_published_cb(). The display module's
// hook is void(void), so it finds the WM via the file-static stashed by
// ewmh_register_focus_state_hook().
static void focus_state_republish_thunk(void)
{
    if (focus_state_wm) {
        ewmh_publish_output_focus_state(focus_state_wm);
    }
}

void ewmh_register_focus_state_hook(CCWM *wm)
{
    focus_state_wm = wm;
    if (wm && wm->dpy) {
        if (active_output_atom == None) {
            active_output_atom = XInternAtom(
                wm->dpy, MOONROCK_ACTIVE_OUTPUT_ATOM_NAME, False);
        }
        if (frontmost_atom == None) {
            frontmost_atom = XInternAtom(
                wm->dpy, MOONROCK_FRONTMOST_PER_OUTPUT_ATOM_NAME, False);
        }
    }
    display_set_scales_published_cb(focus_state_republish_thunk);

    // Kick one initial publish so subscribers that start after MoonRock
    // see a valid state even if the scale table doesn't change.
    ewmh_publish_output_focus_state(wm);
}

// Pick the output row index whose rect contains a given point. Midpoint
// of the client's frame is the intent — any part on the output counts.
// Returns -1 if no output matches (shouldn't happen when outputs are
// connected, but stays sensible during hotplug races).
static int output_index_for_point(MROutput *outputs, int n, int x, int y)
{
    for (int i = 0; i < n; i++) {
        if (x >= outputs[i].x && x < outputs[i].x + outputs[i].width &&
            y >= outputs[i].y && y < outputs[i].y + outputs[i].height) {
            return i;
        }
    }
    return -1;
}

static int home_output_for_client(const Client *c, MROutput *outputs, int n)
{
    if (!c || !c->mapped || c->w <= 0 || c->h <= 0) return -1;
    int mx = c->x + c->w / 2;
    int my = c->y + c->h / 2;
    int idx = output_index_for_point(outputs, n, mx, my);
    if (idx >= 0) return idx;

    // Fall back to primary — happens if the window sits in a gap
    // between outputs during a hotplug frame.
    for (int i = 0; i < n; i++) {
        if (outputs[i].primary) return i;
    }
    return (n > 0) ? 0 : -1;
}

void ewmh_publish_output_focus_state(CCWM *wm)
{
    if (!wm || !wm->dpy) return;
    if (active_output_atom == None) {
        active_output_atom = XInternAtom(
            wm->dpy, MOONROCK_ACTIVE_OUTPUT_ATOM_NAME, False);
    }
    if (frontmost_atom == None) {
        frontmost_atom = XInternAtom(
            wm->dpy, MOONROCK_FRONTMOST_PER_OUTPUT_ATOM_NAME, False);
    }

    int output_count = 0;
    MROutput *outputs = display_get_outputs(&output_count);
    if (!outputs || output_count <= 0) {
        // No output table yet (display_init hasn't run). Subscribers
        // treat missing properties as "no data", so leave the atoms
        // untouched.
        return;
    }

    // ── Active output: keyboard-focused window's home output. ──
    unsigned long active_idx =
        (unsigned long)MOONROCK_ACTIVE_OUTPUT_NONE;
    if (wm->focused) {
        int idx = home_output_for_client(wm->focused, outputs, output_count);
        if (idx >= 0) active_idx = (unsigned long)idx;
    }

    // ── Frontmost per output via real X stacking order. ──
    //
    // XQueryTree returns children bottom-to-top. Walk top-to-bottom,
    // map each frame window back to a managed Client, and record its
    // home output as the frontmost for that row (first write wins =
    // topmost). Unmanaged windows (override-redirect children, the
    // compositor's own helper windows) are skipped naturally because
    // wm_find_client_by_frame returns NULL.
    unsigned long frontmost[MOONROCK_SCALE_MAX_OUTPUTS];
    for (int i = 0; i < output_count && i < MOONROCK_SCALE_MAX_OUTPUTS; i++) {
        frontmost[i] = (unsigned long)None;
    }

    Window root_ret, parent_ret;
    Window *children = NULL;
    unsigned int n_children = 0;
    if (XQueryTree(wm->dpy, wm->root, &root_ret, &parent_ret,
                   &children, &n_children) && children) {
        // Top of stack is last element — walk in reverse.
        for (int i = (int)n_children - 1; i >= 0; i--) {
            Client *c = wm_find_client_by_frame(wm, children[i]);
            if (!c || !c->mapped) continue;
            int idx = home_output_for_client(c, outputs, output_count);
            if (idx < 0 || idx >= output_count) continue;
            if (idx >= MOONROCK_SCALE_MAX_OUTPUTS) continue;
            if (frontmost[idx] == (unsigned long)None) {
                frontmost[idx] = (unsigned long)c->client;
            }
        }
        XFree(children);
    }

    int emit_count = output_count;
    if (emit_count > MOONROCK_SCALE_MAX_OUTPUTS) {
        emit_count = MOONROCK_SCALE_MAX_OUTPUTS;
    }

    // Dedup: skip XChangeProperty writes when neither payload changed.
    // Every focus / map / unmap / hotplug hits this path, and most of
    // them leave one or both atoms unchanged. Avoiding the write also
    // avoids the PropertyNotify round-trip to every subscriber.
    static unsigned long  last_active = 0xDEADBEEFul;  // sentinel
    static unsigned long  last_frontmost[MOONROCK_SCALE_MAX_OUTPUTS];
    static int            last_frontmost_count = -1;

    bool active_changed = (last_active != active_idx);
    bool frontmost_changed = (last_frontmost_count != emit_count) ||
        memcmp(last_frontmost, frontmost,
               (size_t)emit_count * sizeof(unsigned long)) != 0;

    if (active_changed) {
        XChangeProperty(wm->dpy, wm->root, active_output_atom,
                        XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char *)&active_idx, 1);
        last_active = active_idx;
    }

    if (frontmost_changed) {
        XChangeProperty(wm->dpy, wm->root, frontmost_atom,
                        XA_WINDOW, 32, PropModeReplace,
                        (unsigned char *)frontmost, emit_count);
        memcpy(last_frontmost, frontmost,
               (size_t)emit_count * sizeof(unsigned long));
        last_frontmost_count = emit_count;
    }

    if (active_changed || frontmost_changed) {
        XFlush(wm->dpy);
    }
}
