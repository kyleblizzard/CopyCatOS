// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// main.c — Entry point for cc-sysprefs (System Preferences)
// ============================================================================
//
// Follows the standard CopyCatOS shell component pattern:
//   1. Acquire single-instance lock
//   2. Install signal handlers for clean shutdown
//   3. Initialize state (window, assets, registry)
//   4. Run the event loop
//   5. Clean up
// ============================================================================

#include "sysprefs.h"
#include "registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>

// Global pointer for signal handler access
static SysPrefsState *g_state = NULL;

// ----------------------------------------------------------------------------
// Signal handler — sets the running flag to false for a clean exit.
// We don't do any complex cleanup here because signal handlers must be
// async-signal-safe. The event loop checks state->running each iteration.
// ----------------------------------------------------------------------------
static void signal_handler(int sig)
{
    (void)sig;
    if (g_state) {
        g_state->running = false;
    }
}

// ----------------------------------------------------------------------------
// Single-instance lock — prevents multiple System Preferences windows.
// Uses a lock file in /tmp with flock(). Returns the file descriptor
// (caller must close it on exit) or -1 if another instance is running.
// ----------------------------------------------------------------------------
static int acquire_instance_lock(void)
{
    const char *lock_path = "/tmp/cc-sysprefs.lock";
    int fd = open(lock_path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        return -1;
    }

    // Try to acquire an exclusive lock without blocking
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

// ============================================================================
// main — Application entry point
// ============================================================================
int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    fprintf(stderr, "[cc-sysprefs] Starting System Preferences...\n");

    // Prevent duplicate instances
    int lock_fd = acquire_instance_lock();
    if (lock_fd < 0) {
        fprintf(stderr, "[cc-sysprefs] Another instance is already running.\n");
        return EXIT_FAILURE;
    }

    // Allocate state on the stack (same pattern as cc-dock)
    SysPrefsState state = {0};

    // Install signal handlers so Ctrl+C triggers a clean shutdown
    g_state = &state;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize X11 window, Cairo surface, and load assets
    if (!sysprefs_init(&state)) {
        fprintf(stderr, "[cc-sysprefs] Failed to initialize.\n");
        close(lock_fd);
        return EXIT_FAILURE;
    }

    // Register all 27 preference panes and load their icons
    registry_init(&state);
    registry_load_icons(&state);

    fprintf(stderr, "[cc-sysprefs] Registered %d panes in %d categories\n",
            state.pane_count, state.category_count);

    // Run the event loop (blocks until quit)
    sysprefs_run(&state);

    // Clean up everything
    sysprefs_cleanup(&state);
    close(lock_fd);

    fprintf(stderr, "[cc-sysprefs] Shut down cleanly.\n");
    return EXIT_SUCCESS;
}
