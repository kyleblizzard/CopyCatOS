// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
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
#include "struts.h"
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>

static CCWM wm;

static void handle_signal(int sig)
{
    (void)sig;
    fprintf(stderr, "[cc-wm] Received signal %d, shutting down\n", sig);
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
        fprintf(stderr, "[cc-wm] MoonRock Compositor unavailable — no window shadows\n");
    }

    // Set up strut handling so dock/menubar can reserve screen edges
    struts_init(&wm);

    // Load Snow Leopard decoration assets
    decor_init(&wm);

    // Set up keyboard shortcuts
    input_setup_grabs(&wm);

    // Run the event loop (blocks until wm.running = false)
    events_run(&wm);

    // Clean shutdown — unframe all windows so apps survive restart
    decor_shutdown();
    mr_shutdown(&wm);
    wm_shutdown(&wm);

    return 0;
}
