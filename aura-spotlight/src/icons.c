// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ─── icons.c ───
// Icon resolution, loading, and caching.
//
// Desktop files specify an icon by *name* (e.g. "firefox") rather
// than by path.  This module searches the icon-theme hierarchy to
// find a matching file, loads it as a Cairo image surface, scales
// it to 32×32 pixels, and stores the result in a simple hash-map
// cache so the same icon is never loaded from disk twice.
//
// Search order (for a given icon_name):
//   1. Absolute path — load directly if icon_name starts with '/'.
//   2. ~/.local/share/icons/AquaKDE-icons/{size}/apps/{name}.png
//   3. /usr/share/icons/hicolor/{size}/apps/{name}.png
//   4. /usr/share/pixmaps/{name}.png
//   5. Also try .svg and .xpm extensions at each location.
//   6. Fallback: a programmatically-drawn generic app icon.

#define _GNU_SOURCE  // For M_PI in math.h under strict C11

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <math.h>
#include <png.h>
#include <cairo/cairo.h>

#include "icons.h"

// ──────────────────────────────────────────────
// Hash-map cache
// ──────────────────────────────────────────────

// Number of slots in the cache.  Must be a power of two for
// fast modulo (& mask) but we keep it simple with 256.
#define ICON_CACHE_SIZE 256

// One slot in the cache — stores the icon name as a key and
// the loaded Cairo surface as the value.
typedef struct {
    char             name[256];       // icon name key (empty = unused slot)
    cairo_surface_t *surface;         // 32×32 ARGB surface, or NULL
} IconCacheEntry;

// The cache itself — a fixed-size array with linear probing.
static IconCacheEntry icon_cache[ICON_CACHE_SIZE];

// ─── djb2 hash function ───
// A classic string hash by Daniel J. Bernstein.  It's simple,
// fast, and has decent distribution for short strings like
// icon names.
static unsigned int djb2_hash(const char *str) {
    unsigned int hash = 5381;
    int c;
    while ((c = (unsigned char)*str++)) {
        hash = ((hash << 5) + hash) + (unsigned int)c;  // hash * 33 + c
    }
    return hash;
}

// Look up an existing cache entry (or find an empty slot).
// Returns the slot index.
static int cache_find_slot(const char *name) {
    unsigned int h = djb2_hash(name) % ICON_CACHE_SIZE;

    for (int i = 0; i < ICON_CACHE_SIZE; i++) {
        unsigned int idx = (h + (unsigned int)i) % ICON_CACHE_SIZE;

        // Empty slot — the name hasn't been cached yet.
        if (icon_cache[idx].name[0] == '\0')
            return (int)idx;

        // Found a matching name — return this slot.
        if (strcmp(icon_cache[idx].name, name) == 0)
            return (int)idx;
    }

    // Cache is full — fall back to slot 0 (unlikely with 256 slots).
    return 0;
}

// ──────────────────────────────────────────────
// PNG loading via libpng
// ──────────────────────────────────────────────

// Load a PNG file and return it as a Cairo ARGB32 image surface.
// Returns NULL on any failure.
static cairo_surface_t *load_png_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    // Read the first 8 bytes and verify they match the PNG signature.
    unsigned char header[8];
    if (fread(header, 1, 8, fp) != 8 || png_sig_cmp(header, 0, 8)) {
        fclose(fp);
        return NULL;
    }

    // Create the libpng read structures.
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                             NULL, NULL, NULL);
    if (!png) { fclose(fp); return NULL; }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        fclose(fp);
        return NULL;
    }

    // libpng uses setjmp/longjmp for error handling.  If something
    // goes wrong inside any png_* call it jumps back here.
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return NULL;
    }

    png_init_io(png, fp);
    png_set_sig_bytes(png, 8); // we already consumed the 8-byte header

    png_read_info(png, info);

    int width      = (int)png_get_image_width(png, info);
    int height     = (int)png_get_image_height(png, info);
    int color_type = (int)png_get_color_type(png, info);
    int bit_depth  = (int)png_get_bit_depth(png, info);

    // Normalise the pixel format so we always get 8-bit RGBA.
    if (bit_depth == 16)
        png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    // Add an alpha channel if the image doesn't have one.
    if (color_type == PNG_COLOR_TYPE_RGB ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    // Convert grayscale to RGB.
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);

    png_read_update_info(png, info);

    // Allocate row pointers and read the image data.
    png_bytep *rows = malloc(sizeof(png_bytep) * (size_t)height);
    if (!rows) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return NULL;
    }
    for (int y = 0; y < height; y++) {
        rows[y] = malloc(png_get_rowbytes(png, info));
    }

    png_read_image(png, rows);
    fclose(fp);

    // Create a Cairo surface and copy the pixel data into it.
    // Cairo uses pre-multiplied BGRA, while libpng gave us RGBA,
    // so we need to swizzle and pre-multiply each pixel.
    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);

    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        for (int y = 0; y < height; y++) free(rows[y]);
        free(rows);
        png_destroy_read_struct(&png, &info, NULL);
        return NULL;
    }

    cairo_surface_flush(surface);
    unsigned char *data   = cairo_image_surface_get_data(surface);
    int            stride = cairo_image_surface_get_stride(surface);

    for (int y = 0; y < height; y++) {
        png_bytep     src = rows[y];
        unsigned char *dst = data + y * stride;

        for (int x = 0; x < width; x++) {
            // Source pixel: R G B A (each 8 bits).
            unsigned char r = src[x * 4 + 0];
            unsigned char g = src[x * 4 + 1];
            unsigned char b = src[x * 4 + 2];
            unsigned char a = src[x * 4 + 3];

            // Pre-multiply: Cairo expects colour channels scaled
            // by the alpha value.
            unsigned char pr = (unsigned char)((r * a + 127) / 255);
            unsigned char pg = (unsigned char)((g * a + 127) / 255);
            unsigned char pb = (unsigned char)((b * a + 127) / 255);

            // Store as BGRA (native byte order on little-endian,
            // which is what Cairo's ARGB32 format actually is).
            dst[x * 4 + 0] = pb;  // blue
            dst[x * 4 + 1] = pg;  // green
            dst[x * 4 + 2] = pr;  // red
            dst[x * 4 + 3] = a;   // alpha
        }
        free(rows[y]);
    }
    free(rows);

    cairo_surface_mark_dirty(surface);
    png_destroy_read_struct(&png, &info, NULL);

    return surface;
}

// ──────────────────────────────────────────────
// Icon scaling
// ──────────────────────────────────────────────

// Scale a Cairo image surface to the requested dimensions.
// Returns a new surface — the caller should destroy the original
// if it is no longer needed.
static cairo_surface_t *scale_surface(cairo_surface_t *src,
                                       int target_w, int target_h) {
    if (!src) return NULL;

    int src_w = cairo_image_surface_get_width(src);
    int src_h = cairo_image_surface_get_height(src);

    // Nothing to do if the source is already the right size.
    if (src_w == target_w && src_h == target_h) return src;

    cairo_surface_t *dst =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, target_w, target_h);

    cairo_t *cr = cairo_create(dst);

    // Scale the coordinate system so painting the source at
    // (0,0) fills the entire target surface.
    double sx = (double)target_w / (double)src_w;
    double sy = (double)target_h / (double)src_h;
    cairo_scale(cr, sx, sy);

    cairo_set_source_surface(cr, src, 0, 0);

    // Use bilinear filtering for decent quality when shrinking.
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);

    cairo_paint(cr);
    cairo_destroy(cr);

    // The original surface is no longer needed.
    cairo_surface_destroy(src);

    return dst;
}

// ──────────────────────────────────────────────
// Fallback icon
// ──────────────────────────────────────────────

// Draw a simple generic application icon — a gray rounded
// rectangle with a darker border — used when no real icon
// file can be found on disk.
static cairo_surface_t *create_fallback_icon(void) {
    int size = 32;
    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surface);

    double r = 6.0; // corner radius

    // Rounded rectangle path.
    cairo_new_sub_path(cr);
    cairo_arc(cr, size - r, r,        r, -M_PI / 2, 0);
    cairo_arc(cr, size - r, size - r, r, 0,          M_PI / 2);
    cairo_arc(cr, r,        size - r, r, M_PI / 2,   M_PI);
    cairo_arc(cr, r,        r,        r, M_PI,       3 * M_PI / 2);
    cairo_close_path(cr);

    // Fill with a neutral gray.
    cairo_set_source_rgba(cr, 0.75, 0.75, 0.75, 1.0);
    cairo_fill_preserve(cr);

    // Stroke with a slightly darker border.
    cairo_set_source_rgba(cr, 0.55, 0.55, 0.55, 1.0);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    // Draw a small "A" glyph in the centre to hint "app".
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.8);
    cairo_select_font_face(cr, "Sans",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 18);

    cairo_text_extents_t ext;
    cairo_text_extents(cr, "A", &ext);
    cairo_move_to(cr,
                  (size - ext.width) / 2.0 - ext.x_bearing,
                  (size + ext.height) / 2.0);
    cairo_show_text(cr, "A");

    cairo_destroy(cr);
    return surface;
}

// ──────────────────────────────────────────────
// File probing helpers
// ──────────────────────────────────────────────

// Try to load a PNG at the given path.  Returns a 32×32
// Cairo surface on success, NULL on failure.
static cairo_surface_t *try_load(const char *path) {
    if (access(path, R_OK) != 0)
        return NULL;

    cairo_surface_t *s = load_png_file(path);
    if (!s) return NULL;

    return scale_surface(s, 32, 32);
}

// Build a path from format + arguments, then try to load it.
// This tiny helper avoids repeating snprintf boilerplate.
static cairo_surface_t *try_path(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return try_load(buf);
}

// ──────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────

cairo_surface_t *icon_lookup(const char *icon_name) {
    // Handle NULL or empty name immediately.
    if (!icon_name || icon_name[0] == '\0')
        return create_fallback_icon();

    // Check the cache first.
    int slot = cache_find_slot(icon_name);
    if (icon_cache[slot].name[0] != '\0' &&
        strcmp(icon_cache[slot].name, icon_name) == 0) {
        // Cache hit — return the stored surface.
        return icon_cache[slot].surface;
    }

    cairo_surface_t *surface = NULL;

    // ── 1. Absolute path ──
    if (icon_name[0] == '/') {
        surface = try_load(icon_name);
        if (surface) goto done;
    }

    // Get the user's home directory for paths under ~/.local.
    const char *home = getenv("HOME");
    if (!home) home = "/root";

    // Preferred icon sizes — we try 48 first (closest to our
    // target of 32), then 32, then 64.
    static const int sizes[] = { 48, 32, 64 };
    static const int nsizes  = 3;

    // Extensions to try at each candidate path.
    static const char *exts[] = { "png", "svg", "xpm" };
    static const int nexts = 3;

    // ── 2. AquaKDE-icons theme ──
    for (int si = 0; si < nsizes && !surface; si++) {
        for (int ei = 0; ei < nexts && !surface; ei++) {
            surface = try_path(
                "%s/.local/share/icons/AquaKDE-icons/%dx%d/apps/%s.%s",
                home, sizes[si], sizes[si], icon_name, exts[ei]);
        }
    }

    // ── 3. hicolor theme ──
    for (int si = 0; si < nsizes && !surface; si++) {
        for (int ei = 0; ei < nexts && !surface; ei++) {
            surface = try_path(
                "/usr/share/icons/hicolor/%dx%d/apps/%s.%s",
                sizes[si], sizes[si], icon_name, exts[ei]);
        }
    }

    // ── 4. /usr/share/pixmaps ──
    for (int ei = 0; ei < nexts && !surface; ei++) {
        surface = try_path("/usr/share/pixmaps/%s.%s", icon_name, exts[ei]);
    }

    // ── 5. Fallback ──
    if (!surface) {
        surface = create_fallback_icon();
    }

done:
    // Store in cache for next time.
    strncpy(icon_cache[slot].name, icon_name, sizeof(icon_cache[slot].name) - 1);
    icon_cache[slot].name[sizeof(icon_cache[slot].name) - 1] = '\0';
    icon_cache[slot].surface = surface;

    return surface;
}

void icon_cache_cleanup(void) {
    for (int i = 0; i < ICON_CACHE_SIZE; i++) {
        if (icon_cache[i].surface) {
            cairo_surface_destroy(icon_cache[i].surface);
            icon_cache[i].surface = NULL;
        }
        icon_cache[i].name[0] = '\0';
    }
}
