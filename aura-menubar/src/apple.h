// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

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

// Draw the Apple logo in its designated region of the menu bar.
// Uses the selected variant if the Apple menu is open or hovered.
void apple_paint(MenuBar *mb, cairo_t *cr);

// Show the Apple dropdown menu. Creates an override-redirect popup
// at (0, MENUBAR_HEIGHT) with system actions.
void apple_show_menu(MenuBar *mb);

// Dismiss the Apple menu if it's currently open.
void apple_dismiss(MenuBar *mb);

// Free loaded PNG surfaces and other resources.
void apple_cleanup(void);

#endif // AURA_APPLE_H
