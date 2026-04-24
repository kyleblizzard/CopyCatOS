// CopyCatOS — by Kyle Blizzard at Blizzard.show

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
// Multi-output rule (MoonRock multi-monitor mandate): a strut on one
// output does not starve the work area of another output. A top strut
// whose horizontal range [top_start_x, top_end_x] falls entirely on
// the right-hand monitor must not shove windows on the left-hand
// monitor down by the same amount.  So we compute a *per-output*
// work area table here, and both the clamp helper and the zoom
// helper pick the right entry by window midpoint.
//
// For EWMH compliance we also publish a single _NET_WORKAREA on the
// root — the spec allows only one rect per desktop — and use the
// primary output's work area for that.  This is what mutter does.

#include "struts.h"
#include "moonrock_display.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>

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
// Per-output work-area cache.
//
// Each entry mirrors one MROutput and holds the usable rectangle on
// that output after this output's struts have been subtracted.  The
// output_* fields hold the raw output rect for hit-testing in the
// getter/clamp helpers — we cache it here so we don't have to ask
// the display module on every clamp call.
//
// Sized to a comfortable upper bound; any realistic setup will have
// ≤ 4 outputs.  Growing past MAX_OUTPUTS would just drop extras.
// ---------------------------------------------------------------------------
#define MAX_OUTPUTS 16

typedef struct {
    // Output rect in virtual-screen coordinates (raw, no struts).
    int output_x, output_y, output_w, output_h;
    // Usable work area on this output, after its struts are subtracted.
    int wa_x, wa_y, wa_w, wa_h;
    bool primary;
} OutputWorkarea;

static OutputWorkarea output_workareas[MAX_OUTPUTS];
static int            n_output_workareas = 0;
static int            primary_idx = 0;   // index into output_workareas[]

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
    // The full pixel size of the desktop (all monitors combined).
    long geometry[2] = { wm->root_w, wm->root_h };
    XChangeProperty(wm->dpy, wm->root, atom_net_desktop_geometry,
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)geometry, 2);

    // --- Seed the per-output workarea table ---
    // Initially every output's workarea is its full rect (no struts yet).
    // struts_recalculate() rebuilds this on every WM/structure change.
    n_output_workareas = 0;
    primary_idx = 0;
    int out_count = 0;
    MROutput *outs = display_get_outputs(&out_count);
    for (int i = 0; i < out_count && n_output_workareas < MAX_OUTPUTS; i++) {
        OutputWorkarea *ow = &output_workareas[n_output_workareas];
        ow->output_x = outs[i].x;
        ow->output_y = outs[i].y;
        ow->output_w = outs[i].width;
        ow->output_h = outs[i].height;
        ow->wa_x = outs[i].x;
        ow->wa_y = outs[i].y;
        ow->wa_w = outs[i].width;
        ow->wa_h = outs[i].height;
        ow->primary = outs[i].primary;
        if (ow->primary) primary_idx = n_output_workareas;
        n_output_workareas++;
    }

    // If display_init() hasn't run yet (or no outputs returned),
    // fall back to a single synthetic "output" covering the root.
    if (n_output_workareas == 0) {
        OutputWorkarea *ow = &output_workareas[0];
        ow->output_x = 0;
        ow->output_y = 0;
        ow->output_w = wm->root_w;
        ow->output_h = wm->root_h;
        ow->wa_x = 0;
        ow->wa_y = 0;
        ow->wa_w = wm->root_w;
        ow->wa_h = wm->root_h;
        ow->primary = true;
        n_output_workareas = 1;
        primary_idx = 0;
    }

    // --- _NET_WORKAREA ---
    // EWMH advertises one rectangle per desktop — publish primary's.
    OutputWorkarea *p = &output_workareas[primary_idx];
    long workarea[4] = { p->wa_x, p->wa_y, p->wa_w, p->wa_h };
    XChangeProperty(wm->dpy, wm->root, atom_net_workarea,
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)workarea, 4);

    fprintf(stderr, "[moonrock] Struts initialised — %d output(s), "
            "primary workarea %dx%d+%d+%d\n",
            n_output_workareas, p->wa_w, p->wa_h, p->wa_x, p->wa_y);
}

// ===========================================================================
// struts_read_strut  — read strut data from a single window
// ===========================================================================
//
// For _NET_WM_STRUT (the 4-value fallback) we fill the 8 bound fields
// with a sentinel meaning "covers the full edge."  The partial-edge
// math in struts_recalculate() treats {start=0, end=0} as "no coverage"
// so we can't leave those zeroed — a plain _NET_WM_STRUT dock that
// sets a top value would then be ignored per-output.  Using INT_MAX
// makes the overlap test always succeed on any output.
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
        // Basic form has no partial bounds — say "full edge" on every
        // axis so the per-output overlap test matches.
        strut[4]  = 0;          // left_start_y
        strut[5]  = INT_MAX;    // left_end_y
        strut[6]  = 0;          // right_start_y
        strut[7]  = INT_MAX;    // right_end_y
        strut[8]  = 0;          // top_start_x
        strut[9]  = INT_MAX;    // top_end_x
        strut[10] = 0;          // bottom_start_x
        strut[11] = INT_MAX;    // bottom_end_x
        XFree(data);
        return true;
    }
    if (data) XFree(data);

    // No strut property found on this window.
    return false;
}

// ---------------------------------------------------------------------------
// range_overlap — true if two closed intervals [a0,a1] and [b0,b1] share
// at least one coordinate.  Used to test whether a strut's partial bound
// (e.g. top_start_x..top_end_x) falls onto a particular output's edge.
// ---------------------------------------------------------------------------
static bool range_overlap(long a0, long a1, long b0, long b1)
{
    return a0 <= b1 && b0 <= a1;
}

// ===========================================================================
// struts_recalculate  — scan all top-level windows and recompute
// per-output work areas.
// ===========================================================================
void struts_recalculate(CCWM *wm)
{
    // --- Refresh the output table from the display module ---
    // Outputs can change shape on hotplug, resolution change, or
    // rotation.  Rebuild the cache so each recalculate starts from
    // fresh output rects.
    n_output_workareas = 0;
    primary_idx = 0;
    int out_count = 0;
    MROutput *outs = display_get_outputs(&out_count);
    for (int i = 0; i < out_count && n_output_workareas < MAX_OUTPUTS; i++) {
        OutputWorkarea *ow = &output_workareas[n_output_workareas];
        ow->output_x = outs[i].x;
        ow->output_y = outs[i].y;
        ow->output_w = outs[i].width;
        ow->output_h = outs[i].height;
        // Start each workarea at the full output rect; we'll shrink
        // per-edge below as we find struts that overlap.
        ow->wa_x = outs[i].x;
        ow->wa_y = outs[i].y;
        ow->wa_w = outs[i].width;
        ow->wa_h = outs[i].height;
        ow->primary = outs[i].primary;
        if (ow->primary) primary_idx = n_output_workareas;
        n_output_workareas++;
    }
    if (n_output_workareas == 0) {
        // Defensive fallback — should not happen on a healthy session.
        OutputWorkarea *ow = &output_workareas[0];
        ow->output_x = 0;
        ow->output_y = 0;
        ow->output_w = wm->root_w;
        ow->output_h = wm->root_h;
        ow->wa_x = 0;
        ow->wa_y = 0;
        ow->wa_w = wm->root_w;
        ow->wa_h = wm->root_h;
        ow->primary = true;
        n_output_workareas = 1;
        primary_idx = 0;
    }

    // --- Walk every top-level window and collect struts ---
    // XQueryTree returns every direct child of the root, including
    // unmanaged dock panels that the WM deliberately skips framing.
    // Docks are the primary source of strut properties.
    Window root_ret, parent_ret;
    Window *children = NULL;
    unsigned int nchildren = 0;
    if (!XQueryTree(wm->dpy, wm->root, &root_ret, &parent_ret,
                    &children, &nchildren))
    {
        fprintf(stderr, "[moonrock] struts_recalculate: XQueryTree failed\n");
        return;
    }

    // Virtual-screen extents — used to turn top/bottom/left/right
    // strut *values* (measured from the root's edge) into absolute
    // coordinates we can intersect with each output rect.
    const int rw = wm->root_w;
    const int rh = wm->root_h;

    // Max strut cut claimed on each edge, per output. Accumulating with
    // `max` across all windows (rather than subtracting each time) keeps
    // two panels on the same edge — e.g. a dock plus some other strut
    // claimant — from double-reducing the work area.
    long max_cut_top[MAX_OUTPUTS]    = {0};
    long max_cut_bot[MAX_OUTPUTS]    = {0};
    long max_cut_left[MAX_OUTPUTS]   = {0};
    long max_cut_right[MAX_OUTPUTS]  = {0};

    long strut[12];
    for (unsigned int i = 0; i < nchildren; i++) {
        if (!struts_read_strut(wm, children[i], strut))
            continue;

        const long sl = strut[0];           // left strut width
        const long sr = strut[1];           // right strut width
        const long st = strut[2];           // top strut height
        const long sb = strut[3];           // bottom strut height
        const long left_sy0  = strut[4];    // left edge vertical bounds
        const long left_sy1  = strut[5];
        const long right_sy0 = strut[6];    // right edge vertical bounds
        const long right_sy1 = strut[7];
        const long top_sx0   = strut[8];    // top edge horizontal bounds
        const long top_sx1   = strut[9];
        const long bot_sx0   = strut[10];   // bottom edge horizontal bounds
        const long bot_sx1   = strut[11];

        // For each output, compute how much this strut bites into
        // each edge of *this* output, and take the max against any
        // previous claim.
        for (int k = 0; k < n_output_workareas; k++) {
            OutputWorkarea *ow = &output_workareas[k];
            const int ox0 = ow->output_x;
            const int oy0 = ow->output_y;
            const int ox1 = ox0 + ow->output_w - 1;
            const int oy1 = oy0 + ow->output_h - 1;

            // ── Top ── strut's absolute Y extent: [0, st-1]
            if (st > 0 && range_overlap(top_sx0, top_sx1, ox0, ox1)) {
                long cut = st - oy0;
                if (cut > ow->output_h) cut = ow->output_h;
                if (cut > max_cut_top[k]) max_cut_top[k] = cut;
            }

            // ── Bottom ── strut's absolute Y extent: [rh - sb, rh - 1]
            if (sb > 0 && range_overlap(bot_sx0, bot_sx1, ox0, ox1)) {
                long cut = (oy0 + ow->output_h) - (rh - sb);
                if (cut > ow->output_h) cut = ow->output_h;
                if (cut > max_cut_bot[k]) max_cut_bot[k] = cut;
            }

            // ── Left ── strut's absolute X extent: [0, sl-1]
            if (sl > 0 && range_overlap(left_sy0, left_sy1, oy0, oy1)) {
                long cut = sl - ox0;
                if (cut > ow->output_w) cut = ow->output_w;
                if (cut > max_cut_left[k]) max_cut_left[k] = cut;
            }

            // ── Right ── strut's absolute X extent: [rw - sr, rw - 1]
            if (sr > 0 && range_overlap(right_sy0, right_sy1, oy0, oy1)) {
                long cut = (ox0 + ow->output_w) - (rw - sr);
                if (cut > ow->output_w) cut = ow->output_w;
                if (cut > max_cut_right[k]) max_cut_right[k] = cut;
            }
        }
    }
    if (children) XFree(children);

    // Apply the per-edge max cuts to each output's workarea exactly once.
    for (int k = 0; k < n_output_workareas; k++) {
        OutputWorkarea *ow = &output_workareas[k];
        long t = max_cut_top[k];
        long b = max_cut_bot[k];
        long l = max_cut_left[k];
        long r = max_cut_right[k];
        if (t > 0) { ow->wa_y += (int)t; ow->wa_h -= (int)t; }
        if (b > 0) {                      ow->wa_h -= (int)b; }
        if (l > 0) { ow->wa_x += (int)l; ow->wa_w -= (int)l; }
        if (r > 0) {                      ow->wa_w -= (int)r; }

        // Safety: clamp to sane minimums so a broken strut value
        // doesn't give us a negative or zero-sized work area.
        if (ow->wa_w < 100) ow->wa_w = 100;
        if (ow->wa_h < 100) ow->wa_h = 100;
    }

    // Publish primary's workarea on _NET_WORKAREA (one rect per desktop).
    OutputWorkarea *p = &output_workareas[primary_idx];
    long workarea[4] = { p->wa_x, p->wa_y, p->wa_w, p->wa_h };
    XChangeProperty(wm->dpy, wm->root, atom_net_workarea,
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)workarea, 4);

    fprintf(stderr, "[moonrock] Workareas recalculated — %d output(s):\n",
            n_output_workareas);
    for (int k = 0; k < n_output_workareas; k++) {
        OutputWorkarea *ow = &output_workareas[k];
        fprintf(stderr, "  [%d%s] output %dx%d+%d+%d  wa %dx%d+%d+%d\n",
                k, ow->primary ? " PRIMARY" : "",
                ow->output_w, ow->output_h, ow->output_x, ow->output_y,
                ow->wa_w, ow->wa_h, ow->wa_x, ow->wa_y);
    }
}

// ---------------------------------------------------------------------------
// Helper: find the OutputWorkarea whose raw output rect contains (px, py).
// Returns the primary entry if the point falls outside every output
// (hotplug edge cases, negative coords, etc.).
// ---------------------------------------------------------------------------
static OutputWorkarea *find_output_at(int px, int py)
{
    for (int k = 0; k < n_output_workareas; k++) {
        OutputWorkarea *ow = &output_workareas[k];
        if (px >= ow->output_x && px < ow->output_x + ow->output_w &&
            py >= ow->output_y && py < ow->output_y + ow->output_h) {
            return ow;
        }
    }
    return &output_workareas[primary_idx];
}

// ===========================================================================
// struts_get_workarea  — return the cached rectangle for the primary
// ===========================================================================
void struts_get_workarea(CCWM *wm, int *x, int *y, int *w, int *h)
{
    (void)wm;
    OutputWorkarea *ow = &output_workareas[primary_idx];
    if (x) *x = ow->wa_x;
    if (y) *y = ow->wa_y;
    if (w) *w = ow->wa_w;
    if (h) *h = ow->wa_h;
}

// ===========================================================================
// struts_get_workarea_at  — return the workarea for whichever output
// contains the point (px, py)
// ===========================================================================
void struts_get_workarea_at(CCWM *wm, int px, int py,
                            int *x, int *y, int *w, int *h)
{
    (void)wm;
    OutputWorkarea *ow = find_output_at(px, py);
    if (x) *x = ow->wa_x;
    if (y) *y = ow->wa_y;
    if (w) *w = ow->wa_w;
    if (h) *h = ow->wa_h;
}

// ===========================================================================
// struts_clamp_to_workarea  — keep a window inside the workarea of the
// output its midpoint is on
// ===========================================================================
void struts_clamp_to_workarea(CCWM *wm, int *x, int *y, int w, int h)
{
    (void)wm;

    // Pick the output by midpoint — this is the same "home output"
    // rule A.2.1 uses for keyboard focus, so clamp and focus agree.
    int mx = *x + w / 2;
    int my = *y + h / 2;
    OutputWorkarea *ow = find_output_at(mx, my);

    // How many pixels of the window must remain visible when it is
    // partially off the left or right edge of the work area.
    // Lets the user drag a wide window mostly off-screen while still
    // being able to grab it back.
    const int MIN_VISIBLE_X = 50;

    // --- Vertical clamping (strict) ---
    // Title bar must never go above the work-area top edge — otherwise
    // it can hide behind a top strut (the menu bar) and become
    // impossible to drag.
    if (*y < ow->wa_y)
        *y = ow->wa_y;

    // Bottom edge of the window must not extend past the work-area
    // bottom.  If the window is taller than the work area we still
    // keep the top visible — the user can resize later.
    int wa_bottom = ow->wa_y + ow->wa_h;
    if (*y + h > wa_bottom) {
        *y = wa_bottom - h;
        // If the window is taller than the entire work area, pin the
        // top edge to the work area top so the title bar is reachable.
        if (*y < ow->wa_y)
            *y = ow->wa_y;
    }

    // --- Horizontal clamping (lenient) ---
    // Allow the window to hang off the sides, but keep at least
    // MIN_VISIBLE_X pixels inside the work area so the user can
    // still see and grab it.
    int wa_right = ow->wa_x + ow->wa_w;

    // If the window is too far to the right, pull it left until at
    // least MIN_VISIBLE_X pixels are inside the work area.
    if (*x + MIN_VISIBLE_X > wa_right)
        *x = wa_right - MIN_VISIBLE_X;

    // If the window is too far to the left, push it right until at
    // least MIN_VISIBLE_X pixels are inside the work area.
    if (*x + w - MIN_VISIBLE_X < ow->wa_x)
        *x = ow->wa_x - w + MIN_VISIBLE_X;
}

// ===========================================================================
// Display-module hook — runs on every hotplug / primary swap / rotation.
//
// Without this, the cached per-output table goes stale after a plug cycle:
// the user plugs in a second monitor, display_init() rediscovers outputs,
// but struts_recalculate() is only wired to dock-map and strut PropertyNotify
// paths — neither fires on a bare RR event. The clamp and zoom helpers then
// pick an output by midpoint against the *old* table and drop windows into
// the wrong rect.
//
// We register a thunk with the display module's second hook slot so a fresh
// struts_recalculate() runs the moment display publishes a new scale table.
// The thunk is void(void), so we stash the WM in a file-static the same way
// ewmh_register_focus_state_hook() does.
// ===========================================================================
static CCWM *struts_hook_wm = NULL;

static void struts_geometry_thunk(void)
{
    if (struts_hook_wm)
        struts_recalculate(struts_hook_wm);
}

void struts_register_geometry_hook(CCWM *wm)
{
    struts_hook_wm = wm;
    display_set_geometry_changed_cb(struts_geometry_thunk);
}
