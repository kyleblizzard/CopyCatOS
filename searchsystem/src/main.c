// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ─── main.c ───
// Entry point for searchsystem.
//
// This file does three things:
//   1. Opens a connection to the X display server.
//   2. Installs signal handlers so Ctrl+C / kill gracefully shuts down.
//   3. Hands control to the spotlight module which creates the overlay
//      window and enters the event loop.

#define _GNU_SOURCE  // For SA_RESTART in signal.h under strict C11
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <X11/Xlib.h>

#include "spotlight.h"

// ──────────────────────────────────────────────
// Signal handling
// ──────────────────────────────────────────────

// When the user presses Ctrl+C in the terminal or the process
// receives SIGTERM (e.g. from `kill`), we set a flag that tells
// the event loop to exit on its next iteration rather than
// tearing things down abruptly.
static void signal_handler(int sig) {
    (void)sig; // unused — we treat all caught signals the same
    spotlight_request_quit();
}

// ──────────────────────────────────────────────
// main
// ──────────────────────────────────────────────

int main(void) {
    // --- Step 1: Connect to the X server ---
    // XOpenDisplay(NULL) reads the DISPLAY environment variable
    // (usually ":0") to figure out which X server to talk to.
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "searchsystem: cannot open display\n");
        return 1;
    }

    // --- Step 2: Install signal handlers ---
    // "struct sigaction" lets us specify SA_RESTART so that
    // system calls like select() are automatically retried
    // instead of failing with EINTR.
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sa.sa_flags   = SA_RESTART;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT,  &sa, NULL);  // Ctrl+C
    sigaction(SIGTERM, &sa, NULL);  // kill <pid>

    // --- Step 3: Initialise the overlay ---
    if (spotlight_init(dpy) != 0) {
        fprintf(stderr, "searchsystem: init failed\n");
        XCloseDisplay(dpy);
        return 1;
    }

    printf("searchsystem: running (Ctrl+Space to toggle)\n");

    // --- Step 4: Run the event loop (blocks until quit) ---
    spotlight_run(dpy);

    // --- Step 5: Tear down ---
    spotlight_cleanup(dpy);
    XCloseDisplay(dpy);

    printf("searchsystem: exiting\n");
    return 0;
}
