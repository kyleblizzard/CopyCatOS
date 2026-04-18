// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.
//
// CopyCatOS Window Manager — XComposite compositor for Snow Leopard shadows

// M_PI requires _GNU_SOURCE (provided by meson via -D_GNU_SOURCE).
//
// This file implements the compositing layer that gives CopyCatOS its realistic
// drop shadows. The approach is:
//
//   1. Use XComposite to redirect all top-level windows to off-screen pixmaps.
//      This lets us composite them with effects (shadows) onto the root window.
//
//   2. Create frame windows with 32-bit ARGB visuals. A normal X11 window has
//      no alpha channel — every pixel is fully opaque. ARGB visuals add an
//      8-bit alpha channel so we can have semi-transparent pixels for shadows.
//
//   3. Make each frame window bigger than the actual chrome by SHADOW_* pixels
//      on each side. The shadow is painted into this extra padding area.
//
//   4. Render shadows using a Gaussian blur approximation: three passes of box
//      blur. A single box blur averages every pixel with its neighbors in a
//      square region. Repeating this three times closely approximates the smooth
//      bell-curve falloff of a true Gaussian blur, but is much faster to compute.
//
//   5. Use XFixes input shapes so that clicks in the shadow region pass through
//      to the window below. The shadow is purely visual.

#include "compositor.h"
#include "decor.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

// ── Module-level state ──
// These are stored at file scope rather than in CCWM to avoid modifying
// the existing struct. Only compositor.c touches them.

// The 32-bit ARGB visual for frame windows (NULL until compositor_init)
static Visual *argb_visual = NULL;

// A Colormap associated with the ARGB visual (required by X11 — each
// visual needs its own colormap to map pixel values to colors)
static Colormap argb_colormap = 0;

// XDamage event base — XDamage events are numbered starting from this value.
// We need it to identify DamageNotify events in the event loop.
static int damage_event_base = 0;
static int damage_error_base = 0;

// Whether the compositor was successfully initialized.
// This is the global declared as 'extern bool compositor_active' in wm.h
// and defined in decor.c. We use it here, not a local static.

// ── Cached shadow surfaces ──
// Pre-computed shadow images for active and inactive windows. We store one
// shadow per blur radius because active and inactive shadows use different
// radii. These are regenerated whenever a window is resized, but the blur
// kernel approach is reused.
//
// For simplicity, we don't cache per-window — we regenerate on each paint.
// This is fast enough because the box blur is O(width * height * 3 passes)
// and frame decorations don't repaint every frame, only on expose/damage.

// ────────────────────────────────────────────────────────────────────────
// SECTION: Box blur implementation
// ────────────────────────────────────────────────────────────────────────
//
// A "box blur" replaces each pixel with the average of all pixels in a
// rectangular neighborhood. If the box has radius R, then we average over
// a (2R+1) x (2R+1) region centered on the pixel.
//
// For efficiency, we split each pass into:
//   1. Horizontal pass: blur each row independently
//   2. Vertical pass: blur each column independently
//
// This turns O(width * height * (2R+1)^2) into O(width * height * 2 * (2R+1)),
// which is dramatically faster for large radii.
//
// Three passes of box blur closely approximate a Gaussian blur (by the
// Central Limit Theorem — averaging uniform distributions converges to
// a normal/Gaussian distribution).

// Perform a single horizontal box blur pass on an alpha-only image buffer.
// `src` is the input buffer, `dst` is the output buffer, both are
// width*height bytes. Each byte is an alpha value (0–255).
//
// For each pixel, we sum the alpha values of `2*radius+1` neighbors in the
// same row, then divide by the count to get the average.
static void box_blur_horizontal(const unsigned char *src, unsigned char *dst,
                                int width, int height, int radius)
{
    // Process each row independently
    for (int y = 0; y < height; y++) {
        // `row` points to the start of this row in the source buffer
        const unsigned char *row = src + y * width;
        unsigned char *out_row = dst + y * width;

        // Running sum: we maintain a sum of the current window of pixels
        // and slide it across the row. This avoids re-summing the entire
        // neighborhood for every pixel — we just add the new right edge
        // and subtract the old left edge.
        int sum = 0;
        int count = 0;

        // Initialize the sum with the first (radius+1) pixels.
        // We start with only the right half of the window because the
        // left side extends beyond the image edge at position 0.
        for (int x = 0; x <= radius && x < width; x++) {
            sum += row[x];
            count++;
        }

        // Slide the window across the row
        for (int x = 0; x < width; x++) {
            // Store the averaged value for this pixel
            out_row[x] = (unsigned char)(sum / count);

            // Expand the window to the right (add the next pixel entering
            // the window from the right side)
            int right = x + radius + 1;
            if (right < width) {
                sum += row[right];
                count++;
            }

            // Shrink the window from the left (remove the pixel leaving
            // the window from the left side)
            int left = x - radius;
            if (left >= 0) {
                sum -= row[left];
                count--;
            }
        }
    }
}

// Perform a single vertical box blur pass. Same idea as horizontal,
// but we slide down columns instead of across rows.
static void box_blur_vertical(const unsigned char *src, unsigned char *dst,
                              int width, int height, int radius)
{
    // Process each column independently
    for (int x = 0; x < width; x++) {
        int sum = 0;
        int count = 0;

        // Initialize with the first (radius+1) pixels in this column
        for (int y = 0; y <= radius && y < height; y++) {
            sum += src[y * width + x];
            count++;
        }

        // Slide the window down the column
        for (int y = 0; y < height; y++) {
            dst[y * width + x] = (unsigned char)(sum / count);

            // Add pixel entering from below
            int bottom = y + radius + 1;
            if (bottom < height) {
                sum += src[bottom * width + x];
                count++;
            }

            // Remove pixel leaving from above
            int top = y - radius;
            if (top >= 0) {
                sum -= src[top * width + x];
                count--;
            }
        }
    }
}

// Apply a 3-pass box blur to an alpha buffer, approximating a Gaussian blur.
// `buf` is the input/output buffer (width*height bytes, alpha only).
// `blur_radius` is the desired Gaussian blur radius — we divide by 3 to
// get the per-pass box blur radius (since 3 passes of box blur ≈ 1 Gaussian).
//
// We need a temporary buffer for the intermediate results. This function
// allocates it internally and frees it before returning.
static void gaussian_blur_approx(unsigned char *buf, int width, int height,
                                 int blur_radius)
{
    if (blur_radius <= 0 || width <= 0 || height <= 0) return;

    // Each pass uses a smaller radius: ceil(blur_radius / 3)
    // Three passes of this smaller box blur approximate the full Gaussian.
    int pass_radius = (blur_radius + 2) / 3;  // ceil(blur_radius / 3)

    // Allocate temporary buffer for intermediate results
    size_t buf_size = (size_t)width * height;
    unsigned char *tmp = (unsigned char *)calloc(buf_size, 1);
    if (!tmp) return;  // Out of memory — shadow will be missing, not a crash

    // Three passes, each consisting of horizontal + vertical blur
    for (int pass = 0; pass < 3; pass++) {
        // Horizontal blur: buf -> tmp
        box_blur_horizontal(buf, tmp, width, height, pass_radius);
        // Vertical blur: tmp -> buf
        box_blur_vertical(tmp, buf, width, height, pass_radius);
    }

    free(tmp);
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Shadow rendering
// ────────────────────────────────────────────────────────────────────────

// Create a Cairo image surface containing the blurred shadow for a window
// of the given chrome dimensions (not including shadow padding).
//
// chrome_w, chrome_h: size of the actual window chrome (title bar + borders)
// blur_radius: blur spread in pixels (20 for active, 15 for inactive)
// alpha: peak opacity of the shadow (0.4 active, 0.25 inactive)
//
// Returns a new cairo_surface_t in ARGB32 format. The surface dimensions
// are (chrome_w + SHADOW_LEFT + SHADOW_RIGHT) x (chrome_h + SHADOW_TOP + SHADOW_BOTTOM).
// The caller must destroy it with cairo_surface_destroy().
static cairo_surface_t *render_shadow(int chrome_w, int chrome_h,
                                      int blur_radius, double alpha)
{
    // Total size of the shadow image includes the chrome area plus
    // padding on all sides for the blur to spread into
    int total_w = chrome_w + SHADOW_LEFT + SHADOW_RIGHT;
    int total_h = chrome_h + SHADOW_TOP + SHADOW_BOTTOM;

    if (total_w <= 0 || total_h <= 0) return NULL;

    // Step 1: Create an alpha-only buffer.
    // We start with a solid rectangle (the window shape) drawn into the
    // buffer, then blur it. The blurred result becomes our shadow.
    size_t buf_size = (size_t)total_w * total_h;
    unsigned char *alpha_buf = (unsigned char *)calloc(buf_size, 1);
    if (!alpha_buf) return NULL;

    // Fill a rectangle where the window chrome sits.
    // The chrome is centered horizontally (offset by SHADOW_LEFT) and
    // positioned with SHADOW_TOP offset from the top.
    // We also apply the Y-offset here: the shadow source is shifted DOWN
    // by SHADOW_Y_OFFSET, which makes the shadow appear below the window.
    for (int y = SHADOW_TOP + SHADOW_Y_OFFSET;
         y < SHADOW_TOP + SHADOW_Y_OFFSET + chrome_h && y < total_h; y++) {
        for (int x = SHADOW_LEFT; x < SHADOW_LEFT + chrome_w && x < total_w; x++) {
            // 255 = fully opaque in the source shape.
            // After blurring, values will spread out and fade.
            alpha_buf[y * total_w + x] = 255;
        }
    }

    // Step 2: Blur the rectangle to create the shadow falloff.
    // This spreads the sharp rectangle edges into a smooth gradient.
    gaussian_blur_approx(alpha_buf, total_w, total_h, blur_radius);

    // Step 3: Convert the alpha buffer into a Cairo ARGB32 surface.
    // Each pixel is black (R=G=B=0) with alpha from our blurred buffer,
    // scaled by the desired peak opacity.
    cairo_surface_t *shadow_surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, total_w, total_h);

    if (cairo_surface_status(shadow_surface) != CAIRO_STATUS_SUCCESS) {
        free(alpha_buf);
        cairo_surface_destroy(shadow_surface);
        return NULL;
    }

    // Lock the surface data for direct pixel manipulation.
    // Cairo stores ARGB32 as 4 bytes per pixel: [B, G, R, A] on little-endian,
    // but using premultiplied alpha. For black (R=G=B=0), premultiplied values
    // are also 0 for RGB, so we only need to set the alpha byte.
    cairo_surface_flush(shadow_surface);
    unsigned char *pixels = cairo_image_surface_get_data(shadow_surface);
    int stride = cairo_image_surface_get_stride(shadow_surface);

    for (int y = 0; y < total_h; y++) {
        // Each row in the Cairo surface may have padding (stride > width*4)
        unsigned char *row = pixels + y * stride;
        for (int x = 0; x < total_w; x++) {
            // Scale the blurred alpha by our desired peak opacity.
            // The blur already created a falloff from 255 to 0; multiplying
            // by `alpha` dims the whole thing to the desired intensity.
            unsigned char a = (unsigned char)(alpha_buf[y * total_w + x] * alpha);

            // ARGB32 in native byte order (Cairo uses premultiplied alpha):
            //   pixel = (A << 24) | (R << 16) | (G << 8) | B
            // For pure black shadow: R=G=B=0, so pixel = (A << 24)
            uint32_t *pixel = (uint32_t *)(row + x * 4);
            *pixel = ((uint32_t)a << 24);
        }
    }

    // Tell Cairo we modified the pixel data directly
    cairo_surface_mark_dirty(shadow_surface);

    free(alpha_buf);
    return shadow_surface;
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Public API
// ────────────────────────────────────────────────────────────────────────

bool compositor_init(CCWM *wm)
{
    if (!wm || !wm->dpy) return false;

    fprintf(stderr, "[cc-wm] Initializing compositor...\n");

    // ── Check for required X extensions ──
    // XComposite: lets us redirect window contents to off-screen pixmaps
    //   so we can composite them with effects (shadows, transparency).
    int composite_major = 0, composite_minor = 0;
    if (!XCompositeQueryVersion(wm->dpy, &composite_major, &composite_minor)) {
        fprintf(stderr, "[cc-wm] ERROR: XComposite extension not available\n");
        return false;
    }
    fprintf(stderr, "[cc-wm] XComposite %d.%d found\n",
            composite_major, composite_minor);

    // We need at least XComposite 0.2 for NameWindowPixmap, which lets us
    // grab the off-screen pixmap of a redirected window.
    if (composite_major == 0 && composite_minor < 2) {
        fprintf(stderr, "[cc-wm] ERROR: Need XComposite >= 0.2, got %d.%d\n",
                composite_major, composite_minor);
        return false;
    }

    // XDamage: notifies us when a window's contents change so we know
    //   which areas need repainting. Without this we'd have to repaint
    //   everything every frame.
    int damage_major = 0, damage_minor = 0;
    if (!XDamageQueryVersion(wm->dpy, &damage_major, &damage_minor)) {
        fprintf(stderr, "[cc-wm] ERROR: XDamage extension not available\n");
        return false;
    }
    fprintf(stderr, "[cc-wm] XDamage %d.%d found\n",
            damage_major, damage_minor);

    // Store the event base so we can identify DamageNotify events later.
    // X extensions add their events starting at an offset (the "event base").
    XDamageQueryExtension(wm->dpy, &damage_event_base, &damage_error_base);

    // XFixes: provides region objects and input shape manipulation.
    //   We use it to set input shapes on frame windows so clicks pass
    //   through the shadow region.
    int fixes_major = 0, fixes_minor = 0;
    if (!XFixesQueryVersion(wm->dpy, &fixes_major, &fixes_minor)) {
        fprintf(stderr, "[cc-wm] ERROR: XFixes extension not available\n");
        return false;
    }
    fprintf(stderr, "[cc-wm] XFixes %d.%d found\n", fixes_major, fixes_minor);

    // XRender: the X Rendering Extension provides Porter-Duff compositing
    //   operations. Cairo uses it under the hood for hardware-accelerated
    //   rendering on X11.
    int render_event = 0, render_error = 0;
    if (!XRenderQueryExtension(wm->dpy, &render_event, &render_error)) {
        fprintf(stderr, "[cc-wm] ERROR: XRender extension not available\n");
        return false;
    }
    fprintf(stderr, "[cc-wm] XRender found\n");

    // ── Redirect all subwindows of root ──
    // CompositeRedirectAutomatic means:
    //   - The X server handles compositing windows onto the screen
    //   - We can still use ARGB visuals for our frame windows (shadows)
    //   - We do NOT need to manually composite every window every frame
    //
    // We originally used CompositeRedirectManual, but that requires the WM
    // to paint every window's pixmap onto the root — a full compositor.
    // Automatic mode gives us ARGB support (transparent shadow regions)
    // while letting X handle the actual window compositing.
    XCompositeRedirectSubwindows(wm->dpy, wm->root, CompositeRedirectAutomatic);
    fprintf(stderr, "[cc-wm] Composite redirect set (automatic mode)\n");

    // ── Find a 32-bit ARGB visual ──
    // A "visual" in X11 describes the pixel format of a window. Most windows
    // use a 24-bit visual (8 bits each for R, G, B). We need a 32-bit visual
    // that includes an 8-bit alpha channel, so our frame windows can have
    // semi-transparent pixels for the shadow effect.
    if (!compositor_create_argb_visual(wm, &argb_visual, &argb_colormap)) {
        fprintf(stderr, "[cc-wm] WARNING: No 32-bit ARGB visual found. "
                "Shadows will not be available.\n");
        // Don't fail entirely — the WM can still work without shadows
    } else {
        fprintf(stderr, "[cc-wm] Found 32-bit ARGB visual\n");
    }

    compositor_active = true;
    fprintf(stderr, "[cc-wm] Compositor initialized successfully\n");
    return true;
}

bool compositor_create_argb_visual(CCWM *wm, Visual **out_visual,
                                   Colormap *out_colormap)
{
    if (!wm || !wm->dpy || !out_visual || !out_colormap) return false;

    // Get a list of all visuals available on this screen.
    // We're looking for one that is:
    //   - 32 bits deep (8 each for R, G, B, A)
    //   - TrueColor class (direct color, not palette-based)
    //
    // "TrueColor" means each pixel directly encodes its RGB values using
    // bitmasks, as opposed to "PseudoColor" where pixels are indices into
    // a color palette. Modern displays are always TrueColor.
    XVisualInfo templ;
    templ.screen = wm->screen;
    templ.depth = 32;
    templ.class = TrueColor;

    int num_visuals = 0;
    XVisualInfo *visuals = XGetVisualInfo(wm->dpy,
                                          VisualScreenMask | VisualDepthMask |
                                          VisualClassMask,
                                          &templ, &num_visuals);

    if (!visuals || num_visuals == 0) {
        if (visuals) XFree(visuals);
        return false;
    }

    // Use the first matching visual. In practice, there's usually exactly
    // one 32-bit TrueColor visual on modern X servers.
    *out_visual = visuals[0].visual;

    // Create a colormap for this visual. Even though TrueColor visuals
    // don't use color lookup tables, X11 still requires a colormap to be
    // associated with every window. AllocNone means "don't allocate any
    // color cells" — the colormap is just a formality.
    *out_colormap = XCreateColormap(wm->dpy, wm->root,
                                    *out_visual, AllocNone);

    XFree(visuals);
    return true;
}

void compositor_paint_shadow(CCWM *wm, Client *c, cairo_t *cr)
{
    if (!wm || !c || !cr) return;

    // Shadow parameters tuned to match Snow Leopard's appearance.
    // Focused windows get a larger, darker shadow; unfocused windows
    // get a subtler one. The y_offset simulates light from above,
    // pushing the shadow downward — giving windows a "floating" feel.
    bool active = c->focused;
    int radius = active ? 22 : 16;
    double peak_alpha = active ? 0.45 : 0.22;
    int y_offset = 4;  // Light from above pushes shadow down

    // The chrome area within the frame (offset by shadow padding)
    int sl = SHADOW_LEFT;
    int st = SHADOW_TOP;
    int chrome_w = c->w + 2 * BORDER_WIDTH;
    int chrome_h = c->h + TITLEBAR_HEIGHT + BORDER_WIDTH;

    // Draw shadow as concentric rounded rectangles with decreasing alpha.
    // Each layer is 2px larger than the previous, creating a soft falloff.
    // Using the full radius count for layers gives a smoother gradient
    // than the previous radius/2 approach — more layers = finer steps.
    int layers = radius;

    for (int i = layers; i >= 1; i--) {
        double t = (double)i / layers;  // 1.0 = outermost, 0.0 = innermost
        // Cubic falloff: softer edges with a stronger center, closely
        // matching the diffuse shadow look of real Snow Leopard windows.
        double alpha = peak_alpha * (1.0 - t) * (1.0 - t) * (1.0 - t);
        int expand = i * 2;
        double corner_r = 3.0 + i * 0.5;

        double sx = sl - expand;
        double sy = st - expand + y_offset;
        double sw = chrome_w + expand * 2;
        double sh = chrome_h + expand * 2;

        // Skip if dimensions are negative (shouldn't happen, but be safe)
        if (sw <= 0 || sh <= 0) continue;

        cairo_set_source_rgba(cr, 0, 0, 0, alpha);

        // Draw a rounded rectangle using four arcs at the corners
        cairo_new_sub_path(cr);
        cairo_arc(cr, sx + sw - corner_r, sy + corner_r, corner_r, -M_PI/2, 0);
        cairo_arc(cr, sx + sw - corner_r, sy + sh - corner_r, corner_r, 0, M_PI/2);
        cairo_arc(cr, sx + corner_r, sy + sh - corner_r, corner_r, M_PI/2, M_PI);
        cairo_arc(cr, sx + corner_r, sy + corner_r, corner_r, M_PI, 3*M_PI/2);
        cairo_close_path(cr);
        cairo_fill(cr);
    }
}

void compositor_set_input_shape(CCWM *wm, Client *c)
{
    if (!wm || !c || !c->frame) return;

    // The frame window is larger than the visible chrome because of shadow
    // padding. We don't want the shadow area to intercept mouse clicks —
    // clicking on a shadow should go to whatever window is behind it.
    //
    // X11 "input shapes" define which parts of a window respond to mouse
    // events. By setting the input shape to a rectangle that covers only
    // the chrome area (excluding shadow padding), clicks in the shadow
    // region will pass through to the window below.

    // Chrome dimensions (same calculation as frame.c)
    int chrome_w = c->w + 2 * BORDER_WIDTH;
    int chrome_h = c->h + TITLEBAR_HEIGHT + BORDER_WIDTH;

    // Create a rectangular region covering just the chrome.
    // The chrome starts at (SHADOW_LEFT, SHADOW_TOP) within the frame window.
    XRectangle rect;
    rect.x = SHADOW_LEFT;
    rect.y = SHADOW_TOP;
    rect.width = chrome_w;
    rect.height = chrome_h;

    // XFixesCreateRegion creates an XserverRegion from an array of rectangles.
    // We use a single rectangle that covers the clickable chrome area.
    XserverRegion region = XFixesCreateRegion(wm->dpy, &rect, 1);

    // ShapeInput (as opposed to ShapeBounding or ShapeClip) specifically
    // controls which parts of the window receive input events.
    // ShapeSet replaces the current input shape entirely.
    XFixesSetWindowShapeRegion(wm->dpy, c->frame, ShapeInput, 0, 0, region);

    // Clean up the region object — X server has its own copy now
    XFixesDestroyRegion(wm->dpy, region);

    if (getenv("AURA_DEBUG")) {
        fprintf(stderr, "[cc-wm] Set input shape for '%s': "
                "clickable at (%d,%d) %dx%d within frame\n",
                c->title, rect.x, rect.y, rect.width, rect.height);
    }
}

void compositor_damage_notify(CCWM *wm, XEvent *e)
{
    if (!wm || !e) return;
    if (!compositor_active) return;

    // Check if this is actually a DamageNotify event.
    // XDamage events have type (damage_event_base + XDamageNotify).
    // XDamageNotify is 0, so the event type equals damage_event_base.
    if (e->type != damage_event_base + XDamageNotify) return;

    // Cast the generic XEvent to the DamageNotify-specific structure.
    // This gives us access to fields like `drawable` (the damaged window)
    // and `area` (the rectangle that changed).
    XDamageNotifyEvent *dev = (XDamageNotifyEvent *)e;

    // Acknowledge the damage. XDamageSubtract tells the X server:
    // "I've seen this damage, you can clear it now."
    // Passing None for the `repair` and `parts` regions means:
    // "Subtract all damage" (i.e., acknowledge everything).
    //
    // If we don't call this, the damage region accumulates and the server
    // keeps sending us the same damage notifications.
    XDamageSubtract(wm->dpy, dev->damage, None, None);

    // Find which client owns the damaged window and repaint its decorations.
    // The damage might be on the client window or the frame window.
    Window damaged = dev->drawable;
    Client *c = wm_find_client(wm, damaged);
    if (!c) {
        c = wm_find_client_by_frame(wm, damaged);
    }

    if (c && c->frame && c->mapped) {
        // Trigger a repaint of this client's frame decoration.
        // This will re-render the shadow and title bar.
        decor_paint(wm, c);

        if (getenv("AURA_DEBUG")) {
            fprintf(stderr, "[cc-wm] Damage repaint for '%s'\n", c->title);
        }
    }
}

void compositor_shutdown(CCWM *wm)
{
    if (!wm || !wm->dpy) return;

    if (compositor_active) {
        // Undo the XComposite redirection. This tells the X server to go
        // back to rendering windows directly to the screen instead of to
        // off-screen pixmaps. Important for clean shutdown — if we crash
        // without doing this, the screen might go blank until another
        // compositor takes over.
        XCompositeUnredirectSubwindows(wm->dpy, wm->root,
                                       CompositeRedirectAutomatic);
        fprintf(stderr, "[cc-wm] Compositor: unredirected subwindows\n");
    }

    // Free the colormap we created for the ARGB visual.
    // The visual itself is owned by the X server, not us, so we don't free it.
    if (argb_colormap) {
        XFreeColormap(wm->dpy, argb_colormap);
        argb_colormap = 0;
    }

    argb_visual = NULL;
    compositor_active = false;
    fprintf(stderr, "[cc-wm] Compositor shut down\n");
}
