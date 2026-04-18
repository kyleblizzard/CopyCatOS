// CopyCatOS — by Kyle Blizzard at Blizzard.show

// main.c — moonbase entry point
//
// moonbase is the CopyCatOS application framework. It is a native C host
// process that opens .appc bundles and executes the code inside them. The
// code inside a bundle can be written in C, Swift, Python, or web tech
// (HTML/JS/CSS rendered in an embedded browser) — moonbase figures out
// which language host to spin up based on the bundle's Info.appc manifest.
//
// This file is the scaffold entry point. For v0.1 it just prints its
// version and the bundle path it was asked to open. Language hosts and
// the browser embed get added in follow-up milestones.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bundle.h"

// Print the command-line usage and exit with a non-zero status.
// Called when the user runs `moonbase` with no arguments or with --help.
static void print_usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s <path-to-bundle.appc>\n"
        "\n"
        "moonbase is the CopyCatOS application framework. It loads\n"
        "an .appc bundle and runs the code inside it.\n"
        "\n"
        "This is the v0.1 scaffold. Language hosts (Swift/Python/Web)\n"
        "are not yet implemented.\n",
        argv0);
}

int main(int argc, char **argv) {
    // No arguments → show usage. moonbase always needs a bundle to open.
    // In a later milestone we may add a background mode that launches
    // via DBus activation, but for now it is a one-shot.
    if (argc < 2 || strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        return 1;
    }

    const char *bundle_path = argv[1];

    // Resolve the bundle path and read its Info.appc manifest.
    // bundle_open() returns a struct describing what language host to
    // launch, where the executable lives inside the bundle, which
    // resources to load, etc. It returns NULL on error after logging.
    struct mb_bundle *bundle = bundle_open(bundle_path);
    if (!bundle) {
        fprintf(stderr, "[moonbase] Failed to open bundle: %s\n", bundle_path);
        return 2;
    }

    // Scaffold behavior: print the parsed bundle info and exit cleanly.
    // Real behavior (future milestone): dispatch to the correct language
    // host based on bundle->host_kind and hand it the bundle descriptor.
    fprintf(stdout, "[moonbase] Bundle opened: %s\n", bundle->display_name);
    fprintf(stdout, "[moonbase] Host language: %s\n", bundle->host_kind);
    fprintf(stdout, "[moonbase] Executable: %s\n", bundle->executable_path);
    fprintf(stdout, "[moonbase] v0.1 scaffold — language hosts not yet wired.\n");

    bundle_close(bundle);
    return 0;
}
