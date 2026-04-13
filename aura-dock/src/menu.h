// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// menu.h — Right-click context menus
//
// When the user right-clicks a dock icon, a small popup menu appears with
// options like "Show In Finder" and "Quit" (if the app is running).
//
// The menu is an override-redirect window (meaning the window manager won't
// decorate or manage it — it just pops up exactly where we place it).
// It uses the same visual style as macOS context menus: light gray background,
// rounded corners, Lucida Grande font.
// ============================================================================

#ifndef MENU_H
#define MENU_H

#include "dock.h"

// Show a right-click context menu for the given dock item.
// The menu appears at the cursor's current position (above the dock).
//
// Parameters:
//   state — the dock's global state
//   item  — the dock item that was right-clicked
//   x, y  — screen coordinates where the menu should appear
void menu_show(DockState *state, DockItem *item, int x, int y);

// Close the context menu if it's currently visible.
// Called when the user clicks outside the menu or selects an option.
void menu_close(DockState *state);

// Handle events for the context menu (clicks on menu items, mouse motion
// for hover highlights, etc.). Returns true if the event was consumed
// by the menu (so the dock shouldn't also process it).
bool menu_handle_event(DockState *state, XEvent *ev);

#endif // MENU_H
