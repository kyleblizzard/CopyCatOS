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
// Events with embedded pointers (text input, hotplug name, …) are not
// emitted by this slice. When they land, the ring grows a companion
// side-buffer owned per-slot. For now a plain ring of mb_event_t
// structs is enough.

#define MB_EV_QUEUE_CAP 128

static mb_event_t g_ring[MB_EV_QUEUE_CAP];
static size_t     g_ring_head   = 0;
static size_t     g_ring_tail   = 0;
static size_t     g_ring_count  = 0;
static bool       g_quit_latched = false;

static uint64_t mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void ring_push(const mb_event_t *ev) {
    if (g_ring_count >= MB_EV_QUEUE_CAP) {
        // Overflow: drop the oldest. This should only happen if an app
        // stops pumping for a pathologically long time. Losing events
        // silently is worse than losing the oldest ones, so we bias
        // toward letting the app see the recent ones.
        g_ring_head = (g_ring_head + 1) % MB_EV_QUEUE_CAP;
        g_ring_count--;
    }
    g_ring[g_ring_tail] = *ev;
    g_ring_tail = (g_ring_tail + 1) % MB_EV_QUEUE_CAP;
    g_ring_count++;
}

static int ring_pop(mb_event_t *out) {
    if (g_ring_count == 0) return 0;
    *out = g_ring[g_ring_head];
    g_ring_head = (g_ring_head + 1) % MB_EV_QUEUE_CAP;
    g_ring_count--;
    return 1;
}

void mb_internal_eventloop_shutdown(void) {
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

static void translate_frame(uint16_t kind,
                            const uint8_t *body, size_t body_len) {
    switch (kind) {
        case MB_IPC_WINDOW_CLOSED:
            translate_window_closed(body, body_len);
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
