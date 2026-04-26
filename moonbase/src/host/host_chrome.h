// CopyCatOS — by Kyle Blizzard at Blizzard.show

// host_chrome.h — Aqua chrome painter for compositor-internal MoonBase
// surfaces. Shared between moonrock (full-session compositor) and
// moonrock-lite (foreign-distro sidecar). CPU-only Cairo path; the
// caller uploads or blits the resulting pixels.
//
// MoonBase windows are NOT X windows, so the X-WM frame.c/decor.c path
// (Xlib-backed Cairo into a frame window's pixmap) doesn't apply. The
// painter targets a private cairo_image_surface; moonrock uploads it
// to a GL texture, moonrock-lite stamps it into an X drawable.
//
// Cairo and Pango are deliberately kept out of this header so consumers
// can include it without dragging those into their TU graphs. The
// public struct uses void* for the Cairo surface; the painter takes
// the three traffic-light button images as void* with a contract that
// they're cairo_surface_t*.

#ifndef MB_HOST_CHROME_H
#define MB_HOST_CHROME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Snow Leopard 10.6 chrome geometry, in points. Scaled by the painter
// at every backing scale. Single source of truth — moonrock's wm.h and
// moonrock-lite both #include this header for these.
#define MB_CHROME_TITLEBAR_HEIGHT   22
#define MB_CHROME_BORDER_WIDTH       1
#define MB_CHROME_BUTTON_DIAMETER   13
#define MB_CHROME_BUTTON_SPACING     7
#define MB_CHROME_BUTTON_LEFT_PAD    8
#define MB_CHROME_BUTTON_TOP_PAD     4

// One chrome state per MoonBase surface. Lifetime is tied to the
// surface's; the painter re-allocates the Cairo surface when outer
// dimensions change. The `revision` counter increments each time the
// pixels change so consumers can skip redundant uploads/blits.
typedef struct {
    // Physical-pixel dimensions of the outer chrome rectangle. The
    // content rect sits inside at (content_x_inset, content_y_inset).
    uint32_t chrome_w, chrome_h;

    // Inset of the content rect inside the chrome. Filled by the paint
    // call so the caller can position the content quad correctly.
    uint32_t content_x_inset, content_y_inset;

    // Cached Cairo surface + its pixels. Re-allocated when chrome_w or
    // chrome_h changes. Owned; freed by mb_chrome_release.
    void    *cairo_surface;     // cairo_surface_t *
    uint8_t *pixels;            // borrowed from cairo_surface
    uint32_t stride;            // bytes per row (cairo stride)

    // Bumped each time the Cairo pixels change. Consumers compare to
    // their own last-uploaded value to decide whether to re-blit / re-
    // glTexImage2D this frame.
    uint64_t revision;
} mb_chrome_t;

// Repaint the chrome.
//   content_w / content_h: client's committed buffer, physical pixels.
//   scale:                 backing scale (1.0, 1.5, ...).
//   title:                 NULL renders as "(Untitled)".
//   active:                focused state.
//   buttons_hover:         pointer is in the traffic-light region.
//   pressed_button:        0/1/2/3 (none/close/minimize/zoom).
//   btn_imgs[3]:           cairo_surface_t* close/minimize/zoom buttons,
//                          or {NULL,NULL,NULL} to fall back to inactive
//                          gray dots regardless of `active`.
// Returns true on success; false on allocation failure.
bool mb_chrome_repaint(mb_chrome_t *chrome,
                       uint32_t content_w, uint32_t content_h,
                       float    scale,
                       const char *title,
                       bool     active,
                       bool     buttons_hover,
                       int      pressed_button,
                       void *const btn_imgs[3]);

// Free the Cairo surface owned by the chrome. Pure CPU cleanup —
// consumers that own GL textures or X drawables tied to these pixels
// must release those separately. Safe on an all-zero struct.
void mb_chrome_release(mb_chrome_t *chrome);

// Paint just the title strip (gradient + traffic lights + centered title)
// directly into the caller's Cairo context, top-left origin (0,0). Used by
// moonrock-lite, which owns its own X drawable + cairo_xlib_surface and
// blits the chrome bar above the bundle window — it doesn't need the
// rounded-top clip or the side/bottom hairlines, just the title strip
// pixels mb_chrome_repaint already knows how to draw.
//
//   width_px / height_px: physical-pixel size of the strip.
//   scale:                backing scale (1.0, 1.5, ...).
//   title:                NULL renders as "(Untitled)".
//   active:               focused state.
//   buttons_hover:        pointer is in the traffic-light region.
//   pressed_button:       0/1/2/3 (none/close/minimize/zoom).
//   btn_imgs[3]:          cairo_surface_t* close/minimize/zoom buttons,
//                         or {NULL,NULL,NULL} for the gray-dot fallback.
void mb_chrome_paint_title_strip(void *cr,
                                 int width_px, int height_px,
                                 float scale,
                                 const char *title,
                                 bool active,
                                 bool buttons_hover,
                                 int  pressed_button,
                                 void *const btn_imgs[3]);

#ifdef __cplusplus
}
#endif

#endif // MB_HOST_CHROME_H
