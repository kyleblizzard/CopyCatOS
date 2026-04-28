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

#include "moonbase.h"         // MB_MOD_*, MB_BUTTON_*
#include "server.h"
#include "consent_responder.h"
#include "moonbase/ipc/kinds.h"
#include "moonrock_display.h"
#include "moonrock_shaders.h"
#include "host_chrome.h"      // mb_chrome_t, mb_chrome_repaint, mb_chrome_release
#include "host_hittest.h"     // mb_host_chrome_hit_button[_region]
#include "host_protocol.h"    // mb_host_parse_*/build_* CBOR codecs
#include "host_util.h"        // mb_host_default_socket_path / ts_us / rect math
#include "assets.h"           // assets_get_*_button — passed into mb_chrome_repaint
#include "moonbase_xdnd.h"
#include "wm.h"   // TITLEBAR_HEIGHT, BORDER_WIDTH, BUTTON_* (point-space)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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
    // CPU-side Cairo surface (managed by host_chrome.{h,c} as a
    // mb_chrome_t) and uploaded to a GL texture owned by this surface.
    // GL bookkeeping lives here, not in mb_chrome_t, because mb_chrome_t
    // is shared with moonrock-lite (which uploads to an X drawable, not
    // GL). chrome_stale fires whenever dimensions, scale, title, focus,
    // or button hover/press state changes; chrome_uploaded_revision is
    // the last mb_chrome_t.revision we copied into chrome_tex.
    mb_chrome_t    chrome;
    bool           chrome_stale;
    uint64_t       chrome_uploaded_revision;
    GLuint         chrome_tex;             // 0 == no texture yet
    uint32_t       chrome_tex_w, chrome_tex_h;

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

    // Content-rect pointer press tracking. 0 == no content press in
    // flight; otherwise the MB_BUTTON_* value of the button currently
    // held down so the matching POINTER_UP fires for the same button
    // even if the user drags off the proxy before releasing. Last-write-
    // wins — the host only tracks one concurrent button per surface for
    // now (Apple HIG: chord clicks are not first-class anyway).
    int            pointer_btn_down;
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
    pending_delete_push(s->chrome_tex);
    mb_chrome_release(&s->chrome);
    free(s->title);
    memset(s, 0, sizeof(*s));
    g_surface_count--;
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
    // mb_host_handle_button_press / _release. Key + FocusChange let X
    // deliver keystrokes to the focused MoonBase surface — without these
    // KeyPress on root never gets re-routed here, and FocusOut never
    // syncs g_focused_window_id back to 0 when X moves focus away.
    attr.event_mask        = ButtonPressMask | ButtonReleaseMask
                           | PointerMotionMask | LeaveWindowMask
                           | KeyPressMask    | KeyReleaseMask
                           | FocusChangeMask;
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
    size_t len = 0;
    uint8_t *body = mb_host_build_window_focused(surf->window_id, focused, &len);
    if (!body) return;
    (void)mb_server_send(g_server, surf->client,
                         MB_IPC_WINDOW_FOCUSED,
                         body, len, NULL, 0);
    free(body);
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

// Handle a WINDOW_CREATE request. Allocates a window_id and replies.
// Slice 3a stubbed the scale at 1.0; slice 3b (this code) looks up the
// real scale of the target output — primary for now, since we don't yet
// have placement hints on the request. Slice 3c reparents a real X
// window.
static void handle_window_create(mb_server_t *s, mb_client_id_t client,
                                 const uint8_t *body, size_t body_len) {
    mb_host_window_create_req_t req;
    if (!mb_host_parse_window_create(body, body_len, &req)) {
        fprintf(stderr,
                "[moonrock] moonbase client %u sent malformed WINDOW_CREATE\n",
                client);
        mb_host_window_create_req_free(&req);
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
        mb_host_window_create_req_free(&req);
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
    mb_host_chrome_px_from_points(surf->points_w, surf->points_h, surf->scale,
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
    mb_host_window_create_req_free(&req);

    size_t reply_len = 0;
    uint8_t *reply = mb_host_build_window_create_reply(
        window_id,
        output_id,
        (double)init_scale,
        (uint32_t)surf->points_w,
        (uint32_t)surf->points_h,
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

// Handle a client-initiated WINDOW_CLOSE. The app wants to release a
// window it previously created. We drop the surface entry; a future
// slice will emit a damage call so the compositor can stop drawing it.
static void handle_window_close(mb_client_id_t client,
                                const uint8_t *body, size_t body_len) {
    uint32_t window_id = mb_host_parse_window_close_id(body, body_len);
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
    mb_host_window_commit_req_t req;
    if (!mb_host_parse_window_commit(body, body_len, &req)) {
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

    // fstat + size-check + mmap is delegated to the shared host helper so
    // moonrock-lite gets identical validation. The helper logs to stderr
    // with `[mb_host]` context on failure; supplement with one line that
    // names the moonrock client + window_id so the operator can tie the
    // error back to a specific app.
    int    fd   = fds[0];
    void  *p    = NULL;
    size_t need = 0;
    if (!mb_host_validate_and_map_commit_fd(req.stride, req.height_px,
                                            fd, &p, &need)) {
        fprintf(stderr,
                "[moonrock] moonbase client %u WINDOW_COMMIT window_id=%u: "
                "validate/map failed\n", client, req.window_id);
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
        // mb_host_chrome_px_from_points / mb_chrome_repaint: no side or bottom
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

bool mb_host_init(const char *path, Display *dpy, Window root) {
    if (g_server) {
        fprintf(stderr, "[moonrock] moonbase host already running\n");
        return true;
    }

    g_dpy  = dpy;
    g_root = root;

    const char *use_path = path;
    if (!use_path) {
        g_default_path = mb_host_default_socket_path();
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
        long area = mb_host_rect_intersection_area(
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
    size_t len = 0;
    uint8_t *body = mb_host_build_backing_scale_changed(
        s->window_id, old_scale, new_scale, output_id, &len);
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

// Send MB_IPC_WINDOW_CLOSED to `surf`'s owning client so the app's
// event loop can observe the close request and decide how to respond
// (save-on-quit, put up a confirmation sheet, etc.). We do NOT tear the
// surface down here — that waits for the client to send back
// MB_IPC_WINDOW_CLOSE or to disconnect.
static void send_window_closed_event(mb_surface_t *surf) {
    if (!surf || !g_server) return;
    size_t len = 0;
    uint8_t *body = mb_host_build_window_closed(surf->window_id, &len);
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

// X11 button number → MB_BUTTON_*. X numbers 1=left, 2=middle, 3=right;
// MoonBase has LEFT=1, RIGHT=2, MIDDLE=3 (matches Apple HIG/AppKit).
// Returns 0 for buttons we don't route (4–7 are wheel; SCROLL is a
// separate IPC verb that lands in a later slice).
static uint32_t x_button_to_mb(unsigned int button) {
    switch (button) {
        case Button1: return MB_BUTTON_LEFT;
        case Button2: return MB_BUTTON_MIDDLE;
        case Button3: return MB_BUTTON_RIGHT;
        default:      return 0;
    }
}

// Send a content-rect POINTER_MOVE/DOWN/UP frame to the surface's owning
// client. `x_px`, `y_px` are proxy-relative physical pixels — we strip
// the title-strip height and divide by the surface's backing scale to
// match the IPC contract (content-local points; signed because a drag
// off the window can legitimately produce negatives). Drops silently if
// the IPC encode fails — apps observe missed frames as missed events.
static void send_pointer_frame(mb_surface_t *surf, uint16_t kind,
                               int x_px, int y_px,
                               uint32_t button, uint32_t mb_modifiers) {
    if (!surf || !g_server) return;
    float scale = surf->scale > 0.0f ? surf->scale : 1.0f;
    int   titlebar_px   = (int)(TITLEBAR_HEIGHT * scale + 0.5f);
    int   content_y_px  = y_px - titlebar_px;
    int   x_pts = (int)(x_px / scale);
    int   y_pts = (int)(content_y_px / scale);

    size_t len = 0;
    uint8_t *body = mb_host_build_pointer_event(surf->window_id,
                                                x_pts, y_pts,
                                                button,
                                                mb_modifiers,
                                                mb_host_ts_us(),
                                                &len);
    if (!body) return;
    int rc = mb_server_send(g_server, surf->client, kind,
                            body, len, NULL, 0);
    free(body);
    if (rc != 0) {
        fprintf(stderr,
                "[moonrock] moonbase POINTER frame send failed (%d)\n",
                rc);
    }
}

bool mb_host_handle_button_press(Window win, int x, int y,
                                 unsigned int button,
                                 uint32_t mb_modifiers) {
    mb_surface_t *surf = surface_find_by_proxy(win);
    if (!surf) return false;

    // Any click on a MoonBase surface's chrome or content passes focus
    // to that surface — matches X click-to-focus behavior on reparented
    // clients, which also run through wm_focus_client in on_button_press.
    focus_set(surf->window_id);
    // Hand X focus to the proxy so KeyPress events route here next.
    // Done at the press site (not in focus_set) because focus_set is
    // also called by IPC and lifecycle paths where we don't want to
    // grab the X focus.
    if (g_dpy && surf->input_proxy) {
        XSetInputFocus(g_dpy, surf->input_proxy, RevertToParent,
                       CurrentTime);
    }

    // Split title-strip vs content rect by y pixel against the chrome's
    // titlebar height (TITLEBAR_HEIGHT is in points, scale-multiplied
    // here). Title-strip handles only the traffic-light hit-tests; the
    // rest of the strip (drag region) is consumed-without-routing for
    // now. Content rect always forwards to the bundle.
    int titlebar_px = (int)(TITLEBAR_HEIGHT * surf->scale + 0.5f);
    if (y < titlebar_px) {
        // Left-click only for traffic-light dispatch — other buttons in
        // the title strip are a no-op but still consumed so the WM
        // doesn't double-handle them.
        if (button != Button1) return true;

        int btn = mb_host_chrome_hit_button(x, y, surf->scale);
        if (btn > 0) {
            // Press feedback — set pressed_button, show hover glyphs on
            // all three, re-raster chrome. Grab the pointer so we still
            // get the ButtonRelease even if the user drags off the
            // window before letting go (matches decor.c's grab).
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
        }
        return true;
    }

    // Content rect — forward to bundle as a POINTER_DOWN. Track which
    // MB_BUTTON_* is held so the matching UP fires for the same button
    // even after a drag off the proxy. Symmetric XGrabPointer keeps
    // motion + release flowing while the press is open.
    //
    // owner_events MUST be False: with True, X delivers the release to
    // whatever moonrock-owned window the pointer is over (frame, dock,
    // root, menubar) when those select ButtonReleaseMask, and our
    // release path only ungrabs when the window matches input_proxy —
    // so the grab leaks and every subsequent click in the session
    // routes here. False forces all grabbed events to input_proxy,
    // guaranteeing the matching UP unwinds the grab.
    uint32_t mb_btn = x_button_to_mb(button);
    if (mb_btn == 0) return true;

    surf->pointer_btn_down = (int)mb_btn;
    send_pointer_frame(surf, MB_IPC_POINTER_DOWN, x, y, mb_btn, mb_modifiers);

    if (g_dpy && surf->input_proxy) {
        XGrabPointer(g_dpy, surf->input_proxy, False,
                     PointerMotionMask | ButtonReleaseMask |
                     LeaveWindowMask,
                     GrabModeAsync, GrabModeAsync,
                     None, None, CurrentTime);
    }
    return true;
}

bool mb_host_handle_button_release(Window win, int x, int y,
                                   unsigned int button,
                                   uint32_t mb_modifiers) {
    mb_surface_t *surf = surface_find_by_proxy(win);
    if (!surf) return false;

    // Chrome traffic-light release path: only meaningful for Button1
    // and only when a chrome press is currently open.
    if (button == Button1 && surf->pressed_button != 0) {
        int pressed = surf->pressed_button;
        int over    = mb_host_chrome_hit_button(x, y, surf->scale);

        surf->pressed_button = 0;
        surf->chrome_stale   = true;

        if (g_dpy) XUngrabPointer(g_dpy, CurrentTime);

        // Update hover after the grab ends — keep glyphs lit if release
        // landed inside the button region, clear if user dragged off.
        surf->buttons_hover =
            mb_host_chrome_hit_button_region(x, y, surf->scale);

        // Fire the action only if the release was on the SAME button
        // that was originally pressed — matches SL 10.6 click-and-release.
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

    // Content-rect release path. Match against pointer_btn_down so a
    // press that began on content but ended outside still produces an
    // UP — and so a release with no matching DOWN (e.g. a chrome press
    // that didn't hit a traffic light) is silently dropped instead of
    // synthesizing a phantom UP frame.
    uint32_t mb_btn = x_button_to_mb(button);
    if (mb_btn != 0 && surf->pointer_btn_down == (int)mb_btn) {
        send_pointer_frame(surf, MB_IPC_POINTER_UP, x, y, mb_btn,
                           mb_modifiers);
        surf->pointer_btn_down = 0;
        if (g_dpy) XUngrabPointer(g_dpy, CurrentTime);
    }
    return true;
}

bool mb_host_handle_motion(Window win, int x, int y,
                           uint32_t mb_modifiers) {
    mb_surface_t *surf = surface_find_by_proxy(win);
    if (!surf) return false;

    bool now_hover = mb_host_chrome_hit_button_region(x, y, surf->scale);
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

    // Forward POINTER_MOVE for the content rect. While a content press
    // is open we forward every motion frame (so drags that cross out of
    // the proxy keep producing MOVEs against the grabbed surface);
    // otherwise we only forward when the pointer is actually over the
    // content rect.
    int titlebar_px = (int)(TITLEBAR_HEIGHT * surf->scale + 0.5f);
    bool in_content = (y >= titlebar_px);
    if (surf->pointer_btn_down != 0 || in_content) {
        send_pointer_frame(surf, MB_IPC_POINTER_MOVE, x, y, 0, mb_modifiers);
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

bool mb_host_handle_focus_change(Window win, int type, int mode) {
    mb_surface_t *surf = surface_find_by_proxy(win);
    if (!surf) return false;

    // Only honor focus changes that come from real user / WM intent.
    // NotifyGrab / NotifyUngrab fire on every XGrabPointer pair (the
    // press handler grabs to track drag-off), and NotifyVirtual /
    // NotifyInferior fire on hierarchy reshuffles — none of those mean
    // the user actually moved focus. Filtering to NotifyNormal here
    // prevents traffic-light press/release cycles from flickering
    // g_focused_window_id back to 0 mid-click.
    if (mode != NotifyNormal) return true;

    if (type == FocusIn) {
        focus_set(surf->window_id);
    } else if (type == FocusOut) {
        if (g_focused_window_id == surf->window_id) {
            focus_set(0);
        }
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
    if (timestamp_us == 0) timestamp_us = mb_host_ts_us();

    size_t len = 0;
    uint8_t *body = mb_host_build_drag_frame(kind, window_id, x, y,
                                             modifiers, uris, uri_count,
                                             timestamp_us, &len);
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

    size_t len = 0;
    uint8_t *body = mb_host_build_key_event(surf->window_id, keycode,
                                            modifiers, is_repeat,
                                            mb_host_ts_us(), &len);
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

    size_t len = 0;
    uint8_t *body = mb_host_build_text_input(surf->window_id, utf8,
                                             strlen(utf8),
                                             mb_host_ts_us(), &len);
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
// surface_upload_texture but reads from the surface's mb_chrome_t (Cairo
// pixels, stride, dimensions, revision counter) and uploads into the
// surface-owned GL texture. mb_chrome_t carries no GL state because it
// is shared with moonrock-lite, which uploads to an X drawable rather
// than a GL texture. The revision counter lets us skip the GL upload on
// frames where only the content changed (common case: client animating
// inside a window that hasn't resized or changed focus).
static bool chrome_upload_texture(mb_surface_t *s) {
    const mb_chrome_t *c = &s->chrome;
    if (!c->pixels || c->chrome_w == 0 || c->chrome_h == 0) return false;

    if (s->chrome_tex == 0) {
        glGenTextures(1, &s->chrome_tex);
        if (s->chrome_tex == 0) return false;
        glBindTexture(GL_TEXTURE_2D, s->chrome_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        s->chrome_tex_w = s->chrome_tex_h = 0;
    } else {
        glBindTexture(GL_TEXTURE_2D, s->chrome_tex);
    }

    if (s->chrome_uploaded_revision == c->revision
            && s->chrome_tex_w == c->chrome_w
            && s->chrome_tex_h == c->chrome_h) {
        return true;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(c->stride / 4));

    if (s->chrome_tex_w != c->chrome_w || s->chrome_tex_h != c->chrome_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     (GLsizei)c->chrome_w, (GLsizei)c->chrome_h, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, c->pixels);
        s->chrome_tex_w = c->chrome_w;
        s->chrome_tex_h = c->chrome_h;
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
        // input hit MoonBase surfaces. host_chrome.h takes the three
        // traffic-light PNGs as void* so it stays Cairo-only and free
        // of moonrock asset-cache headers — pass them in here.
        if (s->chrome_stale) {
            void *const btn_imgs[3] = {
                assets_get_close_button(),
                assets_get_minimize_button(),
                assets_get_zoom_button(),
            };
            if (mb_chrome_repaint(&s->chrome,
                                  s->px_w, s->px_h, s->scale,
                                  s->title, /*active*/ true,
                                  s->buttons_hover, s->pressed_button,
                                  btn_imgs)) {
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
            glBindTexture(GL_TEXTURE_2D, s->chrome_tex);
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
