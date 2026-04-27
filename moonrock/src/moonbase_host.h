// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase_host.h — MoonRock's side of the MoonBase IPC.
//
// This is the thin adapter between the moonbase server primitives
// (libmoonbase's src/server/server.c) and moonrock's main event
// loop. moonrock owns the compositor-wide socket; every app that
// links libmoonbase.so.1 connects here.
//
// Slice 2 scope: open the listener, accept clients, run the HELLO /
// WELCOME / BYE handshake, log each event. Window creation, input
// routing, and per-output scale reporting are slice 3+.

#ifndef MOONROCK_MOONBASE_HOST_H
#define MOONROCK_MOONBASE_HOST_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <X11/Xlib.h>
#include <GL/gl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the MoonBase IPC server. Resolves the socket path as
// `$XDG_RUNTIME_DIR/moonbase.sock` if `path` is NULL. `dpy` and `root`
// are borrowed (not retained by the server, but must outlive every
// call through this module) — they're used to create per-surface
// InputOnly X proxies so clicks on MoonBase chrome reach moonrock.
// Pass a NULL `dpy` to skip proxy creation (headless / test mode);
// pointer routing is disabled in that case.
// Returns true on success. On failure, logs why and returns false —
// moonrock runs happily without MoonBase apps, just with no way to
// host them.
bool mb_host_init(const char *path, Display *dpy, Window root);

// Populate `out_fds` with every fd moonrock should poll on behalf of
// MoonBase apps (listener + per-client). Returns the number written,
// clamped to `max`. Safe to call before mb_host_init or after
// mb_host_shutdown — returns 0 in both cases.
size_t mb_host_collect_pollfds(struct pollfd *out_fds, size_t max);

// Process I/O that poll() flagged ready. Safe to call with nfds==0.
void mb_host_tick(const struct pollfd *fds, size_t nfds);

// True when some MoonBase surface currently holds the compositor focus.
// Callers (events.c, input.c) consult this before routing a keyboard /
// pointer event: if focus is on an X client instead, the event takes
// the normal X dispatch path.
bool mb_host_has_focus(void);

// Handle an X ButtonPress that may have landed on a MoonBase surface's
// input proxy. `win` is the XButtonEvent.window; `x` and `y` are the
// proxy-relative click coordinates; `button` is the X button number
// (1 = left); `mb_modifiers` is the MB_MOD_* mask the caller has
// already translated from the XButtonEvent state. Returns true if the
// click was consumed by the MoonBase host (focus-on-click, close /
// minimize / zoom press, title drag, content pointer routing). When
// this returns true the caller must not dispatch the event to any
// other handler.
bool mb_host_handle_button_press(Window win, int x, int y,
                                 unsigned int button,
                                 uint32_t mb_modifiers);

// ButtonRelease companion to mb_host_handle_button_press. Fires the
// traffic-light action (close/minimize/zoom) when the release lands
// on the same disc that was originally pressed; otherwise the click
// is cancelled. Emits MB_IPC_POINTER_UP for content-rect releases when
// a matching content press is open. Drops the pointer grab set by the
// press. Returns true if `win` is a live MoonBase proxy.
bool mb_host_handle_button_release(Window win, int x, int y,
                                   unsigned int button,
                                   uint32_t mb_modifiers);

// MotionNotify routed to the MoonBase host. Tracks the traffic-light
// hover region so the ×/−/+ glyphs appear/disappear as the pointer
// crosses in and out, and forwards MB_IPC_POINTER_MOVE frames for the
// content rect (and for the entire proxy while a content press is open
// so drags off the window still produce MOVE frames). Returns true if
// `win` is a live MoonBase proxy (in which case the caller must not
// dispatch the event elsewhere).
bool mb_host_handle_motion(Window win, int x, int y,
                           uint32_t mb_modifiers);

// LeaveNotify routed to the MoonBase host. Clears the traffic-light
// hover state so glyphs disappear when the pointer exits the proxy
// entirely. Returns true if `win` is a live MoonBase proxy.
bool mb_host_handle_leave(Window win);

// FocusIn / FocusOut routed to the MoonBase host. `win` is the X
// FocusChangeEvent.window; `type` is FocusIn or FocusOut; `mode` is
// the X focus mode (NotifyNormal, NotifyGrab, etc.). The host filters
// NotifyNormal — pointer-grab focus glitches and hierarchy reshuffles
// are ignored. FocusIn syncs g_focused_window_id to the surface;
// FocusOut clears it (only if that surface still owns focus). Returns
// true if `win` is a live MoonBase proxy (the caller must not dispatch
// the event elsewhere in that case).
bool mb_host_handle_focus_change(Window win, int type, int mode);

// Snapshot of the per-surface state that XDND needs in order to convert
// root-relative pointer coordinates into window-local points. All sizes
// are in physical pixels; `scale` is the backing scale. `content_*`
// describe the content rect (app-drawn pixels) inside the chrome. The
// caller-facing IPC window_id is `window_id`; `client` is the IPC
// client to send drag frames to.
typedef struct {
    uint32_t window_id;
    uint32_t client;           // mb_client_id_t, but avoid pulling the
                               // full server header into this interface
    int      screen_x, screen_y;   // outer chrome origin in root coords
    int      content_x, content_y; // content rect origin in root coords
    uint32_t content_w, content_h; // content rect physical-pixel size
    float    scale;
} mb_host_proxy_info_t;

// If `win` is the InputOnly proxy of a live MoonBase surface, fill
// `*out` and return true. Otherwise return false (and leave `*out`
// untouched). Used by the XDND module to decide whether an incoming
// ClientMessage targets a MoonBase surface and to translate root
// coordinates into content-local points.
bool mb_host_find_proxy_surface(Window win, mb_host_proxy_info_t *out);

// Emit a MB_IPC_DRAG_{ENTER,OVER,LEAVE,DROP} frame to `client`.
//   `kind` is the IPC kind constant (MB_IPC_DRAG_*).
//   `window_id` targets a surface the client owns.
//   `x`, `y` are content-rect-local points (ignored by LEAVE).
//   `modifiers` is an MB_MOD_* bitmask (ignored by LEAVE).
//   `uris` / `uri_count` attach a URI list to ENTER/DROP frames; pass
//   NULL/0 for OVER and LEAVE. `timestamp_us` is normally 0 (the host
//   stamps monotonic time); non-zero overrides for replay tests.
// Returns true on success, false on any send / encode failure.
bool mb_host_emit_drag_frame(uint32_t client,
                             uint16_t kind,
                             uint32_t window_id,
                             int      x,
                             int      y,
                             uint32_t modifiers,
                             const char *const *uris,
                             size_t   uri_count,
                             uint64_t timestamp_us);

// Deliver a keyboard event to the currently-focused MoonBase surface.
// `keycode` is an X11 keysym (XK_*); `modifiers` is an MB_MOD_* mask;
// `is_down` chooses MB_IPC_KEY_DOWN vs MB_IPC_KEY_UP. Returns true if a
// frame was queued, false when no MoonBase surface currently holds
// focus (the caller should let the event fall through to its regular
// X-client dispatch in that case).
bool mb_host_route_key(uint32_t keycode, uint32_t modifiers,
                       bool is_down, bool is_repeat);

// Deliver a text-input event (UTF-8) to the currently-focused MoonBase
// surface. Tier 1: the caller is responsible for filtering to printable
// characters that represent actual text; shortcuts (Command-held) and
// control bytes should not reach this function. A real IME lands when
// inputd gains an fcitx/IBus client — at that point the composed text
// gets routed through here the same way. Returns true if a frame was
// queued.
bool mb_host_route_text_input(const char *utf8);

// Render every live MoonBase surface into the current GL context. Called
// from mr_composite during the normal-window pass. The caller supplies
// the same basic-shader / ortho-projection pair the X-client draw uses,
// so MoonBase windows pixel-match X windows in the output. No-ops if
// there are no live surfaces. Must be called with the compositor's GL
// context current (mr_composite guarantees this).
void mb_host_render(GLuint basic_shader, float *projection);

// Close the listener, tear down every connected client, unlink the
// socket path, and free server state. Safe to call when never
// initialized.
void mb_host_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // MOONROCK_MOONBASE_HOST_H
