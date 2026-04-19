// CopyCatOS — by Kyle Blizzard at Blizzard.show

// consent-wire.c — Phase D lazy-consent client IPC test.
//
// Pins mb_ipc_consent_request end-to-end against the real mb_server_t.
// The parent stands up a server that, on each MB_IPC_CONSENT_REQUEST,
// replies according to which phase's capability string it sees:
//
//   phase 1 "system.keychain"        -> straight CONSENT_GRANT
//   phase 2 "hardware.camera"        -> straight CONSENT_DENY
//   phase 3 "system.accessibility"   -> POWER_CHANGED (unrelated)
//                                       then CONSENT_GRANT — the helper
//                                       must queue the unrelated frame
//                                       without eating it, and return
//                                       allow=true.
//   phase 4 "system.location"        -> CONSENT_GRANT with req_id+1000
//                                       then CONSENT_GRANT with the
//                                       real req_id — the helper must
//                                       queue the mismatched reply and
//                                       return on the correct one.
//
// Each phase after the helper returns drains mb_conn_pop_queued and
// asserts the expected frame (if any) is parked on the pending-frame
// queue. The test fails (nonzero exit from the child) if any phase's
// allow / remember / queue state doesn't match the per-phase expectation.

#include "moonbase.h"
#include "../src/ipc/consent.h"
#include "../src/ipc/transport.h"
#include "../src/ipc/cbor.h"
#include "../src/server/server.h"
#include "moonbase/ipc/kinds.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FAIL(...) do { \
    fprintf(stderr, "FAIL: " __VA_ARGS__); \
    fputc('\n', stderr); \
    exit(1); \
} while (0)

// Counters shared with the parent-side server callback.
static int g_connected            = 0;
static int g_disconnected         = 0;
static int g_consent_request_seen = 0;

// Decode the req_id + capability out of a CONSENT_REQUEST body. The
// string is copied into `cap_out` (bounded) so the caller can match it
// without holding the frame's borrowed buffer live.
static int decode_request(const uint8_t *body, size_t body_len,
                          uint64_t *out_req_id,
                          char *cap_out, size_t cap_cap) {
    mb_cbor_r_t r;
    mb_cbor_r_init(&r, body, body_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs)) return -1;

    *out_req_id = 0;
    cap_out[0] = '\0';

    for (uint64_t i = 0; i < pairs; i++) {
        uint64_t key = 0;
        if (!mb_cbor_r_uint(&r, &key)) return -1;
        switch (key) {
            case 1: {
                uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return -1;
                *out_req_id = v;
                break;
            }
            case 2: {
                const char *s = NULL;
                size_t sl = 0;
                if (!mb_cbor_r_tstr(&r, &s, &sl)) return -1;
                size_t n = sl < cap_cap - 1 ? sl : cap_cap - 1;
                memcpy(cap_out, s, n);
                cap_out[n] = '\0';
                break;
            }
            default:
                if (!mb_cbor_r_skip(&r)) return -1;
                break;
        }
    }
    return 0;
}

// Build a CONSENT_GRANT / CONSENT_DENY body. remember may be false —
// the helper must treat absence / false identically.
static uint8_t *build_reply_body(uint64_t req_id, bool remember,
                                 size_t *out_len) {
    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 32);
    mb_cbor_w_map_begin(&w, 2);
    mb_cbor_w_key(&w, 1); mb_cbor_w_uint(&w, req_id);
    mb_cbor_w_key(&w, 2); mb_cbor_w_bool(&w, remember);
    if (!mb_cbor_w_ok(&w)) { mb_cbor_w_drop(&w); return NULL; }
    return mb_cbor_w_finish(&w, out_len);
}

// A minimally-valid POWER_CHANGED body. The helper doesn't decode it —
// it only cares that the frame is stored intact and that pop_queued
// returns the same kind + length. But a reader somewhere downstream
// will; keep the body well-formed.
static uint8_t *build_power_body(size_t *out_len) {
    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 16);
    mb_cbor_w_map_begin(&w, 1);
    mb_cbor_w_key(&w, 1); mb_cbor_w_uint(&w, 2);   // on_ac (arbitrary)
    if (!mb_cbor_w_ok(&w)) { mb_cbor_w_drop(&w); return NULL; }
    return mb_cbor_w_finish(&w, out_len);
}

static void send_grant(mb_server_t *s, mb_client_id_t client,
                       uint64_t req_id, bool remember) {
    size_t len = 0;
    uint8_t *body = build_reply_body(req_id, remember, &len);
    if (!body) FAIL("build_reply_body OOM");
    int rc = mb_server_send(s, client, MB_IPC_CONSENT_GRANT, body, len,
                            NULL, 0);
    free(body);
    if (rc != 0) FAIL("mb_server_send GRANT rc=%d", rc);
}

static void send_deny(mb_server_t *s, mb_client_id_t client,
                      uint64_t req_id, bool remember) {
    size_t len = 0;
    uint8_t *body = build_reply_body(req_id, remember, &len);
    if (!body) FAIL("build_reply_body OOM");
    int rc = mb_server_send(s, client, MB_IPC_CONSENT_DENY, body, len,
                            NULL, 0);
    free(body);
    if (rc != 0) FAIL("mb_server_send DENY rc=%d", rc);
}

static void send_power_changed(mb_server_t *s, mb_client_id_t client) {
    size_t len = 0;
    uint8_t *body = build_power_body(&len);
    if (!body) FAIL("build_power_body OOM");
    int rc = mb_server_send(s, client, MB_IPC_POWER_CHANGED, body, len,
                            NULL, 0);
    free(body);
    if (rc != 0) FAIL("mb_server_send POWER rc=%d", rc);
}

static void on_event(mb_server_t *s, const mb_server_event_t *ev, void *user) {
    (void)user;
    switch (ev->kind) {
        case MB_SERVER_EV_CONNECTED:
            g_connected++;
            break;
        case MB_SERVER_EV_FRAME: {
            if (ev->frame_kind != MB_IPC_CONSENT_REQUEST) {
                fprintf(stderr,
                        "[test] unexpected frame 0x%04x (len=%zu)\n",
                        ev->frame_kind, ev->frame_body_len);
                break;
            }
            g_consent_request_seen++;

            uint64_t req_id = 0;
            char     cap[64] = {0};
            if (decode_request(ev->frame_body, ev->frame_body_len,
                               &req_id, cap, sizeof(cap)) != 0) {
                FAIL("decode_request: malformed CONSENT_REQUEST");
            }
            fprintf(stderr,
                    "[test] CONSENT_REQUEST req_id=%llu cap='%s'\n",
                    (unsigned long long)req_id, cap);

            if (strcmp(cap, "system.keychain") == 0) {
                send_grant(s, ev->client, req_id, /*remember=*/false);
            } else if (strcmp(cap, "hardware.camera") == 0) {
                send_deny(s, ev->client, req_id, /*remember=*/true);
            } else if (strcmp(cap, "system.accessibility") == 0) {
                // Unrelated frame comes first — helper must queue it.
                send_power_changed(s, ev->client);
                send_grant(s, ev->client, req_id, /*remember=*/false);
            } else if (strcmp(cap, "system.location") == 0) {
                // Wrong req_id first — helper must queue + keep waiting.
                send_grant(s, ev->client, req_id + 1000,
                           /*remember=*/false);
                send_grant(s, ev->client, req_id, /*remember=*/true);
            } else {
                FAIL("unknown capability in CONSENT_REQUEST: '%s'", cap);
            }
            break;
        }
        case MB_SERVER_EV_DISCONNECTED:
            g_disconnected++;
            break;
    }
}

static int phase_grant(void) {
    bool allow = false, remember = true;
    int rc = mb_ipc_consent_request("system.keychain",
                                    "phase1: grant", 0,
                                    &allow, &remember);
    if (rc != 0)     { fprintf(stderr, "phase1 rc=%d\n", rc); return 10; }
    if (!allow)      { fprintf(stderr, "phase1 allow=false\n"); return 11; }
    if (remember)    { fprintf(stderr, "phase1 remember=true\n"); return 12; }
    // Nothing should be on the queue.
    uint16_t k = 0; uint8_t *b = NULL; size_t bl = 0;
    if (mb_conn_pop_queued(&k, &b, &bl) != 0) {
        fprintf(stderr, "phase1 queue non-empty: kind=0x%04x\n", k);
        free(b);
        return 13;
    }
    return 0;
}

static int phase_deny(void) {
    bool allow = true, remember = false;
    int rc = mb_ipc_consent_request("hardware.camera",
                                    "phase2: deny", 0,
                                    &allow, &remember);
    if (rc != 0)     { fprintf(stderr, "phase2 rc=%d\n", rc); return 20; }
    if (allow)       { fprintf(stderr, "phase2 allow=true\n"); return 21; }
    if (!remember)   { fprintf(stderr, "phase2 remember=false\n"); return 22; }
    uint16_t k = 0; uint8_t *b = NULL; size_t bl = 0;
    if (mb_conn_pop_queued(&k, &b, &bl) != 0) {
        fprintf(stderr, "phase2 queue non-empty: kind=0x%04x\n", k);
        free(b);
        return 23;
    }
    return 0;
}

static int phase_unrelated_queued(void) {
    bool allow = false, remember = true;
    int rc = mb_ipc_consent_request("system.accessibility",
                                    "phase3: unrelated frame parked", 0,
                                    &allow, &remember);
    if (rc != 0)   { fprintf(stderr, "phase3 rc=%d\n", rc); return 30; }
    if (!allow)    { fprintf(stderr, "phase3 allow=false\n"); return 31; }
    if (remember)  { fprintf(stderr, "phase3 remember=true\n"); return 32; }

    // The unrelated POWER_CHANGED frame should be parked.
    uint16_t k = 0; uint8_t *b = NULL; size_t bl = 0;
    int pop = mb_conn_pop_queued(&k, &b, &bl);
    if (pop != 1)                        { fprintf(stderr, "phase3 queue empty\n"); return 33; }
    if (k != MB_IPC_POWER_CHANGED)       { fprintf(stderr, "phase3 kind=0x%04x\n", k); free(b); return 34; }
    free(b);

    // Queue should now be drained.
    if (mb_conn_pop_queued(&k, &b, &bl) != 0) {
        fprintf(stderr, "phase3 queue still has extra: kind=0x%04x\n", k);
        free(b);
        return 35;
    }
    return 0;
}

static int phase_wrong_req_id_queued(void) {
    bool allow = false, remember = false;
    int rc = mb_ipc_consent_request("system.location",
                                    "phase4: stale req_id parked", 0,
                                    &allow, &remember);
    if (rc != 0)   { fprintf(stderr, "phase4 rc=%d\n", rc); return 40; }
    if (!allow)    { fprintf(stderr, "phase4 allow=false\n"); return 41; }
    if (!remember) { fprintf(stderr, "phase4 remember=false\n"); return 42; }

    // The mismatched CONSENT_GRANT (wrong req_id) should be parked.
    uint16_t k = 0; uint8_t *b = NULL; size_t bl = 0;
    int pop = mb_conn_pop_queued(&k, &b, &bl);
    if (pop != 1)                        { fprintf(stderr, "phase4 queue empty\n"); return 43; }
    if (k != MB_IPC_CONSENT_GRANT)       { fprintf(stderr, "phase4 kind=0x%04x\n", k); free(b); return 44; }
    free(b);

    if (mb_conn_pop_queued(&k, &b, &bl) != 0) {
        fprintf(stderr, "phase4 queue still has extra: kind=0x%04x\n", k);
        free(b);
        return 45;
    }
    return 0;
}

static int run_client(const char *sock_dir) {
    if (setenv("XDG_RUNTIME_DIR", sock_dir, 1) != 0) return 1;
    if (setenv("MOONBASE_BUNDLE_ID", "show.blizzard.cwtest", 1) != 0) return 2;
    if (setenv("MOONBASE_BUNDLE_VERSION", "0.1.0", 1) != 0) return 3;

    int rc = moonbase_init(0, NULL);
    if (rc != 0) { fprintf(stderr, "child init rc=%d\n", rc); return 4; }

    int ph;
    if ((ph = phase_grant()))              { moonbase_quit(0); return ph; }
    if ((ph = phase_deny()))               { moonbase_quit(0); return ph; }
    if ((ph = phase_unrelated_queued()))   { moonbase_quit(0); return ph; }
    if ((ph = phase_wrong_req_id_queued())){ moonbase_quit(0); return ph; }

    moonbase_quit(0);
    return 0;
}

int main(void) {
    char dir_tmpl[] = "/tmp/mb-cw.XXXXXX";
    char *dir = mkdtemp(dir_tmpl);
    if (!dir) FAIL("mkdtemp: %s", strerror(errno));

    char sock_path[256];
    snprintf(sock_path, sizeof(sock_path), "%s/moonbase.sock", dir);

    mb_server_t *srv = NULL;
    int rc = mb_server_open(&srv, sock_path, on_event, NULL);
    if (rc != 0) FAIL("mb_server_open rc=%d", rc);

    pid_t pid = fork();
    if (pid < 0) FAIL("fork: %s", strerror(errno));
    if (pid == 0) {
        _exit(run_client(dir));
    }

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const long timeout_ms = 5000;

    while (g_connected < 1 || g_consent_request_seen < 4
        || g_disconnected < 1) {
        struct pollfd fds[16];
        size_t n = mb_server_get_pollfds(srv, fds, 16);
        int pr = poll(fds, (nfds_t)n, 50);
        if (pr < 0 && errno != EINTR) FAIL("poll: %s", strerror(errno));
        mb_server_tick(srv, fds, n);

        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000
                     + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed > timeout_ms) {
            FAIL("timeout: connected=%d requests=%d disconnected=%d",
                 g_connected, g_consent_request_seen, g_disconnected);
        }
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) FAIL("waitpid: %s", strerror(errno));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        FAIL("child exited with status %d (exit=%d)",
             status, WEXITSTATUS(status));
    }

    mb_server_close(srv);
    rmdir(dir);

    printf("ok: consent-wire (%d requests served)\n",
           g_consent_request_seen);
    return 0;
}
