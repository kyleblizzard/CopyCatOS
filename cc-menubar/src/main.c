// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// main.c — Entry point for the CopiCatOS menu bar
//
// This is a very thin wrapper. It:
//   1. Sets up signal handlers so we can shut down cleanly (e.g., on Ctrl+C)
//   2. Initializes the menu bar (which opens the X display, creates the
//      dock window, and sets up all subsystems)
//   3. Enters the event loop (which runs until the process is told to stop)
//   4. Cleans up and exits
//
// The actual logic lives in menubar.c and the other modules.

#include <stdio.h>
#include <signal.h>
#include "menubar.h"

// Global pointer so the signal handler can reach the menu bar state.
// This is a common pattern in C programs — signal handlers can only
// access global variables, not local ones.
static MenuBar g_menubar;

// Signal handler for SIGINT (Ctrl+C) and SIGTERM (kill command).
// We just set the running flag to false, which causes the event loop
// in menubar_run() to exit gracefully on its next iteration.
static void handle_signal(int sig)
{
    (void)sig; // Suppress "unused parameter" warning
    g_menubar.running = false;
}

int main(void)
{
    // Register signal handlers for clean shutdown.
    // SIGINT = Ctrl+C from terminal, SIGTERM = default kill signal.
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Initialize the menu bar — this opens the X display, creates the
    // window, and sets up all subsystems (render, apple, appmenu, systray).
    if (!menubar_init(&g_menubar)) {
        fprintf(stderr, "cc-menubar: failed to initialize\n");
        return 1;
    }

    // Enter the event loop. This blocks until g_menubar.running is false.
    menubar_run(&g_menubar);

    // Clean up everything before exiting.
    menubar_shutdown(&g_menubar);

    return 0;
}
