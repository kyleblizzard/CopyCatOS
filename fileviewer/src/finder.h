// CopyCatOS — by Kyle Blizzard at Blizzard.show

// finder.h — Core Finder state and lifecycle
//
// This is the central struct that holds all state for an CCFinder window.
// It owns the X display connection, the window, and the Cairo rendering
// context. Other modules (toolbar, sidebar, content) read from and write
// to this struct to coordinate painting and interaction.
//
// CCFinder is a standalone C + Xlib + Cairo + Pango application.
// It does NOT use Qt — it creates its own window, and moonrock frames
// it with Snow Leopard title bar chrome automatically.

#ifndef AURA_FINDER_H
#define AURA_FINDER_H

#include <X11/Xlib.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <stdbool.h>

// ── Layout constants ────────────────────────────────────────────────
// These define the fixed dimensions of the Finder's three zones:
// toolbar (top), sidebar (left), and content area (remaining space).

// ── View modes ─────────────────────────────────────────────────────
// Defined here (root header) so toolbar.h and content.h share the same enum.
typedef enum {
    VIEW_MODE_ICON   = 0,   // Grid of icons
    VIEW_MODE_LIST   = 1,   // Rows with columns
    VIEW_MODE_COLUMN = 2,   // Miller columns
    VIEW_MODE_CFLOW  = 3,   // Cover Flow
    VIEW_MODE_COUNT  = 4
} ViewMode;

#define FINDER_DEFAULT_W   700   // Default window width in pixels
#define FINDER_DEFAULT_H   500   // Default window height in pixels
#define FINDER_TOOLBAR_H    30   // Height of the toolbar area (top)
#define FINDER_SIDEBAR_W   200   // Width of the sidebar (left)
#define FINDER_STATUSBAR_H  20   // Height of the status bar (bottom, above path bar)
#define FINDER_PATHBAR_H    22   // Height of the path bar (very bottom)

// ── Core state struct ───────────────────────────────────────────────

typedef struct {
    // X11 resources
    Display *dpy;          // Connection to the X server
    Window   win;          // Our application window (WM frames it)
    int      screen;       // Default screen number (usually 0)
    Visual  *visual;       // Visual for the window (usually default)
    int      depth;        // Color depth of the visual

    // Window dimensions (updated on ConfigureNotify)
    int win_w;             // Current window width in pixels
    int win_h;             // Current window height in pixels

    // Layout dimensions — these are constants but stored here so every
    // module can access them from the FinderState pointer.
    int toolbar_h;         // Height of toolbar (FINDER_TOOLBAR_H)
    int sidebar_w;         // Width of sidebar (FINDER_SIDEBAR_W)
    int statusbar_h;       // Height of status bar (FINDER_STATUSBAR_H)
    int pathbar_h;         // Height of path bar (FINDER_PATHBAR_H)

    // Current directory path being displayed in the content area.
    // Starts at the user's home directory by default.
    char path[1024];

    // Cairo rendering context — recreated whenever the window resizes.
    // All painting (toolbar, sidebar, content) goes through this.
    cairo_surface_t *surface;
    cairo_t         *cr;

    // Event loop control flag. Set to false to exit the main loop.
    bool running;
} FinderState;

// ── Lifecycle functions ─────────────────────────────────────────────

// Initialize the Finder: open X display, create window, set up Cairo.
// initial_path can be NULL (defaults to $HOME) or a specific directory.
// Returns true on success, false on failure.
bool finder_init(FinderState *fs, const char *initial_path);

// Run the main event loop. Blocks until fs->running becomes false.
// Handles Expose, ButtonPress, MotionNotify, KeyPress, ConfigureNotify.
void finder_run(FinderState *fs);

// Clean up all resources: destroy Cairo context, close X display.
void finder_shutdown(FinderState *fs);

// Repaint the entire window (toolbar + sidebar + content).
// Called on Expose events and after navigation changes.
void finder_paint(FinderState *fs);

// Navigate the Finder to a new directory path.
// Updates fs->path and refreshes the content area.
void finder_navigate(FinderState *fs, const char *new_path);

#endif // AURA_FINDER_H
