// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase_chrome.h — Aqua chrome for compositor-internal MoonBase
// surfaces.
//
// MoonBase windows are NOT X windows, so decor.c's Xlib-backed Cairo
// path (which paints into an X frame window's pixmap) doesn't apply.
// Instead we paint the chrome into a CPU-side image surface owned by
// moonrock, upload that to a GL texture, and composite it behind the
// app's content rect. Same visual rules as decor.c — same gradient
// constants, same title-bar height, same traffic-light layout — just a
// different destination buffer and a different input model.
//
// This header stays tiny: the caller owns an mb_chrome_t struct
// embedded in mb_surface_t, and calls mb_chrome_repaint whenever the
// inputs (focus, title, scale, content size) change. The dirty flag
// is managed by the caller.

#ifndef MOONROCK_MOONBASE_CHROME_H
#define MOONROCK_MOONBASE_CHROME_H

#include <stdbool.h>
#include <stdint.h>

#include <GL/gl.h>

#ifdef __cplusplus
extern "C" {
#endif

// One chrome state per MoonBase surface. Lives inside mb_surface_t so
// there is exactly one chrome per window and its lifetime is tied to
// the surface's.
typedef struct {
    // Physical-pixel dimensions of the outer chrome rectangle. The
    // content rect sits inside at (border_px, titlebar_px) and has
    // size content_w × content_h.
    uint32_t chrome_w, chrome_h;

    // Inset of the content rect inside the chrome. Filled by the paint
    // call so the caller can position the content quad correctly.
    uint32_t content_x_inset, content_y_inset;

    // Cached Cairo surface + its pixels. Re-allocated when chrome_w or
    // chrome_h changes. Owned; freed by mb_chrome_release.
    void    *cairo_surface;     // cairo_surface_t *, void* keeps cairo out of the header
    uint8_t *pixels;            // borrowed from cairo_surface
    uint32_t stride;            // bytes per row (cairo_image_surface_get_stride)

    // GL texture that mirrors the chrome pixels. Zero until the first
    // upload; re-uploaded whenever mb_chrome_repaint bumps the revision.
    GLuint   tex;
    uint32_t tex_w, tex_h;

    // Revision bump each time the Cairo pixels change. mb_host_render
    // compares to its own last-seen value to decide whether to
    // glTexImage2D / glTexSubImage2D this frame.
    uint64_t revision;
} mb_chrome_t;

// Repaint the chrome for the given inputs. Reallocates the Cairo
// surface if the outer dimensions changed. Returns true on success
// (chrome is ready to upload/draw); false on any allocation failure
// (chrome.tex will be zero and the surface must not be drawn this
// frame).
//
// content_w / content_h are the physical-pixel dimensions of the
// client's committed buffer. scale is the backing scale (1.0, 1.5…);
// chrome constants (titlebar height, border width, traffic-light
// geometry) scale with it so the chrome matches the client's pixel
// density. title may be NULL (renders as "(Untitled)"). active is the
// focused flag — true until we have real focus routing.
bool mb_chrome_repaint(mb_chrome_t *chrome,
                       uint32_t content_w, uint32_t content_h,
                       float    scale,
                       const char *title,
                       bool     active);

// Release Cairo and GL resources held by the chrome. GL deletion is
// queued via the caller-supplied `defer_gl_delete` callback so the
// actual glDeleteTextures happens from a point known to have the GL
// context current. Safe to call on an all-zero struct.
void mb_chrome_release(mb_chrome_t *chrome,
                       void (*defer_gl_delete)(GLuint tex));

#ifdef __cplusplus
}
#endif

#endif // MOONROCK_MOONBASE_CHROME_H
