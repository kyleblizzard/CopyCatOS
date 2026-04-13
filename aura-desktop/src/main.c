// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// main.c — Entry point for the aura-desktop component
//
// aura-desktop is the wallpaper surface and desktop icon grid for AuraOS.
// It runs as a standalone process, separate from the window manager.
// It creates a full-screen window with type _NET_WM_WINDOW_TYPE_DESKTOP,
// which tells the WM to place it below all other windows and skip framing.
//
// Usage:
//   aura-desktop                          # use default wallpaper
//   aura-desktop --wallpaper /path/to.jpg # override wallpaper image

#include "desktop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

// Global pointer so the signal handler can tell the event loop to stop.
// We need this because signal handlers can't take custom arguments.
static Desktop *g_desktop = NULL;

// Signal handler for clean shutdown.
// When the user hits Ctrl+C or the system sends SIGTERM (e.g., during
// logout), we set the running flag to false so the event loop exits
// gracefully instead of just dying.
static void handle_signal(int sig)
{
    (void)sig;  // Mark as intentionally unused
    fprintf(stderr, "[aura-desktop] Received signal %d, shutting down\n", sig);
    if (g_desktop) {
        g_desktop->running = false;
    }
}

int main(int argc, char *argv[])
{
    const char *wallpaper_path = NULL;

    // Parse command-line arguments.
    // The only option we support is --wallpaper to override the default image.
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--wallpaper") == 0 && i + 1 < argc) {
            wallpaper_path = argv[++i];
        } else {
            fprintf(stderr, "Usage: aura-desktop [--wallpaper PATH]\n");
            return 1;
        }
    }

    fprintf(stderr, "╔══════════════════════════════════════╗\n");
    fprintf(stderr, "║        AuraOS Desktop Surface        ║\n");
    fprintf(stderr, "║   Wallpaper + Icons + Context Menu   ║\n");
    fprintf(stderr, "╚══════════════════════════════════════╝\n");

    // Create the desktop instance on the stack.
    // This struct holds all the X resources and state for the desktop.
    Desktop desktop = {0};
    g_desktop = &desktop;

    // Install signal handlers so we can clean up on shutdown.
    // SIGINT = Ctrl+C, SIGTERM = kill command / system shutdown.
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Initialize everything: X display, window, wallpaper, icons.
    if (!desktop_init(&desktop, wallpaper_path)) {
        fprintf(stderr, "[aura-desktop] Failed to initialize\n");
        return 1;
    }

    // Enter the main event loop (blocks until desktop.running = false).
    desktop_run(&desktop);

    // Clean up all resources before exiting.
    desktop_shutdown(&desktop);

    fprintf(stderr, "[aura-desktop] Shutdown complete\n");
    return 0;
}
