// CopyCatOS — by Kyle Blizzard at Blizzard.show

//
// main.c — Entry point for inputd
//
// inputd is the CopyCatOS input daemon. It reads raw gamepad events from
// the Lenovo Legion Go's built-in controllers, translates them into mouse
// movements, keyboard presses, and CopyCatOS shell actions, and injects
// the results through Linux uinput virtual devices.
//
// This file handles:
//   - Signal setup (SIGINT/SIGTERM for clean shutdown, SIGHUP for config reload)
//   - Single-instance enforcement via flock()
//   - Daemon initialization, main loop, and teardown
//
// Usage:
//   inputd           Run the daemon (requires root for /dev/input + /dev/uinput)
//   inputd --version Print version and exit
//   inputd --help    Print usage and exit
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/file.h>    // flock()

#include "inputd.h"

// --------------------------------------------------------------------------
// Version string — updated with each release
// --------------------------------------------------------------------------
#define INPUTD_VERSION "0.1.0"

// --------------------------------------------------------------------------
// Lock file path — prevents two instances from running simultaneously
// --------------------------------------------------------------------------
// We use flock() on this file to ensure only one inputd process runs
// at a time. If a second instance tries to start, flock() will fail with
// EWOULDBLOCK and we exit with an error message.
// --------------------------------------------------------------------------
#define LOCK_FILE "/tmp/inputd.lock"

// --------------------------------------------------------------------------
// Global daemon pointer — needed by signal handlers
// --------------------------------------------------------------------------
// Signal handlers can't take custom arguments, so we store a pointer to
// the daemon struct globally. This is the standard pattern for signal-driven
// shutdown in C daemons. Only the signal handler writes through this pointer,
// and it only sets boolean flags (which is async-signal-safe).
// --------------------------------------------------------------------------
static InputDaemon *g_daemon = NULL;

// --------------------------------------------------------------------------
// signal_handler — Handles SIGINT, SIGTERM, and SIGHUP
// --------------------------------------------------------------------------
// This function runs in signal context, so it must only call async-signal-safe
// functions. Setting a volatile bool/sig_atomic_t and writing one byte to
// a pipe are both safe operations.
//
// SIGINT / SIGTERM: Set running = false to break the main loop.
// SIGHUP:          Set reload_pending = true to reload config on next iteration.
//
// Both cases also write a byte to the self-pipe to wake up epoll_wait(),
// which might be blocked waiting for events. Without this, the daemon
// wouldn't notice the signal until the next device event arrives.
// --------------------------------------------------------------------------
static void signal_handler(int sig) {
    if (!g_daemon) return;

    if (sig == SIGINT || sig == SIGTERM) {
        // Tell the main loop to exit gracefully
        g_daemon->running = false;
    } else if (sig == SIGHUP) {
        // Tell the main loop to reload the config file
        g_daemon->reload_pending = 1;
    }

    // Wake up epoll_wait() by writing a byte to the self-pipe.
    // We use write() here because it's async-signal-safe.
    // We ignore the return value — if the pipe is full (unlikely with
    // a single byte), the main loop will still check the flags.
    char byte = (char)sig;
    (void)write(g_daemon->pipe_write, &byte, 1);
}

// --------------------------------------------------------------------------
// setup_signals — Install signal handlers for graceful shutdown and reload
// --------------------------------------------------------------------------
// Uses sigaction() instead of signal() because sigaction() gives us control
// over SA_RESTART behavior and is more portable across Unix systems.
// --------------------------------------------------------------------------
static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    // Point all three signals at our handler function
    sa.sa_handler = signal_handler;

    // Block other signals while our handler runs (prevents re-entrancy)
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGINT);
    sigaddset(&sa.sa_mask, SIGTERM);
    sigaddset(&sa.sa_mask, SIGHUP);

    // SA_RESTART: if a signal arrives during a system call (like read()),
    // restart the call automatically instead of returning EINTR. This
    // simplifies error handling throughout the codebase.
    sa.sa_flags = SA_RESTART;

    // Install the handler for each signal
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);

    // Ignore SIGPIPE — we don't want the daemon to die if the IPC client
    // disconnects while we're writing to the socket. Instead, write() will
    // return EPIPE and we handle it gracefully.
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

// --------------------------------------------------------------------------
// acquire_lock — Ensure only one instance of inputd is running
// --------------------------------------------------------------------------
// Opens (or creates) a lock file and applies an exclusive, non-blocking
// flock(). If another instance already holds the lock, flock() returns
// EWOULDBLOCK immediately and we exit with an error.
//
// Returns the lock file descriptor on success (caller keeps it open for
// the lifetime of the daemon — the lock is released automatically when
// the fd is closed or the process exits). Returns -1 on failure.
// --------------------------------------------------------------------------
static int acquire_lock(void) {
    // Open the lock file, creating it if it doesn't exist.
    // O_RDWR because flock() needs at least one access mode.
    int fd = open(LOCK_FILE, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        fprintf(stderr, "inputd: failed to open lock file %s: %s\n",
                LOCK_FILE, strerror(errno));
        return -1;
    }

    // Try to acquire an exclusive lock without blocking.
    // LOCK_EX = exclusive (no other process can hold any lock on this file)
    // LOCK_NB = non-blocking (return immediately if lock can't be acquired)
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK) {
            fprintf(stderr, "inputd: another instance is already running\n");
        } else {
            fprintf(stderr, "inputd: flock failed: %s\n", strerror(errno));
        }
        close(fd);
        return -1;
    }

    return fd;
}

// --------------------------------------------------------------------------
// print_usage — Show command-line help on stderr
// --------------------------------------------------------------------------
static void print_usage(void) {
    fprintf(stderr,
        "inputd — CopyCatOS input daemon\n"
        "\n"
        "Reads Legion Go gamepad events and translates them into\n"
        "mouse, keyboard, and shell actions for the desktop environment.\n"
        "\n"
        "Usage:\n"
        "  inputd           Run the daemon (requires root)\n"
        "  inputd --help    Show this help message\n"
        "  inputd --version Show version\n"
        "\n"
        "Signals:\n"
        "  SIGINT/SIGTERM      Shut down gracefully\n"
        "  SIGHUP              Reload configuration\n"
        "\n"
        "Config file: ~/.config/copycatos/input.conf\n"
        "IPC socket:  /run/inputd.sock\n"
    );
}

// --------------------------------------------------------------------------
// print_version — Show version on stderr
// --------------------------------------------------------------------------
static void print_version(void) {
    fprintf(stderr, "inputd version %s\n", INPUTD_VERSION);
}

// --------------------------------------------------------------------------
// main — Entry point
// --------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    // --- Handle --help and --version flags ---
    // Check command-line arguments before doing any real work.
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            print_version();
            return 0;
        }
    }

    // --- Check for root privileges ---
    // inputd needs root to:
    //   1. Open /dev/input/event* device files
    //   2. Open /dev/uinput to create virtual devices
    //   3. Use EVIOCGRAB to grab exclusive access to controllers
    //   4. Create the IPC socket in /run/
    if (geteuid() != 0) {
        fprintf(stderr, "inputd: must run as root (need /dev/input + /dev/uinput access)\n");
        return 1;
    }

    // --- Single instance enforcement ---
    // Acquire the lock file. If another inputd is already running,
    // this returns -1 and we exit immediately.
    int lock_fd = acquire_lock();
    if (lock_fd < 0) {
        return 1;
    }

    fprintf(stderr, "inputd: starting (version %s)\n", INPUTD_VERSION);

    // --- Initialize the daemon struct ---
    // Zero the struct so all pointers start as NULL and all fds start as 0.
    // inputd_init() will allocate and set up each subsystem.
    InputDaemon daemon;
    memset(&daemon, 0, sizeof(daemon));

    // Set the global pointer so signal handlers can reach the daemon
    g_daemon = &daemon;

    // --- Install signal handlers ---
    // Must happen before inputd_init() so that if init takes a while,
    // the user can still Ctrl-C out cleanly.
    setup_signals();

    // --- Initialize all subsystems ---
    // This opens physical devices, creates virtual devices, loads config,
    // sets up the IPC socket, creates the self-pipe, and builds the epoll set.
    if (inputd_init(&daemon) < 0) {
        fprintf(stderr, "inputd: initialization failed\n");
        inputd_shutdown(&daemon);
        close(lock_fd);
        return 1;
    }

    fprintf(stderr, "inputd: initialized, entering main loop\n");

    // --- Run the main event loop ---
    // This blocks until daemon.running is set to false by a signal handler.
    // All event processing happens inside this function.
    inputd_run(&daemon);

    fprintf(stderr, "inputd: shutting down\n");

    // --- Clean shutdown ---
    // Close all devices, destroy virtual devices, remove IPC socket, free memory.
    inputd_shutdown(&daemon);

    // Clear global pointer (not strictly necessary since we're exiting,
    // but good practice in case this code is ever turned into a library)
    g_daemon = NULL;

    // Close the lock file — this automatically releases the flock
    close(lock_fd);

    fprintf(stderr, "inputd: stopped\n");
    return 0;
}
