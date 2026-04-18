// CopyCatOS — by Kyle Blizzard at Blizzard.show

// CopyCatOS Window Manager — Asset loading (real Snow Leopard PNGs)

#ifndef AURA_ASSETS_H
#define AURA_ASSETS_H

#include "wm.h"
#include <cairo/cairo.h>

// Load all decoration assets from disk
void assets_load(CCWM *wm);

// Get traffic light button surfaces (from real SL screenshot crops)
cairo_surface_t *assets_get_close_button(void);
cairo_surface_t *assets_get_minimize_button(void);
cairo_surface_t *assets_get_zoom_button(void);

// Cleanup
void assets_shutdown(void);

#endif // AURA_ASSETS_H
