// CopyCatOS — by Kyle Blizzard at Blizzard.show

// render.h — Rendering utilities for the menu bar
//
// This module handles all drawing operations: the background gradient,
// text rendering via Pango (using Lucida Grande font), hover highlights,
// and optional background texture tiling.
//
// Cairo is a 2D graphics library that draws to surfaces (like X windows).
// Pango is a text layout engine that handles fonts and Unicode properly.
// We use Pango through Cairo's integration layer (PangoCairo).

#ifndef AURA_RENDER_H
#define AURA_RENDER_H

#include <cairo/cairo.h>
#include <stdbool.h>

// We need the full MenuBar definition for function signatures.
// There's no circular dependency here — menubar.h doesn't include render.h.
#include "menubar.h"

// Load optional background texture (menubar_bg.png).
// If the texture file is missing, the gradient fallback is used instead.
void render_init(MenuBar *mb);

// Draw the menu bar background across the full width.
// Uses the loaded texture if available, otherwise paints a 4-stop
// vertical gradient matching Snow Leopard's appearance.
void render_background(MenuBar *mb, cairo_t *cr);

// Draw a text string at the given position using Pango + Lucida Grande.
// Parameters:
//   cr   — Cairo drawing context
//   text — UTF-8 string to render
//   x, y — Position (top-left of the text bounding box)
//   bold — true for bold weight (used for app name), false for regular
//   r, g, b — Text color as 0.0–1.0 floats
// Returns the pixel width of the rendered text (useful for layout).
double render_text(cairo_t *cr, const char *text, double x, double y,
                   bool bold, double r, double g, double b);

// Measure the pixel width of a text string without drawing it.
// Used during layout to figure out where each menu item should go.
double render_measure_text(const char *text, bool bold);

// Draw a semi-transparent rounded rectangle over a hovered menu item.
// This provides the subtle highlight effect when the mouse is over a menu title.
void render_hover_highlight(cairo_t *cr, int x, int y, int w, int h);

// Free any resources allocated by render_init (background texture, etc.)
void render_cleanup(void);

#endif // AURA_RENDER_H
