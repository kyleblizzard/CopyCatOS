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
// Core constants — EXACT values from the Python prototype
// These define every pixel measurement, timing value, and limit in the dock.
// ---------------------------------------------------------------------------

#define BASE_ICON_SIZE      54      // Default icon size in pixels (no magnification)
#define MAX_ICON_SIZE       82      // Largest an icon gets when mouse is directly over it
#define MAGNIFICATION_RANGE 3       // How many icon slots away magnification reaches
#define ICON_SPACING        6       // Pixels of gap between adjacent icons
#define SEPARATOR_WIDTH     12      // Width of the separator between dock sections
#define SHELF_HEIGHT        42      // Height of the glass shelf at the bottom
#define DOCK_HEIGHT         130     // Total dock window height (icons + shelf + reflections)
#define SHELF_PADDING       35      // Extra pixels on each side of the shelf beyond icons
#define INDICATOR_SIZE      8       // Diameter of the "running" dot below icons
#define BOUNCE_AMPLITUDE    26      // Max pixels an icon bounces upward
#define BOUNCE_CYCLE_MS     720     // Duration of one full bounce cycle in milliseconds
#define BOUNCE_TIMEOUT_MS   10000   // Stop bouncing after this many ms (10 seconds)
#define BOUNCE_FRAME_MS     16      // Time between animation frames (~60fps)
#define MAX_DOCK_ITEMS      32      // Maximum number of items the dock can hold

// How often (in seconds) we check which apps are currently running
#define PROCESS_CHECK_INTERVAL 3.0

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
    char process_name[128];       // Process name to look for in `ps` output

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
    cairo_surface_t *shelf_img;   // The scurve-xl.png shelf background
    cairo_surface_t *frontline_img; // The 1px highlight along the shelf top

    // Indicator asset
    cairo_surface_t *indicator_img; // The running-app indicator dot

    // Event loop control
    bool running_loop;            // Set to false to exit the main loop
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

// Clean up all resources (surfaces, X connection, etc.)
void dock_cleanup(DockState *state);

#endif // DOCK_H
