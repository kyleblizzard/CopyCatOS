// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

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
// Visual style:
//   - Background: #DFE5ED (the distinctive Snow Leopard blue-grey)
//   - Section headers: ALL CAPS, #8C8C8C, bold 11pt
//   - Items: Lucida Grande 13pt, #1A1A1A
//   - Selected item: #3875D7 fill with white text (Aqua highlight)
//   - 1px separator line on the right edge (#B8B8B8)

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
