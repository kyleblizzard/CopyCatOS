// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// paneview.h — Preference pane content rendering
// ============================================================================
//
// When a pane icon is clicked, the view switches from the icon grid to the
// pane content view. This module renders the currently selected pane.
// For Phase 1, all panes show a stub view (icon + "not yet available").
// ============================================================================

#ifndef CC_SYSPREFS_PANEVIEW_H
#define CC_SYSPREFS_PANEVIEW_H

#include "sysprefs.h"

// Paint the currently active pane's content area
void paneview_paint(SysPrefsState *state);

// Handle a click inside the pane view area
bool paneview_handle_click(SysPrefsState *state, int x, int y);

// Handle mouse motion inside the pane view (for slider dragging)
bool paneview_handle_motion(SysPrefsState *state, int x, int y);

// Handle mouse button release inside the pane view (commit slider values)
void paneview_handle_release(SysPrefsState *state);

#endif // CC_SYSPREFS_PANEVIEW_H
