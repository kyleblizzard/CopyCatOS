// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase-version — print the loaded libmoonbase.so runtime version.
//
// Answers exactly one question: "what MoonBase API version is this
// process running against?" Prints `MAJOR.MINOR.PATCH\n` to stdout and
// exits 0. No flags, no options — one fact, one line, so shell consumers
// can grab it with `$(moonbase-version)` without parsing.
//
// This is NOT the CopyCatOS system version (that surface lands later as
// `copycatos-version`). Keep the two separate so a future divergence —
// e.g. CopyCatOS 0.14.x shipping libmoonbase.so.1.0.5 — reports cleanly
// on both axes.

#include <moonbase.h>

#include <stdio.h>

int main(void) {
    uint32_t v = moonbase_runtime_version();
    unsigned major = (unsigned)(v / 10000u);
    unsigned minor = (unsigned)((v / 100u) % 100u);
    unsigned patch = (unsigned)(v % 100u);
    printf("%u.%u.%u\n", major, minor, patch);
    return 0;
}
