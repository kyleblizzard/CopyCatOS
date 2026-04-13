// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// toolbar.h — Snow Leopard Finder toolbar
//
// The toolbar sits at the top of the Finder window (inside the client
// area, below the WM title bar). It contains:
//
//   - View mode buttons: Icon, List, Column, Cover Flow
//   - Path breadcrumb: "Macintosh HD > Users > home > Documents"
//   - Search field: rounded rectangle on the right side
//
// The toolbar has a subtle gradient background matching the real
// Snow Leopard Finder toolbar (#D8D8D8 at top → #C0C0C0 at bottom).

#ifndef AURA_TOOLBAR_H
#define AURA_TOOLBAR_H

#include "finder.h"

// View mode enumeration — controls how the content area displays files.
// Phase 1 only implements ICON view; the others are drawn as buttons
// but not yet functional.
typedef enum {
    VIEW_MODE_ICON    = 0,  // Grid of 64x64 icons
    VIEW_MODE_LIST    = 1,  // Rows with file details
    VIEW_MODE_COLUMN  = 2,  // Miller columns (like Finder column view)
    VIEW_MODE_CFLOW   = 3,  // Cover Flow (preview + list)
    VIEW_MODE_COUNT   = 4   // Total number of view modes
} ViewMode;

// Get the currently active view mode.
ViewMode toolbar_get_view_mode(void);

// Paint the toolbar onto the Finder's Cairo context.
// Draws the gradient background, view buttons, breadcrumb, and search field.
void toolbar_paint(FinderState *fs);

// Handle a mouse click within the toolbar area.
// x, y are in window coordinates (not toolbar-local).
// Returns true if the click was consumed (e.g., hit a button).
bool toolbar_handle_click(FinderState *fs, int x, int y);

#endif // AURA_TOOLBAR_H
