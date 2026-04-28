// CopyCatOS — by Kyle Blizzard at Blizzard.show

// contextmenu.h — Right-click context menus (desktop and icon)
//
// Two context menus live here:
//
// 1. Desktop context menu (right-click on empty space):
//    - New Folder, Sort By, Clean Up, Change Desktop Background, Open Terminal
//
// 2. Icon context menu (right-click on a file/folder):
//    - Open, Get Info, Label ▶ (opens label picker), Move to Trash
//
// Both menus are override_redirect windows rendered with Cairo in the
// Snow Leopard style: light gray background, rounded corners, blue
// hover highlight, 8px drop shadow.
//
// Each menu function runs its own mini event loop until the user selects
// an item or clicks outside. The caller just checks the return value.

#ifndef DESKTOP_CONTEXTMENU_H
#define DESKTOP_CONTEXTMENU_H

#include <X11/Xlib.h>
#include <stdbool.h>
#include "icons.h"   // For DesktopIcon (used by contextmenu_show_icon)

// ── Desktop context menu ─────────────────────────────────────────────

// Show the desktop context menu at the given root window coordinates.
// Returns the internal item index of the selected item, or -1 if dismissed.
// The caller (desktop.c) matches against these indices:
//   0 = New Folder
//   3 = Clean Up
//   5 = Change Desktop Background...
//   7 = Open Terminal Here
int contextmenu_show(Display *dpy, Window root, int root_x, int root_y,
                     int screen_w, int screen_h);

// ── Icon context menu ────────────────────────────────────────────────

// Action codes returned by contextmenu_show_icon().
// Negative = dismissed, 0+ = action taken.
#define ICON_ACTION_NONE        (-1)   // Menu dismissed without selection
#define ICON_ACTION_OPEN          0    // "Open" — open the file
#define ICON_ACTION_INFO          1    // "Get Info" — placeholder
#define ICON_ACTION_TRASH         2    // "Move to Trash" — placeholder
// Label actions: ICON_ACTION_LABEL_BASE + label_index
// e.g. ICON_ACTION_LABEL_BASE + 0 = clear label
//      ICON_ACTION_LABEL_BASE + 1 = Red
//      ICON_ACTION_LABEL_BASE + 7 = Grey
#define ICON_ACTION_LABEL_BASE   10

// Show the icon context menu for the given icon.
// If the user picks "Label ▶", this function immediately shows a label
// picker popup and returns ICON_ACTION_LABEL_BASE + chosen_label.
// Otherwise returns one of the ICON_ACTION_* constants above.
int contextmenu_show_icon(Display *dpy, Window root,
                          int root_x, int root_y,
                          int screen_w, int screen_h,
                          DesktopIcon *icon);

#endif // DESKTOP_CONTEXTMENU_H
