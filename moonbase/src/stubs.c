// CopyCatOS — by Kyle Blizzard at Blizzard.show

// stubs.c — Phase B stubs for every moonbase.h symbol that isn't
// implemented yet.
//
// Convention, applied uniformly across all 41 stubs in this file:
//   int-returning     -> set last-error to MB_ENOSYS, return MB_ENOSYS
//   pointer-returning -> set last-error to MB_ENOSYS, return NULL
//   float-returning   -> set last-error to MB_ENOSYS, return 1.0f
//                        (1.0 is the sensible no-backend default for
//                         backing scale; callers that care should
//                         check last-error)
//   void-returning    -> set last-error to MB_ENOSYS, no side effect.
//                        Functions with out-params zero the out slot
//                        before returning so callers never see
//                        uninitialized bytes.
//
// Two functions break the pattern because they're capability probes,
// not failable operations: moonbase_has_capability and
// moonbase_has_entitlement legitimately return 0 ("no, we don't have
// that") without that being an error state.
//
// The three load-bearing functions (moonbase_last_error,
// moonbase_error_string, moonbase_release) are *not* stubbed — they
// live in error.c so this stub convention can be observed from the
// outside even before Phase C wires real bodies in.

#include "moonbase.h"
#include "internal.h"

#include <stddef.h>

// Uniform error-setter. Inlined by the compiler; kept as a named
// helper so the stub bodies read cleanly.
static inline void nosys(void) {
    mb_internal_set_last_error(MB_ENOSYS);
}

// ---------------------------------------------------------------------
// Lifecycle + main loop
// ---------------------------------------------------------------------

// moonbase_init lives in init.c; moonbase_quit, moonbase_poll_event,
// and moonbase_wait_event live in eventloop.c.

int moonbase_run(void) {
    nosys();
    return MB_ENOSYS;
}

void moonbase_set_event_handler(mb_event_handler_t fn, void *userdata) {
    (void)fn;
    (void)userdata;
    // Deliberately does NOT set last-error: installing a handler is
    // a fire-and-forget configuration call. The handler is only
    // consulted by moonbase_run(), which is still ENOSYS in this slice.
}

void moonbase_dispatch_main(void (*fn)(void *), void *userdata) {
    (void)fn;
    (void)userdata;
    nosys();
}

// ---------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------
//
// moonbase_window_create, moonbase_window_close, moonbase_window_size,
// moonbase_window_backing_scale, moonbase_window_backing_pixel_size
// are real — see window.c.

void moonbase_window_show(mb_window_t *w) {
    (void)w;
    nosys();
}

void moonbase_window_set_title(mb_window_t *w, const char *title_utf8) {
    (void)w;
    (void)title_utf8;
    nosys();
}

int moonbase_window_set_size(mb_window_t *w,
                             int width_points, int height_points) {
    (void)w;
    (void)width_points;
    (void)height_points;
    nosys();
    return MB_ENOSYS;
}

void moonbase_window_position(mb_window_t *w, int *x, int *y) {
    (void)w;
    if (x) *x = 0;
    if (y) *y = 0;
    nosys();
}

int moonbase_window_set_position(mb_window_t *w, int x, int y) {
    (void)w;
    (void)x;
    (void)y;
    nosys();
    return MB_ENOSYS;
}

void moonbase_window_request_redraw(mb_window_t *w,
                                    int x, int y, int width, int height) {
    (void)w;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    nosys();
}

const char *moonbase_window_output_name(mb_window_t *w) {
    (void)w;
    nosys();
    return NULL;
}

// ---------------------------------------------------------------------
// HiDPI — backing scale queries are real — see window.c.
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// Render handoff — Cairo + GL paths
// ---------------------------------------------------------------------
//
// moonbase_window_cairo and moonbase_window_commit are real — see
// window.c (slice 3c.1). The GL pair is still stubbed; slice 3c.3
// replaces them with EGL + dma-buf.

int moonbase_window_gl_make_current(mb_window_t *w) {
    (void)w;
    nosys();
    return MB_ENOSYS;
}

int moonbase_window_gl_swap_buffers(mb_window_t *w) {
    (void)w;
    nosys();
    return MB_ENOSYS;
}

// ---------------------------------------------------------------------
// Capability + entitlement queries
// ---------------------------------------------------------------------
//
// Both return 0/1 booleans, NOT error codes. "The runtime doesn't
// have this capability" is a legitimate answer, not a failure — so
// these two stubs do NOT set last-error. A caller checking
// `if (moonbase_has_capability("gl")) ...` on a Phase B build
// correctly takes the fallback path.

int moonbase_has_capability(const char *name) {
    (void)name;
    return 0;
}

int moonbase_has_entitlement(const char *name) {
    (void)name;
    return 0;
}

// ---------------------------------------------------------------------
// Bundle + app metadata
// ---------------------------------------------------------------------

const char *moonbase_bundle_id(void) {
    nosys();
    return NULL;
}

const char *moonbase_bundle_name(void) {
    nosys();
    return NULL;
}

const char *moonbase_bundle_version(void) {
    nosys();
    return NULL;
}

const char *moonbase_bundle_path(void) {
    nosys();
    return NULL;
}

char *moonbase_bundle_resource_path(const char *relative) {
    (void)relative;
    nosys();
    return NULL;
}

// ---------------------------------------------------------------------
// Per-app data paths
// ---------------------------------------------------------------------

const char *moonbase_data_path(void) {
    nosys();
    return NULL;
}

const char *moonbase_prefs_path(void) {
    nosys();
    return NULL;
}

const char *moonbase_cache_path(void) {
    nosys();
    return NULL;
}

// ---------------------------------------------------------------------
// Preferences
// ---------------------------------------------------------------------
//
// Getters return the caller-provided fallback so apps can write
// tolerant code (`int n = moonbase_prefs_get_int("limit", 10);`) and
// still work against a stubbed libmoonbase. last-error flags that
// the real store wasn't consulted, for callers who care.

const char *moonbase_prefs_get_string(const char *key, const char *fallback) {
    (void)key;
    nosys();
    return fallback;
}

int moonbase_prefs_set_string(const char *key, const char *value) {
    (void)key;
    (void)value;
    nosys();
    return MB_ENOSYS;
}

int moonbase_prefs_get_int(const char *key, int fallback) {
    (void)key;
    nosys();
    return fallback;
}

int moonbase_prefs_set_int(const char *key, int value) {
    (void)key;
    (void)value;
    nosys();
    return MB_ENOSYS;
}

bool moonbase_prefs_get_bool(const char *key, bool fallback) {
    (void)key;
    nosys();
    return fallback;
}

int moonbase_prefs_set_bool(const char *key, bool value) {
    (void)key;
    (void)value;
    nosys();
    return MB_ENOSYS;
}

int moonbase_prefs_remove(const char *key) {
    (void)key;
    nosys();
    return MB_ENOSYS;
}

int moonbase_prefs_sync(void) {
    nosys();
    return MB_ENOSYS;
}
