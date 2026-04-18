// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase-launch — Phase B scaffold launcher.
//
// A real launcher will:
//   1. Read the target .appc bundle's Info.appc (TOML).
//   2. Pick the sandbox tier (native or webview) declared in the
//      bundle.
//   3. Source the matching sandbox/*.profile shell script to build
//      the bwrap arg list, then exec bwrap with the bundle's
//      executable as the inner command.
//   4. Check the quarantine xattr and present the first-launch
//      consent sheet if needed.
//
// Phase B scaffold does *none* of that. It simply execs a dummy
// inner command (/bin/true by default, or the arg list after `--`)
// inside a bwrap sandbox configured with the minimum viable arg
// list. This is enough to prove on the target machine that:
//   a) bwrap is installed,
//   b) the exec chain works,
//   c) Phase C+D can hang real logic off the same binary without
//      reshaping it.

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// The smallest bwrap invocation that actually runs /bin/true on a
// current Linux kernel: read-only bind of the rootfs, a fresh /proc
// and /dev, and an --unshare-all namespace. Every real tier builds
// on top of this — native.profile and webview.profile emit these
// same args plus the bundle/data bindings.
static const char *const bwrap_args_scaffold[] = {
    "bwrap",
    "--ro-bind",   "/",     "/",
    "--proc",      "/proc",
    "--dev",       "/dev",
    "--unshare-all",
    "--die-with-parent",
    "--new-session",
    NULL,
};

static void usage(const char *argv0) {
    fprintf(stderr,
        "moonbase-launch (Phase B scaffold)\n"
        "\n"
        "Usage:\n"
        "  %s                        exec /bin/true inside bwrap\n"
        "  %s -- <inner-cmd> [args]  exec <inner-cmd> inside bwrap\n"
        "\n"
        "The leading `--` is mandatory so future moonbase-launch flags\n"
        "cannot collide with the inner command's own arg space.\n",
        argv0, argv0);
}

int main(int argc, char **argv) {
    // Default inner command: /bin/true. Always present on every
    // Linux host, exits 0 immediately, cheapest possible probe that
    // the sandbox actually ran something.
    char *default_inner[] = { (char *)"/bin/true", NULL };
    char **inner_argv = default_inner;

    if (argc == 1) {
        // no-arg default path.
    } else if (argc >= 3 && strcmp(argv[1], "--") == 0) {
        inner_argv = &argv[2];
    } else {
        usage(argv[0]);
        return 2;
    }

    // Count lengths to build a single argv on the stack. Layout:
    //   bwrap_args_scaffold... <inner_argv...> NULL
    // (bwrap reads its own flags until it hits a non-flag, then
    // treats the remainder as the command to run inside the
    // sandbox, so no `--` separator is needed.)
    size_t base_len = 0;
    while (bwrap_args_scaffold[base_len]) base_len++;

    size_t inner_len = 0;
    while (inner_argv[inner_len]) inner_len++;

    // VLA is fine: both counts are small (< 64) and bounded by argc.
    char *argv_full[base_len + inner_len + 1];
    for (size_t i = 0; i < base_len; i++) {
        argv_full[i] = (char *)bwrap_args_scaffold[i];
    }
    for (size_t i = 0; i < inner_len; i++) {
        argv_full[base_len + i] = inner_argv[i];
    }
    argv_full[base_len + inner_len] = NULL;

    execvp("bwrap", argv_full);

    // execvp returns only on failure.
    fprintf(stderr, "moonbase-launch: execvp(bwrap) failed: %s\n",
            strerror(errno));
    return 127;
}
