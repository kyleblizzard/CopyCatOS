// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// main.c — Entry point for AuraFinder
//
// AuraFinder is the Snow Leopard-style file manager for AuraOS.
// It creates a normal window that aura-wm frames with Snow Leopard
// title bar chrome. The window has three zones:
//
//   1. Toolbar  (top 30px)  — view buttons, breadcrumb path, search field
//   2. Sidebar  (left 200px) — DEVICES and PLACES source list
//   3. Content  (remaining)  — icon grid showing directory contents
//
// Usage:
//   aura-finder                    # open at $HOME
//   aura-finder /path/to/dir       # open at specific directory
//   aura-finder ~/Documents        # open Documents folder

#include "finder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

// Global pointer so the signal handler can set running = false.
// Signal handlers can't take custom arguments, so we stash the
// FinderState pointer here during setup.
static FinderState *g_finder = NULL;

// Signal handler for clean shutdown.
// When the user presses Ctrl+C or the system sends SIGTERM (e.g.,
// during logout), we set running to false so the event loop exits
// gracefully instead of just dying mid-paint.
static void handle_signal(int sig)
{
    (void)sig;  // Mark parameter as intentionally unused
    fprintf(stderr, "[aura-finder] Received signal %d, shutting down\n", sig);
    if (g_finder) {
        g_finder->running = false;
    }
}

int main(int argc, char *argv[])
{
    const char *initial_path = NULL;

    // Parse command-line arguments.
    // The only argument we accept is an optional directory path.
    // If no path is given, we default to $HOME.
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            // Unknown flag — show usage
            fprintf(stderr, "Usage: aura-finder [PATH]\n");
            return 1;
        } else {
            // Treat as directory path
            initial_path = argv[i];
        }
    }

    fprintf(stderr, "╔══════════════════════════════════════╗\n");
    fprintf(stderr, "║          AuraOS Finder v0.1          ║\n");
    fprintf(stderr, "║     Snow Leopard File Manager        ║\n");
    fprintf(stderr, "╚══════════════════════════════════════╝\n");

    // Create the FinderState on the stack.
    // This struct holds all X resources, Cairo context, and module state.
    FinderState finder = {0};
    g_finder = &finder;

    // Install signal handlers for graceful shutdown.
    // SIGINT = Ctrl+C, SIGTERM = kill command / system shutdown.
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Initialize: open X display, create window, set up Cairo,
    // load sidebar items, scan initial directory.
    if (!finder_init(&finder, initial_path)) {
        fprintf(stderr, "[aura-finder] Failed to initialize\n");
        return 1;
    }

    // Enter the main event loop (blocks until finder.running = false).
    // All painting, clicking, and navigation happens inside here.
    finder_run(&finder);

    // Clean up all resources before exiting.
    finder_shutdown(&finder);

    fprintf(stderr, "[aura-finder] Shutdown complete\n");
    return 0;
}
