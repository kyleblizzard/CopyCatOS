// CopyCatOS — by Kyle Blizzard at Blizzard.show

// apple.h — Apple logo button and Apple menu
//
// The Apple logo sits at the far left of the menu bar (at x=14).
// Clicking it opens the "Apple menu" — a dropdown with system-level
// actions like System Preferences, Sleep, Restart, Shut Down, and Log Out.
//
// The logo is loaded from a PNG file at:
//   $HOME/.local/share/aqua-widgets/menubar/apple_logo.png
// A selected/highlighted variant is loaded from:
//   $HOME/.local/share/aqua-widgets/menubar/apple_logo_selected.png
//
// If the PNG files are missing, a simple fallback glyph is drawn instead.

#ifndef MENUBAR_APPLE_H
#define MENUBAR_APPLE_H

#include <cairo/cairo.h>
#include "menubar.h"

// Load Apple logo PNG assets. Falls back to a text glyph if files
// are not found. Call once during startup.
void apple_init(MenuBar *mb);

// Reload Apple logo at the current scale. Call after menubar_scale changes
// (e.g., on SIGHUP height change) so the logo renders at the right size.
// `pane` is provided for symmetry with apple_paint and for future
// per-pane scale support in A.2.3 — the underlying logo cache is global
// today, shared across panes.
void apple_reload(MenuBar *mb, MenuBarPane *pane);

// Draw the Apple logo in `pane`'s dock window. Uses the selected variant
// if this pane hosts the open Apple menu or the logo is hovered.
void apple_paint(MenuBar *mb, MenuBarPane *pane, cairo_t *cr);

// Show the Apple dropdown menu. Creates an override-redirect popup
// at (0, MENUBAR_HEIGHT) with system actions.
void apple_show_menu(MenuBar *mb);

// Dismiss the Apple menu if it's currently open.
void apple_dismiss(MenuBar *mb);

// Get the Apple popup window (for click routing in menubar.c)
Window apple_get_popup(void);

// Get the Apple "Open Applications" submenu window, or None if the
// submenu is not currently open. The submenu is a cascading popup that
// auto-opens when the pointer hovers the parent row in the main popup
// and auto-closes when it leaves to a different parent row.
Window apple_get_submenu(void);

// Handle a click inside the Apple menu popup. Identifies which item
// was clicked from the Y coordinate, executes the action, and returns
// true if the menu should be dismissed.
bool apple_handle_click(MenuBar *mb, int click_x, int click_y);

// Handle a click inside the Open Applications submenu popup. Activates
// the running app via _NET_ACTIVE_WINDOW and returns true so the caller
// dismisses the entire Apple menu.
bool apple_handle_submenu_click(MenuBar *mb, int click_x, int click_y);

// Handle hover inside the Apple menu popup. Updates highlight, and
// auto-opens / auto-closes the Open Applications submenu based on
// which row is hovered.
void apple_handle_motion(MenuBar *mb, int motion_y);

// Handle hover inside the Open Applications submenu popup. Keeps the
// parent row highlighted in the main popup while updating the submenu's
// own hover row.
void apple_handle_submenu_motion(MenuBar *mb, int motion_y);

// Handle an X event directed at the Apple popup window.
// Returns true if the event was consumed.
bool apple_handle_event(MenuBar *mb, XEvent *ev, bool *should_dismiss);

// Free loaded PNG surfaces and other resources.
void apple_cleanup(void);

#endif // MENUBAR_APPLE_H
