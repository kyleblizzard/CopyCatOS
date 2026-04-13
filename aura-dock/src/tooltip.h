// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// tooltip.h — Dock icon tooltip (help tag) interface
//
// When the user hovers over a dock icon for 500ms, a small tooltip window
// appears above the icon showing the app's name. This matches the "help tag"
// behavior from macOS Snow Leopard.
//
// Tooltips are passive — they don't grab the pointer like menus do. They
// simply appear on hover and disappear when the mouse moves away or clicks.
// ============================================================================

#ifndef DOCK_TOOLTIP_H
#define DOCK_TOOLTIP_H

#include "dock.h"

// Set up any one-time tooltip state. Call once during dock initialization.
void tooltip_init(void);

// Show a tooltip for the given dock icon index. Creates (or repositions)
// the tooltip window centered above that icon.
void tooltip_show(DockState *state, int icon_idx);

// Hide the tooltip and reset the hover timer. Call when the mouse leaves
// the dock or a button is pressed.
void tooltip_hide(DockState *state);

// Called on every mouse motion event. Tracks which icon the mouse is over
// and manages the 500ms delay before showing the tooltip.
//   hover_icon_idx: the icon index the mouse is over, or -1 if not on any icon
void tooltip_update_hover(DockState *state, int hover_icon_idx);

// Returns true if the tooltip window is currently visible on screen.
bool tooltip_is_visible(void);

// Destroy the tooltip window and free resources. Call during dock cleanup.
void tooltip_cleanup(DockState *state);

#endif // DOCK_TOOLTIP_H
