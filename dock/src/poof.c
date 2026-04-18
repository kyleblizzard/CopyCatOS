// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// poof.c — "Poof" cloud removal animation
//
// This module plays the classic macOS "poof" smoke effect when a user drags
// an icon out of the dock. The effect is a 5-frame sprite animation loaded
// from a single PNG sprite sheet (128x640, with five 128x128 frames stacked
// vertically). The animation plays in a small override-redirect window at
// the mouse release point and lasts about 150ms total (30ms per frame).
//
// Architecture:
//   - Module-static state (same pattern as menu.c and tooltip.c) — there is
//     only ever one poof animation at a time, so we use a single static struct.
//   - The sprite sheet is loaded once at startup and kept in memory.
//   - Each time poof_start() is called, we create a temporary transparent
//     window, play the 5 frames, then destroy the window.
//
// Integration (in dock.c):
//   In dock_init(): call poof_load(state);
//   In dock_run() timer section:
//     if (poof_is_active()) {
//         poof_update(state);
//         // Keep select() timeout short during animation
//     }
//   In dock_cleanup(): call poof_cleanup(state);
//
// The poof_start() function is called from dnd.c when a drag completes
// outside the dock area.
// ============================================================================

#define _GNU_SOURCE  // Needed for clock_gettime and other POSIX extensions

#include "poof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

// ---------------------------------------------------------------------------
// Animation constants
// ---------------------------------------------------------------------------

// The sprite sheet has 5 frames of animation, each 128x128 pixels,
// stacked vertically in a single 128x640 PNG image.
#define POOF_FRAME_COUNT  5
#define POOF_FRAME_SIZE   128    // Each frame is 128x128 pixels

// 30 milliseconds between frames. With 5 frames, the full animation
// takes 5 * 30 = 150ms — quick enough to feel snappy, slow enough to see.
#define POOF_FRAME_MS     30

// ---------------------------------------------------------------------------
// Module-static state — only one poof animation can play at a time
//
// This follows the same pattern as menu.c: a single static struct holds
// all the state for the poof effect. We don't need to allocate anything
// on the heap or pass state pointers around between functions.
// ---------------------------------------------------------------------------
static struct {
    cairo_surface_t *sheet;      // The 128x640 sprite sheet (loaded once, kept forever)
    Window win;                  // Override-redirect animation window (created per-animation)
    cairo_surface_t *surface;    // Cairo surface bound to the animation window
    cairo_t *cr;                 // Cairo drawing context for the animation window
    int frame;                   // Current frame index (0 to POOF_FRAME_COUNT-1)
    double last_frame_time;      // Timestamp (in seconds) when we last advanced a frame
    bool active;                 // True while an animation is in progress
} poof = {0};

// ---------------------------------------------------------------------------
// Helper: get current monotonic time in seconds.
//
// CLOCK_MONOTONIC is a timer that only goes forward — it's not affected by
// the user changing their system clock or NTP adjustments. We use this
// instead of gettimeofday() so the animation timing is rock-solid.
// ---------------------------------------------------------------------------
static double get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

// ---------------------------------------------------------------------------
// Helper: draw a single frame of the poof animation.
//
// The sprite sheet is a vertical strip of 5 frames. To show frame N, we
// draw the entire sprite sheet offset upward so that frame N's 128x128
// region aligns with the window, then clip to the window bounds.
//
// Example: frame 2 starts at y=256 in the sprite sheet. We set the Cairo
// source surface origin to (0, -256) so that row 256 of the sheet appears
// at row 0 of the window.
// ---------------------------------------------------------------------------
static void poof_draw_frame(void)
{
    // Step 1: Clear the window to fully transparent.
    // CAIRO_OPERATOR_SOURCE replaces pixels entirely (no blending), so
    // painting RGBA(0,0,0,0) makes the whole window invisible.
    cairo_set_operator(poof.cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(poof.cr, 0, 0, 0, 0);
    cairo_paint(poof.cr);

    // Step 2: Draw the current frame from the sprite sheet.
    // Switch to OVER mode so the sprite's alpha channel composites correctly.
    cairo_set_operator(poof.cr, CAIRO_OPERATOR_OVER);

    // Position the sprite sheet so the current frame's top-left is at (0, 0).
    // Each frame is POOF_FRAME_SIZE pixels tall, so frame N starts at
    // y = N * POOF_FRAME_SIZE in the sheet.
    int source_y = poof.frame * POOF_FRAME_SIZE;
    cairo_set_source_surface(poof.cr, poof.sheet, 0, -source_y);

    // Clip to just the 128x128 frame area so we don't draw other frames
    cairo_rectangle(poof.cr, 0, 0, POOF_FRAME_SIZE, POOF_FRAME_SIZE);
    cairo_fill(poof.cr);

    // Step 3: Push the drawn pixels to the X server.
    // cairo_surface_flush() ensures Cairo's internal buffer is written to
    // the underlying X surface, and XFlush() sends it over the wire.
    cairo_surface_flush(poof.surface);
}

// ---------------------------------------------------------------------------
// poof_load — Load the poof sprite sheet from the assets directory.
//
// The sprite sheet lives at ~/.local/share/aqua-widgets/dock/poof.png.
// It's a 128x640 pixel RGBA image containing 5 frames stacked vertically.
// We load it once at startup and keep it in memory for the dock's lifetime.
//
// Returns true if the sprite sheet was loaded and validated successfully.
// ---------------------------------------------------------------------------
bool poof_load(DockState *state)
{
    (void)state;  // Not needed for loading, but kept for API consistency

    // Build the full path to the sprite sheet.
    // $HOME is set by the login shell and points to the user's home directory.
    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "[poof] ERROR: $HOME not set, cannot find poof.png\n");
        return false;
    }

    // Assemble the path: ~/.local/share/aqua-widgets/dock/poof.png
    char path[512];
    snprintf(path, sizeof(path), "%s/.local/share/aqua-widgets/dock/poof.png", home);

    // Load the PNG file into a Cairo image surface.
    // cairo_image_surface_create_from_png() reads the entire file into memory
    // and decodes it into an ARGB pixel buffer.
    poof.sheet = cairo_image_surface_create_from_png(path);

    // Check that the load succeeded. Cairo doesn't return NULL on failure —
    // instead it returns a "nil surface" with an error status.
    cairo_status_t status = cairo_surface_status(poof.sheet);
    if (status != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "[poof] ERROR: Failed to load %s: %s\n",
                path, cairo_status_to_string(status));
        cairo_surface_destroy(poof.sheet);
        poof.sheet = NULL;
        return false;
    }

    // Verify the dimensions match what we expect (128 wide, 640 tall = 5 frames).
    // If someone replaced the file with a different image, this catches it early
    // rather than producing garbled animation frames at runtime.
    int w = cairo_image_surface_get_width(poof.sheet);
    int h = cairo_image_surface_get_height(poof.sheet);
    if (w != POOF_FRAME_SIZE || h != POOF_FRAME_SIZE * POOF_FRAME_COUNT) {
        fprintf(stderr, "[poof] ERROR: poof.png is %dx%d, expected %dx%d\n",
                w, h, POOF_FRAME_SIZE, POOF_FRAME_SIZE * POOF_FRAME_COUNT);
        cairo_surface_destroy(poof.sheet);
        poof.sheet = NULL;
        return false;
    }

    fprintf(stderr, "[poof] Loaded sprite sheet: %s (%dx%d, %d frames)\n",
            path, w, h, POOF_FRAME_COUNT);
    return true;
}

// ---------------------------------------------------------------------------
// poof_start — Begin playing the poof animation at a screen position.
//
// Creates a small 128x128 override-redirect window centered on the given
// screen coordinates, then draws the first frame immediately. Subsequent
// frames are advanced by calling poof_update() from the event loop.
//
// Parameters:
//   state    — the dock state (needed for the X display, visual, colormap)
//   screen_x — X coordinate on screen where the drag was released
//   screen_y — Y coordinate on screen where the drag was released
// ---------------------------------------------------------------------------
void poof_start(DockState *state, int screen_x, int screen_y)
{
    // If the sprite sheet wasn't loaded (maybe the file was missing), bail out
    // silently. The icon removal still happens; the user just won't see the
    // smoke effect.
    if (!poof.sheet) {
        fprintf(stderr, "[poof] No sprite sheet loaded, skipping animation\n");
        return;
    }

    // If there's already an animation playing (e.g., user dragged two icons
    // out very quickly), clean up the old one before starting a new one.
    if (poof.active) {
        // Destroy the old animation window and Cairo resources
        if (poof.cr) {
            cairo_destroy(poof.cr);
            poof.cr = NULL;
        }
        if (poof.surface) {
            cairo_surface_destroy(poof.surface);
            poof.surface = NULL;
        }
        if (poof.win) {
            XDestroyWindow(state->dpy, poof.win);
            poof.win = None;
        }
        poof.active = false;
    }

    // --- Create the animation window ---
    // Override-redirect means the window manager completely ignores this window:
    // no title bar, no borders, no focus stealing, no Alt+Tab entry. This is
    // the standard approach for transient visual effects like this.

    // Center the 128x128 window on the release point
    int win_x = screen_x - POOF_FRAME_SIZE / 2;
    int win_y = screen_y - POOF_FRAME_SIZE / 2;

    // Set up window attributes for a transparent, unmanaged window.
    // We use the dock's 32-bit ARGB visual and colormap so that transparency
    // works correctly with the compositor (picom). Without the matching visual,
    // the window would be opaque or render with garbage in the alpha channel.
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;       // WM: hands off this window
    attrs.colormap = state->colormap;     // Use the dock's ARGB colormap
    attrs.border_pixel = 0;               // No visible border
    attrs.background_pixel = 0;           // Fully transparent background

    poof.win = XCreateWindow(
        state->dpy, state->root,          // Parent is the root (desktop)
        win_x, win_y,                     // Position: centered on release point
        POOF_FRAME_SIZE, POOF_FRAME_SIZE, // Size: 128x128 pixels
        0,                                // Border width: none
        32,                               // Depth: 32-bit ARGB
        InputOutput,                      // Window class: normal drawable window
        state->visual,                    // Visual: dock's 32-bit ARGB visual
        CWOverrideRedirect | CWColormap | CWBorderPixel | CWBackPixel,
        &attrs
    );

    // Tell X not to paint any background on this window. Without this, some
    // X servers fill the window with a default color before we get to draw.
    XSetWindowBackgroundPixmap(state->dpy, poof.win, None);

    // --- Create Cairo drawing surface on the window ---
    // This binds a Cairo context to the X window so we can use Cairo's
    // drawing API (which is much nicer than raw Xlib rendering).
    poof.surface = cairo_xlib_surface_create(
        state->dpy, poof.win, state->visual,
        POOF_FRAME_SIZE, POOF_FRAME_SIZE);
    poof.cr = cairo_create(poof.surface);

    // --- Initialize animation state ---
    poof.frame = 0;                       // Start at the first frame
    poof.last_frame_time = get_time();    // Record when we started
    poof.active = true;                   // Mark the animation as running

    // --- Show the window and bring it to the top ---
    // XMapRaised maps (makes visible) and raises (brings to front) in one call.
    // We want the poof to appear on top of everything, including the dock.
    XMapRaised(state->dpy, poof.win);

    // Draw the first frame immediately so there's no blank flash.
    // Without this, the window would appear as a transparent rectangle for
    // one frame before poof_update() gets a chance to draw.
    poof_draw_frame();
    XFlush(state->dpy);
}

// ---------------------------------------------------------------------------
// poof_update — Advance the animation if enough time has passed.
//
// This function is called every iteration of the dock's event loop. It
// checks whether POOF_FRAME_MS milliseconds have elapsed since the last
// frame, and if so, draws the next frame. When all 5 frames have been
// shown, it destroys the animation window and marks the animation as done.
//
// Returns:
//   true  — animation is still playing (call again next frame)
//   false — animation is finished or was not active
// ---------------------------------------------------------------------------
bool poof_update(DockState *state)
{
    // Nothing to do if no animation is running
    if (!poof.active) {
        return false;
    }

    // Check if enough time has passed for the next frame.
    // POOF_FRAME_MS is in milliseconds; we convert to seconds for comparison.
    double now = get_time();
    double elapsed_ms = (now - poof.last_frame_time) * 1000.0;

    if (elapsed_ms < POOF_FRAME_MS) {
        // Not time for the next frame yet, but the animation is still running
        return true;
    }

    // Time to advance to the next frame
    poof.frame++;
    poof.last_frame_time = now;

    // Check if we've played all the frames
    if (poof.frame >= POOF_FRAME_COUNT) {
        // Animation complete — tear down the temporary window.
        // The poof effect is done; destroy everything except the sprite sheet
        // (which we keep for the next poof_start() call).
        if (poof.cr) {
            cairo_destroy(poof.cr);
            poof.cr = NULL;
        }
        if (poof.surface) {
            cairo_surface_destroy(poof.surface);
            poof.surface = NULL;
        }
        if (poof.win) {
            XDestroyWindow(state->dpy, poof.win);
            poof.win = None;
        }
        poof.active = false;
        return false;
    }

    // Draw the new frame and push it to the screen
    poof_draw_frame();
    XFlush(state->dpy);

    return true;
}

// ---------------------------------------------------------------------------
// poof_is_active — Check whether a poof animation is currently playing.
//
// The event loop uses this to decide whether to use a short select() timeout
// (for smooth animation) or a long one (for power saving when idle).
// ---------------------------------------------------------------------------
bool poof_is_active(void)
{
    return poof.active;
}

// ---------------------------------------------------------------------------
// poof_cleanup — Free all poof resources.
//
// Called during dock shutdown (from dock_cleanup). Destroys the animation
// window if one is active, and frees the sprite sheet that was loaded at
// startup.
// ---------------------------------------------------------------------------
void poof_cleanup(DockState *state)
{
    // If an animation is mid-play, destroy its window and Cairo state
    if (poof.active) {
        if (poof.cr) {
            cairo_destroy(poof.cr);
            poof.cr = NULL;
        }
        if (poof.surface) {
            cairo_surface_destroy(poof.surface);
            poof.surface = NULL;
        }
        if (poof.win && state->dpy) {
            XDestroyWindow(state->dpy, poof.win);
            poof.win = None;
        }
        poof.active = false;
    }

    // Free the sprite sheet (loaded once in poof_load)
    if (poof.sheet) {
        cairo_surface_destroy(poof.sheet);
        poof.sheet = NULL;
    }

    // Zero out the entire struct for safety — prevents use-after-free if
    // someone accidentally calls poof functions after cleanup.
    memset(&poof, 0, sizeof(poof));
}
