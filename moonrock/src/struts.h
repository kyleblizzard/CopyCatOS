// CopyCatOS — by Kyle Blizzard at Blizzard.show

// CopyCatOS Window Manager — Strut (reserved screen space) management
// Handles _NET_WM_STRUT and _NET_WM_STRUT_PARTIAL so that dock panels
// and menu bars can reserve edges of the screen. The "work area" is
// the remaining usable rectangle after all struts are subtracted.

#ifndef AURA_STRUTS_H
#define AURA_STRUTS_H

#include "wm.h"

// Initialize strut-related EWMH properties on the root window.
// Call once during WM startup, after atoms are interned.
void struts_init(CCWM *wm);

// Walk every top-level window (including unmanaged dock windows),
// collect strut reservations, and recompute the work area.
// Sets _NET_WORKAREA on the root so clients can query it.
void struts_recalculate(CCWM *wm);

// Read the strut values for a single window.
// Tries _NET_WM_STRUT_PARTIAL (12 longs) first, then falls back
// to _NET_WM_STRUT (4 longs, remaining 8 entries zeroed).
// Returns true if a strut was found and fills `strut[12]`.
bool struts_read_strut(CCWM *wm, Window w, long strut[12]);

// Retrieve the cached work area for the primary output.
// Used by callers that don't know which output they care about — for
// initial client placement and as the single _NET_WORKAREA rectangle.
void struts_get_workarea(CCWM *wm, int *x, int *y, int *w, int *h);

// Retrieve the cached work area for whichever output contains the
// point (px, py). Falls back to the primary output's workarea if the
// point sits outside every known output rectangle.
//
// Used by callers that have a specific window in mind — for example,
// the zoom handler wants the workarea of the output the window is
// currently sitting on, not the primary's.
void struts_get_workarea_at(CCWM *wm, int px, int py,
                            int *x, int *y, int *w, int *h);

// Register a thunk with the display module so struts_recalculate() runs
// automatically after every hotplug / primary-swap / rotation (anything
// that rewrites _MOONROCK_OUTPUT_SCALES). Without this the per-output
// workarea table goes stale across plug cycles and the clamp/zoom
// helpers pick the wrong output.
//
// Call once, after display_init() succeeds, alongside
// ewmh_register_focus_state_hook().
void struts_register_geometry_hook(CCWM *wm);

// Clamp a window's position so it stays within the work area of the
// output it is currently sitting on (picked by the window's midpoint).
// If the midpoint sits outside every output, falls back to the
// primary's workarea.
//   - The title bar must not go above that output's top strut.
//   - The bottom edge must not go below that output's bottom strut.
//   - On left/right, at least 50 px of the window must stay visible.
void struts_clamp_to_workarea(CCWM *wm, int *x, int *y, int w, int h);

#endif // AURA_STRUTS_H
