// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// panes/controller.h — Controller preferences pane (3-tab interface)
// ============================================================================
//
// Full gamepad configuration with three tabs:
//   - Desktop Mode: stick tuning, scroll speed, trigger threshold, mappings
//   - Desktop Gaming: passthrough toggle, per-game overrides
//   - Steam Mode: gamescope + Big Picture launcher
//
// Uses config_editor from inputmap to read/write input.conf and signals
// inputd via SIGHUP for live config reload.
// ============================================================================

#ifndef CC_SYSPREFS_CONTROLLER_PANE_H
#define CC_SYSPREFS_CONTROLLER_PANE_H

#include "../sysprefs.h"

// Paint the Controller preferences pane (including tab bar)
void controller_pane_paint(SysPrefsState *state);

// Handle mouse clicks in the Controller pane. Returns true if consumed.
bool controller_pane_click(SysPrefsState *state, int x, int y);

// Handle mouse motion for slider dragging. Returns true if needs repaint.
bool controller_pane_motion(SysPrefsState *state, int x, int y);

// Handle mouse button release (commit slider value to disk)
void controller_pane_release(SysPrefsState *state);

#endif // CC_SYSPREFS_CONTROLLER_PANE_H
