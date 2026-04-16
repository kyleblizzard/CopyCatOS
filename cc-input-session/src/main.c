// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

//
// main.c — Entry point for cc-input-session
//
// cc-input-session is a bridge between the X11 desktop environment and
// cc-inputd (the CopiCatOS input daemon). It watches for active window
// changes in X11 and reports them to the daemon, so the daemon can
// apply per-application input profiles. It also receives action
// requests from the daemon (like "open Spotlight") and executes them.
//
// This process runs once per desktop session. It uses an flock on a
// lock file to prevent duplicate instances — if you try to start a
// second one, it exits immediately.
//
// If cc-inputd is not running when we start, we exit gracefully with
// a warning rather than crashing. The session can be started again
// once the daemon is available.
//

#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

// Lock file path — used to ensure only one instance runs at a time.
// flock() is an advisory lock, meaning other programs can ignore it,
// but all copies of cc-input-session will respect it.
#define LOCK_FILE "/tmp/cc-input-session.lock"

// Global pointer to the session bridge, so signal handlers can access it.
// Signal handlers can only use global/static variables because they can
// fire at any point during execution.
static SessionBridge *g_session = NULL;

// signal_handler — Called when we receive SIGINT (Ctrl+C) or SIGTERM (kill).
//
// We set running = false, which tells the main loop in session_run()
// to exit cleanly on its next iteration. This is much safer than
// calling exit() directly from a signal handler.
static void signal_handler(int sig)
{
    (void)sig;  // Suppress "unused parameter" warning

    if (g_session) {
        g_session->running = false;
    }
}

// acquire_lock — Try to get an exclusive lock on the lock file.
//
// Returns the lock file descriptor on success, or -1 if another
// instance already holds the lock.
//
// How it works:
//   1. Open (or create) the lock file
//   2. Try to acquire an exclusive lock with LOCK_NB (non-blocking)
//   3. If the lock is already held, flock returns EWOULDBLOCK
//
// The lock is automatically released when the process exits or the
// fd is closed, so we don't need explicit cleanup.
static int acquire_lock(void)
{
    int fd = open(LOCK_FILE, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        perror("[cc-input-session] open lock file");
        return -1;
    }

    // LOCK_EX = exclusive lock (no other process can lock it)
    // LOCK_NB = non-blocking (return immediately if lock is held)
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        close(fd);
        return -1;  // Another instance is already running
    }

    return fd;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    fprintf(stderr, "[cc-input-session] starting\n");

    // --- Step 1: Ensure only one instance runs at a time ---
    int lock_fd = acquire_lock();
    if (lock_fd < 0) {
        fprintf(stderr, "[cc-input-session] another instance is already running, exiting\n");
        return 0;  // Not an error — just means we're already covered
    }

    // --- Step 2: Set up signal handlers ---
    //
    // We want to catch SIGINT and SIGTERM so we can shut down gracefully
    // (close sockets, close X display) instead of being killed mid-operation.
    //
    // struct sigaction is the modern way to install signal handlers.
    // SA_RESTART tells the kernel to automatically restart interrupted
    // system calls (like read/select) instead of returning EINTR.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // --- Step 3: Initialize the session bridge ---
    SessionBridge sb;
    g_session = &sb;

    if (!session_init(&sb)) {
        // If cc-inputd isn't running, this is expected — just log and exit cleanly.
        // The user can start cc-inputd and then restart us.
        fprintf(stderr, "[cc-input-session] initialization failed "
                "(is cc-inputd running?), exiting\n");

        // Clean up any partial initialization
        session_shutdown(&sb);
        close(lock_fd);
        return 0;  // Exit 0 because this isn't a crash
    }

    // --- Step 4: Run the main event loop ---
    //
    // This blocks until sb.running is set to false (by a signal handler
    // or by an unrecoverable error inside the loop).
    session_run(&sb);

    // --- Step 5: Clean up ---
    session_shutdown(&sb);
    g_session = NULL;

    // Release the instance lock
    close(lock_fd);

    fprintf(stderr, "[cc-input-session] exited cleanly\n");
    return 0;
}
