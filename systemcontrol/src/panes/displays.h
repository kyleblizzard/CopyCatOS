// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// panes/displays.h — Displays preferences pane
// ============================================================================
//
// v1-minimum: per-output HiDPI scale picker. Lists every connected XRandR
// output with its native resolution and physical size, and lets the user
// pick a scale from 0.5 to 3.0 in 0.25 steps.
//
// Writes through _MOONROCK_SET_OUTPUT_SCALE on the root window; MoonRock
// owns EDID hashing and on-disk persistence. The pane shows the effective
// scale MoonRock publishes on _MOONROCK_OUTPUT_SCALES, so anything MoonRock
// rejects (e.g. an out-of-range value) will simply not change the picker.
// ============================================================================

#ifndef CC_SYSPREFS_DISPLAYS_PANE_H
#define CC_SYSPREFS_DISPLAYS_PANE_H

#include "../sysprefs.h"

void displays_pane_paint(SysPrefsState *state);
bool displays_pane_click(SysPrefsState *state, int x, int y);
bool displays_pane_motion(SysPrefsState *state, int x, int y);
void displays_pane_release(SysPrefsState *state);

// Called when the user re-enters the Displays pane so the next paint
// re-enumerates outputs and reloads current scales. Skipping this would
// show stale data after a hotplug or an external scale change.
void displays_pane_mark_dirty(void);

#endif // CC_SYSPREFS_DISPLAYS_PANE_H
