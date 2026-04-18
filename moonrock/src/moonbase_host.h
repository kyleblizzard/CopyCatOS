// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase_host.h — MoonRock's side of the MoonBase IPC.
//
// This is the thin adapter between the moonbase server primitives
// (libmoonbase's src/server/server.c) and moonrock's main event
// loop. moonrock owns the compositor-wide socket; every app that
// links libmoonbase.so.1 connects here.
//
// Slice 2 scope: open the listener, accept clients, run the HELLO /
// WELCOME / BYE handshake, log each event. Window creation, input
// routing, and per-output scale reporting are slice 3+.

#ifndef MOONROCK_MOONBASE_HOST_H
#define MOONROCK_MOONBASE_HOST_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>

#include <GL/gl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the MoonBase IPC server. Resolves the socket path as
// `$XDG_RUNTIME_DIR/moonbase.sock` if `path` is NULL. Returns true on
// success. On failure, logs why and returns false — moonrock runs
// happily without MoonBase apps, just with no way to host them.
bool mb_host_init(const char *path);

// Populate `out_fds` with every fd moonrock should poll on behalf of
// MoonBase apps (listener + per-client). Returns the number written,
// clamped to `max`. Safe to call before mb_host_init or after
// mb_host_shutdown — returns 0 in both cases.
size_t mb_host_collect_pollfds(struct pollfd *out_fds, size_t max);

// Process I/O that poll() flagged ready. Safe to call with nfds==0.
void mb_host_tick(const struct pollfd *fds, size_t nfds);

// Render every live MoonBase surface into the current GL context. Called
// from mr_composite during the normal-window pass. The caller supplies
// the same basic-shader / ortho-projection pair the X-client draw uses,
// so MoonBase windows pixel-match X windows in the output. No-ops if
// there are no live surfaces. Must be called with the compositor's GL
// context current (mr_composite guarantees this).
void mb_host_render(GLuint basic_shader, float *projection);

// Close the listener, tear down every connected client, unlink the
// socket path, and free server state. Safe to call when never
// initialized.
void mb_host_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // MOONROCK_MOONBASE_HOST_H
