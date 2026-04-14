// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// apple.c — Apple logo button and Apple menu dropdown
//
// The Apple logo is the leftmost element of the menu bar. It's loaded
// from a PNG file and scaled to 22x15 pixels (measured from real Snow Leopard). Clicking it opens the
// Apple menu — a dropdown with system-level actions like Sleep, Restart,
// Shut Down, and Log Out.
//
// If the PNG files aren't found, we fall back to drawing a small filled
// circle as a placeholder. The actual Apple logo PNGs need to be placed
// at $HOME/.local/share/aqua-widgets/menubar/apple_logo.png (and the
// _selected variant).

#define _GNU_SOURCE  // For M_PI in math.h under strict C11

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <signal.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include "apple.h"
#include "render.h"

// ── Module state ────────────────────────────────────────────────────

// The Apple logo surfaces, scaled to 22x15 pixels (measured from real Snow Leopard).
// NULL if the PNG files couldn't be loaded.
static cairo_surface_t *logo_normal   = NULL;
static cairo_surface_t *logo_selected = NULL;

// The Apple menu dropdown popup window. None if not open.
static Window apple_popup = None;

// ── Apple menu item definitions ─────────────────────────────────────
// These mimic the classic macOS Apple menu. "---" is a separator.
// "(disabled)" suffix marks items that are shown in gray text.

static const char *apple_items[] = {
    "About AuraOS",
    "---",
    "System Preferences...",
    "---",
    "Force Quit...",
    "---",
    "Sleep",
    "Restart...",
    "Shut Down...",
    "---",
    "Log Out Kyle..."
};
static const int apple_item_count = 11;

// Which items are disabled (grayed out, non-clickable)?
// Index 0 = "About AuraOS" is disabled for now.
static bool is_disabled(int index)
{
    return (index == 0); // Only "About AuraOS" is disabled
}

// ── Internal: load a PNG, scale it, and extract an alpha mask ───────

// Loads a PNG file, scales it to the given width/height, and extracts
// just the alpha (opacity) channel as a grayscale mask surface.
//
// Why a mask? The Apple logo PNG is a colorful image (blue gradient),
// but macOS Snow Leopard renders the menu bar Apple logo as a solid
// dark silhouette. We use the logo's shape (alpha channel) as a stencil
// and paint it in solid black/white via cairo_mask_surface() in the
// paint function. This matches real Snow Leopard behavior.
//
// Returns an A8 (alpha-only) surface, or NULL if the file can't be loaded.
static cairo_surface_t *load_and_scale_png(const char *path, int target_w, int target_h)
{
    // Load the original PNG
    cairo_surface_t *original = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(original) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(original);
        return NULL;
    }

    // Get the original dimensions so we can compute the scale factor
    int orig_w = cairo_image_surface_get_width(original);
    int orig_h = cairo_image_surface_get_height(original);

    // Step 1: Scale the original PNG to the target size (keeps color+alpha)
    cairo_surface_t *scaled = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, target_w, target_h
    );
    cairo_t *cr = cairo_create(scaled);

    // Scale the drawing context so the original image maps to the target size
    double sx = (double)target_w / (double)orig_w;
    double sy = (double)target_h / (double)orig_h;
    cairo_scale(cr, sx, sy);

    // Use BEST filter for smooth downscaling (76x88 -> 22x15 is aggressive)
    cairo_pattern_t *pattern;
    cairo_set_source_surface(cr, original, 0, 0);
    pattern = cairo_get_source(cr);
    cairo_pattern_set_filter(pattern, CAIRO_FILTER_BEST);
    cairo_paint(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(original);

    // Step 2: Extract the alpha channel into an A8 mask surface.
    // A8 format stores only opacity — one byte per pixel, no color info.
    // We walk the scaled ARGB32 pixels and copy each pixel's alpha byte
    // into the A8 surface. This gives us the logo's silhouette shape.
    cairo_surface_flush(scaled);
    unsigned char *argb_data = cairo_image_surface_get_data(scaled);
    int argb_stride = cairo_image_surface_get_stride(scaled);

    cairo_surface_t *mask = cairo_image_surface_create(
        CAIRO_FORMAT_A8, target_w, target_h
    );
    cairo_surface_flush(mask);
    unsigned char *a8_data = cairo_image_surface_get_data(mask);
    int a8_stride = cairo_image_surface_get_stride(mask);

    for (int y = 0; y < target_h; y++) {
        for (int x = 0; x < target_w; x++) {
            // ARGB32 on little-endian: bytes are [B, G, R, A] per pixel
            unsigned char alpha = argb_data[y * argb_stride + x * 4 + 3];
            a8_data[y * a8_stride + x] = alpha;
        }
    }

    // Tell Cairo we modified the pixel data directly
    cairo_surface_mark_dirty(mask);
    cairo_surface_destroy(scaled);

    fprintf(stderr, "[apple] Loaded logo mask from %s (%dx%d -> %dx%d)\n",
            path, orig_w, orig_h, target_w, target_h);

    return mask;
}

// ── Public API ──────────────────────────────────────────────────────

void apple_init(MenuBar *mb)
{
    (void)mb;

    // Build the paths to the Apple logo PNGs.
    // We use $HOME to avoid hardcoding a username.
    const char *home = getenv("HOME");
    if (!home) return;

    char path_normal[512];
    char path_selected[512];
    snprintf(path_normal, sizeof(path_normal),
             "%s/.local/share/aqua-widgets/menubar/apple_logo.png", home);
    snprintf(path_selected, sizeof(path_selected),
             "%s/.local/share/aqua-widgets/menubar/apple_logo_selected.png", home);

    // Load and scale both variants to 22x15 pixels (measured from real Snow Leopard:
    // the Apple logo spans x=14 to x=36, y=3 to y=18, giving 22x15 pixels)
    logo_normal   = load_and_scale_png(path_normal, 22, 15);
    logo_selected = load_and_scale_png(path_selected, 22, 15);

    if (!logo_normal) {
        fprintf(stderr, "aura-menubar: Apple logo not found at %s (using fallback)\n",
                path_normal);
    }
}

void apple_paint(MenuBar *mb, cairo_t *cr)
{
    // Choose which logo variant to draw based on current state.
    // If the Apple menu is open or the mouse is hovering, use the
    // selected variant (or fall back to the normal one).
    bool active = (mb->hover_index == 0 || mb->open_menu == 0);
    cairo_surface_t *logo = (active && logo_selected) ? logo_selected : logo_normal;

    // Position: x=14, vertically centered in the 22px bar
    // Measured from real Snow Leopard: logo starts at x=14, y=3
    double x = 14.0;
    double y = (MENUBAR_HEIGHT - 15) / 2.0; // Center 15px icon in 22px bar (≈3.5, close to measured y=3)

    if (logo) {
        // Draw the logo as a solid-color silhouette using the alpha mask.
        // cairo_mask_surface() uses the mask's alpha values to control
        // where the current source color is painted. Where the mask is
        // opaque, the color shows through; where transparent, nothing
        // is drawn. This gives us a crisp black (or white) Apple logo
        // matching real Snow Leopard behavior.
        if (active) {
            // Selected state: white logo on the blue highlight
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        } else {
            // Normal state: pure black logo on the gray gradient bar
            // Use pure black for maximum contrast — real SL Apple logo
            // is a crisp, solid dark shape
            cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        }
        cairo_mask_surface(cr, logo, x, y);
    } else {
        // Fallback: draw a simple filled circle as a placeholder.
        // This is a dark gray circle roughly matching the Apple logo position.
        cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
        cairo_arc(cr, x + 11, MENUBAR_HEIGHT / 2.0, 7.0, 0, 2 * M_PI);
        cairo_fill(cr);
    }
}

// ── Dropdown helpers ────────────────────────────────────────────────

// Helper: draw a rounded rectangle path (for the dropdown background).
static void apple_rounded_rect(cairo_t *cr, double x, double y,
                               double w, double h, double radius)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - radius, y + radius,     radius, -M_PI / 2, 0);
    cairo_arc(cr, x + w - radius, y + h - radius, radius, 0,          M_PI / 2);
    cairo_arc(cr, x + radius,     y + h - radius, radius, M_PI / 2,   M_PI);
    cairo_arc(cr, x + radius,     y + radius,     radius, M_PI,       3 * M_PI / 2);
    cairo_close_path(cr);
}

// Paint the Apple menu dropdown contents.
static void paint_apple_dropdown(MenuBar *mb, int popup_w, int popup_h)
{
    cairo_surface_t *surface = cairo_xlib_surface_create(
        mb->dpy, apple_popup,
        DefaultVisual(mb->dpy, mb->screen),
        popup_w, popup_h
    );
    cairo_t *cr = cairo_create(surface);

    // Background: slightly transparent white
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 245.0 / 255.0);
    apple_rounded_rect(cr, 0, 0, popup_w, popup_h, 5.0);
    cairo_fill(cr);

    // Border: subtle dark outline
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 60.0 / 255.0);
    cairo_set_line_width(cr, 1.0);
    apple_rounded_rect(cr, 0.5, 0.5, popup_w - 1, popup_h - 1, 5.0);
    cairo_stroke(cr);

    // Draw each menu item
    int y = 4; // Top padding

    for (int i = 0; i < apple_item_count; i++) {
        if (strcmp(apple_items[i], "---") == 0) {
            // Separator line
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 40.0 / 255.0);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, 10, y + 3.5);
            cairo_line_to(cr, popup_w - 10, y + 3.5);
            cairo_stroke(cr);
            y += 7;
        } else {
            // Menu item text
            PangoLayout *layout = pango_cairo_create_layout(cr);
            pango_layout_set_text(layout, apple_items[i], -1);

            PangoFontDescription *desc = pango_font_description_from_string(
                "Lucida Grande 13"
            );
            pango_layout_set_font_description(layout, desc);
            pango_font_description_free(desc);

            // Disabled items are drawn in gray; active items in dark text
            if (is_disabled(i)) {
                cairo_set_source_rgb(cr, 0.6, 0.6, 0.6); // Gray for disabled
            } else {
                cairo_set_source_rgb(cr, 0.1, 0.1, 0.1); // Dark for active
            }

            cairo_move_to(cr, 18, y + 2);
            pango_cairo_show_layout(cr, layout);
            g_object_unref(layout);

            y += 22;
        }
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    XFlush(mb->dpy);
}

void apple_show_menu(MenuBar *mb)
{
    // Dismiss any existing Apple menu first
    apple_dismiss(mb);

    // ── Calculate popup size ────────────────────────────────────
    int popup_w = 220; // Fixed width matching Snow Leopard Apple menu

    // Height: 22px per item, 7px per separator, plus padding
    int popup_h = 8;
    for (int i = 0; i < apple_item_count; i++) {
        popup_h += (strcmp(apple_items[i], "---") == 0) ? 7 : 22;
    }

    // ── Create the popup window ─────────────────────────────────
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.event_mask = ExposureMask | ButtonPressMask
                     | PointerMotionMask | LeaveWindowMask;
    attrs.background_pixel = WhitePixel(mb->dpy, mb->screen);

    apple_popup = XCreateWindow(
        mb->dpy, mb->root,
        0, MENUBAR_HEIGHT,            // Directly below the Apple logo
        (unsigned int)popup_w,
        (unsigned int)popup_h,
        0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWEventMask | CWBackPixel,
        &attrs
    );

    XMapRaised(mb->dpy, apple_popup);

    // Paint the dropdown contents
    paint_apple_dropdown(mb, popup_w, popup_h);
}

void apple_dismiss(MenuBar *mb)
{
    if (apple_popup != None) {
        XDestroyWindow(mb->dpy, apple_popup);
        apple_popup = None;
        XFlush(mb->dpy);
    }
}

void apple_cleanup(void)
{
    // Free the loaded PNG surfaces
    if (logo_normal) {
        cairo_surface_destroy(logo_normal);
        logo_normal = NULL;
    }
    if (logo_selected) {
        cairo_surface_destroy(logo_selected);
        logo_selected = NULL;
    }

    apple_popup = None;
}
