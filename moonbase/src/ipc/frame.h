// CopyCatOS — by Kyle Blizzard at Blizzard.show

// frame.h — TLV frame I/O for the MoonBase protocol.
//
// IPC.md §2 wire format:
//   u32 length (BE) | u16 kind (BE) | body (length - 2 bytes)
//
// `length` is the byte count *after* the length field, so a frame
// with no body has length == 2. Frames are capped at 1 MiB —
// anything larger is an MB_EPROTO and the connection is closed.
//
// Some frames carry SCM_RIGHTS ancillary data (SHM / DMA-BUF fds
// for the surface-memory transport — IPC.md §6). Both send and
// recv take an optional fd array and count. When no ancillary
// data is attached, pass nfds == 0.

#ifndef MOONBASE_IPC_FRAME_H
#define MOONBASE_IPC_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MB_IPC_FRAME_MAX_LEN  (1u << 20)   // 1 MiB, see IPC.md §2
#define MB_IPC_FRAME_MAX_FDS  4            // most we ship per frame

// Send one frame on `fd`. `kind` is the u16 MB_IPC_* constant;
// `body`/`body_len` is the CBOR-encoded body. `fds`/`nfds` is an
// optional ancillary data attachment.
//
// Returns 0 on success, a negative mb_error_t on failure. Handles
// EINTR internally; retries partial writes until the whole frame is
// out or the connection fails.
int mb_ipc_frame_send(int fd,
                      uint16_t kind,
                      const uint8_t *body, size_t body_len,
                      const int *fds, size_t nfds);

// Receive one frame on `fd`, blocking. On success:
//   *out_kind  = received kind
//   *out_body  = newly malloc'd body buffer (caller frees)
//   *out_body_len = body length in bytes
//   fds[0..*io_nfds)] = received fds (if any); caller closes.
// On EOF returns 0 and leaves outputs unchanged. On error returns
// a negative mb_error_t.
//
// `io_nfds` on entry is the caller's fd-array capacity; on exit it
// is the number of fds actually received. If the peer attaches more
// fds than the caller offers room for, the extras are closed and
// MB_EPROTO is returned.
int mb_ipc_frame_recv(int fd,
                      uint16_t *out_kind,
                      uint8_t **out_body, size_t *out_body_len,
                      int *fds, size_t *io_nfds);

// Server-side socket management.
//
// bind: create an AF_UNIX/SOCK_STREAM socket, unlink any stale file
// at `path`, bind, chmod 0600, listen. Returns a listening fd on
// success or negative mb_error_t.
int mb_ipc_frame_listen(const char *path);

// Accept one client; blocks. Returns client fd or negative error.
// Checks SO_PEERCRED: the peer's effective UID must match the
// running process's EUID, otherwise MB_EPERM.
int mb_ipc_frame_accept(int listen_fd);

// Client-side connect to an AF_UNIX socket at `path`. Returns the
// connected fd or negative error.
int mb_ipc_frame_connect(const char *path);

#ifdef __cplusplus
}
#endif

#endif // MOONBASE_IPC_FRAME_H
