// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// wallpaper.h — Wallpaper loading and rendering
//
// Loads a wallpaper image (JPEG or PNG) and paints it onto the desktop
// window. Supports aspect-fill scaling so the image always covers the
// entire screen without distortion (any overflow is center-cropped).
//
// JPEG support uses libjpeg directly because Cairo can only load PNG
// natively. The JPEG pixel data (RGB) gets converted to Cairo's BGRX
// format (CAIRO_FORMAT_RGB24 = 0xXXRRGGBB in native byte order).

#ifndef AURA_WALLPAPER_H
#define AURA_WALLPAPER_H

#include <X11/Xlib.h>
#include <cairo/cairo.h>
#include <stdbool.h>

// Initialize the wallpaper module: load the image from disk, scale it
// to fill the screen, and cache the resulting Cairo surface.
//
// Search order for the wallpaper image:
//   1. The explicit path passed in (if not NULL)
//   2. $HOME/.local/share/aqua-widgets/wallpaper/default.jpg
//   3. $HOME/.local/share/aqua-widgets/wallpaper/aurora-4k.jpg
//   4. Solid fallback color #3A6EA5 (Snow Leopard default blue)
//
// Returns true on success (even if we fell back to solid color).
bool wallpaper_init(const char *path, int screen_w, int screen_h);

// Paint the cached wallpaper surface onto the given Cairo context.
// This is called during expose/repaint events.
void wallpaper_paint(cairo_t *cr, int win_w, int win_h);

// Get the cached wallpaper surface (other modules may need it for
// compositing, e.g., context menu blur effects).
cairo_surface_t *wallpaper_get_surface(void);

// Free the cached wallpaper surface and any related resources.
void wallpaper_shutdown(void);

#endif // AURA_WALLPAPER_H
