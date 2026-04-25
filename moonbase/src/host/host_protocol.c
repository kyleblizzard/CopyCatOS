// CopyCatOS — by Kyle Blizzard at Blizzard.show

// host_protocol.c — implementation of the parsers and encoders declared
// in host_protocol.h. Pure CBOR — no compositor state, no globals. Both
// moonrock and moonrock-lite link this and produce identical bytes on
// the wire.

#include "host_protocol.h"

#include "ipc/cbor.h"
#include "moonbase/ipc/kinds.h"

#include <stdlib.h>
#include <string.h>

// ─── Parsers ────────────────────────────────────────────────────────────

bool mb_host_parse_window_create(const uint8_t *body, size_t body_len,
                                 mb_host_window_create_req_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    mb_cbor_r_t r;
    mb_cbor_r_init(&r, body, body_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs)) return false;

    bool have_w = false, have_h = false;
    for (uint64_t i = 0; i < pairs; i++) {
        uint64_t key = 0;
        if (!mb_cbor_r_uint(&r, &key)) return false;
        switch (key) {
            case 1: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return false;
                out->version = (uint32_t)v; break; }
            case 2: { const char *s = NULL; size_t sl = 0;
                if (!mb_cbor_r_tstr(&r, &s, &sl)) return false;
                out->title = malloc(sl + 1);
                if (!out->title) return false;
                memcpy(out->title, s, sl);
                out->title[sl] = '\0'; break; }
            case 3: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return false;
                out->width_points = (int32_t)v; have_w = true; break; }
            case 4: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return false;
                out->height_points = (int32_t)v; have_h = true; break; }
            case 5: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return false;
                out->min_width_points = (int32_t)v; break; }
            case 6: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return false;
                out->min_height_points = (int32_t)v; break; }
            case 7: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return false;
                out->max_width_points = (int32_t)v; break; }
            case 8: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return false;
                out->max_height_points = (int32_t)v; break; }
            case 9: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return false;
                out->render_mode = (uint32_t)v; break; }
            case 10: { uint64_t v = 0;
                if (!mb_cbor_r_uint(&r, &v)) return false;
                out->flags = (uint32_t)v; break; }
            default:
                if (!mb_cbor_r_skip(&r)) return false;
                break;
        }
    }
    return have_w && have_h;
}

void mb_host_window_create_req_free(mb_host_window_create_req_t *req)
{
    if (!req) return;
    free(req->title);
    req->title = NULL;
}

uint32_t mb_host_parse_window_close_id(const uint8_t *body, size_t body_len)
{
    mb_cbor_r_t r;
    mb_cbor_r_init(&r, body, body_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs)) return 0;
    uint32_t window_id = 0;
    for (uint64_t i = 0; i < pairs; i++) {
        uint64_t key = 0;
        if (!mb_cbor_r_uint(&r, &key)) return 0;
        if (key == 1) {
            uint64_t v = 0;
            if (!mb_cbor_r_uint(&r, &v)) return 0;
            window_id = (uint32_t)v;
        } else {
            if (!mb_cbor_r_skip(&r)) return 0;
        }
    }
    return window_id;
}

bool mb_host_parse_window_commit(const uint8_t *body, size_t body_len,
                                 mb_host_window_commit_req_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    mb_cbor_r_t r;
    mb_cbor_r_init(&r, body, body_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs)) return false;

    for (uint64_t i = 0; i < pairs; i++) {
        uint64_t key = 0;
        if (!mb_cbor_r_uint(&r, &key)) return false;
        uint64_t v = 0;
        switch (key) {
            case 1: if (!mb_cbor_r_uint(&r, &v)) return false;
                    out->window_id = (uint32_t)v; out->have_id = true; break;
            case 2: if (!mb_cbor_r_uint(&r, &v)) return false;
                    out->width_px = (uint32_t)v; out->have_w = true; break;
            case 3: if (!mb_cbor_r_uint(&r, &v)) return false;
                    out->height_px = (uint32_t)v; out->have_h = true; break;
            case 4: if (!mb_cbor_r_uint(&r, &v)) return false;
                    out->stride = (uint32_t)v; out->have_stride = true; break;
            case 5: if (!mb_cbor_r_uint(&r, &v)) return false;
                    out->pixel_format = (uint32_t)v; break;
            case 6: if (!mb_cbor_r_uint(&r, &v)) return false;
                    out->damage_x = (uint32_t)v; break;
            case 7: if (!mb_cbor_r_uint(&r, &v)) return false;
                    out->damage_y = (uint32_t)v; break;
            case 8: if (!mb_cbor_r_uint(&r, &v)) return false;
                    out->damage_w = (uint32_t)v; break;
            case 9: if (!mb_cbor_r_uint(&r, &v)) return false;
                    out->damage_h = (uint32_t)v; break;
            default:
                if (!mb_cbor_r_skip(&r)) return false;
                break;
        }
    }
    return out->have_id && out->have_w && out->have_h && out->have_stride;
}

// ─── Encoders ───────────────────────────────────────────────────────────

uint8_t *mb_host_build_window_create_reply(uint32_t window_id,
                                           uint32_t output_id,
                                           double   initial_scale,
                                           uint32_t actual_w_points,
                                           uint32_t actual_h_points,
                                           size_t  *out_len)
{
    if (out_len) *out_len = 0;
    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 48);
    mb_cbor_w_map_begin(&w, 5);
    mb_cbor_w_key(&w, 1); mb_cbor_w_uint (&w, window_id);
    mb_cbor_w_key(&w, 2); mb_cbor_w_uint (&w, output_id);
    mb_cbor_w_key(&w, 3); mb_cbor_w_float(&w, initial_scale);
    mb_cbor_w_key(&w, 4); mb_cbor_w_uint (&w, actual_w_points);
    mb_cbor_w_key(&w, 5); mb_cbor_w_uint (&w, actual_h_points);
    if (!mb_cbor_w_ok(&w)) { mb_cbor_w_drop(&w); return NULL; }
    return mb_cbor_w_finish(&w, out_len);
}

uint8_t *mb_host_build_window_focused(uint32_t window_id, bool focused,
                                      size_t *out_len)
{
    if (out_len) *out_len = 0;
    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 16);
    mb_cbor_w_map_begin(&w, 2);
    mb_cbor_w_key(&w, 1); mb_cbor_w_uint(&w, window_id);
    mb_cbor_w_key(&w, 2); mb_cbor_w_bool(&w, focused);
    if (!mb_cbor_w_ok(&w)) { mb_cbor_w_drop(&w); return NULL; }
    return mb_cbor_w_finish(&w, out_len);
}

uint8_t *mb_host_build_window_closed(uint32_t window_id, size_t *out_len)
{
    if (out_len) *out_len = 0;
    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 16);
    mb_cbor_w_map_begin(&w, 1);
    mb_cbor_w_key(&w, 1); mb_cbor_w_uint(&w, window_id);
    if (!mb_cbor_w_ok(&w)) { mb_cbor_w_drop(&w); return NULL; }
    return mb_cbor_w_finish(&w, out_len);
}

uint8_t *mb_host_build_backing_scale_changed(uint32_t window_id,
                                             float    old_scale,
                                             float    new_scale,
                                             uint32_t output_id,
                                             size_t  *out_len)
{
    if (out_len) *out_len = 0;
    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 32);
    mb_cbor_w_map_begin(&w, 4);
    mb_cbor_w_key(&w, 1); mb_cbor_w_uint (&w, window_id);
    mb_cbor_w_key(&w, 2); mb_cbor_w_float(&w, (double)old_scale);
    mb_cbor_w_key(&w, 3); mb_cbor_w_float(&w, (double)new_scale);
    mb_cbor_w_key(&w, 4); mb_cbor_w_uint (&w, output_id);
    if (!mb_cbor_w_ok(&w)) { mb_cbor_w_drop(&w); return NULL; }
    return mb_cbor_w_finish(&w, out_len);
}

uint8_t *mb_host_build_drag_frame(uint16_t           kind,
                                  uint32_t           window_id,
                                  int                x,
                                  int                y,
                                  uint32_t           modifiers,
                                  const char *const *uris,
                                  size_t             uri_count,
                                  uint64_t           timestamp_us,
                                  size_t            *out_len)
{
    if (out_len) *out_len = 0;

    bool is_enter = (kind == MB_IPC_DRAG_ENTER);
    bool is_drop  = (kind == MB_IPC_DRAG_DROP);
    bool is_over  = (kind == MB_IPC_DRAG_OVER);
    bool is_leave = (kind == MB_IPC_DRAG_LEAVE);
    if (!is_enter && !is_drop && !is_over && !is_leave) return NULL;

    size_t pairs;
    if (is_leave)        pairs = 2;
    else if (is_over)    pairs = 5;
    else                 pairs = 6;   // ENTER / DROP

    // Headroom for map header + small keys + URIs. URIs are the only
    // thing that can push this over 64 bytes; sum them up once.
    size_t cap = 48;
    if (is_enter || is_drop) {
        for (size_t i = 0; i < uri_count; i++) {
            cap += (uris && uris[i] ? strlen(uris[i]) : 0) + 8;
        }
    }

    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, cap);
    mb_cbor_w_map_begin(&w, pairs);
    mb_cbor_w_key(&w, 1); mb_cbor_w_uint(&w, window_id);
    if (!is_leave) {
        mb_cbor_w_key(&w, 2); mb_cbor_w_int (&w, (int64_t)x);
        mb_cbor_w_key(&w, 3); mb_cbor_w_int (&w, (int64_t)y);
        mb_cbor_w_key(&w, 4); mb_cbor_w_uint(&w, modifiers);
    }
    if (is_enter || is_drop) {
        mb_cbor_w_key(&w, 5);
        mb_cbor_w_array_begin(&w, uri_count);
        for (size_t i = 0; i < uri_count; i++) {
            const char *u = (uris && uris[i]) ? uris[i] : "";
            mb_cbor_w_tstr_n(&w, u, strlen(u));
        }
    }
    mb_cbor_w_key(&w, 6); mb_cbor_w_uint(&w, timestamp_us);

    if (!mb_cbor_w_ok(&w)) { mb_cbor_w_drop(&w); return NULL; }
    return mb_cbor_w_finish(&w, out_len);
}

uint8_t *mb_host_build_key_event(uint32_t window_id,
                                 uint32_t keycode,
                                 uint32_t modifiers,
                                 bool     is_repeat,
                                 uint64_t ts_us,
                                 size_t  *out_len)
{
    if (out_len) *out_len = 0;
    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 32);
    mb_cbor_w_map_begin(&w, 5);
    mb_cbor_w_key(&w, 1); mb_cbor_w_uint(&w, window_id);
    mb_cbor_w_key(&w, 2); mb_cbor_w_uint(&w, keycode);
    mb_cbor_w_key(&w, 3); mb_cbor_w_uint(&w, modifiers);
    mb_cbor_w_key(&w, 4); mb_cbor_w_bool(&w, is_repeat);
    mb_cbor_w_key(&w, 5); mb_cbor_w_uint(&w, ts_us);
    if (!mb_cbor_w_ok(&w)) { mb_cbor_w_drop(&w); return NULL; }
    return mb_cbor_w_finish(&w, out_len);
}

uint8_t *mb_host_build_text_input(uint32_t    window_id,
                                  const char *utf8,
                                  size_t      utf8_len,
                                  uint64_t    ts_us,
                                  size_t     *out_len)
{
    if (out_len) *out_len = 0;
    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 32 + utf8_len);
    mb_cbor_w_map_begin(&w, 3);
    mb_cbor_w_key(&w, 1); mb_cbor_w_uint   (&w, window_id);
    mb_cbor_w_key(&w, 2); mb_cbor_w_tstr_n (&w, utf8, utf8_len);
    mb_cbor_w_key(&w, 3); mb_cbor_w_uint   (&w, ts_us);
    if (!mb_cbor_w_ok(&w)) { mb_cbor_w_drop(&w); return NULL; }
    return mb_cbor_w_finish(&w, out_len);
}
