// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase.h — MoonBase core public API (Phase A spec, declarations only).
//
// This header is the single entry point for every MoonBase application.
// It intentionally stays tight: app lifecycle, window creation + render
// mode selection, event pump, main-thread dispatch, capability queries,
// per-window backing scale. Everything higher-level (keychain, a11y,
// notifications, power, controller, …) lives in its own companion
// header with its own MOONBASE_<DOMAIN>_API_VERSION macro, all inside
// the same libmoonbase.so.1.
//
// Design invariants:
//   * Framework library, not host. Each .appc runs as its own process,
//     links libmoonbase.so.1, and talks to the MoonRock compositor over
//     a single Unix-domain socket (see IPC.md).
//   * MoonRock draws 100% of Aqua chrome (title bar, traffic lights,
//     shadows, resize affordances). Apps draw their content rect only.
//   * Every sized field in this header is in POINTS, not pixels. Points
//     map to physical pixels through a per-window backing scale the
//     framework tracks automatically. See "HiDPI" below.
//   * Every UI call must be made from the thread that called
//     moonbase_init(). moonbase_dispatch_main() is the only API safe to
//     invoke from a worker thread — it schedules `fn` on the main
//     thread the next time the event loop turns.
//
// Phase A status: declarations only. No function implementation ships
// in this phase. Phase B links libmoonbase.so.1 with every symbol below
// defined as a stub returning MB_ENOSYS.

#ifndef MOONBASE_H
#define MOONBASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------
// API version
// ---------------------------------------------------------------------
//
// Encoded as MAJOR*10000 + MINOR*100 + PATCH. 1.0.0 == 10000.
// Bumps are add-only within a major version. Major bumps require a
// rebuild against a new soname (libmoonbase.so.2 may coexist on disk).

#define MOONBASE_API_VERSION 10000

// ---------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------
//
// Most functions either return 0 on success and a negative MB_E* value
// on failure, or return a pointer and set a last-error the caller can
// fetch with moonbase_last_error(). The two styles are documented per
// function. Zero is always success.

typedef enum {
    MB_EOK           =  0,
    MB_EINVAL        = -1,   // bad argument
    MB_ENOSYS        = -2,   // not implemented in this build/version
    MB_EIPC          = -3,   // compositor IPC failure
    MB_EPERM         = -4,   // denied by entitlement or sandbox
    MB_ENOMEM        = -5,
    MB_ENOTFOUND     = -6,   // key/handle/resource not found
    MB_EAGAIN        = -7,   // retry
    MB_EPROTO        = -8,   // wire-format violation
    MB_EVERSION      = -9,   // compositor API incompatible with client
} mb_error_t;

// moonbase_last_error — last error set on the calling thread by any
// MoonBase function whose "returns pointer" contract is documented.
// Returns MB_EOK if no error has been recorded since the last success.
mb_error_t moonbase_last_error(void);

// moonbase_error_string — human-readable name of an error code.
// Returned pointer is static storage; never free.
const char *moonbase_error_string(mb_error_t err);

// ---------------------------------------------------------------------
// Opaque handle types
// ---------------------------------------------------------------------

typedef struct mb_window mb_window_t;

// ---------------------------------------------------------------------
// Rendering model
// ---------------------------------------------------------------------
//
// Render mode is declared at window creation and is not runtime-
// switchable. Web apps are implicitly MOONBASE_RENDER_GL via dmabuf.

typedef enum {
    MOONBASE_RENDER_CAIRO = 0,   // CPU-side Cairo surface. Portable.
    MOONBASE_RENDER_GL    = 1,   // App-owned EGL/GL context.
} mb_render_mode_t;

// ---------------------------------------------------------------------
// Window description
// ---------------------------------------------------------------------
//
// All dimensions are in POINTS. MoonRock draws chrome outside the
// requested content rect — the app never sees chrome coordinates.
// `version` must be set to MOONBASE_WINDOW_DESC_VERSION so the
// framework can grow the struct later without breaking the ABI.

#define MOONBASE_WINDOW_DESC_VERSION 1

// Window flags. Default (flags == 0) is a standard resizable document
// window with full Aqua chrome. Flags are additive modifiers; each bit
// flips one aspect of the default.
#define MB_WINDOW_FLAG_FIXED_SIZE  (1u << 0)  // reject user resize; min == max == initial
#define MB_WINDOW_FLAG_UTILITY     (1u << 1)  // small-chrome utility panel
#define MB_WINDOW_FLAG_CENTER      (1u << 2)  // center on primary output at show

typedef struct {
    int              version;               // MOONBASE_WINDOW_DESC_VERSION
    const char      *title;                 // UTF-8. Borrowed. May be NULL.
    int              width_points;          // initial content width
    int              height_points;         // initial content height
    int              min_width_points;      // 0 = no floor
    int              min_height_points;
    int              max_width_points;      // 0 = no ceiling
    int              max_height_points;
    mb_render_mode_t render_mode;
    uint32_t         flags;                 // MB_WINDOW_FLAG_*
} mb_window_desc_t;

// ---------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------
//
// Tagged union. Identical layout across C, Python, Rust, Swift, JS.
// The event's payload (and any const char * inside it) is valid until
// the next call to moonbase_poll_event / moonbase_wait_event on this
// thread, or until the current event handler returns. Copy what you
// need to keep.

typedef enum {
    MB_EV_NONE = 0,

    // Lifecycle
    MB_EV_APP_WILL_QUIT,

    // Windows
    MB_EV_WINDOW_SHOWN,
    MB_EV_WINDOW_CLOSED,
    MB_EV_WINDOW_RESIZED,
    MB_EV_WINDOW_FOCUSED,
    MB_EV_WINDOW_REDRAW,

    // Keyboard / text
    MB_EV_KEY_DOWN,
    MB_EV_KEY_UP,
    MB_EV_TEXT_INPUT,

    // Pointer / scroll
    MB_EV_POINTER_MOVE,
    MB_EV_POINTER_DOWN,
    MB_EV_POINTER_UP,
    MB_EV_SCROLL,

    // Touch (built-in panel only — see CLAUDE.md Multi-Monitor mandate)
    MB_EV_TOUCH_BEGIN,
    MB_EV_TOUCH_MOVE,
    MB_EV_TOUCH_END,

    // Gestures (trackpad + touch)
    MB_EV_GESTURE_PINCH,
    MB_EV_GESTURE_SWIPE,

    // Controller
    MB_EV_CONTROLLER_BUTTON,
    MB_EV_CONTROLLER_AXIS,
    MB_EV_CONTROLLER_HOTPLUG,

    // System signals
    MB_EV_CLIPBOARD_CHANGED,
    MB_EV_THERMAL_CHANGED,
    MB_EV_POWER_CHANGED,
    MB_EV_LOW_MEMORY,
    MB_EV_COLOR_SCHEME_CHANGED,

    // HiDPI — window crossed outputs of different scale, or user
    // changed its host output's scale.
    MB_EV_BACKING_SCALE_CHANGED,

    MB_EV_KIND_MAX,   // sentinel, not delivered
} mb_event_kind_t;

// Key modifier bitmask, used across key / pointer / scroll events.
#define MB_MOD_SHIFT    (1u << 0)
#define MB_MOD_CONTROL  (1u << 1)
#define MB_MOD_OPTION   (1u << 2)   // Alt
#define MB_MOD_COMMAND  (1u << 3)
#define MB_MOD_CAPSLOCK (1u << 4)
#define MB_MOD_FN       (1u << 5)

// Pointer buttons.
#define MB_BUTTON_LEFT    1
#define MB_BUTTON_RIGHT   2
#define MB_BUTTON_MIDDLE  3

// Thermal states. Match the values exposed by moonbase_power.h.
#define MB_THERMAL_NOMINAL   0
#define MB_THERMAL_FAIR      1
#define MB_THERMAL_SERIOUS   2
#define MB_THERMAL_CRITICAL  3

typedef struct {
    mb_event_kind_t kind;
    mb_window_t    *window;        // associated window, NULL for app-level events
    uint64_t        timestamp_us;  // monotonic, microseconds since boot

    union {
        // MB_EV_WINDOW_RESIZED — sizes are in points
        struct {
            int old_width, old_height;
            int new_width, new_height;
        } resize;

        // MB_EV_WINDOW_FOCUSED
        struct { bool has_focus; } focus;

        // MB_EV_WINDOW_REDRAW — dirty rect in points
        struct { int x, y, width, height; } redraw;

        // MB_EV_KEY_DOWN / MB_EV_KEY_UP
        struct {
            uint32_t keycode;       // X11 keysym (stable across drivers)
            uint32_t modifiers;     // MB_MOD_*
            bool     is_repeat;
        } key;

        // MB_EV_TEXT_INPUT — composed UTF-8 text (NUL-terminated)
        struct { const char *text; } text;

        // MB_EV_POINTER_* — coords in points, window-local
        struct {
            int      x, y;
            uint32_t button;        // MB_BUTTON_*, 0 on MOVE
            uint32_t modifiers;
        } pointer;

        // MB_EV_SCROLL — continuous delta in points (trackpad) or
        // discrete "notch" units (wheel). `high_precision` distinguishes.
        struct {
            double dx, dy;
            bool   high_precision;
            uint32_t modifiers;
        } scroll;

        // MB_EV_TOUCH_* — coords in points, window-local (built-in panel)
        struct {
            int      touch_id;
            int      x, y;
            double   pressure;      // 0.0..1.0, or -1 if unknown
        } touch;

        // MB_EV_GESTURE_PINCH — continuous scale factor relative to start
        struct { double scale; double rotation_radians; } pinch;

        // MB_EV_GESTURE_SWIPE — coarse direction delta in points
        struct { int dx, dy; } swipe;

        // MB_EV_CONTROLLER_BUTTON
        struct {
            uint32_t device_id;
            uint32_t button;        // see moonbase_controller.h
            bool     pressed;
        } ctrl_button;

        // MB_EV_CONTROLLER_AXIS — normalized [-1.0, 1.0] or [0.0, 1.0] for triggers
        struct {
            uint32_t device_id;
            uint32_t axis;
            double   value;
        } ctrl_axis;

        // MB_EV_CONTROLLER_HOTPLUG
        struct {
            uint32_t device_id;
            bool     connected;
            const char *name;       // product name, borrowed
        } ctrl_hotplug;

        // MB_EV_THERMAL_CHANGED
        struct { int state; } thermal;

        // MB_EV_POWER_CHANGED
        struct {
            bool     on_ac;
            int      percent;       // 0..100, -1 if no battery
            int      minutes_remaining;  // -1 if unknown
        } power;

        // MB_EV_COLOR_SCHEME_CHANGED — v1 is always "light"; this
        // event is reserved so the field exists when warm-tint or a
        // later theme lands.
        struct { const char *scheme; } color_scheme;

        // MB_EV_BACKING_SCALE_CHANGED
        struct { float old_scale, new_scale; } backing_scale;
    };
} mb_event_t;

// ---------------------------------------------------------------------
// Lifecycle + main loop
// ---------------------------------------------------------------------
//
// moonbase_init — must be called exactly once before any other
// MoonBase call. `argc`/`argv` may be (0, NULL) for apps that don't
// want command-line parsing. The calling thread becomes the "main
// thread" for this process; every subsequent MoonBase call (except
// moonbase_dispatch_main) must happen on it.
//
// Returns MB_EOK on success. On failure returns a negative mb_error_t.
int moonbase_init(int argc, char **argv);

// moonbase_run — MoonBase owns the run loop. Blocks until
// moonbase_quit() is called. Delivered events are forwarded to the
// handler installed via moonbase_set_event_handler(), plus the
// default-handler drains window/close + ignore-rest behavior.
//
// Returns the exit code passed to moonbase_quit().
int moonbase_run(void);

// moonbase_poll_event — app owns the loop. Returns 1 if an event was
// available and written to *ev, 0 if the queue is empty, negative on
// error. Never blocks.
int moonbase_poll_event(mb_event_t *ev);

// moonbase_wait_event — app owns the loop. Blocks up to `timeout_ms`
// (-1 = forever, 0 = same as poll). Returns 1 if an event was
// delivered, 0 on timeout, negative on error.
int moonbase_wait_event(mb_event_t *ev, int timeout_ms);

// moonbase_quit — request orderly shutdown. Posts MB_EV_APP_WILL_QUIT
// first, then breaks the run loop. Safe to call from the main thread
// only; worker threads must go through moonbase_dispatch_main.
void moonbase_quit(int exit_code);

// ---------------------------------------------------------------------
// Event handler (used by moonbase_run)
// ---------------------------------------------------------------------
//
// When an app calls moonbase_run(), each event is passed to this
// callback. The poll/wait shape does not invoke it. Default behavior
// if no handler is installed: MB_EV_WINDOW_CLOSED on the last visible
// window calls moonbase_quit(0); everything else is ignored.

typedef void (*mb_event_handler_t)(const mb_event_t *ev, void *userdata);

// moonbase_set_event_handler — install the run-loop handler. Replaces
// any previously installed handler. Pass NULL to restore default.
void moonbase_set_event_handler(mb_event_handler_t fn, void *userdata);

// ---------------------------------------------------------------------
// Main-thread dispatch
// ---------------------------------------------------------------------
//
// The ONLY API safe to call from a worker thread. Schedules `fn(ud)`
// to run on the main thread during the next event-loop turn. Order
// between independent calls is FIFO. Reentrancy is allowed: a
// dispatched function may enqueue more dispatches.

void moonbase_dispatch_main(void (*fn)(void *), void *userdata);

// ---------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------
//
// Ownership: mb_window_t * returned by moonbase_window_create is owned
// by the framework. moonbase_window_close tears it down; do not use
// the handle afterwards.

mb_window_t *moonbase_window_create(const mb_window_desc_t *desc);
void         moonbase_window_show(mb_window_t *w);
void         moonbase_window_close(mb_window_t *w);

// Metadata + geometry. All sizes/positions are in points.
void  moonbase_window_set_title(mb_window_t *w, const char *title_utf8);
void  moonbase_window_size(mb_window_t *w, int *width_points, int *height_points);
int   moonbase_window_set_size(mb_window_t *w, int width_points, int height_points);
void  moonbase_window_position(mb_window_t *w, int *x, int *y);
int   moonbase_window_set_position(mb_window_t *w, int x, int y);

// Ask MoonRock to deliver an MB_EV_WINDOW_REDRAW event covering the
// given region (or the whole content if w/h are 0). Coalesces with any
// region the compositor already considers dirty.
void  moonbase_window_request_redraw(mb_window_t *w, int x, int y, int width, int height);

// Name of the output currently hosting this window (EDID-derived, e.g.
// "Legion Built-in" or "LG UltraFine"). Borrowed, valid until the
// next event pump turn.
const char *moonbase_window_output_name(mb_window_t *w);

// ---------------------------------------------------------------------
// HiDPI — backing scale queries (points ↔ physical pixels)
// ---------------------------------------------------------------------
//
// `backing_scale` is the current float multiplier between points and
// physical pixels for this window's host output. 1.0 means 1 pt = 1 px;
// 2.0 means 1 pt = 2 px. Fractional values (1.25, 1.5, 1.75, …) are
// first-class.
//
// Cairo apps do not need to scale themselves — the framework applies
// cairo_scale(ctx, scale, scale) so draw calls stay in points. GL apps
// must size their framebuffer to backing_pixel_size and render in
// physical pixels.
//
// A window moved between outputs of different scale, or an output
// whose user-chosen scale is changed, posts MB_EV_BACKING_SCALE_CHANGED
// before the next MB_EV_WINDOW_REDRAW.

float moonbase_window_backing_scale(mb_window_t *w);
void  moonbase_window_backing_pixel_size(mb_window_t *w,
                                         int *width_px, int *height_px);

// ---------------------------------------------------------------------
// Render handoff — Cairo path
// ---------------------------------------------------------------------
//
// Valid only for MOONBASE_RENDER_CAIRO windows and only inside an
// MB_EV_WINDOW_REDRAW handler. Returns a cairo_t * (typed as void *
// here to keep this header free of a <cairo.h> dependency). The
// framework pre-applies cairo_scale for the window's current backing
// scale, so the caller draws in points. Do not cairo_destroy() it.
//
// moonbase_window_commit marks the surface ready for MoonRock to
// composite. Cairo apps that draw outside an MB_EV_WINDOW_REDRAW
// handler (animation ticks, etc.) must call
// moonbase_window_request_redraw instead of drawing directly.

void *moonbase_window_cairo(mb_window_t *w);
int   moonbase_window_commit(mb_window_t *w);

// ---------------------------------------------------------------------
// Render handoff — GL path
// ---------------------------------------------------------------------
//
// Valid only for MOONBASE_RENDER_GL windows. The framework owns the
// EGL display and the surface; the app owns its context. Call
// make_current on the main thread, render into the backing-pixel-
// sized framebuffer, then call swap_buffers to hand the frame to
// MoonRock (internally implemented as a dmabuf export).

int   moonbase_window_gl_make_current(mb_window_t *w);
int   moonbase_window_gl_swap_buffers(mb_window_t *w);

// ---------------------------------------------------------------------
// Capability + entitlement queries
// ---------------------------------------------------------------------
//
// Capabilities describe what the runtime can do on this machine right
// now — e.g. "gl", "cairo", "webview", "controller", "keychain",
// "notifications". Entitlements describe what THIS app declared it
// may do — e.g. "hardware:camera", "filesystem:documents:read-write".
//
// Both take namespaced string keys. Both return 1 if present, 0 if
// absent. `moonbase_has_entitlement` returns 0 for any entitlement
// not declared in Info.appc even if the user could grant it; the
// consent flow lives in a companion header (moonbase_consent, not
// shipped in v1).

int moonbase_has_capability(const char *name);
int moonbase_has_entitlement(const char *name);

// ---------------------------------------------------------------------
// Bundle + app metadata
// ---------------------------------------------------------------------
//
// All returned `const char *` are borrowed and valid for the lifetime
// of the process — they read from Info.appc loaded at launch.

const char *moonbase_bundle_id(void);        // e.g. "show.blizzard.textedit"
const char *moonbase_bundle_name(void);      // display name
const char *moonbase_bundle_version(void);   // e.g. "1.0.0"
const char *moonbase_bundle_path(void);      // absolute path to .appc

// Resolve a file inside Contents/Resources/, following the locale
// fallback chain (user locale → user fallback list → bundle base
// locale). Returns an absolute path the caller must free with
// moonbase_release(), or NULL if not found.
char *moonbase_bundle_resource_path(const char *relative);

// ---------------------------------------------------------------------
// Per-app data paths
// ---------------------------------------------------------------------
//
// All three return absolute paths inside
//   ~/.local/share/moonbase/<bundle-id>/
// The directories are created lazily on first access. Returned strings
// are borrowed, valid for the process lifetime.

const char *moonbase_data_path(void);       // Application Support/
const char *moonbase_prefs_path(void);      // Preferences/
const char *moonbase_cache_path(void);      // Caches/

// ---------------------------------------------------------------------
// Preferences (plist-equivalent TOML key/value store)
// ---------------------------------------------------------------------
//
// Preferences are stored in Preferences/<domain>.toml. The default
// domain is the bundle-id. Values live per-user. Mutations are
// durable across relaunch; no explicit flush is required but
// moonbase_prefs_sync() forces fsync.
//
// String getters return NULL if the key is absent and `fallback` is
// NULL. If a fallback is given, its pointer is returned unchanged.
// Never free the returned pointer; if you need ownership, strdup.

const char *moonbase_prefs_get_string(const char *key, const char *fallback);
int         moonbase_prefs_set_string(const char *key, const char *value);
int         moonbase_prefs_get_int(const char *key, int fallback);
int         moonbase_prefs_set_int(const char *key, int value);
bool        moonbase_prefs_get_bool(const char *key, bool fallback);
int         moonbase_prefs_set_bool(const char *key, bool value);
int         moonbase_prefs_remove(const char *key);
int         moonbase_prefs_sync(void);

// ---------------------------------------------------------------------
// Memory ownership helper
// ---------------------------------------------------------------------
//
// Any function that returns a freshly-allocated buffer (documented
// per function) hands ownership to the caller, who must release it
// with moonbase_release — never with free(). This lets the framework
// use a different allocator than the app if it has to.

void moonbase_release(void *ptr);

#ifdef __cplusplus
}
#endif

#endif // MOONBASE_H
