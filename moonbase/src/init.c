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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int moonbase_init(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Already initialized — let the caller proceed. Avoids clobbering
    // a good connection if an app wrapper calls init a second time.
    if (mb_conn_is_handshaken()) {
        return 0;
    }

    // Reset the event-loop state. The ring is static storage, so a
    // prior quit + init cycle in the same process would otherwise see
    // a latched quit flag and a stray APP_WILL_QUIT still queued.
    mb_internal_eventloop_shutdown();

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

// moonbase_quit lives in eventloop.c — it posts MB_EV_APP_WILL_QUIT
// before tearing the connection down.

// ---------------------------------------------------------------------
// Bundle path accessors
// ---------------------------------------------------------------------
//
// moonbase-launch exports MOONBASE_BUNDLE_PATH inside the bwrap
// sandbox; native.profile ro-binds the host bundle path to itself, so
// the same string is valid on both sides. The env value is cached on
// first read — env can mutate (or be cleared by an embedding host),
// and the bundle path is conceptually a constant for the process'
// lifetime.

static const char *g_bundle_path_cached = NULL;
static int g_bundle_path_resolved = 0;

const char *moonbase_bundle_path(void) {
    if (!g_bundle_path_resolved) {
        const char *p = getenv("MOONBASE_BUNDLE_PATH");
        if (p && *p) {
            g_bundle_path_cached = p;
        }
        g_bundle_path_resolved = 1;
    }
    if (!g_bundle_path_cached) {
        mb_internal_set_last_error(MB_ENOTFOUND);
        return NULL;
    }
    return g_bundle_path_cached;
}

char *moonbase_bundle_resource_path(const char *relative) {
    if (!relative || !*relative) {
        mb_internal_set_last_error(MB_EINVAL);
        return NULL;
    }
    const char *base = moonbase_bundle_path();
    if (!base) {
        // last_error already set by moonbase_bundle_path
        return NULL;
    }
    // <base>/Contents/Resources/<relative>. Locale fallback (en.lproj
    // etc.) is a future slice — for now this is a flat resolver, which
    // is what slice 19.H.3-α and the other early reference apps need.
    size_t need = strlen(base) + strlen("/Contents/Resources/")
                + strlen(relative) + 1;
    char *buf = malloc(need);
    if (!buf) {
        mb_internal_set_last_error(MB_ENOMEM);
        return NULL;
    }
    int n = snprintf(buf, need, "%s/Contents/Resources/%s", base, relative);
    if (n < 0 || (size_t)n >= need) {
        free(buf);
        mb_internal_set_last_error(MB_EINVAL);
        return NULL;
    }
    return buf;
}
