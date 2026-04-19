// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase_xdnd.c — XDND → MB_IPC_DRAG_* bridge.
//
// Implements the drop-target side of the freedesktop XDND protocol
// (v5) for MoonBase surfaces' InputOnly proxies. The source of truth
// for the protocol is https://www.freedesktop.org/wiki/Specifications/XDND/
//
// State machine per active source (at most one at a time — XDND does
// not multiplex):
//
//   idle → XdndEnter → pending (no coords yet)
//   pending → XdndPosition hits our proxy → entered (emit ENTER);
//             XConvertSelection requested if text/uri-list advertised
//   entered → XdndPosition in content rect & coords changed → emit OVER
//   entered → XdndPosition outside content rect → emit LEAVE, back to
//             pending (target not ours anymore, but source still alive)
//   any   → XdndLeave → emit LEAVE (if entered), back to idle
//   entered → XdndDrop → fetch selection if not yet fetched; emit DROP
//             with URIs or LEAVE if URIs empty; reply XdndFinished;
//             back to idle
//   any   → SelectionNotify on our proxy → parse URIs into session
//
// The whole module lives off a single g_session struct because XDND
// sources only drive one drag at a time.

#include "moonbase_xdnd.h"

#include "moonbase_host.h"
#include "moonbase.h"                    // MB_MOD_*
#include "moonbase/ipc/kinds.h"          // MB_IPC_DRAG_*

#include <X11/Xatom.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ─────────────────────────────────────────────────────────────────────
// Atoms + globals
// ─────────────────────────────────────────────────────────────────────

static Display *g_dpy = NULL;
static Window   g_root = 0;
static bool     g_ready = false;

// XDND atoms. Cached once because XInternAtom on a live connection is
// cheap but not free, and these names show up on every drag.
static Atom a_XdndAware;
static Atom a_XdndEnter;
static Atom a_XdndPosition;
static Atom a_XdndLeave;
static Atom a_XdndDrop;
static Atom a_XdndStatus;
static Atom a_XdndFinished;
static Atom a_XdndSelection;
static Atom a_XdndTypeList;
static Atom a_XdndActionCopy;
static Atom a_text_uri_list;

// ─────────────────────────────────────────────────────────────────────
// Session state
// ─────────────────────────────────────────────────────────────────────

// Exactly one drag may be in flight per X connection. We still check
// the source window on every frame so a stale session from a crashed
// source can't confuse a fresh one.
typedef struct {
    bool    active;          // a session is currently bound
    bool    entered;         // we've emitted DRAG_ENTER for this session
    bool    uris_requested;  // we called XConvertSelection already
    bool    uris_ready;      // SelectionNotify landed with URIs
    bool    drop_pending;    // XdndDrop arrived; waiting on URIs to finish
    bool    advertises_uri_list;  // source offered text/uri-list
    Window  source;          // XDND source window (data.l[0])
    Window  proxy;           // our InputOnly proxy receiving messages
    uint32_t window_id;      // MoonBase window_id of that proxy
    uint32_t client_id;      // IPC client owning that window
    int     version;         // XDND version (1..5) extracted from Enter
    int     last_x_points;   // last emitted OVER coords (content-local)
    int     last_y_points;
    bool    last_coords_valid;
    uint32_t modifiers;      // cached on ENTER / refreshed on DROP
    // URI list parsed out of SelectionNotify.
    char  **uris;            // heap array of heap strings
    size_t  uri_count;
} mb_xdnd_session_t;

static mb_xdnd_session_t g_session;

// ─────────────────────────────────────────────────────────────────────
// Small helpers
// ─────────────────────────────────────────────────────────────────────

static void uris_free(mb_xdnd_session_t *s) {
    if (!s->uris) return;
    for (size_t i = 0; i < s->uri_count; i++) {
        free(s->uris[i]);
    }
    free(s->uris);
    s->uris = NULL;
    s->uri_count = 0;
    s->uris_ready = false;
}

static void session_reset(void) {
    uris_free(&g_session);
    memset(&g_session, 0, sizeof(g_session));
}

// X11 modifier state (XQueryPointer) → MB_MOD_* bitmask. XDND itself
// only carries modifiers in Position's data.l[1] low bit (ButtonMask),
// which isn't what clients want; we query the real mod state.
static uint32_t xmods_to_mb(unsigned int mask) {
    uint32_t mods = 0;
    if (mask & ShiftMask)   mods |= MB_MOD_SHIFT;
    if (mask & ControlMask) mods |= MB_MOD_CONTROL;
    if (mask & Mod1Mask)    mods |= MB_MOD_OPTION;
    if (mask & Mod4Mask)    mods |= MB_MOD_COMMAND;
    if (mask & LockMask)    mods |= MB_MOD_CAPSLOCK;
    return mods;
}

static uint32_t query_modifiers(void) {
    if (!g_dpy || g_root == 0) return 0;
    Window rw, cw;
    int rx, ry, wx, wy;
    unsigned int state = 0;
    XQueryPointer(g_dpy, g_root, &rw, &cw, &rx, &ry, &wx, &wy, &state);
    return xmods_to_mb(state);
}

// Convert a root-relative pixel coordinate to content-local points per
// the host's current surface info. The inverse of what moonbase_host
// uses for outgoing pointer events. Rounds to the nearest integer —
// drag sources fire at ~60Hz and fractional points are meaningless at
// that granularity.
static void root_to_content_points(const mb_host_proxy_info_t *info,
                                   int root_x, int root_y,
                                   int *out_x, int *out_y) {
    float sx = info->scale > 0.0f ? info->scale : 1.0f;
    float xf = (float)(root_x - info->content_x) / sx;
    float yf = (float)(root_y - info->content_y) / sx;
    // roundf isn't available without -lm in some builds; add 0.5 for
    // positive half and -0.5 for negative so truncation rounds to
    // nearest.
    *out_x = (int)(xf >= 0 ? xf + 0.5f : xf - 0.5f);
    *out_y = (int)(yf >= 0 ? yf + 0.5f : yf - 0.5f);
}

// True iff (root_x, root_y) is inside the content rect of `info`.
// Returns false for an info whose content rect isn't committed yet
// (width or height == 0) — no content means nothing to drop onto.
static bool point_in_content(const mb_host_proxy_info_t *info,
                             int root_x, int root_y) {
    if (info->content_w == 0 || info->content_h == 0) return false;
    return root_x >= info->content_x
        && root_y >= info->content_y
        && root_x <  info->content_x + (int)info->content_w
        && root_y <  info->content_y + (int)info->content_h;
}

// Send an XdndStatus reply to the source. action_atom is the accepted
// action (normally XdndActionCopy). accept=true flips the accept bit;
// false tells the source we don't want to accept here. Rect fields are
// zeroed so the source keeps sending Position updates (no skip-region
// optimization).
static void send_xdnd_status(Window target, Window source, bool accept,
                             Atom action_atom) {
    if (!g_dpy) return;
    XClientMessageEvent rep = {0};
    rep.type         = ClientMessage;
    rep.display      = g_dpy;
    rep.window       = source;
    rep.message_type = a_XdndStatus;
    rep.format       = 32;
    rep.data.l[0] = (long)target;                 // our (proxy) window
    rep.data.l[1] = accept ? 1 : 0;               // accept bit; others reserved
    rep.data.l[2] = 0;                            // rect x/y = 0
    rep.data.l[3] = 0;                            // rect w/h = 0 — always re-query
    rep.data.l[4] = accept ? (long)action_atom : 0;
    XSendEvent(g_dpy, source, False, NoEventMask, (XEvent *)&rep);
}

// Send XdndFinished after a drop. accepted=false means we refused the
// drop (e.g. zero URIs). action echoes what XdndStatus approved; None
// on refusal.
static void send_xdnd_finished(Window target, Window source, bool accepted,
                               Atom action_atom) {
    if (!g_dpy) return;
    XClientMessageEvent rep = {0};
    rep.type         = ClientMessage;
    rep.display      = g_dpy;
    rep.window       = source;
    rep.message_type = a_XdndFinished;
    rep.format       = 32;
    rep.data.l[0] = (long)target;
    rep.data.l[1] = accepted ? 1 : 0;
    rep.data.l[2] = accepted ? (long)action_atom : 0;
    XSendEvent(g_dpy, source, False, NoEventMask, (XEvent *)&rep);
}

// Ask the source to deliver the drag selection as text/uri-list onto
// our proxy window's XdndSelection property. The SelectionNotify will
// arrive later and mb_xdnd_handle_selection_notify will pick it up.
// XDND v5 says to use the Position timestamp as the selection timestamp,
// but CurrentTime works with every source we've tested and keeps the
// session state smaller. Sources that require the real timestamp can
// be added later.
static void request_uri_list(Window proxy) {
    if (!g_dpy) return;
    XConvertSelection(g_dpy, a_XdndSelection, a_text_uri_list,
                      a_XdndSelection, proxy, CurrentTime);
}

// Parse a `text/uri-list` property payload (RFC 2483). Each line that
// isn't a comment (`#` prefix) and isn't empty is a URI. Lines are
// separated by CRLF per the RFC but we accept bare LF too since many
// Linux sources ship that way. `bytes` / `len` is the raw property
// data; trailing NUL bytes (common) are tolerated. Returns true if at
// least one URI landed.
static bool parse_uri_list(const unsigned char *bytes, size_t len,
                           char ***out_storage, size_t *out_count) {
    *out_storage = NULL;
    *out_count   = 0;

    size_t cap = 0, count = 0;
    char **arr = NULL;

    size_t i = 0;
    while (i < len) {
        // Find end of line (LF or CRLF). Stop at NUL — some sources
        // null-terminate the payload after the final CRLF.
        size_t j = i;
        while (j < len && bytes[j] != '\n' && bytes[j] != '\0') j++;
        size_t line_end = j;
        // Strip trailing CR.
        if (line_end > i && bytes[line_end - 1] == '\r') line_end--;

        // Skip empty lines and comment lines (RFC 2483 §5).
        if (line_end > i && bytes[i] != '#') {
            // Trim leading whitespace conservatively — RFC says URIs
            // don't contain whitespace at the start anyway.
            size_t start = i;
            while (start < line_end && isspace(bytes[start])) start++;
            if (start < line_end) {
                size_t slen = line_end - start;
                char *copy = malloc(slen + 1);
                if (!copy) {
                    // Partial success is fine — return what we have.
                    break;
                }
                memcpy(copy, bytes + start, slen);
                copy[slen] = '\0';
                if (count >= cap) {
                    size_t newcap = cap ? cap * 2 : 4;
                    char **grown = realloc(arr, newcap * sizeof(char *));
                    if (!grown) { free(copy); break; }
                    arr = grown;
                    cap = newcap;
                }
                arr[count++] = copy;
            }
        }

        // Advance past NUL or LF.
        if (j < len && (bytes[j] == '\n' || bytes[j] == '\0')) {
            i = j + 1;
        } else {
            i = j;
        }
    }

    if (count == 0) {
        free(arr);
        return false;
    }
    *out_storage = arr;
    *out_count   = count;
    return true;
}

// ─────────────────────────────────────────────────────────────────────
// Public entry points
// ─────────────────────────────────────────────────────────────────────

void mb_xdnd_init(Display *dpy, Window root) {
    if (g_ready) return;
    if (!dpy) return;
    g_dpy  = dpy;
    g_root = root;

    a_XdndAware      = XInternAtom(dpy, "XdndAware",      False);
    a_XdndEnter      = XInternAtom(dpy, "XdndEnter",      False);
    a_XdndPosition   = XInternAtom(dpy, "XdndPosition",   False);
    a_XdndLeave      = XInternAtom(dpy, "XdndLeave",      False);
    a_XdndDrop       = XInternAtom(dpy, "XdndDrop",       False);
    a_XdndStatus     = XInternAtom(dpy, "XdndStatus",     False);
    a_XdndFinished   = XInternAtom(dpy, "XdndFinished",   False);
    a_XdndSelection  = XInternAtom(dpy, "XdndSelection",  False);
    a_XdndTypeList   = XInternAtom(dpy, "XdndTypeList",   False);
    a_XdndActionCopy = XInternAtom(dpy, "XdndActionCopy", False);
    a_text_uri_list  = XInternAtom(dpy, "text/uri-list",  False);

    memset(&g_session, 0, sizeof(g_session));
    g_ready = true;
}

void mb_xdnd_forget_window(Window win) {
    if (!g_session.active || g_session.proxy != win) return;
    // Source still thinks there's an active drag, but its target has
    // vanished. Don't emit anything into the IPC — the client is
    // already gone. Just tidy up our state.
    session_reset();
}

// Handle XdndEnter: cache the source window, XDND version, and whether
// text/uri-list appears in the advertised type list. Do NOT emit
// DRAG_ENTER yet — we need a Position to learn the coords and decide
// whether to accept. If text/uri-list is advertised, kick off a
// selection request early so the SelectionNotify typically lands
// before Drop.
static void on_enter(const XClientMessageEvent *cm,
                     const mb_host_proxy_info_t *info) {
    // New session always supplants any previous one. Real sources
    // always send Leave before starting a new drag, but a crashed
    // source can skip it. If the supplanted session had already
    // emitted ENTER to a client, fire LEAVE so the client's drag
    // state can't get stuck "in drag" forever. IPC.md §5.3 says every
    // session ends with DROP or LEAVE — honor that even under flake.
    if (g_session.active && g_session.entered) {
        fprintf(stderr, "[xdnd] stale session on win=0x%lx — emitting LEAVE\n",
                (unsigned long)g_session.proxy);
        mb_host_emit_drag_frame(g_session.client_id, MB_IPC_DRAG_LEAVE,
                                g_session.window_id,
                                0, 0, 0, NULL, 0, 0);
    }
    session_reset();
    g_session.active   = true;
    g_session.proxy    = cm->window;
    g_session.source   = (Window)cm->data.l[0];
    g_session.window_id= info->window_id;
    g_session.client_id= info->client;
    g_session.version  = (int)((unsigned long)cm->data.l[1] >> 24);
    if (g_session.version < 1) g_session.version = 5;
    if (g_session.version > 5) g_session.version = 5;

    // data.l[1] low bit: if set, the source has >3 types and they
    // live on the source window's XdndTypeList property. Otherwise
    // the inline atoms in data.l[2..4] are authoritative.
    bool uri_list_advertised = false;
    if (cm->data.l[1] & 1L) {
        Atom actual = 0;
        int fmt = 0;
        unsigned long nitems = 0, bytes_after = 0;
        unsigned char *prop = NULL;
        if (XGetWindowProperty(g_dpy, g_session.source, a_XdndTypeList,
                               0, 256, False, XA_ATOM,
                               &actual, &fmt, &nitems, &bytes_after,
                               &prop) == Success && prop) {
            Atom *atoms = (Atom *)prop;
            for (unsigned long i = 0; i < nitems; i++) {
                if (atoms[i] == a_text_uri_list) {
                    uri_list_advertised = true;
                    break;
                }
            }
            XFree(prop);
        }
    } else {
        for (int i = 2; i <= 4; i++) {
            if ((Atom)cm->data.l[i] == a_text_uri_list) {
                uri_list_advertised = true;
                break;
            }
        }
    }

    g_session.advertises_uri_list = uri_list_advertised;
    g_session.modifiers = query_modifiers();

    fprintf(stderr, "[xdnd] Enter src=0x%lx proxy=0x%lx v=%d uri_list=%d\n",
            (unsigned long)g_session.source,
            (unsigned long)g_session.proxy,
            g_session.version,
            uri_list_advertised ? 1 : 0);

    // Ask for the selection early so SelectionNotify typically lands
    // before Drop. Sources that withhold the data until Drop (older
    // Qt, some ports) still trigger a second request at Drop time.
    if (uri_list_advertised) {
        request_uri_list(g_session.proxy);
        g_session.uris_requested = true;
    }
}

static void on_position(const XClientMessageEvent *cm,
                        const mb_host_proxy_info_t *info) {
    // Ignore Position from a different source than the one we enrolled
    // on Enter. XDND is single-source but the guard is cheap.
    if (!g_session.active || g_session.source != (Window)cm->data.l[0]) {
        // No prior Enter on this proxy — tell the source we decline so
        // it stops spamming Position at us. (Without a prior Enter we
        // have no session state to emit from anyway.)
        send_xdnd_status(cm->window, (Window)cm->data.l[0], false, None);
        return;
    }

    // data.l[2] packs root coords: high 16 bits X, low 16 bits Y.
    // Mask both halves — l[2] is long (64-bit), and a sign-extended
    // negative Y (monitors arranged left/above primary produce those)
    // would otherwise bleed into the X shift.
    unsigned long packed = (unsigned long)cm->data.l[2];
    int root_x = (int)((packed >> 16) & 0xFFFF);
    int root_y = (int)( packed        & 0xFFFF);

    bool inside = point_in_content(info, root_x, root_y);
    Atom action = (Atom)cm->data.l[4];
    if (action == 0) action = a_XdndActionCopy;

    // Always ACK Position. Not acking freezes the drag on strict
    // sources (GTK). Accept bit follows hit-test.
    send_xdnd_status(cm->window, g_session.source, inside, action);

    if (!inside) {
        // Pointer wandered off our content rect. If we'd previously
        // announced ENTER, retract it with a LEAVE so the client
        // doesn't keep drawing a drop highlight.
        if (g_session.entered) {
            fprintf(stderr, "[xdnd] Position OUT → LEAVE win=0x%lx\n",
                    (unsigned long)g_session.proxy);
            mb_host_emit_drag_frame(g_session.client_id,
                                    MB_IPC_DRAG_LEAVE,
                                    g_session.window_id,
                                    0, 0, 0, NULL, 0, 0);
            g_session.entered = false;
            g_session.last_coords_valid = false;
        }
        return;
    }

    int xp, yp;
    root_to_content_points(info, root_x, root_y, &xp, &yp);

    if (!g_session.entered) {
        // First time the pointer has entered the content rect. Emit
        // DRAG_ENTER with the URI list if it's already been fetched;
        // otherwise an empty URI array — clients that care can wait
        // for DROP to get URIs.
        const char *const *uris = NULL;
        size_t count = 0;
        if (g_session.uris_ready) {
            uris  = (const char *const *)g_session.uris;
            count = g_session.uri_count;
        }
        fprintf(stderr, "[xdnd] ENTER win=0x%lx pt=(%d,%d) uris=%zu mods=0x%x\n",
                (unsigned long)g_session.proxy, xp, yp, count,
                (unsigned)g_session.modifiers);
        mb_host_emit_drag_frame(g_session.client_id, MB_IPC_DRAG_ENTER,
                                g_session.window_id,
                                xp, yp,
                                g_session.modifiers,
                                uris, count, 0);
        g_session.entered = true;
        g_session.last_x_points = xp;
        g_session.last_y_points = yp;
        g_session.last_coords_valid = true;
        return;
    }

    // Throttle OVER to actual coordinate changes. GTK sources fire
    // Position at ~60Hz even when stationary and we'd just burn IPC
    // frames for no behavior change on the client side.
    if (g_session.last_coords_valid
            && xp == g_session.last_x_points
            && yp == g_session.last_y_points) {
        return;
    }
    mb_host_emit_drag_frame(g_session.client_id, MB_IPC_DRAG_OVER,
                            g_session.window_id,
                            xp, yp,
                            g_session.modifiers,
                            NULL, 0, 0);
    g_session.last_x_points = xp;
    g_session.last_y_points = yp;
    g_session.last_coords_valid = true;
}

static void on_leave(const XClientMessageEvent *cm) {
    if (!g_session.active || g_session.source != (Window)cm->data.l[0]) {
        return;
    }
    if (g_session.entered) {
        fprintf(stderr, "[xdnd] Leave → LEAVE win=0x%lx\n",
                (unsigned long)g_session.proxy);
        mb_host_emit_drag_frame(g_session.client_id, MB_IPC_DRAG_LEAVE,
                                g_session.window_id,
                                0, 0, 0, NULL, 0, 0);
    }
    session_reset();
}

// Close out a drop whose URIs are finally available. Called from
// on_drop when the selection was already in hand, or from the
// SelectionNotify handler when the selection arrived after the Drop.
// If there are zero URIs, emit LEAVE (the IPC schema's DROP carries
// URIs) and tell the source we refused. Otherwise emit DROP with the
// URI list and signal XdndFinished(accepted=1).
static void finish_drop(void) {
    if (!g_session.drop_pending) return;

    if (g_session.uri_count == 0) {
        fprintf(stderr, "[xdnd] DROP refused (0 URIs) win=0x%lx\n",
                (unsigned long)g_session.proxy);
        if (g_session.entered) {
            mb_host_emit_drag_frame(g_session.client_id, MB_IPC_DRAG_LEAVE,
                                    g_session.window_id,
                                    0, 0, 0, NULL, 0, 0);
        }
        send_xdnd_finished(g_session.proxy, g_session.source, false, None);
    } else {
        int xp = g_session.last_coords_valid ? g_session.last_x_points : 0;
        int yp = g_session.last_coords_valid ? g_session.last_y_points : 0;
        fprintf(stderr, "[xdnd] DROP win=0x%lx pt=(%d,%d) uris=%zu",
                (unsigned long)g_session.proxy, xp, yp, g_session.uri_count);
        if (g_session.uri_count > 0) {
            fprintf(stderr, " [0]=%s", g_session.uris[0]);
        }
        fprintf(stderr, "\n");
        mb_host_emit_drag_frame(g_session.client_id, MB_IPC_DRAG_DROP,
                                g_session.window_id,
                                xp, yp,
                                g_session.modifiers,
                                (const char *const *)g_session.uris,
                                g_session.uri_count, 0);
        send_xdnd_finished(g_session.proxy, g_session.source, true,
                           a_XdndActionCopy);
    }
    session_reset();
}

static void on_drop(const XClientMessageEvent *cm) {
    if (!g_session.active || g_session.source != (Window)cm->data.l[0]) {
        // Drop on a target we didn't enroll. Tell the source we
        // refused so it can finish cleanly.
        send_xdnd_finished(cm->window, (Window)cm->data.l[0], false, None);
        return;
    }

    // Refresh modifiers — user could have tapped Option between Enter
    // and Drop to switch copy/link intent.
    g_session.modifiers = query_modifiers();
    g_session.drop_pending = true;
    fprintf(stderr, "[xdnd] Drop win=0x%lx uris_ready=%d\n",
            (unsigned long)g_session.proxy, g_session.uris_ready ? 1 : 0);

    // If URIs aren't in hand yet, hold the drop until SelectionNotify
    // arrives. Re-request if we never asked (sources that don't
    // advertise but still honor the conversion).
    if (!g_session.uris_ready) {
        if (!g_session.uris_requested) {
            request_uri_list(g_session.proxy);
            g_session.uris_requested = true;
        }
        return;
    }
    finish_drop();
}

bool mb_xdnd_handle_client_message(const XClientMessageEvent *cm) {
    if (!g_ready || !cm) return false;

    Atom mt = cm->message_type;
    bool is_xdnd = (mt == a_XdndEnter || mt == a_XdndPosition
                    || mt == a_XdndLeave || mt == a_XdndDrop);
    if (!is_xdnd) return false;

    // Every XDND message must target our proxy window. If the message
    // lands on something else we leave it alone — some other path
    // handles it or it's a spurious event.
    mb_host_proxy_info_t info;
    if (!mb_host_find_proxy_surface(cm->window, &info)) {
        return false;
    }

    if (mt == a_XdndEnter) {
        on_enter(cm, &info);
    } else if (mt == a_XdndPosition) {
        on_position(cm, &info);
    } else if (mt == a_XdndLeave) {
        on_leave(cm);
    } else if (mt == a_XdndDrop) {
        on_drop(cm);
    }
    return true;
}

bool mb_xdnd_handle_selection_notify(const XSelectionEvent *se) {
    if (!g_ready || !se) return false;
    if (se->selection != a_XdndSelection) return false;

    // requestor is the proxy we passed to XConvertSelection.
    mb_host_proxy_info_t info;
    if (!mb_host_find_proxy_surface(se->requestor, &info)) return false;
    if (!g_session.active || g_session.proxy != se->requestor) return false;

    if (se->property == None) {
        // Source refused the conversion. Mark URIs as ready-but-empty
        // so a pending drop doesn't stall forever.
        g_session.uris_ready = true;
    } else {
        Atom actual = 0;
        int fmt = 0;
        unsigned long nitems = 0, bytes_after = 0;
        unsigned char *prop = NULL;
        if (XGetWindowProperty(g_dpy, se->requestor, se->property,
                               0, 1 << 20, True, AnyPropertyType,
                               &actual, &fmt, &nitems, &bytes_after,
                               &prop) == Success && prop) {
            char **arr = NULL;
            size_t count = 0;
            if (parse_uri_list(prop, nitems, &arr, &count)) {
                uris_free(&g_session);
                g_session.uris      = arr;
                g_session.uri_count = count;
            }
            g_session.uris_ready = true;
            XFree(prop);
        } else {
            g_session.uris_ready = true;
        }
    }

    fprintf(stderr, "[xdnd] SelectionNotify win=0x%lx uris=%zu drop_pending=%d\n",
            (unsigned long)se->requestor, g_session.uri_count,
            g_session.drop_pending ? 1 : 0);

    // Two paths converge here. Pre-drop delivery: URIs arrived during
    // an Enter/Position phase — stash and keep going. Post-drop
    // delivery: a Drop was held waiting on this SelectionNotify — run
    // finish_drop() now, which emits the DROP frame and replies
    // XdndFinished to the source.
    if (g_session.drop_pending) {
        finish_drop();
    }
    return true;
}
