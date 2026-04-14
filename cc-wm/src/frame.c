// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// CopiCatOS Window Manager — Frame management and decoration rendering

#include "frame.h"
#include "decor.h"
#include "ewmh.h"
#include "moonrock.h"
#include "struts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void frame_window(CCWM *wm, Window client)
{
    // Get current window attributes
    XWindowAttributes wa;
    if (!XGetWindowAttributes(wm->dpy, client, &wa)) return;

    // Don't frame override-redirect windows (menus, tooltips, popups)
    if (wa.override_redirect) return;

    // Check window type — skip docks and desktops
    Atom type = ewmh_get_window_type(wm, client);
    if (type == wm->atom_net_wm_type_dock ||
        type == wm->atom_net_wm_type_desktop) {
        XMapWindow(wm->dpy, client);
        return;
    }

    // Calculate frame dimensions — when compositor is active, the frame
    // includes extra padding on all sides for the drop shadow
    int shadow_l = compositor_active ? SHADOW_LEFT : 0;
    int shadow_r = compositor_active ? SHADOW_RIGHT : 0;
    int shadow_t = compositor_active ? SHADOW_TOP : 0;
    int shadow_b = compositor_active ? SHADOW_BOTTOM : 0;

    int frame_w = wa.width + 2 * BORDER_WIDTH + shadow_l + shadow_r;
    int frame_h = wa.height + TITLEBAR_HEIGHT + BORDER_WIDTH + shadow_t + shadow_b;

    // Where the client sits inside the frame (offset by shadow + chrome)
    int client_in_frame_x = shadow_l + BORDER_WIDTH;
    int client_in_frame_y = shadow_t + TITLEBAR_HEIGHT;

    // Create the frame window — use ARGB visual if compositor is active
    // so the shadow region can be semi-transparent
    XSetWindowAttributes attrs;
    attrs.border_pixel = 0;
    attrs.event_mask = SubstructureRedirectMask | SubstructureNotifyMask |
                       ExposureMask | ButtonPressMask | ButtonReleaseMask |
                       PointerMotionMask | EnterWindowMask | FocusChangeMask;

    Visual *frame_visual = CopyFromParent;
    int frame_depth = CopyFromParent;
    unsigned long attr_mask = CWBorderPixel | CWEventMask;

    if (compositor_active) {
        // Get 32-bit ARGB visual for transparent shadow regions
        Visual *argb_visual = NULL;
        Colormap argb_cmap;
        if (mr_create_argb_visual(wm, &argb_visual, &argb_cmap)) {
            frame_visual = argb_visual;
            frame_depth = 32;
            attrs.colormap = argb_cmap;
            attrs.background_pixel = 0;  // Transparent black
            attr_mask |= CWColormap | CWBackPixel;
        }
    } else {
        attrs.background_pixel = 0xE0E0E0;
        attr_mask |= CWBackPixel;
    }

    // Clamp initial position to work area
    int pos_x = wa.x;
    int pos_y = wa.y;
    struts_clamp_to_workarea(wm, &pos_x, &pos_y, frame_w, frame_h);

    Window frame = XCreateWindow(
        wm->dpy, wm->root,
        pos_x, pos_y,
        frame_w, frame_h,
        0,                          // No X11 border (we draw our own)
        frame_depth,
        InputOutput,
        frame_visual,
        attr_mask,
        &attrs);

    // Add client to save set (so it survives WM crash/restart)
    XAddToSaveSet(wm->dpy, client);

    // Reparent client into frame (offset by shadow padding + chrome)
    XReparentWindow(wm->dpy, client, frame, client_in_frame_x, client_in_frame_y);

    // Remove client border (we handle all chrome)
    XSetWindowBorderWidth(wm->dpy, client, 0);

    // Map both
    XMapWindow(wm->dpy, frame);
    XMapWindow(wm->dpy, client);

    // Track the client
    Client *c = wm_add_client(wm, client);
    if (!c) return;

    c->frame = frame;
    c->x = pos_x;
    c->y = pos_y;
    c->w = wa.width;
    c->h = wa.height;
    c->mapped = true;

    // Get the window title
    ewmh_get_title(wm, client, c->title, sizeof(c->title));

    // Read WM_CLASS so we know which app this window belongs to
    extern void client_read_wm_class(CCWM *wm, Client *c);
    client_read_wm_class(wm, c);

    // Set _NET_FRAME_EXTENTS on the client
    ewmh_set_frame_extents(wm, client);

    // Set input shape so clicks in the shadow region pass through
    if (compositor_active) {
        mr_set_input_shape(wm, c);
    }

    // Update EWMH client list
    ewmh_update_client_list(wm);

    // Focus the new window
    wm_focus_client(wm, c);

    // Draw the initial decoration
    frame_redraw_decor(wm, c);

    // Tell MoonRock Compositor about the new window so it can create a
    // texture and start compositing it. This must happen AFTER the frame
    // is fully set up (mapped, decorated, input shape configured) so
    // MoonRock can read valid window attributes and pixmap data.
    if (mr_is_active()) {
        mr_window_mapped(wm, c);
    }

    if (getenv("AURA_DEBUG")) {
        fprintf(stderr, "[cc-wm] Framed window '%s' client=0x%lx frame=0x%lx %dx%d+%d+%d\n",
                c->title, client, frame, c->w, c->h, c->x, c->y);
    }
}

void unframe_window(CCWM *wm, Client *c)
{
    if (!c || !c->frame) return;

    // Tell MoonRock to stop tracking this window BEFORE we destroy the frame.
    // MoonRock needs the frame window ID to find and release the associated
    // OpenGL texture and XDamage handle. Once the frame is destroyed, the
    // window ID is invalid and MoonRock can't clean up properly.
    if (mr_is_active()) {
        mr_window_unmapped(wm, c);
    }

    // Reparent client back to root
    XReparentWindow(wm->dpy, c->client, wm->root, c->x, c->y);
    XRemoveFromSaveSet(wm->dpy, c->client);

    // Destroy the frame
    XDestroyWindow(wm->dpy, c->frame);
    c->frame = 0;

    if (getenv("AURA_DEBUG")) {
        fprintf(stderr, "[cc-wm] Unframed window '%s' client=0x%lx\n",
                c->title, c->client);
    }

    wm_remove_client(wm, c);
    ewmh_update_client_list(wm);
}

void frame_existing_windows(CCWM *wm)
{
    // At WM startup, frame all pre-existing top-level windows
    Window root_ret, parent_ret;
    Window *children = NULL;
    unsigned int nchildren = 0;

    XQueryTree(wm->dpy, wm->root, &root_ret, &parent_ret,
               &children, &nchildren);

    for (unsigned int i = 0; i < nchildren; i++) {
        XWindowAttributes wa;
        if (!XGetWindowAttributes(wm->dpy, children[i], &wa)) continue;

        // Skip unmapped windows and override-redirect
        if (wa.override_redirect || wa.map_state != IsViewable) continue;

        frame_window(wm, children[i]);
    }

    if (children) XFree(children);

    fprintf(stderr, "[cc-wm] Framed %d existing windows\n", wm->num_clients);
}

void frame_redraw_decor(CCWM *wm, Client *c)
{
    if (!c || !c->frame) return;
    decor_paint(wm, c);
}
