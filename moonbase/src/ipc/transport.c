// CopyCatOS — by Kyle Blizzard at Blizzard.show

// transport.c — per-process MoonBase client connection state.
//
// Owns the singleton socket to MoonRock (IPC.md §1). One connection
// per app process: opened by moonbase_init, torn down by
// moonbase_quit. Everything above (stubs filling in during Phase C)
// routes frames through mb_conn_send / mb_conn_recv.

#include "transport.h"
#include "frame.h"
#include "cbor.h"
#include "debug.h"
#include "../internal.h"
#include "moonbase.h"
#include "moonbase/ipc/kinds.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

// ---------------------------------------------------------------------
// Singleton connection state
// ---------------------------------------------------------------------
//
// All fields are zero-initialized. `fd == -1` is the canonical
// "not connected" marker — 0 is a legitimate fd number so we cannot
// rely on the zeroed struct to mean "closed".

typedef struct queued_frame {
    uint16_t              kind;
    uint8_t              *body;     // malloc'd; NULL when body_len == 0
    size_t                body_len;
    struct queued_frame  *next;
} queued_frame_t;

typedef struct {
    int       fd;
    bool      connected;
    bool      handshaken;
    uint32_t  peer_api_version;
    uint32_t  max_frame_len;
    uint32_t  session_type;
    char    **capabilities;    // NULL-terminated
    size_t    capability_count;

    // Pending-frame FIFO. Populated by mb_conn_request when it reads a
    // frame whose kind doesn't match the one it's waiting for.
    // Drained by the event loop (mb_conn_pop_queued) on poll/wait.
    queued_frame_t *queue_head;
    queued_frame_t *queue_tail;
} mb_conn_t;

static mb_conn_t g_conn = { .fd = -1 };

// Push-back onto the FIFO. Takes ownership of `body`.
static int conn_queue_push(uint16_t kind, uint8_t *body, size_t body_len) {
    queued_frame_t *n = malloc(sizeof(*n));
    if (!n) return MB_ENOMEM;
    n->kind = kind;
    n->body = body;
    n->body_len = body_len;
    n->next = NULL;
    if (g_conn.queue_tail) g_conn.queue_tail->next = n;
    else                   g_conn.queue_head = n;
    g_conn.queue_tail = n;
    return 0;
}

// Drop everything in the queue. Used on teardown.
static void conn_queue_drop(void) {
    queued_frame_t *n = g_conn.queue_head;
    while (n) {
        queued_frame_t *next = n->next;
        free(n->body);
        free(n);
        n = next;
    }
    g_conn.queue_head = NULL;
    g_conn.queue_tail = NULL;
}

int mb_conn_pop_queued(uint16_t *out_kind,
                       uint8_t **out_body, size_t *out_body_len) {
    queued_frame_t *n = g_conn.queue_head;
    if (!n) return 0;
    g_conn.queue_head = n->next;
    if (!g_conn.queue_head) g_conn.queue_tail = NULL;
    if (out_kind)     *out_kind     = n->kind;
    if (out_body)     *out_body     = n->body;     // transfer ownership
    if (out_body_len) *out_body_len = n->body_len;
    free(n);
    return 1;
}

static void conn_reset(void) {
    if (g_conn.capabilities) {
        for (size_t i = 0; i < g_conn.capability_count; i++) {
            free(g_conn.capabilities[i]);
        }
        free(g_conn.capabilities);
    }
    conn_queue_drop();
    g_conn.fd = -1;
    g_conn.connected = false;
    g_conn.handshaken = false;
    g_conn.peer_api_version = 0;
    g_conn.max_frame_len = 0;
    g_conn.session_type = 0;
    g_conn.capabilities = NULL;
    g_conn.capability_count = 0;
}

// ---------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------

int mb_conn_open(const char *socket_path) {
    if (g_conn.connected) {
        return MB_EINVAL;
    }

    // Resolve the default path when the caller passes NULL.
    char   default_path[512];
    const char *path = socket_path;
    if (!path) {
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        if (!xdg || !*xdg) {
            return MB_EINVAL;
        }
        int n = snprintf(default_path, sizeof(default_path),
                         "%s/moonbase.sock", xdg);
        if (n < 0 || (size_t)n >= sizeof(default_path)) {
            return MB_EINVAL;
        }
        path = default_path;
    }

    int fd = mb_ipc_frame_connect(path);
    if (fd < 0) {
        return fd;
    }

    g_conn.fd = fd;
    g_conn.connected = true;
    return 0;
}

void mb_conn_close(void) {
    if (!g_conn.connected) {
        return;
    }

    // Try to send a BYE. If the peer is gone the send fails
    // silently — we're closing anyway.
    if (g_conn.handshaken) {
        (void)mb_conn_send(MB_IPC_BYE, NULL, 0, NULL, 0);
    }

    close(g_conn.fd);
    conn_reset();
}

// ---------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------
//
// HELLO body (IPC.md §5.1):
//   map(6) { 1: api_version, 2: bundle_id, 3: bundle_version,
//            4: pid, 5: entitlements [], 6: language }
//
// Phase C slice 1 sends an empty entitlements array. Later phases
// will read declared entitlements from Info.appc.

int mb_conn_handshake(const char *bundle_id,
                      const char *bundle_version,
                      uint32_t    language) {
    if (!g_conn.connected) {
        return MB_EINVAL;
    }
    if (g_conn.handshaken) {
        return MB_EINVAL;
    }

    const char *bid = bundle_id      ? bundle_id      : "unknown.bundle";
    const char *bve = bundle_version ? bundle_version : "0.0.0";

    // Encode HELLO.
    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 128);
    mb_cbor_w_map_begin(&w, 6);
    mb_cbor_w_key(&w, 1);  mb_cbor_w_uint(&w, MOONBASE_API_VERSION);
    mb_cbor_w_key(&w, 2);  mb_cbor_w_tstr(&w, bid);
    mb_cbor_w_key(&w, 3);  mb_cbor_w_tstr(&w, bve);
    mb_cbor_w_key(&w, 4);  mb_cbor_w_uint(&w, (uint64_t)getpid());
    mb_cbor_w_key(&w, 5);  mb_cbor_w_array_begin(&w, 0);
    mb_cbor_w_key(&w, 6);  mb_cbor_w_uint(&w, language);

    if (!mb_cbor_w_ok(&w)) {
        int err = mb_cbor_w_err(&w);
        mb_cbor_w_drop(&w);
        return err ? err : MB_ENOMEM;
    }

    size_t   body_len = 0;
    uint8_t *body = mb_cbor_w_finish(&w, &body_len);
    if (!body) {
        return MB_ENOMEM;
    }

    int rc = mb_conn_send(MB_IPC_HELLO, body, body_len, NULL, 0);
    free(body);
    if (rc < 0) {
        return rc;
    }

    // Receive the WELCOME (or an ERROR frame).
    uint16_t  in_kind = 0;
    uint8_t  *in_body = NULL;
    size_t    in_len  = 0;
    size_t    nfds = 0;
    int       recv_rc = mb_conn_recv(&in_kind, &in_body, &in_len,
                                     NULL, &nfds);
    if (recv_rc == 0) {
        return MB_EIPC;   // peer closed mid-handshake
    }
    if (recv_rc < 0) {
        return recv_rc;
    }

    if (in_kind == MB_IPC_ERROR) {
        // ERROR { 1: int code, 2: tstr message, 3: uint req_id? }
        mb_cbor_r_t r;
        mb_cbor_r_init(&r, in_body, in_len);
        uint64_t pairs = 0;
        int64_t  code  = MB_EVERSION;
        if (mb_cbor_r_map_begin(&r, &pairs)) {
            for (uint64_t i = 0; i < pairs; i++) {
                uint64_t key = 0;
                if (!mb_cbor_r_uint(&r, &key)) break;
                if (key == 1) {
                    (void)mb_cbor_r_int(&r, &code);
                } else {
                    mb_cbor_r_skip(&r);
                }
            }
        }
        free(in_body);
        return (int)code;
    }

    if (in_kind != MB_IPC_WELCOME) {
        free(in_body);
        return MB_EPROTO;
    }

    // Parse WELCOME.
    //   1: uint api_version
    //   2: uint max_frame_len
    //   3: [tstr] capabilities
    //   4: uint session_type
    mb_cbor_r_t r;
    mb_cbor_r_init(&r, in_body, in_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs)) {
        free(in_body);
        return MB_EPROTO;
    }

    uint32_t peer_api = 0;
    uint32_t peer_max = 0;
    uint32_t peer_session = 0;
    char   **caps = NULL;
    size_t   caps_len = 0;

    for (uint64_t i = 0; i < pairs; i++) {
        uint64_t key = 0;
        if (!mb_cbor_r_uint(&r, &key)) { goto proto_err; }
        switch (key) {
            case 1: {
                uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) goto proto_err;
                peer_api = (uint32_t)v;
                break;
            }
            case 2: {
                uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) goto proto_err;
                peer_max = (uint32_t)v;
                break;
            }
            case 3: {
                uint64_t n = 0;
                if (!mb_cbor_r_array_begin(&r, &n)) goto proto_err;
                caps = calloc(n + 1, sizeof(char *));
                if (!caps) goto proto_err;
                for (uint64_t j = 0; j < n; j++) {
                    const char *s = NULL;
                    size_t      sl = 0;
                    if (!mb_cbor_r_tstr(&r, &s, &sl)) goto proto_err;
                    char *copy = malloc(sl + 1);
                    if (!copy) goto proto_err;
                    memcpy(copy, s, sl);
                    copy[sl] = '\0';
                    caps[caps_len++] = copy;
                }
                break;
            }
            case 4: {
                uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) goto proto_err;
                peer_session = (uint32_t)v;
                break;
            }
            default:
                if (!mb_cbor_r_skip(&r)) goto proto_err;
                break;
        }
    }

    free(in_body);

    // Major-version check. Client's own major is MOONBASE_API_VERSION / 10000.
    if (peer_api / 10000 != MOONBASE_API_VERSION / 10000) {
        if (caps) {
            for (size_t i = 0; i < caps_len; i++) free(caps[i]);
            free(caps);
        }
        return MB_EVERSION;
    }

    g_conn.peer_api_version = peer_api;
    g_conn.max_frame_len    = peer_max;
    g_conn.session_type     = peer_session;
    g_conn.capabilities     = caps;
    g_conn.capability_count = caps_len;
    g_conn.handshaken       = true;
    return 0;

proto_err:
    if (caps) {
        for (size_t i = 0; i < caps_len; i++) free(caps[i]);
        free(caps);
    }
    free(in_body);
    return MB_EPROTO;
}

// ---------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------

bool     mb_conn_is_connected(void)      { return g_conn.connected; }
bool     mb_conn_is_handshaken(void)     { return g_conn.handshaken; }
int      mb_conn_fd(void)                { return g_conn.fd; }
uint32_t mb_conn_peer_api_version(void)  { return g_conn.peer_api_version; }
uint32_t mb_conn_max_frame_len(void)     { return g_conn.max_frame_len; }

int mb_conn_has_capability(const char *name) {
    if (!name || !g_conn.capabilities) return 0;
    for (size_t i = 0; i < g_conn.capability_count; i++) {
        if (strcmp(g_conn.capabilities[i], name) == 0) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------
// Frame send / recv
// ---------------------------------------------------------------------

int mb_conn_send(uint16_t kind,
                 const uint8_t *body, size_t body_len,
                 const int *fds, size_t nfds) {
    if (!g_conn.connected) return MB_EINVAL;
    mb_debug_dump_frame(true, kind, body, body_len);
    return mb_ipc_frame_send(g_conn.fd, kind, body, body_len, fds, nfds);
}

int mb_conn_recv(uint16_t *out_kind,
                 uint8_t **out_body, size_t *out_body_len,
                 int *fds, size_t *io_nfds) {
    if (!g_conn.connected) return MB_EINVAL;
    int rc = mb_ipc_frame_recv(g_conn.fd, out_kind, out_body, out_body_len,
                               fds, io_nfds);
    if (rc == 1) {
        mb_debug_dump_frame(false, *out_kind, *out_body, *out_body_len);
    }
    return rc;
}

// Decode the `code` field of an ERROR body. Returns MB_EPROTO if the
// body is malformed. Safe on body == NULL / body_len == 0.
static int decode_error_code(const uint8_t *body, size_t body_len) {
    mb_cbor_r_t r;
    mb_cbor_r_init(&r, body, body_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs)) return MB_EPROTO;
    int64_t code = MB_EIPC;
    for (uint64_t i = 0; i < pairs; i++) {
        uint64_t key = 0;
        if (!mb_cbor_r_uint(&r, &key)) return MB_EPROTO;
        if (key == 1) {
            if (!mb_cbor_r_int(&r, &code)) return MB_EPROTO;
        } else {
            if (!mb_cbor_r_skip(&r)) return MB_EPROTO;
        }
    }
    return (int)code;
}

int mb_conn_request(uint16_t kind,
                    const uint8_t *body, size_t body_len,
                    uint16_t reply_kind,
                    uint8_t **out_reply_body, size_t *out_reply_body_len) {
    if (out_reply_body)     *out_reply_body     = NULL;
    if (out_reply_body_len) *out_reply_body_len = 0;

    int rc = mb_conn_send(kind, body, body_len, NULL, 0);
    if (rc < 0) return rc;

    for (;;) {
        uint16_t in_kind = 0;
        uint8_t *in_body = NULL;
        size_t   in_len  = 0;
        size_t   nfds    = 0;
        int r = mb_conn_recv(&in_kind, &in_body, &in_len, NULL, &nfds);
        if (r == 0) return MB_EIPC;     // peer closed mid-request
        if (r < 0) return r;

        if (in_kind == reply_kind) {
            if (out_reply_body)     *out_reply_body     = in_body;
            else                    free(in_body);
            if (out_reply_body_len) *out_reply_body_len = in_len;
            return 0;
        }
        if (in_kind == MB_IPC_ERROR) {
            int code = decode_error_code(in_body, in_len);
            free(in_body);
            return code;
        }
        // Unrelated frame — park it on the pending-frame queue so the
        // app's next poll_event / wait_event turn can pick it up.
        int qrc = conn_queue_push(in_kind, in_body, in_len);
        if (qrc < 0) {
            free(in_body);
            return qrc;
        }
    }
}
