// CopyCatOS — by Kyle Blizzard at Blizzard.show

// consent-responder.c — Phase D slice 6B.2 end-to-end responder test.
//
// Pins the compositor-side half of the lazy-consent IPC leg. The
// parent stands up mb_server_t + mb_consent_responder pointing at
// the real moonbase-consent binary, then forks the client once per
// phase:
//
//   phase 1 "system.keychain" / AUTO=approve
//           → spawn ask, child exits 0, reply CONSENT_GRANT, client
//             returns rc=0 + allow=true. Writer lands ALLOW.
//   phase 2 "hardware.camera" / AUTO=reject
//           → child exits 1, reply CONSENT_DENY, client returns rc=0
//             + allow=false. Writer lands DENY.
//   phase 3 "system.accessibility" / AUTO=approve, client also flips
//           MOONBASE_BUNDLE_ID in its env before calling the helper.
//           → responder must use the HELLO-recorded bundle, not
//             anything from the client's env or frame. Writer lands
//             under the HELLO bundle-id's data dir.
//   phase 4 malformed capability "no-dot" / AUTO=approve
//           → responder rejects with ERROR (MB_EINVAL) before
//             spawning. Client's mb_ipc_consent_request returns a
//             negative mb_error_t.
//
// After all four phases run, the parent reads consents.toml back
// through mb_consent_query using the HELLO bundle-id and asserts
// phase 3 landed under the right store (not the faked one).
//
// Single sock tmpdir, single data tmpdir. AUTO is swapped between
// phases via setenv in the parent — the responder's env-whitelist
// fork forwards MOONBASE_CONSENT_AUTO to each spawn.

#include "CopyCatAppKit.h"
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
// DISCONNECTED by teaching the responder about per-client bundle_id.
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

static int phase_approve(void) {
    bool allow = false, remember = false;
    int rc = mb_ipc_consent_request("system.keychain",
                                    "phase1: approve", 0,
                                    &allow, &remember);
    if (rc != 0)   { fprintf(stderr, "phase1 rc=%d\n", rc); return 10; }
    if (!allow)    { fprintf(stderr, "phase1 allow=false\n"); return 11; }
    if (!remember) { fprintf(stderr, "phase1 remember=false\n"); return 12; }
    return 0;
}

static int phase_reject(void) {
    bool allow = true, remember = false;
    int rc = mb_ipc_consent_request("hardware.camera",
                                    "phase2: reject", 0,
                                    &allow, &remember);
    if (rc != 0)   { fprintf(stderr, "phase2 rc=%d\n", rc); return 20; }
    if (allow)     { fprintf(stderr, "phase2 allow=true\n"); return 21; }
    if (!remember) { fprintf(stderr, "phase2 remember=false\n"); return 22; }
    return 0;
}

static int phase_trust(void) {
    // Client flips its local env after handshake — responder must
    // not consult it. A process boundary already makes this true at
    // the OS level; this phase pins the test against future bugs
    // that might try to forward frame-side bundle_id.
    setenv("MOONBASE_BUNDLE_ID", "show.blizzard.NOT-the-real-bundle", 1);
    bool allow = false, remember = false;
    int rc = mb_ipc_consent_request("system.accessibility",
                                    "phase3: trust hello", 0,
                                    &allow, &remember);
    if (rc != 0) { fprintf(stderr, "phase3 rc=%d\n", rc); return 30; }
    if (!allow)  { fprintf(stderr, "phase3 allow=false\n"); return 31; }
    return 0;
}

static int phase_malformed(void) {
    bool allow = true, remember = true;
    int rc = mb_ipc_consent_request("no-dot-here",
                                    "phase4: malformed", 0,
                                    &allow, &remember);
    if (rc == 0) { fprintf(stderr, "phase4 rc=0 (want error)\n"); return 40; }
    if (rc >= 0) { fprintf(stderr, "phase4 rc=%d (want <0)\n", rc); return 41; }
    return 0;
}

// Child runs `moonbase_init` + one phase + `moonbase_quit`.
static int run_phase_in_child(const char *sock_dir, const char *bundle_id,
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

    char sock_dir[] = "/tmp/mb-cr-sock.XXXXXX";
    char data_dir[] = "/tmp/mb-cr-data.XXXXXX";
    if (!mkdtemp(sock_dir)) FAIL("mkdtemp sock: %s", strerror(errno));
    if (!mkdtemp(data_dir)) FAIL("mkdtemp data: %s", strerror(errno));

    char sock_path[256];
    snprintf(sock_path, sizeof(sock_path), "%s/moonbase.sock", sock_dir);

    // Parent-side env — the responder's fork inherits these via the
    // child whitelist (XDG_DATA_HOME, HOME, MOONBASE_CONSENT_AUTO).
    setenv("MOONBASE_CONSENT_BIN", consent_bin, 1);
    setenv("XDG_DATA_HOME", data_dir, 1);
    setenv("HOME", data_dir, 1);

    mb_server_t *srv = NULL;
    int rc = mb_server_open(&srv, sock_path, on_event, NULL);
    if (rc != 0) FAIL("mb_server_open rc=%d", rc);

    rc = mb_consent_responder_init(consent_bin);
    if (rc != 0) FAIL("mb_consent_responder_init rc=%d", rc);

    const char *bundle_id = "show.blizzard.responder-test";

    struct phase {
        const char *tag;
        const char *auto_mode;
        int (*fn)(void);
    };
    struct phase phases[] = {
        { "phase1:approve",   "approve", phase_approve   },
        { "phase2:reject",    "reject",  phase_reject    },
        { "phase3:trust",     "approve", phase_trust     },
        { "phase4:malformed", "approve", phase_malformed },
    };

    for (size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); i++) {
        setenv("MOONBASE_CONSENT_AUTO", phases[i].auto_mode, 1);
        pid_t pid = fork();
        if (pid < 0) FAIL("fork: %s", strerror(errno));
        if (pid == 0) {
            _exit(run_phase_in_child(sock_dir, bundle_id, phases[i].fn));
        }
        drive_until_reaped(srv, pid, phases[i].tag);
    }

    // Verify the writer lined up under the HELLO bundle's store.
    // mb_consent_query reads XDG_DATA_HOME + MOONBASE_BUNDLE_ID.
    setenv("MOONBASE_BUNDLE_ID", bundle_id, 1);
    if (mb_consent_query("system", "keychain") != MB_CONSENT_ALLOW) {
        FAIL("post: keychain not ALLOW");
    }
    if (mb_consent_query("hardware", "camera") != MB_CONSENT_DENY) {
        FAIL("post: camera not DENY");
    }
    if (mb_consent_query("system", "accessibility") != MB_CONSENT_ALLOW) {
        FAIL("post: accessibility not ALLOW");
    }

    // The fake bundle-id the client set in phase 3 must never have
    // gotten a consents.toml written under it — responder ignored it.
    setenv("MOONBASE_BUNDLE_ID", "show.blizzard.NOT-the-real-bundle", 1);
    if (mb_consent_query("system", "accessibility") != MB_CONSENT_MISSING) {
        FAIL("post: fake bundle received a write");
    }

    mb_consent_responder_shutdown();
    mb_server_close(srv);

    printf("ok: consent-responder (sock=%s data=%s)\n", sock_dir, data_dir);
    return 0;
}
