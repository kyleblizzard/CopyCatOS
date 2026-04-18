// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// indicator.c — Running-app indicator dots
//
// A small glowing dot beneath each running app's icon. This tells the user
// at a glance which apps are currently open.
//
// We first try to load indicator_medium.png from the widget assets directory.
// If that file doesn't exist, we fall back to drawing a procedural dot using
// a radial gradient: white center -> aqua blue -> transparent edge. This
// fallback ensures the dock still looks right even without the image asset.
// ============================================================================

#include "indicator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// How far from the bottom of the dock window to place the indicator center.
// With DOCK_HEIGHT=160 and SHELF_HEIGHT=48, the shelf spans y=112 to y=160.
// We want the dot about 10px above the very bottom so it sits clearly on
// the visible shelf surface.
#define INDICATOR_BOTTOM_OFFSET 10  // Sits on the shelf surface, 10px from dock bottom

bool indicator_load(DockState *state)
{
    // Try to load the indicator image from the aqua-widgets asset directory
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    char path[1024];
    snprintf(path, sizeof(path),
             "%s/.local/share/aqua-widgets/dock/indicator_medium.png", home);

    // Attempt to load the PNG file
    state->indicator_img = cairo_image_surface_create_from_png(path);

    // Check if the load succeeded. Cairo doesn't return NULL on failure;
    // instead it returns a surface with an error status.
    if (cairo_surface_status(state->indicator_img) != CAIRO_STATUS_SUCCESS) {
        // The image file wasn't found or was invalid — clean up and create
        // a procedural indicator instead
        cairo_surface_destroy(state->indicator_img);
        state->indicator_img = NULL;

        // Create a surface for our procedural dot.
        // We use 3x the indicator diameter to give room for the glow halo
        // to spread out naturally. At INDICATOR_SIZE=8, this gives us a
        // 24x24 surface — large enough to see clearly on the shelf.
        int size = INDICATOR_SIZE * 3;
        state->indicator_img = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, size, size);

        cairo_t *cr = cairo_create(state->indicator_img);

        // Draw a radial gradient: bright white center, aqua blue glow, fades out.
        // The gradient has a wider bright core (0.5 instead of 0.4) so the
        // dot is clearly visible against the semi-transparent shelf surface.
        double cx = size / 2.0;
        double cy = size / 2.0;
        double radius = size / 2.0;

        cairo_pattern_t *grad = cairo_pattern_create_radial(
            cx, cy, 0,       // Inner circle: center point with radius 0
            cx, cy, radius   // Outer circle: same center, full radius
        );

        // White hot center — fully opaque
        cairo_pattern_add_color_stop_rgba(grad, 0.0, 1.0, 1.0, 1.0, 1.0);
        // Bright aqua blue glow with a wider core for visibility
        cairo_pattern_add_color_stop_rgba(grad, 0.35, 0.6, 0.85, 1.0, 0.95);
        // Fade to transparent at the edges
        cairo_pattern_add_color_stop_rgba(grad, 1.0, 0.3, 0.6, 1.0, 0.0);

        cairo_set_source(cr, grad);
        cairo_paint(cr);
        cairo_pattern_destroy(grad);

        // Draw a small solid white dot in the center for a crisp highlight.
        // The radial gradient alone blends into the shelf background at
        // small sizes. This opaque core ensures the dot is unmistakable.
        cairo_arc(cr, cx, cy, 2.5, 0, 2 * 3.14159265);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        cairo_fill(cr);

        cairo_destroy(cr);

        cairo_surface_flush(state->indicator_img);
    }

    return true;
}

void indicator_draw(DockState *state, double center_x)
{
    if (!state->indicator_img) return;

    int img_w = cairo_image_surface_get_width(state->indicator_img);
    int img_h = cairo_image_surface_get_height(state->indicator_img);

    // Position the indicator centered horizontally on center_x,
    // and near the bottom of the dock window
    double draw_x = center_x - img_w / 2.0;
    double draw_y = state->win_h - INDICATOR_BOTTOM_OFFSET - img_h / 2.0;

    cairo_save(state->cr);
    cairo_set_source_surface(state->cr, state->indicator_img, draw_x, draw_y);
    cairo_paint(state->cr);
    cairo_restore(state->cr);
}

void indicator_cleanup(DockState *state)
{
    if (state->indicator_img) {
        cairo_surface_destroy(state->indicator_img);
        state->indicator_img = NULL;
    }
}
