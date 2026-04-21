// CopyCatOS — by Kyle Blizzard at Blizzard.show

// version.c — moonbase_runtime_version().
//
// The compile-time MOONBASE_API_VERSION lives in <moonbase.h>. This
// file bakes that same value into a symbol inside libmoonbase.so so
// callers can ask the loaded runtime what version it is, not just
// what version they were built against. The two answers diverge the
// moment the SDK ships separately from the OS image — an app built
// against 1.0.1 headers can end up running on a host with a 1.0.5
// runtime, and the only way it finds out is by calling this.
//
// Not a stub. Real, cheap, and ABI-safe: a literal field read.
// Kept in its own file so the intent is obvious and so Phase C
// changes to init.c or error.c never need to touch version code.

#include "CopyCatAppKit.h"

uint32_t moonbase_runtime_version(void) {
    return (uint32_t)MOONBASE_API_VERSION;
}
