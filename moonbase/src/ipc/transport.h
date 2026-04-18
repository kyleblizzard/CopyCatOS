// CopyCatOS — by Kyle Blizzard at Blizzard.show

// transport.h — per-process MoonBase client connection state.
//
// libmoonbase.so.1 holds exactly one connection to MoonRock per app
// process (IPC.md §1). This file wraps that singleton: connect,
// handshake, send/recv, teardown.
//
// Everything here runs on the app's main thread. Non-UI threads do
// not touch the connection directly — they schedule work via
// moonbase_dispatch_main() and the main thread does the I/O. That's
// enforced in init.c when the main-thread requirement lands.

#ifndef MOONBASE_IPC_TRANSPORT_H
#define MOONBASE_IPC_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------
// Connection lifecycle
// ---------------------------------------------------------------------

// Open the MoonRock socket. If `socket_path` is NULL, builds it as
// `$XDG_RUNTIME_DIR/moonbase.sock`. Returns 0 on success, negative
// mb_error_t on failure:
//   MB_EINVAL     -- XDG_RUNTIME_DIR unset and no path given
//   MB_ENOTFOUND  -- connect() refused (no compositor listening)
//   MB_EIPC       -- socket-level failure
//   MB_EINVAL     -- already connected
int mb_conn_open(const char *socket_path);

// Perform the HELLO/WELCOME handshake over an already-open
// connection. On success stores the peer's API version /
// capabilities / max_frame_len for later accessors.
//
//   bundle_id, bundle_version: app identity. NULL falls back to
//                              "unknown.bundle" and "0.0.0".
//   language: 0=c, 1=web, 2=python, 3=rust, 4=swift. See IPC.md
//             §5.1.
int mb_conn_handshake(const char *bundle_id,
                      const char *bundle_version,
                      uint32_t    language);

// Send BYE and close the socket. Safe to call when not connected.
void mb_conn_close(void);

// ---------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------

bool     mb_conn_is_connected(void);
bool     mb_conn_is_handshaken(void);
int      mb_conn_fd(void);
uint32_t mb_conn_peer_api_version(void);
uint32_t mb_conn_max_frame_len(void);

// Capability probe. Returns 1 if the compositor advertised `name`
// in WELCOME's capability list, 0 otherwise. Matching is exact.
int mb_conn_has_capability(const char *name);

// ---------------------------------------------------------------------
// Frame send / recv
// ---------------------------------------------------------------------
//
// These are thin routing wrappers over mb_ipc_frame_send /
// _recv that also fan into the debug dumper when
// MOONBASE_DEBUG_JSON=1.

int mb_conn_send(uint16_t kind,
                 const uint8_t *body, size_t body_len,
                 const int *fds, size_t nfds);

// Receive one frame. Return semantics match mb_ipc_frame_recv: 1 on
// success (body is malloc'd, caller frees with free()), 0 on clean
// EOF, negative mb_error_t on failure.
int mb_conn_recv(uint16_t *out_kind,
                 uint8_t **out_body, size_t *out_body_len,
                 int *fds, size_t *io_nfds);

// Synchronous request/reply over the connection. Sends one frame of
// kind `kind` with the given body, then waits for a frame whose kind
// matches `reply_kind`. On success returns 0 and hands the malloc'd
// reply body back via `out_reply_body` / `out_reply_body_len` — the
// caller frees with free().
//
// If the peer replies with MB_IPC_ERROR first, the error body's code
// is decoded and returned (negative mb_error_t). If the peer hangs
// up, returns MB_EIPC. Frames of unrelated kinds that arrive before
// the match are pushed onto the pending-frame queue (see below) and
// delivered to the app's next poll/wait turn — compositor-initiated
// events (a WINDOW_CLOSED landing while a WINDOW_CREATE round-trip
// is in flight) don't get swallowed.
int mb_conn_request(uint16_t kind,
                    const uint8_t *body, size_t body_len,
                    uint16_t reply_kind,
                    uint8_t **out_reply_body, size_t *out_reply_body_len);

// ---------------------------------------------------------------------
// Pending-frame queue
// ---------------------------------------------------------------------
//
// Frames that arrive on the socket but aren't claimed by
// mb_conn_request park here until the app pumps them out via the
// event loop. The queue is single-threaded (main thread only), FIFO,
// and owns the frame body: pop hands body ownership to the caller,
// who must free() with stdlib free() once done.
//
// Returns 1 on pop, 0 if the queue is empty.
int mb_conn_pop_queued(uint16_t *out_kind,
                       uint8_t **out_body, size_t *out_body_len);

#ifdef __cplusplus
}
#endif

#endif // MOONBASE_IPC_TRANSPORT_H
