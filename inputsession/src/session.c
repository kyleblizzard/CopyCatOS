// CopyCatOS — by Kyle Blizzard at Blizzard.show

//
// session.c — Main session bridge implementation
//
// This is the core of inputsession. It manages two connections
// simultaneously:
//
//   1. X11 display — to watch for active window changes
//   2. Unix socket — to communicate with inputd
//
// The main loop uses select() to wait on both file descriptors at
// once, so we can respond to either X11 events or IPC messages
// without blocking on one and missing the other.
//
// When the active window changes (detected via X11 PropertyNotify),
// we read the new window's WM_CLASS and send it to inputd so
// the daemon can apply the right input profile.
//
// When inputd sends us a COPYCATOS_ACTION message, we dispatch
// it to the actions module to perform the requested operation.
//

#include "session.h"
#include "window_track.h"
#include "actions.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdbool.h>

// --- IPC helpers ---

// ipc_send — Send an IPC message to inputd.
//
// The wire format is:
//   [type: 1 byte] [length: 2 bytes big-endian] [payload: length bytes]
//
// "Big-endian" means the most significant byte comes first. For example,
// a length of 300 (0x012C) is sent as bytes [0x01, 0x2C].
//
// Returns true if the entire message was sent successfully.
static bool ipc_send(int fd, uint8_t type, const void *payload, uint16_t len)
{
    // Build the 3-byte header
    uint8_t header[3];
    header[0] = type;
    header[1] = (uint8_t)(len >> 8);   // High byte of length
    header[2] = (uint8_t)(len & 0xFF); // Low byte of length

    // Send the header first
    ssize_t n = write(fd, header, 3);
    if (n != 3) return false;

    // Send the payload (if any)
    if (len > 0 && payload) {
        n = write(fd, payload, len);
        if (n != (ssize_t)len) return false;
    }

    return true;
}

// ipc_recv — Read one IPC message from inputd.
//
// Reads the 3-byte header, then reads the payload into the provided buffer.
// Sets *out_type to the message type and returns the payload length,
// or -1 on error/disconnect.
static int ipc_recv(int fd, uint8_t *out_type, void *buf, int buf_size)
{
    // Read the 3-byte header
    uint8_t header[3];
    ssize_t n = read(fd, header, 3);

    // If we got 0 bytes, the other end closed the connection
    if (n == 0) return -1;

    // If we got an error (or partial read), treat it as a disconnect
    if (n != 3) return -1;

    *out_type = header[0];

    // Decode the 2-byte big-endian length
    uint16_t len = (uint16_t)((header[1] << 8) | header[2]);

    // If the payload is bigger than our buffer, something is wrong
    if (len > (uint16_t)buf_size) {
        fprintf(stderr, "[inputsession] message too large: %u bytes\n", len);
        return -1;
    }

    // Read the payload
    if (len > 0) {
        n = read(fd, buf, len);
        if (n != (ssize_t)len) return -1;
    }

    return (int)len;
}

// --- Socket connection ---

// connect_to_daemon — Attempt to connect to inputd's Unix socket.
//
// Unix domain sockets are like network sockets but for local-only
// communication. They use file paths instead of IP addresses.
//
// Returns the connected socket fd, or -1 on failure.
static int connect_to_daemon(void)
{
    // Create a Unix stream socket
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[inputsession] socket");
        return -1;
    }

    // Set up the address structure pointing to the daemon's socket file
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, INPUTD_SOCK_PATH, sizeof(addr.sun_path) - 1);

    // Try to connect
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

// perform_handshake — Send HELLO and wait for HELLO_ACK.
//
// The HELLO message tells inputd which X display we're managing.
// The daemon responds with HELLO_ACK to confirm the connection.
// We give it 2 seconds to respond before giving up.
//
// Returns true if the handshake completed successfully.
static bool perform_handshake(int fd)
{
    // Get the DISPLAY environment variable (usually ":0" or ":1")
    const char *display_env = getenv("DISPLAY");
    if (!display_env) display_env = ":0";

    // Build the HELLO payload: "DISPLAY=:0"
    char payload[128];
    snprintf(payload, sizeof(payload), "DISPLAY=%s", display_env);

    // Send the HELLO message
    if (!ipc_send(fd, MSG_HELLO, payload, (uint16_t)strlen(payload))) {
        fprintf(stderr, "[inputsession] failed to send HELLO\n");
        return false;
    }

    fprintf(stderr, "[inputsession] sent HELLO (%s)\n", payload);

    // Wait for HELLO_ACK with a 2-second timeout using select()
    //
    // select() lets us wait for data on a file descriptor with a timeout.
    // If no data arrives within the timeout, we give up.
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;

    int ret = select(fd + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) {
        fprintf(stderr, "[inputsession] timeout waiting for HELLO_ACK\n");
        return false;
    }

    // Read the response
    uint8_t msg_type;
    char buf[256];
    int len = ipc_recv(fd, &msg_type, buf, sizeof(buf) - 1);

    if (len < 0 || msg_type != MSG_HELLO_ACK) {
        fprintf(stderr, "[inputsession] expected HELLO_ACK (0x%02x), got 0x%02x\n",
                MSG_HELLO_ACK, msg_type);
        return false;
    }

    fprintf(stderr, "[inputsession] received HELLO_ACK, connected!\n");
    return true;
}

// --- Public API ---

// session_init — Initialize the session bridge.
//
// This does three things in order:
//   1. Opens the X11 display connection
//   2. Interns the atoms we need (basically caching string-to-ID lookups)
//   3. Connects to inputd and performs the handshake
//
// If inputd isn't running, we'll fail gracefully — the caller (main.c)
// handles this by exiting cleanly rather than crashing.
bool session_init(SessionBridge *sb)
{
    // Zero out the struct to start clean
    memset(sb, 0, sizeof(SessionBridge));
    sb->sock_fd = -1;

    // --- Step 1: Open X11 display ---
    //
    // XOpenDisplay connects to the X server. Passing NULL means
    // "use the DISPLAY environment variable" which is usually ":0".
    sb->dpy = XOpenDisplay(NULL);
    if (!sb->dpy) {
        fprintf(stderr, "[inputsession] cannot open X display\n");
        return false;
    }

    sb->screen = DefaultScreen(sb->dpy);
    sb->root = RootWindow(sb->dpy, sb->screen);

    // --- Step 2: Intern atoms ---
    //
    // In X11, "atoms" are integer IDs for string names. Instead of
    // passing the string "_NET_ACTIVE_WINDOW" every time we need it,
    // we convert it to a number once and reuse that number.
    // XInternAtom does this conversion.
    sb->net_active_window = XInternAtom(sb->dpy, "_NET_ACTIVE_WINDOW", False);
    sb->wm_class_atom     = XInternAtom(sb->dpy, "WM_CLASS", False);

    // Tell the X server we want to be notified when properties change
    // on the root window. This is how we detect active window changes —
    // the WM updates _NET_ACTIVE_WINDOW on the root window whenever
    // focus changes, and we get a PropertyNotify event.
    XSelectInput(sb->dpy, sb->root, PropertyChangeMask);

    fprintf(stderr, "[inputsession] X11 display opened (screen %d)\n", sb->screen);

    // --- Step 3: Connect to inputd ---
    sb->sock_fd = connect_to_daemon();
    if (sb->sock_fd < 0) {
        fprintf(stderr, "[inputsession] inputd not available at %s\n",
                INPUTD_SOCK_PATH);
        return false;
    }

    // Perform the HELLO/HELLO_ACK handshake
    if (!perform_handshake(sb->sock_fd)) {
        close(sb->sock_fd);
        sb->sock_fd = -1;
        return false;
    }

    sb->connected = true;
    sb->running = true;

    return true;
}

// handle_active_window_change — Called when _NET_ACTIVE_WINDOW changes.
//
// Reads the new active window's WM_CLASS and sends it to inputd
// so the daemon can switch input profiles if needed.
static void handle_active_window_change(SessionBridge *sb)
{
    // Find out which window is now active
    Window active = window_track_get_active(sb->dpy, sb->root,
                                             sb->net_active_window);
    if (active == None) return;

    // Read the active window's WM_CLASS
    char instance[256] = {0};
    char class_name[256] = {0};

    if (!window_track_get_wm_class(sb->dpy, active,
                                    instance, sizeof(instance),
                                    class_name, sizeof(class_name))) {
        return;
    }

    // Only send an update if the WM_CLASS actually changed.
    // This avoids flooding inputd with duplicate messages when
    // the user clicks around within the same application.
    if (strcmp(instance, sb->current_wm_class) == 0 &&
        strcmp(class_name, sb->current_wm_class_name) == 0) {
        return;  // Same window class, nothing to report
    }

    // Cache the new values
    strncpy(sb->current_wm_class, instance, sizeof(sb->current_wm_class) - 1);
    strncpy(sb->current_wm_class_name, class_name, sizeof(sb->current_wm_class_name) - 1);

    fprintf(stderr, "[inputsession] active window: %s / %s\n", instance, class_name);

    // Build the ACTIVE_WINDOW payload: "instance\0class_name"
    // The two strings are separated by a null byte, which is the
    // standard WM_CLASS format. The total length includes both
    // strings and the null separator.
    char payload[512];
    size_t inst_len = strlen(instance);
    size_t cls_len = strlen(class_name);
    size_t total = inst_len + 1 + cls_len;  // instance + \0 + class_name

    if (total > sizeof(payload)) return;  // Safety check (should never happen)

    memcpy(payload, instance, inst_len);
    payload[inst_len] = '\0';  // Null separator between the two strings
    memcpy(payload + inst_len + 1, class_name, cls_len);

    // Send to inputd
    if (!ipc_send(sb->sock_fd, MSG_ACTIVE_WINDOW, payload, (uint16_t)total)) {
        fprintf(stderr, "[inputsession] failed to send ACTIVE_WINDOW, disconnecting\n");
        close(sb->sock_fd);
        sb->sock_fd = -1;
        sb->connected = false;
    }
}

// handle_ipc_message — Process an incoming message from inputd.
//
// Currently the only message we expect from the daemon is COPYCATOS_ACTION,
// which tells us to perform a desktop action like opening Spotlight.
static void handle_ipc_message(SessionBridge *sb)
{
    uint8_t msg_type;
    char buf[512];

    int len = ipc_recv(sb->sock_fd, &msg_type, buf, sizeof(buf) - 1);
    if (len < 0) {
        // Connection lost — the daemon probably shut down or crashed
        fprintf(stderr, "[inputsession] lost connection to inputd\n");
        close(sb->sock_fd);
        sb->sock_fd = -1;
        sb->connected = false;
        return;
    }

    // Null-terminate the payload so we can treat it as a string
    buf[len] = '\0';

    switch (msg_type) {
        case MSG_COPYCATOS_ACTION:
            // The daemon wants us to perform a desktop action.
            // The payload is the action name as a string.
            fprintf(stderr, "[inputsession] received action: '%s'\n", buf);
            actions_dispatch(buf, sb->dpy, sb->root);
            break;

        default:
            fprintf(stderr, "[inputsession] unknown message type: 0x%02x\n", msg_type);
            break;
    }
}

// try_reconnect — Attempt to reconnect to inputd after a disconnect.
//
// Returns true if reconnection succeeded, false otherwise.
// The caller should wait between attempts to avoid hammering the socket.
static bool try_reconnect(SessionBridge *sb)
{
    sb->sock_fd = connect_to_daemon();
    if (sb->sock_fd < 0) return false;

    if (!perform_handshake(sb->sock_fd)) {
        close(sb->sock_fd);
        sb->sock_fd = -1;
        return false;
    }

    sb->connected = true;
    fprintf(stderr, "[inputsession] reconnected to inputd\n");

    // Re-send the current active window so the daemon is up to date
    // (clear cached class to force a fresh send)
    sb->current_wm_class[0] = '\0';
    sb->current_wm_class_name[0] = '\0';
    handle_active_window_change(sb);

    return true;
}

// session_run — Main event loop.
//
// This is the heart of inputsession. It uses select() to wait on
// two file descriptors simultaneously:
//
//   1. The X11 connection fd — for PropertyNotify events (active window changes)
//   2. The inputd socket fd — for incoming IPC messages (action requests)
//
// select() blocks until at least one of these has data ready to read,
// then we handle whichever events are available.
//
// If we lose the connection to inputd, we keep running and try to
// reconnect every 5 seconds. X11 event processing continues regardless.
void session_run(SessionBridge *sb)
{
    // Get the file descriptor for the X11 connection.
    // X11 uses a regular TCP or Unix socket under the hood, so we
    // can use its fd with select().
    int x11_fd = ConnectionNumber(sb->dpy);

    // Track when we last tried to reconnect (to enforce the 5-second interval)
    time_t last_reconnect_attempt = 0;

    fprintf(stderr, "[inputsession] entering main loop\n");

    while (sb->running) {
        // --- Build the set of file descriptors to watch ---
        fd_set read_fds;
        FD_ZERO(&read_fds);

        // Always watch the X11 connection
        FD_SET(x11_fd, &read_fds);
        int max_fd = x11_fd;

        // Watch the daemon socket only if we're connected
        if (sb->connected && sb->sock_fd >= 0) {
            FD_SET(sb->sock_fd, &read_fds);
            if (sb->sock_fd > max_fd) max_fd = sb->sock_fd;
        }

        // Use a 1-second timeout so we can periodically attempt reconnection
        // even if no events are arriving.
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);

        // select() returns -1 on error (usually EINTR from a signal)
        if (ret < 0) {
            if (errno == EINTR) continue;  // Signal interrupted us, just loop again
            perror("[inputsession] select");
            break;
        }

        // --- Handle X11 events ---
        //
        // XPending checks if any X11 events are queued (either from the
        // network or from the internal buffer). We process all pending
        // events before going back to select().
        while (XPending(sb->dpy)) {
            XEvent ev;
            XNextEvent(sb->dpy, &ev);

            // We only care about PropertyNotify on the root window
            if (ev.type == PropertyNotify) {
                // Check if the property that changed is _NET_ACTIVE_WINDOW
                if (ev.xproperty.atom == sb->net_active_window) {
                    handle_active_window_change(sb);
                }
            }
        }

        // --- Handle IPC messages from inputd ---
        if (sb->connected && sb->sock_fd >= 0 && FD_ISSET(sb->sock_fd, &read_fds)) {
            handle_ipc_message(sb);
        }

        // --- Reconnection logic ---
        //
        // If we lost the connection, try to reconnect every 5 seconds.
        // We don't want to hammer the socket file with rapid retries.
        if (!sb->connected) {
            time_t now = time(NULL);
            if (now - last_reconnect_attempt >= 5) {
                last_reconnect_attempt = now;
                fprintf(stderr, "[inputsession] attempting reconnect...\n");
                try_reconnect(sb);
            }
        }
    }

    fprintf(stderr, "[inputsession] main loop exited\n");
}

// session_shutdown — Clean up all resources.
//
// Called when we're shutting down (either from a signal or because
// the main loop exited). Closes both the daemon socket and the
// X11 display connection.
void session_shutdown(SessionBridge *sb)
{
    // Close the daemon socket if it's open
    if (sb->sock_fd >= 0) {
        close(sb->sock_fd);
        sb->sock_fd = -1;
    }

    // Close the X11 display connection
    if (sb->dpy) {
        XCloseDisplay(sb->dpy);
        sb->dpy = NULL;
    }

    sb->connected = false;
    sb->running = false;

    fprintf(stderr, "[inputsession] shutdown complete\n");
}
