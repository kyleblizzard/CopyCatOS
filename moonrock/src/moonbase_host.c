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
#include "moonrock_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mb_server_t *g_server = NULL;
static char        *g_default_path = NULL;   // owned, only used when we built it
static uint32_t     g_next_window_id = 1;    // opaque window_id counter

// ─────────────────────────────────────────────────────────────────────
// Surface table — one entry per live MoonBase window
// ─────────────────────────────────────────────────────────────────────
//
// MoonBase windows are NOT X windows. They're compositor-internal
// surfaces that moonrock draws with Aqua chrome, composited via its
// existing GL pipeline alongside the reparented X clients. This table
// tracks the state we need to render (size, scale, target output,
// owning client) and to route events back (window_id <-> client).
//
// A linear array is fine — a typical desktop session has well under
// 100 live windows. When a client disconnects we sweep the table and
// drop every surface belonging to that client id, which is how crash
// cleanup works: if the app process dies, the socket closes, the
// server synthesizes DISCONNECTED, and the sweep runs.
//
// Rendering-path fields (GL texture id, shm fd, damage rect) land in
// slice 3c.2 when the commit path is real. For now the table just
// carries identity + geometry + pending visibility.

#define MB_MAX_SURFACES 256

typedef struct {
    bool           in_use;
    uint32_t       window_id;          // opaque handle returned to the client
    mb_client_id_t client;             // owning client id from the server layer
    int            points_w, points_h; // size in points (client's coordinates)
    float          scale;              // backing scale at creation time
    uint32_t       output_id;          // target RandR output_id at creation
    uint32_t       render_mode;        // 0 cairo, 1 gl
    uint32_t       flags;              // window flags from the request
    char          *title;              // owned copy, or NULL
} mb_surface_t;

static mb_surface_t g_surfaces[MB_MAX_SURFACES];
static int          g_surface_count = 0;

static mb_surface_t *surface_alloc(void) {
    for (int i = 0; i < MB_MAX_SURFACES; i++) {
        if (!g_surfaces[i].in_use) {
            memset(&g_surfaces[i], 0, sizeof(g_surfaces[i]));
            g_surfaces[i].in_use = true;
            g_surface_count++;
            return &g_surfaces[i];
        }
    }
    return NULL;
}

static mb_surface_t *surface_find(uint32_t window_id) {
    for (int i = 0; i < MB_MAX_SURFACES; i++) {
        if (g_surfaces[i].in_use && g_surfaces[i].window_id == window_id) {
            return &g_surfaces[i];
        }
    }
    return NULL;
}

static void surface_release(mb_surface_t *s) {
    if (!s || !s->in_use) return;
    free(s->title);
    memset(s, 0, sizeof(*s));
    g_surface_count--;
}

// Sweep every surface belonging to a given client. Called when that
// client disconnects (graceful BYE or abrupt EOF both route here).
static void surface_sweep_client(mb_client_id_t client) {
    int freed = 0;
    for (int i = 0; i < MB_MAX_SURFACES; i++) {
        if (g_surfaces[i].in_use && g_surfaces[i].client == client) {
            uint32_t wid = g_surfaces[i].window_id;
            surface_release(&g_surfaces[i]);
            freed++;
            (void)wid; // reserved for a compositor damage call in 3c.2
        }
    }
    if (freed > 0) {
        fprintf(stderr,
                "[moonrock] moonbase client %u disconnect: freed %d surface(s)\n",
                client, freed);
    }
}

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
// Slice 3a stubbed the scale at 1.0; slice 3b (this code) looks up the
// real scale of the target output — primary for now, since we don't yet
// have placement hints on the request. Slice 3c reparents a real X
// window.
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

    // Target the primary output for now. Future slice lets the client
    // request a specific output, or moonrock picks based on cursor /
    // focus / last-used-output state.
    MROutput *target = display_get_primary();
    uint32_t output_id  = 0;
    float    init_scale = 1.0f;
    if (target) {
        // output_id on the wire is an opaque handle the compositor owns.
        // We use the RandR output_id directly so later slices can route
        // per-output messages back to the right output without a lookup
        // table. u32 truncation is safe — RandR IDs fit in 32 bits.
        output_id  = (uint32_t)target->output_id;
        init_scale = display_get_scale_for_output(target);
    }

    // Record the surface so we can look it up on subsequent frames
    // (WINDOW_COMMIT, WINDOW_CLOSE, pointer/key routing). If the table
    // is full the request fails silently — no reply goes out and the
    // client sees a timeout, which is the right failure shape because
    // only a runaway app would pile up >256 live windows.
    mb_surface_t *surf = surface_alloc();
    if (!surf) {
        fprintf(stderr,
                "[moonrock] moonbase client %u: surface table full — "
                "dropping WINDOW_CREATE\n", client);
        free(req.title);
        return;
    }
    surf->window_id   = window_id;
    surf->client      = client;
    surf->points_w    = req.width_points;
    surf->points_h    = req.height_points;
    surf->scale       = init_scale;
    surf->output_id   = output_id;
    surf->render_mode = req.render_mode;
    surf->flags       = req.flags;
    surf->title       = req.title; // take ownership
    req.title = NULL;               // avoid the free below

    fprintf(stderr,
            "[moonrock] moonbase client %u WINDOW_CREATE: "
            "%s %dx%d pt render=%u flags=0x%x -> window_id=%u "
            "(output=%u scale=%.2f, live=%d)\n",
            client, surf->title ? surf->title : "(no title)",
            req.width_points, req.height_points,
            req.render_mode, req.flags, window_id,
            output_id, (double)init_scale, g_surface_count);
    free(req.title);

    size_t reply_len = 0;
    uint8_t *reply = build_window_create_reply(
        window_id,
        output_id,
        (double)init_scale,
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

// Parse a WINDOW_CLOSE body: { 1: uint window_id }.
// Returns 0 on failure — 0 is never a valid window_id because we
// start the counter at 1, so the caller can null-check cheaply.
static uint32_t parse_window_close_id(const uint8_t *body, size_t body_len) {
    mb_cbor_r_t r;
    mb_cbor_r_init(&r, body, body_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs)) return 0;
    uint32_t window_id = 0;
    for (uint64_t i = 0; i < pairs; i++) {
        uint64_t key = 0;
        if (!mb_cbor_r_uint(&r, &key)) return 0;
        if (key == 1) {
            uint64_t v = 0;
            if (!mb_cbor_r_uint(&r, &v)) return 0;
            window_id = (uint32_t)v;
        } else {
            if (!mb_cbor_r_skip(&r)) return 0;
        }
    }
    return window_id;
}

// Handle a client-initiated WINDOW_CLOSE. The app wants to release a
// window it previously created. We drop the surface entry; a future
// slice will emit a damage call so the compositor can stop drawing it.
static void handle_window_close(mb_client_id_t client,
                                const uint8_t *body, size_t body_len) {
    uint32_t window_id = parse_window_close_id(body, body_len);
    if (window_id == 0) {
        fprintf(stderr,
                "[moonrock] moonbase client %u sent malformed WINDOW_CLOSE\n",
                client);
        return;
    }
    mb_surface_t *surf = surface_find(window_id);
    if (!surf) {
        // Not an error — races between a client-side close and a
        // compositor-initiated close race-close on exit are possible.
        fprintf(stderr,
                "[moonrock] moonbase client %u WINDOW_CLOSE: window_id=%u "
                "already gone (ignoring)\n", client, window_id);
        return;
    }
    if (surf->client != client) {
        // Defense in depth: apps can only close their own windows.
        fprintf(stderr,
                "[moonrock] moonbase client %u WINDOW_CLOSE: window_id=%u "
                "belongs to client %u — refusing\n",
                client, window_id, surf->client);
        return;
    }
    fprintf(stderr,
            "[moonrock] moonbase client %u WINDOW_CLOSE: window_id=%u "
            "(%dx%d pt, live=%d)\n",
            client, window_id, surf->points_w, surf->points_h,
            g_surface_count - 1);
    surface_release(surf);
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
                    handle_window_close(ev->client,
                                        ev->frame_body, ev->frame_body_len);
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
            surface_sweep_client(ev->client);
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
