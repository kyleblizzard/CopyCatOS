// CopyCatOS — by Kyle Blizzard at Blizzard.show

// gl_path.h — internal GL-rendering helper surface.
//
// Drives MOONBASE_RENDER_GL windows. Not public — apps see only the
// moonbase_window_gl_make_current / _swap_buffers pair in moonbase.h.
//
// v1 internal implementation is the simple path:
//
//   * Process-wide EGL display + config + context (OpenGL ES 3).
//   * Per-window EGL pbuffer surface, sized to the window's current
//     backing-pixel dimensions. The app renders into the default
//     framebuffer of that pbuffer.
//   * At swap time the framework glReadPixels()'s the pbuffer's
//     default FB into an shm memfd and hands the memfd to MoonRock
//     through the existing SHM WINDOW_COMMIT path. Zero moonrock-side
//     changes — the compositor still sees ARGB32 premul pixels.
//
// A later slice swaps the internals for GBM + dmabuf zero-copy. The
// public ABI shape (make_current / swap_buffers) does not change.

#ifndef MB_GL_PATH_H
#define MB_GL_PATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "CopyCatAppKit.h"  // mb_error_t

// Opaque per-window GL state. Lives embedded in mb_window as a
// pointer so window.c doesn't drag EGL headers into its own surface.
typedef struct mb_gl_window mb_gl_window_t;

// Initialise the process-wide EGL display + context. Idempotent —
// subsequent calls are no-ops. Returns MB_EOK or a negative error.
//
// May be called from any thread but must not race against itself; in
// practice the framework calls it lazily from the main thread only.
mb_error_t mb_gl_path_init(void);

// Allocate a per-window GL state block and size its pbuffer to the
// given physical-pixel dimensions. Called at the first make_current,
// and again whenever the backing scale changes (the caller releases
// the old block first).
//
// On success *out receives an owned pointer; release with
// mb_gl_window_destroy. On failure returns a negative mb_error_t and
// *out is untouched.
mb_error_t mb_gl_window_create(int px_w, int px_h, mb_gl_window_t **out);

// Release an mb_gl_window_t, unbinding any pbuffer currently current
// in this thread. Safe to call on NULL.
void mb_gl_window_destroy(mb_gl_window_t *win);

// Make the per-window pbuffer + process-wide context current on this
// thread. Every subsequent GL call in the calling thread targets this
// window's default framebuffer. Returns MB_EOK or a negative error.
mb_error_t mb_gl_window_make_current(mb_gl_window_t *win);

// Flush + glFinish, then read the pbuffer's default framebuffer into
// `dst`. Pixel layout is native-byte-order ARGB32 premultiplied alpha
// — matches the Cairo path exactly so MoonRock stays mode-agnostic.
//
// `dst` must point to at least stride_bytes * px_h bytes. Returns
// MB_EOK or a negative error.
mb_error_t mb_gl_window_read_framebuffer(mb_gl_window_t *win,
                                         int stride_bytes,
                                         void *dst);

// Pixel dimensions the pbuffer was created with. Used by window.c to
// decide whether to recreate the block on a backing-scale change.
void mb_gl_window_pixel_size(const mb_gl_window_t *win,
                             int *px_w, int *px_h);

#endif // MB_GL_PATH_H
