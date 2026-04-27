// CopyCatOS — by Kyle Blizzard at Blizzard.show

// desktop.h — Core desktop surface manager
//
// This is the central module that ties everything together. It owns
// the X display connection, the full-screen desktop window, and the
// main event loop. Other modules (wallpaper, icons, context menu)
// are initialized and driven from here.

#ifndef AURA_DESKTOP_H
#define AURA_DESKTOP_H

#include <X11/Xlib.h>
#include <stdbool.h>

#include "moonrock_scale.h"  // MOONROCK_SCALE_MAX_OUTPUTS, MOONROCK_SCALE_NAME_MAX

// Per-output HiDPI scale for the output hosting the desktop's icon grid
// (the primary output, since icons anchor top-right of primary). Published
// by MoonRock on _MOONROCK_OUTPUT_SCALES. 1.0 when MoonRock isn't running
// (the desktop still draws, just without hidpi awareness).
extern float desktop_hidpi_scale;

// Scale a point value to physical pixels, rounded to nearest int.
// Every layout constant in icons.h is written against the 1.0x baseline.
// S()/SF() multiply by desktop_hidpi_scale so one expression produces the
// right pixel count on 1.0x externals, 1.5x midrange panels, and 1.75x on
// the Legion Go S.
#define S(x) ((int)((x) * desktop_hidpi_scale + 0.5))

// Scale a point value to physical pixels as a double (no rounding).
// Use for Cairo coordinates, corner radii, and other fractional values.
#define SF(x) ((x) * (double)desktop_hidpi_scale)

// One DesktopOutput per connected output. Each one owns its own
// _NET_WM_WINDOW_TYPE_DESKTOP window sized exactly to that output's
// rectangle in virtual-screen pixels — so each physical display gets a
// real surface to draw the wallpaper on, instead of one giant window
// that only happens to cover the leftmost screen.
typedef struct {
    char     name[MOONROCK_SCALE_NAME_MAX];  // XRandR name, e.g. "eDP-1"
    int      x, y;        // Top-left in virtual-screen pixels
    int      width;       // Pixel width  of this output
    int      height;      // Pixel height of this output
    float    scale;       // Effective scale published by MoonRock
    bool     primary;     // True if this is the XRandR primary output
    Window   win;         // Per-output desktop window (None until mapped)
} DesktopOutput;

// Holds all the state for the desktop surface.
// A single instance of this struct is created in main.c and passed
// around to the other modules.
typedef struct {
    Display *dpy;          // Connection to the X server
    int      screen;       // Default screen number (usually 0)
    Window   root;         // Root window of the screen
    bool     running;      // Set to false to exit the event loop
    Visual  *visual;       // Visual we're using (ARGB if available)
    int      depth;        // Color depth (32 for ARGB, else default)
    Colormap colormap;     // Colormap matching our visual

    // Per-output windows. Index into icon-anchor logic and PropertyNotify
    // reconcile. output_count == 0 only briefly during startup before the
    // first reconcile.
    DesktopOutput outputs[MOONROCK_SCALE_MAX_OUTPUTS];
    int           output_count;
    int           primary_idx;  // Index of primary output, or 0 as fallback
} Desktop;

// Initialize the desktop: open X display, create window, load wallpaper
// and icons. The wallpaper_path can be NULL to use the default.
// Returns true on success, false on failure.
bool desktop_init(Desktop *d, const char *wallpaper_path);

// Run the main event loop. This blocks until d->running becomes false
// (e.g., from a signal handler or error). Handles all X events and
// inotify events for ~/Desktop file changes.
void desktop_run(Desktop *d);

// Clean up all resources: destroy windows, free surfaces, close display.
void desktop_shutdown(Desktop *d);

#endif // AURA_DESKTOP_H
