// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonrock-host — active X11 frontend for native libmoonbase .app
// bundles on foreign distros (slice 19.H.2.b).
//
// Sibling to moonrock-lite. The two binaries split foreign-distro duty
// by toolkit:
//
//   - moonrock-lite: Qt / GTK / AppImage bundles. The bundle process
//     opens its own X window using the host toolkit; moonrock-lite
//     waits for it to appear in _NET_CLIENT_LIST and decorates it from
//     the outside.
//
//   - moonrock-host (this file): native libmoonbase bundles. The
//     bundle never opens an X window. It talks MoonBase IPC to us
//     and hands over a backing buffer. We are the X frontend: own
//     the X window, paint Aqua chrome on top, blit the bundle's
//     committed content below.
//
// Lifecycle (one process per .app):
//
//   1. moonbase-launch fork()s. Child execs `moonrock-host
//      --bundle-id <id>`. Parent execvp's into the bundle binary.
//      PR_SET_PDEATHSIG=SIGTERM ties our lifetime to the bundle.
//   2. We bind the per-bundle MoonBase IPC socket BEFORE XOpenDisplay
//      so libmoonbase wins connect() on its very first try.
//   3. libmoonbase HELLOs. We WELCOME (server.c handles the handshake).
//   4. libmoonbase sends MB_IPC_WINDOW_CREATE. We probe the primary
//      output's effective scale, create one managed X window with
//      _MOTIF_WM_HINTS decorations=0 so the host WM (KWin/Mutter/Xfwm)
//      handles placement / shadow / alt-tab while we own the chrome
//      bar. We paint the Aqua title strip via the shared host_chrome
//      painter, allocate a cairo_image_surface ARGB32 backing surface
//      at the effective scale, and reply WINDOW_CREATE_REPLY.
//   5. (slice 19.H.2.c) bundle commits pixels via MB_IPC_WINDOW_COMMIT.
//      We blit them into the content rect.
//   6. On close (WM_DELETE_WINDOW or close traffic light, or the bundle
//      sending WINDOW_CLOSE) we tear the window down, close the socket,
//      and exit.
//
// Per-bundle socket path: $XDG_RUNTIME_DIR/moonbase/moonbase-<id>.sock.
// We mkdir -p the moonbase/ subdirectory at 0700 if it doesn't exist —
// the IPC frame layer chmods the socket itself. The two-tier guard
// (XDG_SESSION_DESKTOP=CopyCatOS check + connect-probe) is the same as
// moonrock-lite's and prevents stealing a live socket.

#include <cairo/cairo-xlib.h>
#include <cairo/cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

#include "host_chrome.h"
#include "host_protocol.h"
#include "host_util.h"
#include "moonbase/ipc/kinds.h"
#include "server.h"

// ----------------------------------------------------------------------------
// Lifecycle + signals
// ----------------------------------------------------------------------------

static volatile sig_atomic_t g_should_exit = 0;
static void on_sigterm(int sig) { (void)sig; g_should_exit = 1; }

// ----------------------------------------------------------------------------
// Globals (single-window, single-client)
// ----------------------------------------------------------------------------
//
// moonrock-host is one process per .app. There is exactly one IPC
// client (the bundle's libmoonbase) and at most one X window per run.
// A multi-window app would create siblings via additional WINDOW_CREATE
// frames against the same socket — but for slice 19.H.2.b we lock to
// one window and reject any extras with a stderr log.

typedef struct {
    bool             alive;
    uint32_t         window_id;       // u32 id we hand back in REPLY
    Window           xwin;            // our managed X window
    cairo_surface_t *xsurf;           // cairo_xlib_surface on xwin
    cairo_t         *xcr;             // context for chrome paint
    cairo_surface_t *backing;         // cairo_image_surface ARGB32 — the
                                      // libmoonbase committed-buffer target
    int              chrome_w_px;     // outer window width (= content_w_px)
    int              chrome_h_px;     // outer window height (title + content)
    int              title_h_px;      // chrome strip height in physical px
    int              content_w_px;
    int              content_h_px;
    float            scale;           // effective scale (backing × multiplier)
    char            *title;           // owned; from WINDOW_CREATE
    bool             buttons_hover;
    int              pressed_button;  // 1=close, 2=min, 3=zoom, 0=none
} host_window_t;

static Display    *g_dpy        = NULL;
static Window      g_root       = None;
static int         g_screen     = 0;
static Visual     *g_vis        = NULL;
static int         g_depth      = 0;

static mb_server_t *g_server         = NULL;
static char        *g_socket_path    = NULL;
static char        *g_bundle_id      = NULL;     // owned
static const char  *g_display_name   = NULL;     // borrowed (argv or bundle_id)
static host_window_t g_window        = {0};
static uint32_t      g_next_window_id = 1;

// Atoms we send / listen for on the chrome path.
static Atom A_WM_PROTOCOLS              = None;
static Atom A_WM_DELETE_WINDOW          = None;
static Atom A_WM_CHANGE_STATE           = None;
static Atom A_NET_WM_STATE              = None;
static Atom A_NET_WM_STATE_MAX_VERT     = None;
static Atom A_NET_WM_STATE_MAX_HORZ     = None;
static Atom A_MOTIF_WM_HINTS            = None;

// Traffic-light PNGs — same loader pattern as moonrock-lite. NULLs are
// fine: host_chrome's title-strip painter falls back to gray dots.
static cairo_surface_t *g_btn_close    = NULL;
static cairo_surface_t *g_btn_minimize = NULL;
static cairo_surface_t *g_btn_zoom     = NULL;

// ----------------------------------------------------------------------------
// Per-bundle socket path
// ----------------------------------------------------------------------------
//
// $XDG_RUNTIME_DIR/moonbase/moonbase-<bundle-id>.sock. Returns malloc'd
// string the caller frees, or NULL on missing XDG_RUNTIME_DIR / OOM.
// Creates the moonbase/ subdirectory at mode 0700 if absent — XDG_RUNTIME_DIR
// itself is already 0700, but the moonbase/ subdir doesn't exist on a
// fresh foreign-distro session and mb_ipc_frame_listen would fail to
// bind otherwise.

static char *build_per_bundle_socket_path(const char *bundle_id) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (!xdg || !*xdg) {
        fprintf(stderr,
                "[moonrock-host %d] XDG_RUNTIME_DIR not set — "
                "cannot place per-bundle socket\n", getpid());
        return NULL;
    }
    char dir[512];
    int rc = snprintf(dir, sizeof(dir), "%s/moonbase", xdg);
    if (rc < 0 || (size_t)rc >= sizeof(dir)) return NULL;
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr,
                "[moonrock-host %d] mkdir %s failed: %s\n",
                getpid(), dir, strerror(errno));
        return NULL;
    }
    char path[768];
    rc = snprintf(path, sizeof(path),
                  "%s/moonbase-%s.sock", dir, bundle_id);
    if (rc < 0 || (size_t)rc >= sizeof(path)) return NULL;
    return strdup(path);
}

static bool socket_path_has_live_server(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    struct sockaddr_un sa = {0};
    sa.sun_family = AF_UNIX;
    size_t plen = strlen(path);
    if (plen >= sizeof(sa.sun_path)) { close(fd); return false; }
    memcpy(sa.sun_path, path, plen + 1);
    bool live = (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    close(fd);
    return live;
}

static bool should_host_moonbase(const char *socket_path) {
    const char *xdg_session = getenv("XDG_SESSION_DESKTOP");
    if (xdg_session && strcmp(xdg_session, "CopyCatOS") == 0) {
        // Inside a real CopyCatOS session, moonrock proper owns the
        // multi-tenant moonbase.sock. The per-bundle socket path is in
        // a different namespace and shouldn't collide, but binding it
        // would still confuse moonbase-launch's dispatch.
        fprintf(stderr,
                "[moonrock-host %d] XDG_SESSION_DESKTOP=CopyCatOS — "
                "skipping IPC host (real moonrock owns app windows)\n",
                getpid());
        return false;
    }
    if (socket_path_has_live_server(socket_path)) {
        fprintf(stderr,
                "[moonrock-host %d] live server detected at %s — "
                "another moonrock-host already owns this bundle\n",
                getpid(), socket_path);
        return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
// Asset loading — Snow Leopard traffic-light PNGs
// ----------------------------------------------------------------------------

static cairo_surface_t *load_button_png(const char *home, const char *name) {
    char path[512];
    snprintf(path, sizeof(path),
             "%s/.local/share/aqua-widgets/%s", home, name);
    cairo_surface_t *s = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr,
                "[moonrock-host %d] traffic-light asset missing: %s (%s)\n",
                getpid(), path,
                cairo_status_to_string(cairo_surface_status(s)));
        cairo_surface_destroy(s);
        return NULL;
    }
    return s;
}

// ----------------------------------------------------------------------------
// Scale probe — same two-tier strategy as moonrock-lite
// ----------------------------------------------------------------------------
//
// Foreign distros never publish _MOONROCK_OUTPUT_SCALES, so we always
// land on the XRandR PPI-ladder fallback. The fast path stays here so a
// future "moonrock running but moonrock-host hosts this bundle" mode
// (e.g. user-namespace sandbox) gets the right scale for free.

static double probe_scale_at(int cx, int cy) {
    Atom scales_atom = XInternAtom(g_dpy, "_MOONROCK_OUTPUT_SCALES", True);
    if (scales_atom != None) {
        Atom actual_type = None;
        int actual_format = 0;
        unsigned long nitems = 0, bytes_after = 0;
        unsigned char *prop = NULL;
        if (XGetWindowProperty(g_dpy, g_root, scales_atom, 0, (~0L), False,
                               XA_STRING, &actual_type, &actual_format,
                               &nitems, &bytes_after, &prop) == Success
            && prop && nitems > 0) {
            char *line = (char *)prop;
            while (line && *line) {
                char *eol = strchr(line, '\n');
                if (eol) *eol = '\0';
                char namebuf[64];
                int x = 0, y = 0, w = 0, h = 0;
                float eff = 1.0f;
                int got = sscanf(line, "%63s %d %d %d %d %f",
                                 namebuf, &x, &y, &w, &h, &eff);
                if (got >= 6 && cx >= x && cx < x + w &&
                    cy >= y && cy < y + h && eff > 0.0f) {
                    XFree(prop);
                    return (double)eff;
                }
                if (!eol) break;
                line = eol + 1;
            }
        }
        if (prop) XFree(prop);
    }

    int xrr_ev = 0, xrr_err = 0;
    if (!XRRQueryExtension(g_dpy, &xrr_ev, &xrr_err)) return 1.0;

    XRRScreenResources *res = XRRGetScreenResources(g_dpy, g_root);
    if (!res) return 1.0;

    double picked = 1.0;
    for (int i = 0; i < res->noutput; i++) {
        XRROutputInfo *oi = XRRGetOutputInfo(g_dpy, res, res->outputs[i]);
        if (!oi) continue;
        if (oi->connection != RR_Connected || oi->crtc == None) {
            XRRFreeOutputInfo(oi);
            continue;
        }
        XRRCrtcInfo *ci = XRRGetCrtcInfo(g_dpy, res, oi->crtc);
        if (!ci) { XRRFreeOutputInfo(oi); continue; }

        if (cx >= ci->x && cx < ci->x + (int)ci->width &&
            cy >= ci->y && cy < ci->y + (int)ci->height) {
            unsigned int rot      = ci->rotation & 0xf;
            unsigned int pix_axis = ci->width;
            unsigned long mm_axis = oi->mm_width;
            if (rot == RR_Rotate_90 || rot == RR_Rotate_270) {
                pix_axis = ci->height;
                mm_axis  = oi->mm_height;
            }
            if (mm_axis > 0 && pix_axis > 0) {
                double ppi = ((double)pix_axis / (double)mm_axis) * 25.4;
                if      (ppi <  160.0) picked = 1.0;
                else if (ppi <  210.0) picked = 1.25;
                else if (ppi <  260.0) picked = 1.5;
                else if (ppi <  320.0) picked = 1.75;
                else                   picked = 2.0;
            }
            XRRFreeCrtcInfo(ci);
            XRRFreeOutputInfo(oi);
            break;
        }
        XRRFreeCrtcInfo(ci);
        XRRFreeOutputInfo(oi);
    }
    XRRFreeScreenResources(res);
    return picked;
}

// ----------------------------------------------------------------------------
// Self-decoration: ask the host WM not to draw its own title bar
// ----------------------------------------------------------------------------
//
// _MOTIF_WM_HINTS with decorations=0 is the de-facto "client-side
// decorations" hint Linux WMs honor — KWin, Mutter, Xfwm, Openbox all
// strip their frame when this is set. We still get the WM's shadow,
// alt-tab, taskbar entry, focus, and snapping. Same hint Chrome,
// Firefox-with-CSD, GTK4 apps use.
//
// Layout matches the original Motif spec: { flags, functions,
// decorations, input_mode, status }, all CARD32. flags bit 1 (1<<1)
// declares the decorations field is meaningful; decorations=0 means
// none. We don't touch functions/input_mode so the WM keeps default
// move/resize behavior.

static void set_motif_no_decorations(Display *dpy, Window w) {
    if (A_MOTIF_WM_HINTS == None) return;
    long hints[5] = {
        (1L << 1),  // MWM_HINTS_DECORATIONS
        0L,         // functions (unused)
        0L,         // decorations: none
        0L,         // input_mode (unused)
        0L,         // status     (unused)
    };
    XChangeProperty(dpy, w, A_MOTIF_WM_HINTS, A_MOTIF_WM_HINTS, 32,
                    PropModeReplace, (unsigned char *)hints, 5);
}

// ----------------------------------------------------------------------------
// Chrome paint — title strip via host_chrome shared painter
// ----------------------------------------------------------------------------
//
// On a foreign distro there is no real moonrock compositing our window,
// so we don't call mr_window_mapped(). Chrome is painted directly into
// the X drawable via cairo_xlib_surface — same pattern moonrock-lite
// uses since slice 19.H.1.b. Pixels come out bit-identical to a
// CopyCatOS-session-decorated MoonBase window because both paths share
// host_chrome.c.
//
// Below the title strip we fill the content area white. In 19.H.2.c
// MB_IPC_WINDOW_COMMIT replaces this with the bundle's actual pixels.

static void paint_chrome(host_window_t *w) {
    if (!w->alive || !w->xcr) return;

    void *btns[3] = { g_btn_close, g_btn_minimize, g_btn_zoom };
    mb_chrome_paint_title_strip(
        w->xcr,
        w->chrome_w_px, w->title_h_px,
        w->scale,
        w->title ? w->title : g_display_name,
        true,                       // active — single-window app, always focused
        w->buttons_hover,
        w->pressed_button,
        btns);

    // Content area placeholder: white fill until COMMIT_BUFFER lands
    // (slice 19.H.2.c). cairo_set_operator OVER + opaque white matches
    // what the WM expects from a normal RGB window.
    cairo_save(w->xcr);
    cairo_rectangle(w->xcr,
                    0, w->title_h_px,
                    w->content_w_px, w->content_h_px);
    cairo_set_source_rgb(w->xcr, 1.0, 1.0, 1.0);
    cairo_fill(w->xcr);
    cairo_restore(w->xcr);

    cairo_surface_flush(w->xsurf);
    XFlush(g_dpy);
}

// ----------------------------------------------------------------------------
// WINDOW_CREATE handler
// ----------------------------------------------------------------------------

static void send_window_create_reply(mb_client_id_t client,
                                     uint32_t window_id,
                                     uint32_t output_id,
                                     double scale,
                                     uint32_t actual_w_pt,
                                     uint32_t actual_h_pt) {
    size_t reply_len = 0;
    uint8_t *reply = mb_host_build_window_create_reply(
        window_id, output_id, scale,
        actual_w_pt, actual_h_pt, &reply_len);
    if (!reply) {
        fprintf(stderr,
                "[moonrock-host %d] OOM building WINDOW_CREATE_REPLY\n",
                getpid());
        return;
    }
    int rc = mb_server_send(g_server, client, MB_IPC_WINDOW_CREATE_REPLY,
                            reply, reply_len, NULL, 0);
    free(reply);
    if (rc != 0) {
        fprintf(stderr,
                "[moonrock-host %d] WINDOW_CREATE_REPLY send failed (%d)\n",
                getpid(), rc);
    }
}

static void handle_window_create(mb_client_id_t client,
                                 const uint8_t *body, size_t body_len) {
    mb_host_window_create_req_t req;
    if (!mb_host_parse_window_create(body, body_len, &req)) {
        fprintf(stderr,
                "[moonrock-host %d] malformed WINDOW_CREATE from client %u\n",
                getpid(), client);
        mb_host_window_create_req_free(&req);
        return;
    }

    // 19.H.2.b accepts only the Cairo render mode. GL needs a separate
    // dmabuf path (libmoonbase + EGL + DRI3) that lands in 19.H.2.d.
    // Reject early with a stderr log and no reply — libmoonbase's
    // WINDOW_CREATE call will time out and the app exits cleanly.
    if (req.render_mode != 0 /* MOONBASE_RENDER_CAIRO */) {
        fprintf(stderr,
                "[moonrock-host %d] render_mode=%u not yet supported "
                "(slice 19.H.2.b is Cairo-only) — dropping WINDOW_CREATE\n",
                getpid(), req.render_mode);
        mb_host_window_create_req_free(&req);
        return;
    }

    // One window per process for this slice. Multi-window apps land in
    // a later slice where g_window becomes a small array.
    if (g_window.alive) {
        fprintf(stderr,
                "[moonrock-host %d] second WINDOW_CREATE rejected — "
                "multi-window apps not yet supported\n", getpid());
        mb_host_window_create_req_free(&req);
        return;
    }

    // Probe the primary output's effective scale — best initial guess
    // before we know where the WM will place the window. (0,0) sits on
    // the primary output for every WM we care about. ConfigureNotify
    // in a later slice re-probes from the window's actual centre and
    // emits BACKING_SCALE_CHANGED if the value moves.
    double scale_d = probe_scale_at(0, 0);
    float  scale   = (float)scale_d;

    int title_h_pts   = MB_CHROME_TITLEBAR_HEIGHT;
    int title_h_px    = (int)(title_h_pts * scale + 0.5f);
    int content_w_px  = (int)(req.width_points  * scale + 0.5f);
    int content_h_px  = (int)(req.height_points * scale + 0.5f);
    int chrome_w_px   = content_w_px;
    int chrome_h_px   = content_h_px + title_h_px;

    XSetWindowAttributes wa = {0};
    wa.background_pixel = WhitePixel(g_dpy, g_screen);
    wa.event_mask       = ExposureMask | StructureNotifyMask
                        | ButtonPressMask | ButtonReleaseMask
                        | EnterWindowMask | LeaveWindowMask
                        | PointerMotionMask;

    Window xwin = XCreateWindow(
        g_dpy, g_root,
        0, 0, chrome_w_px, chrome_h_px, 0,
        g_depth, InputOutput, g_vis,
        CWBackPixel | CWEventMask, &wa);

    XStoreName(g_dpy, xwin, req.title ? req.title : g_display_name);
    XClassHint ch = {(char *)g_bundle_id, (char *)g_bundle_id};
    XSetClassHint(g_dpy, xwin, &ch);

    // Hand the WM a clean ICCCM contract so taskbar / alt-tab work.
    // WM_DELETE_WINDOW lets us catch close-via-WM-X-button without a
    // hard XKillClient — mirrors what moonrock-lite does for the
    // bundle window.
    Atom protos[1] = { A_WM_DELETE_WINDOW };
    XSetWMProtocols(g_dpy, xwin, protos, 1);

    set_motif_no_decorations(g_dpy, xwin);

    XMapWindow(g_dpy, xwin);
    XFlush(g_dpy);

    cairo_surface_t *xsurf =
        cairo_xlib_surface_create(g_dpy, xwin, g_vis,
                                  chrome_w_px, chrome_h_px);
    cairo_t *xcr = cairo_create(xsurf);

    // The backing surface receives MB_IPC_WINDOW_COMMIT pixels in
    // 19.H.2.c. Allocate now so the size matches the agreed scale and
    // we don't have to renegotiate on the first commit. ARGB32 — the
    // format moonbase clients render into when render_mode=Cairo.
    cairo_surface_t *backing = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, content_w_px, content_h_px);
    if (cairo_surface_status(backing) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr,
                "[moonrock-host %d] backing image alloc failed (%dx%d)\n",
                getpid(), content_w_px, content_h_px);
        cairo_destroy(xcr);
        cairo_surface_destroy(xsurf);
        cairo_surface_destroy(backing);
        XDestroyWindow(g_dpy, xwin);
        mb_host_window_create_req_free(&req);
        return;
    }

    g_window.alive          = true;
    g_window.window_id      = g_next_window_id++;
    g_window.xwin           = xwin;
    g_window.xsurf          = xsurf;
    g_window.xcr            = xcr;
    g_window.backing        = backing;
    g_window.chrome_w_px    = chrome_w_px;
    g_window.chrome_h_px    = chrome_h_px;
    g_window.title_h_px     = title_h_px;
    g_window.content_w_px   = content_w_px;
    g_window.content_h_px   = content_h_px;
    g_window.scale          = scale;
    g_window.title          = req.title;        // take ownership
    req.title               = NULL;             // skip the free below
    g_window.buttons_hover  = false;
    g_window.pressed_button = 0;

    fprintf(stderr,
            "[moonrock-host %d] WINDOW_CREATE id=%u %dx%dpt -> "
            "%dx%dpx (scale=%.2f, title='%s')\n",
            getpid(), g_window.window_id,
            req.width_points, req.height_points,
            chrome_w_px, chrome_h_px, scale_d,
            g_window.title ? g_window.title : "(none)");

    paint_chrome(&g_window);

    send_window_create_reply(client, g_window.window_id,
                             /*output_id*/ 0, scale_d,
                             (uint32_t)req.width_points,
                             (uint32_t)req.height_points);

    mb_host_window_create_req_free(&req);
}

static void teardown_window(host_window_t *w) {
    if (!w->alive) return;
    if (w->xcr)     { cairo_destroy(w->xcr);            w->xcr     = NULL; }
    if (w->xsurf)   { cairo_surface_destroy(w->xsurf);  w->xsurf   = NULL; }
    if (w->backing) { cairo_surface_destroy(w->backing); w->backing = NULL; }
    if (w->xwin)    { XDestroyWindow(g_dpy, w->xwin);   w->xwin    = None; }
    free(w->title); w->title = NULL;
    w->alive = false;
}

static void handle_window_close(mb_client_id_t client,
                                const uint8_t *body, size_t body_len) {
    uint32_t window_id = mb_host_parse_window_close_id(body, body_len);
    if (window_id == 0) {
        fprintf(stderr,
                "[moonrock-host %d] malformed WINDOW_CLOSE from client %u\n",
                getpid(), client);
        return;
    }
    if (!g_window.alive || g_window.window_id != window_id) {
        fprintf(stderr,
                "[moonrock-host %d] WINDOW_CLOSE id=%u no match — ignoring\n",
                getpid(), window_id);
        return;
    }
    fprintf(stderr,
            "[moonrock-host %d] WINDOW_CLOSE id=%u — tearing down\n",
            getpid(), window_id);
    teardown_window(&g_window);
    // moonrock-host's lifetime is tied to its single window — once the
    // bundle closes it, we exit so moonbase-launch can cleanup squashfs
    // + bwrap. mb_server_close in main() unlinks the per-bundle socket.
    g_should_exit = 1;
}

// ----------------------------------------------------------------------------
// IPC event callback
// ----------------------------------------------------------------------------

static void on_ipc_event(mb_server_t *s,
                         const mb_server_event_t *ev,
                         void *user) {
    (void)s; (void)user;
    switch (ev->kind) {
        case MB_SERVER_EV_CONNECTED:
            fprintf(stderr,
                    "[moonrock-host %d] client %u connected "
                    "(bundle=%s ver=%s pid=%u api=%u lang=%u)\n",
                    getpid(), ev->client,
                    ev->hello.bundle_id      ? ev->hello.bundle_id      : "?",
                    ev->hello.bundle_version ? ev->hello.bundle_version : "?",
                    ev->hello.pid, ev->hello.api_version, ev->hello.language);
            break;

        case MB_SERVER_EV_FRAME:
            switch (ev->frame_kind) {
                case MB_IPC_WINDOW_CREATE:
                    handle_window_create(ev->client,
                                         ev->frame_body, ev->frame_body_len);
                    break;
                case MB_IPC_WINDOW_CLOSE:
                    handle_window_close(ev->client,
                                        ev->frame_body, ev->frame_body_len);
                    break;
                // WINDOW_COMMIT lands in 19.H.2.c — ignore for now.
                default:
                    fprintf(stderr,
                            "[moonrock-host %d] frame kind=0x%04x len=%zu "
                            "ignored (slice 19.H.2.b handles only "
                            "WINDOW_CREATE/CLOSE)\n",
                            getpid(), ev->frame_kind, ev->frame_body_len);
                    break;
            }
            break;

        case MB_SERVER_EV_DISCONNECTED:
            fprintf(stderr,
                    "[moonrock-host %d] client %u disconnected "
                    "(reason=%d)\n",
                    getpid(), ev->client, ev->disconnect_reason);
            // The bundle is gone — exit so moonbase-launch tears down
            // the squashfs mount + bwrap pid namespace.
            g_should_exit = 1;
            break;
    }
}

// ----------------------------------------------------------------------------
// Server lifecycle
// ----------------------------------------------------------------------------

static bool start_server(const char *bundle_id) {
    g_socket_path = build_per_bundle_socket_path(bundle_id);
    if (!g_socket_path) return false;
    if (!should_host_moonbase(g_socket_path)) {
        free(g_socket_path);
        g_socket_path = NULL;
        return false;
    }
    int rc = mb_server_open(&g_server, g_socket_path,
                            on_ipc_event, NULL);
    if (rc != 0) {
        fprintf(stderr,
                "[moonrock-host %d] mb_server_open(%s) failed: %d\n",
                getpid(), g_socket_path, rc);
        free(g_socket_path);
        g_socket_path = NULL;
        g_server      = NULL;
        return false;
    }
    fprintf(stderr,
            "[moonrock-host %d] moonbase IPC server bound at %s\n",
            getpid(), g_socket_path);
    return true;
}

static void stop_server(void) {
    if (g_server) {
        mb_server_close(g_server);
        g_server = NULL;
    }
    // mb_ipc_frame_listen unlinks before bind (so a stale socket
    // doesn't block us), but on clean exit we should leave the path
    // free for the next process — unlink explicitly.
    if (g_socket_path) {
        unlink(g_socket_path);
        free(g_socket_path);
        g_socket_path = NULL;
    }
}

// ----------------------------------------------------------------------------
// Event loop — select(X ∪ MoonBase server)
// ----------------------------------------------------------------------------
//
// Same translation pattern moonrock-lite uses: pull POLLIN/POLLOUT
// pollfds from the server, fold them into rfds/wfds, after select copy
// the readable/writable state back into the pollfd revents array, hand
// it to mb_server_tick. POLLOUT matters even pre-WINDOW_CREATE — the
// frame layer queues bytes when the kernel send buffer is full and
// only re-flushes when wfds fires.

#define MB_HOST_MAX_POLLFDS 64

static int host_select(int x_fd, int timeout_ms) {
    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_SET(x_fd, &rfds);
    int max_fd = x_fd;

    struct pollfd mb_fds[MB_HOST_MAX_POLLFDS];
    size_t mb_nfds = 0;
    if (g_server) {
        mb_nfds = mb_server_get_pollfds(g_server, mb_fds,
                                        MB_HOST_MAX_POLLFDS);
        for (size_t i = 0; i < mb_nfds; i++) {
            int fd = mb_fds[i].fd;
            if (fd < 0) continue;
            if (mb_fds[i].events & POLLIN)  FD_SET(fd, &rfds);
            if (mb_fds[i].events & POLLOUT) FD_SET(fd, &wfds);
            if (fd > max_fd) max_fd = fd;
        }
    }

    struct timeval tv;
    struct timeval *ptv = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }
    int n = select(max_fd + 1, &rfds, &wfds, NULL, ptv);

    if (g_server) {
        for (size_t i = 0; i < mb_nfds; i++) {
            int fd = mb_fds[i].fd;
            short rev = 0;
            if (fd >= 0 && FD_ISSET(fd, &rfds)) rev |= POLLIN;
            if (fd >= 0 && FD_ISSET(fd, &wfds)) rev |= POLLOUT;
            mb_fds[i].revents = rev;
        }
        mb_server_tick(g_server, mb_fds, mb_nfds);
    }
    return n;
}

// ----------------------------------------------------------------------------
// X event handling
// ----------------------------------------------------------------------------

static int hit_test_traffic_light(int fx, int fy, double scale) {
    int btn_d  = (int)(MB_CHROME_BUTTON_DIAMETER  * scale);
    int btn_sp = (int)(MB_CHROME_BUTTON_SPACING   * scale);
    int btn_lx = (int)(MB_CHROME_BUTTON_LEFT_PAD  * scale);
    int btn_ty = (int)(MB_CHROME_BUTTON_TOP_PAD   * scale);
    if (fy < btn_ty || fy > btn_ty + btn_d) return 0;
    int bx = btn_lx;
    if (fx >= bx && fx <= bx + btn_d) return 1;
    bx += btn_d + btn_sp;
    if (fx >= bx && fx <= bx + btn_d) return 2;
    bx += btn_d + btn_sp;
    if (fx >= bx && fx <= bx + btn_d) return 3;
    return 0;
}

// Toggle EWMH maximised state for the "zoom" traffic light. data.l[0]=2
// means "toggle"; source=2 (pager-class) so KWin/Mutter accept it
// without focus-stealing-prevention rejection.
static void send_zoom_toggle(Window w) {
    XEvent ev = {0};
    ev.type                 = ClientMessage;
    ev.xclient.window       = w;
    ev.xclient.message_type = A_NET_WM_STATE;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = 2;
    ev.xclient.data.l[1]    = (long)A_NET_WM_STATE_MAX_VERT;
    ev.xclient.data.l[2]    = (long)A_NET_WM_STATE_MAX_HORZ;
    ev.xclient.data.l[3]    = 2;
    XSendEvent(g_dpy, g_root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

static void send_minimize_request(Window w) {
    XEvent ev = {0};
    ev.type                 = ClientMessage;
    ev.xclient.window       = w;
    ev.xclient.message_type = A_WM_CHANGE_STATE;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = 3;            // IconicState (Xutil.h)
    XSendEvent(g_dpy, g_root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

static void handle_x_event(XEvent *ev) {
    if (!g_window.alive) return;

    switch (ev->type) {
        case Expose:
            if (ev->xexpose.window == g_window.xwin && ev->xexpose.count == 0) {
                paint_chrome(&g_window);
            }
            break;

        case ConfigureNotify:
            // Foreign WMs may resize our window (snapping, tiling).
            // Slice 19.H.2.b keeps the size we asked for; later slices
            // will resize the backing surface and emit WINDOW_RESIZED.
            // Accept the geometry as-is for now and just repaint chrome
            // at the new width.
            if (ev->xconfigure.window == g_window.xwin) {
                int nw = ev->xconfigure.width;
                int nh = ev->xconfigure.height;
                if (nw != g_window.chrome_w_px || nh != g_window.chrome_h_px) {
                    cairo_xlib_surface_set_size(g_window.xsurf, nw, nh);
                    g_window.chrome_w_px = nw;
                    g_window.chrome_h_px = nh;
                    g_window.content_w_px = nw;
                    g_window.content_h_px = nh - g_window.title_h_px;
                    paint_chrome(&g_window);
                }
            }
            break;

        case ClientMessage:
            // WM_DELETE_WINDOW from the host WM — clean exit. Mirrors
            // moonrock-lite's bundle-side WM_DELETE handling.
            if (ev->xclient.window == g_window.xwin &&
                ev->xclient.message_type == A_WM_PROTOCOLS &&
                (Atom)ev->xclient.data.l[0] == A_WM_DELETE_WINDOW) {
                fprintf(stderr,
                        "[moonrock-host %d] WM_DELETE_WINDOW — exiting\n",
                        getpid());
                g_should_exit = 1;
            }
            break;

        case ButtonPress:
            if (ev->xbutton.window == g_window.xwin &&
                ev->xbutton.button == Button1 &&
                ev->xbutton.y < g_window.title_h_px) {
                int hit = hit_test_traffic_light(ev->xbutton.x,
                                                 ev->xbutton.y,
                                                 g_window.scale);
                if (hit > 0) {
                    g_window.pressed_button = hit;
                    paint_chrome(&g_window);
                }
            }
            break;

        case ButtonRelease:
            if (ev->xbutton.window == g_window.xwin &&
                ev->xbutton.button == Button1 &&
                g_window.pressed_button > 0) {
                int press_hit = g_window.pressed_button;
                int rel_hit   = (ev->xbutton.y < g_window.title_h_px)
                              ? hit_test_traffic_light(ev->xbutton.x,
                                                       ev->xbutton.y,
                                                       g_window.scale)
                              : 0;
                g_window.pressed_button = 0;
                paint_chrome(&g_window);
                if (rel_hit == press_hit) {
                    switch (press_hit) {
                        case 1: // close
                            fprintf(stderr,
                                    "[moonrock-host %d] close button — "
                                    "exiting\n", getpid());
                            g_should_exit = 1;
                            break;
                        case 2: // minimize
                            send_minimize_request(g_window.xwin);
                            break;
                        case 3: // zoom (toggle maximize)
                            send_zoom_toggle(g_window.xwin);
                            break;
                    }
                }
            }
            break;

        case MotionNotify:
            if (ev->xmotion.window == g_window.xwin) {
                bool over = (ev->xmotion.y < g_window.title_h_px) &&
                            (ev->xmotion.x < (int)(MB_CHROME_BUTTON_LEFT_PAD
                                                + 3 * MB_CHROME_BUTTON_DIAMETER
                                                + 2 * MB_CHROME_BUTTON_SPACING)
                                            * g_window.scale);
                if (over != g_window.buttons_hover) {
                    g_window.buttons_hover = over;
                    paint_chrome(&g_window);
                }
            }
            break;

        case LeaveNotify:
            if (ev->xcrossing.window == g_window.xwin &&
                g_window.buttons_hover) {
                g_window.buttons_hover = false;
                paint_chrome(&g_window);
            }
            break;

        case DestroyNotify:
            if (ev->xdestroywindow.window == g_window.xwin) {
                fprintf(stderr,
                        "[moonrock-host %d] DestroyNotify on chrome — exiting\n",
                        getpid());
                g_should_exit = 1;
            }
            break;
    }
}

// ----------------------------------------------------------------------------
// Argument parsing — bundle_id from --bundle-id or MOONBASE_BUNDLE_ID
// ----------------------------------------------------------------------------

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s --bundle-id <id> [--display-name <name>]\n"
            "       (or set MOONBASE_BUNDLE_ID env)\n", argv0);
}

static bool parse_args(int argc, char **argv,
                       char **out_bundle_id,
                       const char **out_display_name) {
    *out_bundle_id    = NULL;
    *out_display_name = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bundle-id") == 0 && i + 1 < argc) {
            *out_bundle_id = strdup(argv[++i]);
        } else if (strcmp(argv[i], "--display-name") == 0 && i + 1 < argc) {
            *out_display_name = argv[++i];
        } else {
            fprintf(stderr,
                    "[moonrock-host] unknown arg: %s\n", argv[i]);
            return false;
        }
    }
    if (!*out_bundle_id) {
        const char *env = getenv("MOONBASE_BUNDLE_ID");
        if (env && *env) *out_bundle_id = strdup(env);
    }
    if (!*out_bundle_id) return false;
    if (!*out_display_name) *out_display_name = *out_bundle_id;
    return true;
}

// ----------------------------------------------------------------------------
// main
// ----------------------------------------------------------------------------

int main(int argc, char **argv) {
    if (!parse_args(argc, argv, &g_bundle_id, &g_display_name)) {
        usage(argv[0]);
        return 2;
    }

    // Re-arm parent-death-signal: if moonbase-launch dies before we do,
    // the kernel TERMs us so the per-bundle socket gets cleaned up.
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    fprintf(stderr,
            "[moonrock-host %d] start; bundle=%s display=%s\n",
            getpid(), g_bundle_id, g_display_name);

    struct sigaction sa = {0};
    sa.sa_handler = on_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    // Bind the IPC socket BEFORE XOpenDisplay. moonbase-launch fork()s
    // and execvp()s into the bundle in parallel with us — if libmoonbase
    // calls connect() before we bind, the bundle errors out at startup.
    if (!start_server(g_bundle_id)) {
        free(g_bundle_id);
        return 1;
    }

    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr,
                "[moonrock-host %d] XOpenDisplay failed\n", getpid());
        stop_server();
        free(g_bundle_id);
        return 1;
    }
    g_screen = DefaultScreen(g_dpy);
    g_root   = RootWindow(g_dpy, g_screen);
    g_vis    = DefaultVisual(g_dpy, g_screen);
    g_depth  = DefaultDepth(g_dpy, g_screen);

    A_WM_PROTOCOLS           = XInternAtom(g_dpy, "WM_PROTOCOLS",                  False);
    A_WM_DELETE_WINDOW       = XInternAtom(g_dpy, "WM_DELETE_WINDOW",              False);
    A_WM_CHANGE_STATE        = XInternAtom(g_dpy, "WM_CHANGE_STATE",               False);
    A_NET_WM_STATE           = XInternAtom(g_dpy, "_NET_WM_STATE",                 False);
    A_NET_WM_STATE_MAX_VERT  = XInternAtom(g_dpy, "_NET_WM_STATE_MAXIMIZED_VERT",  False);
    A_NET_WM_STATE_MAX_HORZ  = XInternAtom(g_dpy, "_NET_WM_STATE_MAXIMIZED_HORZ",  False);
    A_MOTIF_WM_HINTS         = XInternAtom(g_dpy, "_MOTIF_WM_HINTS",               False);

    const char *home = getenv("HOME");
    if (!home) home = "";
    g_btn_close    = load_button_png(home, "sl_close_button.png");
    g_btn_minimize = load_button_png(home, "sl_minimize_button.png");
    g_btn_zoom     = load_button_png(home, "sl_zoom_button.png");

    int x_fd = ConnectionNumber(g_dpy);

    // Main loop: drive X + IPC together. We don't paint until the
    // bundle sends WINDOW_CREATE, so the loop's only job pre-create
    // is to pump the server through the HELLO/WELCOME handshake.
    while (!g_should_exit) {
        (void)host_select(x_fd, 250);

        while (XPending(g_dpy) > 0 && !g_should_exit) {
            XEvent ev;
            XNextEvent(g_dpy, &ev);
            handle_x_event(&ev);
        }
    }

    fprintf(stderr,
            "[moonrock-host %d] shutting down\n", getpid());

    teardown_window(&g_window);

    if (g_btn_close)    cairo_surface_destroy(g_btn_close);
    if (g_btn_minimize) cairo_surface_destroy(g_btn_minimize);
    if (g_btn_zoom)     cairo_surface_destroy(g_btn_zoom);

    XCloseDisplay(g_dpy);
    stop_server();
    free(g_bundle_id);
    return 0;
}
