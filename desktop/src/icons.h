// CopyCatOS — by Kyle Blizzard at Blizzard.show

// icons.h — Desktop icon grid manager
//
// Scans ~/Desktop for files and folders, loads appropriate icons from
// the Aqua icon theme, and arranges them in a grid starting from
// the top-right corner of the screen (like classic Mac OS).
//
// Supports:
//   - Click to select, double-click to open with xdg-open
//   - Drag and drop to reposition icons on the grid
//   - inotify-based auto-refresh when ~/Desktop contents change
//   - Icon labels with white text and drop shadow

#ifndef DESKTOP_ICONS_H
#define DESKTOP_ICONS_H

#include <X11/Xlib.h>
#include <cairo/cairo.h>
#include <stdbool.h>
#include <stdint.h>

// Grid layout constants — these control how icons are spaced and sized.
// The grid starts at the top-right and fills downward, then moves left
// (column by column), just like Finder in Mac OS X.
//
// Values are in POINTS at the 1.0x baseline (matches the original Snow
// Leopard desktop). Every use site runs them through S() in desktop.h so
// a HiDPI output doubles/triples them without the constants themselves
// having to change. ICON_SIZE is the target rendered size — source
// surfaces from the theme are cairo_scaled to S(ICON_SIZE) per paint.
#define ICON_CELL_W         140  // Grid cell width (points)
#define ICON_CELL_H         140  // Grid cell height (points)
#define ICON_SIZE           128  // Rendered icon size (points) — 128pt matches SL
#define ICON_TOP_MARGIN     45   // Space below the menubar for breathing room (points)
#define ICON_RIGHT_MARGIN   20   // Space from the right edge of screen (points)
#define ICON_MAX_LABEL_WIDTH 110 // Max label width before ellipsis (points)

// Represents a single desktop icon. Each file or folder in ~/Desktop
// gets one of these structs.
typedef struct {
    char name[256];              // Display name (filename, possibly without extension)
    char path[1024];             // Full filesystem path to the file
    cairo_surface_t *icon;       // Pre-loaded icon surface (scaled to ICON_SIZE)
    int grid_col, grid_row;      // Last canonical grid cell (runtime hint).
                                 // Used by Clean Up and as a tiebreak for
                                 // auto-place collision avoidance — not the
                                 // source of truth for icon position. The
                                 // user-visible position lives in (x, y) and
                                 // persists via the user.moonbase.position
                                 // xattr on the file itself.
    int x, y;                    // Pixel position on screen (free-form).
                                 // Restored from xattr (in points → pixels)
                                 // or computed by auto-place when no xattr.
    bool has_pos;                // True if (x, y) was restored from a
                                 // saved xattr position (free-form drop).
                                 // False for auto-placed icons.
    bool selected;               // Whether this icon is currently highlighted
    bool is_directory;           // true if this is a folder
    int label;                   // Color label index (0=none, 1-7=Red/Orange/Yellow/Green/Blue/Purple/Grey)
    int z_order;                 // Stacking order. Higher = visually on top.
                                 // Bumped on click so clicking a buried icon
                                 // surfaces it. Ephemeral — initialized at
                                 // scan time and not persisted to xattr.
    uint8_t *hit_mask;           // Pixel mask, source-surface resolution. 1
                                 // means the pixel is inside the icon's
                                 // outer perimeter (opaque OR interior hole
                                 // unreachable from the corners); 0 means
                                 // exterior transparency that should let
                                 // clicks fall through to a lower icon.
                                 // Computed at scan time by flood-filling
                                 // transparent pixels from the four corners.
                                 // Owned by this struct — freed in
                                 // icons_shutdown / on rescan.
    int hit_mask_w, hit_mask_h;  // Dimensions of the mask buffer (matches
                                 // the source PNG, not the rendered cell).
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

// Open the given icon's file (fork + exec).
// For .desktop files, parses and executes the Exec= line directly.
// For all other files, launches xdg-open. Called on double-click.
void icons_handle_double_click(DesktopIcon *icon);

// Select a single icon (deselects all others first).
void icons_select(DesktopIcon *icon);

// Clear the selection on all icons.
void icons_deselect_all(void);

// Check the inotify file descriptor for changes to ~/Desktop.
// If changes are detected, debounce for 200ms and rescan.
// Returns true if icons were refreshed (caller should repaint).
bool icons_check_inotify(void);

// Begin dragging an icon. (x, y) are primary-pane-local pixel coords of
// the click point. No window is created here — the override-redirect
// "ghost" popup that visually follows the cursor is created lazily on
// the first icons_drag_update() call (i.e. only after the drag-threshold
// crosses), so a simple click+release that never moves doesn't create
// or map any X window.
void icons_drag_begin(DesktopIcon *icon, int x, int y);

// Update the dragged icon's position. (local_x, local_y) are pane-local
// (used to update the icon's logical x/y so icons_drag_end's clamp uses
// the right final position); (root_x, root_y) are virtual-screen-root
// coords used to XMoveWindow the ghost popup. First call after a
// drag_begin creates and maps the ghost; subsequent calls just move it.
void icons_drag_update(int local_x, int local_y, int root_x, int root_y);

// End the drag operation: clamp the icon's position to the visible
// primary-pane bounds, persist its xattr, destroy the ghost popup, and
// clear all drag state. Call this on a successful (non-XDND) drop.
void icons_drag_end(int screen_w, int screen_h);

// Cancel any in-progress drag without clamping or saving: destroys the
// ghost popup if one was mapped and clears all drag state. Safe to call
// even when no drag is active (simple click+release, XDND-handled drop,
// XDND cancellation, ButtonRelease before threshold cross).
void icons_drag_cancel(void);

// Return the inotify file descriptor so the event loop can select() on it.
// Returns -1 if inotify is not available.
int icons_get_inotify_fd(void);

// Get the current number of desktop icons.
int icons_get_count(void);

// Re-layout all icons to canonical grid positions (used by "Clean Up").
void icons_relayout(int screen_w, int screen_h);

// Re-apply the saved layout against the cached screen dimensions.
// Called from desktop_run after a MoonRock scale change so icon pixel
// positions track the new scale. The cached dimensions are set by
// icons_init() and match the screen size, which doesn't change on a
// pure scale change — only the layout math (which walks the ICON_*
// constants through the S() macro) does.
void icons_rescale(void);

// Free all icon resources.
void icons_shutdown(void);

#endif // DESKTOP_ICONS_H
