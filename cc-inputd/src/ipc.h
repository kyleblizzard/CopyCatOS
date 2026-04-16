// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

//
// ipc.h — Unix socket IPC for session bridge communication
//
// cc-inputd runs as a system service (root) but needs to talk to the
// user's desktop session (cc-wm, System Preferences, etc.). It does this
// through a Unix domain socket at /run/cc-inputd.sock.
//
// The session bridge (a small user-level helper) connects to this socket
// and acts as a relay:
//   - Daemon → Bridge: "open Spotlight", "show power menu", etc.
//   - Bridge → Daemon: "switch to gamepad profile", "reload config",
//                       "active window changed to X"
//
// Messages use a simple 3-byte header: [type (1 byte)] [length (2 bytes BE)]
// followed by `length` bytes of payload. This keeps the protocol trivial
// to implement without any external serialization library.
//

#ifndef IPC_H
#define IPC_H

#include <stdbool.h>

// --------------------------------------------------------------------------
// Socket path — where the daemon listens
// --------------------------------------------------------------------------
// Lives in /run because:
//   1. It's a tmpfs, so the socket vanishes on reboot (no stale files)
//   2. The daemon runs as root and has write access
//   3. We chmod 0666 so the user-level session bridge can connect
// --------------------------------------------------------------------------
#define IPC_SOCK_PATH "/run/cc-inputd.sock"

// --------------------------------------------------------------------------
// IPC message types
// --------------------------------------------------------------------------
// Messages flowing in both directions share this type enum.
// Types 0x01–0x0F are daemon → session bridge (outbound).
// Types 0x10–0x1F are session bridge → daemon (inbound).
// --------------------------------------------------------------------------
typedef enum {
    // Daemon → Bridge
    IPC_MSG_COPICATOS_ACTION = 0x01,   // Payload: action name (e.g. "spotlight")
    IPC_MSG_POWER_ACTION     = 0x02,   // Payload: action name (e.g. "suspend")

    // Bridge → Daemon
    IPC_MSG_SET_PROFILE      = 0x10,   // Payload: profile name (e.g. "gamepad")
    IPC_MSG_CONFIG_RELOAD    = 0x11,   // Payload: empty (just a signal)
    IPC_MSG_ACTIVE_WINDOW    = 0x12,   // Payload: WM_CLASS string of focused window
} IpcMsgType;

// Maximum payload size for a single IPC message.
// 256 bytes is plenty for action names and window class strings.
#define IPC_MAX_PAYLOAD 256

// --------------------------------------------------------------------------
// IpcMessage — A single IPC message (header + payload)
// --------------------------------------------------------------------------
typedef struct {
    IpcMsgType type;                   // What kind of message this is
    int        length;                 // Payload length in bytes (0–256)
    char       payload[IPC_MAX_PAYLOAD]; // The message data (not null-terminated)
} IpcMessage;

// --------------------------------------------------------------------------
// IpcServer — The daemon's side of the IPC connection
// --------------------------------------------------------------------------
typedef struct IpcServer {
    int listen_fd;                     // The listening socket fd (bound to path)
    int client_fd;                     // The connected session bridge fd (-1 if none)
} IpcServer;

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------

// ipc_server_init — Create and bind the Unix domain socket.
// Removes any stale socket file, binds, listens, and sets permissions.
// Returns true on success, false on failure (logged to stderr).
bool ipc_server_init(IpcServer *srv);

// ipc_server_accept — Accept a waiting connection on the listen socket.
// If a client is already connected, the old one is closed first.
// (Only one session bridge connects at a time.)
void ipc_server_accept(IpcServer *srv);

// ipc_server_send — Send a message to the connected session bridge.
// Returns false if no client is connected or write fails (EPIPE).
bool ipc_server_send(IpcServer *srv, const IpcMessage *msg);

// ipc_server_recv — Try to receive a message from the session bridge.
// Non-blocking: returns false immediately if no data is available.
// Also returns false (and cleans up) if the client disconnected.
bool ipc_server_recv(IpcServer *srv, IpcMessage *msg);

// ipc_server_shutdown — Close all sockets and remove the socket file.
void ipc_server_shutdown(IpcServer *srv);

#endif // IPC_H
