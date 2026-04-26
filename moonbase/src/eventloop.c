// CopyCatOS — by Kyle Blizzard at Blizzard.show

// eventloop.c — moonbase_poll_event / moonbase_wait_event / moonbase_quit.
//
// Phase C slice 4: the app-facing event pump. Sits on top of
// src/ipc/transport.c and translates incoming frames into the tagged
// union mb_event_t that moonbase.h exposes.
//
// Shape:
//
//   frame in  (transport)  ──►  translate_frame(kind, body, len)  ──►
//     0+ mb_event_t pushed onto the in-memory event ring
//     ──►  app pops via moonbase_poll_event / moonbase_wait_event
//
// Two frame sources feed the translator:
//   1. mb_conn_pop_queued() — frames that arrived during a
//      mb_conn_request() round-trip but didn't match the awaited reply.
//   2. direct socket reads — poll(fd) + mb_conn_recv().
//
// This slice wires exactly one frame→event translation:
//   MB_IPC_WINDOW_CLOSED  → MB_EV_WINDOW_CLOSED
// plus the lifecycle event
//   moonbase_quit()       → MB_EV_APP_WILL_QUIT
// plus the compositor-EOF synthesis
//   peer closes socket     → MB_EV_APP_WILL_QUIT
//
// Later slices extend `translate_frame` with input events
// (KEY_DOWN / POINTER_* / SCROLL / TOUCH_*), the backing-scale
// broadcast, controller + thermal + power signals, etc. The plumbing
// here stays the same — only the translator grows.
//
// Thread model: every public function in this file must be called
// from the thread that called moonbase_init(). moonbase_dispatch_main
// (the only cross-thread entry) is a later-slice concern.

#include "moonbase.h"
#include "internal.h"
#include "ipc/transport.h"
#include "ipc/cbor.h"
#include "moonbase/ipc/kinds.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------
// Event queue — fixed-size ring
// ---------------------------------------------------------------------
//
// Most events fit in a plain mb_event_t. Events that carry an
// application-visible string (MB_EV_TEXT_INPUT today, CONTROLLER_HOTPLUG
// names later, etc.) keep the heap-owned UTF-8 alive in a parallel
// side-buffer indexed by the same ring slot. On pop, the side-buffer
// entry is transferred to g_last_returned_text so that ev.text.text
// stays valid until the next pump call — matching the pattern most
// event-loop APIs use for pointer-carrying fields.

#define MB_EV_QUEUE_CAP 128

// Heap-owned URI list backing MB_EV_DRAG_ENTER / DROP. Array of count
// char* + a parallel char** of borrowed pointers cast to const char*
// for the public ev.drag.uris field. Freed together.
typedef struct {
    uint32_t            count;
    char              **storage;   // owned UTF-8 strings
    const char        **view;      // const view handed to the app
} mb_drag_payload_t;

static mb_event_t         g_ring[MB_EV_QUEUE_CAP];
static char              *g_ring_text[MB_EV_QUEUE_CAP];
static mb_drag_payload_t *g_ring_drag[MB_EV_QUEUE_CAP];
static size_t             g_ring_head   = 0;
static size_t             g_ring_tail   = 0;
static size_t             g_ring_count  = 0;
static bool               g_quit_latched = false;

// Owns the string that backs the most recently returned event's
// pointer-carrying field. Freed at the start of every pump call, which
// is the documented lifetime for the returned pointer.
static char              *g_last_returned_text = NULL;
static mb_drag_payload_t *g_last_returned_drag = NULL;

static uint64_t mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void free_last_returned_text(void) {
    if (g_last_returned_text) {
        free(g_last_returned_text);
        g_last_returned_text = NULL;
    }
}

static void drag_payload_free(mb_drag_payload_t *p) {
    if (!p) return;
    if (p->storage) {
        for (uint32_t i = 0; i < p->count; i++) free(p->storage[i]);
        free(p->storage);
    }
    free(p->view);
    free(p);
}

static void free_last_returned_drag(void) {
    if (g_last_returned_drag) {
        drag_payload_free(g_last_returned_drag);
        g_last_returned_drag = NULL;
    }
}

// Push with optional owned text and/or owned drag payload. Each extra
// (if non-NULL) is ring-owned: malloc'd string for `owned_text`, an
// mb_drag_payload_t carrying its own storage for `owned_drag`.
static void ring_push_with_sides(const mb_event_t *ev,
                                 char *owned_text,
                                 mb_drag_payload_t *owned_drag) {
    if (g_ring_count >= MB_EV_QUEUE_CAP) {
        // Overflow: drop the oldest. This should only happen if an app
        // stops pumping for a pathologically long time. Losing events
        // silently is worse than losing the oldest ones, so we bias
        // toward letting the app see the recent ones. Free the dropped
        // slot's side-buffers too so we don't leak on wraparound.
        if (g_ring_text[g_ring_head]) {
            free(g_ring_text[g_ring_head]);
            g_ring_text[g_ring_head] = NULL;
        }
        if (g_ring_drag[g_ring_head]) {
            drag_payload_free(g_ring_drag[g_ring_head]);
            g_ring_drag[g_ring_head] = NULL;
        }
        g_ring_head = (g_ring_head + 1) % MB_EV_QUEUE_CAP;
        g_ring_count--;
    }
    g_ring[g_ring_tail] = *ev;
    g_ring_text[g_ring_tail] = owned_text;
    g_ring_drag[g_ring_tail] = owned_drag;
    g_ring_tail = (g_ring_tail + 1) % MB_EV_QUEUE_CAP;
    g_ring_count++;
}

static void ring_push(const mb_event_t *ev) {
    ring_push_with_sides(ev, NULL, NULL);
}

static void ring_push_with_text(const mb_event_t *ev, char *owned_text) {
    ring_push_with_sides(ev, owned_text, NULL);
}

static void ring_push_with_drag(const mb_event_t *ev,
                                mb_drag_payload_t *owned_drag) {
    ring_push_with_sides(ev, NULL, owned_drag);
}

static int ring_pop(mb_event_t *out) {
    if (g_ring_count == 0) return 0;
    *out = g_ring[g_ring_head];
    char              *text = g_ring_text[g_ring_head];
    mb_drag_payload_t *drag = g_ring_drag[g_ring_head];
    g_ring_text[g_ring_head] = NULL;
    g_ring_drag[g_ring_head] = NULL;
    g_ring_head = (g_ring_head + 1) % MB_EV_QUEUE_CAP;
    g_ring_count--;
    if (text) {
        // Transfer ownership to the "last returned" slot. The previous
        // last-returned was already freed at the top of this pump call,
        // so no double-free. ev.text.text stays valid until the next
        // pump call.
        g_last_returned_text = text;
        out->text.text = text;
    }
    if (drag) {
        // Same lifetime story as text: ring → last-returned slot,
        // app-visible pointer into drag->view survives until the next
        // pump call.
        g_last_returned_drag = drag;
        out->drag.uri_count = drag->count;
        out->drag.uris      = drag->view;
    }
    return 1;
}

// Client-side redraw enqueue. Pushes MB_EV_WINDOW_REDRAW straight onto
// the ring so the app's next moonbase_wait_event / _poll_event turn
// sees it. No server round-trip — this slice is the "Path A-plus"
// bridge until MoonRock grows a real REDRAW push (damage-driven,
// expose-driven). Apps that call moonbase_window_request_redraw from
// inside an MB_EV_BACKING_SCALE_CHANGED handler also flow through here.
void mb_internal_eventloop_post_redraw(mb_window_t *w,
                                       int x, int y,
                                       int width, int height) {
    if (!w) return;
    if (width <= 0 || height <= 0) {
        int ww = 0, hh = 0;
        moonbase_window_size(w, &ww, &hh);
        x = 0;
        y = 0;
        width  = ww;
        height = hh;
    }
    mb_event_t ev = {0};
    ev.kind         = MB_EV_WINDOW_REDRAW;
    ev.window       = w;
    ev.timestamp_us = mono_us();
    ev.redraw.x      = x;
    ev.redraw.y      = y;
    ev.redraw.width  = width;
    ev.redraw.height = height;
    ring_push(&ev);
}

void mb_internal_eventloop_shutdown(void) {
    for (size_t i = 0; i < MB_EV_QUEUE_CAP; i++) {
        if (g_ring_text[i]) {
            free(g_ring_text[i]);
            g_ring_text[i] = NULL;
        }
        if (g_ring_drag[i]) {
            drag_payload_free(g_ring_drag[i]);
            g_ring_drag[i] = NULL;
        }
    }
    free_last_returned_text();
    free_last_returned_drag();
    g_ring_head = 0;
    g_ring_tail = 0;
    g_ring_count = 0;
    g_quit_latched = false;
}

// ---------------------------------------------------------------------
// Frame → event translation
// ---------------------------------------------------------------------

static void translate_window_closed(const uint8_t *body, size_t body_len) {
    // WINDOW_CLOSED body: { 1: uint window_id }
    mb_cbor_r_t r;
    mb_cbor_r_init(&r, body, body_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs)) return;

    uint32_t window_id = 0;
    bool     have_id = false;
    for (uint64_t i = 0; i < pairs; i++) {
        uint64_t key = 0;
        if (!mb_cbor_r_uint(&r, &key)) return;
        if (key == 1) {
            uint64_t v = 0;
            if (!mb_cbor_r_uint(&r, &v)) return;
            window_id = (uint32_t)v;
            have_id = true;
        } else {
            if (!mb_cbor_r_skip(&r)) return;
        }
    }
    if (!have_id) return;

    mb_event_t ev = {0};
    ev.kind = MB_EV_WINDOW_CLOSED;
    ev.window = mb_internal_window_find(window_id);
    ev.timestamp_us = mono_us();

    // An unknown window_id means the app already destroyed the handle
    // locally; no event is meaningful to deliver. Drop silently.
    if (!ev.window) return;

    ring_push(&ev);
}

// WINDOW_FOCUSED body: { 1: uint window_id, 2: bool has_focus }.
// Dropped silently if the window_id is unknown (stale reference).
static void translate_window_focused(const uint8_t *body, size_t body_len) {
    mb_cbor_r_t r;
    mb_cbor_r_init(&r, body, body_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs)) return;

    uint32_t window_id = 0;
    bool     has_focus = false;
    bool     have_id = false, have_focus = false;
    for (uint64_t i = 0; i < pairs; i++) {
        uint64_t key = 0;
        if (!mb_cbor_r_uint(&r, &key)) return;
        switch (key) {
            case 1: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return;
                window_id = (uint32_t)v; have_id = true; break; }
            case 2: { bool v = false;
                if (!mb_cbor_r_bool(&r, &v)) return;
                has_focus = v; have_focus = true; break; }
            default:
                if (!mb_cbor_r_skip(&r)) return;
                break;
        }
    }
    if (!have_id || !have_focus) return;

    mb_event_t ev = {0};
    ev.kind = MB_EV_WINDOW_FOCUSED;
    ev.window = mb_internal_window_find(window_id);
    if (!ev.window) return;
    ev.timestamp_us = mono_us();
    ev.focus.has_focus = has_focus;
    ring_push(&ev);
}

// WINDOW_RESIZED body: { 1: uint window_id, 2: uint w_points, 3: uint h_points }.
// Dropped silently if the window_id is unknown (stale reference).
// Reads the window's current width/height into ev.resize.old_*; #115
// will mutate the tracked dims so subsequent moonbase_window_size()
// calls report the new size — this slice locks only the wire and the
// translator.
static void translate_window_resized(const uint8_t *body, size_t body_len) {
    mb_cbor_r_t r;
    mb_cbor_r_init(&r, body, body_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs)) return;

    uint32_t window_id = 0;
    uint32_t w_points  = 0, h_points = 0;
    bool     have_id = false, have_w = false, have_h = false;
    for (uint64_t i = 0; i < pairs; i++) {
        uint64_t key = 0;
        if (!mb_cbor_r_uint(&r, &key)) return;
        switch (key) {
            case 1: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return;
                window_id = (uint32_t)v; have_id = true; break; }
            case 2: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return;
                w_points = (uint32_t)v; have_w = true; break; }
            case 3: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return;
                h_points = (uint32_t)v; have_h = true; break; }
            default:
                if (!mb_cbor_r_skip(&r)) return;
                break;
        }
    }
    if (!have_id || !have_w || !have_h) return;

    mb_window_t *w = mb_internal_window_find(window_id);
    if (!w) return;

    int old_w = 0, old_h = 0;
    moonbase_window_size(w, &old_w, &old_h);

    mb_event_t ev = {0};
    ev.kind = MB_EV_WINDOW_RESIZED;
    ev.window = w;
    ev.timestamp_us = mono_us();
    ev.resize.old_width  = old_w;
    ev.resize.old_height = old_h;
    ev.resize.new_width  = (int)w_points;
    ev.resize.new_height = (int)h_points;
    ring_push(&ev);
}

// KEY_DOWN/KEY_UP body:
//   { 1: window_id, 2: keycode, 3: modifiers, 4: is_repeat, 5: timestamp_us }
// Unknown/missing is_repeat defaults to false (schema says optional).
// Unknown window_ids are silently dropped — the app already closed locally.
static void translate_key(uint16_t kind,
                          const uint8_t *body, size_t body_len) {
    mb_cbor_r_t r;
    mb_cbor_r_init(&r, body, body_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs)) return;

    uint32_t window_id = 0;
    uint32_t keycode = 0;
    uint32_t modifiers = 0;
    bool     is_repeat = false;
    uint64_t timestamp_us = 0;
    bool     have_id = false, have_code = false;
    for (uint64_t i = 0; i < pairs; i++) {
        uint64_t key = 0;
        if (!mb_cbor_r_uint(&r, &key)) return;
        switch (key) {
            case 1: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return;
                window_id = (uint32_t)v; have_id = true; break; }
            case 2: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return;
                keycode = (uint32_t)v; have_code = true; break; }
            case 3: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return;
                modifiers = (uint32_t)v; break; }
            case 4: { bool v = false;
                if (!mb_cbor_r_bool(&r, &v)) return;
                is_repeat = v; break; }
            case 5: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return;
                timestamp_us = v; break; }
            default:
                if (!mb_cbor_r_skip(&r)) return;
                break;
        }
    }
    if (!have_id || !have_code) return;

    mb_event_t ev = {0};
    ev.kind = (kind == MB_IPC_KEY_DOWN) ? MB_EV_KEY_DOWN : MB_EV_KEY_UP;
    ev.window = mb_internal_window_find(window_id);
    if (!ev.window) return;
    ev.timestamp_us = timestamp_us ? timestamp_us : mono_us();
    ev.key.keycode   = keycode;
    ev.key.modifiers = modifiers;
    ev.key.is_repeat = is_repeat;
    ring_push(&ev);
}

// POINTER_MOVE / POINTER_DOWN / POINTER_UP body:
//   { 1: window_id, 2: int x_points, 3: int y_points,
//     4: button (MB_BUTTON_*; 0 on MOVE), 5: modifiers, 6: timestamp_us }
// Coords arrive in points, content-rect-relative — the host already
// stripped the chrome titlebar height and divided by backing scale.
// Stale window_ids are dropped silently (the app already closed locally).
static void translate_pointer(uint16_t kind,
                              const uint8_t *body, size_t body_len) {
    mb_cbor_r_t r;
    mb_cbor_r_init(&r, body, body_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs)) return;

    uint32_t window_id = 0;
    int64_t  x = 0, y = 0;
    uint32_t button = 0;
    uint32_t modifiers = 0;
    uint64_t timestamp_us = 0;
    bool     have_id = false;
    for (uint64_t i = 0; i < pairs; i++) {
        uint64_t key = 0;
        if (!mb_cbor_r_uint(&r, &key)) return;
        switch (key) {
            case 1: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return;
                window_id = (uint32_t)v; have_id = true; break; }
            case 2: { int64_t v = 0;
                if (!mb_cbor_r_int(&r, &v)) return;
                x = v; break; }
            case 3: { int64_t v = 0;
                if (!mb_cbor_r_int(&r, &v)) return;
                y = v; break; }
            case 4: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return;
                button = (uint32_t)v; break; }
            case 5: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return;
                modifiers = (uint32_t)v; break; }
            case 6: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return;
                timestamp_us = v; break; }
            default:
                if (!mb_cbor_r_skip(&r)) return;
                break;
        }
    }
    if (!have_id) return;

    mb_event_t ev = {0};
    switch (kind) {
        case MB_IPC_POINTER_MOVE: ev.kind = MB_EV_POINTER_MOVE; break;
        case MB_IPC_POINTER_DOWN: ev.kind = MB_EV_POINTER_DOWN; break;
        case MB_IPC_POINTER_UP:   ev.kind = MB_EV_POINTER_UP;   break;
        default: return;
    }
    ev.window = mb_internal_window_find(window_id);
    if (!ev.window) return;
    ev.timestamp_us = timestamp_us ? timestamp_us : mono_us();
    ev.pointer.x         = (int)x;
    ev.pointer.y         = (int)y;
    ev.pointer.button    = button;
    ev.pointer.modifiers = modifiers;
    ring_push(&ev);
}

// TEXT_INPUT body:
//   { 1: window_id, 2: tstr text, 3: timestamp_us }
// Tier 1: the server only emits printable ASCII. UTF-8 beyond ASCII
// lands when real IME arrives (inputd + fcitx/IBus). Empty strings and
// stale window_ids are dropped silently.
static void translate_text_input(const uint8_t *body, size_t body_len) {
    mb_cbor_r_t r;
    mb_cbor_r_init(&r, body, body_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs)) return;

    uint32_t    window_id = 0;
    const char *text_ptr = NULL;
    size_t      text_len = 0;
    uint64_t    timestamp_us = 0;
    bool        have_id = false, have_text = false;
    for (uint64_t i = 0; i < pairs; i++) {
        uint64_t key = 0;
        if (!mb_cbor_r_uint(&r, &key)) return;
        switch (key) {
            case 1: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return;
                window_id = (uint32_t)v; have_id = true; break; }
            case 2: {
                if (!mb_cbor_r_tstr(&r, &text_ptr, &text_len)) return;
                have_text = true; break; }
            case 3: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return;
                timestamp_us = v; break; }
            default:
                if (!mb_cbor_r_skip(&r)) return;
                break;
        }
    }
    if (!have_id || !have_text || text_len == 0) return;

    mb_window_t *w = mb_internal_window_find(window_id);
    if (!w) return;   // stale reference — app already closed locally

    char *owned = malloc(text_len + 1);
    if (!owned) return;
    memcpy(owned, text_ptr, text_len);
    owned[text_len] = '\0';

    mb_event_t ev = {0};
    ev.kind = MB_EV_TEXT_INPUT;
    ev.window = w;
    ev.timestamp_us = timestamp_us ? timestamp_us : mono_us();
    // ev.text.text is re-pointed to the owned buffer on pop. Leaving
    // it NULL here is intentional — ring_pop consults g_ring_text[].
    ring_push_with_text(&ev, owned);
}

// BACKING_SCALE_CHANGED body:
//   { 1: window_id, 2: float old_scale, 3: float new_scale, 4: uint output_id }
// Updates the local window handle's cached scale + output_id (so
// moonbase_window_backing_scale reflects the new value immediately,
// and the next moonbase_window_cairo() allocates at the new physical
// pixel size), then pushes MB_EV_BACKING_SCALE_CHANGED for the app.
static void translate_backing_scale_changed(const uint8_t *body, size_t body_len) {
    mb_cbor_r_t r;
    mb_cbor_r_init(&r, body, body_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs)) return;

    uint32_t window_id = 0;
    double   old_scale = 0.0, new_scale = 0.0;
    uint32_t output_id = 0;
    bool     have_id = false, have_old = false, have_new = false;
    for (uint64_t i = 0; i < pairs; i++) {
        uint64_t key = 0;
        if (!mb_cbor_r_uint(&r, &key)) return;
        switch (key) {
            case 1: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return;
                window_id = (uint32_t)v; have_id = true; break; }
            case 2: { double v = 0.0;
                if (!mb_cbor_r_float(&r, &v)) return;
                old_scale = v; have_old = true; break; }
            case 3: { double v = 0.0;
                if (!mb_cbor_r_float(&r, &v)) return;
                new_scale = v; have_new = true; break; }
            case 4: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return;
                output_id = (uint32_t)v; break; }
            default:
                if (!mb_cbor_r_skip(&r)) return;
                break;
        }
    }
    if (!have_id || !have_old || !have_new) return;

    mb_window_t *w = mb_internal_window_find(window_id);
    if (!w) return;   // stale reference — app already closed locally

    mb_internal_window_apply_backing_scale(w, (float)new_scale, output_id);

    mb_event_t ev = {0};
    ev.kind = MB_EV_BACKING_SCALE_CHANGED;
    ev.window = w;
    ev.timestamp_us = mono_us();
    ev.backing_scale.old_scale = (float)old_scale;
    ev.backing_scale.new_scale = (float)new_scale;
    ring_push(&ev);
}

// DRAG_ENTER / DRAG_DROP body:
//   { 1: window_id, 2: int x, 3: int y, 4: uint modifiers,
//     5: array<tstr> uris, 6: timestamp_us }
// DRAG_OVER body:
//   { 1: window_id, 2: int x, 3: int y, 4: uint modifiers,
//     6: timestamp_us }
// DRAG_LEAVE body:
//   { 1: window_id, 6: timestamp_us }
//
// want_uris==true for ENTER/DROP, false for OVER/LEAVE. A missing uris
// array on an ENTER/DROP is fine — the app just sees uri_count == 0.
// An uris array on an OVER/LEAVE frame is skipped.
static void translate_drag(uint16_t kind,
                           const uint8_t *body, size_t body_len) {
    mb_cbor_r_t r;
    mb_cbor_r_init(&r, body, body_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs)) return;

    bool     want_uris = (kind == MB_IPC_DRAG_ENTER ||
                          kind == MB_IPC_DRAG_DROP);
    uint32_t window_id = 0;
    int64_t  x = 0, y = 0;
    uint32_t modifiers = 0;
    uint64_t timestamp_us = 0;
    bool     have_id = false;

    mb_drag_payload_t *drag = NULL;
    if (want_uris) {
        drag = calloc(1, sizeof(*drag));
        if (!drag) return;
    }

    for (uint64_t i = 0; i < pairs; i++) {
        uint64_t key = 0;
        if (!mb_cbor_r_uint(&r, &key)) goto fail;
        switch (key) {
            case 1: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) goto fail;
                window_id = (uint32_t)v; have_id = true; break; }
            case 2: { int64_t v = 0;
                if (!mb_cbor_r_int(&r, &v)) goto fail;
                x = v; break; }
            case 3: { int64_t v = 0;
                if (!mb_cbor_r_int(&r, &v)) goto fail;
                y = v; break; }
            case 4: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) goto fail;
                modifiers = (uint32_t)v; break; }
            case 5: {
                if (!want_uris) {
                    if (!mb_cbor_r_skip(&r)) goto fail;
                    break;
                }
                uint64_t count = 0;
                if (!mb_cbor_r_array_begin(&r, &count)) goto fail;
                if (count > 0) {
                    drag->storage = calloc(count, sizeof(char *));
                    drag->view    = calloc(count, sizeof(const char *));
                    if (!drag->storage || !drag->view) goto fail;
                }
                for (uint64_t u = 0; u < count; u++) {
                    const char *s = NULL; size_t slen = 0;
                    if (!mb_cbor_r_tstr(&r, &s, &slen)) goto fail;
                    char *copy = malloc(slen + 1);
                    if (!copy) goto fail;
                    memcpy(copy, s, slen);
                    copy[slen] = '\0';
                    drag->storage[u] = copy;
                    drag->view[u]    = copy;
                    drag->count = (uint32_t)(u + 1);
                }
                break;
            }
            case 6: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) goto fail;
                timestamp_us = v; break; }
            default:
                if (!mb_cbor_r_skip(&r)) goto fail;
                break;
        }
    }
    if (!have_id) goto fail;

    mb_window_t *w = mb_internal_window_find(window_id);
    if (!w) goto fail;   // stale reference — app already closed locally

    mb_event_t ev = {0};
    switch (kind) {
        case MB_IPC_DRAG_ENTER: ev.kind = MB_EV_DRAG_ENTER; break;
        case MB_IPC_DRAG_OVER:  ev.kind = MB_EV_DRAG_OVER;  break;
        case MB_IPC_DRAG_LEAVE: ev.kind = MB_EV_DRAG_LEAVE; break;
        case MB_IPC_DRAG_DROP:  ev.kind = MB_EV_DRAG_DROP;  break;
        default:                goto fail;
    }
    ev.window = w;
    ev.timestamp_us = timestamp_us ? timestamp_us : mono_us();
    ev.drag.x = (int)x;
    ev.drag.y = (int)y;
    ev.drag.modifiers = modifiers;
    // uri_count + uris stay zero/NULL here; ring_pop() will re-point
    // them at the owned drag payload if we pass one.

    if (drag) {
        ring_push_with_drag(&ev, drag);
    } else {
        ring_push(&ev);
    }
    return;

fail:
    drag_payload_free(drag);
}

static void translate_frame(uint16_t kind,
                            const uint8_t *body, size_t body_len) {
    switch (kind) {
        case MB_IPC_WINDOW_CLOSED:
            translate_window_closed(body, body_len);
            break;
        case MB_IPC_WINDOW_FOCUSED:
            translate_window_focused(body, body_len);
            break;
        case MB_IPC_WINDOW_RESIZED:
            translate_window_resized(body, body_len);
            break;
        case MB_IPC_KEY_DOWN:
        case MB_IPC_KEY_UP:
            translate_key(kind, body, body_len);
            break;
        case MB_IPC_TEXT_INPUT:
            translate_text_input(body, body_len);
            break;
        case MB_IPC_POINTER_MOVE:
        case MB_IPC_POINTER_DOWN:
        case MB_IPC_POINTER_UP:
            translate_pointer(kind, body, body_len);
            break;
        case MB_IPC_BACKING_SCALE_CHANGED:
            translate_backing_scale_changed(body, body_len);
            break;
        case MB_IPC_DRAG_ENTER:
        case MB_IPC_DRAG_OVER:
        case MB_IPC_DRAG_LEAVE:
        case MB_IPC_DRAG_DROP:
            translate_drag(kind, body, body_len);
            break;
        default:
            // Unmapped kind in this slice. Dropping it on the floor is
            // fine — later slices extend this switch per IPC kind.
            break;
    }
}

// ---------------------------------------------------------------------
// Pumping
// ---------------------------------------------------------------------
//
// Both pumping functions drain the transport-internal queue first (it
// holds any unrelated frames seen during a prior mb_conn_request), then
// attempt to read from the socket under a specified timeout. On EOF
// they synthesize MB_EV_APP_WILL_QUIT and latch the quit flag so a
// second pump doesn't pile up another APP_WILL_QUIT.

// Drain every frame currently parked on the transport-internal queue
// through the translator. Cheap and blocking-free.
static void drain_parked_frames(void) {
    for (;;) {
        uint16_t kind = 0;
        uint8_t *body = NULL;
        size_t   body_len = 0;
        int rc = mb_conn_pop_queued(&kind, &body, &body_len);
        if (rc <= 0) break;
        translate_frame(kind, body, body_len);
        free(body);
    }
}

// Synthesize the EOF event exactly once per process life.
static void synth_eof_quit(void) {
    if (g_quit_latched) return;
    g_quit_latched = true;
    mb_event_t q = { .kind = MB_EV_APP_WILL_QUIT,
                     .timestamp_us = mono_us() };
    ring_push(&q);
}

// Non-blocking socket read. Returns 1 on frame consumed, 0 on would-block
// or EOF, negative mb_error_t on failure.
static int pump_once_nonblocking(void) {
    int fd = mb_conn_fd();
    if (fd < 0) return 0;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pr = poll(&pfd, 1, 0);
    if (pr < 0) {
        if (errno == EINTR) return 0;
        return MB_EIPC;
    }
    if (pr == 0) return 0;
    if (!(pfd.revents & POLLIN)) return 0;

    uint16_t kind = 0;
    uint8_t *body = NULL;
    size_t   body_len = 0;
    int rc = mb_conn_recv(&kind, &body, &body_len, NULL, NULL);
    if (rc == 0) { synth_eof_quit(); return 0; }
    if (rc < 0)  return rc;
    translate_frame(kind, body, body_len);
    free(body);
    return 1;
}

int moonbase_poll_event(mb_event_t *ev) {
    if (!ev) {
        mb_internal_set_last_error(MB_EINVAL);
        return MB_EINVAL;
    }

    // Release the heap-owned string (if any) that backed the pointer
    // we handed the app in the last pump call. Doing this at pump
    // entry — not pop — is what pins the documented lifetime:
    // "ev.text.text is valid until the next moonbase_poll_event or
    // moonbase_wait_event call."
    free_last_returned_text();
    free_last_returned_drag();

    drain_parked_frames();
    if (ring_pop(ev)) return 1;

    int rc = pump_once_nonblocking();
    if (rc < 0) {
        mb_internal_set_last_error((mb_error_t)rc);
        return rc;
    }
    if (ring_pop(ev)) return 1;
    return 0;
}

int moonbase_wait_event(mb_event_t *ev, int timeout_ms) {
    if (!ev) {
        mb_internal_set_last_error(MB_EINVAL);
        return MB_EINVAL;
    }

    // See moonbase_poll_event: free the previous pump call's
    // pointer-carrying string at pump entry so ev.text.text stays valid
    // until the next pump call.
    free_last_returned_text();
    free_last_returned_drag();

    drain_parked_frames();
    if (ring_pop(ev)) return 1;

    // timeout_ms == 0 is "same as poll_event": one non-blocking
    // socket check, no blocking. Delegate so the two shapes agree.
    if (timeout_ms == 0) {
        int rc = pump_once_nonblocking();
        if (rc < 0) {
            mb_internal_set_last_error((mb_error_t)rc);
            return rc;
        }
        if (ring_pop(ev)) return 1;
        return 0;
    }

    int fd = mb_conn_fd();
    if (fd < 0) {
        mb_internal_set_last_error(MB_EIPC);
        return MB_EIPC;
    }

    // Monotonic deadline for EINTR-safe waiting. `timeout_ms < 0`
    // means "block forever". The poll() below is called with the
    // remaining budget after each EINTR.
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        int remaining;
        if (timeout_ms < 0) {
            remaining = -1;
        } else {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed = (now.tv_sec - start.tv_sec) * 1000
                         + (now.tv_nsec - start.tv_nsec) / 1000000;
            long rem = (long)timeout_ms - elapsed;
            if (rem <= 0) return 0;
            remaining = (int)rem;
        }

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, remaining);
        if (pr < 0) {
            if (errno == EINTR) continue;
            mb_internal_set_last_error(MB_EIPC);
            return MB_EIPC;
        }
        if (pr == 0) return 0;
        if (!(pfd.revents & POLLIN)) continue;

        uint16_t kind = 0;
        uint8_t *body = NULL;
        size_t   body_len = 0;
        int rc = mb_conn_recv(&kind, &body, &body_len, NULL, NULL);
        if (rc == 0) {
            synth_eof_quit();
        } else if (rc < 0) {
            mb_internal_set_last_error((mb_error_t)rc);
            return rc;
        } else {
            translate_frame(kind, body, body_len);
            free(body);
        }

        if (ring_pop(ev)) return 1;
        // Frame translated into zero events (unmapped kind). Loop and
        // wait for the next frame against the remaining time budget.
    }
}

// ---------------------------------------------------------------------
// moonbase_quit
// ---------------------------------------------------------------------
//
// Posts MB_EV_APP_WILL_QUIT (once), then sends BYE and closes the
// socket. The event stays in the ring so a subsequent poll_event /
// wait_event can see it — an app using the poll/wait style will pick
// it up on the next turn and decide whether to break its own loop.

void moonbase_quit(int exit_code) {
    (void)exit_code;
    if (!g_quit_latched) {
        g_quit_latched = true;
        mb_event_t ev = { .kind = MB_EV_APP_WILL_QUIT,
                          .timestamp_us = mono_us() };
        ring_push(&ev);
    }
    // mb_conn_close is idempotent.
    mb_conn_close();
}
