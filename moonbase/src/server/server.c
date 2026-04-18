// CopyCatOS — by Kyle Blizzard at Blizzard.show

// server.c — MoonBase IPC server implementation.
//
// Non-blocking listener plus per-client state. Frames arrive in
// whatever fragments the kernel hands us; we buffer until a complete
// u32+u16+body frame is present, then dispatch. Outbound frames are
// serialized into a growing byte queue and drained as POLLOUT fires.

#include "server.h"

#include "../ipc/cbor.h"
#include "../ipc/frame.h"
#include "moonbase.h"
#include "moonbase/ipc/kinds.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

// ---------------------------------------------------------------------
// Byte queue
// ---------------------------------------------------------------------
//
// Ring-buffer-ish growing byte queue with O(1) amortised append and
// O(1) consume-from-front. Used for both recv reassembly and send
// staging per client. Simple enough that a struct + 4 functions is
// the whole interface.

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   head;     // first valid byte
    size_t   tail;     // one past last valid byte
} byteq_t;

static void bq_init(byteq_t *q) {
    q->buf = NULL;
    q->cap = q->head = q->tail = 0;
}

static void bq_free(byteq_t *q) {
    free(q->buf);
    bq_init(q);
}

static size_t bq_len(const byteq_t *q) { return q->tail - q->head; }

static void bq_compact(byteq_t *q) {
    if (q->head == 0) return;
    size_t n = bq_len(q);
    if (n) memmove(q->buf, q->buf + q->head, n);
    q->head = 0;
    q->tail = n;
}

static int bq_reserve(byteq_t *q, size_t extra) {
    if (q->tail + extra <= q->cap) return 0;
    bq_compact(q);
    if (q->tail + extra <= q->cap) return 0;
    size_t new_cap = q->cap ? q->cap : 256;
    while (new_cap < q->tail + extra) {
        if (new_cap > SIZE_MAX / 2) return MB_ENOMEM;
        new_cap *= 2;
    }
    uint8_t *nb = realloc(q->buf, new_cap);
    if (!nb) return MB_ENOMEM;
    q->buf = nb;
    q->cap = new_cap;
    return 0;
}

static int bq_append(byteq_t *q, const uint8_t *data, size_t n) {
    int rc = bq_reserve(q, n);
    if (rc) return rc;
    memcpy(q->buf + q->tail, data, n);
    q->tail += n;
    return 0;
}

static void bq_consume(byteq_t *q, size_t n) {
    q->head += n;
    if (q->head == q->tail) { q->head = q->tail = 0; }
}

// ---------------------------------------------------------------------
// Per-client state
// ---------------------------------------------------------------------

typedef struct mb_client {
    struct mb_client *next;
    mb_client_id_t    id;
    int               fd;
    bool              handshaken;
    bool              pending_close;    // flush then drop
    byteq_t           rx;
    byteq_t           tx;

    // Identity recorded from HELLO. Copied into owned storage so the
    // frame buffer can be reused immediately.
    uint32_t          api_version;
    char             *bundle_id;
    char             *bundle_version;
    uint32_t          pid;
    uint32_t          language;
} mb_client_t;

static void client_free(mb_client_t *c) {
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    bq_free(&c->rx);
    bq_free(&c->tx);
    free(c->bundle_id);
    free(c->bundle_version);
    free(c);
}

// ---------------------------------------------------------------------
// Server state
// ---------------------------------------------------------------------

struct mb_server {
    int                    listen_fd;
    char                  *path;
    mb_server_on_event_fn  on_event;
    void                  *user;
    mb_client_t           *clients;       // singly-linked list
    mb_client_id_t         next_client_id;
};

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return MB_EIPC;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return MB_EIPC;
    return 0;
}

// Big-endian pack helpers, matching frame.c.
static void be32(uint8_t *p, uint32_t v) {
    p[0]=(v>>24)&0xff; p[1]=(v>>16)&0xff; p[2]=(v>>8)&0xff; p[3]=v&0xff;
}
static void be16(uint8_t *p, uint16_t v) {
    p[0]=(v>>8)&0xff; p[1]=v&0xff;
}
static uint32_t ld_be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16)
         | ((uint32_t)p[2]<<8)  |  (uint32_t)p[3];
}
static uint16_t ld_be16(const uint8_t *p) {
    return (uint16_t)((p[0]<<8) | p[1]);
}

// Queue a framed message onto a client's tx buffer.
static int client_queue_frame(mb_client_t *c, uint16_t kind,
                              const uint8_t *body, size_t body_len) {
    if (body_len > MB_IPC_FRAME_MAX_LEN - 2) return MB_EPROTO;
    uint8_t  hdr[6];
    uint32_t length = (uint32_t)body_len + 2;
    be32(hdr, length);
    be16(hdr + 4, kind);
    int rc = bq_append(&c->tx, hdr, 6);
    if (rc) return rc;
    if (body_len) {
        rc = bq_append(&c->tx, body, body_len);
        if (rc) return rc;
    }
    return 0;
}

// Send an MB_IPC_ERROR on a client with code + optional message.
// Best-effort; if queueing fails we just drop the client.
static void client_send_error(mb_client_t *c, int code, const char *msg) {
    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 64);
    mb_cbor_w_map_begin(&w, msg ? 2 : 1);
    mb_cbor_w_key(&w, 1); mb_cbor_w_int(&w, code);
    if (msg) { mb_cbor_w_key(&w, 2); mb_cbor_w_tstr(&w, msg); }
    if (!mb_cbor_w_ok(&w)) { mb_cbor_w_drop(&w); c->pending_close = true; return; }
    size_t wl = 0;
    uint8_t *wb = mb_cbor_w_finish(&w, &wl);
    if (!wb) { c->pending_close = true; return; }
    if (client_queue_frame(c, MB_IPC_ERROR, wb, wl) != 0) {
        c->pending_close = true;
    }
    free(wb);
}

// Build a WELCOME body.
static uint8_t *build_welcome(size_t *out_len) {
    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 64);
    mb_cbor_w_map_begin(&w, 4);
    mb_cbor_w_key(&w, 1); mb_cbor_w_uint(&w, MOONBASE_API_VERSION);
    mb_cbor_w_key(&w, 2); mb_cbor_w_uint(&w, MB_IPC_FRAME_MAX_LEN);
    // Capabilities list — advertise the two rendering backends we
    // plan to support. Real entries will grow as features land.
    mb_cbor_w_key(&w, 3); mb_cbor_w_array_begin(&w, 2);
                          mb_cbor_w_tstr(&w, "cairo");
                          mb_cbor_w_tstr(&w, "gl");
    // session_type=0 (desktop). Gaming never handshakes, per IPC.md.
    mb_cbor_w_key(&w, 4); mb_cbor_w_uint(&w, 0);
    if (!mb_cbor_w_ok(&w)) { mb_cbor_w_drop(&w); return NULL; }
    return mb_cbor_w_finish(&w, out_len);
}

// Parse HELLO body into client identity. Returns 0 or a negative
// mb_error_t (MB_EPROTO on any malformed field, MB_EVERSION on a
// major mismatch).
static int parse_hello(mb_client_t *c, const uint8_t *body, size_t body_len) {
    mb_cbor_r_t r;
    mb_cbor_r_init(&r, body, body_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs)) return MB_EPROTO;

    for (uint64_t i = 0; i < pairs; i++) {
        uint64_t key = 0;
        if (!mb_cbor_r_uint(&r, &key)) return MB_EPROTO;
        switch (key) {
            case 1: {
                uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return MB_EPROTO;
                c->api_version = (uint32_t)v;
                break;
            }
            case 2: {
                const char *s = NULL; size_t sl = 0;
                if (!mb_cbor_r_tstr(&r, &s, &sl)) return MB_EPROTO;
                free(c->bundle_id);
                c->bundle_id = malloc(sl + 1);
                if (!c->bundle_id) return MB_ENOMEM;
                memcpy(c->bundle_id, s, sl);
                c->bundle_id[sl] = '\0';
                break;
            }
            case 3: {
                const char *s = NULL; size_t sl = 0;
                if (!mb_cbor_r_tstr(&r, &s, &sl)) return MB_EPROTO;
                free(c->bundle_version);
                c->bundle_version = malloc(sl + 1);
                if (!c->bundle_version) return MB_ENOMEM;
                memcpy(c->bundle_version, s, sl);
                c->bundle_version[sl] = '\0';
                break;
            }
            case 4: {
                uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return MB_EPROTO;
                c->pid = (uint32_t)v;
                break;
            }
            case 5: {
                // Entitlements array — skipped in this slice. When the
                // permission system lands, this is where per-client
                // entitlements get recorded.
                uint64_t n = 0;
                if (!mb_cbor_r_array_begin(&r, &n)) return MB_EPROTO;
                for (uint64_t j = 0; j < n; j++) {
                    if (!mb_cbor_r_skip(&r)) return MB_EPROTO;
                }
                break;
            }
            case 6: {
                uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return MB_EPROTO;
                c->language = (uint32_t)v;
                break;
            }
            default:
                if (!mb_cbor_r_skip(&r)) return MB_EPROTO;
                break;
        }
    }

    // Major version must match. Minor may differ.
    if (c->api_version / 10000 != MOONBASE_API_VERSION / 10000) {
        return MB_EVERSION;
    }
    return 0;
}

// ---------------------------------------------------------------------
// Connection lifecycle
// ---------------------------------------------------------------------

static void deliver_disconnect(mb_server_t *s, mb_client_t *c, int reason) {
    mb_server_event_t ev = {0};
    ev.kind = MB_SERVER_EV_DISCONNECTED;
    ev.client = c->id;
    ev.disconnect_reason = reason;
    // Only deliver disconnects for clients the caller was told about.
    // Clients that never finished the handshake are torn down silently.
    if (c->handshaken && s->on_event) s->on_event(s, &ev, s->user);
}

static void server_remove_client(mb_server_t *s, mb_client_t *c, int reason) {
    deliver_disconnect(s, c, reason);
    mb_client_t **pp = &s->clients;
    while (*pp && *pp != c) pp = &(*pp)->next;
    if (*pp) *pp = c->next;
    client_free(c);
}

static int server_accept(mb_server_t *s) {
    int cfd = accept(s->listen_fd, NULL, NULL);
    if (cfd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MB_EAGAIN;
        if (errno == EINTR) return MB_EAGAIN;
        return MB_EIPC;
    }

    // Peer-credential check: same euid as moonrock. Rejects apps run
    // by other users from sneaking in via an open-mode socket.
#if defined(SO_PEERCRED) && defined(__linux__)
    struct ucred cred; socklen_t cl = sizeof(cred);
    if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &cl) == 0) {
        if (cred.uid != geteuid()) { close(cfd); return MB_EPERM; }
    }
#endif

    if (set_nonblock(cfd) != 0) { close(cfd); return MB_EIPC; }

    mb_client_t *c = calloc(1, sizeof(*c));
    if (!c) { close(cfd); return MB_ENOMEM; }
    c->id = ++s->next_client_id;
    c->fd = cfd;
    bq_init(&c->rx);
    bq_init(&c->tx);
    c->next = s->clients;
    s->clients = c;
    return 0;
}

// ---------------------------------------------------------------------
// Frame parse / dispatch
// ---------------------------------------------------------------------

static void handle_complete_frame(mb_server_t *s, mb_client_t *c,
                                  uint16_t kind,
                                  const uint8_t *body, size_t body_len) {
    if (!c->handshaken) {
        if (kind != MB_IPC_HELLO) {
            client_send_error(c, MB_EPROTO, "handshake required");
            c->pending_close = true;
            return;
        }
        int rc = parse_hello(c, body, body_len);
        if (rc == MB_EVERSION) {
            client_send_error(c, MB_EVERSION, "api major mismatch");
            c->pending_close = true;
            return;
        }
        if (rc != 0) {
            client_send_error(c, rc, "bad hello");
            c->pending_close = true;
            return;
        }

        // Send WELCOME.
        size_t   wl = 0;
        uint8_t *wb = build_welcome(&wl);
        if (!wb) { c->pending_close = true; return; }
        rc = client_queue_frame(c, MB_IPC_WELCOME, wb, wl);
        free(wb);
        if (rc != 0) { c->pending_close = true; return; }

        c->handshaken = true;

        mb_server_event_t ev = {0};
        ev.kind              = MB_SERVER_EV_CONNECTED;
        ev.client            = c->id;
        ev.hello.api_version = c->api_version;
        ev.hello.bundle_id   = c->bundle_id      ? c->bundle_id      : "";
        ev.hello.bundle_version = c->bundle_version ? c->bundle_version : "";
        ev.hello.pid         = c->pid;
        ev.hello.language    = c->language;
        if (s->on_event) s->on_event(s, &ev, s->user);
        return;
    }

    // Post-handshake. BYE drops the client cleanly.
    if (kind == MB_IPC_BYE) {
        c->pending_close = true;
        return;
    }

    mb_server_event_t ev = {0};
    ev.kind           = MB_SERVER_EV_FRAME;
    ev.client         = c->id;
    ev.frame_kind     = kind;
    ev.frame_body     = body;
    ev.frame_body_len = body_len;
    if (s->on_event) s->on_event(s, &ev, s->user);
}

// Try to parse and dispatch as many complete frames as the rx buffer
// holds. Returns 0 on success or a negative mb_error_t if the stream
// is unrecoverable (e.g. oversize frame, malformed header).
static int client_drain_rx(mb_server_t *s, mb_client_t *c) {
    while (bq_len(&c->rx) >= 6) {
        const uint8_t *p = c->rx.buf + c->rx.head;
        uint32_t length = ld_be32(p);
        uint16_t kind   = ld_be16(p + 4);

        if (length < 2 || length > MB_IPC_FRAME_MAX_LEN) {
            return MB_EPROTO;
        }
        size_t frame_total = (size_t)length + 4;  // length field is u32
        if (bq_len(&c->rx) < frame_total) {
            return 0;  // wait for more bytes
        }

        const uint8_t *body = p + 6;
        size_t body_len     = (size_t)length - 2;
        handle_complete_frame(s, c, kind, body, body_len);
        bq_consume(&c->rx, frame_total);

        if (c->pending_close) return 0;
    }
    return 0;
}

// Read what's available on a client fd and drain the rx buffer.
// We always drain before reporting EOF so a BYE frame delivered in
// the same read as the close is observed as a clean shutdown instead
// of a crash.
static int client_read(mb_server_t *s, mb_client_t *c) {
    uint8_t buf[4096];
    bool eof = false;
    for (;;) {
        ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
        if (n > 0) {
            int rc = bq_append(&c->rx, buf, (size_t)n);
            if (rc != 0) return rc;
            continue;
        }
        if (n == 0) { eof = true; break; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return MB_EIPC;
    }
    int rc = client_drain_rx(s, c);
    if (rc != 0) return rc;
    if (eof) {
        // BYE during the drain already marked pending_close — that
        // path treats the exit as clean (reason=0). Any other EOF is
        // a crash.
        if (!c->pending_close) return MB_ENOTFOUND;
    }
    return 0;
}

// Flush as much of the tx buffer as the kernel accepts.
static int client_write(mb_client_t *c) {
    while (bq_len(&c->tx) > 0) {
        ssize_t n = send(c->fd, c->tx.buf + c->tx.head, bq_len(&c->tx),
#ifdef MSG_NOSIGNAL
                         MSG_NOSIGNAL
#else
                         0
#endif
                         );
        if (n > 0) { bq_consume(&c->tx, (size_t)n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return MB_EIPC;
    }
    return 0;
}

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------

int mb_server_open(mb_server_t **out,
                   const char *path,
                   mb_server_on_event_fn on_event,
                   void *user) {
    if (!out || !path) return MB_EINVAL;

    int listen_fd = mb_ipc_frame_listen(path);
    if (listen_fd < 0) return listen_fd;

    int rc = set_nonblock(listen_fd);
    if (rc != 0) { close(listen_fd); unlink(path); return rc; }

    mb_server_t *s = calloc(1, sizeof(*s));
    if (!s) { close(listen_fd); unlink(path); return MB_ENOMEM; }
    s->listen_fd = listen_fd;
    s->path      = strdup(path);
    s->on_event  = on_event;
    s->user      = user;
    *out = s;
    return 0;
}

void mb_server_close(mb_server_t *s) {
    if (!s) return;
    mb_client_t *c = s->clients;
    while (c) {
        mb_client_t *next = c->next;
        // Deliver disconnects for any handshaken clients so callers
        // can clean up their own per-client state.
        deliver_disconnect(s, c, 0);
        client_free(c);
        c = next;
    }
    if (s->listen_fd >= 0) close(s->listen_fd);
    if (s->path) { unlink(s->path); free(s->path); }
    free(s);
}

size_t mb_server_get_pollfds(mb_server_t *s,
                             struct pollfd *out_fds, size_t max) {
    if (!s || !out_fds || max == 0) return 0;
    size_t n = 0;
    out_fds[n].fd      = s->listen_fd;
    out_fds[n].events  = POLLIN;
    out_fds[n].revents = 0;
    n++;
    for (mb_client_t *c = s->clients; c && n < max; c = c->next) {
        out_fds[n].fd = c->fd;
        out_fds[n].events = POLLIN | (bq_len(&c->tx) ? POLLOUT : 0);
        out_fds[n].revents = 0;
        n++;
    }
    return n;
}

void mb_server_tick(mb_server_t *s,
                    const struct pollfd *fds, size_t nfds) {
    if (!s) return;

    // Listener: drain any pending accepts first so new clients are
    // seen the moment they connect.
    bool listener_ready = false;
    for (size_t i = 0; i < nfds; i++) {
        if (fds[i].fd == s->listen_fd && (fds[i].revents & (POLLIN|POLLERR))) {
            listener_ready = true;
            break;
        }
    }
    if (listener_ready) {
        for (;;) {
            int rc = server_accept(s);
            if (rc == MB_EAGAIN) break;
            if (rc != 0) {
                fprintf(stderr, "[moonbase-server] accept failed: %d\n", rc);
                break;
            }
        }
    }

    // Drive each client. We iterate with an explicit next-pointer so
    // dropping the current client inside the loop is safe.
    mb_client_t *c = s->clients;
    while (c) {
        mb_client_t *next = c->next;

        short revents = 0;
        for (size_t i = 0; i < nfds; i++) {
            if (fds[i].fd == c->fd) { revents = fds[i].revents; break; }
        }

        if (revents & (POLLIN | POLLERR | POLLHUP)) {
            int rc = client_read(s, c);
            if (rc != 0) {
                server_remove_client(s, c, rc);
                c = next;
                continue;
            }
        }
        if (revents & POLLOUT) {
            int rc = client_write(c);
            if (rc != 0) {
                server_remove_client(s, c, rc);
                c = next;
                continue;
            }
        }
        // Pending close: try one more flush, then drop when the tx
        // queue is empty. Avoids racing past a just-queued WELCOME or
        // ERROR that hasn't made it to the wire yet.
        if (c->pending_close) {
            (void)client_write(c);
            if (bq_len(&c->tx) == 0) {
                server_remove_client(s, c, 0);
                c = next;
                continue;
            }
        }
        c = next;
    }
}

int mb_server_send(mb_server_t *s,
                   mb_client_id_t client,
                   uint16_t kind,
                   const uint8_t *body, size_t body_len,
                   const int *fds, size_t nfds) {
    (void)fds; (void)nfds;  // SCM_RIGHTS not in this slice
    if (!s) return MB_EINVAL;
    mb_client_t *c = NULL;
    for (mb_client_t *it = s->clients; it; it = it->next) {
        if (it->id == client) { c = it; break; }
    }
    if (!c) return MB_ENOTFOUND;
    int rc = client_queue_frame(c, kind, body, body_len);
    if (rc != 0) return rc;
    // Opportunistically flush so small frames don't wait for the next
    // poll tick.
    (void)client_write(c);
    return 0;
}
