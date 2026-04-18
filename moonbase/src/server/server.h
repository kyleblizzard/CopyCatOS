// CopyCatOS — by Kyle Blizzard at Blizzard.show

// server.h — MoonBase IPC server primitives.
//
// This module is the server-side counterpart to src/ipc/transport.{h,c}.
// It owns a non-blocking AF_UNIX listener plus per-client state, runs
// the HELLO/WELCOME handshake internally, and surfaces everything else
// (post-handshake frames, disconnects) to a caller-supplied event
// callback.
//
// MoonRock is the expected consumer in CopyCatOS, but the module
// depends only on libc and the MoonBase IPC primitives (cbor, frame),
// so it is equally driveable from tests. The caller is responsible for
// the outer event loop: it collects the server's fds via
// mb_server_get_pollfds(), calls poll(), and feeds the result back to
// mb_server_tick(). The server itself never blocks.
//
// Design rules:
//   * All sockets are non-blocking. Reads and writes fragment freely;
//     the server buffers partial frames per-client until complete.
//   * The first frame on every connection must be MB_IPC_HELLO. Any
//     other kind before the handshake is a protocol error and the
//     connection is dropped with MB_IPC_ERROR.
//   * The caller sees connections only *after* a successful handshake.
//     A broken handshake is logged and the socket is closed before the
//     caller is notified.
//   * SCM_RIGHTS (fd passing, client → server) is implemented: any
//     ancillary fds arriving with a frame are surfaced on the matching
//     MB_SERVER_EV_FRAME as `frame_fds` / `frame_fd_count`. The server
//     owns those fds and closes them after the callback returns, so
//     the callback must `dup()` (or `fcntl(F_DUPFD_CLOEXEC)`) any fd
//     it wants to keep past the event. Server → client fd passing
//     (mb_server_send with nfds > 0) is not wired yet and currently
//     returns MB_ENOSYS.

#ifndef MOONBASE_SERVER_H
#define MOONBASE_SERVER_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mb_server mb_server_t;

// Stable identifier for the lifetime of a connection. Assigned in
// ascending order; never reused within a server lifetime.
typedef uint32_t mb_client_id_t;

typedef enum {
    // Fires after a successful HELLO handshake. The WELCOME has
    // already been queued to send; no caller action is required.
    MB_SERVER_EV_CONNECTED = 1,

    // Fires for every post-handshake frame received from a client.
    // `frame_body` and `frame_fds` are borrowed for the duration of
    // the callback only — copy before returning if needed.
    MB_SERVER_EV_FRAME = 2,

    // Fires when a client's connection is closing. Delivered once per
    // connection. After the callback returns, the client is gone and
    // its id must not be used again. BYE triggers this event too.
    MB_SERVER_EV_DISCONNECTED = 3,
} mb_server_event_kind_t;

typedef struct {
    mb_server_event_kind_t kind;
    mb_client_id_t         client;

    // Populated on MB_SERVER_EV_CONNECTED. All const char * pointers
    // are borrowed — they live as long as the callback runs.
    struct {
        uint32_t     api_version;
        const char  *bundle_id;
        const char  *bundle_version;
        uint32_t     pid;
        uint32_t     language;     // 0=c 1=web 2=python 3=rust 4=swift
    } hello;

    // Populated on MB_SERVER_EV_FRAME.
    uint16_t        frame_kind;
    const uint8_t  *frame_body;
    size_t          frame_body_len;

    // Ancillary fds delivered alongside the frame (SCM_RIGHTS). The
    // pointer is borrowed — the server retains ownership and closes
    // every fd after the callback returns. If the callback wants any
    // fd to survive past the event, it must dup (`F_DUPFD_CLOEXEC`).
    // `frame_fd_count` is zero for frames that carried no fds.
    const int      *frame_fds;
    size_t          frame_fd_count;

    // Populated on MB_SERVER_EV_DISCONNECTED.
    int             disconnect_reason;   // 0 == clean BYE / peer EOF
                                         // negative == mb_error_t
} mb_server_event_t;

typedef void (*mb_server_on_event_fn)(mb_server_t *s,
                                      const mb_server_event_t *ev,
                                      void *user);

// Open the listener at `path`. On success *out is set and the listener
// is installed in non-blocking mode. `on_event` is called from within
// mb_server_tick() with `user` as the third argument. Returns 0 or a
// negative mb_error_t.
int  mb_server_open(mb_server_t **out,
                    const char *path,
                    mb_server_on_event_fn on_event,
                    void *user);

// Close the listener, drop every connected client, unlink the socket
// path, and free the server. Safe to call on a NULL pointer.
void mb_server_close(mb_server_t *s);

// Fill `out_fds` with every fd the outer loop should poll. At most
// `max` entries are written; if the real number exceeds `max` the
// caller can either grow its buffer or rely on the subset for this
// tick (uncovered clients will be serviced next time). Returns the
// number of entries written. Events requested are POLLIN on all fds
// plus POLLOUT on clients with a non-empty send queue.
size_t mb_server_get_pollfds(mb_server_t *s,
                             struct pollfd *out_fds, size_t max);

// Advance the server. Call after poll() returns. The fds passed in
// must be the same slice returned by mb_server_get_pollfds() with
// .revents filled in by poll(). Safe to call with nfds==0 to force a
// pass that services already-queued writes and tears down pending
// disconnects.
void mb_server_tick(mb_server_t *s,
                    const struct pollfd *fds, size_t nfds);

// Queue a frame to a specific client. The bytes are copied into the
// client's send buffer and flushed opportunistically; the caller may
// free `body` immediately. Returns 0 on queue success or a negative
// mb_error_t (MB_ENOTFOUND if the client id is unknown, MB_ENOMEM if
// the queue is exhausted, etc.). On error the frame is not queued.
// fds/nfds are placeholders for a later slice — pass NULL/0 today.
int mb_server_send(mb_server_t *s,
                   mb_client_id_t client,
                   uint16_t kind,
                   const uint8_t *body, size_t body_len,
                   const int *fds, size_t nfds);

#ifdef __cplusplus
}
#endif

#endif // MOONBASE_SERVER_H
