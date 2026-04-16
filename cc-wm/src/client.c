// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.
//
// CopyCatOS Window Manager — Client window tracking and app grouping
//
// This module handles WM_CLASS reading (so we know which app owns a window)
// and app grouping (so Super+H can hide ALL windows of the focused app).

#include "wm.h"
#include <stdio.h>
#include <string.h>
#include <X11/Xutil.h>

// Read WM_CLASS from a client window and store in the Client struct.
// WM_CLASS has two null-terminated strings: instance name and class name.
// Example: "kate\0Kate\0" — instance is "kate", class is "Kate".
void client_read_wm_class(CCWM *wm, Client *c)
{
    c->wm_class[0] = '\0';
    c->wm_class_name[0] = '\0';

    XClassHint hint;
    if (XGetClassHint(wm->dpy, c->client, &hint)) {
        if (hint.res_name) {
            snprintf(c->wm_class, sizeof(c->wm_class), "%s", hint.res_name);
            XFree(hint.res_name);
        }
        if (hint.res_class) {
            snprintf(c->wm_class_name, sizeof(c->wm_class_name), "%s", hint.res_class);
            XFree(hint.res_class);
        }
    }
}

// Hide all windows belonging to the same app as the given client.
// "Same app" means matching wm_class_name (the class, not the instance).
// This implements macOS Cmd+H behavior.
void client_hide_app(CCWM *wm, Client *c)
{
    if (!c || c->wm_class_name[0] == '\0') return;

    const char *app_class = c->wm_class_name;
    int hidden = 0;

    for (int i = 0; i < wm->num_clients; i++) {
        Client *other = &wm->clients[i];
        if (other->mapped && other->frame &&
            strcmp(other->wm_class_name, app_class) == 0) {
            XUnmapWindow(wm->dpy, other->frame);
            other->mapped = false;
            hidden++;
        }
    }

    if (hidden > 0) {
        fprintf(stderr, "[cc-wm] Hid %d window(s) of app '%s'\n", hidden, app_class);
        // Update focus to next available window
        wm->focused = NULL;
        for (int i = wm->num_clients - 1; i >= 0; i--) {
            if (wm->clients[i].mapped && wm->clients[i].frame) {
                wm_focus_client(wm, &wm->clients[i]);
                break;
            }
        }
    }
}

// Smart zoom: toggle between saved geometry and maximized to work area.
// On first zoom: save current geometry, maximize to work area.
// On second zoom: restore saved geometry.
void client_smart_zoom(CCWM *wm, Client *c)
{
    if (!c || !c->frame) return;

    if (c->zoomed) {
        // Restore saved geometry
        c->x = c->saved_x;
        c->y = c->saved_y;
        c->w = c->saved_w;
        c->h = c->saved_h;
        c->zoomed = false;
    } else {
        // Save current geometry
        c->saved_x = c->x;
        c->saved_y = c->y;
        c->saved_w = c->w;
        c->saved_h = c->h;

        // Get work area from struts (declared in struts.h)
        // If struts aren't available, use full screen minus some padding
        int wa_x = 0, wa_y = 0, wa_w = wm->root_w, wa_h = wm->root_h;
        // Try to get work area — extern declaration
        extern void struts_get_workarea(CCWM *wm, int *x, int *y, int *w, int *h);
        struts_get_workarea(wm, &wa_x, &wa_y, &wa_w, &wa_h);

        c->x = wa_x;
        c->y = wa_y;
        c->w = wa_w - 2 * BORDER_WIDTH;
        c->h = wa_h - TITLEBAR_HEIGHT - BORDER_WIDTH;
        c->zoomed = true;
    }

    // Apply the new geometry
    int sl = 0, sr = 0, st = 0, sb = 0;
    extern bool compositor_active;
    if (compositor_active) {
        sl = 20; sr = 20; st = 17; sb = 23; // SHADOW extents
    }

    XMoveResizeWindow(wm->dpy, c->frame,
                      c->x, c->y,
                      c->w + 2 * BORDER_WIDTH + sl + sr,
                      c->h + TITLEBAR_HEIGHT + BORDER_WIDTH + st + sb);
    XMoveResizeWindow(wm->dpy, c->client,
                      sl + BORDER_WIDTH, st + TITLEBAR_HEIGHT,
                      c->w, c->h);

    fprintf(stderr, "[cc-wm] Smart zoom: %s → %dx%d+%d+%d\n",
            c->zoomed ? "maximized" : "restored",
            c->w, c->h, c->x, c->y);
}
