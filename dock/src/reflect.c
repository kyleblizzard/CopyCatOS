// CopyCatOS — by Kyle Blizzard at Blizzard.show

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
// 3. Apply a gradient mask so it fades from 50% opacity at the top
//    (where it meets the icon) to 0% opacity at the bottom
// 4. The reflection height is 40% of the icon's current size
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

    // How tall the reflection should be (35% of the icon height).
    // In Snow Leopard, the reflection is a short, fading mirror image
    // that appears on the glass shelf surface directly below each icon.
    double reflect_h = icon_size * 0.40;  // 40% of icon height — more visible on the glass

    // The reflection starts just below the bottom edge of the icon
    double reflect_y = y + icon_size;

    // Get the original icon dimensions so we can scale properly
    int src_w = cairo_image_surface_get_width(icon);
    int src_h = cairo_image_surface_get_height(icon);

    // Skip if the icon surface is somehow invalid
    if (src_w <= 0 || src_h <= 0) return;

    // --- Strategy: draw the flipped icon into a temporary surface, then
    // paint that surface onto the main context with a gradient alpha mask.
    // This avoids tricky coordinate math with flipped gradient directions. ---

    // Step 1: Create a temporary surface to hold the flipped, scaled icon
    cairo_surface_t *tmp = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, (int)icon_size, (int)reflect_h);
    cairo_t *tmp_cr = cairo_create(tmp);

    // Clear the temp surface to transparent
    cairo_set_operator(tmp_cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(tmp_cr, 0, 0, 0, 0);
    cairo_paint(tmp_cr);
    cairo_set_operator(tmp_cr, CAIRO_OPERATOR_OVER);

    // Step 2: Draw the icon flipped vertically into the temp surface.
    // We want the BOTTOM of the icon to appear at the TOP of the temp surface
    // (since reflections mirror from the bottom edge upward).
    //
    // Transform: translate so the icon's bottom aligns with y=0 in temp space,
    // then flip Y. The icon is scaled from src dimensions to icon_size.
    double sx = icon_size / (double)src_w;
    double sy = icon_size / (double)src_h;

    cairo_save(tmp_cr);
    // After flipping, the icon extends from y=0 (was bottom) downward.
    // We scale X normally, and negate Y to flip. The translate shifts the
    // flipped image so the old bottom edge sits at y=0 in our temp surface.
    cairo_scale(tmp_cr, sx, -sy);
    // In source-image coordinates, we need to shift the image up so that
    // after the flip the bottom of the icon lands at y=0. The flip turns
    // y into -y, so the original bottom (at src_h) goes to -src_h.
    // We need it at 0, so translate by -src_h in source coords.
    cairo_translate(tmp_cr, 0, -src_h);
    cairo_set_source_surface(tmp_cr, icon, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(tmp_cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(tmp_cr);
    cairo_restore(tmp_cr);

    // Step 3: Apply the fade-out gradient mask.
    // Use DEST_IN operator: keeps existing pixels but replaces their alpha
    // with the alpha from the new source. This lets us fade the reflection.
    cairo_set_operator(tmp_cr, CAIRO_OPERATOR_DEST_IN);
    cairo_pattern_t *mask = cairo_pattern_create_linear(0, 0, 0, reflect_h);

    // Top of reflection (closest to the icon): 35% opacity
    cairo_pattern_add_color_stop_rgba(mask, 0.0, 0, 0, 0, 0.50);  // More visible reflection
    // Bottom of reflection: fully transparent
    cairo_pattern_add_color_stop_rgba(mask, 1.0, 0, 0, 0, 0.0);

    cairo_set_source(tmp_cr, mask);
    cairo_paint(tmp_cr);
    cairo_pattern_destroy(mask);
    cairo_destroy(tmp_cr);

    // Step 4: Paint the finished reflection onto the main context
    cairo_save(cr);
    cairo_set_source_surface(cr, tmp, x, reflect_y);
    cairo_paint(cr);
    cairo_restore(cr);

    cairo_surface_destroy(tmp);
}
