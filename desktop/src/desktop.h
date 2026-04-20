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

// Per-output HiDPI scale for the output hosting the desktop's icon grid,
// published by MoonRock on _MOONROCK_OUTPUT_SCALES. 1.0 when MoonRock isn't
// running (the desktop still draws, just without hidpi awareness).
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

// Holds all the state for the desktop surface.
// A single instance of this struct is created in main.c and passed
// around to the other modules.
typedef struct {
    Display *dpy;          // Connection to the X server
    int      screen;       // Default screen number (usually 0)
    Window   root;         // Root window of the screen
    Window   win;          // Our full-screen desktop window
    int      width;        // Screen width in pixels
    int      height;       // Screen height in pixels
    bool     running;      // Set to false to exit the event loop
    Visual  *visual;       // Visual we're using (ARGB if available)
    int      depth;        // Color depth (32 for ARGB, else default)
    Colormap colormap;     // Colormap matching our visual
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
