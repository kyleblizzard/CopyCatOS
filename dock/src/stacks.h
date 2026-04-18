// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// stacks.h — Folder stacks grid popup (Snow Leopard style)
//
// When the user clicks a folder icon in the dock (e.g., Downloads), a popup
// appears above the dock showing the folder's contents in a grid layout.
// This mimics the "Grid" view of Mac OS X Snow Leopard's dock stacks.
//
// The popup features:
//   - A dark semi-transparent background built from nine-patch assets
//   - A callout arrow pointing down at the folder icon in the dock
//   - Items arranged in a grid (5 columns by N rows)
//   - Each item shows a 48x48 icon and a truncated label below it
//   - Hover highlighting with a blue rounded rectangle
//   - Click to open items with xdg-open
//   - Escape or click-outside to dismiss
//
// The nine-patch background pieces are PNG files stored at:
//   ~/.local/share/aqua-widgets/dock/stacks/eccl_*.png
// ============================================================================

#ifndef DOCK_STACKS_H
#define DOCK_STACKS_H

#include "dock.h"

// ---------------------------------------------------------------------------
// Load the nine-patch background assets for the stack popup.
// Called once during dock_init(). If some assets are missing, the popup
// will still work using a fallback solid background.
//
// Parameters:
//   state — the dock's global state (needed for the X display)
//
// Returns true on success (even if some assets are missing).
// ---------------------------------------------------------------------------
bool stacks_load_assets(DockState *state);

// ---------------------------------------------------------------------------
// Show a grid popup for a folder item.
//
// Scans the folder at folder_item->folder_path, loads icons for each file,
// arranges them in a grid, and displays the popup above the dock.
//
// Parameters:
//   state         — the dock's global state (X display, visual, colormap)
//   folder_item   — the DockItem for the folder that was clicked
//   icon_center_x — the screen X coordinate of the folder icon's center,
//                    used to position the callout arrow
// ---------------------------------------------------------------------------
void stacks_show(DockState *state, DockItem *folder_item, double icon_center_x);

// ---------------------------------------------------------------------------
// Close the stack popup if it's currently open.
// Releases the pointer grab, destroys the window, and frees entry icons.
// ---------------------------------------------------------------------------
void stacks_close(DockState *state);

// ---------------------------------------------------------------------------
// Handle X11 events for the stack popup (mouse clicks, motion, key presses).
// Called from the dock's main event loop before other handlers.
//
// Returns true if the event was consumed by the stacks popup, meaning
// the dock should skip its normal event processing for this event.
// ---------------------------------------------------------------------------
bool stacks_handle_event(DockState *state, XEvent *ev);

// ---------------------------------------------------------------------------
// Check whether a stack popup is currently open.
// Used by the event loop to decide whether to route events to stacks first.
// ---------------------------------------------------------------------------
bool stacks_is_open(void);

// ---------------------------------------------------------------------------
// Free all stacks resources (nine-patch surfaces, entry icons, etc.).
// Called during dock_cleanup().
// ---------------------------------------------------------------------------
void stacks_cleanup(DockState *state);

#endif // DOCK_STACKS_H
