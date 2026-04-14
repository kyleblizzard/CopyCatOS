// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// appmenu.h — Application-specific menu management
//
// This module handles two things:
//   1. Tracking which application is currently active by reading the
//      _NET_ACTIVE_WINDOW property from the root window and resolving
//      the window's WM_CLASS into a human-readable name.
//   2. Providing the correct set of menu titles and dropdown contents
//      for each known application. Unknown apps get a sensible default.
//
// Dropdown menus are rendered as override-redirect popup windows — they
// bypass the window manager entirely so they appear immediately without
// any title bar or decorations.

#ifndef AURA_APPMENU_H
#define AURA_APPMENU_H

#include "menubar.h"

// One-time initialization. Currently a no-op, but reserved for future
// use (e.g., loading menu definitions from config files).
void appmenu_init(MenuBar *mb);

// Read the currently active window from _NET_ACTIVE_WINDOW on the root,
// look up its WM_CLASS, and update mb->active_app and mb->active_class.
void appmenu_update_active(MenuBar *mb);

// Get the list of menu titles for a given app class (e.g., "dolphin").
// Sets *menus to point to a static array of strings and *count to the
// number of items. The returned pointers are valid for the lifetime of
// the program — do NOT free them.
void appmenu_get_menus(const char *app_class, const char ***menus, int *count);

// Show a dropdown menu below a specific menu title.
// Parameters:
//   mb         — menu bar state (needed for X display connection)
//   menu_index — which menu to show (0 = first menu title after app name)
//   x          — X pixel position where the dropdown should appear
void appmenu_show_dropdown(MenuBar *mb, int menu_index, int x);

// Dismiss any currently open dropdown menu. Destroys the popup window.
void appmenu_dismiss(MenuBar *mb);

// Returns the Window ID of the currently open dropdown, or None.
// Used by the event loop to route events to the dropdown.
Window appmenu_get_dropdown_win(void);

// Handle an X event that was delivered to the dropdown window.
// Returns true if the event was consumed, false otherwise.
// Sets *should_dismiss to true if the dropdown should be closed.
bool appmenu_handle_dropdown_event(MenuBar *mb, XEvent *ev, bool *should_dismiss);

// Clean up resources.
void appmenu_cleanup(void);

#endif // AURA_APPMENU_H
