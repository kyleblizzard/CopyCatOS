// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// main.c — Entry point for aura-dock
//
// This is the starting point of the dock application. It does three things:
//   1. Initialize the dock (create window, load icons, set up X properties)
//   2. Run the main event loop (handle mouse, draw frames, animate)
//   3. Clean up when the dock exits
//
// The dock is a single-process C application that uses Xlib for window
// management, Cairo for 2D rendering, and Pango for text. It creates a
// transparent (32-bit ARGB) window at the bottom of the screen and draws
// the classic Snow Leopard dock with glass shelf, magnification, bounce
// animations, icon reflections, and running indicators.
// ============================================================================

#include "dock.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

// Global pointer to the dock state so the signal handler can access it.
// This is needed for clean shutdown when the user presses Ctrl+C.
static DockState *g_state = NULL;

// ---------------------------------------------------------------------------
// Signal handler for SIGINT (Ctrl+C) and SIGTERM (kill command).
// Sets the running flag to false so the main loop exits gracefully.
// ---------------------------------------------------------------------------
static void signal_handler(int sig)
{
    (void)sig;  // Suppress unused parameter warning
    if (g_state) {
        g_state->running_loop = false;
    }
}

int main(int argc, char *argv[])
{
    // Suppress unused parameter warnings — we don't use command-line args yet
    (void)argc;
    (void)argv;

    printf("AuraDock v0.1.0 starting...\n");

    // Allocate the dock state on the stack. This struct holds everything:
    // X11 handles, Cairo surfaces, dock items, mouse state, etc.
    DockState state;

    // Set up signal handlers for clean shutdown.
    // When the user presses Ctrl+C, we want to exit the event loop
    // gracefully instead of just terminating (which would leak X resources).
    g_state = &state;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize the dock: open X display, create window, load icons
    if (!dock_init(&state)) {
        fprintf(stderr, "Failed to initialize dock\n");
        return EXIT_FAILURE;
    }

    printf("Dock initialized: %d items, window %dx%d at (%d,%d)\n",
           state.item_count, state.win_w, state.win_h,
           state.win_x, state.win_y);

    // Run the main event loop. This blocks until the dock is closed.
    dock_run(&state);

    // Clean up all resources (surfaces, X connection, etc.)
    dock_cleanup(&state);

    printf("AuraDock exited cleanly.\n");
    return EXIT_SUCCESS;
}
