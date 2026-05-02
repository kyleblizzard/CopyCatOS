// CopyCatOS — by Kyle Blizzard at Blizzard.show

// dnd.c — XDND (X Drag and Drop Protocol v5) implementation
//
// Full implementation of the XDND protocol for desktop.
// We participate as both SOURCE (dragging icons TO other apps) and
// TARGET (receiving drops FROM other apps onto the desktop).
//
// Source role flow:
//   1. dnd_source_begin()  — grab pointer, store file URI
//   2. dnd_source_motion() — send XdndEnter/Position/Leave as cursor moves
//   3. dnd_source_drop()   — send XdndDrop on button release
//   4. XdndStatus          — target accepts/rejects (from dnd_handle_client_message)
//   5. XdndFinished        — transfer done (or dnd_tick() timeout after 3s)
//
// Target role flow:
//   1. XdndEnter    — source announces drag entering our window
//   2. XdndPosition — we reply with XdndStatus (accept, copy)
//   3. XdndLeave    — drag moved away, clear state
//   4. XdndDrop     — user released; we call XConvertSelection
//   5. SelectionNotify — data arrives; we copy file to ~/Desktop

#define _GNU_SOURCE  // For sendfile(), realpath()

#include "dnd.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <libgen.h>   // basename()
#include <errno.h>

// ── Module-level state ───────────────────────────────────────────────

// Interned atom table — populated once by dnd_init()
static DndAtoms g_atoms;

// Our own desktop window — stored at dnd_init() so that target-role
// functions (which don't receive it as a parameter) can reference it
// as the XConvertSelection requestor and in XdndStatus replies.
static Window g_desktop_win = None;

// ── Source state ─────────────────────────────────────────────────────
// Tracks an in-progress outgoing drag (we are the source, dragging TO
// an external app).

static struct {
    bool     active;           // True while a drag is in progress
    Window   src_win;          // Our window (desktop)
    Window   root_win;         // Root window (for XQueryPointer)
    Window   target;           // Current XDND-aware window under cursor
    bool     entered;          // True if we've sent XdndEnter to target
    bool     target_accepted;  // True if target sent XdndStatus accept
    char     path[4096];       // Absolute file path being dragged
    char     uri[4096];        // "file:///path/to/file" percent-encoded
    Time     drop_time;        // Time XdndDrop was sent (for timeout)
    bool     drop_sent;        // True after we sent XdndDrop
} src;

// ── Target state ─────────────────────────────────────────────────────
// Tracks an incoming drag from another app (we are the target, receiving
// files dropped ONTO the desktop).

static struct {
    bool     active;           // True while a foreign drag is over us
    Window   source;           // The source window sending us the drag
    Atom     offered_type;     // First accepted MIME type from XdndEnter
    Time     drop_time;        // Timestamp from XdndDrop (needed for reply)
    bool     drop_received;    // True after XdndDrop arrived (awaiting data)
    // The destination path after a successful copy — returned by
    // dnd_handle_selection_notify() so the caller can trigger a repaint.
    char     dest_path[4096];
} tgt;

// ── Atom access ──────────────────────────────────────────────────────

const DndAtoms *dnd_atoms(void)
{
    return &g_atoms;
}

// ── URI percent-encoding ─────────────────────────────────────────────
//
// XDND data for files is sent as "text/uri-list", which requires
// percent-encoding of non-ASCII and reserved characters in the path.
// e.g., "/home/user/My File.pdf" → "file:///home/user/My%20File.pdf"
//
// We encode everything except:
//   unreserved: A-Z a-z 0-9 - _ . ~
//   path chars: /
// Everything else gets % + uppercase hex pair.

static void uri_encode_path(const char *path, char *out, size_t out_size)
{
    // Start with the file:// scheme prefix
    size_t prefix_len = 7;  // "file://"
    if (out_size <= prefix_len) {
        out[0] = '\0';
        return;
    }
    memcpy(out, "file://", prefix_len);

    size_t in_len = strlen(path);
    size_t out_pos = prefix_len;

    for (size_t i = 0; i < in_len && out_pos + 4 < out_size; i++) {
        unsigned char c = (unsigned char)path[i];

        // These characters are safe and don't need encoding.
        // Slash (/) is the path separator so it passes through unchanged.
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out[out_pos++] = (char)c;
        } else {
            // Encode as %XX (uppercase hex)
            out[out_pos++] = '%';
            out[out_pos++] = "0123456789ABCDEF"[c >> 4];
            out[out_pos++] = "0123456789ABCDEF"[c & 0xF];
        }
    }

    out[out_pos] = '\0';
}

// Reverse of the above: decode a percent-encoded URI path back to
// a plain filesystem path. Strips the "file://" prefix first.
// Returns number of bytes written (not counting the NUL), or -1 on error.
static int uri_decode_path(const char *uri, char *out, size_t out_size)
{
    // Must start with file://
    if (strncmp(uri, "file://", 7) != 0) {
        return -1;
    }

    const char *in = uri + 7;   // Skip "file://"
    size_t out_pos = 0;

    while (*in && out_pos + 1 < out_size) {
        if (*in == '%' && in[1] && in[2]) {
            // Decode a percent-encoded byte
            char hex[3] = { in[1], in[2], '\0' };
            out[out_pos++] = (char)strtol(hex, NULL, 16);
            in += 3;
        } else if (*in == '\r' || *in == '\n') {
            // Stop at line ending (text/uri-list uses \r\n separators)
            break;
        } else {
            out[out_pos++] = *in++;
        }
    }

    out[out_pos] = '\0';
    return (int)out_pos;
}

// ── Init / Shutdown ──────────────────────────────────────────────────

void dnd_init(Display *dpy, Window desktop_win, Window root_win)
{
    // Intern all the atoms we need. XInternAtom(dpy, name, only_if_exists=False)
    // creates the atom if it doesn't exist yet.
    g_atoms.XdndAware      = XInternAtom(dpy, "XdndAware",      False);
    g_atoms.XdndProxy      = XInternAtom(dpy, "XdndProxy",      False);
    g_atoms.XdndEnter      = XInternAtom(dpy, "XdndEnter",      False);
    g_atoms.XdndPosition   = XInternAtom(dpy, "XdndPosition",   False);
    g_atoms.XdndStatus     = XInternAtom(dpy, "XdndStatus",     False);
    g_atoms.XdndLeave      = XInternAtom(dpy, "XdndLeave",      False);
    g_atoms.XdndDrop       = XInternAtom(dpy, "XdndDrop",       False);
    g_atoms.XdndFinished   = XInternAtom(dpy, "XdndFinished",   False);
    g_atoms.XdndActionCopy = XInternAtom(dpy, "XdndActionCopy", False);
    g_atoms.XdndActionMove = XInternAtom(dpy, "XdndActionMove", False);
    g_atoms.XdndTypeList   = XInternAtom(dpy, "XdndTypeList",   False);
    g_atoms.XdndSelection  = XInternAtom(dpy, "XdndSelection",  False);
    g_atoms.text_uri_list  = XInternAtom(dpy, "text/uri-list",  False);
    g_atoms.text_plain     = XInternAtom(dpy, "text/plain",     False);
    g_atoms.UTF8_STRING    = XInternAtom(dpy, "UTF8_STRING",    False);

    // Advertise XDND support on our window by setting the XdndAware property.
    // The value is the protocol version (5) as a 32-bit integer.
    // Other apps check this property before sending us XDND messages.
    long xdnd_version = XDND_VERSION;
    XChangeProperty(dpy, desktop_win,
                    g_atoms.XdndAware, XA_ATOM, 32,
                    PropModeReplace,
                    (unsigned char *)&xdnd_version, 1);

    // Set XdndProxy on the ROOT window pointing to our desktop window.
    // When another app tries to drop onto the root window (which handles
    // many desktop environments), it checks XdndProxy first and redirects
    // the drop to us instead. This is how we intercept root-level drops.
    XChangeProperty(dpy, root_win,
                    g_atoms.XdndProxy, XA_WINDOW, 32,
                    PropModeReplace,
                    (unsigned char *)&desktop_win, 1);

    // Also set XdndProxy on our OWN window, pointing to itself.
    // This is a "proof of life" check — apps verify that the proxy target
    // has this property set to itself before using it. If we crashed and
    // left a stale proxy pointing to our dead window, this check fails
    // and they fall back to direct root interaction.
    XChangeProperty(dpy, desktop_win,
                    g_atoms.XdndProxy, XA_WINDOW, 32,
                    PropModeReplace,
                    (unsigned char *)&desktop_win, 1);

    // Store the desktop window for use by target-role functions
    g_desktop_win = desktop_win;

    // Clear state
    memset(&src, 0, sizeof(src));
    memset(&tgt, 0, sizeof(tgt));

    fprintf(stderr, "[dnd] XDND v5 initialized, desktop_win=0x%lx\n",
            desktop_win);
}

void dnd_shutdown(Display *dpy, Window root_win)
{
    // Remove our XdndProxy from the root window so apps that check it
    // after we exit don't try to send XDND messages to our dead window.
    XDeleteProperty(dpy, root_win, g_atoms.XdndProxy);
    XFlush(dpy);
    fprintf(stderr, "[dnd] XdndProxy removed from root\n");
}

// ── Finding the XDND target window ───────────────────────────────────
//
// During a drag, we need to know which window the cursor is over so we
// can send XdndPosition to it. XQueryPointer walks the window tree to
// find the leaf window under the cursor, then we check if it (or any
// ancestor) advertises XdndAware.

// Check if window 'w' has XdndAware set, and return its version.
// Returns 0 if XdndAware is not set on this window.
static int get_xdnd_version(Display *dpy, Window w)
{
    Atom actual_type;
    int actual_format;
    unsigned long n_items, bytes_after;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(dpy, w,
        g_atoms.XdndAware, 0, 1, False,
        XA_ATOM, &actual_type, &actual_format,
        &n_items, &bytes_after, &data);

    if (status != Success || actual_type == None || !data) {
        if (data) XFree(data);
        return 0;
    }

    // The property value is the XDND version supported by this window
    int version = (int)(*(long *)data);
    XFree(data);
    return version;
}

// Walk upward from the leaf window under the cursor to find the first
// window with XdndAware set. Returns None if no XDND window found.
// Also fills out *version_out with the supported XDND version.
static Window find_xdnd_target(Display *dpy, int root_x, int root_y,
                                int *version_out)
{
    // XQueryPointer returns the leaf-level window directly under the cursor.
    // We start there and walk up to the root looking for XdndAware.
    Window root_ret, child_ret;
    int win_x, win_y;
    unsigned int mask;

    // Start from root, ask for the child under (root_x, root_y)
    if (!XQueryPointer(dpy, src.root_win,
                       &root_ret, &child_ret,
                       &root_x, &root_y,
                       &win_x, &win_y, &mask)) {
        return None;
    }

    if (child_ret == None) {
        return None;  // Cursor is over root window itself (no child)
    }

    // Walk down to the deepest child under the cursor.
    // We keep calling XQueryPointer on the child until there are no
    // more children — that's the leaf window.
    Window w = child_ret;
    while (true) {
        Window child;
        if (!XQueryPointer(dpy, w, &root_ret, &child,
                           &root_x, &root_y, &win_x, &win_y, &mask)) {
            break;
        }
        if (child == None) break;
        w = child;
    }

    // Don't send XDND messages to ourselves
    if (w == src.src_win) {
        return None;
    }

    // Walk up from the leaf until we find an XdndAware window.
    // Some apps put XdndAware on a parent window, not the visible leaf.
    // We stop at the root.
    while (w != None && w != src.root_win) {
        int ver = get_xdnd_version(dpy, w);
        if (ver > 0) {
            *version_out = ver;
            return w;
        }

        // Also check for XdndProxy — some apps redirect drops via a proxy
        // (this is symmetric to what we do ourselves with the root window)
        Atom actual_type;
        int actual_format;
        unsigned long n_items, bytes_after;
        unsigned char *data = NULL;

        if (XGetWindowProperty(dpy, w, g_atoms.XdndProxy, 0, 1, False,
                XA_WINDOW, &actual_type, &actual_format,
                &n_items, &bytes_after, &data) == Success
            && actual_type != None && data) {
            Window proxy = *(Window *)data;
            XFree(data);
            // Verify the proxy is alive (it sets XdndProxy→itself)
            unsigned char *pdata = NULL;
            if (XGetWindowProperty(dpy, proxy, g_atoms.XdndProxy, 0, 1, False,
                    XA_WINDOW, &actual_type, &actual_format,
                    &n_items, &bytes_after, &pdata) == Success
                && actual_type != None && pdata) {
                Window proxy2 = *(Window *)pdata;
                XFree(pdata);
                if (proxy2 == proxy) {
                    int ver = get_xdnd_version(dpy, proxy);
                    if (ver > 0) {
                        *version_out = ver;
                        return proxy;
                    }
                }
            }
        }

        // Move up to parent
        Window parent, root_tmp;
        Window *children;
        unsigned int nchildren;
        if (!XQueryTree(dpy, w, &root_tmp, &parent, &children, &nchildren)) {
            break;
        }
        if (children) XFree(children);
        w = parent;
    }

    return None;  // No XDND-aware window found
}

// ── Source: sending XdndEnter ─────────────────────────────────────────
//
// XdndEnter tells the target window "I'm starting to drag over you".
// We send our MIME types so the target can decide if it can accept the data.
// For file drags we offer text/uri-list as the primary type.

static void send_xdnd_enter(Display *dpy, Window target, int target_version)
{
    XClientMessageEvent msg;
    memset(&msg, 0, sizeof(msg));

    msg.type         = ClientMessage;
    msg.display      = dpy;
    msg.window       = target;
    msg.message_type = g_atoms.XdndEnter;
    msg.format       = 32;

    // data[0]: source window ID (us)
    msg.data.l[0] = (long)src.src_win;

    // data[1]: version in top byte, bit 0 = more-than-3-types flag.
    // We only offer 2 types (uri-list + plain), so bit 0 = 0.
    // Use the minimum of our version and the target's version.
    int ver = target_version < XDND_VERSION ? target_version : XDND_VERSION;
    msg.data.l[1] = (long)(ver << 24);

    // data[2..4]: up to 3 MIME type atoms (when bit 0 in data[1] is 0).
    // We offer text/uri-list first (preferred for file DnD), then text/plain.
    msg.data.l[2] = (long)g_atoms.text_uri_list;
    msg.data.l[3] = (long)g_atoms.text_plain;
    msg.data.l[4] = 0;

    // Send the message to the target window.
    // We don't propagate (False) since it's addressed to a specific window.
    XSendEvent(dpy, target, False, 0, (XEvent *)&msg);
}

// ── Source: sending XdndPosition ─────────────────────────────────────
//
// XdndPosition updates the target with the current cursor position.
// We send this on every MotionNotify while dragging over an XDND window.
// The target will reply with XdndStatus.

static void send_xdnd_position(Display *dpy, Window target,
                                int root_x, int root_y)
{
    XClientMessageEvent msg;
    memset(&msg, 0, sizeof(msg));

    msg.type         = ClientMessage;
    msg.display      = dpy;
    msg.window       = target;
    msg.message_type = g_atoms.XdndPosition;
    msg.format       = 32;

    // data[0]: source window
    msg.data.l[0] = (long)src.src_win;

    // data[1]: reserved, must be 0
    msg.data.l[1] = 0;

    // data[2]: cursor position as (x << 16 | y), both in root coordinates.
    // Root coordinates are screen-absolute so the target can position
    // feedback relative to the screen.
    msg.data.l[2] = (long)((root_x << 16) | (root_y & 0xFFFF));

    // data[3]: timestamp (CurrentTime is fine for positions)
    msg.data.l[3] = CurrentTime;

    // data[4]: proposed action — we want to copy the file (not move)
    msg.data.l[4] = (long)g_atoms.XdndActionCopy;

    XSendEvent(dpy, target, False, 0, (XEvent *)&msg);
}

// ── Source: sending XdndLeave ─────────────────────────────────────────
//
// XdndLeave tells the target "the cursor moved away, cancel any hover state".
// We send this when the cursor moves from one XDND window to another,
// or when the drag is cancelled.

static void send_xdnd_leave(Display *dpy, Window target)
{
    XClientMessageEvent msg;
    memset(&msg, 0, sizeof(msg));

    msg.type         = ClientMessage;
    msg.display      = dpy;
    msg.window       = target;
    msg.message_type = g_atoms.XdndLeave;
    msg.format       = 32;

    msg.data.l[0] = (long)src.src_win;
    // data[1..4]: reserved, all 0

    XSendEvent(dpy, target, False, 0, (XEvent *)&msg);
}

// ── Source: sending XdndDrop ──────────────────────────────────────────
//
// XdndDrop tells the target "the user released the button, please accept
// the data". After this, the target will call XConvertSelection to pull
// the file URI from us, and we'll get a SelectionRequest event.

static void send_xdnd_drop(Display *dpy, Window target)
{
    XClientMessageEvent msg;
    memset(&msg, 0, sizeof(msg));

    msg.type         = ClientMessage;
    msg.display      = dpy;
    msg.window       = target;
    msg.message_type = g_atoms.XdndDrop;
    msg.format       = 32;

    msg.data.l[0] = (long)src.src_win;
    msg.data.l[1] = 0;

    // data[2]: timestamp of the drop event.
    // We use CurrentTime here; ideally we'd use the ButtonRelease timestamp,
    // but CurrentTime is accepted by virtually all apps.
    msg.data.l[2] = CurrentTime;
    src.drop_time = CurrentTime;

    XSendEvent(dpy, target, False, 0, (XEvent *)&msg);
}

// ── Source role API ───────────────────────────────────────────────────

void dnd_source_begin(Display *dpy, Window src_win, Window root_win,
                      const char *file_path, int root_x, int root_y)
{
    // Prevent starting a new drag while one is in progress
    if (src.active) {
        fprintf(stderr, "[dnd] source_begin called while already active\n");
        return;
    }

    src.active   = true;
    src.src_win  = src_win;
    src.root_win = root_win;
    src.target   = None;
    src.entered  = false;
    src.target_accepted = false;
    src.drop_sent = false;
    src.drop_time = 0;

    // Store the file path and build the percent-encoded URI
    strncpy(src.path, file_path, sizeof(src.path) - 1);
    src.path[sizeof(src.path) - 1] = '\0';
    uri_encode_path(src.path, src.uri, sizeof(src.uri));

    // Take ownership of the XdndSelection X selection so we can respond
    // to SelectionRequest events when the target asks for the data.
    XSetSelectionOwner(dpy, g_atoms.XdndSelection, src_win, CurrentTime);

    // Grab the pointer (mouse) so all mouse events come to us regardless
    // of which window the cursor is over. This is what allows us to track
    // the drag position when the cursor is over a different application.
    // Snow Leopard parity: the cursor stays the standard arrow during a
    // file drag — no four-way "move" shape. Pass None to inherit whatever
    // cursor the source window already has on it.
    XGrabPointer(dpy, src_win, False,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                 GrabModeAsync,   // Don't freeze pointer events
                 GrabModeAsync,   // Don't freeze keyboard
                 None,            // Confine to no specific window
                 None,            // Inherit existing cursor (default arrow)
                 CurrentTime);

    // Send the initial position so the target gets an immediate Enter+Position
    // pair right when the drag starts (before any motion events arrive).
    int ver = 0;
    Window initial_target = find_xdnd_target(dpy, root_x, root_y, &ver);
    if (initial_target != None) {
        src.target  = initial_target;
        src.entered = true;
        send_xdnd_enter(dpy, initial_target, ver);
        send_xdnd_position(dpy, initial_target, root_x, root_y);
    }

    XFlush(dpy);
    fprintf(stderr, "[dnd] drag started: %s\n", src.path);
}

void dnd_source_motion(Display *dpy, int root_x, int root_y)
{
    if (!src.active) return;

    // Find the XDND-aware window currently under the cursor
    int ver = 0;
    Window new_target = find_xdnd_target(dpy, root_x, root_y, &ver);

    if (new_target != src.target) {
        // Cursor moved to a different window — leave the old one, enter new one

        if (src.target != None && src.entered) {
            // Tell the old target we're leaving
            send_xdnd_leave(dpy, src.target);
        }

        src.target  = new_target;
        src.entered = false;
        src.target_accepted = false;

        if (new_target != None) {
            // Tell the new target we're entering and send initial position
            send_xdnd_enter(dpy, new_target, ver);
            send_xdnd_position(dpy, new_target, root_x, root_y);
            src.entered = true;
        }
    } else if (src.target != None && src.entered) {
        // Still over the same window — just update position
        send_xdnd_position(dpy, src.target, root_x, root_y);
    }

    XFlush(dpy);
}

bool dnd_source_active(void)
{
    // True when the cursor is over an EXTERNAL XDND window.
    // The desktop sets XdndAware on its own window (so the root XdndProxy
    // can redirect drops to us); find_xdnd_target therefore returns
    // src.src_win whenever the cursor is over our own desktop. That is NOT
    // an "active external handshake" — it's just our own surface — so we
    // must exclude it. Without this check, the visual-icon drag-update
    // gate (`if (!dnd_source_active())` in desktop.c MotionNotify) skips
    // every frame after threshold cross, leaving the dragged icon frozen
    // at the threshold position while the cursor moves on. That looks
    // identical to "drag lag" but it's actually "drag stuck".
    return src.active && src.target != None && src.target != src.src_win;
}

bool dnd_source_drop(Display *dpy)
{
    if (!src.active) return false;

    if (src.target != None && src.entered) {
        // We have an XDND target — send XdndDrop and hand control to the
        // target. We stay "active" until we get XdndFinished (or timeout).
        send_xdnd_drop(dpy, src.target);
        src.drop_sent = true;

        // Record the time so dnd_tick() can timeout if no XdndFinished arrives
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        src.drop_time = (Time)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

        // Release the pointer grab — the drag is over from our movement side
        XUngrabPointer(dpy, CurrentTime);
        XFlush(dpy);

        fprintf(stderr, "[dnd] XdndDrop sent to 0x%lx\n", src.target);
        return true;  // XDND handled the drop
    }

    // No XDND target — release the grab and tell the caller to do grid-snap
    XUngrabPointer(dpy, CurrentTime);
    src.active  = false;
    src.target  = None;
    src.entered = false;
    XFlush(dpy);
    return false;  // Caller should handle the drop itself
}

void dnd_source_cancel(Display *dpy)
{
    if (!src.active) return;

    if (src.target != None && src.entered) {
        send_xdnd_leave(dpy, src.target);
    }

    XUngrabPointer(dpy, CurrentTime);
    XFlush(dpy);

    src.active  = false;
    src.target  = None;
    src.entered = false;

    fprintf(stderr, "[dnd] drag cancelled\n");
}

// ── Event dispatch ───────────────────────────────────────────────────

void dnd_handle_client_message(Display *dpy, XClientMessageEvent *ev)
{
    Atom type = ev->message_type;

    // ── Source side: responses from the target we dropped onto ───────

    if (type == g_atoms.XdndStatus) {
        // Target told us whether it accepts our drag.
        // data[1] bit 0 = accept/reject.
        bool accepted = (ev->data.l[1] & 1) != 0;
        src.target_accepted = accepted;

        if (accepted) {
            Atom action = (Atom)ev->data.l[4];
            fprintf(stderr, "[dnd] XdndStatus: accepted, action=0x%lx\n",
                    (unsigned long)action);
        } else {
            fprintf(stderr, "[dnd] XdndStatus: rejected\n");
        }
        return;
    }

    if (type == g_atoms.XdndFinished) {
        // Data transfer complete — the target received our file.
        bool success = (ev->data.l[1] & 1) != 0;
        fprintf(stderr, "[dnd] XdndFinished: %s\n",
                success ? "success" : "failed");

        // Clean up source state — the drag is fully complete now
        src.active        = false;
        src.target        = None;
        src.entered       = false;
        src.drop_sent     = false;
        src.target_accepted = false;
        return;
    }

    // ── Target side: incoming drag from another app ───────────────────

    if (type == g_atoms.XdndEnter) {
        // Another app is starting to drag over us.
        // data[0] = source window, data[1] bits = version + type count flag
        Window source  = (Window)ev->data.l[0];
        int    version = (int)(ev->data.l[1] >> 24);
        bool   has_type_list = (ev->data.l[1] & 1) != 0;

        fprintf(stderr, "[dnd] XdndEnter from 0x%lx (v%d)\n",
                (unsigned long)source, version);

        tgt.active       = true;
        tgt.source       = source;
        tgt.offered_type = None;
        tgt.drop_received = false;

        // Find a type we can handle — we only care about text/uri-list
        // which carries file paths. Check the inline types first (data[2..4]),
        // then the XdndTypeList property if the "has_type_list" flag is set.

        if (!has_type_list) {
            // Up to 3 types are inline in the message
            for (int i = 2; i <= 4; i++) {
                if ((Atom)ev->data.l[i] == g_atoms.text_uri_list) {
                    tgt.offered_type = g_atoms.text_uri_list;
                    break;
                }
            }
        } else {
            // More than 3 types — read the XdndTypeList property
            Atom actual_type;
            int actual_format;
            unsigned long n_items, bytes_after;
            unsigned char *data = NULL;

            if (XGetWindowProperty(dpy, source,
                    g_atoms.XdndTypeList, 0, 256, False,
                    XA_ATOM, &actual_type, &actual_format,
                    &n_items, &bytes_after, &data) == Success && data) {
                Atom *types = (Atom *)data;
                for (unsigned long i = 0; i < n_items; i++) {
                    if (types[i] == g_atoms.text_uri_list) {
                        tgt.offered_type = g_atoms.text_uri_list;
                        break;
                    }
                }
                XFree(data);
            }
        }

        if (tgt.offered_type == None) {
            fprintf(stderr, "[dnd] XdndEnter: no text/uri-list type, ignoring\n");
        }
        return;
    }

    if (type == g_atoms.XdndPosition) {
        // Source is telling us where the cursor is.
        // Reply with XdndStatus — accept if we have a URI type, reject otherwise.
        if (!tgt.active) return;

        bool accept = (tgt.offered_type != None);

        // Build XdndStatus reply.
        // We send it FROM our desktop window TO the source window.
        XClientMessageEvent reply;
        memset(&reply, 0, sizeof(reply));
        reply.type         = ClientMessage;
        reply.display      = dpy;
        reply.window       = tgt.source;
        reply.message_type = g_atoms.XdndStatus;
        reply.format       = 32;

        // data[0]: our window (target) — identifies us to the source
        reply.data.l[0] = (long)g_desktop_win;
        reply.data.l[1] = accept ? 1L : 0L;
        reply.data.l[2] = 0;
        reply.data.l[3] = 0;
        reply.data.l[4] = accept ? (long)g_atoms.XdndActionCopy : 0L;

        XSendEvent(dpy, tgt.source, False, 0, (XEvent *)&reply);
        XFlush(dpy);
        return;
    }

    if (type == g_atoms.XdndLeave) {
        // Drag moved away from our window — cancel target state
        fprintf(stderr, "[dnd] XdndLeave\n");
        tgt.active       = false;
        tgt.source       = None;
        tgt.offered_type = None;
        tgt.drop_received = false;
        return;
    }

    if (type == g_atoms.XdndDrop) {
        // User released the mouse button over us — request the file data.
        if (!tgt.active || tgt.offered_type == None) {
            // We can't accept — send XdndFinished with failure
            Window source = (Window)ev->data.l[0];
            XClientMessageEvent fin;
            memset(&fin, 0, sizeof(fin));
            fin.type         = ClientMessage;
            fin.display      = dpy;
            fin.window       = source;
            fin.message_type = g_atoms.XdndFinished;
            fin.format       = 32;
            fin.data.l[0]    = (long)g_desktop_win;  // target window = us
            fin.data.l[1]    = 0L;  // failure
            XSendEvent(dpy, source, False, 0, (XEvent *)&fin);
            XFlush(dpy);
            return;
        }

        // Record the drop timestamp from data[2] (for the XdndFinished reply)
        tgt.drop_time     = (Time)ev->data.l[2];
        tgt.drop_received = true;

        fprintf(stderr, "[dnd] XdndDrop received from 0x%lx\n",
                (unsigned long)tgt.source);

        // Ask the source to convert XdndSelection into text/uri-list format
        // and store it in the XdndSelection property on OUR window.
        // When done, the source sends a SelectionNotify event to our window.
        // We use g_desktop_win as the requestor so the event arrives in
        // our event loop (desktop.c routes SelectionNotify → dnd_handle_selection_notify).
        XConvertSelection(dpy,
                          g_atoms.XdndSelection,  // Selection name
                          tgt.offered_type,        // Requested format
                          g_atoms.XdndSelection,   // Property to write to
                          g_desktop_win,           // Requestor: our desktop window
                          tgt.drop_time);
        XFlush(dpy);
        return;
    }
}

// ── Selection: providing data (we're the source) ──────────────────────

void dnd_handle_selection_request(Display *dpy, XSelectionRequestEvent *ev)
{
    // Another app called XConvertSelection to get our drag data.
    // We need to write the file URI into the requested property on their window,
    // then send them a SelectionNotify to say "the data is ready".

    XSelectionEvent reply;
    memset(&reply, 0, sizeof(reply));
    reply.type       = SelectionNotify;
    reply.display    = dpy;
    reply.requestor  = ev->requestor;
    reply.selection  = ev->selection;
    reply.target     = ev->target;
    reply.property   = None;  // None = "I couldn't provide this format"
    reply.time       = ev->time;

    if (ev->selection != g_atoms.XdndSelection || !src.active) {
        // Not our selection or no drag in progress
        XSendEvent(dpy, ev->requestor, False, 0, (XEvent *)&reply);
        return;
    }

    // Build the URI list. text/uri-list format is one URI per line,
    // each line ending with \r\n (CRLF), and a final trailing \r\n.
    char uri_list[8200];
    int len = snprintf(uri_list, sizeof(uri_list), "%s\r\n", src.uri);
    if (len < 0 || len >= (int)sizeof(uri_list)) {
        XSendEvent(dpy, ev->requestor, False, 0, (XEvent *)&reply);
        return;
    }

    // We can provide text/uri-list and text/plain (just the path)
    if (ev->target == g_atoms.text_uri_list) {
        XChangeProperty(dpy, ev->requestor,
                        ev->property, g_atoms.text_uri_list,
                        8,              // 8-bit format (byte string)
                        PropModeReplace,
                        (unsigned char *)uri_list, len);
        reply.property = ev->property;
        fprintf(stderr, "[dnd] SelectionRequest: providing text/uri-list\n");
    } else if (ev->target == g_atoms.text_plain ||
               ev->target == g_atoms.UTF8_STRING) {
        // Provide the raw path as plain text fallback
        XChangeProperty(dpy, ev->requestor,
                        ev->property, ev->target,
                        8,
                        PropModeReplace,
                        (unsigned char *)src.path, (int)strlen(src.path));
        reply.property = ev->property;
        fprintf(stderr, "[dnd] SelectionRequest: providing text/plain\n");
    } else {
        fprintf(stderr, "[dnd] SelectionRequest: unsupported target atom 0x%lx\n",
                (unsigned long)ev->target);
    }

    // Notify the requestor that the data is in the property
    XSendEvent(dpy, ev->requestor, False, 0, (XEvent *)&reply);
    XFlush(dpy);
}

// ── Selection: receiving data (we're the target) ──────────────────────

const char *dnd_handle_selection_notify(Display *dpy, XSelectionEvent *ev)
{
    // The source wrote the file data into our property — read it and
    // copy the file to ~/Desktop.

    if (!tgt.drop_received) return NULL;
    if (ev->property == None) {
        // Source couldn't provide the format — fail gracefully
        fprintf(stderr, "[dnd] SelectionNotify: property=None (conversion failed)\n");
        goto fail;
    }

    // Read the property the source wrote for us
    Atom actual_type;
    int  actual_format;
    unsigned long n_items, bytes_after;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, ev->requestor,
            ev->property, 0, 65536, True,  // True = delete after reading
            AnyPropertyType, &actual_type, &actual_format,
            &n_items, &bytes_after, &data) != Success || !data) {
        fprintf(stderr, "[dnd] SelectionNotify: failed to read property\n");
        goto fail;
    }

    // Parse the text/uri-list data.
    // Format: one "file:///path/to/file" URI per line, \r\n separated.
    // We only handle the first URI (single file drop).
    char *uri_line = (char *)data;

    // Find end of first line
    char *end = uri_line;
    while (*end && *end != '\r' && *end != '\n') end++;
    size_t uri_len = (size_t)(end - uri_line);

    if (uri_len == 0 || uri_len >= sizeof(tgt.dest_path)) {
        fprintf(stderr, "[dnd] SelectionNotify: empty or oversized URI\n");
        XFree(data);
        goto fail;
    }

    // Decode the percent-encoded file:// URI into a plain path
    char src_path[4096];
    char uri_buf[4096];
    memcpy(uri_buf, uri_line, uri_len);
    uri_buf[uri_len] = '\0';

    if (uri_decode_path(uri_buf, src_path, sizeof(src_path)) <= 0) {
        fprintf(stderr, "[dnd] SelectionNotify: URI decode failed: %s\n", uri_buf);
        XFree(data);
        goto fail;
    }
    XFree(data);

    // Build the destination path: ~/Desktop/<filename>
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    // Get just the filename from the source path (basename)
    // We use a local copy because POSIX basename() may modify the string
    char src_copy[4096];
    strncpy(src_copy, src_path, sizeof(src_copy) - 1);
    src_copy[sizeof(src_copy) - 1] = '\0';
    char *fname = basename(src_copy);

    snprintf(tgt.dest_path, sizeof(tgt.dest_path),
             "%s/Desktop/%s", home, fname);

    fprintf(stderr, "[dnd] Copying '%s' → '%s'\n", src_path, tgt.dest_path);

    // Open source for reading
    int fd_in = open(src_path, O_RDONLY);
    if (fd_in < 0) {
        fprintf(stderr, "[dnd] open source failed: %s\n", strerror(errno));
        goto fail;
    }

    // Stat the source to get the file size
    struct stat st;
    if (fstat(fd_in, &st) < 0) {
        close(fd_in);
        goto fail;
    }

    // Open destination for writing (create or truncate)
    int fd_out = open(tgt.dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_out < 0) {
        fprintf(stderr, "[dnd] open dest failed: %s\n", strerror(errno));
        close(fd_in);
        goto fail;
    }

    // Use sendfile() for an efficient in-kernel copy (no user-space buffer needed)
    off_t offset = 0;
    ssize_t copied = sendfile(fd_out, fd_in, &offset, (size_t)st.st_size);
    close(fd_in);
    close(fd_out);

    if (copied < 0 || copied != st.st_size) {
        fprintf(stderr, "[dnd] sendfile failed: %s\n", strerror(errno));
        goto fail;
    }

    fprintf(stderr, "[dnd] File copied successfully (%zd bytes)\n", copied);

    // Tell the source we're done
    {
        XClientMessageEvent fin;
        memset(&fin, 0, sizeof(fin));
        fin.type         = ClientMessage;
        fin.display      = dpy;
        fin.window       = tgt.source;
        fin.message_type = g_atoms.XdndFinished;
        fin.format       = 32;
        fin.data.l[0]    = (long)g_desktop_win;  // target window = us
        fin.data.l[1]    = 1L;    // success
        fin.data.l[2]    = (long)g_atoms.XdndActionCopy;
        XSendEvent(dpy, tgt.source, False, 0, (XEvent *)&fin);
        XFlush(dpy);
    }

    // Reset target state
    tgt.active       = false;
    tgt.source       = None;
    tgt.offered_type = None;
    tgt.drop_received = false;

    // Return the destination path so the caller can trigger a rescan/repaint
    return tgt.dest_path;

fail:
    // Reset target state and tell source we failed
    if (tgt.source != None) {
        XClientMessageEvent fin;
        memset(&fin, 0, sizeof(fin));
        fin.type         = ClientMessage;
        fin.display      = dpy;
        fin.window       = tgt.source;
        fin.message_type = g_atoms.XdndFinished;
        fin.format       = 32;
        fin.data.l[0]    = (long)g_desktop_win;  // target window = us
        fin.data.l[1]    = 0L;    // failure
        fin.data.l[2]    = 0L;
        XSendEvent(dpy, tgt.source, False, 0, (XEvent *)&fin);
        XFlush(dpy);
    }
    tgt.active       = false;
    tgt.source       = None;
    tgt.offered_type = None;
    tgt.drop_received = false;
    return NULL;
}

// ── Idle tick ─────────────────────────────────────────────────────────

bool dnd_tick(void)
{
    // Called every 500ms from the event loop's timeout path.
    // If we sent XdndDrop and haven't received XdndFinished in 3 seconds,
    // the target app probably never sent it (some apps never do).
    // We reset source state so the caller can snap the icon back.

    if (!src.active || !src.drop_sent) {
        return false;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    Time now_ms = (Time)(now.tv_sec * 1000 + now.tv_nsec / 1000000);

    // Check if 3 seconds have elapsed since we sent XdndDrop
    if (src.drop_time > 0 && (now_ms - src.drop_time) > 3000) {
        fprintf(stderr, "[dnd] XdndFinished timeout — resetting source state\n");

        src.active        = false;
        src.target        = None;
        src.entered       = false;
        src.drop_sent     = false;
        src.target_accepted = false;
        src.drop_time     = 0;

        // Return true to signal the caller to snap the icon back to grid
        return true;
    }

    return false;
}
