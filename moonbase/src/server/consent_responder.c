// CopyCatOS — by Kyle Blizzard at Blizzard.show

// consent_responder.c — compositor-side MB_IPC_CONSENT_REQUEST handler.
//
// Forks `moonbase-consent ask <group> <value> <bundle-id> [context]`
// per in-flight request, uses pidfd_open(2) to poll for exit without
// blocking, and maps the child's exit status to a GRANT / DENY /
// ERROR reply on the same mb_server_t connection.
//
// State is kept in two small static tables — one per known client
// (bundle_id + client_id), one per in-flight request (client_id,
// req_id, window_id, pidfd, pid). The tables are sized well beyond
// any realistic desktop session; linear scans are fine at this scale.
//
// All fork/exec plumbing lives here so moonrock's host adapter only
// has to delegate from its existing on_event switch. The integration
// test (moonbase/tests/consent-responder.c) compiles this same file
// to pin the contract without pulling the X11/GL half of moonrock
// into the test binary.

#include "consent_responder.h"

#include "ipc/cbor.h"
#include "moonbase.h"              // mb_error_t, MB_E*
#include "moonbase/ipc/kinds.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// pidfd_open(2) wrapper. glibc grew a libc entry only in 2.36; going
// through syscall(SYS_pidfd_open, ...) works on every Linux ≥ 5.3
// regardless of libc vintage. The syscall number is defined in
// <sys/syscall.h> on every target we care about.
#ifndef SYS_pidfd_open
#  if defined(__x86_64__)
#    define SYS_pidfd_open 434
#  elif defined(__aarch64__)
#    define SYS_pidfd_open 434
#  endif
#endif

static int call_pidfd_open(pid_t pid, unsigned int flags) {
    return (int)syscall(SYS_pidfd_open, pid, flags);
}

// Compile-time default. Meson injects -DMOONBASE_CONSENT_PATH="..."
// based on libexecdir; an env override wins for tests.
#ifndef MOONBASE_CONSENT_PATH
#define MOONBASE_CONSENT_PATH "/usr/local/libexec/moonbase-consent"
#endif

#define MB_CR_MAX_CLIENTS   256
#define MB_CR_MAX_INFLIGHT  64

typedef struct {
    bool            in_use;
    mb_client_id_t  client;
    char           *bundle_id;   // owned, strdup'd from HELLO; may be empty
} client_slot_t;

typedef struct {
    bool            in_use;
    mb_client_id_t  client;
    uint64_t        req_id;
    uint32_t        window_id;
    int             pidfd;       // -1 when not in use
    pid_t           pid;
} inflight_slot_t;

static bool              g_initialized = false;
static char             *g_consent_bin = NULL;    // owned
static client_slot_t     g_clients[MB_CR_MAX_CLIENTS];
static inflight_slot_t   g_inflight[MB_CR_MAX_INFLIGHT];

// ─────────────────────────────────────────────────────────────────────
// Small helpers
// ─────────────────────────────────────────────────────────────────────

static client_slot_t *client_find(mb_client_id_t c) {
    for (size_t i = 0; i < MB_CR_MAX_CLIENTS; i++) {
        if (g_clients[i].in_use && g_clients[i].client == c) {
            return &g_clients[i];
        }
    }
    return NULL;
}

static client_slot_t *client_alloc(mb_client_id_t c) {
    for (size_t i = 0; i < MB_CR_MAX_CLIENTS; i++) {
        if (!g_clients[i].in_use) {
            g_clients[i].in_use    = true;
            g_clients[i].client    = c;
            g_clients[i].bundle_id = NULL;
            return &g_clients[i];
        }
    }
    return NULL;
}

static void client_release(client_slot_t *slot) {
    if (!slot) return;
    free(slot->bundle_id);
    slot->bundle_id = NULL;
    slot->in_use    = false;
    slot->client    = 0;
}

static inflight_slot_t *inflight_alloc(void) {
    for (size_t i = 0; i < MB_CR_MAX_INFLIGHT; i++) {
        if (!g_inflight[i].in_use) {
            g_inflight[i].in_use   = true;
            g_inflight[i].pidfd    = -1;
            g_inflight[i].pid      = 0;
            return &g_inflight[i];
        }
    }
    return NULL;
}

static void inflight_release(inflight_slot_t *slot) {
    if (!slot) return;
    if (slot->pidfd >= 0) close(slot->pidfd);
    slot->pidfd   = -1;
    slot->pid     = 0;
    slot->in_use  = false;
}

// Build and send an ERROR reply. `code` is a negative mb_error_t.
static void send_error(mb_server_t *s, mb_client_id_t client,
                       int code, const char *msg) {
    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 64);
    mb_cbor_w_map_begin(&w, msg ? 2 : 1);
    mb_cbor_w_key(&w, 1); mb_cbor_w_int(&w, code);
    if (msg) { mb_cbor_w_key(&w, 2); mb_cbor_w_tstr(&w, msg); }
    if (!mb_cbor_w_ok(&w)) { mb_cbor_w_drop(&w); return; }
    size_t len = 0;
    uint8_t *body = mb_cbor_w_finish(&w, &len);
    if (!body) return;
    (void)mb_server_send(s, client, MB_IPC_ERROR, body, len, NULL, 0);
    free(body);
}

// Build and send a CONSENT_GRANT / CONSENT_DENY reply.
static void send_decision(mb_server_t *s, mb_client_id_t client,
                          uint16_t kind, uint64_t req_id, bool remember) {
    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 32);
    mb_cbor_w_map_begin(&w, 2);
    mb_cbor_w_key(&w, 1); mb_cbor_w_uint(&w, req_id);
    mb_cbor_w_key(&w, 2); mb_cbor_w_bool(&w, remember);
    if (!mb_cbor_w_ok(&w)) { mb_cbor_w_drop(&w); return; }
    size_t len = 0;
    uint8_t *body = mb_cbor_w_finish(&w, &len);
    if (!body) return;
    (void)mb_server_send(s, client, kind, body, len, NULL, 0);
    free(body);
}

// ─────────────────────────────────────────────────────────────────────
// Init / shutdown
// ─────────────────────────────────────────────────────────────────────

int mb_consent_responder_init(const char *consent_bin_override) {
    if (g_initialized) return 0;

    const char *pick = consent_bin_override;
    if (!pick || !*pick) {
        const char *env = getenv("MOONBASE_CONSENT_BIN");
        if (env && *env) pick = env;
    }
    if (!pick || !*pick) pick = MOONBASE_CONSENT_PATH;

    g_consent_bin = strdup(pick);
    if (!g_consent_bin) return MB_ENOMEM;

    memset(g_clients,  0, sizeof(g_clients));
    memset(g_inflight, 0, sizeof(g_inflight));
    for (size_t i = 0; i < MB_CR_MAX_INFLIGHT; i++) {
        g_inflight[i].pidfd = -1;
    }

    g_initialized = true;
    return 0;
}

void mb_consent_responder_shutdown(void) {
    if (!g_initialized) return;

    // Reap every in-flight child. SIGTERM first; a short polite wait
    // would be ideal but we're on the shutdown path — the kernel will
    // reap orphans anyway, and closing the pidfd releases our handle.
    for (size_t i = 0; i < MB_CR_MAX_INFLIGHT; i++) {
        inflight_slot_t *f = &g_inflight[i];
        if (!f->in_use) continue;
        if (f->pid > 0) {
            kill(f->pid, SIGTERM);
            // Non-blocking reap — if the child hasn't exited yet, the
            // init process takes it over when we close pidfd below.
            (void)waitpid(f->pid, NULL, WNOHANG);
        }
        inflight_release(f);
    }

    for (size_t i = 0; i < MB_CR_MAX_CLIENTS; i++) {
        if (g_clients[i].in_use) client_release(&g_clients[i]);
    }

    free(g_consent_bin);
    g_consent_bin = NULL;
    g_initialized = false;
}

// ─────────────────────────────────────────────────────────────────────
// Client lifecycle
// ─────────────────────────────────────────────────────────────────────

void mb_consent_responder_note_connected(mb_client_id_t client,
                                         const char *bundle_id) {
    if (!g_initialized) return;
    client_slot_t *slot = client_find(client);
    if (!slot) slot = client_alloc(client);
    if (!slot) {
        fprintf(stderr,
            "[consent-responder] client %u: no free slot\n", client);
        return;
    }
    free(slot->bundle_id);
    slot->bundle_id = strdup(bundle_id ? bundle_id : "");
    // strdup failure → bundle_id NULL — REQUEST handler treats that
    // the same as empty and ERRORs the request.
}

// Kill and reap any in-flight request for `client`. Called from
// mb_consent_responder_note_disconnected and whenever we need to
// clear a hung child on error paths.
static void sweep_inflight_for_client(mb_client_id_t client) {
    for (size_t i = 0; i < MB_CR_MAX_INFLIGHT; i++) {
        inflight_slot_t *f = &g_inflight[i];
        if (!f->in_use || f->client != client) continue;
        if (f->pid > 0) {
            kill(f->pid, SIGTERM);
            (void)waitpid(f->pid, NULL, WNOHANG);
        }
        inflight_release(f);
    }
}

void mb_consent_responder_note_disconnected(mb_client_id_t client) {
    if (!g_initialized) return;
    sweep_inflight_for_client(client);
    client_slot_t *slot = client_find(client);
    if (slot) client_release(slot);
}

// ─────────────────────────────────────────────────────────────────────
// REQUEST decode + child fork/exec
// ─────────────────────────────────────────────────────────────────────

// Decode MB_IPC_CONSENT_REQUEST body into out params. Returns 0 on
// success, negative mb_error_t on failure. Strings are copied out of
// the borrowed buffer into caller-owned storage sized by `cap`/`ctx`
// caps; truncation treats the original string as too-long and fails
// with MB_EPROTO so the child never sees a silently-shortened cap.
#define MB_CR_CAP_MAX 128          // capability string cap
#define MB_CR_CTX_MAX 256          // context string cap

static int decode_request(const uint8_t *body, size_t body_len,
                          uint64_t *out_req_id,
                          char *cap_out,    size_t cap_cap,
                          char *ctx_out,    size_t ctx_cap,
                          uint32_t *out_window_id) {
    mb_cbor_r_t r;
    mb_cbor_r_init(&r, body, body_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs)) return MB_EPROTO;

    *out_req_id    = 0;
    *out_window_id = 0;
    cap_out[0] = '\0';
    ctx_out[0] = '\0';

    for (uint64_t i = 0; i < pairs; i++) {
        uint64_t key = 0;
        if (!mb_cbor_r_uint(&r, &key)) return MB_EPROTO;
        switch (key) {
            case 1: {
                uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return MB_EPROTO;
                *out_req_id = v;
                break;
            }
            case 2: {
                const char *s = NULL;
                size_t sl = 0;
                if (!mb_cbor_r_tstr(&r, &s, &sl)) return MB_EPROTO;
                if (sl + 1 > cap_cap) return MB_EPROTO;
                memcpy(cap_out, s, sl);
                cap_out[sl] = '\0';
                break;
            }
            case 3: {
                const char *s = NULL;
                size_t sl = 0;
                if (!mb_cbor_r_tstr(&r, &s, &sl)) return MB_EPROTO;
                if (sl + 1 > ctx_cap) {
                    // Context is advisory — truncating is safer than
                    // rejecting the whole request. Keep the first
                    // ctx_cap-1 bytes.
                    sl = ctx_cap - 1;
                }
                memcpy(ctx_out, s, sl);
                ctx_out[sl] = '\0';
                break;
            }
            case 4: {
                uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return MB_EPROTO;
                *out_window_id = (uint32_t)v;
                break;
            }
            default:
                if (!mb_cbor_r_skip(&r)) return MB_EPROTO;
                break;
        }
    }
    return 0;
}

// Split "group.value" in place — overwrites the '.' with '\0' and
// returns pointers into the same buffer. Returns 0 on success or
// MB_EINVAL if the string lacks a dot or either half is empty.
static int split_capability(char *cap,
                            const char **out_group,
                            const char **out_value) {
    if (!cap || !*cap) return MB_EINVAL;
    char *dot = strchr(cap, '.');
    if (!dot || dot == cap || dot[1] == '\0') return MB_EINVAL;
    // Reject additional dots — capabilities are exactly one "group.value"
    // pair per IPC.md §5.4. Unknown future syntax (nested, escaped)
    // would be silently miscategorized otherwise.
    if (strchr(dot + 1, '.')) return MB_EINVAL;
    *dot = '\0';
    *out_group = cap;
    *out_value = dot + 1;
    return 0;
}

// Spawn moonbase-consent. Returns the pidfd on success (child is
// running, pid written to *out_pid), or -1 with errno set on failure.
static int spawn_consent(const char *group, const char *value,
                         const char *bundle_id, const char *context,
                         pid_t *out_pid) {
    // Prepare argv in the parent; execv reads it after fork.
    // moonbase-consent's arg check is argc == 5 (no context) or 6
    // (with context). Pass empty-string context as "no context arg"
    // — the ask verb takes up to argv[5] and treats absence as empty.
    char *argv[7];
    argv[0] = g_consent_bin;
    argv[1] = (char *)"ask";
    argv[2] = (char *)group;
    argv[3] = (char *)value;
    argv[4] = (char *)bundle_id;
    size_t ac;
    if (context && *context) {
        argv[5] = (char *)context;
        argv[6] = NULL;
        ac = 6;
    } else {
        argv[5] = NULL;
        ac = 5;
    }
    (void)ac;

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        // Child: explicit env whitelist. Every var the user might
        // need for the consent UI (locale, HOME for fallback storage,
        // XDG_DATA_HOME for store location, XDG_RUNTIME_DIR for
        // runtime sockets, PATH for any helper exec, and the AUTO
        // override for headless/test use) is forwarded by name;
        // everything else (including client-influenced vars, if any)
        // is discarded. MOONBASE_BUNDLE_ID is set from the trusted
        // server-side HELLO value — never from the frame body.
        static const char *const passthrough_keys[] = {
            "XDG_RUNTIME_DIR",
            "XDG_DATA_HOME",
            "HOME",
            "LANG",
            "LC_ALL",
            "PATH",
            "MOONBASE_CONSENT_AUTO",
            NULL,
        };
        // saved[] copies are intentionally not freed. We're in the
        // forked child and the next line is execv; the strdup'd memory
        // dies with the address space a few microseconds later. Any
        // early-return path below also calls _exit, not return.
        char *saved[16] = {0};
        for (size_t i = 0; passthrough_keys[i]; i++) {
            const char *v = getenv(passthrough_keys[i]);
            if (v) saved[i] = strdup(v);
        }
        clearenv();
        for (size_t i = 0; passthrough_keys[i]; i++) {
            if (saved[i]) setenv(passthrough_keys[i], saved[i], 1);
        }
        setenv("MOONBASE_BUNDLE_ID", bundle_id, 1);

        execv(g_consent_bin, argv);
        _exit(127);
    }

    int pidfd = call_pidfd_open(pid, 0);
    if (pidfd < 0) {
        int saved_errno = errno;
        // pidfd_open failed — we have no way to poll for exit, so
        // the safest move is to kill and reap the child now and let
        // the caller send ERROR. This also catches pre-5.3 kernels.
        kill(pid, SIGTERM);
        (void)waitpid(pid, NULL, 0);
        errno = saved_errno;
        return -1;
    }

    *out_pid = pid;
    return pidfd;
}

void mb_consent_responder_handle_request(mb_server_t *s,
                                         mb_client_id_t client,
                                         const uint8_t *body,
                                         size_t body_len) {
    if (!g_initialized) {
        send_error(s, client, MB_EIPC, "consent responder not initialized");
        return;
    }

    uint64_t req_id = 0;
    uint32_t window_id = 0;
    char cap[MB_CR_CAP_MAX];
    char ctx[MB_CR_CTX_MAX];
    int rc = decode_request(body, body_len,
                            &req_id, cap, sizeof(cap),
                            ctx, sizeof(ctx), &window_id);
    if (rc != 0) {
        send_error(s, client, rc, "malformed CONSENT_REQUEST");
        return;
    }

    client_slot_t *cslot = client_find(client);
    if (!cslot || !cslot->bundle_id || !*cslot->bundle_id) {
        send_error(s, client, MB_EPERM, "no bundle id recorded for client");
        return;
    }

    const char *group = NULL;
    const char *value = NULL;
    if (split_capability(cap, &group, &value) != 0) {
        send_error(s, client, MB_EINVAL, "capability must be \"group.value\"");
        return;
    }

    inflight_slot_t *f = inflight_alloc();
    if (!f) {
        send_error(s, client, MB_ENOMEM, "in-flight consent table full");
        return;
    }

    pid_t pid = 0;
    int pidfd = spawn_consent(group, value, cslot->bundle_id,
                              ctx, &pid);
    if (pidfd < 0) {
        fprintf(stderr,
            "[consent-responder] spawn failed for %s [%s.%s]: %s\n",
            cslot->bundle_id, group, value, strerror(errno));
        inflight_release(f);
        send_error(s, client, MB_EIPC, "failed to spawn consent helper");
        return;
    }

    f->client    = client;
    f->req_id    = req_id;
    f->window_id = window_id;
    f->pid       = pid;
    f->pidfd     = pidfd;

    fprintf(stderr,
        "[consent-responder] client %u req_id=%llu %s [%s.%s] "
        "pid=%d pidfd=%d\n",
        client, (unsigned long long)req_id,
        cslot->bundle_id, group, value, (int)pid, pidfd);
}

// ─────────────────────────────────────────────────────────────────────
// Poll integration
// ─────────────────────────────────────────────────────────────────────

size_t mb_consent_responder_collect_pollfds(struct pollfd *out_fds,
                                            size_t max) {
    if (!g_initialized || !out_fds || max == 0) return 0;
    size_t n = 0;
    for (size_t i = 0; i < MB_CR_MAX_INFLIGHT && n < max; i++) {
        if (!g_inflight[i].in_use || g_inflight[i].pidfd < 0) continue;
        out_fds[n].fd      = g_inflight[i].pidfd;
        out_fds[n].events  = POLLIN;
        out_fds[n].revents = 0;
        n++;
    }
    return n;
}

// Convert a waitpid() status to a reply kind. Returns MB_IPC_CONSENT_GRANT,
// MB_IPC_CONSENT_DENY, or MB_IPC_ERROR. For ERROR we also fill *out_code
// with the mb_error_t to report.
static uint16_t classify_exit(int status, int *out_code, const char **out_msg) {
    *out_code = MB_EIPC;
    *out_msg  = "consent helper failed";
    if (WIFEXITED(status)) {
        int ec = WEXITSTATUS(status);
        switch (ec) {
            case 0: return MB_IPC_CONSENT_GRANT;
            case 1: return MB_IPC_CONSENT_DENY;
            case 2:
                // argv shape was wrong — a bug in the responder's exec
                // plumbing, not an IPC failure. MB_EINVAL is the code
                // for "bad arguments" and surfaces the real category
                // when this shows up in a client log.
                *out_code = MB_EINVAL;
                *out_msg  = "consent helper usage error";
                return MB_IPC_ERROR;
            case 3:
                // Writer failure — disk full, read-only store, etc.
                // Not a protocol bug; stays MB_EIPC so the caller
                // treats it as "compositor couldn't answer this time".
                *out_msg  = "consent helper writer failed";
                return MB_IPC_ERROR;
            default:
                *out_msg  = "consent helper unknown exit";
                return MB_IPC_ERROR;
        }
    }
    if (WIFSIGNALED(status)) {
        *out_msg = "consent helper killed by signal";
    }
    return MB_IPC_ERROR;
}

void mb_consent_responder_tick(mb_server_t *s,
                               const struct pollfd *fds, size_t nfds) {
    if (!g_initialized) return;

    for (size_t i = 0; i < nfds; i++) {
        if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        int fd = fds[i].fd;

        // Find the in-flight slot that owns this pidfd.
        inflight_slot_t *f = NULL;
        for (size_t j = 0; j < MB_CR_MAX_INFLIGHT; j++) {
            if (g_inflight[j].in_use && g_inflight[j].pidfd == fd) {
                f = &g_inflight[j];
                break;
            }
        }
        if (!f) continue;

        int status = 0;
        pid_t w = waitpid(f->pid, &status, WNOHANG);
        if (w == 0) {
            // Child hasn't actually exited yet — unlikely given the
            // pidfd signal, but tolerate it (next tick will catch).
            continue;
        }
        if (w < 0) {
            // waitpid failed (ECHILD most likely — child was already
            // reaped somewhere). Fall through with status=0 which
            // classifies as GRANT; instead, send an ERROR and move on.
            fprintf(stderr,
                "[consent-responder] waitpid(%d) failed: %s\n",
                (int)f->pid, strerror(errno));
            send_error(s, f->client, MB_EIPC, "consent helper reap failed");
            inflight_release(f);
            continue;
        }

        int         err_code = MB_EIPC;
        const char *err_msg  = NULL;
        uint16_t    kind     = classify_exit(status, &err_code, &err_msg);

        if (kind == MB_IPC_ERROR) {
            fprintf(stderr,
                "[consent-responder] client %u req_id=%llu: %s "
                "(status=0x%x)\n",
                f->client, (unsigned long long)f->req_id,
                err_msg ? err_msg : "error",
                (unsigned)status);
            send_error(s, f->client, err_code, err_msg);
        } else {
            send_decision(s, f->client, kind, f->req_id, /*remember=*/true);
        }

        inflight_release(f);
    }
}
