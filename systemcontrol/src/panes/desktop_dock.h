// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// panes/desktop_dock.h — Desktop & Dock preferences pane
// ============================================================================
//
// Two multi-display toggles with the exact macOS wording:
//   [ ] Displays have separate menu bars  → _COPYCATOS_MENUBAR_MODE
//   [ ] Displays have separate Spaces     → _COPYCATOS_SPACES_MODE
//
// Both default ON (Modern menu bars, per-display Spaces) and persist in
// ~/.config/copycatos/shell.conf. On toggle, the pane writes both the
// on-disk config and the matching root-window atom — no SIGHUP, no restart.
// ============================================================================

#ifndef CC_SYSPREFS_DESKTOP_DOCK_PANE_H
#define CC_SYSPREFS_DESKTOP_DOCK_PANE_H

#include "../sysprefs.h"

void desktop_dock_pane_paint(SysPrefsState *state);
bool desktop_dock_pane_click(SysPrefsState *state, int x, int y);
bool desktop_dock_pane_motion(SysPrefsState *state, int x, int y);
void desktop_dock_pane_release(SysPrefsState *state);

#endif // CC_SYSPREFS_DESKTOP_DOCK_PANE_H
