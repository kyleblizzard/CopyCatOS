// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// dock.h — Core dock data structures and public interface
//
// This header defines everything the dock needs to track its state: the list
// of dock items (apps), the X11 display/window handles, animation state, and
// the functions to initialize, run, and clean up the dock.
//
// Every other module (shelf, magnify, bounce, etc.) includes this header to
// access the shared DockItem struct and the global dock state.
// ============================================================================

#ifndef DOCK_H
#define DOCK_H

#include <stdbool.h>
#include <cairo/cairo.h>
#include <X11/Xlib.h>

// ---------------------------------------------------------------------------
// Default values — used when no config file exists.
// At runtime, all sizes come from DockConfig (computed from icon_size).
// ---------------------------------------------------------------------------

#define DEFAULT_ICON_SIZE        64   // Configurable: 32-128
#define DEFAULT_MAX_ICON_SIZE    96   // Auto: icon_size * 1.5
#define DEFAULT_SHELF_HEIGHT     48   // Auto: icon_size * 0.75
#define DEFAULT_DOCK_HEIGHT     160   // Auto: icon_size * 2.5
#define DEFAULT_ICON_SPACING      6   // Auto: max(4, icon_size / 10)
#define DEFAULT_SEPARATOR_WIDTH  12   // Auto: max(8, icon_size / 5)
#define DEFAULT_SHELF_PADDING    30   // Auto: icon_size / 2
#define DEFAULT_INDICATOR_SIZE    8   // Auto: max(6, icon_size / 8)
#define DEFAULT_BOUNCE_AMPLITUDE 26   // Auto: icon_size * 0.4

// These don't scale with icon size
#define MAGNIFICATION_RANGE 3       // How many icon slots away magnification reaches
#define BOUNCE_CYCLE_MS     720     // Duration of one full bounce cycle in milliseconds
#define BOUNCE_TIMEOUT_MS   10000   // Stop bouncing after this many ms (10 seconds)
#define BOUNCE_FRAME_MS     16      // Time between animation frames (~60fps)
#define MAX_DOCK_ITEMS      32      // Maximum number of items the dock can hold
#define PROCESS_CHECK_INTERVAL 3.0  // Seconds between running-app checks

// ---------------------------------------------------------------------------
// DockConfig — Runtime sizing values, computed from icon_size
//
// All dimensions scale proportionally from icon_size. The user sets icon_size
// via ~/.config/copycatos/desktop.conf [dock] section. Everything else is
// derived automatically. This supports sizes from 32px (tiny, handheld-dense)
// to 128px (huge, handheld-friendly).
// ---------------------------------------------------------------------------
typedef struct {
    int icon_size;          // Base icon display size (32-128)
    int max_icon_size;      // Max magnified size (icon_size * 1.5)
    int shelf_height;       // Glass shelf height (icon_size * 0.75)
    int dock_height;        // Total window height (icon_size * 2.5)
    int icon_spacing;       // Gap between icons (max(4, icon_size / 10))
    int separator_width;    // Section divider width (max(8, icon_size / 5))
    int shelf_padding;      // Horizontal padding beyond icons (icon_size / 2)
    int indicator_size;     // Running-app dot diameter (max(6, icon_size / 8))
    int bounce_amplitude;   // Max bounce height (icon_size * 0.4)
    int icon_bottom_offset; // Icons rest this far above dock bottom (icon_size * 0.19)
} DockConfig;

// Compute all DockConfig values from a given icon_size.
// Call this after reading the config file.
static inline void dock_config_from_icon_size(DockConfig *cfg, int icon_size)
{
    if (icon_size < 32)  icon_size = 32;
    if (icon_size > 196) icon_size = 196;

    cfg->icon_size          = icon_size;
    cfg->max_icon_size      = icon_size * 3 / 2;       // 1.5x
    cfg->shelf_height       = icon_size * 3 / 4;        // 0.75x
    cfg->dock_height        = icon_size * 5 / 2;        // 2.5x
    cfg->icon_spacing       = icon_size / 10;
    if (cfg->icon_spacing < 4) cfg->icon_spacing = 4;
    cfg->separator_width    = icon_size / 5;
    if (cfg->separator_width < 8) cfg->separator_width = 8;
    cfg->shelf_padding      = icon_size / 2;
    cfg->indicator_size     = icon_size / 8;
    if (cfg->indicator_size < 6) cfg->indicator_size = 6;
    cfg->bounce_amplitude   = icon_size * 2 / 5;        // 0.4x
    cfg->icon_bottom_offset = icon_size * 19 / 100;     // 0.19x
    if (cfg->icon_bottom_offset < 8) cfg->icon_bottom_offset = 8;
}

// Convenience macros so existing code can reference config through state->cfg
// without changing every function signature. These are used in the transition
// from hardcoded #defines to dynamic config.
#define BASE_ICON_SIZE      (state->cfg.icon_size)
#define MAX_ICON_SIZE       (state->cfg.max_icon_size)
#define ICON_SPACING        (state->cfg.icon_spacing)
#define SEPARATOR_WIDTH     (state->cfg.separator_width)
#define SHELF_HEIGHT        (state->cfg.shelf_height)
#define DOCK_HEIGHT         (state->cfg.dock_height)
#define SHELF_PADDING       (state->cfg.shelf_padding)
#define INDICATOR_SIZE      (state->cfg.indicator_size)
#define BOUNCE_AMPLITUDE    (state->cfg.bounce_amplitude)

// ---------------------------------------------------------------------------
// DockItem — Represents a single app icon in the dock
//
// Each item knows its display name, how to launch it, where its icon file is,
// and tracks real-time animation state (magnification scale, bounce offset).
// ---------------------------------------------------------------------------
typedef struct {
    char name[128];               // Human-readable app name (e.g., "Finder")
    char exec_path[512];          // Command to launch the app (e.g., "dolphin")
    char icon_path[512];          // Resolved filesystem path to the icon PNG
    char icon_name[256];          // Original icon theme name (e.g., "org.kde.dolphin")
                                  // Stored so we can write it back to the config file
    char process_name[128];       // Process name to look for in `ps` output

    bool is_folder;               // True if this is a folder stack item (not an app)
    char folder_path[512];        // Filesystem path to the folder (for stacks, e.g., ~/Downloads)

    bool is_spacer;               // True if this is a blank draggable spacer (no icon, no launch)

    cairo_surface_t *icon;        // The loaded icon image (128x128 typically)

    double scale;                 // Current magnification multiplier (1.0 = normal)
    double bounce_offset;         // Y-axis offset for bounce animation (negative = up)

    bool running;                 // True if this app's process is currently running
    bool bouncing;                // True if the bounce animation is active
    bool separator_after;         // True if a separator line should be drawn after this icon

    double bounce_start_time;     // Timestamp (seconds) when the bounce animation started
} DockItem;

// ---------------------------------------------------------------------------
// DockState — Global state for the entire dock
//
// Holds all X11 resources, the list of items, mouse tracking info, and
// flags that control the event loop and animation timers.
// ---------------------------------------------------------------------------
typedef struct {
    // Runtime sizing config (computed from icon_size at startup)
    DockConfig cfg;

    // X11 display and window handles
    Display *dpy;                 // Connection to the X server
    Window win;                   // The dock's own window
    Window root;                  // The root window (desktop background)
    int screen;                   // Screen number (usually 0)
    int screen_w;                 // Screen width in pixels
    int screen_h;                 // Screen height in pixels
    Visual *visual;               // 32-bit ARGB visual for transparency
    Colormap colormap;            // Colormap matching our ARGB visual

    // Cairo drawing surface bound to our X window
    cairo_surface_t *surface;
    cairo_t *cr;

    // Dock items (the apps shown in the dock)
    DockItem items[MAX_DOCK_ITEMS];
    int item_count;               // How many items are actually in the array

    // Window geometry
    int win_x;                    // X position of dock window on screen
    int win_y;                    // Y position of dock window on screen
    int win_w;                    // Current width of the dock window
    int win_h;                    // Current height of the dock window

    // Mouse tracking
    int mouse_x;                  // Last known mouse X position relative to dock
    int mouse_y;                  // Last known mouse Y position relative to dock
    bool mouse_in_dock;           // True when the mouse cursor is inside the dock window

    // Animation state
    bool any_bouncing;            // True if any icon is currently bouncing
    double last_process_check;    // Timestamp of last process-running check

    // Shelf assets (loaded once, reused every frame)
    // All from real Snow Leopard assets — never generated/faked
    cairo_surface_t *shelf_img;      // scurve-xl.png — glass shelf gradient
    cairo_surface_t *frontline_img;  // frontline.png — top edge highlight
    cairo_surface_t *separator_img;  // separator.png — dashed divider between sections

    // Indicator asset
    cairo_surface_t *indicator_img; // indicator_medium.png — running-app dot

    // Event loop control
    bool running_loop;            // Set to false to exit the main loop

    // (Drag-and-drop state is managed by dnd.c using a local DndState variable)
} DockState;

// ---------------------------------------------------------------------------
// Public functions
// ---------------------------------------------------------------------------

// Initialize the dock: create the X window, load icons, set up struts.
// Returns true on success, false if something critical failed.
bool dock_init(DockState *state);

// Enter the main event loop. This function blocks until the dock is closed.
void dock_run(DockState *state);

// Repaint the entire dock (called on Expose events and animation frames).
void dock_paint(DockState *state);

// Calculate the total width the icons occupy at their current scales.
// This is used to center icons in the dock window.
int dock_calculate_total_width(DockState *state);

// Get the X coordinate of a specific icon's center, accounting for
// all icons' current scales and spacing.
double dock_get_icon_center_x(DockState *state, int index);

// Hit test: return the index of the icon under (local_x, local_y), or -1
int dock_hit_test(DockState *state, int local_x, int local_y);

// Clean up all resources (surfaces, X connection, etc.)
void dock_cleanup(DockState *state);

#endif // DOCK_H
