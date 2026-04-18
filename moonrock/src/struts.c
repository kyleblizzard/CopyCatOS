// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// CopyCatOS Window Manager — Strut (reserved screen space) management
//
// "Struts" are strips of screen real estate that panels, docks, and
// menu bars reserve along screen edges.  The EWMH spec defines two
// properties for this:
//
//   _NET_WM_STRUT           — 4 longs: left, right, top, bottom
//   _NET_WM_STRUT_PARTIAL   — 12 longs: same 4 edges + start/end
//                              bounds for each edge strip
//
// We read both (preferring PARTIAL), take the maximum reservation on
// each edge across all windows, and publish the remaining rectangle
// as _NET_WORKAREA on the root window.  Other parts of the WM (like
// frame.c) use the cached work area to clamp window placement.

#include "struts.h"
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Module-level atoms that struts.c needs beyond what wm.h provides.
// We intern them once in struts_init() and keep them here so every
// function in this file can use them without passing extra arguments.
// ---------------------------------------------------------------------------
static Atom atom_net_workarea;
static Atom atom_net_desktop_geometry;
static Atom atom_net_number_of_desktops;
static Atom atom_net_current_desktop;

// ---------------------------------------------------------------------------
// Cached work area — updated by struts_recalculate(), read by the
// getter and clamp helpers.  Starts as the full screen (no struts).
// ---------------------------------------------------------------------------
static int wa_x = 0;
static int wa_y = 0;
static int wa_w = 0;
static int wa_h = 0;

// ===========================================================================
// struts_init  — one-time setup during WM startup
// ===========================================================================
void struts_init(CCWM *wm)
{
    // Intern the four EWMH atoms this module needs.
    // XInternAtom asks the X server to map a human-readable name
    // (like "_NET_WORKAREA") to a numeric Atom we can use in
    // property get/set calls.  The False argument means "create
    // the atom if it doesn't exist yet."
    atom_net_workarea            = XInternAtom(wm->dpy, "_NET_WORKAREA", False);
    atom_net_desktop_geometry    = XInternAtom(wm->dpy, "_NET_DESKTOP_GEOMETRY", False);
    atom_net_number_of_desktops  = XInternAtom(wm->dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    atom_net_current_desktop     = XInternAtom(wm->dpy, "_NET_CURRENT_DESKTOP", False);

    // --- _NET_NUMBER_OF_DESKTOPS ---
    // CopyCatOS uses a single desktop for now, so we advertise 1.
    long num_desktops = 1;
    XChangeProperty(wm->dpy, wm->root, atom_net_number_of_desktops,
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&num_desktops, 1);

    // --- _NET_CURRENT_DESKTOP ---
    // Index of the active desktop (0-based).
    long current_desktop = 0;
    XChangeProperty(wm->dpy, wm->root, atom_net_current_desktop,
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&current_desktop, 1);

    // --- _NET_DESKTOP_GEOMETRY ---
    // The full pixel size of the desktop (all monitors combined in a
    // single-head setup this is just root_w x root_h).
    long geometry[2] = { wm->root_w, wm->root_h };
    XChangeProperty(wm->dpy, wm->root, atom_net_desktop_geometry,
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)geometry, 2);

    // --- _NET_WORKAREA ---
    // Initially the entire screen is usable (no struts yet).
    wa_x = 0;
    wa_y = 0;
    wa_w = wm->root_w;
    wa_h = wm->root_h;

    long workarea[4] = { wa_x, wa_y, wa_w, wa_h };
    XChangeProperty(wm->dpy, wm->root, atom_net_workarea,
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)workarea, 4);

    fprintf(stderr, "[cc-wm] Struts initialised — workarea %dx%d+%d+%d\n",
            wa_w, wa_h, wa_x, wa_y);
}

// ===========================================================================
// struts_read_strut  — read strut data from a single window
// ===========================================================================
bool struts_read_strut(CCWM *wm, Window w, long strut[12])
{
    Atom type_ret;
    int fmt;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    // --- Try _NET_WM_STRUT_PARTIAL first (12 longs) ---
    // This is the preferred property because it includes start/end
    // bounds that let a panel say "I only cover part of an edge."
    if (XGetWindowProperty(wm->dpy, w, wm->atom_net_wm_strut_partial,
                           0, 12, False, XA_CARDINAL,
                           &type_ret, &fmt, &nitems, &bytes_after,
                           &data) == Success
        && data && nitems == 12)
    {
        // Copy all 12 values out of the X-allocated buffer.
        long *vals = (long *)data;
        for (int i = 0; i < 12; i++)
            strut[i] = vals[i];
        XFree(data);
        return true;
    }
    if (data) XFree(data);
    data = NULL;

    // --- Fall back to _NET_WM_STRUT (4 longs) ---
    // Older or simpler panels may only set the basic 4-value form.
    // We zero-fill the 8 extra entries so callers can always treat
    // the result as a 12-element array.
    if (XGetWindowProperty(wm->dpy, w, wm->atom_net_wm_strut,
                           0, 4, False, XA_CARDINAL,
                           &type_ret, &fmt, &nitems, &bytes_after,
                           &data) == Success
        && data && nitems == 4)
    {
        long *vals = (long *)data;
        strut[0] = vals[0];   // left
        strut[1] = vals[1];   // right
        strut[2] = vals[2];   // top
        strut[3] = vals[3];   // bottom
        // Zero the start/end fields since the basic property
        // doesn't specify them.
        for (int i = 4; i < 12; i++)
            strut[i] = 0;
        XFree(data);
        return true;
    }
    if (data) XFree(data);

    // No strut property found on this window.
    return false;
}

// ===========================================================================
// struts_recalculate  — scan all top-level windows and recompute
// ===========================================================================
void struts_recalculate(CCWM *wm)
{
    // Accumulators for the maximum reservation on each edge.
    // Multiple windows can set struts (e.g. a top menu bar AND a
    // bottom dock).  We take the largest value per edge.
    long max_left   = 0;
    long max_right  = 0;
    long max_top    = 0;
    long max_bottom = 0;

    // XQueryTree returns every direct child of the root window.
    // This includes BOTH managed (framed) clients AND unmanaged
    // windows like dock panels that the WM deliberately skips
    // framing.  We need to check them all because docks are the
    // primary source of strut properties.
    Window root_ret, parent_ret;
    Window *children = NULL;
    unsigned int nchildren = 0;

    if (!XQueryTree(wm->dpy, wm->root, &root_ret, &parent_ret,
                    &children, &nchildren))
    {
        fprintf(stderr, "[cc-wm] struts_recalculate: XQueryTree failed\n");
        return;
    }

    // Walk each child and look for strut properties.
    long strut[12];
    for (unsigned int i = 0; i < nchildren; i++) {
        if (struts_read_strut(wm, children[i], strut)) {
            // Take the maximum on each edge.
            if (strut[0] > max_left)   max_left   = strut[0];
            if (strut[1] > max_right)  max_right  = strut[1];
            if (strut[2] > max_top)    max_top    = strut[2];
            if (strut[3] > max_bottom) max_bottom = strut[3];
        }
    }

    // XQueryTree allocates the children array — we must free it.
    if (children)
        XFree(children);

    // Compute the usable rectangle after subtracting all struts.
    //
    //   +----root_w----+
    //   |  top strut   |
    //   |L+---------+R |
    //   |e| workarea |i |
    //   |f|         |g |
    //   |t+---------+h |
    //   | bottom strut |
    //   +--------------+
    wa_x = (int)max_left;
    wa_y = (int)max_top;
    wa_w = wm->root_w - (int)max_left - (int)max_right;
    wa_h = wm->root_h - (int)max_top  - (int)max_bottom;

    // Safety: clamp to sane minimums so a broken strut value
    // doesn't give us a negative or zero-sized work area.
    if (wa_w < 100) wa_w = 100;
    if (wa_h < 100) wa_h = 100;

    // Publish the new work area on the root window so EWMH-aware
    // clients (file managers, IDEs, etc.) can query it.
    // The spec says one set of 4 values per desktop; we have one desktop.
    long workarea[4] = { wa_x, wa_y, wa_w, wa_h };
    XChangeProperty(wm->dpy, wm->root, atom_net_workarea,
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)workarea, 4);

    fprintf(stderr, "[cc-wm] Workarea recalculated: %dx%d+%d+%d "
            "(struts L=%ld R=%ld T=%ld B=%ld)\n",
            wa_w, wa_h, wa_x, wa_y,
            max_left, max_right, max_top, max_bottom);
}

// ===========================================================================
// struts_get_workarea  — return the cached rectangle
// ===========================================================================
void struts_get_workarea(CCWM *wm, int *x, int *y, int *w, int *h)
{
    // Suppress unused-parameter warning — we read from the cache,
    // not from the wm struct, but keep the parameter for API
    // consistency with the rest of the codebase.
    (void)wm;

    if (x) *x = wa_x;
    if (y) *y = wa_y;
    if (w) *w = wa_w;
    if (h) *h = wa_h;
}

// ===========================================================================
// struts_clamp_to_workarea  — keep a window inside the usable area
// ===========================================================================
void struts_clamp_to_workarea(CCWM *wm, int *x, int *y, int w, int h)
{
    (void)wm;

    // How many pixels of the window must remain visible when it is
    // partially off the left or right edge of the work area.
    // This lets the user drag a wide window mostly off-screen while
    // still being able to grab it back.
    const int MIN_VISIBLE_X = 50;

    // --- Vertical clamping (strict) ---
    // The title bar must never go above the work area top edge.
    // This prevents the title bar from hiding behind a top strut
    // (like the menu bar), which would make the window impossible
    // to drag.
    if (*y < wa_y)
        *y = wa_y;

    // The bottom edge of the window must not extend past the work
    // area bottom.  If the window is taller than the work area we
    // still keep the top visible — the user can resize later.
    int wa_bottom = wa_y + wa_h;
    if (*y + h > wa_bottom) {
        *y = wa_bottom - h;
        // If the window is taller than the entire work area, pin
        // the top edge to the work area top so the title bar is
        // always reachable.
        if (*y < wa_y)
            *y = wa_y;
    }

    // --- Horizontal clamping (lenient) ---
    // Allow the window to hang off the sides, but keep at least
    // MIN_VISIBLE_X pixels inside the work area so the user can
    // still see and grab it.
    int wa_right = wa_x + wa_w;

    // If the window is too far to the right, pull it left until
    // at least MIN_VISIBLE_X pixels are inside the work area.
    if (*x + MIN_VISIBLE_X > wa_right)
        *x = wa_right - MIN_VISIBLE_X;

    // If the window is too far to the left, push it right until
    // at least MIN_VISIBLE_X pixels are inside the work area.
    if (*x + w - MIN_VISIBLE_X < wa_x)
        *x = wa_x - w + MIN_VISIBLE_X;
}
