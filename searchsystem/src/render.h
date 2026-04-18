// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ─── render.h ───
// Public interface for drawing the Spotlight overlay.
//
// All rendering is done with Cairo + Pango onto a 32-bit ARGB
// surface backed by the Xlib overlay window.  The caller is
// responsible for providing a valid Cairo surface that maps to
// the window's pixels.

#ifndef RENDER_H
#define RENDER_H

#include <cairo/cairo.h>
#include "search.h"

// Draw the complete overlay frame: background, shadow, search
// field, and result list.
//
// cr              — Cairo context targeting the overlay window
// width / height  — current window dimensions in pixels
// query           — the text the user has typed so far
// results         — array of pointers to matching SearchEntry items
// result_count    — how many entries are in the results array
// selected        — index of the currently highlighted row (0-based)
void render_frame(cairo_t *cr,
                  int width, int height,
                  const char *query,
                  SearchEntry **results,
                  int result_count,
                  int selected);

#endif // RENDER_H
