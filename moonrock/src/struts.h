// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

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

// Retrieve the cached work area computed by struts_recalculate().
// The rectangle is stored as (x, y, width, height).
void struts_get_workarea(CCWM *wm, int *x, int *y, int *w, int *h);

// Clamp a window's position so it stays within the work area.
//   - The title bar must not go above the top strut.
//   - The bottom edge must not go below the bottom strut.
//   - On left/right, at least 50 px of the window must stay visible.
void struts_clamp_to_workarea(CCWM *wm, int *x, int *y, int w, int h);

#endif // AURA_STRUTS_H
