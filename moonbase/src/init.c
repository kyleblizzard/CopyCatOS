// CopyCatOS — by Kyle Blizzard at Blizzard.show

// init.c — moonbase_init / moonbase_quit.
//
// Phase C slice 1 makes the lifecycle pair real for C apps: open the
// MoonRock socket, run the HELLO/WELCOME handshake, record the
// negotiated capabilities and session type. moonbase_quit sends BYE
// and closes the connection.
//
// Bundle identity comes from the environment for now — the launcher
// (`moonbase-launch`) is responsible for exporting MOONBASE_BUNDLE_ID
// and MOONBASE_BUNDLE_VERSION from the app's Info.appc before exec.
// Direct invocations during development fall back to "unknown.bundle"
// / "0.0.0", which the compositor treats as a dev client.

#include "moonbase.h"
#include "internal.h"
#include "ipc/transport.h"

#include <stdlib.h>

int moonbase_init(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Already initialized — let the caller proceed. Avoids clobbering
    // a good connection if an app wrapper calls init a second time.
    if (mb_conn_is_handshaken()) {
        return 0;
    }

    int rc = mb_conn_open(NULL);
    if (rc < 0) {
        mb_internal_set_last_error((mb_error_t)rc);
        return rc;
    }

    const char *bid = getenv("MOONBASE_BUNDLE_ID");
    const char *bve = getenv("MOONBASE_BUNDLE_VERSION");
    // language == 0 (c). Other runtimes supply their own language
    // value when they call moonbase_init through the binding layer.
    rc = mb_conn_handshake(bid, bve, 0);
    if (rc < 0) {
        mb_conn_close();
        mb_internal_set_last_error((mb_error_t)rc);
        return rc;
    }

    return 0;
}

void moonbase_quit(int exit_code) {
    (void)exit_code;
    // exit_code is accepted for API parity with a future "graceful
    // exit with status" semantic; the current BYE body is empty and
    // MoonRock treats BYE as a clean shutdown regardless.
    mb_conn_close();
}
