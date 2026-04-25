// CopyCatOS — by Kyle Blizzard at Blizzard.show

// host_protocol.h — CBOR parsers + encoders for MoonBase host
// frames. Compositor-agnostic. Shared by moonrock and moonrock-lite so
// the IPC schema lives in exactly one place.
//
// Each builder returns a malloc'd buffer the caller must free(); on
// allocation failure they return NULL and *out_len stays 0. Each parser
// returns true on success and false when the body is malformed or the
// minimum-required key set is missing — see IPC.md §5 for per-frame
// schemas.

#ifndef MB_HOST_PROTOCOL_H
#define MB_HOST_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── Parsers ────────────────────────────────────────────────────────────

// MB_IPC_WINDOW_CREATE body. `title` is malloc'd by the parser if
// present; release with mb_host_window_create_req_free().
typedef struct {
    uint32_t version;
    char    *title;
    int32_t  width_points, height_points;
    int32_t  min_width_points, min_height_points;
    int32_t  max_width_points, max_height_points;
    uint32_t render_mode;          // 0 cairo, 1 gl
    uint32_t flags;
} mb_host_window_create_req_t;

bool mb_host_parse_window_create(const uint8_t *body, size_t body_len,
                                 mb_host_window_create_req_t *out);

void mb_host_window_create_req_free(mb_host_window_create_req_t *req);

// MB_IPC_WINDOW_CLOSE body. Returns 0 on failure — 0 is never a valid
// window_id since the counter starts at 1.
uint32_t mb_host_parse_window_close_id(const uint8_t *body, size_t body_len);

// MB_IPC_WINDOW_COMMIT body.
typedef struct {
    uint32_t window_id;
    uint32_t width_px, height_px;
    uint32_t stride;
    uint32_t pixel_format;
    uint32_t damage_x, damage_y, damage_w, damage_h;
    bool     have_id, have_w, have_h, have_stride;
} mb_host_window_commit_req_t;

bool mb_host_parse_window_commit(const uint8_t *body, size_t body_len,
                                 mb_host_window_commit_req_t *out);

// ─── Encoders (return malloc'd CBOR body; caller frees) ─────────────────

// MB_IPC_WINDOW_CREATE_REPLY:
//   { 1: window_id, 2: output_id, 3: float scale,
//     4: actual_w_points, 5: actual_h_points }
uint8_t *mb_host_build_window_create_reply(uint32_t window_id,
                                           uint32_t output_id,
                                           double   initial_scale,
                                           uint32_t actual_w_points,
                                           uint32_t actual_h_points,
                                           size_t  *out_len);

// MB_IPC_WINDOW_FOCUSED:
//   { 1: window_id, 2: bool focused }
uint8_t *mb_host_build_window_focused(uint32_t window_id, bool focused,
                                      size_t *out_len);

// MB_IPC_WINDOW_CLOSED:
//   { 1: window_id }
uint8_t *mb_host_build_window_closed(uint32_t window_id, size_t *out_len);

// MB_IPC_BACKING_SCALE_CHANGED:
//   { 1: window_id, 2: float old_scale, 3: float new_scale,
//     4: output_id }
uint8_t *mb_host_build_backing_scale_changed(uint32_t window_id,
                                             float    old_scale,
                                             float    new_scale,
                                             uint32_t output_id,
                                             size_t  *out_len);

// MB_IPC_DRAG_{ENTER,OVER,LEAVE,DROP} per IPC.md §5.3:
//   ENTER / DROP → { 1, 2, 3, 4, 5, 6 }
//   OVER         → { 1, 2, 3, 4, 6 }
//   LEAVE        → { 1, 6 }
// `kind` selects which key set is emitted; an unknown kind returns NULL.
// `uri_count` ignored unless ENTER or DROP. `uris[i]` may be NULL — the
// encoder substitutes an empty tstr.
uint8_t *mb_host_build_drag_frame(uint16_t           kind,
                                  uint32_t           window_id,
                                  int                x,
                                  int                y,
                                  uint32_t           modifiers,
                                  const char *const *uris,
                                  size_t             uri_count,
                                  uint64_t           timestamp_us,
                                  size_t            *out_len);

// MB_IPC_KEY_DOWN / MB_IPC_KEY_UP body:
//   { 1: window_id, 2: keycode, 3: modifiers, 4: bool repeat,
//     5: ts_us }
uint8_t *mb_host_build_key_event(uint32_t window_id,
                                 uint32_t keycode,
                                 uint32_t modifiers,
                                 bool     is_repeat,
                                 uint64_t ts_us,
                                 size_t  *out_len);

// MB_IPC_TEXT_INPUT body:
//   { 1: window_id, 2: utf8 (tstr), 3: ts_us }
// `utf8` need not be NUL-terminated; `utf8_len` is bytes.
uint8_t *mb_host_build_text_input(uint32_t    window_id,
                                  const char *utf8,
                                  size_t      utf8_len,
                                  uint64_t    ts_us,
                                  size_t     *out_len);

#ifdef __cplusplus
}
#endif

#endif // MB_HOST_PROTOCOL_H
