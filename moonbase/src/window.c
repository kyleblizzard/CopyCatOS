// CopyCatOS — by Kyle Blizzard at Blizzard.show

// window.c — client-side window lifecycle.
//
// Slice 3a scope: a MoonBase window is a handle (mb_window_t) cached
// by the framework plus an opaque window_id the compositor returned.
// Everything still happens in a single round-trip:
//
//   app: moonbase_window_create(desc)
//        -> encode MB_IPC_WINDOW_CREATE body
//        -> mb_conn_request waits for MB_IPC_WINDOW_CREATE_REPLY
//        -> parse reply, malloc mb_window_t, return to app
//
// Render surface allocation (SHM / dma-buf), chrome drawing, event
// routing, and backing-scale tracking land in later slices.
//
// Wire schema — WINDOW_CREATE body (app -> compositor), integer keys:
//   1  uint    version              (MOONBASE_WINDOW_DESC_VERSION)
//   2  tstr    title                (optional, omitted if desc->title NULL)
//   3  uint    width_points
//   4  uint    height_points
//   5  uint    min_width_points     (0 = no floor)
//   6  uint    min_height_points
//   7  uint    max_width_points     (0 = no ceiling)
//   8  uint    max_height_points
//   9  uint    render_mode          (0=cairo, 1=gl)
//  10  uint    flags                (MB_WINDOW_FLAG_*)
//
// Wire schema — WINDOW_CREATE_REPLY body (compositor -> app):
//   1  uint    window_id            (opaque, stable for lifetime)
//   2  uint    output_id            (opaque compositor-assigned)
//   3  float   initial_scale        (1.0..4.0)
//   4  uint    actual_width_points  (may differ if clamped)
//   5  uint    actual_height_points

#include "moonbase.h"
#include "internal.h"
#include "ipc/cbor.h"
#include "ipc/transport.h"
#include "moonbase/ipc/kinds.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------
// Opaque handle
// ---------------------------------------------------------------------

struct mb_window {
    uint32_t         id;              // compositor-assigned window_id
    uint32_t         output_id;       // compositor-assigned output_id
    float            backing_scale;   // 1.0 until compositor says otherwise
    int              width_points;
    int              height_points;
    mb_render_mode_t render_mode;
    uint32_t         flags;
    char            *title;           // owned, may be NULL
};

// ---------------------------------------------------------------------
// moonbase_window_create
// ---------------------------------------------------------------------

mb_window_t *moonbase_window_create(const mb_window_desc_t *desc) {
    if (!desc || desc->version != MOONBASE_WINDOW_DESC_VERSION
             || desc->width_points  <= 0
             || desc->height_points <= 0) {
        mb_internal_set_last_error(MB_EINVAL);
        return NULL;
    }
    if (!mb_conn_is_handshaken()) {
        mb_internal_set_last_error(MB_EIPC);
        return NULL;
    }

    // Encode WINDOW_CREATE body. Title is only written when present so
    // the compositor can tell "no title set" from "empty title".
    size_t pairs = 8 + (desc->title ? 1 : 0) + 1;  // base 8 + title? + flags
    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 128);
    mb_cbor_w_map_begin(&w, pairs);
    mb_cbor_w_key(&w, 1);  mb_cbor_w_uint(&w, (uint64_t)desc->version);
    if (desc->title) {
        mb_cbor_w_key(&w, 2); mb_cbor_w_tstr(&w, desc->title);
    }
    mb_cbor_w_key(&w, 3);  mb_cbor_w_uint(&w, (uint64_t)desc->width_points);
    mb_cbor_w_key(&w, 4);  mb_cbor_w_uint(&w, (uint64_t)desc->height_points);
    mb_cbor_w_key(&w, 5);  mb_cbor_w_uint(&w, (uint64_t)desc->min_width_points);
    mb_cbor_w_key(&w, 6);  mb_cbor_w_uint(&w, (uint64_t)desc->min_height_points);
    mb_cbor_w_key(&w, 7);  mb_cbor_w_uint(&w, (uint64_t)desc->max_width_points);
    mb_cbor_w_key(&w, 8);  mb_cbor_w_uint(&w, (uint64_t)desc->max_height_points);
    mb_cbor_w_key(&w, 9);  mb_cbor_w_uint(&w, (uint64_t)desc->render_mode);
    mb_cbor_w_key(&w, 10); mb_cbor_w_uint(&w, (uint64_t)desc->flags);

    if (!mb_cbor_w_ok(&w)) {
        int err = mb_cbor_w_err(&w);
        mb_cbor_w_drop(&w);
        mb_internal_set_last_error((mb_error_t)(err ? err : MB_ENOMEM));
        return NULL;
    }
    size_t body_len = 0;
    uint8_t *body = mb_cbor_w_finish(&w, &body_len);
    if (!body) {
        mb_internal_set_last_error(MB_ENOMEM);
        return NULL;
    }

    uint8_t *reply_body = NULL;
    size_t   reply_len  = 0;
    int rc = mb_conn_request(MB_IPC_WINDOW_CREATE, body, body_len,
                             MB_IPC_WINDOW_CREATE_REPLY,
                             &reply_body, &reply_len);
    free(body);
    if (rc < 0) {
        mb_internal_set_last_error((mb_error_t)rc);
        return NULL;
    }

    // Parse WINDOW_CREATE_REPLY.
    mb_cbor_r_t r;
    mb_cbor_r_init(&r, reply_body, reply_len);
    uint64_t pairs_in = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs_in)) {
        free(reply_body);
        mb_internal_set_last_error(MB_EPROTO);
        return NULL;
    }

    uint32_t window_id  = 0;
    uint32_t output_id  = 0;
    double   init_scale = 1.0;
    uint32_t actual_w   = (uint32_t)desc->width_points;
    uint32_t actual_h   = (uint32_t)desc->height_points;
    bool     have_id    = false;

    for (uint64_t i = 0; i < pairs_in; i++) {
        uint64_t key = 0;
        if (!mb_cbor_r_uint(&r, &key)) { free(reply_body);
            mb_internal_set_last_error(MB_EPROTO); return NULL; }
        switch (key) {
            case 1: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) goto proto;
                window_id = (uint32_t)v; have_id = true; break; }
            case 2: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) goto proto;
                output_id = (uint32_t)v; break; }
            case 3: { if (!mb_cbor_r_float(&r, &init_scale)) goto proto; break; }
            case 4: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) goto proto;
                actual_w = (uint32_t)v; break; }
            case 5: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) goto proto;
                actual_h = (uint32_t)v; break; }
            default:
                if (!mb_cbor_r_skip(&r)) goto proto;
                break;
        }
    }
    free(reply_body);

    if (!have_id) {
        mb_internal_set_last_error(MB_EPROTO);
        return NULL;
    }

    mb_window_t *win = calloc(1, sizeof(*win));
    if (!win) {
        mb_internal_set_last_error(MB_ENOMEM);
        return NULL;
    }
    win->id            = window_id;
    win->output_id     = output_id;
    win->backing_scale = (float)init_scale;
    win->width_points  = (int)actual_w;
    win->height_points = (int)actual_h;
    win->render_mode   = desc->render_mode;
    win->flags         = desc->flags;
    if (desc->title) {
        win->title = strdup(desc->title);  // best-effort; NULL is tolerated
    }
    return win;

proto:
    free(reply_body);
    mb_internal_set_last_error(MB_EPROTO);
    return NULL;
}

// ---------------------------------------------------------------------
// moonbase_window_close
// ---------------------------------------------------------------------
//
// Tells the compositor to tear down its window_id mapping, then frees
// the local handle. No reply is expected — WINDOW_CLOSE is fire-and-
// forget; the compositor acknowledges by posting MB_IPC_WINDOW_CLOSED
// (slice 4 event-queue work). In slice 3a we just send and forget.

void moonbase_window_close(mb_window_t *w) {
    if (!w) {
        mb_internal_set_last_error(MB_EINVAL);
        return;
    }
    if (mb_conn_is_handshaken()) {
        mb_cbor_w_t cw;
        mb_cbor_w_init_grow(&cw, 16);
        mb_cbor_w_map_begin(&cw, 1);
        mb_cbor_w_key(&cw, 1); mb_cbor_w_uint(&cw, w->id);
        size_t body_len = 0;
        uint8_t *body = NULL;
        if (mb_cbor_w_ok(&cw)) {
            body = mb_cbor_w_finish(&cw, &body_len);
        } else {
            mb_cbor_w_drop(&cw);
        }
        if (body) {
            (void)mb_conn_send(MB_IPC_WINDOW_CLOSE, body, body_len, NULL, 0);
            free(body);
        }
    }
    free(w->title);
    free(w);
}

// ---------------------------------------------------------------------
// Accessors backed by the local handle
// ---------------------------------------------------------------------
//
// These used to be ENOSYS stubs. Now that the handle is real, the
// getters serve the cached copy the reply wrote. Setters that must
// round-trip to the compositor (SET_SIZE, SET_POSITION, SET_TITLE)
// stay as stubs — they need later slices to wire replies.

void moonbase_window_size(mb_window_t *w,
                          int *width_points, int *height_points) {
    if (!w) {
        if (width_points)  *width_points  = 0;
        if (height_points) *height_points = 0;
        mb_internal_set_last_error(MB_EINVAL);
        return;
    }
    if (width_points)  *width_points  = w->width_points;
    if (height_points) *height_points = w->height_points;
}

float moonbase_window_backing_scale(mb_window_t *w) {
    if (!w) {
        mb_internal_set_last_error(MB_EINVAL);
        return 1.0f;
    }
    return w->backing_scale;
}

void moonbase_window_backing_pixel_size(mb_window_t *w,
                                        int *width_px, int *height_px) {
    if (!w) {
        if (width_px)  *width_px  = 0;
        if (height_px) *height_px = 0;
        mb_internal_set_last_error(MB_EINVAL);
        return;
    }
    float s = w->backing_scale > 0.0f ? w->backing_scale : 1.0f;
    if (width_px)  *width_px  = (int)(w->width_points  * s + 0.5f);
    if (height_px) *height_px = (int)(w->height_points * s + 0.5f);
}
