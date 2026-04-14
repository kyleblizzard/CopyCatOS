// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// contextmenu.h — Right-click desktop context menu
//
// When the user right-clicks on empty desktop space, this module creates
// a popup menu rendered with Cairo. The menu is an override_redirect
// window (meaning the window manager won't put a frame around it).
//
// Menu items:
//   - New Folder: creates "untitled folder" in ~/Desktop
//   - Sort By: submenu with Name, Date Modified, Size, Kind
//   - Clean Up: relayout icons to canonical grid positions
//   - Change Desktop Background...: placeholder
//   - Open Terminal Here: launches konsole in ~/Desktop
//
// Visual style matches Mac OS X Snow Leopard context menus:
// light gray background, rounded corners, blue selection highlight.

#ifndef AURA_CONTEXTMENU_H
#define AURA_CONTEXTMENU_H

#include <X11/Xlib.h>
#include <stdbool.h>

// Show the context menu at the given root window coordinates.
// Creates a new override_redirect popup window and enters its own
// mini event loop to handle hover/click/dismiss.
//
// Returns the index of the selected item, or -1 if dismissed.
// The caller (desktop.c) uses the return value to trigger actions.
int contextmenu_show(Display *dpy, Window root, int root_x, int root_y,
                     int screen_w, int screen_h);

#endif // AURA_CONTEXTMENU_H
