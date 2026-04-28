// CopyCatOS — by Kyle Blizzard at Blizzard.show

// appmenu.h — Application-specific menu management
//
// This module does three jobs:
//
//   1. Tracks which application is currently active per pane by
//      reading _MOONROCK_FRONTMOST_PER_OUTPUT (with _NET_ACTIVE_WINDOW
//      as fallback) and resolving WM_CLASS into a human-readable name
//      (pane->active_app / pane->active_class).
//
//   2. Owns a small set of built-in MenuNode trees for the apps we
//      know by class — Finder, Terminal, browser, sysprefs, and a
//      generic default. These are the fallback menus a native app
//      gets when it isn't a DBusMenu-exporting Legacy Mode client.
//
//   3. For Legacy Mode apps (GTK3 / Qt5 / Qt6-appmenu), reconciles a
//      DbusMenuClient pointed at the app's registered endpoint. The
//      imported MenuNode tree replaces the built-in fallback while
//      that window is active.
//
// Dropdowns are rendered as override-redirect popup windows with a
// bounded depth stack so submenus can drill into the DBusMenu tree
// (Qt AppMenu builds File → Recent Files → individual file lists).

#ifndef MENUBAR_APPMENU_H
#define MENUBAR_APPMENU_H

#include "menubar.h"
#include "menu_model.h"

// One-time initialization. Builds the per-class fallback MenuNode
// trees — freed in appmenu_cleanup.
void appmenu_init(MenuBar *mb);

// Refresh per-pane app tracking. For every pane, resolve the frontmost
// window of its host output (from _MOONROCK_FRONTMOST_PER_OUTPUT, indexed
// by pane row), look up WM_CLASS, write into pane->active_app/class, and
// reconcile pane->legacy_client against that window's AppMenu.Registrar
// registration. When MoonRock isn't running or the array is empty, every
// pane falls back to _NET_ACTIVE_WINDOW so the bar stays useful in
// single-display test sessions.
void appmenu_update_all_panes(MenuBar *mb);

// The MenuNode the given pane should render right now. Pane-scoped so
// two outputs hosting two different apps each draw their own menus.
//
//   - Pane has live DbusMenuClient + data arrived → imported root
//   - Pane has live DbusMenuClient + first-paint gap → NULL (pane
//     shows the app name only until the first GetLayout lands)
//   - No DbusMenuClient on this pane (native app) → built-in fallback
//     by pane->active_class
//
// The returned root's children are the top-level menu titles. Pointer
// is invalidated whenever this pane's DbusMenuClient refetches; callers
// don't cache across events.
const MenuNode *appmenu_root_for(MenuBar *mb, MenuBarPane *pane);

// Tear down a pane's app-tracking state before its dock window is
// destroyed. Frees pane->legacy_client and clears the cached layout
// pointer. Called by the reconciler for any pane that didn't survive
// a hotplug or mode toggle.
void appmenu_pane_destroyed(MenuBar *mb, MenuBarPane *pane);

// Show a dropdown menu below a specific top-level menu title. Coordinates
// are root-absolute — the caller translates the pane-local x of the menu
// title and the pane-local y of the bar bottom into virtual-root space
// using the host pane's (screen_x, screen_y) origin. This module treats
// every popup as a root-space override-redirect window; any pane-local
// math stays in menubar.c.
//
// Parameters:
//   mb         — menu bar state (X display, active app)
//   menu_index — 0-based index into the top-level menu children
//   root_x     — virtual-root X where the dropdown's left edge belongs
//   root_y     — virtual-root Y where the dropdown's top edge belongs
//                (the menu bar's bottom on the host output)
void appmenu_show_dropdown(MenuBar *mb, int menu_index,
                           int root_x, int root_y);

// Show the synthesized "Application menu" — the bold-app-name dropdown
// that holds About / Settings / Services / Hide / Hide Others / Show All
// / Quit. Anchored under the bold app-name region of mb->active_pane
// (caller sets active_pane right before this call, mirroring open_menu_at
// for the regular per-app titles). Rebuilds the pane's app_menu_root if
// it's stale relative to the current active_app, so the labels always
// embed the right name.
void appmenu_show_app_menu(MenuBar *mb, int root_x, int root_y);

// Dismiss every open dropdown in the submenu stack.
void appmenu_dismiss(MenuBar *mb);

// The topmost currently open dropdown (innermost submenu), or None.
// Menubar.c uses this for the "click outside the bar but inside the
// active popup" branch.
Window appmenu_get_dropdown_win(void);

// Find whichever level of the submenu stack contains (mx, my) in root
// coordinates. Returns true on hit and writes the owning Window plus
// popup-local (x, y). Used by menubar.c when the pointer is grabbed on
// root and events need to be routed to the right level.
bool appmenu_find_dropdown_at(MenuBar *mb, int mx, int my,
                              Window *out_win, int *out_lx, int *out_ly);

// Handle an X event the menubar loop determined belongs to some
// dropdown in the stack. Returns true if the event was consumed. Sets
// *should_dismiss to true only when the user did something that
// dismisses the whole stack (clicked a leaf item, clicked outside,
// Escape from level 0). Escape in a deeper submenu pops one level
// and leaves *should_dismiss false.
bool appmenu_handle_dropdown_event(MenuBar *mb, XEvent *ev,
                                   bool *should_dismiss);

// Pop the innermost submenu level (close one submenu). Returns true
// only if the stack had depth >= 2 and a level was popped; returns
// false when only the top-level dropdown is open, in which case the
// menubar loop should call appmenu_dismiss itself.
//
// Exists because KeyPress events fire on the focus window — which is
// the menubar window, not a dropdown — so the menubar loop is the
// only place Escape is visible. This function lets that path decide
// between "pop a submenu" and "dismiss the whole stack."
bool appmenu_pop_submenu_level(MenuBar *mb);

// Free the built-in MenuNode trees and drop any lingering DbusMenuClient.
void appmenu_cleanup(MenuBar *mb);

#endif // MENUBAR_APPMENU_H
