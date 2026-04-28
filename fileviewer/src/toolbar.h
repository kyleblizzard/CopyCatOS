// CopyCatOS — by Kyle Blizzard at Blizzard.show

// toolbar.h — Snow Leopard Finder toolbar
//
// The toolbar sits at the top of the Finder window (inside the client
// area, below the WM title bar). It contains:
//
//   - View mode buttons: Icon, List, Column, Cover Flow
//   - Path breadcrumb: "Macintosh HD > Users > home > Documents"
//   - Search field: rounded rectangle on the right side
//
// The toolbar has a multi-stop gradient background measured from a real
// Snow Leopard Finder: 191 (top) → 154 (separator) → 170→168 (bottom).

#ifndef FILEVIEWER_TOOLBAR_H
#define FILEVIEWER_TOOLBAR_H

#include "finder.h"

// ViewMode is defined in finder.h (shared root header).

// Get the currently active view mode.
ViewMode toolbar_get_view_mode(void);

// Paint the toolbar onto the Finder's Cairo context.
// Draws the gradient background, view buttons, breadcrumb, and search field.
void toolbar_paint(FinderState *fs);

// Handle a mouse click within the toolbar area.
// x, y are in window coordinates (not toolbar-local).
// Returns true if the click was consumed (e.g., hit a button).
bool toolbar_handle_click(FinderState *fs, int x, int y);

#endif // FILEVIEWER_TOOLBAR_H
