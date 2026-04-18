// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

//
// session.h — SessionBridge definition and interface
//
// The SessionBridge is the central struct that ties together the X11
// display connection and the Unix socket connection to cc-inputd.
// It tracks the currently active window's WM_CLASS so the daemon
// can apply per-application input profiles automatically.
//

#ifndef CC_INPUT_SESSION_H
#define CC_INPUT_SESSION_H

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdbool.h>

// Path to the cc-inputd Unix domain socket.
// cc-inputd listens here; we connect as a client.
#define INPUTD_SOCK_PATH "/run/cc-inputd.sock"

// IPC message types — these are the first byte of every message.
// The protocol is: [type: 1 byte] [length: 2 bytes big-endian] [payload: N bytes]
#define MSG_HELLO            0x01  // Session -> Daemon: "here I am, here's my DISPLAY"
#define MSG_HELLO_ACK        0x02  // Daemon -> Session: "acknowledged, you're connected"
#define MSG_SET_PROFILE      0x10  // Session -> Daemon: switch to "desktop" or "game" profile
#define MSG_ACTIVE_WINDOW    0x11  // Session -> Daemon: active window changed, here's its WM_CLASS
#define MSG_CONFIG_RELOAD    0x12  // Session -> Daemon: please reload your config files
#define MSG_COPYCATOS_ACTION 0x20  // Daemon -> Session: trigger a desktop action (spotlight, etc.)

// SessionBridge — holds all state for the session bridge process.
//
// This struct owns:
//   - The X11 display connection (used to watch for active window changes)
//   - The Unix socket connection to cc-inputd (used to send/receive IPC messages)
//   - Cached WM_CLASS strings for the currently active window
//
typedef struct {
    // --- X11 state ---
    Display *dpy;           // Connection to the X server
    Window   root;          // The root window (we watch PropertyNotify on this)
    int      screen;        // Default screen number

    // --- X11 atoms (interned once at startup for efficiency) ---
    Atom net_active_window; // _NET_ACTIVE_WINDOW — tells us which window is focused
    Atom wm_class_atom;     // WM_CLASS — the application identifier property

    // --- IPC state ---
    int  sock_fd;           // File descriptor for the Unix socket to cc-inputd
    bool running;           // True while the main loop should keep running
    bool connected;         // True if we have an active connection to cc-inputd

    // --- Cached active window info ---
    // WM_CLASS has two parts: instance name and class name.
    // For example, Firefox might be: instance="Navigator", class="firefox"
    char current_wm_class[256];       // Instance name (first string in WM_CLASS)
    char current_wm_class_name[256];  // Class name (second string in WM_CLASS)
} SessionBridge;

// session_init — Set up X11, connect to cc-inputd, and send HELLO.
// Returns true if everything succeeded, false on failure.
bool session_init(SessionBridge *sb);

// session_run — Main event loop. Watches for X11 property changes and
// IPC messages from cc-inputd. Blocks until sb->running becomes false
// or an unrecoverable error occurs.
void session_run(SessionBridge *sb);

// session_shutdown — Clean up all resources: close socket, close X display.
void session_shutdown(SessionBridge *sb);

#endif // CC_INPUT_SESSION_H
