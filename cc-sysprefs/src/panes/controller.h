// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// panes/controller.h — Controller / Mouse preferences pane
// ============================================================================
//
// Provides controls for gamepad stick sensitivity, deadzone, trigger
// assignments, and a read-only display of default button mappings.
//
// Changes are written to ~/.config/copycatos/input.conf and applied
// live by sending SIGHUP to the running cc-inputd process.
// ============================================================================

#ifndef CC_SYSPREFS_CONTROLLER_PANE_H
#define CC_SYSPREFS_CONTROLLER_PANE_H

#include "../sysprefs.h"

// Paint the Controller preferences pane
void controller_pane_paint(SysPrefsState *state);

// Handle mouse clicks in the Controller pane. Returns true if consumed.
bool controller_pane_click(SysPrefsState *state, int x, int y);

// Handle mouse motion for slider dragging. Returns true if needs repaint.
bool controller_pane_motion(SysPrefsState *state, int x, int y);

// Handle mouse button release (commit slider value)
void controller_pane_release(SysPrefsState *state);

#endif // CC_SYSPREFS_CONTROLLER_PANE_H
