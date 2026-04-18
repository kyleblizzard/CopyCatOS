// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// icongrid.h — Category-grouped icon grid for the main System Preferences view
// ============================================================================
//
// Renders the icon grid that appears when "Show All" is active. Each category
// (Personal, Hardware, Internet & Wireless, System) gets a gray header label
// with a separator line, followed by a row of 32x32 icons with centered text
// labels underneath.
// ============================================================================

#ifndef CC_SYSPREFS_ICONGRID_H
#define CC_SYSPREFS_ICONGRID_H

#include "sysprefs.h"

// Icon cell dimensions (matches real Snow Leopard layout)
#define CELL_WIDTH       80
#define CELL_HEIGHT      76
#define ICON_SIZE        32
#define GRID_LEFT_PAD    14
#define GRID_TOP_PAD      6
#define CATEGORY_GAP      8
#define HEADER_HEIGHT    20
#define SEPARATOR_GAP     3

// Paint the entire icon grid (all categories)
void icongrid_paint(SysPrefsState *state);

// Determine which pane the mouse is hovering over.
// Returns the pane index, or -1 if not over any icon.
int icongrid_hit_test(SysPrefsState *state, int x, int y);

// Handle a click inside the icon grid area. Returns true if a pane was clicked.
bool icongrid_handle_click(SysPrefsState *state, int x, int y);

// Update hover state based on mouse position. Returns true if hover changed.
bool icongrid_handle_motion(SysPrefsState *state, int x, int y);

#endif // CC_SYSPREFS_ICONGRID_H
