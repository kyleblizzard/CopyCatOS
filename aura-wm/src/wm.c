// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// AuraOS Window Manager — Core state and initialization

#include "wm.h"
#include "ewmh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// X error handler — CRITICAL for reparenting WMs.
// BadWindow and BadDrawable happen constantly when windows destroy
// themselves between our request and the server processing it.
// Without this handler, the WM crashes on the first app close.
static int x_error_handler(Display *dpy, XErrorEvent *e)
{
    // Silently ignore errors that are normal for a reparenting WM
    if (e->error_code == BadWindow ||
        e->error_code == BadDrawable ||
        e->error_code == BadMatch ||
        e->error_code == BadAccess) {
        if (getenv("AURA_DEBUG")) {
            char buf[256];
            XGetErrorText(dpy, e->error_code, buf, sizeof(buf));
            fprintf(stderr, "[aura-wm] X error (ignored): %s (request %d)\n",
                    buf, e->request_code);
        }
        return 0;
    }

    // Unexpected errors — log them
    char buf[256];
    XGetErrorText(dpy, e->error_code, buf, sizeof(buf));
    fprintf(stderr, "[aura-wm] X error: %s (request %d, resource 0x%lx)\n",
            buf, e->request_code, e->resourceid);
    return 0;
}

// Temporary error handler used during WM detection
static AuraWM *detect_wm_ptr = NULL;
static int detect_wm_error(Display *dpy, XErrorEvent *e)
{
    (void)dpy;
    if (e->error_code == BadAccess) {
        if (detect_wm_ptr) detect_wm_ptr->another_wm = true;
    }
    return 0;
}

bool wm_init(AuraWM *wm, const char *display_name)
{
    memset(wm, 0, sizeof(*wm));

    // Connect to X server
    wm->dpy = XOpenDisplay(display_name);
    if (!wm->dpy) {
        fprintf(stderr, "[aura-wm] Cannot open display '%s'\n",
                display_name ? display_name : ":0");
        return false;
    }

    wm->screen = DefaultScreen(wm->dpy);
    wm->root = RootWindow(wm->dpy, wm->screen);
    wm->root_w = DisplayWidth(wm->dpy, wm->screen);
    wm->root_h = DisplayHeight(wm->dpy, wm->screen);

    fprintf(stderr, "[aura-wm] Connected to display, root=%lu, %dx%d\n",
            wm->root, wm->root_w, wm->root_h);

    // Intern all atoms before anything else
    wm_intern_atoms(wm);

    // Detect if another WM is running by trying to select
    // SubstructureRedirectMask on root. Only one client can do this.
    detect_wm_ptr = wm;
    wm->another_wm = false;
    XSetErrorHandler(detect_wm_error);
    XSelectInput(wm->dpy, wm->root,
                 SubstructureRedirectMask | SubstructureNotifyMask |
                 StructureNotifyMask | PropertyChangeMask |
                 FocusChangeMask);
    XSync(wm->dpy, False);
    XSetErrorHandler(x_error_handler);

    if (wm->another_wm) {
        fprintf(stderr, "[aura-wm] Another window manager is running. Exiting.\n");
        XCloseDisplay(wm->dpy);
        return false;
    }

    fprintf(stderr, "[aura-wm] Claimed window manager role\n");

    // Set EWMH properties on root
    ewmh_setup(wm);

    wm->running = true;
    return true;
}

void wm_shutdown(AuraWM *wm)
{
    // Unframe all clients (reparent back to root) so apps survive WM restart
    for (int i = 0; i < wm->num_clients; i++) {
        Client *c = &wm->clients[i];
        if (c->frame) {
            XReparentWindow(wm->dpy, c->client, wm->root, c->x, c->y);
            XRemoveFromSaveSet(wm->dpy, c->client);
            XDestroyWindow(wm->dpy, c->frame);
            c->frame = 0;
        }
    }
    XSync(wm->dpy, False);
    XCloseDisplay(wm->dpy);
    fprintf(stderr, "[aura-wm] Shutdown complete\n");
}

void wm_intern_atoms(AuraWM *wm)
{
    Display *d = wm->dpy;
    wm->atom_wm_protocols        = XInternAtom(d, "WM_PROTOCOLS", False);
    wm->atom_wm_delete           = XInternAtom(d, "WM_DELETE_WINDOW", False);
    wm->atom_wm_name             = XInternAtom(d, "WM_NAME", False);
    wm->atom_net_wm_name         = XInternAtom(d, "_NET_WM_NAME", False);
    wm->atom_net_wm_type         = XInternAtom(d, "_NET_WM_WINDOW_TYPE", False);
    wm->atom_net_wm_type_normal  = XInternAtom(d, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    wm->atom_net_wm_type_dock    = XInternAtom(d, "_NET_WM_WINDOW_TYPE_DOCK", False);
    wm->atom_net_wm_type_desktop = XInternAtom(d, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    wm->atom_net_wm_type_dialog  = XInternAtom(d, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    wm->atom_net_wm_type_splash  = XInternAtom(d, "_NET_WM_WINDOW_TYPE_SPLASH", False);
    wm->atom_net_wm_type_utility = XInternAtom(d, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    wm->atom_net_wm_state        = XInternAtom(d, "_NET_WM_STATE", False);
    wm->atom_net_wm_state_fullscreen = XInternAtom(d, "_NET_WM_STATE_FULLSCREEN", False);
    wm->atom_net_wm_state_hidden = XInternAtom(d, "_NET_WM_STATE_HIDDEN", False);
    wm->atom_net_active_window   = XInternAtom(d, "_NET_ACTIVE_WINDOW", False);
    wm->atom_net_client_list     = XInternAtom(d, "_NET_CLIENT_LIST", False);
    wm->atom_net_client_list_stacking = XInternAtom(d, "_NET_CLIENT_LIST_STACKING", False);
    wm->atom_net_frame_extents   = XInternAtom(d, "_NET_FRAME_EXTENTS", False);
    wm->atom_net_supported       = XInternAtom(d, "_NET_SUPPORTED", False);
    wm->atom_net_supporting_wm_check = XInternAtom(d, "_NET_SUPPORTING_WM_CHECK", False);
    wm->atom_net_wm_allowed_actions = XInternAtom(d, "_NET_WM_ALLOWED_ACTIONS", False);
    wm->atom_net_close_window    = XInternAtom(d, "_NET_CLOSE_WINDOW", False);
    wm->atom_net_wm_strut        = XInternAtom(d, "_NET_WM_STRUT", False);
    wm->atom_net_wm_strut_partial = XInternAtom(d, "_NET_WM_STRUT_PARTIAL", False);
    wm->atom_utf8_string         = XInternAtom(d, "UTF8_STRING", False);
}

Client *wm_find_client(AuraWM *wm, Window w)
{
    for (int i = 0; i < wm->num_clients; i++) {
        if (wm->clients[i].client == w)
            return &wm->clients[i];
    }
    return NULL;
}

Client *wm_find_client_by_frame(AuraWM *wm, Window frame)
{
    for (int i = 0; i < wm->num_clients; i++) {
        if (wm->clients[i].frame == frame)
            return &wm->clients[i];
    }
    return NULL;
}

Client *wm_add_client(AuraWM *wm, Window client_window)
{
    if (wm->num_clients >= MAX_CLIENTS) {
        fprintf(stderr, "[aura-wm] Max clients reached!\n");
        return NULL;
    }

    Client *c = &wm->clients[wm->num_clients++];
    memset(c, 0, sizeof(*c));
    c->client = client_window;
    return c;
}

void wm_remove_client(AuraWM *wm, Client *c)
{
    int idx = (int)(c - wm->clients);
    if (idx < 0 || idx >= wm->num_clients) return;

    if (wm->focused == c) wm->focused = NULL;

    // Shift remaining clients down
    for (int i = idx; i < wm->num_clients - 1; i++) {
        wm->clients[i] = wm->clients[i + 1];
    }
    wm->num_clients--;
}

void wm_focus_client(AuraWM *wm, Client *c)
{
    if (wm->focused && wm->focused != c) {
        wm_unfocus_client(wm, wm->focused);
    }
    if (c) {
        XSetInputFocus(wm->dpy, c->client, RevertToPointerRoot, CurrentTime);
        XRaiseWindow(wm->dpy, c->frame);
        c->focused = true;
        wm->focused = c;

        // Update _NET_ACTIVE_WINDOW
        XChangeProperty(wm->dpy, wm->root, wm->atom_net_active_window,
                        XA_WINDOW, 32, PropModeReplace,
                        (unsigned char *)&c->client, 1);
    }
}

void wm_unfocus_client(AuraWM *wm, Client *c)
{
    if (c) {
        c->focused = false;
        // Redraw decoration in inactive state will happen on FocusOut event
    }
}
