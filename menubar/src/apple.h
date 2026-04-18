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

#ifndef AURA_APPLE_H
#define AURA_APPLE_H

#include <cairo/cairo.h>
#include "menubar.h"

// Load Apple logo PNG assets. Falls back to a text glyph if files
// are not found. Call once during startup.
void apple_init(MenuBar *mb);

// Reload Apple logo at the current scale. Call after menubar_scale changes
// (e.g., on SIGHUP height change) so the logo renders at the right size.
void apple_reload(MenuBar *mb);

// Draw the Apple logo in its designated region of the menu bar.
// Uses the selected variant if the Apple menu is open or hovered.
void apple_paint(MenuBar *mb, cairo_t *cr);

// Show the Apple dropdown menu. Creates an override-redirect popup
// at (0, MENUBAR_HEIGHT) with system actions.
void apple_show_menu(MenuBar *mb);

// Dismiss the Apple menu if it's currently open.
void apple_dismiss(MenuBar *mb);

// Get the Apple popup window (for click routing in menubar.c)
Window apple_get_popup(void);

// Handle a click inside the Apple menu popup. Identifies which item
// was clicked from the Y coordinate, executes the action, and returns
// true if the menu should be dismissed.
bool apple_handle_click(MenuBar *mb, int click_x, int click_y);

// Handle hover inside the Apple menu popup. Updates highlight.
void apple_handle_motion(MenuBar *mb, int motion_y);

// Handle an X event directed at the Apple popup window.
// Returns true if the event was consumed.
bool apple_handle_event(MenuBar *mb, XEvent *ev, bool *should_dismiss);

// Free loaded PNG surfaces and other resources.
void apple_cleanup(void);

#endif // AURA_APPLE_H
