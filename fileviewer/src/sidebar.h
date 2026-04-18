// CopyCatOS — by Kyle Blizzard at Blizzard.show

// sidebar.h — Snow Leopard Finder source list (sidebar)
//
// The sidebar is the left panel of the Finder window. It shows a list
// of common locations organized into sections, just like the real
// Snow Leopard Finder:
//
//   DEVICES
//     Macintosh HD   → navigates to /
//
//   PLACES
//     Home           → ~/
//     Desktop        → ~/Desktop
//     Applications   → /usr/share/applications
//     Documents      → ~/Documents
//     Downloads      → ~/Downloads
//     Music          → ~/Music
//     Pictures       → ~/Pictures
//
// Visual style (measured from real Snow Leopard):
//   - Background: #DDE4EA — RGB(221,228,234)
//   - Section headers: ALL CAPS, #8C8C8C, bold 9pt
//   - Items: Lucida Grande 11pt, #1A1A1A
//   - Selected item: gradient #5A96C8 → #386C9D with white text
//   - 1px separator line on the right edge (#A9A9A9)

#ifndef AURA_SIDEBAR_H
#define AURA_SIDEBAR_H

#include "finder.h"

// Initialize the sidebar — builds the list of sidebar items
// with their display names, target paths, and icons.
void sidebar_init(void);

// Paint the sidebar onto the Finder's Cairo context.
// Draws the blue-grey background, section headers, items, and
// selection highlight.
void sidebar_paint(FinderState *fs);

// Handle a mouse click within the sidebar area.
// x, y are in window coordinates (not sidebar-local).
// Returns true if a sidebar item was clicked (navigation happens
// automatically via finder_navigate).
bool sidebar_handle_click(FinderState *fs, int x, int y);

// Free any resources allocated by the sidebar (icon surfaces, etc.).
void sidebar_shutdown(void);

#endif // AURA_SIDEBAR_H
