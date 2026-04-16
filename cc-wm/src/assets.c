// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// CopyCatOS Window Manager — Asset loading
// Loads real Snow Leopard PNG assets from ~/.local/share/aqua-widgets/

#include "assets.h"
#include <cairo/cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cairo_surface_t *s_close = NULL;
static cairo_surface_t *s_minimize = NULL;
static cairo_surface_t *s_zoom = NULL;

static cairo_surface_t *load_png(const char *path)
{
    cairo_surface_t *s = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "[cc-wm] Failed to load asset: %s (%s)\n",
                path, cairo_status_to_string(cairo_surface_status(s)));
        cairo_surface_destroy(s);
        return NULL;
    }
    fprintf(stderr, "[cc-wm] Loaded asset: %s (%dx%d)\n",
            path,
            cairo_image_surface_get_width(s),
            cairo_image_surface_get_height(s));
    return s;
}

void assets_load(CCWM *wm)
{
    (void)wm;

    const char *home = getenv("HOME");
    if (!home) home = "/home/nobara-user";

    char path[512];

    // Traffic light buttons — cropped from real Snow Leopard screenshot
    snprintf(path, sizeof(path), "%s/.local/share/aqua-widgets/sl_close_button.png", home);
    s_close = load_png(path);

    snprintf(path, sizeof(path), "%s/.local/share/aqua-widgets/sl_minimize_button.png", home);
    s_minimize = load_png(path);

    snprintf(path, sizeof(path), "%s/.local/share/aqua-widgets/sl_zoom_button.png", home);
    s_zoom = load_png(path);

    if (s_close && s_minimize && s_zoom) {
        fprintf(stderr, "[cc-wm] All decoration assets loaded\n");
    } else {
        fprintf(stderr, "[cc-wm] WARNING: Some assets missing — using fallback rendering\n");
    }
}

cairo_surface_t *assets_get_close_button(void) { return s_close; }
cairo_surface_t *assets_get_minimize_button(void) { return s_minimize; }
cairo_surface_t *assets_get_zoom_button(void) { return s_zoom; }

void assets_shutdown(void)
{
    if (s_close) { cairo_surface_destroy(s_close); s_close = NULL; }
    if (s_minimize) { cairo_surface_destroy(s_minimize); s_minimize = NULL; }
    if (s_zoom) { cairo_surface_destroy(s_zoom); s_zoom = NULL; }
}
