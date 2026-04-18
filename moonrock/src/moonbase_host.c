// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase_host.c — MoonRock's MoonBase IPC host.
//
// Owns the server-side singleton: one AF_UNIX listener, one mb_server_t.
// The compositor's event loop (events.c) calls through this module
// each tick; every post-handshake frame flows through on_event below.
//
// The adapter logs every lifecycle event in this slice. Slice 3 grows
// the on_event handler into real dispatch: WINDOW_CREATE → reparented
// X window, POINTER events out to focused client, scale-change events
// on output migration. For now the compositor just proves the socket
// is live and speaking the protocol.

#include "moonbase_host.h"

#include "server.h"
#include "cbor.h"
#include "moonbase/ipc/kinds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mb_server_t *g_server = NULL;
static char        *g_default_path = NULL;   // owned, only used when we built it
static uint32_t     g_next_window_id = 1;    // opaque window_id counter

// Parse a WINDOW_CREATE body. Ignores unknown keys so the schema can
// grow later without wire-format churn. Returns true if the minimum
// viable set (width, height) was present and well-formed.
typedef struct {
    uint32_t version;
    char    *title;              // owned or NULL
    int32_t  width_points, height_points;
    int32_t  min_width_points, min_height_points;
    int32_t  max_width_points, max_height_points;
    uint32_t render_mode;        // 0 cairo, 1 gl
    uint32_t flags;
} window_create_req_t;

static bool parse_window_create(const uint8_t *body, size_t body_len,
                                window_create_req_t *out) {
    memset(out, 0, sizeof(*out));
    mb_cbor_r_t r;
    mb_cbor_r_init(&r, body, body_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs)) return false;
    bool have_w = false, have_h = false;
    for (uint64_t i = 0; i < pairs; i++) {
        uint64_t key = 0;
        if (!mb_cbor_r_uint(&r, &key)) return false;
        switch (key) {
            case 1: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return false;
                out->version = (uint32_t)v; break; }
            case 2: { const char *s = NULL; size_t sl = 0;
                if (!mb_cbor_r_tstr(&r, &s, &sl)) return false;
                out->title = malloc(sl + 1);
                if (!out->title) return false;
                memcpy(out->title, s, sl);
                out->title[sl] = '\0'; break; }
            case 3: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return false;
                out->width_points = (int32_t)v; have_w = true; break; }
            case 4: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return false;
                out->height_points = (int32_t)v; have_h = true; break; }
            case 5: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return false;
                out->min_width_points = (int32_t)v; break; }
            case 6: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return false;
                out->min_height_points = (int32_t)v; break; }
            case 7: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return false;
                out->max_width_points = (int32_t)v; break; }
            case 8: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return false;
                out->max_height_points = (int32_t)v; break; }
            case 9: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return false;
                out->render_mode = (uint32_t)v; break; }
            case 10: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return false;
                out->flags = (uint32_t)v; break; }
            default:
                if (!mb_cbor_r_skip(&r)) return false;
                break;
        }
    }
    return have_w && have_h;
}

// Build a WINDOW_CREATE_REPLY body with the given fields.
// Caller takes ownership of the returned buffer.
static uint8_t *build_window_create_reply(uint32_t window_id,
                                          uint32_t output_id,
                                          double   initial_scale,
                                          uint32_t actual_w_points,
                                          uint32_t actual_h_points,
                                          size_t *out_len) {
    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 48);
    mb_cbor_w_map_begin(&w, 5);
    mb_cbor_w_key(&w, 1); mb_cbor_w_uint(&w, window_id);
    mb_cbor_w_key(&w, 2); mb_cbor_w_uint(&w, output_id);
    mb_cbor_w_key(&w, 3); mb_cbor_w_float(&w, initial_scale);
    mb_cbor_w_key(&w, 4); mb_cbor_w_uint(&w, actual_w_points);
    mb_cbor_w_key(&w, 5); mb_cbor_w_uint(&w, actual_h_points);
    if (!mb_cbor_w_ok(&w)) {
        mb_cbor_w_drop(&w);
        return NULL;
    }
    return mb_cbor_w_finish(&w, out_len);
}

// Handle a WINDOW_CREATE request. Allocates a window_id and replies.
// Slice 3a stubs the real X-window allocation — scale=1.0, output=0,
// dimensions echoed unchanged. Slice 3b lands per-output scale by
// EDID. Slice 3c reparents a real X window.
static void handle_window_create(mb_server_t *s, mb_client_id_t client,
                                 const uint8_t *body, size_t body_len) {
    window_create_req_t req;
    if (!parse_window_create(body, body_len, &req)) {
        fprintf(stderr,
                "[moonrock] moonbase client %u sent malformed WINDOW_CREATE\n",
                client);
        free(req.title);
        return;
    }
    uint32_t window_id = g_next_window_id++;
    fprintf(stderr,
            "[moonrock] moonbase client %u WINDOW_CREATE: "
            "%s %dx%d pt render=%u flags=0x%x -> window_id=%u\n",
            client, req.title ? req.title : "(no title)",
            req.width_points, req.height_points,
            req.render_mode, req.flags, window_id);
    free(req.title);

    size_t reply_len = 0;
    uint8_t *reply = build_window_create_reply(
        window_id,
        0,                                          // output_id (slice 3b)
        1.0,                                        // initial_scale (slice 3b)
        (uint32_t)req.width_points,
        (uint32_t)req.height_points,
        &reply_len);
    if (!reply) {
        fprintf(stderr,
                "[moonrock] moonbase client %u: out of memory building reply\n",
                client);
        return;
    }
    int rc = mb_server_send(s, client, MB_IPC_WINDOW_CREATE_REPLY,
                            reply, reply_len, NULL, 0);
    free(reply);
    if (rc != 0) {
        fprintf(stderr,
                "[moonrock] moonbase client %u: send reply failed (%d)\n",
                client, rc);
    }
}

static void on_event(mb_server_t *s, const mb_server_event_t *ev, void *user) {
    (void)user;
    switch (ev->kind) {
        case MB_SERVER_EV_CONNECTED:
            fprintf(stderr,
                    "[moonrock] moonbase client %u connected: "
                    "bundle=%s ver=%s pid=%u lang=%u api=%u\n",
                    ev->client,
                    ev->hello.bundle_id && *ev->hello.bundle_id
                        ? ev->hello.bundle_id : "(unknown)",
                    ev->hello.bundle_version && *ev->hello.bundle_version
                        ? ev->hello.bundle_version : "(0.0.0)",
                    ev->hello.pid, ev->hello.language, ev->hello.api_version);
            break;
        case MB_SERVER_EV_FRAME:
            switch (ev->frame_kind) {
                case MB_IPC_WINDOW_CREATE:
                    handle_window_create(s, ev->client,
                                         ev->frame_body, ev->frame_body_len);
                    break;
                case MB_IPC_WINDOW_CLOSE:
                    fprintf(stderr,
                            "[moonrock] moonbase client %u WINDOW_CLOSE len=%zu\n",
                            ev->client, ev->frame_body_len);
                    break;
                default:
                    fprintf(stderr,
                            "[moonrock] moonbase client %u frame 0x%04x len=%zu\n",
                            ev->client, ev->frame_kind, ev->frame_body_len);
                    break;
            }
            break;
        case MB_SERVER_EV_DISCONNECTED:
            fprintf(stderr,
                    "[moonrock] moonbase client %u disconnected (reason=%d)\n",
                    ev->client, ev->disconnect_reason);
            break;
    }
}

static char *default_socket_path(void) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (!xdg || !*xdg) return NULL;
    size_t len = strlen(xdg) + strlen("/moonbase.sock") + 1;
    char *p = malloc(len);
    if (!p) return NULL;
    snprintf(p, len, "%s/moonbase.sock", xdg);
    return p;
}

bool mb_host_init(const char *path) {
    if (g_server) {
        fprintf(stderr, "[moonrock] moonbase host already running\n");
        return true;
    }

    const char *use_path = path;
    if (!use_path) {
        g_default_path = default_socket_path();
        if (!g_default_path) {
            fprintf(stderr,
                    "[moonrock] XDG_RUNTIME_DIR not set — MoonBase IPC disabled\n");
            return false;
        }
        use_path = g_default_path;
    }

    int rc = mb_server_open(&g_server, use_path, on_event, NULL);
    if (rc != 0) {
        fprintf(stderr, "[moonrock] mb_server_open(%s) failed: %d\n",
                use_path, rc);
        free(g_default_path); g_default_path = NULL;
        g_server = NULL;
        return false;
    }
    fprintf(stderr, "[moonrock] moonbase host listening on %s\n", use_path);
    return true;
}

size_t mb_host_collect_pollfds(struct pollfd *out_fds, size_t max) {
    if (!g_server || !out_fds || max == 0) return 0;
    return mb_server_get_pollfds(g_server, out_fds, max);
}

void mb_host_tick(const struct pollfd *fds, size_t nfds) {
    if (!g_server) return;
    mb_server_tick(g_server, fds, nfds);
}

void mb_host_shutdown(void) {
    if (!g_server) return;
    mb_server_close(g_server);
    g_server = NULL;
    free(g_default_path);
    g_default_path = NULL;
}
