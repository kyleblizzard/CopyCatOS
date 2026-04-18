// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// sysprefs.h — System Preferences main state and public API
// ============================================================================
//
// This is the core header for systemcontrol. It defines the main application
// state struct and the public functions for initialization, running, and
// cleanup. The architecture mirrors dock and searchsystem: a single state
// struct owns all X11 resources, Cairo surfaces, and UI state.
// ============================================================================

#ifndef CC_SYSPREFS_H
#define CC_SYSPREFS_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>
#include <stdbool.h>

// ============================================================================
//  Constants
// ============================================================================

// Window dimensions (matches real Snow Leopard System Preferences)
#define SYSPREFS_WIN_W      668
#define SYSPREFS_WIN_H      500

// Toolbar area at the top of the content window
#define TOOLBAR_HEIGHT       38

// Maximum number of preference panes
#define MAX_PANES            40

// Navigation history depth
#define MAX_HISTORY          64

// View modes
#define VIEW_GRID            0
#define VIEW_PANE            1

// ============================================================================
//  Data structures
// ============================================================================

// Information about a single preference pane (icon, name, category)
typedef struct {
    char id[64];                // Internal identifier (e.g. "dock")
    char name[128];             // Display name (e.g. "Dock")
    char category[32];          // Category key (e.g. "personal")
    char icon_path_32[512];     // Path to 32x32 icon PNG
    char icon_path_128[512];    // Path to 128x128 icon PNG
    cairo_surface_t *icon_32;   // Loaded 32x32 icon surface
    cairo_surface_t *icon_128;  // Loaded 128x128 icon surface (lazy)
} PaneInfo;

// Category header (rendered above each group of panes)
typedef struct {
    char key[32];               // Internal key (e.g. "personal")
    char label[64];             // Display label (e.g. "Personal")
    int first_pane;             // Index of first pane in this category
    int pane_count;             // Number of panes in this category
} CategoryInfo;

// Main application state — owns all resources
typedef struct {
    // X11 resources
    Display *dpy;
    Window win;
    Window root;
    int screen;
    int screen_w, screen_h;
    Visual *visual;
    Colormap colormap;

    // Cairo drawing context
    cairo_surface_t *surface;
    cairo_t *cr;

    // Window geometry
    int win_w, win_h;

    // View state
    int current_view;           // VIEW_GRID or VIEW_PANE
    int current_pane;           // Index into panes[] when viewing a pane
    int hover_pane;             // Mouse hover target in grid (-1 = none)

    // Navigation history
    int history[MAX_HISTORY];
    int history_pos;            // Current position in history
    int history_len;            // Total items in history

    // Search
    char search_query[256];
    bool search_focused;

    // Pane registry
    PaneInfo panes[MAX_PANES];
    int pane_count;

    // Category info (computed from panes)
    CategoryInfo categories[8];
    int category_count;

    // Mouse state
    int mouse_x, mouse_y;

    // Toolbar hover state
    int toolbar_hover;          // -1=none, 0=show_all, 1=back, 2=forward

    // Event loop control
    bool running;
} SysPrefsState;

// ============================================================================
//  Public API
// ============================================================================

// Initialize the application: open display, create window, load assets
bool sysprefs_init(SysPrefsState *state);

// Run the main event loop (blocks until quit)
void sysprefs_run(SysPrefsState *state);

// Full repaint of the entire window
void sysprefs_paint(SysPrefsState *state);

// Clean up all resources
void sysprefs_cleanup(SysPrefsState *state);

// Navigate to a specific pane by index
void sysprefs_open_pane(SysPrefsState *state, int pane_index);

// Return to the icon grid view
void sysprefs_show_all(SysPrefsState *state);

#endif // CC_SYSPREFS_H
