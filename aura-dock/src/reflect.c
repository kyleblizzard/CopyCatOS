// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// reflect.c — Icon reflections on the glass shelf
//
// Each dock icon gets a faint reflection drawn below it, as if the glass
// shelf is reflecting the icon. This is a core part of the Snow Leopard
// dock aesthetic.
//
// How it works:
// 1. Flip the icon vertically (mirror it top-to-bottom)
// 2. Draw it below the original icon
// 3. Apply a gradient mask so it fades from 35% opacity at the top
//    (where it meets the icon) to 0% opacity at the bottom
// 4. The reflection height is 35% of the icon's current size
//
// Cairo's compositing operators make this straightforward:
// - We paint the flipped icon, then use a gradient as an alpha mask
//   to make it fade out.
// ============================================================================

#include "reflect.h"
#include <cairo/cairo.h>

void reflect_draw(cairo_t *cr, cairo_surface_t *icon, double x, double y,
                  double icon_size)
{
    if (!icon) return;

    // How tall the reflection should be (35% of the icon height)
    double reflect_h = icon_size * 0.35;

    // The reflection starts just below the bottom edge of the icon
    double reflect_y = y + icon_size;

    // Get the original icon dimensions so we can scale properly
    int src_w = cairo_image_surface_get_width(icon);
    int src_h = cairo_image_surface_get_height(icon);

    // Skip if the icon surface is somehow invalid
    if (src_w <= 0 || src_h <= 0) return;

    cairo_save(cr);

    // Clip to the reflection area so we don't draw outside it.
    // The reflection should only appear in this rectangle.
    cairo_rectangle(cr, x, reflect_y, icon_size, reflect_h);
    cairo_clip(cr);

    // --- Flip the icon vertically ---
    // To flip an image in Cairo, we use a transformation matrix.
    // We translate to the bottom of where the reflected image should be,
    // then scale Y by -1 to flip it. This effectively mirrors the icon
    // around the horizontal axis at the icon's bottom edge.
    cairo_translate(cr, x, reflect_y);

    // Scale to fit the icon into icon_size pixels, and flip Y axis.
    // The negative Y scale (-icon_size/src_h) flips the image vertically.
    // We also need to shift down by icon_size because the flip moves the
    // image upward (since we're drawing from the flipped origin).
    double sx = icon_size / (double)src_w;
    double sy = icon_size / (double)src_h;
    cairo_scale(cr, sx, -sy);

    // Draw the flipped icon. We translate back so the image starts at
    // the right position after the flip transformation.
    cairo_set_source_surface(cr, icon, 0, 0);

    // --- Create the fade-out gradient mask ---
    // This is a linear gradient that goes from semi-transparent at the top
    // of the reflection to fully transparent at the bottom.
    // Since we're in the flipped coordinate space, we need to work in
    // source image coordinates.
    cairo_pattern_t *mask = cairo_pattern_create_linear(0, 0, 0, -reflect_h / sy);

    // Top of reflection (where it meets the icon): 35% opacity
    cairo_pattern_add_color_stop_rgba(mask, 0.0, 1, 1, 1, 0.35);

    // Bottom of reflection: fully transparent
    cairo_pattern_add_color_stop_rgba(mask, 1.0, 1, 1, 1, 0.0);

    // Paint the flipped icon through the gradient mask.
    // cairo_mask() uses the alpha channel of the mask pattern to control
    // how much of the source (our flipped icon) actually appears.
    cairo_mask(cr, mask);

    cairo_pattern_destroy(mask);
    cairo_restore(cr);
}
