// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// panes/dock.h — Dock preferences pane
// ============================================================================
//
// Provides controls for dock icon size, matching the Snow Leopard layout:
//   - Size slider: Small (32) to Large (128)
//   - Menubar height slider: 22 to 44
//
// Changes are written to ~/.config/copycatos/desktop.conf and applied
// live by sending SIGHUP to dock and menubar.
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
