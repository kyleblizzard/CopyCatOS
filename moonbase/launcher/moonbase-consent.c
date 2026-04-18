// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase-consent — first-launch consent sheet.
//
// Exec'd by moonbase-launch when a bundle's user.moonbase.quarantine
// xattr is "pending". Exit 0 = trust, exit 1 = refuse; the launcher
// persists the choice on the bundle's xattr and proceeds (or aborts).
//
// Phase D slice 5 ships two user-facing fronts and one automation shim:
//
//   1. $MOONBASE_CONSENT_AUTO = "approve" | "reject"
//        Deterministic override for tests, CI, and kiosks. Always wins.
//
//   2. If stdin is a TTY: interactive text prompt on stderr.
//        Dev / terminal-launched scenario. Prints the bundle's name,
//        version, language, rendering mode, and declared entitlements
//        (the exact same data that the Aqua sheet shows), then reads a
//        y/n line from stdin. Empty answer = Cancel (safe default).
//
//   3. Otherwise: print the summary to stderr and refuse (exit 1).
//        Headless launch with no automation override and no terminal —
//        there's no safe way to ask the user, so we default to cancel.
//        A follow-up slice will plug in the MoonBase-drawn Aqua sheet
//        here; once pointer events are routed to client windows the
//        helper graduates into its own tiny MoonBase app.

#include "bundle/bundle.h"
#include "bundle/info_appc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Pretty labels for enum values, so the summary the user reads matches
// the words they'd see in systemcontrol → Security & Privacy later.
static const char *lang_label(mb_info_appc_lang_t l) {
    switch (l) {
    case MB_INFO_APPC_LANG_C:      return "C";
    case MB_INFO_APPC_LANG_WEB:    return "Web";
    case MB_INFO_APPC_LANG_PYTHON: return "Python";
    case MB_INFO_APPC_LANG_RUST:   return "Rust";
    case MB_INFO_APPC_LANG_SWIFT:  return "Swift";
    }
    return "?";
}

static const char *render_label(mb_info_appc_render_t r) {
    switch (r) {
    case MB_INFO_APPC_RENDER_DEFAULT: return "default";
    case MB_INFO_APPC_RENDER_CAIRO:   return "Cairo (CPU)";
    case MB_INFO_APPC_RENDER_GL:      return "OpenGL";
    }
    return "?";
}

// Print one indented bullet per entry, or "  (none)" if empty. The
// sheet shows the same lines — summary on stderr is both the text-mode
// UI *and* a transcript the user can review after approving.
static void print_perm_group(FILE *out, const char *group,
                             char *const *entries, size_t count) {
    fprintf(out, "  %s:\n", group);
    if (count == 0) {
        fprintf(out, "    (none)\n");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        fprintf(out, "    - %s\n", entries[i]);
    }
}

static void print_summary(FILE *out, const char *bundle_path,
                          const mb_info_appc_t *info) {
    fprintf(out,
        "\n"
        "━━ Open this bundle? ━━\n"
        "  %s %s  (%s)\n"
        "  bundle id   : %s\n"
        "  path        : %s\n"
        "  language    : %s\n"
        "  rendering   : %s\n"
        "  needs MB API: %s\n",
        info->name ? info->name : "(unnamed)",
        info->version ? info->version : "",
        info->copyright ? info->copyright : "no copyright notice",
        info->id,
        bundle_path,
        lang_label(info->lang),
        render_label(info->render_default),
        info->minimum_moonbase ? info->minimum_moonbase : "1.0");

    fprintf(out, "\n  Declared permissions:\n");
    print_perm_group(out, "filesystem", info->perm_filesystem, info->perm_filesystem_count);
    print_perm_group(out, "network",    info->perm_network,    info->perm_network_count);
    print_perm_group(out, "hardware",   info->perm_hardware,   info->perm_hardware_count);
    print_perm_group(out, "system",     info->perm_system,     info->perm_system_count);
    print_perm_group(out, "ipc",        info->perm_ipc,        info->perm_ipc_count);
    fprintf(out, "\n");
}

// Read a y/n answer from stdin. Anything starting with 'y' / 'Y' = yes;
// everything else, including EOF and a bare newline, = no. Safe
// default: the user who forgot to type anything didn't authorize.
static int prompt_yn(void) {
    char line[64];
    fprintf(stderr, "Allow this app to run? [y/N] ");
    fflush(stderr);
    if (!fgets(line, sizeof(line), stdin)) return 0;
    for (size_t i = 0; line[i]; i++) {
        if (line[i] == ' ' || line[i] == '\t') continue;
        return (line[i] == 'y' || line[i] == 'Y') ? 1 : 0;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
            "moonbase-consent: usage: %s <bundle-path> <bundle-id>\n",
            argv[0]);
        return 2;
    }
    const char *bundle_path = argv[1];
    const char *bundle_id   = argv[2];

    // 1. Automation override always wins — tests / CI / unattended
    //    kiosk scenarios need to be deterministic.
    const char *mode = getenv("MOONBASE_CONSENT_AUTO");
    if (mode && strcmp(mode, "reject") == 0) {
        fprintf(stderr,
            "moonbase-consent: declining %s at %s (MOONBASE_CONSENT_AUTO=reject)\n",
            bundle_id, bundle_path);
        return 1;
    }
    if (mode && strcmp(mode, "approve") == 0) {
        return 0;
    }

    // 2. Load the bundle's Info.appc so we have real data to show. If
    //    this fails the launcher already validated the bundle
    //    successfully (that's the only way we'd be here), so a parse
    //    failure on our side is a bug, not a user-facing error — but
    //    bail with "reject" to be safe.
    mb_bundle_t b;
    char err[512] = {0};
    mb_bundle_err_t rc = mb_bundle_load(bundle_path, &b, err, sizeof(err));
    if (rc != MB_BUNDLE_OK) {
        fprintf(stderr, "moonbase-consent: couldn't reload %s: %s (%s)\n",
                bundle_path, err, mb_bundle_err_string(rc));
        return 1;
    }

    print_summary(stderr, bundle_path, &b.info);

    // 3. If stdin is a terminal, ask the user directly.
    int answer_is_yes = 0;
    if (isatty(STDIN_FILENO)) {
        answer_is_yes = prompt_yn();
    } else {
        // Headless, no automation override. The launcher will print a
        // follow-up and the user can re-run from a terminal or set
        // MOONBASE_CONSENT_AUTO. Cancel is the safe default.
        fprintf(stderr,
            "moonbase-consent: no TTY + no MOONBASE_CONSENT_AUTO — "
            "declining %s.\n"
            "Re-run from a terminal or set MOONBASE_CONSENT_AUTO=approve.\n",
            bundle_id);
    }

    mb_bundle_free(&b);
    return answer_is_yes ? 0 : 1;
}
