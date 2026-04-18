// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// ipc.c — Unix domain socket IPC server
// ============================================================================
//
// Manages a Unix domain socket at /run/inputd.sock through which the
// daemon communicates with the user-level session bridge. The session bridge
// relays messages between inputd (running as root) and desktop components
// like moonrock and System Preferences (running as the logged-in user).
//
// The protocol is intentionally simple — a 3-byte header followed by a
// variable-length payload:
//
//   Byte 0:     Message type (IpcMsgType enum)
//   Bytes 1-2:  Payload length as 16-bit big-endian unsigned integer
//   Bytes 3+:   Payload data (0 to IPC_MAX_PAYLOAD bytes)
//
// Only one client (session bridge) connects at a time. If a new client
// connects while one is already active, the old connection is closed.
// This avoids any need for client multiplexing.
// ============================================================================

#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

// ============================================================================
//  ipc_server_init — Create and bind the listening socket
// ============================================================================
//
// Creates a non-blocking Unix domain socket, binds it to IPC_SOCK_PATH,
// and starts listening for connections. The socket is set non-blocking so
// that accept() in the main epoll loop never stalls.
//
// The socket file gets chmod 0666 so that the user-level session bridge
// (which runs as the logged-in user, not root) can connect to it. This is
// safe because the socket only accepts local connections and the protocol
// is simple with no sensitive data.
//
// Returns true on success, false on failure.
// ============================================================================

bool ipc_server_init(IpcServer *srv)
{
    // Initialize to known-invalid state so shutdown is safe if init fails
    srv->listen_fd = -1;
    srv->client_fd = -1;

    // Create a Unix domain stream socket with non-blocking I/O.
    // SOCK_STREAM gives us reliable, ordered byte delivery (like TCP).
    // SOCK_NONBLOCK means accept() returns EAGAIN instead of blocking
    // when no client is waiting.
    srv->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (srv->listen_fd < 0) {
        fprintf(stderr, "ipc: socket() failed: %s\n", strerror(errno));
        return false;
    }

    // Remove any stale socket file from a previous run.
    // If the daemon crashed without cleaning up, the old socket file
    // would prevent bind() from succeeding (EADDRINUSE).
    unlink(IPC_SOCK_PATH);

    // Set up the socket address structure.
    // AF_UNIX sockets use a filesystem path instead of an IP:port.
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, IPC_SOCK_PATH, sizeof(addr.sun_path) - 1);

    // Bind the socket to the filesystem path.
    // After this call, /run/inputd.sock exists as a special file.
    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "ipc: bind(%s) failed: %s\n",
                IPC_SOCK_PATH, strerror(errno));
        close(srv->listen_fd);
        srv->listen_fd = -1;
        return false;
    }

    // Start listening for connections.
    // Backlog of 1: only one session bridge connects at a time.
    // Any additional connection attempts will queue (and we'll close
    // the old client when we accept the new one).
    if (listen(srv->listen_fd, 1) < 0) {
        fprintf(stderr, "ipc: listen() failed: %s\n", strerror(errno));
        close(srv->listen_fd);
        unlink(IPC_SOCK_PATH);
        srv->listen_fd = -1;
        return false;
    }

    // Make the socket world-readable/writable so the user-level session
    // bridge can connect without needing root privileges.
    chmod(IPC_SOCK_PATH, 0666);

    fprintf(stderr, "ipc: listening on %s\n", IPC_SOCK_PATH);
    return true;
}

// ============================================================================
//  ipc_server_accept — Accept a pending client connection
// ============================================================================
//
// Called by the main loop when epoll signals that the listen_fd is readable
// (meaning a client is waiting to connect). Accepts the connection and stores
// the client file descriptor.
//
// If there's already a connected client, it's closed first. Only one session
// bridge is active at a time — if the bridge restarts, it reconnects and the
// old (presumably dead) connection is cleaned up here.
// ============================================================================

void ipc_server_accept(IpcServer *srv)
{
    int new_fd = accept(srv->listen_fd, NULL, NULL);
    if (new_fd < 0) {
        // EAGAIN/EWOULDBLOCK is normal for non-blocking sockets — it means
        // there's no pending connection right now (spurious epoll wakeup).
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "ipc: accept() failed: %s\n", strerror(errno));
        }
        return;
    }

    // If we already have a client, close it first.
    // This handles the case where the session bridge crashed and a new
    // instance is connecting before we noticed the old one was gone.
    if (srv->client_fd >= 0) {
        fprintf(stderr, "ipc: closing old client (fd %d), new client connecting\n",
                srv->client_fd);
        close(srv->client_fd);
    }

    srv->client_fd = new_fd;
    fprintf(stderr, "ipc: session bridge connected (fd %d)\n", new_fd);
}

// ============================================================================
//  ipc_server_send — Send a message to the connected session bridge
// ============================================================================
//
// Writes a 3-byte header (type + big-endian length) followed by the payload.
// Returns false if no client is connected or if the write fails.
//
// On EPIPE (broken pipe — client disconnected), the client fd is cleaned up
// automatically. The next time the session bridge starts, it will reconnect.
// ============================================================================

bool ipc_server_send(IpcServer *srv, const IpcMessage *msg)
{
    if (srv->client_fd < 0) {
        return false;   // No client connected — nothing to do
    }

    // Validate payload length
    if (msg->length < 0 || msg->length > IPC_MAX_PAYLOAD) {
        fprintf(stderr, "ipc: send: invalid payload length %d\n", msg->length);
        return false;
    }

    // Build the 3-byte header:
    //   byte 0: message type
    //   byte 1: payload length high byte (big-endian)
    //   byte 2: payload length low byte
    unsigned char header[3];
    header[0] = (unsigned char)msg->type;
    header[1] = (unsigned char)((msg->length >> 8) & 0xFF);
    header[2] = (unsigned char)(msg->length & 0xFF);

    // Write the header
    ssize_t n = write(srv->client_fd, header, 3);
    if (n < 0) {
        if (errno == EPIPE) {
            // Client disconnected — clean up
            fprintf(stderr, "ipc: client disconnected (EPIPE on send)\n");
            close(srv->client_fd);
            srv->client_fd = -1;
        } else {
            fprintf(stderr, "ipc: send header failed: %s\n", strerror(errno));
        }
        return false;
    }

    // Write the payload (if any)
    if (msg->length > 0) {
        n = write(srv->client_fd, msg->payload, msg->length);
        if (n < 0) {
            if (errno == EPIPE) {
                fprintf(stderr, "ipc: client disconnected (EPIPE on payload)\n");
                close(srv->client_fd);
                srv->client_fd = -1;
            } else {
                fprintf(stderr, "ipc: send payload failed: %s\n", strerror(errno));
            }
            return false;
        }
    }

    return true;
}

// ============================================================================
//  ipc_server_recv — Try to receive a message from the session bridge
// ============================================================================
//
// Non-blocking receive. If data is available, reads the 3-byte header then
// the payload and fills in the IpcMessage struct. Returns true if a complete
// message was received.
//
// Returns false in three cases:
//   1. No client is connected (client_fd == -1)
//   2. No data available right now (EAGAIN — non-blocking socket)
//   3. Client disconnected (EOF or error — fd is cleaned up)
//
// The caller typically checks this in the epoll loop when the client_fd
// is reported as readable.
// ============================================================================

bool ipc_server_recv(IpcServer *srv, IpcMessage *msg)
{
    if (srv->client_fd < 0) {
        return false;   // No client connected
    }

    // ------------------------------------------------------------------
    // Read the 3-byte header
    // ------------------------------------------------------------------
    unsigned char header[3];
    ssize_t n = read(srv->client_fd, header, 3);

    if (n == 0) {
        // EOF — client closed the connection gracefully
        fprintf(stderr, "ipc: session bridge disconnected (EOF)\n");
        close(srv->client_fd);
        srv->client_fd = -1;
        return false;
    }

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // No data available right now — this is normal for non-blocking I/O
            return false;
        }
        // Real error
        fprintf(stderr, "ipc: recv header failed: %s\n", strerror(errno));
        close(srv->client_fd);
        srv->client_fd = -1;
        return false;
    }

    if (n < 3) {
        // Partial header read — shouldn't happen with small messages on a
        // local socket, but handle it defensively by discarding.
        fprintf(stderr, "ipc: partial header read (%zd bytes), discarding\n", n);
        return false;
    }

    // Decode the header
    msg->type   = (IpcMsgType)header[0];
    msg->length = (header[1] << 8) | header[2];

    // Validate payload length
    if (msg->length < 0 || msg->length > IPC_MAX_PAYLOAD) {
        fprintf(stderr, "ipc: invalid payload length %d from client\n", msg->length);
        close(srv->client_fd);
        srv->client_fd = -1;
        return false;
    }

    // ------------------------------------------------------------------
    // Read the payload (if any)
    // ------------------------------------------------------------------
    if (msg->length > 0) {
        n = read(srv->client_fd, msg->payload, msg->length);
        if (n <= 0) {
            if (n == 0) {
                fprintf(stderr, "ipc: session bridge disconnected during payload\n");
            } else {
                fprintf(stderr, "ipc: recv payload failed: %s\n", strerror(errno));
            }
            close(srv->client_fd);
            srv->client_fd = -1;
            return false;
        }
    }

    return true;
}

// ============================================================================
//  ipc_server_shutdown — Clean up all IPC resources
// ============================================================================
//
// Closes both the client connection (if any) and the listening socket,
// then removes the socket file from the filesystem. Called during daemon
// shutdown to ensure a clean exit.
// ============================================================================

void ipc_server_shutdown(IpcServer *srv)
{
    // Close the client connection if one is active
    if (srv->client_fd >= 0) {
        close(srv->client_fd);
        srv->client_fd = -1;
    }

    // Close the listening socket
    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }

    // Remove the socket file so a future daemon instance can bind cleanly
    unlink(IPC_SOCK_PATH);

    fprintf(stderr, "ipc: shutdown complete\n");
}
