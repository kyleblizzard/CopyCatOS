// CopyCatOS — by Kyle Blizzard at Blizzard.show

// appmenu.h — Application-specific menu management
//
// This module does three jobs:
//
//   1. Tracks which application is currently active by reading
//      _NET_ACTIVE_WINDOW from the root and resolving WM_CLASS into a
//      human-readable name (mb->active_app / mb->active_class).
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

#ifndef AURA_APPMENU_H
#define AURA_APPMENU_H

#include "menubar.h"
#include "menu_model.h"

// One-time initialization. Builds the per-class fallback MenuNode
// trees — freed in appmenu_cleanup.
void appmenu_init(MenuBar *mb);

// Read _NET_ACTIVE_WINDOW, resolve WM_CLASS → display name, and
// reconcile mb->legacy_client against the new active window's
// AppMenu.Registrar registration (create / replace / tear down as
// needed).
void appmenu_update_active(MenuBar *mb);

// The MenuNode the bar should render right now for the active app.
//
//   - Live DbusMenuClient + data arrived  → returns the imported root
//   - Live DbusMenuClient + first-paint gap → returns NULL (bar shows
//                                              app name only until the
//                                              first GetLayout lands)
//   - No DbusMenuClient (native app)      → returns built-in fallback
//                                              by mb->active_class
//
// The returned root's children are the top-level menu titles. Their
// children are the dropdown items. Pointer is invalidated whenever
// the DbusMenuClient refetches; callers don't cache across events.
const MenuNode *appmenu_root_for(MenuBar *mb);

// Show a dropdown menu below a specific top-level menu title.
// Parameters:
//   mb         — menu bar state (X display, active app)
//   menu_index — 0-based index into the top-level menu children
//   x          — X pixel position where the dropdown should appear
void appmenu_show_dropdown(MenuBar *mb, int menu_index, int x);

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

#endif // AURA_APPMENU_H
