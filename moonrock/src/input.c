// CopyCatOS — by Kyle Blizzard at Blizzard.show

// CopyCatOS Window Manager — Keyboard input (macOS-style shortcuts)

#include "input.h"
#include "ewmh.h"
#include <X11/keysym.h>
#include <stdio.h>

// We use Super (Mod4) as the Command key equivalent
#define MOD_KEY Mod4Mask

void input_setup_grabs(CCWM *wm)
{
    // Grab Super+Q (close window)
    XGrabKey(wm->dpy, XKeysymToKeycode(wm->dpy, XK_q),
             MOD_KEY, wm->root, True, GrabModeAsync, GrabModeAsync);

    // Grab Super+W (close window)
    XGrabKey(wm->dpy, XKeysymToKeycode(wm->dpy, XK_w),
             MOD_KEY, wm->root, True, GrabModeAsync, GrabModeAsync);

    // Grab Super+M (minimize)
    XGrabKey(wm->dpy, XKeysymToKeycode(wm->dpy, XK_m),
             MOD_KEY, wm->root, True, GrabModeAsync, GrabModeAsync);

    // Grab Super+H (hide/minimize all)
    XGrabKey(wm->dpy, XKeysymToKeycode(wm->dpy, XK_h),
             MOD_KEY, wm->root, True, GrabModeAsync, GrabModeAsync);

    // Grab Super+Tab (switch windows)
    XGrabKey(wm->dpy, XKeysymToKeycode(wm->dpy, XK_Tab),
             MOD_KEY, wm->root, True, GrabModeAsync, GrabModeAsync);

    fprintf(stderr, "[moonrock] Key grabs set (Super+Q/W/M/H/Tab)\n");
}

void input_handle_key(CCWM *wm, XKeyEvent *e)
{
    KeySym sym = XLookupKeysym(e, 0);

    if (!(e->state & MOD_KEY)) return;

    switch (sym) {
    case XK_q:
        // Quit focused app — close ALL windows of the same WM_CLASS (Mac Cmd+Q)
        if (wm->focused) {
            extern void client_close_app(CCWM *wm, Client *c);
            client_close_app(wm, wm->focused);
        }
        break;

    case XK_w:
        // Close focused window only (Mac Cmd+W)
        if (wm->focused) {
            ewmh_send_delete(wm, wm->focused->client);
        }
        break;

    case XK_m:
        // Minimize focused window
        if (wm->focused && wm->focused->frame) {
            XUnmapWindow(wm->dpy, wm->focused->frame);
            wm->focused->mapped = false;
            ewmh_update_client_list(wm);
        }
        break;

    case XK_h:
        // Hide focused app — minimize ALL windows of the same app (macOS Cmd+H)
        if (wm->focused) {
            extern void client_hide_app(CCWM *wm, Client *c);
            client_hide_app(wm, wm->focused);
            ewmh_update_client_list(wm);
        }
        break;

    case XK_Tab:
        // Cycle through windows
        if (wm->num_clients > 1) {
            // Find next mapped client after focused
            int start = wm->focused ? (int)(wm->focused - wm->clients) : -1;
            for (int i = 1; i <= wm->num_clients; i++) {
                int idx = (start + i) % wm->num_clients;
                Client *c = &wm->clients[idx];
                if (c->mapped && c->frame) {
                    XMapWindow(wm->dpy, c->frame);
                    wm_focus_client(wm, c);
                    break;
                }
            }
        }
        break;
    }
}
