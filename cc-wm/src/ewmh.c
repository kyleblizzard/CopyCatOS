// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// CopiCatOS Window Manager — EWMH/ICCCM compliance
// Aggressive early implementation per review feedback — Qt apps
// and shell components misbehave without _NET_FRAME_EXTENTS,
// _NET_CLIENT_LIST, _NET_ACTIVE_WINDOW, etc.

#include "ewmh.h"
#include "moonrock.h"
#include <stdio.h>
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
    const char *name = "CopiCatOS";
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

    fprintf(stderr, "[cc-wm] EWMH properties set on root\n");
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
