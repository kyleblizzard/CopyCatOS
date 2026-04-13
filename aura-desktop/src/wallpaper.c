// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// wallpaper.c — Wallpaper loading and rendering
//
// Loads a wallpaper image from disk and scales it to fill the screen
// using aspect-fill (the image is scaled up until it covers the entire
// screen, then center-cropped to remove overflow).
//
// Supports both JPEG and PNG:
//   - PNG: loaded natively by Cairo (cairo_image_surface_create_from_png)
//   - JPEG: loaded with libjpeg, then pixel data is converted from
//     libjpeg's RGB format (3 bytes: R, G, B) to Cairo's RGB24 format
//     (4 bytes: 0xXXRRGGBB in native byte order, alpha byte ignored)
//
// If no wallpaper image is found, we fall back to a solid color
// matching Snow Leopard's default blue (#3A6EA5).

#include "wallpaper.h"

#include <cairo/cairo.h>
#include <jpeglib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>

// ── Module state ────────────────────────────────────────────────────

// The cached wallpaper surface, already scaled to screen size.
// Created once during init and reused for every repaint.
static cairo_surface_t *wallpaper_surface = NULL;

// ── JPEG loading ────────────────────────────────────────────────────

// Custom error handler for libjpeg.
// libjpeg uses setjmp/longjmp for error handling (because it's a C89
// library). When an error occurs, it jumps back to where we set up
// the error handler instead of calling exit().
struct jpeg_error_context {
    struct jpeg_error_mgr pub;   // Standard libjpeg error manager
    jmp_buf escape;              // Where to jump on error
};

static void jpeg_error_exit(j_common_ptr cinfo)
{
    // Jump back to the caller that set up the error handler
    struct jpeg_error_context *ctx = (struct jpeg_error_context *)cinfo->err;
    longjmp(ctx->escape, 1);
}

// Load a JPEG file and return it as a Cairo surface.
// Returns NULL on failure (file not found, corrupt, etc).
static cairo_surface_t *load_jpeg(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    // Set up the JPEG decompressor with our custom error handler
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_context jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;

    // If an error occurs during decompression, we'll jump here
    if (setjmp(jerr.escape)) {
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return NULL;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);          // Read from our file
    jpeg_read_header(&cinfo, TRUE);      // Parse the JPEG header
    cinfo.out_color_space = JCS_RGB;     // We want RGB output
    jpeg_start_decompress(&cinfo);       // Begin decompression

    int w = cinfo.output_width;
    int h = cinfo.output_height;
    int row_stride = w * 3;  // 3 bytes per pixel (RGB)

    // Allocate a buffer for one row of JPEG pixels
    unsigned char *row_buf = malloc(row_stride);
    if (!row_buf) {
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return NULL;
    }

    // Create a Cairo surface to hold the image.
    // CAIRO_FORMAT_RGB24 uses 4 bytes per pixel: 0xXXRRGGBB
    // (the high byte is ignored, not alpha).
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_RGB24, w, h);

    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        free(row_buf);
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return NULL;
    }

    // Get direct access to the Cairo surface's pixel buffer
    cairo_surface_flush(surface);
    unsigned char *data = cairo_image_surface_get_data(surface);
    int cairo_stride = cairo_image_surface_get_stride(surface);

    // Read the JPEG one row at a time and convert pixels.
    // libjpeg gives us RGB (3 bytes), but Cairo wants BGRX (4 bytes):
    //   Cairo RGB24 pixel = (R << 16) | (G << 8) | B
    // in native byte order (little-endian on x86).
    while (cinfo.output_scanline < cinfo.output_height) {
        int y = cinfo.output_scanline;
        JSAMPROW row_ptr = row_buf;
        jpeg_read_scanlines(&cinfo, &row_ptr, 1);

        // Pointer to this row in the Cairo buffer
        uint32_t *dest = (uint32_t *)(data + y * cairo_stride);

        for (int x = 0; x < w; x++) {
            unsigned char r = row_buf[x * 3 + 0];
            unsigned char g = row_buf[x * 3 + 1];
            unsigned char b = row_buf[x * 3 + 2];

            // Pack into Cairo's 0xXXRRGGBB format
            dest[x] = (r << 16) | (g << 8) | b;
        }
    }

    // Tell Cairo we're done writing pixels directly
    cairo_surface_mark_dirty(surface);

    // Clean up libjpeg resources
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    free(row_buf);
    fclose(fp);

    fprintf(stderr, "[wallpaper] Loaded JPEG: %s (%dx%d)\n", path, w, h);
    return surface;
}

// ── PNG loading ─────────────────────────────────────────────────────

// Load a PNG file using Cairo's built-in PNG loader.
// Much simpler than JPEG since Cairo handles the format natively.
static cairo_surface_t *load_png(const char *path)
{
    cairo_surface_t *surface = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return NULL;
    }
    int w = cairo_image_surface_get_width(surface);
    int h = cairo_image_surface_get_height(surface);
    fprintf(stderr, "[wallpaper] Loaded PNG: %s (%dx%d)\n", path, w, h);
    return surface;
}

// ── Image loading dispatcher ────────────────────────────────────────

// Try to load an image file, detecting format by extension.
static cairo_surface_t *load_image(const char *path)
{
    if (!path) return NULL;

    // Check file extension to decide which loader to use
    const char *ext = strrchr(path, '.');
    if (!ext) return NULL;

    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) {
        return load_jpeg(path);
    } else if (strcasecmp(ext, ".png") == 0) {
        return load_png(path);
    }

    fprintf(stderr, "[wallpaper] Unsupported format: %s\n", ext);
    return NULL;
}

// ── Solid color fallback ────────────────────────────────────────────

// Create a solid-color surface as a last resort.
// #3A6EA5 is close to the default Snow Leopard blue gradient.
static cairo_surface_t *create_solid_fallback(int w, int h)
{
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_RGB24, w, h);
    cairo_t *cr = cairo_create(surface);

    // Snow Leopard blue: #3A6EA5 = RGB(58, 110, 165)
    cairo_set_source_rgb(cr, 58.0 / 255.0, 110.0 / 255.0, 165.0 / 255.0);
    cairo_paint(cr);

    cairo_destroy(cr);
    fprintf(stderr, "[wallpaper] Using solid fallback color #3A6EA5\n");
    return surface;
}

// ── Aspect-fill scaling ─────────────────────────────────────────────

// Scale the loaded image to fill the screen using "aspect-fill":
//   1. Calculate scale factor so both dimensions >= screen size
//   2. Center the scaled image (any overflow is cropped evenly)
//
// This ensures the wallpaper always covers the entire screen without
// any letterboxing, at the cost of potentially cropping the edges.
static cairo_surface_t *scale_to_fill(cairo_surface_t *src, int dst_w, int dst_h)
{
    int src_w = cairo_image_surface_get_width(src);
    int src_h = cairo_image_surface_get_height(src);

    // Calculate the scale factor.
    // We need the image to be at least as big as the screen in both
    // dimensions, so we take the LARGER of the two ratios.
    double scale_x = (double)dst_w / src_w;
    double scale_y = (double)dst_h / src_h;
    double scale = (scale_x > scale_y) ? scale_x : scale_y;

    // Calculate the offset to center the scaled image.
    // If the scaled image is wider than the screen, we shift it left
    // by half the overflow (and similarly for height).
    double offset_x = (dst_w - src_w * scale) / 2.0;
    double offset_y = (dst_h - src_h * scale) / 2.0;

    // Create the destination surface at screen size
    cairo_surface_t *dst = cairo_image_surface_create(
        CAIRO_FORMAT_RGB24, dst_w, dst_h);
    cairo_t *cr = cairo_create(dst);

    // Apply the scale and offset, then paint the source image
    cairo_translate(cr, offset_x, offset_y);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, src, 0, 0);

    // Use BILINEAR filtering for smooth scaling (avoids pixelation)
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);

    cairo_destroy(cr);
    return dst;
}

// ── Public API ──────────────────────────────────────────────────────

bool wallpaper_init(const char *path, int screen_w, int screen_h)
{
    cairo_surface_t *raw = NULL;

    // Try loading from the paths in priority order:
    // 1. Explicit path from --wallpaper argument
    if (path) {
        raw = load_image(path);
    }

    // 2. Default wallpaper location
    if (!raw) {
        const char *home = getenv("HOME");
        if (home) {
            char buf[1024];
            snprintf(buf, sizeof(buf),
                     "%s/.local/share/aqua-widgets/wallpaper/default.jpg", home);
            raw = load_image(buf);

            // 3. Aurora 4K fallback
            if (!raw) {
                snprintf(buf, sizeof(buf),
                         "%s/.local/share/aqua-widgets/wallpaper/aurora-4k.jpg", home);
                raw = load_image(buf);
            }

            // Also try PNG versions in case the install script converted them
            if (!raw) {
                snprintf(buf, sizeof(buf),
                         "%s/.local/share/aqua-widgets/wallpaper/default.png", home);
                raw = load_image(buf);
            }
            if (!raw) {
                snprintf(buf, sizeof(buf),
                         "%s/.local/share/aqua-widgets/wallpaper/aurora-4k.png", home);
                raw = load_image(buf);
            }
        }
    }

    // 4. Solid color fallback if nothing else worked
    if (!raw) {
        wallpaper_surface = create_solid_fallback(screen_w, screen_h);
        return true;
    }

    // Scale the loaded image to fill the screen
    wallpaper_surface = scale_to_fill(raw, screen_w, screen_h);

    // Free the original (unscaled) surface — we only keep the scaled copy
    cairo_surface_destroy(raw);

    return (wallpaper_surface != NULL);
}

void wallpaper_paint(cairo_t *cr, int win_w, int win_h)
{
    (void)win_w;
    (void)win_h;

    if (!wallpaper_surface) return;

    // Paint the wallpaper surface at (0, 0).
    // Since it's already scaled to screen size, no transform is needed.
    cairo_set_source_surface(cr, wallpaper_surface, 0, 0);
    cairo_paint(cr);
}

cairo_surface_t *wallpaper_get_surface(void)
{
    return wallpaper_surface;
}

void wallpaper_shutdown(void)
{
    if (wallpaper_surface) {
        cairo_surface_destroy(wallpaper_surface);
        wallpaper_surface = NULL;
    }
}
