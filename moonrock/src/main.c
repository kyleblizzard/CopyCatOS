// CopyCatOS — by Kyle Blizzard at Blizzard.show

// CopyCatOS Window Manager — Entry point
//
// A custom X11 reparenting compositing window manager that recreates
// Mac OS X Snow Leopard pixel-perfectly using real assets.
//
// Tech stack: C + Xlib + Cairo + OpenGL
// Everything outside app windows is rendered by this WM.
// Everything inside app windows is rendered by AquaStyle (C++ Qt6 plugin).

#include "wm.h"
#include "events.h"
#include "decor.h"
#include "input.h"
#include "moonrock.h"
#include "moonbase_host.h"
#include "moonbase_xdnd.h"
#include "struts.h"
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>

static CCWM wm;

static void handle_signal(int sig)
{
    (void)sig;
    fprintf(stderr, "[moonrock] Received signal %d, shutting down\n", sig);
    wm.running = false;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    fprintf(stderr, "╔══════════════════════════════════════╗\n");
    fprintf(stderr, "║         CopyCatOS Window Manager        ║\n");
    fprintf(stderr, "║   Pixel-Perfect Snow Leopard in C    ║\n");
    fprintf(stderr, "╚══════════════════════════════════════╝\n");

    // Handle shutdown signals gracefully
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);

    // Initialize the window manager
    const char *display = getenv("DISPLAY");
    if (!wm_init(&wm, display)) {
        return 1;
    }

    // Initialize the MoonRock Compositor for OpenGL compositing and window
    // shadows (optional — degrades gracefully if GLX/XComposite aren't available)
    if (!mr_init(&wm)) {
        fprintf(stderr, "[moonrock] MoonRock Compositor unavailable — no window shadows\n");
    }

    // Start the MoonBase IPC host. Degrades gracefully: if the socket
    // can't be opened (missing XDG_RUNTIME_DIR, permission denied,
    // etc.) moonrock still runs as a pure X11 compositor — MoonBase
    // apps just can't connect until the path is resolved.
    if (!mb_host_init(NULL, wm.dpy, wm.root)) {
        fprintf(stderr, "[moonrock] MoonBase host unavailable — no .app support\n");
    }

    // XDND → MB_IPC_DRAG_* bridge. Caches the XDND atom set and arms
    // the state machine that forwards XdndEnter/Position/Leave/Drop
    // ClientMessages (on MoonBase InputOnly proxies) into IPC drag
    // frames. Always initialized even if the IPC host failed — it
    // still provides safe no-op return paths for events.c.
    mb_xdnd_init(wm.dpy, wm.root);

    // Set up strut handling so dock/menubar can reserve screen edges
    struts_init(&wm);

    // Wire the struts recalc into display's scales-publish event so that
    // hotplug / primary-swap / rotation rebuild the per-output workarea
    // table automatically. Must run AFTER struts_init so atom_net_workarea
    // is interned before the first thunk can fire.
    struts_register_geometry_hook(&wm);

    // Load Snow Leopard decoration assets
    decor_init(&wm);

    // Set up keyboard shortcuts
    input_setup_grabs(&wm);

    // Run the event loop (blocks until wm.running = false)
    events_run(&wm);

    // Clean shutdown — unframe all windows so apps survive restart
    mb_host_shutdown();
    decor_shutdown();
    mr_shutdown(&wm);
    wm_shutdown(&wm);

    return 0;
}
