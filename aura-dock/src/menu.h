// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// menu.h — Right-click context menus (Snow Leopard style)
//
// When the user right-clicks a dock icon, a popup menu appears that mirrors
// Mac OS X Snow Leopard's dock context menus. The menu varies depending on
// whether the item is an app or a folder:
//
// Apps get: header label, Options submenu (Keep in Dock, Open at Login,
// Show In Finder), Show In Finder, Remove from Dock, and Quit (if running).
//
// Folders get: header label, Sort By submenu, Display As submenu,
// Show In Finder, and Remove from Dock.
//
// Submenus slide out to the right when hovered. The menu is an
// override-redirect window (no window manager decoration).
// ============================================================================

#ifndef MENU_H
#define MENU_H

#include "dock.h"

// ---------------------------------------------------------------------------
// Menu item types — each row in the menu can be one of these kinds
// ---------------------------------------------------------------------------
typedef enum {
    MTYPE_NORMAL,       // Regular clickable text item
    MTYPE_SEPARATOR,    // Thin horizontal line divider
    MTYPE_SUBMENU,      // Has a "►" arrow; opens a child popup on hover
    MTYPE_HEADER,       // Bold, non-clickable label (app/folder name)
    MTYPE_CHECKBOX,     // Has a "✓" checkmark when checked
} MenuItemType;

// ---------------------------------------------------------------------------
// Action IDs — dispatched when the user clicks a menu item
// ---------------------------------------------------------------------------
#define ACTION_NONE              0
#define ACTION_SHOW_IN_FINDER    1
#define ACTION_QUIT              2
#define ACTION_REMOVE_FROM_DOCK  3
#define ACTION_KEEP_IN_DOCK      4
#define ACTION_OPEN_AT_LOGIN     5
// Folder sort modes (for future stacks feature)
#define ACTION_SORT_NAME         10
#define ACTION_SORT_DATE_ADDED   11
#define ACTION_SORT_DATE_MODIFIED 12
#define ACTION_SORT_DATE_CREATED 13
#define ACTION_SORT_KIND         14
// Folder display modes (for future stacks feature)
#define ACTION_DISPLAY_FAN       20
#define ACTION_DISPLAY_GRID      21
#define ACTION_DISPLAY_AUTO      22

// ---------------------------------------------------------------------------
// MenuItem — One row in the menu (or submenu)
//
// The menu is built dynamically based on whether the dock item is an app
// or a folder. Each row knows its label, type, and what action to fire.
// ---------------------------------------------------------------------------
typedef struct {
    char label[128];        // Text displayed in the menu row
    MenuItemType type;      // What kind of row this is (normal, separator, etc.)
    bool enabled;           // If false, the item is grayed out and not clickable
    bool checked;           // For MTYPE_CHECKBOX: whether the checkmark shows
    int action;             // Which ACTION_* constant to fire on click
} MenuItem;

// Show a right-click context menu for the given dock item.
// The menu appears above the dock at the specified screen coordinates.
//
// Parameters:
//   state — the dock's global state (needed for X11 resources)
//   item  — the dock item that was right-clicked
//   x, y  — screen coordinates where the menu should appear
void menu_show(DockState *state, DockItem *item, int x, int y);

// Close the context menu (and any open submenu) if currently visible.
// Called when the user clicks outside the menu or selects an action.
void menu_close(DockState *state);

// Handle X11 events for the context menu (mouse motion for hover,
// clicks on items, etc.). Returns true if the event was consumed
// by the menu, so the dock main loop shouldn't also process it.
bool menu_handle_event(DockState *state, XEvent *ev);

#endif // MENU_H
