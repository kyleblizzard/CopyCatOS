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
#include "consent_responder.h"
#include "cbor.h"
#include "moonbase/ipc/kinds.h"
#include "moonrock_display.h"
#include "moonrock_shaders.h"
#include "moonbase_chrome.h"
#include "moonbase_xdnd.h"
#include "wm.h"   // TITLEBAR_HEIGHT, BORDER_WIDTH, BUTTON_* (point-space)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <GL/gl.h>
#include <X11/Xatom.h>

static mb_server_t *g_server = NULL;
static char        *g_default_path = NULL;   // owned, only used when we built it
static uint32_t     g_next_window_id = 1;    // opaque window_id counter

// Borrowed X handles used for the per-surface InputOnly click proxies.
// NULL dpy means no proxies are created (test / headless mode) and
// pointer-based close / focus-on-click won't fire — the key-routing
// path still works fine.
static Display     *g_dpy  = NULL;
static Window       g_root = 0;

// Focused MoonBase window. 0 means no MoonBase surface currently has the
// input focus — either because nothing has ever focused (pre-first-
// window), because every MoonBase window was closed, or because focus is
// currently on a reparented X window and the shell has dispatched it
// elsewhere. Set from handle_window_create (new window auto-focuses for
// now — focus-on-click routing is a later slice) and from the focus
// sweep in handle_window_close / surface_sweep_client.
static uint32_t g_focused_window_id = 0;

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

    // Screen placement in physical pixels. 3c.2a uses a simple cascade
    // seeded at create time; future slices let the compositor choose
    // based on focus / cursor / last-used-output / Info.appc hints.
    int            screen_x, screen_y;

    // Pixel-state of the most recently committed frame.
    uint32_t       px_w, px_h;         // physical-pixel size
    uint32_t       stride;             // bytes per row (ARGB32 aligned)
    uint32_t       pixel_format;       // 0 = ARGB32 premultiplied
    uint64_t       commit_count;       // frames received (debug/throughput)

    // Shared-memory backing of the last-committed frame. The memfd
    // itself is owned by the server (closed after the callback returns);
    // we keep our mapping instead — Linux mmap survives fd close because
    // the kernel holds its own reference to the memfd's pages until
    // every mapping unmaps.
    void          *map;                // NULL == no frame yet (or released)
    size_t         map_size;           // size of the mapping in bytes
    bool           has_frame;          // any commit has arrived
    bool           dirty;              // new commit since last GL upload

    // GL texture that mirrors the current shm buffer. Lazy-created on
    // the first render. When the surface's pixel dimensions change we
    // re-glTexImage2D; otherwise we glTexSubImage2D on each dirty flip.
    // Deleted via the pending-delete list so glDeleteTextures is only
    // ever called from mb_host_render, where the GL context is current.
    GLuint         tex;                // 0 == no texture yet
    uint32_t       tex_w, tex_h;       // dimensions last uploaded to tex

    // Aqua chrome (title bar, borders, traffic lights) painted into a
    // CPU-side Cairo surface and uploaded to its own GL texture. Managed
    // by moonbase_chrome.{h,c}. chrome_stale is set whenever content
    // dimensions, scale, title, or focus change and the chrome needs to
    // be re-painted and re-uploaded. chrome_uploaded_revision is the
    // last chrome.revision we copied into chrome.tex.
    mb_chrome_t    chrome;
    bool           chrome_stale;
    uint64_t       chrome_uploaded_revision;

    // InputOnly X window parked at the same physical-pixel footprint as
    // the composited chrome. X routes ButtonPress events on the proxy to
    // moonrock, which hit-tests against chrome regions (close button,
    // title-bar drag, content passthrough). 0 == no proxy (headless
    // init). chrome_w_px / chrome_h_px cache the proxy's current size in
    // physical pixels so we only reconfigure the X window when it
    // actually changes.
    Window         input_proxy;
    uint32_t       chrome_w_px, chrome_h_px;

    // Traffic-light hover / press state. buttons_hover == true while the
    // pointer is anywhere in the three-button region — triggers ALL
    // three glyphs (matches SL 10.6). pressed_button is 0 for none, or
    // 1/2/3 for close/minimize/zoom. Both feed mb_chrome_repaint and
    // cause a chrome re-raster when they change.
    bool           buttons_hover;
    int            pressed_button;
} mb_surface_t;

static mb_surface_t g_surfaces[MB_MAX_SURFACES];
static int          g_surface_count = 0;

// Pending GL texture deletes. surface_release may run from any path the
// event loop reaches (including DISCONNECTED inside mb_host_tick, where
// the GL context is NOT guaranteed current). We never call
// glDeleteTextures directly from those paths — we push here, and
// mb_host_render drains the queue while the GL context is current.
static GLuint       g_pending_delete[MB_MAX_SURFACES];
static int          g_pending_delete_count = 0;

// Adapter so moonbase_chrome.c can enqueue GL deletion without knowing
// about our pending-delete list. Matches mb_chrome_release's defer_gl_delete
// callback signature.
static void chrome_defer_gl_delete(GLuint tex);

static void pending_delete_push(GLuint tex) {
    if (tex == 0) return;
    if (g_pending_delete_count >= MB_MAX_SURFACES) {
        // Queue is sized to match the surface table, so overflow means
        // something is structurally wrong — leaking one texture is a
        // strictly better outcome than a silent OOB write.
        fprintf(stderr,
                "[moonrock] moonbase pending-delete queue overflow — "
                "leaking texture %u\n", tex);
        return;
    }
    g_pending_delete[g_pending_delete_count++] = tex;
}

static void chrome_defer_gl_delete(GLuint tex) {
    pending_delete_push(tex);
}

// Cascade placement for a new surface. The menu bar is ~22 points tall
// and windows should open clear of it; at scale 1.5 that's ~33 px, so
// seed the cascade at y=60 to stay clear at every sensible scale. The
// modulo keeps us from marching off the right edge of a small panel.
static void surface_assign_cascade(mb_surface_t *s) {
    static int slot = 0;
    s->screen_x = 80 + (slot % 8) * 30;
    s->screen_y = 60 + (slot % 8) * 30;
    slot++;
}

static mb_surface_t *surface_alloc(void) {
    for (int i = 0; i < MB_MAX_SURFACES; i++) {
        if (!g_surfaces[i].in_use) {
            memset(&g_surfaces[i], 0, sizeof(g_surfaces[i]));
            g_surfaces[i].in_use = true;
            g_surface_count++;
            surface_assign_cascade(&g_surfaces[i]);
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
    if (s->input_proxy && g_dpy) {
        // If a drag session was in flight against this proxy, tell the
        // XDND bridge to drop its state before the XID goes away —
        // otherwise it could try to XSendEvent to a dead window.
        mb_xdnd_forget_window(s->input_proxy);
        // XDestroyWindow is a one-way request; the server free-lists the
        // XID. No reply check — a dead display handle would already have
        // broken the WM loop.
        XDestroyWindow(g_dpy, s->input_proxy);
        s->input_proxy = 0;
    }
    if (s->map) {
        munmap(s->map, s->map_size);
    }
    pending_delete_push(s->tex);
    mb_chrome_release(&s->chrome, chrome_defer_gl_delete);
    free(s->title);
    memset(s, 0, sizeof(*s));
    g_surface_count--;
}

// Compute the chrome's physical-pixel footprint from the client-declared
// points size plus the current backing scale. This is the initial /
// pre-commit estimate we use to size the InputOnly proxy. Once a commit
// arrives, handle_window_commit recomputes from real content pixels and
// reconfigures the proxy if the footprint changed.
//
// Must match the math in moonbase_chrome.c's layout so the click region
// lines up with the painted pixels.
static void chrome_px_from_points(int points_w, int points_h, float scale,
                                  uint32_t *out_w, uint32_t *out_h) {
    int content_w_px = (int)(points_w * scale + 0.5f);
    int content_h_px = (int)(points_h * scale + 0.5f);
    int titlebar_px  = (int)(TITLEBAR_HEIGHT * scale + 0.5f);
    // Chrome has no side/bottom inset (SL 10.6 ground truth — see
    // mb_chrome_repaint for the full note). The 1-px hairline is
    // overlaid on the content's outer pixels, so the proxy footprint
    // equals content_w × (content_h + titlebar).
    *out_w = (uint32_t)content_w_px;
    *out_h = (uint32_t)(content_h_px + titlebar_px);
}

// Create the InputOnly click proxy for a newly-created surface.
// Returns the proxy XID, or 0 if proxies are disabled (headless init).
static Window create_input_proxy(int screen_x, int screen_y,
                                 uint32_t chrome_w, uint32_t chrome_h) {
    if (!g_dpy || g_root == 0) return 0;
    XSetWindowAttributes attr;
    // Motion + Leave are needed for traffic-light hover glyphs; the
    // proxy is the only window receiving pointer events in the
    // MoonBase-chrome region. Button Press/Release route to
    // mb_host_handle_button_press / _release.
    attr.event_mask        = ButtonPressMask | ButtonReleaseMask
                           | PointerMotionMask | LeaveWindowMask;
    // override_redirect keeps the WM's MapRequest handler from framing
    // this as a client window. It is a helper, not a client.
    attr.override_redirect = True;
    Window w = XCreateWindow(g_dpy, g_root,
                             screen_x, screen_y,
                             chrome_w ? chrome_w : 1,
                             chrome_h ? chrome_h : 1,
                             0,                 // border width
                             0,                 // depth (CopyFromParent for InputOnly)
                             InputOnly,
                             CopyFromParent,
                             CWEventMask | CWOverrideRedirect,
                             &attr);
    if (!w) return 0;
    // Advertise XDND support BEFORE mapping. GTK and Qt sources scan
    // XdndAware at the instant the pointer enters a drop target; if
    // the property is missing at that moment they skip the target for
    // the rest of the drag session.
    Atom xdnd_aware = XInternAtom(g_dpy, "XdndAware", False);
    unsigned long version = 5;  // we support XDND protocol v5
    XChangeProperty(g_dpy, w, xdnd_aware, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&version, 1);
    // Raise so the proxy sits above reparented X client windows — GL
    // composites MoonBase surfaces on top visually, and the click
    // region has to agree with the pixels.
    XMapRaised(g_dpy, w);
    return w;
}

// ─────────────────────────────────────────────────────────────────────
// Focus + input routing
// ─────────────────────────────────────────────────────────────────────
//
// Each focus transition sends MB_IPC_WINDOW_FOCUSED to the affected
// surface's client — `focused=false` to the outgoing surface first,
// then `focused=true` to the incoming one. Only one MoonBase surface
// holds focus at a time; routing to X windows is handled by the rest
// of CCWM and is invisible to this layer.

// Send WINDOW_FOCUSED to the client owning `surf`.
static void send_focus_event(mb_surface_t *surf, bool focused) {
    if (!surf || !g_server) return;
    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 16);
    mb_cbor_w_map_begin(&w, 2);
    mb_cbor_w_key(&w, 1); mb_cbor_w_uint(&w, surf->window_id);
    mb_cbor_w_key(&w, 2); mb_cbor_w_bool(&w, focused);
    if (!mb_cbor_w_ok(&w)) {
        mb_cbor_w_drop(&w);
        return;
    }
    size_t len = 0;
    uint8_t *body = mb_cbor_w_finish(&w, &len);
    if (body) {
        (void)mb_server_send(g_server, surf->client,
                             MB_IPC_WINDOW_FOCUSED,
                             body, len, NULL, 0);
        free(body);
    }
}

// Transition focus to the given window_id (0 == no MoonBase focus).
// Emits the outgoing-false + incoming-true pair of events as needed.
static void focus_set(uint32_t new_id) {
    if (g_focused_window_id == new_id) return;

    if (g_focused_window_id != 0) {
        mb_surface_t *prev = surface_find(g_focused_window_id);
        if (prev) send_focus_event(prev, false);
    }
    g_focused_window_id = new_id;
    if (new_id != 0) {
        mb_surface_t *next = surface_find(new_id);
        if (next) send_focus_event(next, true);
    }
}

// Find the newest live surface (highest window_id) to take focus when
// the currently-focused one goes away. Returns 0 if the table is empty.
static uint32_t focus_pick_successor(void) {
    uint32_t best = 0;
    for (int i = 0; i < MB_MAX_SURFACES; i++) {
        if (g_surfaces[i].in_use
                && g_surfaces[i].window_id != g_focused_window_id
                && g_surfaces[i].window_id > best) {
            best = g_surfaces[i].window_id;
        }
    }
    return best;
}

// Sweep every surface belonging to a given client. Called when that
// client disconnects (graceful BYE or abrupt EOF both route here).
static void surface_sweep_client(mb_client_id_t client) {
    int  freed = 0;
    bool lost_focus = false;
    for (int i = 0; i < MB_MAX_SURFACES; i++) {
        if (g_surfaces[i].in_use && g_surfaces[i].client == client) {
            if (g_surfaces[i].window_id == g_focused_window_id) {
                lost_focus = true;
            }
            surface_release(&g_surfaces[i]);
            freed++;
        }
    }
    if (lost_focus) {
        // The focused client is gone — don't try to send a "lost focus"
        // event to it. Pick a successor silently and only announce the
        // "gained focus" side.
        g_focused_window_id = 0;
        focus_set(focus_pick_successor());
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
    surf->chrome_stale = true;      // first-paint trigger for mb_host_render

    // Compute chrome footprint from the declared points size and build
    // an InputOnly proxy so clicks on the chrome reach moonrock.
    chrome_px_from_points(surf->points_w, surf->points_h, surf->scale,
                          &surf->chrome_w_px, &surf->chrome_h_px);
    surf->input_proxy = create_input_proxy(surf->screen_x, surf->screen_y,
                                           surf->chrome_w_px,
                                           surf->chrome_h_px);

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
        return;
    }

    // Newly-created MoonBase windows auto-focus until click-to-focus and
    // X-focus arbitration land. Sends a "lost focus" to any previous
    // MoonBase surface and a "gained focus" to this one.
    focus_set(window_id);
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
    bool was_focused = (window_id == g_focused_window_id);
    surface_release(surf);
    if (was_focused) {
        // App closed its own focused window — no "lost focus" event
        // needed (the window_id is gone). Pick a successor.
        g_focused_window_id = 0;
        focus_set(focus_pick_successor());
    }
}

// Parse a WINDOW_COMMIT body. Schema:
//   { 1: window_id, 2: width_px, 3: height_px, 4: stride_bytes,
//     5: pixel_format, 6: damage_x, 7: damage_y,
//     8: damage_w, 9: damage_h }
// Unknown keys are tolerated — schema can grow without wire-format churn.
typedef struct {
    uint32_t window_id;
    uint32_t width_px, height_px;
    uint32_t stride;
    uint32_t pixel_format;
    uint32_t damage_x, damage_y, damage_w, damage_h;
    bool     have_id, have_w, have_h, have_stride;
} window_commit_req_t;

static bool parse_window_commit(const uint8_t *body, size_t body_len,
                                window_commit_req_t *out) {
    memset(out, 0, sizeof(*out));
    mb_cbor_r_t r;
    mb_cbor_r_init(&r, body, body_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs)) return false;
    for (uint64_t i = 0; i < pairs; i++) {
        uint64_t key = 0;
        if (!mb_cbor_r_uint(&r, &key)) return false;
        uint64_t v = 0;
        switch (key) {
            case 1: if (!mb_cbor_r_uint(&r, &v)) return false;
                    out->window_id = (uint32_t)v; out->have_id = true; break;
            case 2: if (!mb_cbor_r_uint(&r, &v)) return false;
                    out->width_px = (uint32_t)v; out->have_w = true; break;
            case 3: if (!mb_cbor_r_uint(&r, &v)) return false;
                    out->height_px = (uint32_t)v; out->have_h = true; break;
            case 4: if (!mb_cbor_r_uint(&r, &v)) return false;
                    out->stride = (uint32_t)v; out->have_stride = true; break;
            case 5: if (!mb_cbor_r_uint(&r, &v)) return false;
                    out->pixel_format = (uint32_t)v; break;
            case 6: if (!mb_cbor_r_uint(&r, &v)) return false;
                    out->damage_x = (uint32_t)v; break;
            case 7: if (!mb_cbor_r_uint(&r, &v)) return false;
                    out->damage_y = (uint32_t)v; break;
            case 8: if (!mb_cbor_r_uint(&r, &v)) return false;
                    out->damage_w = (uint32_t)v; break;
            case 9: if (!mb_cbor_r_uint(&r, &v)) return false;
                    out->damage_h = (uint32_t)v; break;
            default:
                if (!mb_cbor_r_skip(&r)) return false;
                break;
        }
    }
    return out->have_id && out->have_w && out->have_h && out->have_stride;
}

// Handle a WINDOW_COMMIT from a client. The client has rendered a frame
// into an shm-backed buffer and is handing us the memfd. We validate the
// fd, mmap it, and keep the mapping alive for mb_host_render to upload
// on the next frame.
//
// Ownership: the server closes the passed-in fd after the callback
// returns. That's fine because Linux mmap keeps its own reference to the
// underlying memfd's pages — the mapping survives the fd close. We only
// lose access when we munmap (surface_release or next commit).
//
// If a previous frame's mapping was still live, we drop it here before
// installing the new one. The GL texture is kept across commits — it's
// only re-uploaded (or resized via glTexImage2D) on the next render.
static void handle_window_commit(mb_client_id_t client,
                                 const uint8_t *body, size_t body_len,
                                 const int *fds, size_t nfds) {
    window_commit_req_t req;
    if (!parse_window_commit(body, body_len, &req)) {
        fprintf(stderr,
                "[moonrock] moonbase client %u sent malformed WINDOW_COMMIT\n",
                client);
        return;
    }
    if (nfds != 1 || !fds) {
        fprintf(stderr,
                "[moonrock] moonbase client %u WINDOW_COMMIT window_id=%u: "
                "expected 1 fd, got %zu\n", client, req.window_id, nfds);
        return;
    }
    mb_surface_t *surf = surface_find(req.window_id);
    if (!surf) {
        fprintf(stderr,
                "[moonrock] moonbase client %u WINDOW_COMMIT: window_id=%u "
                "not found (ignoring)\n", client, req.window_id);
        return;
    }
    if (surf->client != client) {
        fprintf(stderr,
                "[moonrock] moonbase client %u WINDOW_COMMIT: window_id=%u "
                "belongs to client %u — refusing\n",
                client, req.window_id, surf->client);
        return;
    }

    int fd = fds[0];

    // Sanity-check the fd: must be sizeable and match stride*height.
    // An oversized buffer is tolerated (the app may have allocated a
    // larger backing store than it's committing), but an undersized
    // one is rejected because mapping past EOF segfaults readers.
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        fprintf(stderr,
                "[moonrock] moonbase client %u WINDOW_COMMIT window_id=%u: "
                "fstat failed\n", client, req.window_id);
        return;
    }
    size_t need = (size_t)req.stride * (size_t)req.height_px;
    if ((size_t)st.st_size < need) {
        fprintf(stderr,
                "[moonrock] moonbase client %u WINDOW_COMMIT window_id=%u: "
                "buffer too small (%zu bytes, need %zu)\n",
                client, req.window_id, (size_t)st.st_size, need);
        return;
    }

    // Map the new buffer read-only. MAP_SHARED so we see any future
    // writes the client makes — in practice the client does one draw
    // and unmaps, so the buffer contents are stable by the time we
    // upload, but MAP_SHARED is the semantically correct mode for a
    // buffer whose ownership is about to be shared.
    void *p = mmap(NULL, need, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr,
                "[moonrock] moonbase client %u WINDOW_COMMIT window_id=%u: "
                "mmap failed\n", client, req.window_id);
        return;
    }

    // Drop the old mapping only after the new one is installed — this
    // ensures mb_host_render can run between two commits without racing
    // on a half-installed surface.
    void  *old_map  = surf->map;
    size_t old_size = surf->map_size;

    // Chrome wraps the content at whatever pixel dimensions the client
    // last committed. If those dimensions changed, the chrome's outer
    // rectangle is now wrong — repaint. First-commit also trips this
    // branch because previous px_w/px_h are zero.
    if (surf->px_w != req.width_px || surf->px_h != req.height_px) {
        surf->chrome_stale = true;

        // Re-derive the proxy footprint from the real content pixels
        // (more accurate than the pre-commit points-×-scale estimate)
        // and resize the InputOnly proxy if it changed. Matches
        // chrome_px_from_points / mb_chrome_repaint: no side or bottom
        // inset — just content × (content + titlebar).
        int titlebar_px = (int)(TITLEBAR_HEIGHT * surf->scale + 0.5f);
        uint32_t new_cw = req.width_px;
        uint32_t new_ch = req.height_px + (uint32_t)titlebar_px;
        if (surf->input_proxy && g_dpy
                && (new_cw != surf->chrome_w_px
                    || new_ch != surf->chrome_h_px)) {
            XMoveResizeWindow(g_dpy, surf->input_proxy,
                              surf->screen_x, surf->screen_y,
                              new_cw, new_ch);
        }
        surf->chrome_w_px = new_cw;
        surf->chrome_h_px = new_ch;
    }

    surf->map          = p;
    surf->map_size     = need;
    surf->px_w         = req.width_px;
    surf->px_h         = req.height_px;
    surf->stride       = req.stride;
    surf->pixel_format = req.pixel_format;
    surf->has_frame    = true;
    surf->dirty        = true;
    surf->commit_count++;

    if (old_map) {
        munmap(old_map, old_size);
    }

    fprintf(stderr,
            "[moonrock] moonbase client %u WINDOW_COMMIT window_id=%u: "
            "%ux%u stride=%u fmt=%u damage=(%u,%u %ux%u) frame#%llu\n",
            client, req.window_id, req.width_px, req.height_px,
            req.stride, req.pixel_format,
            req.damage_x, req.damage_y, req.damage_w, req.damage_h,
            (unsigned long long)surf->commit_count);
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
            mb_consent_responder_note_connected(ev->client,
                                                ev->hello.bundle_id);
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
                case MB_IPC_WINDOW_COMMIT:
                    handle_window_commit(ev->client,
                                         ev->frame_body, ev->frame_body_len,
                                         ev->frame_fds, ev->frame_fd_count);
                    break;
                case MB_IPC_CONSENT_REQUEST:
                    mb_consent_responder_handle_request(
                        s, ev->client,
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
            mb_consent_responder_note_disconnected(ev->client);
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

bool mb_host_init(const char *path, Display *dpy, Window root) {
    if (g_server) {
        fprintf(stderr, "[moonrock] moonbase host already running\n");
        return true;
    }

    g_dpy  = dpy;
    g_root = root;

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
    // Boot the consent responder. Failure is logged but non-fatal —
    // the host runs happily without lazy consent, it just replies
    // ERROR to every CONSENT_REQUEST until the responder is fixed.
    int crc = mb_consent_responder_init(NULL);
    if (crc != 0) {
        fprintf(stderr,
                "[moonrock] mb_consent_responder_init failed: %d\n", crc);
    }
    fprintf(stderr, "[moonrock] moonbase host listening on %s\n", use_path);
    return true;
}

size_t mb_host_collect_pollfds(struct pollfd *out_fds, size_t max) {
    if (!g_server || !out_fds || max == 0) return 0;
    size_t n = mb_server_get_pollfds(g_server, out_fds, max);
    if (n < max) {
        n += mb_consent_responder_collect_pollfds(out_fds + n, max - n);
    }
    return n;
}

// ─────────────────────────────────────────────────────────────────────
// Scale migration
// ─────────────────────────────────────────────────────────────────────
//
// A MoonBase surface's "host output" is the output that contains the
// largest share of its chrome rectangle — Per-Monitor DPI v2 style. When
// that output's scale factor differs from what the client last saw, we
// push MB_IPC_BACKING_SCALE_CHANGED so the client can re-allocate its
// Cairo surface at the new physical-pixel size before the next frame.
//
// The check runs on every mb_host_tick — cost is O(live_surfaces ×
// connected_outputs), both tiny, and no syscalls unless a change is
// detected. Triggers in practice:
//   * Output hotplug (current host disconnects → migrate to primary)
//   * User changes an output's scale in SysPrefs → Displays
//   * (Future slice) user drags a window between outputs

// Axis-aligned rectangle intersection area. Returns 0 when rects are
// disjoint.
static long rect_intersection_area(int ax, int ay, int aw, int ah,
                                   int bx, int by, int bw, int bh) {
    int x0 = ax > bx ? ax : bx;
    int y0 = ay > by ? ay : by;
    int x1 = (ax + aw) < (bx + bw) ? (ax + aw) : (bx + bw);
    int y1 = (ay + ah) < (by + bh) ? (ay + ah) : (by + bh);
    if (x1 <= x0 || y1 <= y0) return 0;
    return (long)(x1 - x0) * (long)(y1 - y0);
}

// Pick which output the surface lives on. NULL means no connected
// output intersects its rectangle — happens only during hotplug
// transients, in which case the caller falls back to primary.
static MROutput *pick_target_output(const mb_surface_t *s) {
    int count = 0;
    MROutput *outs = display_get_outputs(&count);
    if (!outs || count <= 0) return NULL;

    MROutput *best      = NULL;
    long      best_area = 0;
    for (int i = 0; i < count; i++) {
        long area = rect_intersection_area(
            s->screen_x, s->screen_y,
            (int)s->chrome_w_px, (int)s->chrome_h_px,
            outs[i].x, outs[i].y, outs[i].width, outs[i].height);
        if (area > best_area) {
            best_area = area;
            best      = &outs[i];
        }
    }
    return best;
}

// Build and send MB_IPC_BACKING_SCALE_CHANGED:
//   { 1: window_id, 2: float old_scale, 3: float new_scale,
//     4: uint output_id }
static void emit_backing_scale_changed(mb_surface_t *s,
                                       float old_scale, float new_scale,
                                       uint32_t output_id) {
    if (!g_server) return;
    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 32);
    mb_cbor_w_map_begin(&w, 4);
    mb_cbor_w_key(&w, 1); mb_cbor_w_uint (&w, s->window_id);
    mb_cbor_w_key(&w, 2); mb_cbor_w_float(&w, (double)old_scale);
    mb_cbor_w_key(&w, 3); mb_cbor_w_float(&w, (double)new_scale);
    mb_cbor_w_key(&w, 4); mb_cbor_w_uint (&w, output_id);
    if (!mb_cbor_w_ok(&w)) { mb_cbor_w_drop(&w); return; }
    size_t len = 0;
    uint8_t *body = mb_cbor_w_finish(&w, &len);
    if (!body) return;
    (void)mb_server_send(g_server, s->client,
                         MB_IPC_BACKING_SCALE_CHANGED,
                         body, len, NULL, 0);
    free(body);
}

static void mb_host_check_scale_migration(void) {
    if (g_surface_count == 0) return;
    for (int i = 0; i < MB_MAX_SURFACES; i++) {
        mb_surface_t *s = &g_surfaces[i];
        if (!s->in_use) continue;

        MROutput *target = pick_target_output(s);
        if (!target) {
            // Surface geometry doesn't intersect any known output — fall
            // back to primary so the client doesn't stall on an output
            // id that no longer exists.
            target = display_get_primary();
            if (!target) continue;
        }

        float    new_scale = display_get_scale_for_output(target);
        uint32_t new_out   = (uint32_t)target->output_id;
        if (new_scale == s->scale && new_out == s->output_id) continue;

        fprintf(stderr,
                "[moonrock] moonbase window_id=%u scale migration: "
                "%.2f (output %u) → %.2f (output %u)\n",
                s->window_id, (double)s->scale, s->output_id,
                (double)new_scale, new_out);

        emit_backing_scale_changed(s, s->scale, new_scale, new_out);
        s->scale        = new_scale;
        s->output_id    = new_out;
        s->chrome_stale = true;   // chrome must be repainted at the new scale
    }
}

void mb_host_tick(const struct pollfd *fds, size_t nfds) {
    if (!g_server) return;
    mb_server_tick(g_server, fds, nfds);
    // Same poll set covers consent helper pidfds. The responder
    // walks it and only touches fds it recognizes, so running both
    // ticks on the same slice is safe.
    mb_consent_responder_tick(g_server, fds, nfds);
    // Runs once per main-loop iteration. Catches output-scale changes
    // and hotplug-induced migrations before the next composite pass.
    mb_host_check_scale_migration();
}

bool mb_host_has_focus(void) {
    if (g_focused_window_id == 0) return false;
    return surface_find(g_focused_window_id) != NULL;
}

// Timestamp for outgoing input events. Monotonic microseconds since
// boot — matches mb_event_t.timestamp_us semantics on the client side.
static uint64_t ts_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

// Send MB_IPC_WINDOW_CLOSED to `surf`'s owning client so the app's
// event loop can observe the close request and decide how to respond
// (save-on-quit, put up a confirmation sheet, etc.). We do NOT tear the
// surface down here — that waits for the client to send back
// MB_IPC_WINDOW_CLOSE or to disconnect.
static void send_window_closed_event(mb_surface_t *surf) {
    if (!surf || !g_server) return;
    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 16);
    mb_cbor_w_map_begin(&w, 1);
    mb_cbor_w_key(&w, 1); mb_cbor_w_uint(&w, surf->window_id);
    if (!mb_cbor_w_ok(&w)) { mb_cbor_w_drop(&w); return; }
    size_t len = 0;
    uint8_t *body = mb_cbor_w_finish(&w, &len);
    if (!body) return;
    (void)mb_server_send(g_server, surf->client,
                         MB_IPC_WINDOW_CLOSED,
                         body, len, NULL, 0);
    free(body);
}

// Locate the surface whose input_proxy is `win`. Linear scan — the
// table is small (≤256 entries) and click events are infrequent.
static mb_surface_t *surface_find_by_proxy(Window win) {
    if (win == 0) return NULL;
    for (int i = 0; i < MB_MAX_SURFACES; i++) {
        if (g_surfaces[i].in_use && g_surfaces[i].input_proxy == win) {
            return &g_surfaces[i];
        }
    }
    return NULL;
}

// Hit-test the three traffic-light buttons in a chrome rect at the
// current scale. Returns 1 for close, 2 for minimize, 3 for zoom, or 0
// when (x, y) misses every disc. x/y are proxy-relative pixel
// coordinates (i.e. chrome-relative, since the proxy and chrome share
// an origin). Mirrors hit_test_button in events.c so MoonBase and
// X-client windows have identical click regions.
static int chrome_hit_button(mb_surface_t *s, int x, int y) {
    int left_pad = (int)(BUTTON_LEFT_PAD  * s->scale + 0.5f);
    int top_pad  = (int)(BUTTON_TOP_PAD   * s->scale + 0.5f);
    int diameter = (int)(BUTTON_DIAMETER  * s->scale + 0.5f);
    int spacing  = (int)(BUTTON_SPACING   * s->scale + 0.5f);
    if (diameter < 1) diameter = 1;
    if (y < top_pad || y > top_pad + diameter) return 0;

    int bx = left_pad;
    if (x >= bx && x <= bx + diameter) return 1;
    bx += diameter + spacing;
    if (x >= bx && x <= bx + diameter) return 2;
    bx += diameter + spacing;
    if (x >= bx && x <= bx + diameter) return 3;
    return 0;
}

// Wider "button region" hit test — the bounding box around all three
// discs. Hovering anywhere inside reveals the glyphs on ALL three
// buttons, matching SL 10.6.
static bool chrome_hit_button_region(mb_surface_t *s, int x, int y) {
    int left_pad = (int)(BUTTON_LEFT_PAD  * s->scale + 0.5f);
    int top_pad  = (int)(BUTTON_TOP_PAD   * s->scale + 0.5f);
    int diameter = (int)(BUTTON_DIAMETER  * s->scale + 0.5f);
    int spacing  = (int)(BUTTON_SPACING   * s->scale + 0.5f);
    if (diameter < 1) diameter = 1;
    int region_left  = left_pad;
    int region_right = left_pad + 3 * diameter + 2 * spacing;
    return y >= top_pad && y <= top_pad + diameter
        && x >= region_left && x <= region_right;
}

bool mb_host_handle_button_press(Window win, int x, int y, unsigned int button) {
    mb_surface_t *surf = surface_find_by_proxy(win);
    if (!surf) return false;

    // Any click on a MoonBase surface's chrome or content passes focus
    // to that surface — matches X click-to-focus behavior on reparented
    // clients, which also run through wm_focus_client in on_button_press.
    focus_set(surf->window_id);

    // Left-click only for button-dispatch in this slice. Other buttons
    // on the chrome are a no-op but still counted as consumed so the WM
    // doesn't double-handle them.
    if (button != Button1) return true;

    int btn = chrome_hit_button(surf, x, y);
    if (btn > 0) {
        // Press feedback — set pressed_button, show hover glyphs on all
        // three, re-raster chrome. Grab the pointer so we still get the
        // ButtonRelease even if the user drags off the window before
        // letting go (matches decor.c's on_button_press grab).
        surf->pressed_button = btn;
        surf->buttons_hover  = true;
        surf->chrome_stale   = true;

        if (g_dpy && surf->input_proxy) {
            XGrabPointer(g_dpy, surf->input_proxy, True,
                         PointerMotionMask | ButtonReleaseMask |
                         LeaveWindowMask,
                         GrabModeAsync, GrabModeAsync,
                         None, None, CurrentTime);
        }
        return true;
    }

    // Rest of chrome / content is consumed-without-routing in this
    // slice. Drag-to-move and content pointer events land in later
    // slices.
    return true;
}

bool mb_host_handle_button_release(Window win, int x, int y,
                                   unsigned int button) {
    mb_surface_t *surf = surface_find_by_proxy(win);
    if (!surf) return false;
    if (button != Button1) return true;
    if (surf->pressed_button == 0) return true;

    int pressed = surf->pressed_button;
    int over    = chrome_hit_button(surf, x, y);

    surf->pressed_button = 0;
    surf->chrome_stale   = true;

    if (g_dpy) XUngrabPointer(g_dpy, CurrentTime);

    // Update hover after the grab ends — if the release happened inside
    // the button region, glyphs should keep showing; if the user dragged
    // off, clear. chrome_hit_button_region does the correct test.
    surf->buttons_hover = chrome_hit_button_region(surf, x, y);

    // Fire the action only if the release was on the SAME button that
    // was originally pressed — matches SL 10.6 click-and-release.
    if (over == pressed) {
        switch (pressed) {
        case 1:  // close
            fprintf(stderr,
                    "[moonrock] moonbase close on window_id=%u\n",
                    surf->window_id);
            send_window_closed_event(surf);
            break;
        case 2:  // minimize — no IPC verb yet; press feedback only
            fprintf(stderr,
                    "[moonrock] moonbase minimize on window_id=%u "
                    "(no action wired)\n", surf->window_id);
            break;
        case 3:  // zoom — no IPC verb yet; press feedback only
            fprintf(stderr,
                    "[moonrock] moonbase zoom on window_id=%u "
                    "(no action wired)\n", surf->window_id);
            break;
        }
    }
    return true;
}

bool mb_host_handle_motion(Window win, int x, int y) {
    mb_surface_t *surf = surface_find_by_proxy(win);
    if (!surf) return false;

    bool now_hover = chrome_hit_button_region(surf, x, y);
    if (now_hover != surf->buttons_hover) {
        surf->buttons_hover = now_hover;
        surf->chrome_stale  = true;
    }
    // While a button is pressed, the disc-darkening only shows when the
    // pointer is still over the originally-pressed button. chrome
    // repaint reads pressed_button directly and doesn't need a hit-test
    // result stored here — we just need a re-raster whenever the
    // pointer crosses the button boundary during the press.
    if (surf->pressed_button > 0) {
        surf->chrome_stale = true;
    }
    return true;
}

bool mb_host_handle_leave(Window win) {
    mb_surface_t *surf = surface_find_by_proxy(win);
    if (!surf) return false;

    if (surf->buttons_hover) {
        surf->buttons_hover = false;
        surf->chrome_stale  = true;
    }
    return true;
}

// Populate the per-surface snapshot the XDND module uses to translate
// root coordinates into content-local points and to address the IPC
// client. Returns false if `win` is not a live MoonBase proxy.
bool mb_host_find_proxy_surface(Window win, mb_host_proxy_info_t *out) {
    if (!out) return false;
    mb_surface_t *s = surface_find_by_proxy(win);
    if (!s) return false;

    // Chrome insets must match moonbase_chrome.c's layout. The content
    // rect sits at (0, titlebar_px) inside the chrome — no side inset
    // (the 1-px hairline is an overlay, not reserved chrome space) —
    // and spans the full committed px_w × px_h. Before the first
    // commit px_w / px_h are zero — the caller treats that case as
    // "no content rect yet" and sends LEAVE / skips OVER.
    int titlebar_px = (int)(TITLEBAR_HEIGHT * s->scale + 0.5f);

    out->window_id = s->window_id;
    out->client    = (uint32_t)s->client;
    out->screen_x  = s->screen_x;
    out->screen_y  = s->screen_y;
    out->content_x = s->screen_x;
    out->content_y = s->screen_y + titlebar_px;
    out->content_w = s->px_w;
    out->content_h = s->px_h;
    out->scale     = s->scale;
    return true;
}

bool mb_host_emit_drag_frame(uint32_t client,
                             uint16_t kind,
                             uint32_t window_id,
                             int      x,
                             int      y,
                             uint32_t modifiers,
                             const char *const *uris,
                             size_t   uri_count,
                             uint64_t timestamp_us) {
    if (!g_server) return false;
    if (timestamp_us == 0) timestamp_us = ts_us();

    // Map shape per IPC.md §5.3:
    //   ENTER / DROP → { 1, 2, 3, 4, 5, 6 }
    //   OVER         → { 1, 2, 3, 4, 6 }
    //   LEAVE        → { 1, 6 }
    // Unknown kinds are refused here — the caller is the only path
    // that builds these frames so wrong kinds are a bug, not a network
    // condition.
    bool is_enter = (kind == MB_IPC_DRAG_ENTER);
    bool is_drop  = (kind == MB_IPC_DRAG_DROP);
    bool is_over  = (kind == MB_IPC_DRAG_OVER);
    bool is_leave = (kind == MB_IPC_DRAG_LEAVE);
    if (!is_enter && !is_drop && !is_over && !is_leave) return false;

    size_t pairs;
    if (is_leave)                 pairs = 2;
    else if (is_over)             pairs = 5;
    else                          pairs = 6;  // ENTER / DROP

    mb_cbor_w_t w;
    // Rough headroom for map header + small keys + URIs. URIs are the
    // only thing that can push this over 64 bytes; sum them up once.
    size_t cap = 48;
    if (is_enter || is_drop) {
        for (size_t i = 0; i < uri_count; i++) {
            cap += (uris && uris[i] ? strlen(uris[i]) : 0) + 8;
        }
    }
    mb_cbor_w_init_grow(&w, cap);
    mb_cbor_w_map_begin(&w, pairs);
    mb_cbor_w_key(&w, 1); mb_cbor_w_uint(&w, window_id);
    if (!is_leave) {
        mb_cbor_w_key(&w, 2); mb_cbor_w_int (&w, (int64_t)x);
        mb_cbor_w_key(&w, 3); mb_cbor_w_int (&w, (int64_t)y);
        mb_cbor_w_key(&w, 4); mb_cbor_w_uint(&w, modifiers);
    }
    if (is_enter || is_drop) {
        mb_cbor_w_key(&w, 5);
        mb_cbor_w_array_begin(&w, uri_count);
        for (size_t i = 0; i < uri_count; i++) {
            const char *u = (uris && uris[i]) ? uris[i] : "";
            mb_cbor_w_tstr_n(&w, u, strlen(u));
        }
    }
    mb_cbor_w_key(&w, 6); mb_cbor_w_uint(&w, timestamp_us);

    if (!mb_cbor_w_ok(&w)) { mb_cbor_w_drop(&w); return false; }
    size_t len = 0;
    uint8_t *body = mb_cbor_w_finish(&w, &len);
    if (!body) return false;

    int rc = mb_server_send(g_server, (mb_client_id_t)client,
                            kind, body, len, NULL, 0);
    free(body);
    return rc == 0;
}

bool mb_host_route_key(uint32_t keycode, uint32_t modifiers,
                       bool is_down, bool is_repeat) {
    if (!g_server || g_focused_window_id == 0) return false;
    mb_surface_t *surf = surface_find(g_focused_window_id);
    if (!surf) {
        // Focused id points at a gone-away surface. Clear and bail.
        g_focused_window_id = 0;
        return false;
    }

    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 32);
    mb_cbor_w_map_begin(&w, 5);
    mb_cbor_w_key(&w, 1); mb_cbor_w_uint(&w, surf->window_id);
    mb_cbor_w_key(&w, 2); mb_cbor_w_uint(&w, keycode);
    mb_cbor_w_key(&w, 3); mb_cbor_w_uint(&w, modifiers);
    mb_cbor_w_key(&w, 4); mb_cbor_w_bool(&w, is_repeat);
    mb_cbor_w_key(&w, 5); mb_cbor_w_uint(&w, ts_us());
    if (!mb_cbor_w_ok(&w)) {
        mb_cbor_w_drop(&w);
        return false;
    }
    size_t len = 0;
    uint8_t *body = mb_cbor_w_finish(&w, &len);
    if (!body) return false;

    int rc = mb_server_send(g_server, surf->client,
                            is_down ? MB_IPC_KEY_DOWN : MB_IPC_KEY_UP,
                            body, len, NULL, 0);
    free(body);
    return rc == 0;
}

bool mb_host_route_text_input(const char *utf8) {
    if (!g_server || g_focused_window_id == 0) return false;
    if (!utf8 || utf8[0] == '\0') return false;
    mb_surface_t *surf = surface_find(g_focused_window_id);
    if (!surf) {
        g_focused_window_id = 0;
        return false;
    }

    size_t text_len = strlen(utf8);

    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 32 + text_len);
    mb_cbor_w_map_begin(&w, 3);
    mb_cbor_w_key(&w, 1); mb_cbor_w_uint(&w, surf->window_id);
    mb_cbor_w_key(&w, 2); mb_cbor_w_tstr_n(&w, utf8, text_len);
    mb_cbor_w_key(&w, 3); mb_cbor_w_uint(&w, ts_us());
    if (!mb_cbor_w_ok(&w)) {
        mb_cbor_w_drop(&w);
        return false;
    }
    size_t len = 0;
    uint8_t *body = mb_cbor_w_finish(&w, &len);
    if (!body) return false;

    int rc = mb_server_send(g_server, surf->client, MB_IPC_TEXT_INPUT,
                            body, len, NULL, 0);
    free(body);
    return rc == 0;
}

// Drain the deferred-delete queue. Must run with a current GL context,
// which is the contract of mb_host_render.
static void drain_pending_deletes(void) {
    if (g_pending_delete_count == 0) return;
    glDeleteTextures((GLsizei)g_pending_delete_count, g_pending_delete);
    g_pending_delete_count = 0;
}

// Upload the committed shm buffer into a GL texture. Returns true if
// the surface is ready to draw after this call, false on any GL error
// path (in which case the surface simply isn't rendered this frame).
//
// Cairo's CAIRO_FORMAT_ARGB32 is native-endian uint32 per pixel with
// premultiplied alpha. On every CopyCatOS target (x86_64, aarch64, both
// little-endian) that lays out as BGRA bytes, which matches GL_BGRA +
// GL_UNSIGNED_BYTE one-to-one — no swizzling, no shader format shim.
//
// Stride-vs-width: Cairo guarantees stride is a multiple of 4 and at
// least width*4. GL_UNPACK_ROW_LENGTH lets us feed that stride directly
// as a pixel-count so partial-row buffers upload correctly.
static bool surface_upload_texture(mb_surface_t *s) {
    if (!s->has_frame || !s->map) return false;
    if (s->tex == 0) {
        glGenTextures(1, &s->tex);
        if (s->tex == 0) return false;
        glBindTexture(GL_TEXTURE_2D, s->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        // Force a first upload below even if dirty wasn't set (it always
        // is on the commit that created the surface, but belt-and-braces).
        s->tex_w = s->tex_h = 0;
    } else {
        glBindTexture(GL_TEXTURE_2D, s->tex);
    }

    if (!s->dirty && s->tex_w == s->px_w && s->tex_h == s->px_h) {
        return true;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(s->stride / 4));

    if (s->tex_w != s->px_w || s->tex_h != s->px_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     (GLsizei)s->px_w, (GLsizei)s->px_h, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, s->map);
        s->tex_w = s->px_w;
        s->tex_h = s->px_h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        (GLsizei)s->px_w, (GLsizei)s->px_h,
                        GL_BGRA, GL_UNSIGNED_BYTE, s->map);
    }

    // Reset the row-length override so later X-client texture uploads
    // and blur captures don't inherit our setting.
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    s->dirty = false;
    return true;
}

// Make sure the chrome texture mirrors chrome.pixels for this surface.
// Returns true if the chrome is ready to draw. Structurally identical to
// surface_upload_texture but reads from chrome state instead of the shm
// mapping. The revision counter lets us skip the GL upload on frames
// where only the content changed (common case: client animating inside
// a window that hasn't resized or changed focus).
static bool chrome_upload_texture(mb_surface_t *s) {
    mb_chrome_t *c = &s->chrome;
    if (!c->pixels || c->chrome_w == 0 || c->chrome_h == 0) return false;

    if (c->tex == 0) {
        glGenTextures(1, &c->tex);
        if (c->tex == 0) return false;
        glBindTexture(GL_TEXTURE_2D, c->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        c->tex_w = c->tex_h = 0;
    } else {
        glBindTexture(GL_TEXTURE_2D, c->tex);
    }

    if (s->chrome_uploaded_revision == c->revision
            && c->tex_w == c->chrome_w
            && c->tex_h == c->chrome_h) {
        return true;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(c->stride / 4));

    if (c->tex_w != c->chrome_w || c->tex_h != c->chrome_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     (GLsizei)c->chrome_w, (GLsizei)c->chrome_h, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, c->pixels);
        c->tex_w = c->chrome_w;
        c->tex_h = c->chrome_h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        (GLsizei)c->chrome_w, (GLsizei)c->chrome_h,
                        GL_BGRA, GL_UNSIGNED_BYTE, c->pixels);
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    s->chrome_uploaded_revision = c->revision;
    return true;
}

void mb_host_render(GLuint basic_shader, float *projection) {
    drain_pending_deletes();

    if (g_surface_count == 0) return;

    // Premultiplied-alpha blending matches the rest of mr_composite's
    // window pass. mr_composite already sets this mode up for the X
    // clients; we don't touch the global blend state on exit because
    // the caller's pass continues with the same mode.

    for (int i = 0; i < MB_MAX_SURFACES; i++) {
        mb_surface_t *s = &g_surfaces[i];
        if (!s->in_use || !s->has_frame) continue;
        if (s->px_w == 0 || s->px_h == 0)  continue;

        if (!surface_upload_texture(s)) continue;

        // Repaint the chrome if first frame, dimensions changed, or
        // focus / title / scale / hover / pressed changed. active=true
        // for now; real focus routing arrives when pointer + keyboard
        // input hit MoonBase surfaces.
        if (s->chrome_stale) {
            if (mb_chrome_repaint(&s->chrome,
                                  s->px_w, s->px_h, s->scale,
                                  s->title, /*active*/ true,
                                  s->buttons_hover, s->pressed_button)) {
                s->chrome_stale = false;
            }
        }
        bool chrome_ready = chrome_upload_texture(s);

        if (!basic_shader) {
            // No shaders = X-client path is in its fixed-function
            // fallback; skip this frame rather than double-failing.
            continue;
        }

        shaders_use(basic_shader);
        shaders_set_projection(basic_shader, projection);
        shaders_set_texture(basic_shader, 0);
        shaders_set_alpha(basic_shader, 1.0f);

        // Content first, chrome second. Chrome is the same width as
        // content (no horizontal inset); its 1-px hairline at x=0 /
        // x=chrome_w-1 / y=chrome_h-1 must stamp over the content's
        // edge pixels. Drawing chrome second guarantees that — the
        // rest of the chrome's content region is transparent so
        // content reads through unchanged.
        glBindTexture(GL_TEXTURE_2D, s->tex);
        shaders_draw_quad(
            (float)(s->screen_x + (int)s->chrome.content_x_inset),
            (float)(s->screen_y + (int)s->chrome.content_y_inset),
            (float)s->px_w, (float)s->px_h);

        if (chrome_ready) {
            glBindTexture(GL_TEXTURE_2D, s->chrome.tex);
            shaders_draw_quad((float)s->screen_x, (float)s->screen_y,
                              (float)s->chrome.chrome_w,
                              (float)s->chrome.chrome_h);
        }
    }
}

void mb_host_shutdown(void) {
    if (!g_server) return;
    // Release every live surface before closing the server — this also
    // destroys each InputOnly proxy so we don't leak XIDs on the way out.
    for (int i = 0; i < MB_MAX_SURFACES; i++) {
        if (g_surfaces[i].in_use) surface_release(&g_surfaces[i]);
    }
    mb_consent_responder_shutdown();
    mb_server_close(g_server);
    g_server = NULL;
    g_dpy  = NULL;
    g_root = 0;
    free(g_default_path);
    g_default_path = NULL;
}
