// CopyCatOS — by Kyle Blizzard at Blizzard.show

// wallpaper.h — Wallpaper loading and rendering
//
// Loads a wallpaper image (JPEG or PNG) and paints it onto the desktop
// window. Supports aspect-fill scaling so the image always covers the
// entire screen without distortion (any overflow is center-cropped).
//
// JPEG support uses libjpeg directly because Cairo can only load PNG
// natively. The JPEG pixel data (RGB) gets converted to Cairo's BGRX
// format (CAIRO_FORMAT_RGB24 = 0xXXRRGGBB in native byte order).

#ifndef DESKTOP_WALLPAPER_H
#define DESKTOP_WALLPAPER_H

#include <X11/Xlib.h>
#include <cairo/cairo.h>
#include <stdbool.h>

// Initialize the wallpaper module: load the unscaled image from disk
// once. Per-output scaled copies are produced on demand by wallpaper_paint
// and cached, so a multi-monitor setup with two outputs of different
// sizes ends up with one source surface plus two scaled surfaces.
//
// Search order for the wallpaper image:
//   1. The explicit path passed in (if not NULL)
//   2. $HOME/.local/share/aqua-widgets/wallpaper/default.jpg
//   3. $HOME/.local/share/aqua-widgets/wallpaper/aurora-4k.jpg
//   4. Solid fallback color #3A6EA5 (Snow Leopard default blue)
//
// Returns true on success (even if we fell back to solid color).
bool wallpaper_init(const char *path);

// Paint the wallpaper onto the given Cairo context, scaled to fill an
// output of (win_w × win_h) physical pixels. The Cairo surface targets
// one output's window with origin (0, 0), so no virtual-screen offset is
// needed here. Scaled copies are cached per-size; a hotplug that resizes
// an output causes a one-time recompute, then steady-state reuse.
void wallpaper_paint(cairo_t *cr, int win_w, int win_h);

// Drop any cached scaled surfaces. Called on _MOONROCK_OUTPUT_SCALES
// changes that resize one or more outputs — the per-size cache becomes
// stale, so we throw it away and let the next paint repopulate.
void wallpaper_invalidate_cache(void);

// Free the cached wallpaper surface and any related resources.
void wallpaper_shutdown(void);

#endif // DESKTOP_WALLPAPER_H
