// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// main.c — Entry point for cc-dock
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

#define _GNU_SOURCE  // For lockf(), F_TLOCK
#include "dock.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

// Global pointer to the dock state so the signal handler can access it.
// This is needed for clean shutdown when the user presses Ctrl+C.
static DockState *g_state = NULL;

// ---------------------------------------------------------------------------
// Signal handler for SIGINT (Ctrl+C) and SIGTERM (kill command).
// Sets the running flag to false so the main loop exits gracefully.
// ---------------------------------------------------------------------------
volatile bool g_reload_config = false;

static void signal_handler(int sig)
{
    if (sig == SIGHUP) {
        // SIGHUP = reload config (sent by System Preferences when sizes change)
        g_reload_config = true;
    } else {
        // SIGINT/SIGTERM = clean shutdown
        if (g_state) {
            g_state->running_loop = false;
        }
    }
}

// ---------------------------------------------------------------------------
// Single-instance lock using a lock file.
// If another cc-dock process is already running, this prevents a second
// copy from starting — which would cause both to fight over the same screen
// space and produce the "double startup" / flickering symptom.
// Returns the lock file descriptor (>= 0) on success, or -1 if locked.
// ---------------------------------------------------------------------------
static int acquire_instance_lock(void)
{
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    char lock_path[512];
    snprintf(lock_path, sizeof(lock_path), "%s/.cache/cc-dock.lock", home);

    int fd = open(lock_path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return -1;

    // Try to get an exclusive lock without blocking.
    // If another instance holds the lock, lockf returns -1 immediately.
    if (lockf(fd, F_TLOCK, 0) < 0) {
        close(fd);
        return -1;
    }

    // Write our PID so debugging tools can identify the lock holder
    char pid_str[32];
    int len = snprintf(pid_str, sizeof(pid_str), "%d\n", (int)getpid());
    if (ftruncate(fd, 0) == 0) {
        (void)write(fd, pid_str, len);
    }

    // Keep fd open — closing it releases the lock
    return fd;
}

int main(int argc, char *argv[])
{
    // Suppress unused parameter warnings — we don't use command-line args yet
    (void)argc;
    (void)argv;

    // Prevent two instances from running simultaneously.
    // A second copy would fight the first for X events and screen space,
    // causing flickering, double redraws, and duplicate log messages.
    int lock_fd = acquire_instance_lock();
    if (lock_fd < 0) {
        fprintf(stderr, "AuraDock: another instance is already running.\n");
        return EXIT_FAILURE;
    }

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
    signal(SIGHUP, signal_handler);  // Reload config (sent by System Preferences)

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

    // Release the single-instance lock
    if (lock_fd >= 0) {
        close(lock_fd);
    }

    printf("AuraDock exited cleanly.\n");
    return EXIT_SUCCESS;
}
