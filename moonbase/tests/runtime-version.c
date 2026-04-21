// CopyCatOS — by Kyle Blizzard at Blizzard.show

// mb-runtime-version — moonbase_runtime_version() symbol test.
//
// Proves three things:
//   1. The symbol exists in libmoonbase.so.1 (link succeeds).
//   2. It returns the same value the header advertises today —
//      MOONBASE_API_VERSION from the same .so the test linked against.
//   3. It shares a major with MOONBASE_API_VERSION, the invariant the
//      soname guarantees.
//
// This is a compile-time-against-build check, not a real SDK/runtime
// skew test. Actual skew will only show up once the SDK ships
// separately from the OS image and the two can diverge on disk; until
// then, the runtime value echoing the macro is exactly what we want.

#include <CopyCatAppKit.h>

#include <stdio.h>

int main(void) {
    uint32_t runtime = moonbase_runtime_version();

    if (runtime != (uint32_t)MOONBASE_API_VERSION) {
        fprintf(stderr,
                "FAIL: runtime %u != MOONBASE_API_VERSION %u\n",
                runtime, (uint32_t)MOONBASE_API_VERSION);
        return 1;
    }

    if (runtime / 10000u != (uint32_t)MOONBASE_API_VERSION / 10000u) {
        fprintf(stderr,
                "FAIL: major mismatch — runtime %u, header %u\n",
                runtime, (uint32_t)MOONBASE_API_VERSION);
        return 1;
    }

    if (runtime == 0) {
        fprintf(stderr, "FAIL: runtime version is zero\n");
        return 1;
    }

    return 0;
}
