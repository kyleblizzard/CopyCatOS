// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// panes/appearance.h — Appearance preferences pane
// ============================================================================
//
// Holds the Modern/Classic menu-bar toggle today. Reserved for future
// Snow Leopard Appearance controls (highlight color, scroll-bar style, recent
// items, font smoothing) — add them here, not in dock.c, so the Dock pane
// stays focused on the Dock.
//
// The toggle writes the _COPYCATOS_MENUBAR_MODE root-window atom as
// XA_STRING — exactly "modern" or "classic", no trailing newline. menubar
// subscribes via PropertyNotify and reconciles its pane set.
// ============================================================================

#ifndef CC_SYSPREFS_APPEARANCE_PANE_H
#define CC_SYSPREFS_APPEARANCE_PANE_H

#include "../sysprefs.h"

void appearance_pane_paint(SysPrefsState *state);
bool appearance_pane_click(SysPrefsState *state, int x, int y);
bool appearance_pane_motion(SysPrefsState *state, int x, int y);
void appearance_pane_release(SysPrefsState *state);

#endif // CC_SYSPREFS_APPEARANCE_PANE_H
