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

#include <cairo/cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

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

    // ── Current-frame Cairo draw state (MOONBASE_RENDER_CAIRO path) ──
    //
    // Populated the first time the app calls moonbase_window_cairo()
    // after a commit (or after the window is created). Cleared at the
    // next commit when the fd has been handed off to MoonRock. The
    // layout:
    //
    //   shm_fd      — memfd created via memfd_create(). The shm backs
    //                 the Cairo surface and gets passed to MoonRock
    //                 via SCM_RIGHTS on commit.
    //   shm_map     — mmap of shm_fd in this process. App writes
    //                 pixels directly through Cairo into this region.
    //   shm_size    — size of the mapping (stride * px_height).
    //   px_width/_height — physical pixel dimensions of the surface.
    //   stride      — bytes per row, CAIRO_FORMAT_ARGB32 aligned.
    //   surf / cr   — Cairo objects owning the mapping. They live as
    //                 long as the app is still drawing this frame.
    //
    // At creation time every field is zeroed / -1 so a "nothing to
    // commit yet" check is a single `if (shm_fd < 0)`.
    int             shm_fd;
    uint8_t        *shm_map;
    size_t          shm_size;
    int             px_width, px_height;
    int             stride;
    cairo_surface_t *surf;
    cairo_t         *cr;
};

// ---------------------------------------------------------------------
// Window registry (window_id → mb_window_t *)
// ---------------------------------------------------------------------
//
// The event loop uses this to fill ev->window on compositor-originated
// events that reference a window by id. Small linked list; per-process
// window counts in Snow Leopard-era apps are typically under a dozen.
//
// All mutations happen on the main thread (window_create + window_close
// are main-thread-only, per moonbase.h), so no locking is required.

typedef struct window_reg_node {
    mb_window_t            *win;
    struct window_reg_node *next;
} window_reg_node_t;

static window_reg_node_t *g_registry = NULL;

static void registry_add(mb_window_t *w) {
    window_reg_node_t *n = malloc(sizeof(*n));
    if (!n) return;   // best-effort — event routing will find NULL later
    n->win = w;
    n->next = g_registry;
    g_registry = n;
}

static void registry_remove(mb_window_t *w) {
    window_reg_node_t **p = &g_registry;
    while (*p) {
        if ((*p)->win == w) {
            window_reg_node_t *gone = *p;
            *p = gone->next;
            free(gone);
            return;
        }
        p = &(*p)->next;
    }
}

mb_window_t *mb_internal_window_find(uint32_t window_id) {
    for (window_reg_node_t *n = g_registry; n; n = n->next) {
        if (n->win->id == window_id) return n->win;
    }
    return NULL;
}

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
    win->shm_fd        = -1;   // no pending frame
    if (desc->title) {
        win->title = strdup(desc->title);  // best-effort; NULL is tolerated
    }
    registry_add(win);
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

// Tear down any in-flight Cairo draw state. Called at commit time (once
// the fd has been duped to the kernel for the SCM_RIGHTS hand-off) and
// at close time (so a never-committed frame doesn't leak mmap space).
// Safe to call when no frame is pending.
static void window_release_frame(mb_window_t *w) {
    if (w->cr) {
        cairo_destroy(w->cr);
        w->cr = NULL;
    }
    if (w->surf) {
        cairo_surface_destroy(w->surf);
        w->surf = NULL;
    }
    if (w->shm_map && w->shm_size > 0) {
        munmap(w->shm_map, w->shm_size);
        w->shm_map  = NULL;
        w->shm_size = 0;
    }
    if (w->shm_fd >= 0) {
        close(w->shm_fd);
        w->shm_fd = -1;
    }
    w->px_width = w->px_height = w->stride = 0;
}

void mb_internal_window_apply_backing_scale(mb_window_t *w,
                                            float new_scale,
                                            uint32_t new_output_id) {
    if (!w || new_scale <= 0.0f) return;
    if (w->backing_scale == new_scale && w->output_id == new_output_id) return;

    w->backing_scale = new_scale;
    w->output_id     = new_output_id;

    // Any pending Cairo frame was sized against the old scale. Drop it
    // so the app's next moonbase_window_cairo() allocates at the right
    // physical-pixel dimensions. The app will see MB_EV_BACKING_SCALE_CHANGED
    // and redraw.
    window_release_frame(w);
}

void moonbase_window_close(mb_window_t *w) {
    if (!w) {
        mb_internal_set_last_error(MB_EINVAL);
        return;
    }
    registry_remove(w);
    window_release_frame(w);
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
// moonbase_window_cairo / moonbase_window_commit
// ---------------------------------------------------------------------
//
// The Cairo draw path. Apps ask for a cairo_t, draw, and commit:
//
//     cairo_t *cr = moonbase_window_cairo(win);
//     cairo_set_source_rgb(cr, 1.0, 0.8, 0.2);
//     cairo_paint(cr);
//     moonbase_window_commit(win);
//
// Between those two calls the app is drawing into a shared-memory
// buffer that lives on a memfd. At commit time we send the fd to
// MoonRock via SCM_RIGHTS; MoonRock mmaps it, uploads to a GL texture,
// and composites it with Aqua chrome. After commit the client-side
// fd + mmap + cairo state are released — the next call to
// moonbase_window_cairo allocates a fresh buffer for the next frame.
//
// A fresh memfd per frame is wasteful — a real implementation would
// double-buffer with a buffer-release protocol. That's a later
// optimization slice; per-frame alloc is functionally correct and the
// cost is dominated by the GPU upload anyway on any sane hardware.

// memfd_create wrapper. glibc before 2.27 didn't expose it, so fall
// back to the raw syscall. No flags needed — we keep CLOEXEC implicit
// via the explicit F_SETFD dance.
static int mb_memfd(const char *name, unsigned flags) {
#ifdef MFD_CLOEXEC
    return (int)syscall(SYS_memfd_create, name, flags | MFD_CLOEXEC);
#else
    return (int)syscall(SYS_memfd_create, name, flags);
#endif
}

// CAIRO_FORMAT_ARGB32 stride rule: 4 bytes per pixel, 4-byte aligned
// width. cairo_format_stride_for_width() handles this but isn't
// available on some ancient cairo builds — the math is trivial so we
// do it inline. Keeps one fewer symbol dependency.
static int argb32_stride_for(int width_px) {
    return ((width_px * 4) + 3) & ~3;
}

void *moonbase_window_cairo(mb_window_t *w) {
    if (!w) {
        mb_internal_set_last_error(MB_EINVAL);
        return NULL;
    }
    if (w->render_mode != MOONBASE_RENDER_CAIRO) {
        // A GL-mode window can't hand out a Cairo context. Pass a
        // specific error so the app can tell the mode mismatch from a
        // true failure.
        mb_internal_set_last_error(MB_EINVAL);
        return NULL;
    }

    // Already have a live frame? Hand the same cairo_t back. This lets
    // apps call moonbase_window_cairo() in a loop without leaking.
    if (w->cr) return w->cr;

    // Compute pixel dimensions at the current backing scale. Round to
    // nearest to avoid off-by-one shrinkage on fractional scales.
    float s = w->backing_scale > 0.0f ? w->backing_scale : 1.0f;
    int px_w = (int)(w->width_points  * s + 0.5f);
    int px_h = (int)(w->height_points * s + 0.5f);
    if (px_w <= 0 || px_h <= 0) {
        mb_internal_set_last_error(MB_EINVAL);
        return NULL;
    }
    int stride = argb32_stride_for(px_w);
    size_t size = (size_t)stride * (size_t)px_h;

    // Allocate the shm buffer.
    int fd = mb_memfd("moonbase-window", 0);
    if (fd < 0) {
        mb_internal_set_last_error(MB_ENOMEM);
        return NULL;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        mb_internal_set_last_error(MB_ENOMEM);
        return NULL;
    }
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        mb_internal_set_last_error(MB_ENOMEM);
        return NULL;
    }

    // Wrap the mapping in a Cairo surface. CAIRO_FORMAT_ARGB32 uses
    // native-byte-order premultiplied alpha — on little-endian Linux
    // that's BGRA in memory. MoonRock's GL uploader will match the
    // same format so we don't pay a per-frame byte-swap.
    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        (unsigned char *)map, CAIRO_FORMAT_ARGB32,
        px_w, px_h, stride);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        munmap(map, size);
        close(fd);
        mb_internal_set_last_error(MB_ENOMEM);
        return NULL;
    }
    cairo_t *cr = cairo_create(surf);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
        munmap(map, size);
        close(fd);
        mb_internal_set_last_error(MB_ENOMEM);
        return NULL;
    }

    // Points-to-pixels: every Cairo draw call the app makes is in
    // points. The framework scales them up to physical pixels so the
    // same source code draws pixel-correct on a 1.0x external and a
    // 1.5x Legion Go S panel with zero app-side math.
    if (s != 1.0f) cairo_scale(cr, s, s);

    w->shm_fd   = fd;
    w->shm_map  = (uint8_t *)map;
    w->shm_size = size;
    w->px_width = px_w;
    w->px_height = px_h;
    w->stride   = stride;
    w->surf     = surf;
    w->cr       = cr;
    return cr;
}

int moonbase_window_commit(mb_window_t *w) {
    if (!w) {
        mb_internal_set_last_error(MB_EINVAL);
        return MB_EINVAL;
    }
    // A commit with no pending draw call is a no-op, not an error —
    // apps that do their own dirty tracking can hit this path a lot.
    if (w->shm_fd < 0) return 0;
    if (!mb_conn_is_handshaken()) {
        window_release_frame(w);
        mb_internal_set_last_error(MB_EIPC);
        return MB_EIPC;
    }

    // Flush Cairo so every pending draw has landed in the mmap before
    // we hand the fd off to MoonRock. Without this the compositor can
    // upload a partially-drawn frame and the user sees tearing.
    if (w->cr) cairo_surface_flush(w->surf);

    // Compose the WINDOW_COMMIT body. Damage covers the full window
    // for now — a future slice lets apps pass a tighter rect via
    // moonbase_window_request_redraw().
    mb_cbor_w_t cw;
    mb_cbor_w_init_grow(&cw, 64);
    mb_cbor_w_map_begin(&cw, 9);
    mb_cbor_w_key(&cw, 1); mb_cbor_w_uint(&cw, w->id);
    mb_cbor_w_key(&cw, 2); mb_cbor_w_uint(&cw, (uint64_t)w->px_width);
    mb_cbor_w_key(&cw, 3); mb_cbor_w_uint(&cw, (uint64_t)w->px_height);
    mb_cbor_w_key(&cw, 4); mb_cbor_w_uint(&cw, (uint64_t)w->stride);
    mb_cbor_w_key(&cw, 5); mb_cbor_w_uint(&cw, 0);  // 0 = ARGB32 premul
    mb_cbor_w_key(&cw, 6); mb_cbor_w_uint(&cw, 0);                          // damage x
    mb_cbor_w_key(&cw, 7); mb_cbor_w_uint(&cw, 0);                          // damage y
    mb_cbor_w_key(&cw, 8); mb_cbor_w_uint(&cw, (uint64_t)w->px_width);      // damage w
    mb_cbor_w_key(&cw, 9); mb_cbor_w_uint(&cw, (uint64_t)w->px_height);     // damage h
    if (!mb_cbor_w_ok(&cw)) {
        mb_cbor_w_drop(&cw);
        window_release_frame(w);
        mb_internal_set_last_error(MB_ENOMEM);
        return MB_ENOMEM;
    }
    size_t body_len = 0;
    uint8_t *body = mb_cbor_w_finish(&cw, &body_len);
    if (!body) {
        window_release_frame(w);
        mb_internal_set_last_error(MB_ENOMEM);
        return MB_ENOMEM;
    }

    int fd = w->shm_fd;
    int rc = mb_conn_send(MB_IPC_WINDOW_COMMIT, body, body_len, &fd, 1);
    free(body);

    // The kernel has dup'd our fd for delivery, so releasing our copy
    // is correct regardless of whether send succeeded. On failure,
    // MoonRock either didn't receive the fd (no harm) or dropped it
    // on decode (the send already returned an error).
    window_release_frame(w);

    if (rc < 0) {
        mb_internal_set_last_error((mb_error_t)rc);
        return rc;
    }
    return 0;
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
