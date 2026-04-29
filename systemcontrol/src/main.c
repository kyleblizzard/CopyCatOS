// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// main.c — Entry point for systemcontrol (System Preferences)
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
#include "shellconf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    const char *lock_path = "/tmp/systemcontrol.lock";
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
    // Parse --pane flag so external scripts can open directly to a pane.
    // Usage: systemcontrol --pane controller
    // If no --pane is given, the icon grid is shown (default behavior).
    const char *open_pane = NULL;
    bool publish_atoms_only = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pane") == 0 && i + 1 < argc) {
            open_pane = argv[i + 1];
            i++;  // Skip the pane name argument
        } else if (strcmp(argv[i], "--publish-atoms") == 0) {
            publish_atoms_only = true;
        }
    }

    // Headless mode: load shell.conf and publish the root atoms, then exit.
    // Invoked from moonrock-session.sh on session start so menubar / desktop
    // / moonrock see the persisted menu-bar + Spaces modes before they
    // create their windows. systemcontrol owns the conf→atom mapping; this
    // is the same path the GUI takes on launch (lines below) — sharing it
    // keeps the two from drifting.
    if (publish_atoms_only) {
        Display *dpy = XOpenDisplay(NULL);
        if (!dpy) {
            fprintf(stderr,
                "[systemcontrol] --publish-atoms: cannot open display\n");
            return EXIT_FAILURE;
        }
        Window root = DefaultRootWindow(dpy);
        ShellConf shell = {0};
        shellconf_load(&shell);
        shellconf_publish_atoms(dpy, root, &shell);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        fprintf(stderr,
            "[systemcontrol] published shell atoms (menu-bar=%s, spaces=%s)\n",
            shell.displays_separate_menu_bars ? "modern" : "classic",
            shell.displays_separate_spaces    ? "per_display" : "global");
        return EXIT_SUCCESS;
    }

    fprintf(stderr, "[systemcontrol] Starting System Preferences...\n");

    // Prevent duplicate instances
    int lock_fd = acquire_instance_lock();
    if (lock_fd < 0) {
        fprintf(stderr, "[systemcontrol] Another instance is already running.\n");
        return EXIT_FAILURE;
    }

    // Allocate state on the stack (same pattern as dock)
    SysPrefsState state = {0};

    // Install signal handlers so Ctrl+C triggers a clean shutdown
    g_state = &state;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize X11 window, Cairo surface, and load assets
    if (!sysprefs_init(&state)) {
        fprintf(stderr, "[systemcontrol] Failed to initialize.\n");
        close(lock_fd);
        return EXIT_FAILURE;
    }

    // Register all 27 preference panes and load their icons
    registry_init(&state);
    registry_load_icons(&state);

    fprintf(stderr, "[systemcontrol] Registered %d panes in %d categories\n",
            state.pane_count, state.category_count);

    // Rehydrate shell-wide toggles on every launch. This ensures a fresh
    // X session picks up the saved menu-bar / Spaces modes even before the
    // user opens the Desktop & Dock pane — the atoms are the single source
    // of truth at runtime, shell.conf is the single source of truth on disk.
    {
        ShellConf shell = {0};
        shellconf_load(&shell);
        shellconf_publish_atoms(state.dpy, state.root, &shell);
    }

    // If --pane was specified, open directly to that pane instead of the grid.
    // We search by pane ID (e.g. "mouse", "controller", "dock").
    if (open_pane) {
        bool found = false;
        for (int i = 0; i < state.pane_count; i++) {
            if (strcmp(state.panes[i].id, open_pane) == 0) {
                sysprefs_open_pane(&state, i);
                found = true;
                break;
            }
        }
        // Alias: "controller" maps to the "mouse" pane ID in the registry.
        // This lets desktop shortcuts use the intuitive name while the
        // registry keeps the macOS-style "Mouse & Keyboard" icon slot.
        if (!found && strcmp(open_pane, "controller") == 0) {
            for (int i = 0; i < state.pane_count; i++) {
                if (strcmp(state.panes[i].id, "mouse") == 0) {
                    sysprefs_open_pane(&state, i);
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            fprintf(stderr, "[systemcontrol] Unknown pane: %s\n", open_pane);
        }
    }

    // Run the event loop (blocks until quit)
    sysprefs_run(&state);

    // Clean up everything
    sysprefs_cleanup(&state);
    close(lock_fd);

    fprintf(stderr, "[systemcontrol] Shut down cleanly.\n");
    return EXIT_SUCCESS;
}
