// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase-consent — first-launch consent sheet (Phase D slice 4 stub).
//
// Real impl (slice D.5) draws an Aqua consent sheet with Cairo over a
// short-lived MoonBase window, summarises declared entitlements, and
// asks the user whether to trust the bundle.
//
// This slice ships a console-side stub so the launcher's quarantine
// gate has a concrete helper to exec and so the integration test has
// something deterministic to fork. Behaviour:
//
//   * Reads two positional args: <bundle_path> <bundle_id>.
//   * Optional env:
//       MOONBASE_CONSENT_AUTO = "approve"  -> exit 0 (trust)
//       MOONBASE_CONSENT_AUTO = "reject"   -> exit 1 (refuse)
//       MOONBASE_CONSENT_AUTO unset        -> print an approval banner
//                                             and exit 0 (default-allow
//                                             until D.5 ships)
//
//   * Never reads from stdin. There is no interactive mode here — this
//     is exclusively the stub / test-shim surface.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "moonbase-consent: usage: %s <bundle-path> <bundle-id>\n",
                argv[0]);
        return 2;
    }
    const char *bundle_path = argv[1];
    const char *bundle_id   = argv[2];

    const char *mode = getenv("MOONBASE_CONSENT_AUTO");
    if (mode && strcmp(mode, "reject") == 0) {
        fprintf(stderr, "moonbase-consent: declining %s at %s (MOONBASE_CONSENT_AUTO=reject)\n",
                bundle_id, bundle_path);
        return 1;
    }
    if (mode && strcmp(mode, "approve") == 0) {
        return 0;
    }
    fprintf(stderr,
        "moonbase-consent [stub]: approving %s at %s (replace with "
        "Aqua consent sheet in D.5)\n",
        bundle_id, bundle_path);
    return 0;
}
