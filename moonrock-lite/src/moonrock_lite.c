// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonrock-lite — sidecar Aqua chrome process for foreign-distro .app
// launches. See moonrock-lite/meson.build for the why; this file is
// the entire implementation.
//
// Pre-19.H this code lived as `run_chrome_stub()` and its helpers
// inside moonbase/runtime/moonbase-launcher.c (lines ~817–2129). The
// extraction is a pure plumbing slice: nothing about the X / Cairo /
// DBusMenu logic changes, the function bodies are moved verbatim. The
// launcher's role shrinks to fork+execvp; moonrock-lite owns its own
// X connection, signal handling, and lifecycle.
//
// Argv shape (parsed by parse_args below):
//
//   moonrock-lite --bundle-id <id> [--display-name <name>]
//                 [--theme <0|1|2>]
//
//   --bundle-id     required; matched against WM_CLASS instance to
//                   discover the bundle window in _NET_CLIENT_LIST.
//   --display-name  defaults to bundle-id; rendered as the bold
//                   app-name slot in the menu bar and as WM_NAME on
//                   the chrome window.
//   --theme         menubar_render_theme_t value:
//                     0 = MENUBAR_THEME_AQUA (default, hard fallback)
//                     1 = MENUBAR_THEME_HOST_BREEZE_LIGHT
//                     2 = MENUBAR_THEME_HOST_ADWAITA_LIGHT
//                   Resolved by the launcher's 4-tier theme precedence
//                   resolver (slice 19.F) and round-tripped here.

#include "menubar_render.h"

#include "appmenu_bridge.h"
#include "dbusmenu_client.h"
#include "menu_model.h"

#include <cairo/cairo-xlib.h>
#include <cairo/cairo.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>

#include "host_chrome.h"

// ----------------------------------------------------------------------------
// Lifecycle + signals
// ----------------------------------------------------------------------------

static volatile sig_atomic_t chrome_stub_should_exit = 0;
static void chrome_stub_sigterm(int sig) { (void)sig; chrome_stub_should_exit = 1; }

// 19.D-β-2b — DBusMenu menu import for the chrome stub.
//
// The bundle's Qt/GTK appmenu module exports its menu tree on the session
// bus. The chrome stub claims com.canonical.AppMenu.Registrar via
// appmenu_bridge_init(), maps wid → (service, path) on RegisterWindow,
// then binds a DbusMenuClient that pulls the tree as a MenuNode and
// pushes refetches through on_changed.
//
// items[] borrows MenuNode->label pointers. Bridge dispatch, on_changed,
// rebuild_items and paint all run in the same select+dispatch iteration,
// so labels stay valid between rebuild and paint without a copy. Rebuild
// runs only when on_changed has flipped legacy_dirty true.
//
// Lookup retries each loop iteration until it succeeds or times out —
// RegisterWindow normally arrives before bundle discovery, but slow Qt/GTK
// platform-theme load can race past it. After ~5s without a hit we stop
// retrying and stay on the bold-app-name fallback. KDE / nil-state hosts
// also land here unconditionally (the bridge returns false when KWin owns
// the registrar name).

#define CHROME_STUB_MAX_ITEMS    32
#define CHROME_STUB_LOOKUP_SECS   5

typedef struct {
    menubar_render_item_t   items[CHROME_STUB_MAX_ITEMS];
    // Parallel to items[]: the DBusMenu legacy_id of the MenuNode each
    // top-level slot was sourced from, so a click can translate slot index
    // → AboutToShow / Event(id, …) targets without re-walking the root.
    // Slot 0 (the bold app name) has no DBusMenu source — sentinel -1.
    int32_t                 top_level_ids[CHROME_STUB_MAX_ITEMS];
    size_t                  n_items;
    bool                    legacy_dirty;
    DbusMenuClient         *dbusmenu;
    const char             *display_name;
} chrome_stub_state_t;

static void chrome_stub_on_menu_changed(DbusMenuClient *client,
                                        void *user_data) {
    (void)client;
    chrome_stub_state_t *s = (chrome_stub_state_t *)user_data;
    s->legacy_dirty = true;
}

// Repopulate items[] from the bound DbusMenuClient's MenuNode root, or
// fall back to bundle name only if no root has arrived yet. items[0] is
// always the bold app name slot (Snow Leopard convention); items[1..]
// are top-level menu titles taken from the legacy tree.
static void chrome_stub_rebuild_items(chrome_stub_state_t *s) {
    s->items[0].title            = s->display_name;
    s->items[0].is_app_name_bold = true;
    s->items[0].x                = 0;
    s->items[0].width            = 0;
    s->top_level_ids[0]          = -1;        // bold app name has no DBusMenu source
    s->n_items = 1;

    if (!s->dbusmenu) return;
    const MenuNode *root = dbusmenu_client_root(s->dbusmenu);
    if (!root) return;

    for (int i = 0; i < root->n_children &&
                    s->n_items < CHROME_STUB_MAX_ITEMS; ++i) {
        const MenuNode *c = root->children[i];
        if (!c || !c->visible || c->type == MENU_ITEM_SEPARATOR ||
            !c->label) continue;
        s->items[s->n_items].title            = c->label;
        s->items[s->n_items].is_app_name_bold = false;
        s->items[s->n_items].x                = 0;
        s->items[s->n_items].width            = 0;
        s->top_level_ids[s->n_items]          = c->legacy_id;
        s->n_items++;
    }
}

// select(x_fd ∪ glib_fds) — drives both the X connection and the GLib
// main context that owns appmenu_bridge + DbusMenuClient. Without this
// the chrome stub would block in XNextEvent and miss every D-Bus reply:
// the registrar name would never finish acquiring, RegisterWindow would
// queue indefinitely, and DBusMenu refetches would never land.
//
// Returns when X has events ready, glib dispatched at least one source,
// or `timeout_ms` elapses. Caller drains XPending and rebuilds items[]
// in response to legacy_dirty before painting.
static int chrome_stub_select(int x_fd, int timeout_ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(x_fd, &rfds);
    int max_fd = x_fd;

    appmenu_bridge_prepare_select(&rfds, &max_fd, &timeout_ms);

    struct timeval tv;
    struct timeval *ptv = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }
    int n = select(max_fd + 1, &rfds, NULL, NULL, ptv);

    appmenu_bridge_dispatch(&rfds);
    return n;
}

// True iff w's WM_CLASS instance (XClassHint.res_name) equals bundle_id.
// The instance is what argv-name overrides set; the class is the toolkit's
// natural name. Match instance only — Qt sets `instance=bundle_id, class=kate`
// when launched with `-name bundle_id`, GTK with argv[0]=bundle_id sets both.
static bool x_window_class_instance_matches(Display *dpy, Window w,
                                            const char *bundle_id) {
    XClassHint ch = {0};
    if (!XGetClassHint(dpy, w, &ch)) return false;
    bool match = (ch.res_name && strcmp(ch.res_name, bundle_id) == 0);
    if (ch.res_name)  XFree(ch.res_name);
    if (ch.res_class) XFree(ch.res_class);
    return match;
}

// Walk the WM's _NET_CLIENT_LIST on root and return the first window
// whose WM_CLASS instance matches bundle_id, or 0 if none. Linear scan
// of the list is fine — discovery runs once at startup.
static Window find_bundle_window_in_client_list(Display *dpy, Window root,
                                                Atom net_client_list,
                                                const char *bundle_id) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, root, net_client_list, 0, 1024,
                           False, XA_WINDOW, &actual_type, &actual_format,
                           &nitems, &bytes_after, &data) != Success) {
        return 0;
    }
    Window found = 0;
    if (actual_type == XA_WINDOW && actual_format == 32 && data) {
        Window *list = (Window *)data;
        for (unsigned long i = 0; i < nitems; ++i) {
            if (x_window_class_instance_matches(dpy, list[i], bundle_id)) {
                found = list[i];
                break;
            }
        }
    }
    if (data) XFree(data);
    return found;
}

// Block (with timeout) until a window matching bundle_id appears in
// _NET_CLIENT_LIST. Returns 0 on timeout. The bundle is launched
// downstream of the chrome stub's parent fork, so we generally see it
// within ~1s; allow timeout_sec for slow cold starts (squashfuse mount,
// bwrap setup, language runtime warmup).
//
// EWMH-correct path: PropertyNotify on root for _NET_CLIENT_LIST. On
// reparenting WMs (KWin, Mutter, Xfwm) MapNotify on root fires for
// the WM frame, not the client; the client gets a ReparentNotify into
// the frame and is otherwise invisible to root listeners. Every modern
// EWMH-compliant WM does append the *client* (not frame) to
// _NET_CLIENT_LIST, so this is the universal hook.
//
// Race-safe: we select PropertyChangeMask first, then immediately scan
// the current _NET_CLIENT_LIST. If the bundle window is already there
// (chrome stub started slowly), we catch it without waiting for an
// event that already fired.
static Window wait_for_bundle_window(Display *dpy, Window root,
                                     const char *bundle_id,
                                     int timeout_sec) {
    Atom net_client_list = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    XSelectInput(dpy, root, PropertyChangeMask);

    Window found = find_bundle_window_in_client_list(dpy, root,
                                                    net_client_list,
                                                    bundle_id);
    if (found) {
        XSelectInput(dpy, root, NoEventMask);
        return found;
    }

    int x_fd = ConnectionNumber(dpy);
    time_t start = time(NULL);
    while (!chrome_stub_should_exit) {
        while (XPending(dpy) > 0) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == PropertyNotify &&
                ev.xproperty.window == root &&
                ev.xproperty.atom == net_client_list) {
                found = find_bundle_window_in_client_list(dpy, root,
                                                         net_client_list,
                                                         bundle_id);
                if (found) {
                    XSelectInput(dpy, root, NoEventMask);
                    return found;
                }
            }
        }
        // 1s slice keeps SIGTERM and the overall timeout reactive
        // without busy-looping when no events are pending.
        // chrome_stub_select also pumps the GLib main context so
        // appmenu_bridge can complete name acquisition and process any
        // RegisterWindow calls that race ahead of bundle window mapping.
        (void)chrome_stub_select(x_fd, 1000);
        if (time(NULL) - start > timeout_sec) break;
    }
    XSelectInput(dpy, root, NoEventMask);
    return 0;
}

// Translate a window's (0,0) origin to root coordinates. xconfigure.x/y
// on a reparented client is frame-relative, not screen-relative, so
// we re-translate every time we follow the bundle.
static bool translate_window_to_root(Display *dpy, Window w,
                                     int *rx, int *ry) {
    Window root = DefaultRootWindow(dpy);
    Window child;
    return XTranslateCoordinates(dpy, w, root, 0, 0, rx, ry, &child)
           ? true : false;
}

// Hint chord on a *managed* chrome window (UTILITY + transient_for +
// Motif decorations=0) is the EWMH-clean approach in theory, but in
// practice every reparenting WM still wraps chrome in a frame and
// reinterprets our XMoveResizeWindow against frame coordinates — so
// chrome ends up shifted by the frame extents, not pinned to the
// bundle's actual client origin. The Aqua-chrome use case is a fake
// decoration, not a managed window: we want absolute pixel placement,
// no negotiation. That's the override-redirect idiom in X11.
//
// Override-redirect side effects we accept:
//   * No taskbar entry — fine, chrome is decoration not an app window.
//   * No automatic stacking — we re-raise chrome above the bundle on
//     every ConfigureNotify, which keeps it visually attached.
//   * No WM-managed focus — a click on chrome won't auto-activate the
//     bundle, so 19.D-β-3 wires ButtonPress on the chrome window into
//     explicit forwarding (traffic-light actions + _NET_ACTIVE_WINDOW
//     pager-class messages). Without that the user sees decorations
//     they can't interact with.
//
// WM_CLASS, _NET_WM_NAME, and WM_TRANSIENT_FOR are still set —
// purely informational (taskbar grouping, _NET_CLIENT_LIST_STACKING,
// debugging). The WM is told not to manage chrome via override-
// redirect, so these don't affect placement.
static void set_chrome_window_metadata(Display *dpy, Window chrome,
                                       Window bundle) {
    XSetTransientForHint(dpy, chrome, bundle);
}

// Hit-test a click against the title-bar traffic lights. Returns
// 1=close, 2=minimize, 3=zoom, 0=miss. Mirrors the canonical
// hit_test_button() in moonrock/src/events.c — same constants,
// same scaling rule, same return convention. Geometry constants come
// from host_chrome.h (MB_CHROME_BUTTON_*) — single source of truth
// shared with moonrock's wm.h aliases. Inlined here (rather than
// extracted into menubar_render) because the SL traffic-light glyphs
// and gradient still live in moonrock/src/moonbase_chrome.c; pulling
// both paint and hit-test belongs to slice 19.H.1.b.
static int chrome_stub_hit_test_button(int fx, int fy, double scale) {
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

// Send _NET_ACTIVE_WINDOW to root requesting bundle_win be activated.
// source=2 (pager-class) is correct: the chrome stub is acting as a
// delegate for user input, not as a normal app sending its own window
// to the front. EWMH-compliant WMs (KWin, Mutter, Xfwm) honour pager
// requests unconditionally; source=1 (normal app) gets focus-stealing-
// prevention rejection on a backgrounded chrome process.
static void chrome_stub_send_active_window(Display *dpy, Window root,
                                           Window bundle_win,
                                           Atom net_active_win) {
    XEvent ev = {0};
    ev.type                 = ClientMessage;
    ev.xclient.window       = bundle_win;
    ev.xclient.message_type = net_active_win;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = 2;            // source: pager
    ev.xclient.data.l[1]    = CurrentTime;
    ev.xclient.data.l[2]    = 0;            // requestor's currently-active window: none
    XSendEvent(dpy, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

// Send WM_DELETE_WINDOW to bundle_win if it advertises support, falling
// back to XKillClient if it doesn't. ICCCM 4.2.8.1: WM_DELETE goes to
// the *client*, not root — unlike the EWMH state messages below.
static void chrome_stub_send_delete(Display *dpy, Window bundle_win,
                                    Atom wm_protocols, Atom wm_delete) {
    Atom *protos = NULL;
    int n_protos = 0;
    bool supported = false;
    if (XGetWMProtocols(dpy, bundle_win, &protos, &n_protos)) {
        for (int i = 0; i < n_protos; i++) {
            if (protos[i] == wm_delete) { supported = true; break; }
        }
        if (protos) XFree(protos);
    }
    if (supported) {
        XEvent ev = {0};
        ev.type                 = ClientMessage;
        ev.xclient.window       = bundle_win;
        ev.xclient.message_type = wm_protocols;
        ev.xclient.format       = 32;
        ev.xclient.data.l[0]    = (long)wm_delete;
        ev.xclient.data.l[1]    = CurrentTime;
        XSendEvent(dpy, bundle_win, False, NoEventMask, &ev);
    } else {
        XKillClient(dpy, bundle_win);
    }
}

// Send WM_CHANGE_STATE / IconicState to root — the ICCCM minimize
// request. Reparenting WMs map this to their per-window minimise/iconify
// flow (KWin, Mutter, Xfwm, moonrock all comply).
static void chrome_stub_send_minimize(Display *dpy, Window root,
                                      Window bundle_win,
                                      Atom wm_change_state) {
    XEvent ev = {0};
    ev.type                 = ClientMessage;
    ev.xclient.window       = bundle_win;
    ev.xclient.message_type = wm_change_state;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = 3;            // IconicState (X11/Xutil.h)
    XSendEvent(dpy, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

// Toggle the EWMH maximised state — the SL "zoom" button. data.l[0]=2
// means "toggle"; data.l[1]/l[2] are the two atoms being toggled
// together (vertical + horizontal). source=2 (pager-class), same
// reasoning as _NET_ACTIVE_WINDOW above.
static void chrome_stub_send_zoom(Display *dpy, Window root,
                                  Window bundle_win,
                                  Atom net_wm_state,
                                  Atom net_wm_state_max_vert,
                                  Atom net_wm_state_max_horz) {
    XEvent ev = {0};
    ev.type                 = ClientMessage;
    ev.xclient.window       = bundle_win;
    ev.xclient.message_type = net_wm_state;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = 2;            // _NET_WM_STATE_TOGGLE
    ev.xclient.data.l[1]    = (long)net_wm_state_max_vert;
    ev.xclient.data.l[2]    = (long)net_wm_state_max_horz;
    ev.xclient.data.l[3]    = 2;            // source: pager
    XSendEvent(dpy, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

// ── Dropdown popup ──────────────────────────────────────────────────
// Slice 19.D-β-4 — a click on a top-level menu title in the chrome row
// opens a flat, single-level popup of that menu's children. The full
// daemon's appmenu.c maintains a 4-deep stack so submenus can drill in
// (Qt builds File → Recent → <files>); the chrome stub deliberately
// stays one level deep — leaf items dispatch via the bound
// DbusMenuClient and submenu items just close. Submenu drill-down is a
// later slice once the bundle window's foreign-distro chrome is solid.
//
// Layout matches appmenu.c lines 596-820: ROW_H_ITEM=22pt, ROW_H_SEP=7pt,
// 4pt top/bottom panel padding, S(200) min width, S(40) per-item label
// pad + S(30) shortcut gutter + S(18) submenu-arrow column. Rendered
// rows go through the shared menubar_render_paint_menu_item primitive,
// so pixels are bit-identical to the daemon dropdown — same selection
// pill (#3875D7), same toggle glyphs, same shortcut tier colours.

typedef struct {
    bool             open;
    Window           win;
    cairo_surface_t *surf;
    cairo_t         *cr;
    int              w_px;
    int              h_px;
    // Borrowed: lives inside the bound DbusMenuClient's MenuNode root.
    // dbusmenu_client refetches replace the root wholesale, so a refetch
    // mid-popup invalidates this pointer; we close on legacy_dirty before
    // the rebuild walks the new tree.
    const MenuNode  *parent;
    int              hover_row;          // visible-row index, -1 = none
} chrome_stub_dropdown_t;

// Visible-row iteration: skips !visible children. Separators are visible
// rows that consume vertical space but aren't hoverable/clickable.
static const MenuNode *
chrome_stub_dropdown_at_y(const chrome_stub_dropdown_t *d, int fy,
                          double scale, int *out_row_idx,
                          bool *out_clickable) {
    *out_row_idx   = -1;
    *out_clickable = false;
    if (!d || !d->parent) return NULL;

    int top_pad    = (int)(4  * scale + 0.5);
    int row_h_item = (int)(22 * scale + 0.5);
    int row_h_sep  = (int)(7  * scale + 0.5);
    int y = top_pad;
    int row_idx = 0;
    for (int i = 0; i < d->parent->n_children; i++) {
        const MenuNode *c = d->parent->children[i];
        if (!c || !c->visible) continue;
        int rh = (c->type == MENU_ITEM_SEPARATOR) ? row_h_sep : row_h_item;
        if (fy >= y && fy < y + rh) {
            *out_row_idx   = row_idx;
            *out_clickable = (c->type != MENU_ITEM_SEPARATOR && c->enabled);
            return c;
        }
        y += rh;
        row_idx++;
    }
    return NULL;
}

static int chrome_stub_dropdown_measure_w(const MenuNode *parent,
                                          double scale) {
    int min_w = (int)(200 * scale + 0.5);
    int max_w = min_w;
    for (int i = 0; i < parent->n_children; i++) {
        const MenuNode *c = parent->children[i];
        if (!c || !c->visible) continue;
        if (c->type == MENU_ITEM_SEPARATOR) continue;
        if (!c->label) continue;

        double label_w = menubar_render_measure_text(c->label, false, scale);
        int extra = 0;
        if (c->shortcut && *c->shortcut) {
            double sw = menubar_render_measure_text(c->shortcut, false, scale);
            extra += (int)sw + (int)(30 * scale + 0.5);
        }
        if (c->is_submenu) {
            extra += (int)(18 * scale + 0.5);
        }
        int needed = (int)label_w + (int)(40 * scale + 0.5) + extra;
        if (needed > max_w) max_w = needed;
    }
    return max_w;
}

static int chrome_stub_dropdown_measure_h(const MenuNode *parent,
                                          double scale) {
    int row_h_item = (int)(22 * scale + 0.5);
    int row_h_sep  = (int)(7  * scale + 0.5);
    int h = (int)(8 * scale + 0.5);    // 4pt top + 4pt bottom padding
    for (int i = 0; i < parent->n_children; i++) {
        const MenuNode *c = parent->children[i];
        if (!c || !c->visible) continue;
        h += (c->type == MENU_ITEM_SEPARATOR) ? row_h_sep : row_h_item;
    }
    return h;
}

static void chrome_stub_paint_dropdown(chrome_stub_dropdown_t *d,
                                       menubar_render_theme_t theme,
                                       double scale) {
    if (!d || !d->open || !d->cr || !d->parent) return;
    cairo_t *cr = d->cr;

    // White panel + 1px gray border. The daemon paints SL's translucent
    // 245/255 white with a rounded 5pt radius and shadowed border; the
    // chrome stub is drawn into a plain RGB X window without a compositor
    // alpha channel, so opaque white + a thin 0.25 alpha border is the
    // closest match the surface can carry. Rounding/shadow becomes
    // possible once the launcher pulls in the same ARGB visual path the
    // daemon uses.
    cairo_save(cr);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.25);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 0.5, 0.5, d->w_px - 1, d->h_px - 1);
    cairo_stroke(cr);
    cairo_restore(cr);

    int top_pad    = (int)(4  * scale + 0.5);
    int row_h_item = (int)(22 * scale + 0.5);
    int row_h_sep  = (int)(7  * scale + 0.5);
    int side_inset = (int)(10 * scale + 0.5);

    int y = top_pad;
    int row_idx = 0;
    for (int i = 0; i < d->parent->n_children; i++) {
        const MenuNode *c = d->parent->children[i];
        if (!c || !c->visible) continue;

        if (c->type == MENU_ITEM_SEPARATOR) {
            // 1px #D0D0D0 horizontal rule, vertically centered in the
            // 7pt separator row. menubar_render_paint_menu_item is for
            // item rows only; separators are intentionally drawn here
            // inline (header note at menubar_render.h:82).
            cairo_save(cr);
            cairo_set_source_rgb(cr,
                                 208.0/255.0, 208.0/255.0, 208.0/255.0);
            cairo_set_line_width(cr, 1.0);
            double yy = y + (row_h_sep / 2.0) + 0.5;
            cairo_move_to(cr, side_inset,           yy);
            cairo_line_to(cr, d->w_px - side_inset, yy);
            cairo_stroke(cr);
            cairo_restore(cr);
            y += row_h_sep;
            row_idx++;
            continue;
        }

        menubar_render_toggle_t tog = MENUBAR_TOGGLE_NONE;
        if (c->toggle == MENU_TOGGLE_CHECKMARK && c->toggle_state == 1) {
            tog = MENUBAR_TOGGLE_CHECKMARK;
        } else if (c->toggle == MENU_TOGGLE_RADIO && c->toggle_state == 1) {
            tog = MENUBAR_TOGGLE_RADIO;
        }

        menubar_render_menu_item_t mi = {
            .label      = c->label ? c->label : "",
            .toggle     = tog,
            .enabled    = c->enabled,
            .is_submenu = c->is_submenu,
            .shortcut   = c->shortcut,
        };
        bool hovered = (row_idx == d->hover_row);
        menubar_render_paint_menu_item(cr, 0, y, d->w_px, row_h_item,
                                       &mi, hovered, theme, scale);
        y += row_h_item;
        row_idx++;
    }

    cairo_surface_flush(d->surf);
}

// Open a flat dropdown for the top-level menu identified by
// `parent_legacy_id`. Returns true if a popup actually mapped (false on
// missing tree, missing matching id, or empty children).
//
// `dbusmenu_client_about_to_show` is called *before* reading children:
// Qt's appmenu module lazy-builds submenu contents and won't populate
// them otherwise. The reply is async on the GLib main context — by the
// time we paint, the main loop has had a chance to dispatch the
// LayoutUpdated signal that follows. If it hasn't yet, we open with
// whatever's there and let the next refetch repaint the popup via the
// chrome_stub_on_menu_changed → legacy_dirty path (which closes-and-
// re-opens because the borrowed `parent` pointer is invalidated).
static bool chrome_stub_open_dropdown(Display *dpy, Window root, int screen,
                                      Visual *vis,
                                      DbusMenuClient *dbusmenu,
                                      int32_t parent_legacy_id,
                                      int root_x, int root_y,
                                      menubar_render_theme_t theme,
                                      double scale,
                                      chrome_stub_dropdown_t *d) {
    if (!dbusmenu || !d || d->open) return false;

    dbusmenu_client_about_to_show(dbusmenu, parent_legacy_id);

    const MenuNode *root_node = dbusmenu_client_root(dbusmenu);
    if (!root_node) return false;

    const MenuNode *parent = NULL;
    for (int i = 0; i < root_node->n_children; i++) {
        const MenuNode *c = root_node->children[i];
        if (!c || !c->visible) continue;
        if (c->legacy_id == parent_legacy_id) { parent = c; break; }
    }
    if (!parent || parent->n_children == 0) return false;

    int w = chrome_stub_dropdown_measure_w(parent, scale);
    int h = chrome_stub_dropdown_measure_h(parent, scale);

    XSetWindowAttributes wa = {0};
    wa.background_pixel  = WhitePixel(dpy, screen);
    wa.event_mask        = ExposureMask | ButtonPressMask
                         | PointerMotionMask | LeaveWindowMask
                         | KeyPressMask;
    wa.override_redirect = True;

    Window win = XCreateWindow(
        dpy, root,
        root_x, root_y, w, h, 0,
        DefaultDepth(dpy, screen),
        InputOutput, vis,
        CWBackPixel | CWEventMask | CWOverrideRedirect, &wa);

    XMapWindow(dpy, win);
    // Round-trip the map *before* attempting the grab; XGrabPointer
    // returns GrabNotViewable on a window the server hasn't yet
    // acknowledged as mapped (common gotcha — same fix as GTK menus).
    XSync(dpy, False);

    int gp = XGrabPointer(dpy, win, False,
                          ButtonPressMask | ButtonReleaseMask
                          | PointerMotionMask | LeaveWindowMask,
                          GrabModeAsync, GrabModeAsync,
                          None, None, CurrentTime);
    if (gp != GrabSuccess) {
        fprintf(stderr,
                "[moonrock-lite %d] XGrabPointer for popup failed (%d) — "
                "outside-click dismissal will not work this open\n",
                getpid(), gp);
    }
    // Keyboard grab so Escape lands on the popup, not on whichever
    // window has focus. Failure is non-fatal — Escape just won't dismiss.
    XGrabKeyboard(dpy, win, False,
                  GrabModeAsync, GrabModeAsync, CurrentTime);

    d->open      = true;
    d->win       = win;
    d->surf      = cairo_xlib_surface_create(dpy, win, vis, w, h);
    d->cr        = cairo_create(d->surf);
    d->w_px      = w;
    d->h_px      = h;
    d->parent    = parent;
    d->hover_row = -1;

    // Paint immediately rather than waiting for Expose — XGrabPointer
    // can swallow the synthetic Expose under some WMs, leaving the
    // popup blank for a frame. theme is passed through so the Aqua /
    // host-tint variants render right from the first paint.
    chrome_stub_paint_dropdown(d, theme, scale);
    XFlush(dpy);
    return true;
}

static void chrome_stub_close_dropdown(Display *dpy,
                                       chrome_stub_dropdown_t *d) {
    if (!d || !d->open) return;
    XUngrabKeyboard(dpy, CurrentTime);
    XUngrabPointer(dpy, CurrentTime);
    if (d->cr)   { cairo_destroy(d->cr);            d->cr   = NULL; }
    if (d->surf) { cairo_surface_destroy(d->surf);  d->surf = NULL; }
    if (d->win)  { XDestroyWindow(dpy, d->win);     d->win  = None; }
    d->open      = false;
    d->parent    = NULL;
    d->w_px      = 0;
    d->h_px      = 0;
    d->hover_row = -1;
    XFlush(dpy);
}

// 19.D-γ — Probe the effective UI scale at a point in root coordinates.
//
// Two-tier strategy:
//   1. Fast path: parse _MOONROCK_OUTPUT_SCALES, the per-output atom that
//      moonrock publishes inside a full CopyCatOS session. Wire format
//      (one row per output, newline-separated):
//          "<name> <x> <y> <w> <h> <effective> <primary> <rot> <mult>"
//      Field 6 is already-folded effective_scale = backing × multiplier;
//      don't re-multiply. The atom is absent on foreign distros, which
//      is fine — we fall through to XRandR.
//   2. Fallback: walk XRandR outputs, find the CRTC that contains
//      (cx, cy), compute PPI from the CRTC's pixel width and the
//      output's EDID-reported mm_width (rotation-corrected), then map
//      to the discrete scale ladder from the HiDPI mandate:
//        ≲160 → 1.0×, 160–210 → 1.25×, 210–260 → 1.5×,
//        260–320 → 1.75×, ≥320 → 2.0×
//      mm_width == 0 (no EDID) or no enclosing CRTC → 1.0×.
//
// Returns 1.0 on any failure path, never less.
static double chrome_stub_probe_scale(Display *dpy, Window root,
                                      int cx, int cy) {
    Atom scales_atom = XInternAtom(dpy, "_MOONROCK_OUTPUT_SCALES", True);
    if (scales_atom != None) {
        Atom actual_type = None;
        int actual_format = 0;
        unsigned long nitems = 0, bytes_after = 0;
        unsigned char *prop = NULL;
        if (XGetWindowProperty(dpy, root, scales_atom, 0, (~0L), False,
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

    int xrr_event_base = 0, xrr_error_base = 0;
    if (!XRRQueryExtension(dpy, &xrr_event_base, &xrr_error_base)) {
        return 1.0;
    }

    XRRScreenResources *res = XRRGetScreenResources(dpy, root);
    if (!res) return 1.0;

    double picked = 1.0;
    for (int i = 0; i < res->noutput; i++) {
        XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
        if (!oi) continue;
        if (oi->connection != RR_Connected || oi->crtc == None) {
            XRRFreeOutputInfo(oi);
            continue;
        }
        XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
        if (!ci) { XRRFreeOutputInfo(oi); continue; }

        if (cx >= ci->x && cx < ci->x + (int)ci->width &&
            cy >= ci->y && cy < ci->y + (int)ci->height) {
            // Rotation: 90/270 swaps the panel's natural axes, so the
            // CRTC's width corresponds to the EDID's mm_height.
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
// Chrome run loop — extracted verbatim from moonbase-launcher.c's
// run_chrome_stub(). Returns 0 on a clean teardown, non-zero on hard
// init failure (no display, no bundle window before timeout).
// ----------------------------------------------------------------------------

static int run_chrome(const char *bundle_id,
                      const char *display_name,
                      menubar_render_theme_t theme) {
    // Re-arm parent-death-signal in case the launcher's pre-exec arm
    // didn't survive (setuid binaries reset it; we're not setuid but
    // belt+suspenders is cheap).
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    fprintf(stderr, "[moonrock-lite %d] start; bundle=%s theme=%d\n",
            getpid(), bundle_id, theme);
    fflush(stderr);

    struct sigaction sa = {0};
    sa.sa_handler = chrome_stub_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[moonrock-lite %d] XOpenDisplay failed\n", getpid());
        return 1;
    }
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    Visual *vis = DefaultVisual(dpy, screen);

    // Claim com.canonical.AppMenu.Registrar before the bundle's Qt/GTK
    // appmenu module fires its first RegisterWindow. The async name
    // acquisition completes inside chrome_stub_select pumps that follow.
    // Init failure (no session bus, introspection broken) is non-fatal —
    // chrome falls back to the bold-app-name layout.
    bool bridge_up = appmenu_bridge_init();
    if (!bridge_up) {
        fprintf(stderr,
                "[moonrock-lite %d] appmenu bridge init failed — "
                "no DBusMenu import this session\n", getpid());
    }

    // Discovery — wait for the bundle's first top-level to appear in
    // _NET_CLIENT_LIST with WM_CLASS instance matching bundle_id. 30s
    // budget covers cold starts; on a warm system the bundle arrives
    // in ~1s. wait_for_bundle_window's select pumps the GLib main
    // context so RegisterWindow dispatches even while we're blocked
    // on the WM publishing the new client.
    Window bundle_win = wait_for_bundle_window(dpy, root, bundle_id, 30);
    if (!bundle_win) {
        if (chrome_stub_should_exit) {
            fprintf(stderr,
                    "[moonrock-lite %d] exiting (signaled) before bundle "
                    "window for '%s' was located\n",
                    getpid(), bundle_id);
        } else {
            fprintf(stderr,
                    "[moonrock-lite %d] bundle window for '%s' did not appear "
                    "within 30s — exiting (no chrome to draw)\n",
                    getpid(), bundle_id);
        }
        if (bridge_up) appmenu_bridge_shutdown();
        XCloseDisplay(dpy);
        return 0;
    }
    fprintf(stderr, "[moonrock-lite %d] bundle window 0x%lx located\n",
            getpid(), bundle_win);
    fflush(stderr);

    XWindowAttributes attrs;
    if (!XGetWindowAttributes(dpy, bundle_win, &attrs)) {
        fprintf(stderr,
                "[moonrock-lite %d] XGetWindowAttributes failed; exiting\n",
                getpid());
        if (bridge_up) appmenu_bridge_shutdown();
        XCloseDisplay(dpy);
        return 1;
    }
    int bundle_w = attrs.width;
    int bundle_h = attrs.height;
    int bundle_root_x = 0, bundle_root_y = 0;
    translate_window_to_root(dpy, bundle_win,
                             &bundle_root_x, &bundle_root_y);

    // Track the bundle for placement + lifecycle.
    XSelectInput(dpy, bundle_win, StructureNotifyMask);

    // 19.D-γ — Subscribe to RandR screen-change events so a mode/scale
    // change on the bundle's output rerenders chrome inside one event
    // round-trip. xrr_event_base is added to RRScreenChangeNotify in
    // the loop's event match. Foreign distros without XRandR keep
    // working — the probe + extension query both degrade to no-op.
    int  xrr_event_base = 0, xrr_error_base = 0;
    bool have_xrr = XRRQueryExtension(dpy, &xrr_event_base, &xrr_error_base);
    if (have_xrr) {
        XRRSelectInput(dpy, root, RRScreenChangeNotifyMask);
    }

    // Chrome geometry — Menu Bar Law: chrome_h = 22pt × effective_scale,
    // re-evaluated when the bundle moves between outputs (ConfigureNotify)
    // or the output's scale itself changes (RRScreenChangeNotify). The
    // bundle's centre is the probe point — splits straddling outputs
    // resolve to the output owning the majority of pixels.
    double scale = chrome_stub_probe_scale(
        dpy, root,
        bundle_root_x + bundle_w / 2,
        bundle_root_y + bundle_h / 2);
    int title_h_px = (int)(menubar_render_title_bar_height_pts() * scale);
    int menu_h_px  = (int)(menubar_render_menu_bar_height_pts()  * scale);
    int chrome_h   = title_h_px + menu_h_px;
    int chrome_w   = bundle_w;
    fprintf(stderr,
            "[moonrock-lite %d] initial scale=%.2f title_h=%d menu_h=%d\n",
            getpid(), scale, title_h_px, menu_h_px);

    XSetWindowAttributes wa = {0};
    wa.background_pixel    = WhitePixel(dpy, screen);
    wa.event_mask          = ExposureMask | StructureNotifyMask
                           | ButtonPressMask;
    wa.override_redirect   = True;

    Window chrome_win = XCreateWindow(
        dpy, root,
        bundle_root_x, bundle_root_y - chrome_h,
        chrome_w, chrome_h, 0,
        DefaultDepth(dpy, screen),
        InputOutput, vis,
        CWBackPixel | CWEventMask | CWOverrideRedirect, &wa);

    XStoreName(dpy, chrome_win, display_name);
    XClassHint ch = {(char *)bundle_id, (char *)bundle_id};
    XSetClassHint(dpy, chrome_win, &ch);

    set_chrome_window_metadata(dpy, chrome_win, bundle_win);

    // _NET_ACTIVE_WINDOW on root tracks which client the user is talking
    // to. We re-raise chrome when the bundle becomes active so override-
    // redirect chrome stays visually stuck to its bundle.
    //
    // SubstructureNotifyMask on root: ICCCM says reparenting WMs MUST
    // send a synthetic ConfigureNotify to the client when its absolute
    // position changes, so our XSelectInput on bundle_win should be
    // enough on a compliant WM (KWin, Mutter). It isn't enough on every
    // real WM — moonrock for example only fires ConfigureNotify on
    // *resize*, not on a pure move, leaving chrome stranded. Listening
    // for ConfigureNotify on root catches the WM frame moving, which
    // covers the moonrock case and is harmless duplication elsewhere.
    Atom net_active_win           = XInternAtom(dpy, "_NET_ACTIVE_WINDOW",            False);
    Atom wm_protocols             = XInternAtom(dpy, "WM_PROTOCOLS",                  False);
    Atom wm_delete_window         = XInternAtom(dpy, "WM_DELETE_WINDOW",              False);
    Atom wm_change_state          = XInternAtom(dpy, "WM_CHANGE_STATE",               False);
    Atom net_wm_state             = XInternAtom(dpy, "_NET_WM_STATE",                 False);
    Atom net_wm_state_max_vert    = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT",  False);
    Atom net_wm_state_max_horz    = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ",  False);
    {
        XWindowAttributes ra;
        XGetWindowAttributes(dpy, root, &ra);
        XSelectInput(dpy, root,
                     ra.your_event_mask
                     | PropertyChangeMask
                     | SubstructureNotifyMask);
    }

    XMapWindow(dpy, chrome_win);
    XRaiseWindow(dpy, chrome_win);
    XFlush(dpy);

    cairo_surface_t *surf =
        cairo_xlib_surface_create(dpy, chrome_win, vis, chrome_w, chrome_h);
    cairo_t *cr = cairo_create(surf);

    menubar_render_init();

    // Menu state — items[] starts with the bold bundle name only.
    // DBusMenu refetches replace items[1..] in chrome_stub_rebuild_items
    // when on_changed flips legacy_dirty. Until the bridge binds (or
    // forever, on KDE / nil-state hosts) the stub paints the bundle
    // name slot only, no fake File/Edit/View row.
    chrome_stub_state_t state = {0};
    state.display_name = display_name;
    chrome_stub_rebuild_items(&state);

    chrome_stub_dropdown_t dropdown = {0};

    int cur_w = chrome_w;
    int cur_h = chrome_h;
    int cur_title_h = title_h_px;
    int cur_menu_h  = menu_h_px;

    int x_fd = ConnectionNumber(dpy);
    time_t lookup_start = time(NULL);
    bool need_paint = true; // first paint after map

    while (!chrome_stub_should_exit) {
        // Lazy bind to the bundle's DBusMenu service. RegisterWindow
        // usually arrives before bundle discovery, but Qt/GTK platform-
        // theme load can race; retry each tick until we hit or time out.
        // After CHROME_STUB_LOOKUP_SECS we accept the bold-name layout.
        if (bridge_up && !state.dbusmenu &&
            (time(NULL) - lookup_start) <= CHROME_STUB_LOOKUP_SECS) {
            const char *service = NULL;
            const char *path    = NULL;
            if (appmenu_bridge_lookup((uint32_t)bundle_win,
                                      &service, &path)) {
                fprintf(stderr,
                        "[moonrock-lite %d] DBusMenu bind wid=0x%lx "
                        "service=%s path=%s\n",
                        getpid(), bundle_win, service, path);
                state.dbusmenu = dbusmenu_client_new(
                    service, path,
                    chrome_stub_on_menu_changed, &state);
                // Initial GetLayout is async — on_changed will fire
                // when the tree arrives and rebuild_items will run on
                // the next iteration.
            }
        }

        // 250ms wake budget keeps the lookup retry responsive without
        // burning CPU. Larger budgets are fine — X events and glib
        // sources both wake select() the moment they need attention.
        (void)chrome_stub_select(x_fd, 250);

        // Drain X events. Geometry changes flip need_paint; lifecycle
        // exits the loop. _NET_ACTIVE_WINDOW raises chrome but doesn't
        // need a repaint by itself.
        while (XPending(dpy) > 0 && !chrome_stub_should_exit) {
            XEvent ev;
            XNextEvent(dpy, &ev);

            if (ev.xany.window == bundle_win &&
                (ev.type == UnmapNotify || ev.type == DestroyNotify)) {
                chrome_stub_should_exit = 1;
                break;
            }

            // ── Popup event branch ─────────────────────────────────
            // Funnels every event whose target is the dropdown window
            // through the popup state machine. With XGrabPointer
            // owner_events=False (no confine_to), button presses
            // outside the popup still arrive here — coords are reported
            // relative to the popup origin, so anything outside the
            // [0..w_px) × [0..h_px) box is the click-outside case.
            if (dropdown.open && ev.xany.window == dropdown.win) {
                if (ev.type == Expose) {
                    chrome_stub_paint_dropdown(&dropdown, theme, scale);
                    XFlush(dpy);
                    continue;
                }
                if (ev.type == ButtonPress &&
                    ev.xbutton.button == Button1) {
                    int fx = ev.xbutton.x;
                    int fy = ev.xbutton.y;
                    if (fx < 0 || fy < 0 ||
                        fx >= dropdown.w_px || fy >= dropdown.h_px) {
                        chrome_stub_close_dropdown(dpy, &dropdown);
                        continue;
                    }
                    int row_idx = -1;
                    bool clickable = false;
                    const MenuNode *node = chrome_stub_dropdown_at_y(
                        &dropdown, fy, scale, &row_idx, &clickable);
                    if (node && clickable && state.dbusmenu &&
                        !node->is_submenu) {
                        // Leaf row: dispatch the dbusmenu Event then
                        // close. Submenu drill-down stays a future
                        // slice — chrome stub is one level deep.
                        dbusmenu_client_activate(state.dbusmenu,
                                                 node->legacy_id);
                        chrome_stub_close_dropdown(dpy, &dropdown);
                    } else if (node && node->is_submenu) {
                        // No drill-down yet — close so the popup isn't
                        // a dead-end. Once submenu support lands here
                        // this branch opens the next stack level.
                        chrome_stub_close_dropdown(dpy, &dropdown);
                    }
                    // Separator / disabled hits are ignored — pointer
                    // stays grabbed, popup stays open.
                    continue;
                }
                if (ev.type == MotionNotify) {
                    int fx = ev.xmotion.x;
                    int fy = ev.xmotion.y;
                    int new_hover = -1;
                    if (fx >= 0 && fx < dropdown.w_px &&
                        fy >= 0 && fy < dropdown.h_px) {
                        int row_idx = -1;
                        bool clickable = false;
                        (void)chrome_stub_dropdown_at_y(
                            &dropdown, fy, scale,
                            &row_idx, &clickable);
                        if (clickable) new_hover = row_idx;
                    }
                    if (new_hover != dropdown.hover_row) {
                        dropdown.hover_row = new_hover;
                        chrome_stub_paint_dropdown(&dropdown,
                                                   theme, scale);
                        XFlush(dpy);
                    }
                    continue;
                }
                if (ev.type == LeaveNotify) {
                    if (dropdown.hover_row != -1) {
                        dropdown.hover_row = -1;
                        chrome_stub_paint_dropdown(&dropdown,
                                                   theme, scale);
                        XFlush(dpy);
                    }
                    continue;
                }
                if (ev.type == KeyPress) {
                    KeySym ks = XLookupKeysym(&ev.xkey, 0);
                    if (ks == XK_Escape) {
                        chrome_stub_close_dropdown(dpy, &dropdown);
                    }
                    continue;
                }
            }

            if (ev.type == PropertyNotify &&
                ev.xany.window == root &&
                ev.xproperty.atom == net_active_win) {
                Atom t; int f;
                unsigned long n = 0, ba = 0;
                unsigned char *d = NULL;
                if (XGetWindowProperty(dpy, root, net_active_win, 0, 1,
                                       False, XA_WINDOW, &t, &f, &n,
                                       &ba, &d) == Success
                    && d && n == 1 && *(Window *)d == bundle_win) {
                    XRaiseWindow(dpy, chrome_win);
                }
                if (d) XFree(d);
                continue;
            }

            if (ev.type == ConfigureNotify) {
                XWindowAttributes a;
                int rx = 0, ry = 0;
                if (!XGetWindowAttributes(dpy, bundle_win, &a)) {
                    chrome_stub_should_exit = 1;
                    break;
                }
                if (!translate_window_to_root(dpy, bundle_win,
                                              &rx, &ry)) {
                    continue;
                }
                int nw = a.width;
                int nh = a.height;
                bool moved   = (rx != bundle_root_x || ry != bundle_root_y);
                bool resized = (nw != chrome_w || nh != bundle_h);

                // Re-probe scale at the new bundle centre. A move across
                // outputs of different scale flips chrome_h here; a same-
                // output resize leaves the integer chrome_h untouched and
                // skips the close-popup/cairo-resize cost below.
                double new_scale = chrome_stub_probe_scale(
                    dpy, root, rx + nw / 2, ry + nh / 2);
                int new_title_h = (int)(
                    menubar_render_title_bar_height_pts() * new_scale);
                int new_menu_h  = (int)(
                    menubar_render_menu_bar_height_pts()  * new_scale);
                int new_chrome_h = new_title_h + new_menu_h;

                bool scaled = (new_chrome_h != chrome_h);

                if (!moved && !resized && !scaled) continue;

                bundle_root_x = rx;
                bundle_root_y = ry;
                bundle_h      = nh;
                chrome_w      = nw;

                if (scaled) {
                    // Borrowed surface dims will change; popup parent
                    // pointers and cached row geometry no longer match.
                    if (dropdown.open) {
                        chrome_stub_close_dropdown(dpy, &dropdown);
                    }
                    scale       = new_scale;
                    title_h_px  = new_title_h;
                    menu_h_px   = new_menu_h;
                    chrome_h    = new_chrome_h;
                    fprintf(stderr,
                            "[moonrock-lite %d] scale=%.2f title_h=%d "
                            "menu_h=%d (output change)\n",
                            getpid(), scale, title_h_px, menu_h_px);
                }

                XMoveResizeWindow(dpy, chrome_win,
                                  rx, ry - chrome_h,
                                  nw, chrome_h);
                XRaiseWindow(dpy, chrome_win);
                cur_w       = nw;
                cur_h       = chrome_h;
                cur_title_h = title_h_px;
                cur_menu_h  = menu_h_px;
                cairo_xlib_surface_set_size(surf, cur_w, cur_h);
                need_paint  = true;
                continue;
            }

            // 19.D-γ — RandR screen change: a mode/rotation/scale change
            // on the host. Always reprobe — the bundle's output may have
            // flipped scale without a single ConfigureNotify firing on
            // the bundle window. Same close-popup-on-scale-change rule.
            if (have_xrr &&
                ev.type == xrr_event_base + RRScreenChangeNotify) {
                XRRUpdateConfiguration(&ev);
                double new_scale = chrome_stub_probe_scale(
                    dpy, root,
                    bundle_root_x + chrome_w / 2,
                    bundle_root_y + bundle_h / 2);
                int new_title_h = (int)(
                    menubar_render_title_bar_height_pts() * new_scale);
                int new_menu_h  = (int)(
                    menubar_render_menu_bar_height_pts()  * new_scale);
                int new_chrome_h = new_title_h + new_menu_h;
                if (new_chrome_h == chrome_h) continue;

                if (dropdown.open) {
                    chrome_stub_close_dropdown(dpy, &dropdown);
                }
                scale       = new_scale;
                title_h_px  = new_title_h;
                menu_h_px   = new_menu_h;
                chrome_h    = new_chrome_h;
                fprintf(stderr,
                        "[moonrock-lite %d] scale=%.2f title_h=%d "
                        "menu_h=%d (RRScreenChangeNotify)\n",
                        getpid(), scale, title_h_px, menu_h_px);

                XMoveResizeWindow(dpy, chrome_win,
                                  bundle_root_x,
                                  bundle_root_y - chrome_h,
                                  chrome_w, chrome_h);
                XRaiseWindow(dpy, chrome_win);
                cur_w       = chrome_w;
                cur_h       = chrome_h;
                cur_title_h = title_h_px;
                cur_menu_h  = menu_h_px;
                cairo_xlib_surface_set_size(surf, cur_w, cur_h);
                need_paint  = true;
                continue;
            }

            if (ev.type == ButtonPress &&
                ev.xbutton.window == chrome_win &&
                ev.xbutton.button == Button1) {
                int fx = ev.xbutton.x;
                int fy = ev.xbutton.y;
                if (fy < cur_title_h) {
                    int hit = chrome_stub_hit_test_button(fx, fy, scale);
                    if (hit == 1) {
                        chrome_stub_send_delete(dpy, bundle_win,
                                                wm_protocols,
                                                wm_delete_window);
                        continue;
                    }
                    if (hit == 2) {
                        chrome_stub_send_minimize(dpy, root, bundle_win,
                                                  wm_change_state);
                        continue;
                    }
                    if (hit == 3) {
                        chrome_stub_send_zoom(dpy, root, bundle_win,
                                              net_wm_state,
                                              net_wm_state_max_vert,
                                              net_wm_state_max_horz);
                        continue;
                    }
                    // Empty title-bar area — fall through to activate.
                } else if (fy < cur_title_h + cur_menu_h) {
                    // Menu row: hit-test top-level slots[1..n] (slot 0
                    // is the bold app name and has no DBusMenu source —
                    // a future slice may synthesize an Application menu
                    // there, but β-4 leaves it as a no-op click).
                    bool opened = false;
                    if (state.dbusmenu && !dropdown.open) {
                        for (size_t i = 1; i < state.n_items; i++) {
                            int sx = state.items[i].x;
                            int sw = state.items[i].width;
                            if (fx >= sx && fx < sx + sw) {
                                // Activate the bundle so a subsequent
                                // dbusmenu Event lands on a focused
                                // window (some Qt-appmenu apps gate
                                // accelerator delivery on focus).
                                chrome_stub_send_active_window(
                                    dpy, root, bundle_win,
                                    net_active_win);
                                int popup_x = bundle_root_x + sx;
                                int popup_y = bundle_root_y; // menu-bar bottom
                                (void)chrome_stub_open_dropdown(
                                    dpy, root, screen, vis,
                                    state.dbusmenu,
                                    state.top_level_ids[i],
                                    popup_x, popup_y,
                                    theme, scale, &dropdown);
                                opened = true;
                                break;
                            }
                        }
                    }
                    if (opened) continue;
                    // Slot miss, no DBusMenu bound, or popup already
                    // open — fall through to the activate-only branch
                    // so β-3's input-delegate behaviour is preserved
                    // for menu-row clicks that don't open a popup.
                }
                // Empty chrome area (title-bar gap, or menu row when
                // no slot was hit). Acting as the bundle's input
                // delegate, ask the WM to activate it so keystrokes
                // go to the right window.
                chrome_stub_send_active_window(dpy, root, bundle_win,
                                               net_active_win);
                continue;
            }

            if (ev.type == Expose) {
                need_paint = true;
                continue;
            }
        }

        if (state.legacy_dirty) {
            // The dbusmenu client replaces its root wholesale on
            // refetch — any popup's borrowed `parent` pointer is now
            // dangling. Close before rebuilding; the user can re-open
            // against the new tree on their next click.
            if (dropdown.open) chrome_stub_close_dropdown(dpy, &dropdown);
            chrome_stub_rebuild_items(&state);
            state.legacy_dirty = false;
            need_paint = true;
        }

        if (!need_paint || chrome_stub_should_exit) continue;
        need_paint = false;

        cairo_save(cr);
        menubar_render_paint_title_bar(cr, cur_w, cur_title_h,
                                       display_name, true,
                                       theme, scale);
        cairo_restore(cr);

        cairo_save(cr);
        cairo_translate(cr, 0, cur_title_h);
        menubar_render_layout_menus(state.items, state.n_items,
                                    /*origin_x=*/76, scale);
        menubar_render_paint_menu_bar(cr, cur_w, cur_menu_h,
                                      state.items, state.n_items,
                                      /*hover_index=*/-1,
                                      theme, scale);
        cairo_restore(cr);

        cairo_surface_flush(surf);
        XFlush(dpy);
    }

    if (dropdown.open)  chrome_stub_close_dropdown(dpy, &dropdown);
    if (state.dbusmenu) dbusmenu_client_free(state.dbusmenu);
    if (bridge_up)      appmenu_bridge_shutdown();
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    menubar_render_cleanup();
    XDestroyWindow(dpy, chrome_win);
    XCloseDisplay(dpy);
    return 0;
}

// ----------------------------------------------------------------------------
// Argv parsing + main
// ----------------------------------------------------------------------------

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s --bundle-id <id> [--display-name <name>]\n"
        "                  [--theme <0|1|2>]\n"
        "\n"
        "Sidecar Aqua chrome process for foreign-distro .app launches.\n"
        "Spawned by moonbase-launcher; not generally invoked by hand.\n"
        "\n"
        "Options:\n"
        "  --bundle-id <id>      WM_CLASS instance to match against the\n"
        "                        bundle's top-level (required)\n"
        "  --display-name <s>    Title rendered in the chrome bar\n"
        "                        (default: bundle-id)\n"
        "  --theme <n>           0=Aqua (default), 1=Breeze-light,\n"
        "                        2=Adwaita-light\n",
        argv0);
}

int main(int argc, char **argv) {
    const char *bundle_id    = NULL;
    const char *display_name = NULL;
    int         theme_int    = MENUBAR_THEME_AQUA;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bundle-id") == 0 && i + 1 < argc) {
            bundle_id = argv[++i];
        } else if (strcmp(argv[i], "--display-name") == 0 && i + 1 < argc) {
            display_name = argv[++i];
        } else if (strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
            char *end = NULL;
            long v = strtol(argv[++i], &end, 10);
            if (end && *end == '\0' && v >= 0 && v <= 2) {
                theme_int = (int)v;
            }
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "[moonrock-lite] unknown arg: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (!bundle_id || !*bundle_id) {
        usage(argv[0]);
        return 2;
    }
    if (!display_name) display_name = bundle_id;

    return run_chrome(bundle_id, display_name,
                      (menubar_render_theme_t)theme_int);
}
