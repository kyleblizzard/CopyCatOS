// CopyCatOS — by Kyle Blizzard at Blizzard.show

// consent-gate-ipc.c — Phase D slice 6A end-to-end gate test.
//
// Pins mb_consent_gate_allows' deny-on-missing + IPC path. The parent
// stands up mb_server_t + mb_consent_responder pointing at the real
// moonbase-consent binary, then forks the client once per phase:
//
//   phase 1 MISSING / AUTO=approve + handshake
//           → gate queries consents.toml (no file), sees handshaken,
//             dispatches MB_IPC_CONSENT_REQUEST, responder forks
//             moonbase-consent (exits 0 → GRANT), replies CONSENT_GRANT,
//             gate returns true. consents.toml lands ALLOW.
//   phase 2 MISSING / AUTO=reject + handshake
//           → gate dispatches IPC, responder forks consent helper
//             (exits 1 → DENY), replies CONSENT_DENY, gate returns
//             false. consents.toml lands DENY for this capability.
//   phase 3 MISSING / no handshake
//           → child never calls moonbase_init, so mb_conn_is_handshaken
//             is false. Gate denies without touching IPC; consents.toml
//             is never written for this capability.
//
// Complements consent-reader.c (which exercises the ALLOW/DENY readback
// paths with no compositor) and consent-responder.c (which exercises
// the raw mb_ipc_consent_request wire). This test is the only one
// that pins the gate's deny-on-missing + IPC fallback end-to-end.

#include "moonbase.h"
#include "consents.h"
#include "../src/ipc/consent.h"
#include "../src/server/server.h"
#include "../src/server/consent_responder.h"
#include "moonbase/ipc/kinds.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FAIL(...) do { \
    fprintf(stderr, "FAIL: " __VA_ARGS__); \
    fputc('\n', stderr); \
    exit(1); \
} while (0)

// Forward REQUEST frames into the responder; handle CONNECTED /
// DISCONNECTED so the responder knows the client's bundle_id.
static void on_event(mb_server_t *s, const mb_server_event_t *ev, void *user) {
    (void)user;
    switch (ev->kind) {
        case MB_SERVER_EV_CONNECTED:
            mb_consent_responder_note_connected(ev->client,
                                                ev->hello.bundle_id);
            break;
        case MB_SERVER_EV_FRAME:
            if (ev->frame_kind == MB_IPC_CONSENT_REQUEST) {
                mb_consent_responder_handle_request(
                    s, ev->client, ev->frame_body, ev->frame_body_len);
            }
            break;
        case MB_SERVER_EV_DISCONNECTED:
            mb_consent_responder_note_disconnected(ev->client);
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────
// Child phases — one exec of the child per phase.
// ─────────────────────────────────────────────────────────────────────

static int phase_missing_approve(void) {
    // No consents.toml exists yet (fresh tmpdir + fresh bundle-id).
    // Gate must hit IPC and come back with true.
    if (!mb_consent_gate_allows("system", "keychain")) {
        fprintf(stderr, "phase1: gate returned false (want true)\n");
        return 11;
    }
    return 0;
}

static int phase_missing_reject(void) {
    if (mb_consent_gate_allows("hardware", "camera")) {
        fprintf(stderr, "phase2: gate returned true (want false)\n");
        return 21;
    }
    return 0;
}

static int phase_no_handshake(void) {
    // Child did NOT call moonbase_init. mb_conn_is_handshaken is false;
    // the gate must deny without touching IPC.
    if (mb_consent_gate_allows("system", "accessibility")) {
        fprintf(stderr, "phase3: gate returned true (want false)\n");
        return 31;
    }
    return 0;
}

// Child runs `moonbase_init` + one phase + `moonbase_quit`.
static int run_phase_connected(const char *sock_dir, const char *bundle_id,
                               int (*fn)(void)) {
    setenv("XDG_RUNTIME_DIR", sock_dir, 1);
    setenv("MOONBASE_BUNDLE_ID", bundle_id, 1);
    setenv("MOONBASE_BUNDLE_VERSION", "0.1.0", 1);
    if (moonbase_init(0, NULL) != 0) {
        fprintf(stderr, "child moonbase_init failed\n");
        return 4;
    }
    int r = fn();
    moonbase_quit(0);
    return r;
}

// Child for the no-handshake phase — bundle-id is set so the gate's
// query knows where to look, but moonbase_init is *not* called.
static int run_phase_disconnected(const char *bundle_id, int (*fn)(void)) {
    setenv("MOONBASE_BUNDLE_ID", bundle_id, 1);
    setenv("MOONBASE_BUNDLE_VERSION", "0.1.0", 1);
    // Deliberately no moonbase_init — mb_conn_is_handshaken stays false.
    return fn();
}

// ─────────────────────────────────────────────────────────────────────
// Parent: fork once per phase, drive poll loop until child exits.
// ─────────────────────────────────────────────────────────────────────

static void drive_until_reaped(mb_server_t *srv, pid_t pid, const char *tag) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const long timeout_ms = 15000;

    for (;;) {
        struct pollfd fds[32];
        size_t n = mb_server_get_pollfds(srv, fds, 32);
        if (n < 32) {
            n += mb_consent_responder_collect_pollfds(fds + n, 32 - n);
        }
        int pr = poll(fds, (nfds_t)n, 50);
        if (pr < 0 && errno != EINTR) FAIL("poll: %s", strerror(errno));
        mb_server_tick(srv, fds, n);
        mb_consent_responder_tick(srv, fds, n);

        int status = 0;
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                FAIL("%s: child status 0x%x", tag, (unsigned)status);
            }
            return;
        }
        if (w < 0 && errno != ECHILD) {
            FAIL("%s: waitpid: %s", tag, strerror(errno));
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000
                     + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed > timeout_ms) {
            kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
            FAIL("%s: timeout", tag);
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-moonbase-consent>\n", argv[0]);
        return 2;
    }
    const char *consent_bin = argv[1];

    char sock_dir[] = "/tmp/mb-gate-sock.XXXXXX";
    char data_dir[] = "/tmp/mb-gate-data.XXXXXX";
    if (!mkdtemp(sock_dir)) FAIL("mkdtemp sock: %s", strerror(errno));
    if (!mkdtemp(data_dir)) FAIL("mkdtemp data: %s", strerror(errno));

    char sock_path[256];
    snprintf(sock_path, sizeof(sock_path), "%s/moonbase.sock", sock_dir);

    // The responder's fork inherits XDG_DATA_HOME / HOME /
    // MOONBASE_CONSENT_AUTO through the child whitelist.
    setenv("MOONBASE_CONSENT_BIN", consent_bin, 1);
    setenv("XDG_DATA_HOME", data_dir, 1);
    setenv("HOME", data_dir, 1);

    mb_server_t *srv = NULL;
    int rc = mb_server_open(&srv, sock_path, on_event, NULL);
    if (rc != 0) FAIL("mb_server_open rc=%d", rc);

    rc = mb_consent_responder_init(consent_bin);
    if (rc != 0) FAIL("mb_consent_responder_init rc=%d", rc);

    const char *bundle_id = "show.blizzard.gate-ipc-test";

    // Phase 1 — MISSING + handshake + AUTO=approve → gate returns true.
    setenv("MOONBASE_CONSENT_AUTO", "approve", 1);
    pid_t pid = fork();
    if (pid < 0) FAIL("fork: %s", strerror(errno));
    if (pid == 0) {
        _exit(run_phase_connected(sock_dir, bundle_id,
                                  phase_missing_approve));
    }
    drive_until_reaped(srv, pid, "phase1:approve");

    // Phase 2 — MISSING + handshake + AUTO=reject → gate returns false.
    setenv("MOONBASE_CONSENT_AUTO", "reject", 1);
    pid = fork();
    if (pid < 0) FAIL("fork: %s", strerror(errno));
    if (pid == 0) {
        _exit(run_phase_connected(sock_dir, bundle_id,
                                  phase_missing_reject));
    }
    drive_until_reaped(srv, pid, "phase2:reject");

    // Phase 3 — no handshake, gate denies without IPC.
    // AUTO stays set so we'd notice if the gate accidentally reached
    // the responder (it shouldn't — no connection).
    pid = fork();
    if (pid < 0) FAIL("fork: %s", strerror(errno));
    if (pid == 0) {
        _exit(run_phase_disconnected(bundle_id, phase_no_handshake));
    }
    // The parent still drives the poll loop in case a stray CONNECTED
    // event slips through — drive_until_reaped reaps the child either
    // way via WNOHANG.
    drive_until_reaped(srv, pid, "phase3:no-handshake");

    // Verify the writer lined up under the HELLO bundle's store.
    setenv("MOONBASE_BUNDLE_ID", bundle_id, 1);
    if (mb_consent_query("system", "keychain") != MB_CONSENT_ALLOW) {
        FAIL("post: keychain not ALLOW after gate+approve");
    }
    if (mb_consent_query("hardware", "camera") != MB_CONSENT_DENY) {
        FAIL("post: camera not DENY after gate+reject");
    }
    // Phase 3 never hit IPC — accessibility must still be absent.
    if (mb_consent_query("system", "accessibility") != MB_CONSENT_MISSING) {
        FAIL("post: accessibility recorded a decision without handshake");
    }

    mb_consent_responder_shutdown();
    mb_server_close(srv);

    printf("ok: consent-gate-ipc (sock=%s data=%s)\n", sock_dir, data_dir);
    return 0;
}
