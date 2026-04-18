// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// panes/power.h — Energy Saver preferences pane
// ============================================================================
//
// Provides controls for power button behavior: short press and long press
// actions, plus timing thresholds for distinguishing between them.
//
// Changes are written to ~/.config/copycatos/input.conf [power] section
// and applied live by sending SIGHUP to inputd.
// ============================================================================

#ifndef CC_SYSPREFS_POWER_PANE_H
#define CC_SYSPREFS_POWER_PANE_H

#include "../sysprefs.h"

// Paint the Energy Saver preferences pane
void power_pane_paint(SysPrefsState *state);

// Handle mouse clicks in the Energy Saver pane. Returns true if consumed.
bool power_pane_click(SysPrefsState *state, int x, int y);

// Handle mouse motion for slider dragging. Returns true if needs repaint.
bool power_pane_motion(SysPrefsState *state, int x, int y);

// Handle mouse button release (commit slider value)
void power_pane_release(SysPrefsState *state);

#endif // CC_SYSPREFS_POWER_PANE_H
