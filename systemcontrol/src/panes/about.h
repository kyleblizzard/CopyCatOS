// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// panes/about.h — About MoonBase pane
// ============================================================================
//
// Shows the installed libmoonbase.so.1 runtime version. Mirrors the shape
// of macOS's "About This Mac" — big icon, framework name, version, and
// copyright line. Stays read-only; no interactive controls yet.
// ============================================================================

#ifndef CC_SYSPREFS_ABOUT_H
#define CC_SYSPREFS_ABOUT_H

#include "../sysprefs.h"

// Paint the About MoonBase pane — icon, title, runtime version, copyright
void about_paint(SysPrefsState *state, int pane_index);

#endif // CC_SYSPREFS_ABOUT_H
