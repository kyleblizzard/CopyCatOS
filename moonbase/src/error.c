// CopyCatOS — by Kyle Blizzard at Blizzard.show

// error.c — thread-local last-error slot, error-string table, and
// the moonbase_release() free-wrapper.
//
// These three symbols are the only pieces of libmoonbase that are
// NOT stubs in Phase B. Everything else in stubs.c returns MB_ENOSYS
// (or sets last-error to MB_ENOSYS for the pointer-returning shape)
// and waits for Phase C to replace the body with a real IPC call to
// MoonRock.
//
// Making these three real in Phase B is deliberate: it lets us
// exercise the stub-error convention end-to-end. A caller that
// receives NULL from moonbase_window_create() can already read
// moonbase_last_error() and turn it into a readable string.

#include "moonbase.h"
#include "internal.h"

#include <stdlib.h>

// Per-thread storage. Every MoonBase call runs on the thread that
// made it (UI calls on the main thread, anything else the caller
// chose), so the last-error slot is kept strictly thread-local to
// avoid cross-thread mis-reads. __thread is supported by glibc,
// musl, and Clang on Linux — the only platforms libmoonbase targets.
static __thread mb_error_t tls_last_error = MB_EOK;

void mb_internal_set_last_error(mb_error_t err) {
    tls_last_error = err;
}

mb_error_t moonbase_last_error(void) {
    return tls_last_error;
}

const char *moonbase_error_string(mb_error_t err) {
    switch (err) {
        case MB_EOK:        return "ok";
        case MB_EINVAL:     return "invalid argument";
        case MB_ENOSYS:     return "not implemented";
        case MB_EIPC:       return "compositor IPC failure";
        case MB_EPERM:      return "denied by entitlement or sandbox";
        case MB_ENOMEM:     return "out of memory";
        case MB_ENOTFOUND:  return "not found";
        case MB_EAGAIN:     return "temporarily unavailable";
        case MB_EPROTO:     return "protocol error";
        case MB_EVERSION:   return "incompatible compositor version";
    }
    return "unknown error";
}

void moonbase_release(void *ptr) {
    // Any buffer handed out by a MoonBase function that documented
    // "caller frees with moonbase_release" lands here. Routing
    // through a framework-owned free() means we can swap to an arena
    // allocator later without breaking callers.
    free(ptr);
}
