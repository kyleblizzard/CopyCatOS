// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// toolbar.h — Unified toolbar rendering for System Preferences
// ============================================================================
//
// The toolbar sits at the top of the content area (below the WM title bar).
// It contains:
//   - "Show All" button (left side, always visible)
//   - Back/Forward arrows (left side, visible in pane view)
//   - Search field (right side, pill-shaped text input)
//
// The toolbar has a subtle gradient background that matches the Snow Leopard
// unified toolbar appearance.
// ============================================================================

#ifndef CC_SYSPREFS_TOOLBAR_H
#define CC_SYSPREFS_TOOLBAR_H

#include "sysprefs.h"

// Paint the toolbar area at the top of the window
void toolbar_paint(SysPrefsState *state);

// Handle a click inside the toolbar area. Returns true if the click was
// consumed (e.g. Show All button was pressed).
bool toolbar_handle_click(SysPrefsState *state, int x, int y);

// Update hover state for toolbar buttons. Returns true if hover changed
// (caller should repaint).
bool toolbar_handle_motion(SysPrefsState *state, int x, int y);

#endif // CC_SYSPREFS_TOOLBAR_H
