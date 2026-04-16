// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// panes/dock.h — Dock preferences pane
// ============================================================================
//
// Provides controls for dock icon size, matching the Snow Leopard layout:
//   - Size slider: Small (32) to Large (128)
//   - Menubar height slider: 22 to 44
//
// Changes are written to ~/.config/copycatos/desktop.conf and applied
// live by sending SIGHUP to cc-dock and cc-menubar.
// ============================================================================

#ifndef CC_SYSPREFS_DOCK_PANE_H
#define CC_SYSPREFS_DOCK_PANE_H

#include "../sysprefs.h"

// Paint the Dock preferences pane
void dock_pane_paint(SysPrefsState *state);

// Handle mouse clicks in the Dock pane. Returns true if consumed.
bool dock_pane_click(SysPrefsState *state, int x, int y);

// Handle mouse motion for slider dragging. Returns true if needs repaint.
bool dock_pane_motion(SysPrefsState *state, int x, int y);

// Handle mouse button release (commit slider value)
void dock_pane_release(SysPrefsState *state);

#endif // CC_SYSPREFS_DOCK_PANE_H
