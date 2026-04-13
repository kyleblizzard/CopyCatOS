// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// icons.h — Desktop icon grid manager
//
// Scans ~/Desktop for files and folders, loads appropriate icons from
// the AquaKDE icon theme, and arranges them in a grid starting from
// the top-right corner of the screen (like classic Mac OS).
//
// Supports:
//   - Click to select, double-click to open with xdg-open
//   - Drag and drop to reposition icons on the grid
//   - inotify-based auto-refresh when ~/Desktop contents change
//   - Icon labels with white text and drop shadow

#ifndef AURA_ICONS_H
#define AURA_ICONS_H

#include <X11/Xlib.h>
#include <cairo/cairo.h>
#include <stdbool.h>

// Grid layout constants — these control how icons are spaced and sized.
// The grid starts at the top-right and fills downward, then moves left
// (column by column), just like Finder in Mac OS X.
#define ICON_CELL_W         120  // Width of each grid cell in pixels
#define ICON_CELL_H         120  // Height of each grid cell in pixels
#define ICON_SIZE           96   // Size of the icon image (96x96 — closer to real SL)
#define ICON_LABEL_FONT     "Lucida Grande 12"  // Pango font description (SL uses 12pt)
#define ICON_TOP_MARGIN     40   // Pixels below the top edge (room for menubar)
#define ICON_RIGHT_MARGIN   20   // Pixels from the right edge of screen
#define ICON_MAX_LABEL_WIDTH 110 // Max pixel width before truncating with "..."

// Represents a single desktop icon. Each file or folder in ~/Desktop
// gets one of these structs.
typedef struct {
    char name[256];              // Display name (filename, possibly without extension)
    char path[1024];             // Full filesystem path to the file
    cairo_surface_t *icon;       // Pre-loaded icon surface (scaled to ICON_SIZE)
    int grid_col, grid_row;      // Position in the logical grid
    int x, y;                    // Pixel position on screen (computed from grid)
    bool selected;               // Whether this icon is currently highlighted
    bool is_directory;           // true if this is a folder
} DesktopIcon;

// Initialize the icon system: scan ~/Desktop, load icons, compute grid
// layout, and set up inotify watching.
// screen_w and screen_h are needed to compute grid positions.
void icons_init(Display *dpy, int screen_w, int screen_h);

// Paint all desktop icons onto the given Cairo context.
// Each icon gets: selection highlight (if selected), the icon image,
// and a text label with drop shadow below it.
void icons_paint(cairo_t *cr, int screen_w, int screen_h);

// Check if a click at (x, y) hit any icon. Returns a pointer to the
// clicked icon, or NULL if the click was on empty space.
DesktopIcon *icons_handle_click(int x, int y);

// Open the given icon's file using xdg-open (fork + exec).
// Called when a double-click is detected.
void icons_handle_double_click(DesktopIcon *icon);

// Select a single icon (deselects all others first).
void icons_select(DesktopIcon *icon);

// Clear the selection on all icons.
void icons_deselect_all(void);

// Check the inotify file descriptor for changes to ~/Desktop.
// If changes are detected, debounce for 200ms and rescan.
// Returns true if icons were refreshed (caller should repaint).
bool icons_check_inotify(void);

// Begin dragging an icon from position (x, y).
void icons_drag_begin(DesktopIcon *icon, int x, int y);

// Update the position of the icon being dragged to (x, y).
void icons_drag_update(int x, int y);

// End the drag operation and snap the icon to the nearest free grid cell.
void icons_drag_end(int screen_w, int screen_h);

// Return the inotify file descriptor so the event loop can select() on it.
// Returns -1 if inotify is not available.
int icons_get_inotify_fd(void);

// Get the current number of desktop icons.
int icons_get_count(void);

// Re-layout all icons to canonical grid positions (used by "Clean Up").
void icons_relayout(int screen_w, int screen_h);

// Free all icon resources.
void icons_shutdown(void);

#endif // AURA_ICONS_H
