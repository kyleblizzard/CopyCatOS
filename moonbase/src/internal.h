// CopyCatOS — by Kyle Blizzard at Blizzard.show

// internal.h — cross-TU helpers inside libmoonbase.so.1.
//
// Nothing here is exported through moonbase.h. Symbols prefixed with
// `mb_internal_` are for use between translation units of the library
// itself (error.c + stubs.c today, more as Phase C onwards fills in
// real bodies). Keep this header tight: every symbol added here
// widens the surface between library files.

#ifndef MOONBASE_INTERNAL_H
#define MOONBASE_INTERNAL_H

#include "moonbase.h"

// Record the given error on the calling thread's last-error slot. The
// public getter moonbase_last_error() reads from this same slot.
void mb_internal_set_last_error(mb_error_t err);

// Window registry lookup. Used by eventloop.c to fill `ev->window` on
// compositor-originated frames that carry a window_id. Returns NULL
// when no live mb_window_t matches — unknown window_ids are treated as
// stale references (the app already closed the window locally) and
// silently dropped rather than surfaced to the app.
mb_window_t *mb_internal_window_find(uint32_t window_id);

// Apply a new backing scale to a window handle. Called by the event
// pump when MB_IPC_BACKING_SCALE_CHANGED arrives. Retires any pending
// (uncommitted) Cairo frame so the next moonbase_window_cairo()
// allocation honors the new scale. Also updates the cached output_id
// the handle reports. Does nothing if new_scale matches the current
// value.
void mb_internal_window_apply_backing_scale(mb_window_t *w,
                                            float new_scale,
                                            uint32_t new_output_id);

// Apply a new content size (in points) to a window handle. Called by
// the event pump when MB_IPC_WINDOW_RESIZED arrives, before the event
// is delivered to the app. Retires any pending uncommitted Cairo frame
// (which was sized against the old dims) and any GL pbuffer so the
// next moonbase_window_cairo() / make_current allocates at the new
// size. Subsequent moonbase_window_size() calls report the new dims.
// Does nothing when the dims already match.
void mb_internal_window_apply_resize(mb_window_t *w,
                                     int new_width_points,
                                     int new_height_points);

// Event-loop hooks called by init / quit. `shutdown` flushes the queue
// and resets the quit latch so the next process (or embedded re-init)
// starts with an empty slate.
void mb_internal_eventloop_shutdown(void);

// Enqueue a local MB_EV_WINDOW_REDRAW for `w` on this process's own
// event ring. Used by moonbase_window_request_redraw so an app that
// asks for a redraw sees the event on the next pump turn without any
// server round-trip. If `width` or `height` is <= 0 the helper fills
// the rect with the window's full content size (matches the documented
// "whole content if w/h are 0" contract in moonbase.h). A no-op when
// `w` is NULL.
void mb_internal_eventloop_post_redraw(mb_window_t *w,
                                       int x, int y,
                                       int width, int height);

#endif // MOONBASE_INTERNAL_H
