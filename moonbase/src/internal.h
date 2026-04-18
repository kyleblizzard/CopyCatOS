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

// Event-loop hooks called by init / quit. `shutdown` flushes the queue
// and resets the quit latch so the next process (or embedded re-init)
// starts with an empty slate.
void mb_internal_eventloop_shutdown(void);

#endif // MOONBASE_INTERNAL_H
