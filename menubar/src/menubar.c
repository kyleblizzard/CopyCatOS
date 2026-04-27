// CopyCatOS — by Kyle Blizzard at Blizzard.show

// menubar.c — Core menu bar lifecycle and event handling
//
// This is the heart of the menu bar. It manages:
//   - One or more X11 dock-type windows, one per pane, pinned to the
//     top edge of their host outputs
//   - The main event loop (mouse, expose, property changes)
//   - Layout computation per pane
//   - Coordination between all subsystems
//
// Each pane uses _NET_WM_WINDOW_TYPE_DOCK so the window manager knows
// to keep it always on top and not give it decorations. The _NET_WM_STRUT
// properties reserve screen space so other windows don't overlap the bar.
// The STRUT_PARTIAL horizontal range narrows the reservation to the
// hosting output only — other outputs stay unreserved at their top.
//
// Classic mode (A.2.2 baseline): pane_count == 1 on the primary output.
// Modern mode (A.2.3): pane_count == number of connected outputs.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sys/select.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#include "menubar.h"
#include "render.h"
#include "apple.h"
#include "appmenu.h"
#include "appmenu_bridge.h"
#include "systray.h"

// MoonRock -> shell scale bridge. MoonRock publishes per-output HiDPI scale
// to the root window; we subscribe so menubar knows the effective scale for
// its hosting output.
#include "moonrock_scale.h"

// Logical bar height in points, from ~/.config/copycatos/desktop.conf.
// Default 22 (Snow Leopard baseline), range 22–88 for handheld devices.
int menubar_height = DEFAULT_MENUBAR_HEIGHT;

// Per-output HiDPI scale from MoonRock. 1.0 until the bridge hands us
// a real value (or 1.0 forever if MoonRock isn't running).
float menubar_hidpi_scale = 1.0f;

// Combined points-to-physical-pixels factor. See menubar.h for the
// formula. Recomputed by recompute_menubar_scale() any time its two
// inputs (menubar_height, menubar_hidpi_scale) change.
double menubar_scale = 1.0;

// Live snapshot of every connected output's scale as published by MoonRock
// on _MOONROCK_OUTPUT_SCALES. Rewritten at startup and on every
// PropertyNotify for that atom. menubar_hidpi_scale is derived from this
// table for the point (0,0) — the origin of the bar's hosting output.
static MoonRockScaleTable g_output_scales;

// Visual/depth/colormap the pane windows are born with. Cached at
// menubar_init time so the reconciler can spawn new pane windows on
// hotplug without re-walking XGetVisualInfo. In practice every pane
// uses the same 32-bit ARGB visual on the default screen, so one
// cached triple is enough for all panes.
static Visual  *g_pane_visual   = NULL;
static int      g_pane_depth    = 0;
static Colormap g_pane_colormap = 0;

// Recompute the combined scale factor. Must run any time menubar_height
// (user config / SIGHUP) or menubar_hidpi_scale (MoonRock output scale)
// changes.
static void recompute_menubar_scale(void)
{
    menubar_scale =
        ((double)menubar_height / 22.0) * (double)menubar_hidpi_scale;
}

// Recompute pane->scale from menubar_height and pane->hidpi_scale. Mirrors
// recompute_menubar_scale() but per-pane. Called whenever the user's
// height preference changes or a pane's hidpi_scale is updated.
static void recompute_pane_scale(MenuBarPane *pane)
{
    pane->scale =
        ((double)menubar_height / 22.0) * (double)pane->hidpi_scale;
}

// Save+set the scale globals so S() / SF() resolve at `pane`'s scale until
// the matching leave_pane_scale call. Every pane-scoped routine — paint,
// resize, layout compute, hit-test, dropdown event dispatch — wraps its
// body with this pair so the macro reads stay correct even though two
// panes can run at different scales (1.0× external + 1.5× Legion panel).
//
// The returned struct carries the previous globals so leave can restore
// them — supports nested wraps, though nothing in the bar wraps deeper
// than one level today.
typedef struct {
    float  hidpi;
    double scale;
} PaneScaleSaved;

static PaneScaleSaved enter_pane_scale(const MenuBarPane *pane)
{
    PaneScaleSaved saved = { menubar_hidpi_scale, menubar_scale };
    if (pane && pane->hidpi_scale > 0.0f) {
        menubar_hidpi_scale = pane->hidpi_scale;
        menubar_scale       = pane->scale;
    }
    return saved;
}

static void leave_pane_scale(PaneScaleSaved saved)
{
    menubar_hidpi_scale = saved.hidpi;
    menubar_scale       = saved.scale;
}

// Pull the hosting-output scale from the cached MoonRock table, write it
// into menubar_hidpi_scale, and recompute menubar_scale. Returns the
// previous hidpi value so callers can detect change.
//
// In Classic mode the bar anchors to the primary output, so its scale is
// the primary's scale. A point-based lookup at (0,0) ambiguously matches
// every output that shares the origin — e.g. mirror layouts where eDP-1
// and DP-2 both sit at (0,0) — and would otherwise return whichever
// output walked first, not necessarily the primary.
//
// TODO A.2.3: Modern mode needs per-pane scale. This function stays as
// the Classic-mode single-scale derivation until the per-pane refactor
// lands.
static float apply_hidpi_scale_from_table(void)
{
    float old = menubar_hidpi_scale;
    float next = 1.0f;
    if (g_output_scales.valid) {
        const MoonRockOutputScale *p = moonrock_scale_primary(&g_output_scales);
        if (p && p->scale > 0.0f) {
            next = p->scale;
        } else {
            // No primary flag yet (pre-primary publisher) — fall back to
            // the origin scan so single-display boxes still work.
            next = moonrock_scale_for_point(&g_output_scales, 0, 0);
        }
    }
    menubar_hidpi_scale = next;
    recompute_menubar_scale();
    return old;
}

// Log a one-line summary of the current scale table. Keeps the bridge
// observable end-to-end without cluttering the main paint loop yet.
static void log_scale_table(const char *reason)
{
    if (!g_output_scales.valid || g_output_scales.count == 0) {
        fprintf(stderr,
                "[menubar] scale: %s — no MoonRock table yet "
                "(compositor may not be running)\n",
                reason);
        return;
    }
    // Host scale = primary output's scale (the bar anchors to primary).
    // Falls back to the (0,0) scan when no output carries a primary flag
    // yet — matches what apply_hidpi_scale_from_table() actually applies.
    const MoonRockOutputScale *pr = moonrock_scale_primary(&g_output_scales);
    float here = pr ? pr->scale
                    : moonrock_scale_for_point(&g_output_scales, 0, 0);
    fprintf(stderr, "[menubar] scale: %s — %d output(s); host=%.2f%s\n",
            reason, g_output_scales.count, (double)here,
            pr ? " (primary)" : "");
    for (int i = 0; i < g_output_scales.count; i++) {
        const MoonRockOutputScale *o = &g_output_scales.outputs[i];
        fprintf(stderr, "[menubar] scale:   %s %dx%d @ (%d,%d) scale=%.2f\n",
                o->name, o->width, o->height, o->x, o->y, (double)o->scale);
    }
}

// SIGHUP reload flag — set by signal handler, checked in event loop
static volatile bool reload_config = false;

static void sighup_handler(int sig)
{
    (void)sig;
    reload_config = true;
}

// Read [menubar] height from the shared config file
static void read_menubar_config(void)
{
    const char *home = getenv("HOME");
    if (!home) return;

    char path[512];
    snprintf(path, sizeof(path), "%s/.config/copycatos/desktop.conf", home);

    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[256];
    bool in_menubar = false;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') {
            in_menubar = (strncmp(p, "[menubar]", 9) == 0);
        } else if (in_menubar && strncmp(p, "height=", 7) == 0) {
            int h = atoi(p + 7);
            if (h >= 22 && h <= 88) {
                menubar_height = h;
            }
        }
    }
    fclose(fp);

    // Refresh the combined scale factor any time the user's height
    // preference changes. menubar_hidpi_scale is unaffected here — it
    // tracks the output, not the user config.
    recompute_menubar_scale();
}

// ── Forward declarations ────────────────────────────────────────────
static void dismiss_open_menu(MenuBar *mb);
static int  hit_test_menu(MenuBar *mb, MenuBarPane *pane, int mx);
static void grab_pointer(MenuBar *mb);
static void ungrab_pointer(MenuBar *mb);
static void paint_pane(MenuBar *mb, MenuBarPane *pane);
static void compute_pane_layout(MenuBarPane *pane);

// ── Pane lookup helpers ─────────────────────────────────────────────

MenuBarPane *mb_pane_for_window(MenuBar *mb, Window w)
{
    for (int i = 0; i < mb->pane_count; i++) {
        if (mb->panes[i].win == w) return &mb->panes[i];
    }
    return NULL;
}

MenuBarPane *mb_pane_for_point(MenuBar *mb, int rx, int ry)
{
    for (int i = 0; i < mb->pane_count; i++) {
        MenuBarPane *p = &mb->panes[i];
        if (rx >= p->screen_x && rx < p->screen_x + p->screen_w &&
            ry >= p->screen_y && ry < p->screen_y + p->screen_h) {
            return p;
        }
    }
    return NULL;
}

MenuBarPane *mb_primary_pane(MenuBar *mb)
{
    return (mb->pane_count >= 1) ? &mb->panes[0] : NULL;
}

// Write the standard dock-window properties onto a pane we just created:
// _NET_WM_WINDOW_TYPE_DOCK and the per-pane strut. Struts are narrowed to
// this pane's X range via top_start_x / top_end_x so only this pane's
// output has its top edge reserved. Called from create_pane_window and
// from apply_menubar_resize whenever the X range changes.
static void write_pane_properties(MenuBar *mb, MenuBarPane *pane, int h_px)
{
    Atom dock_type = mb->atom_net_wm_window_type_dock;
    XChangeProperty(mb->dpy, pane->win,
                    mb->atom_net_wm_window_type, XA_ATOM,
                    32, PropModeReplace,
                    (unsigned char *)&dock_type, 1);

    long strut_partial[12] = {0};
    strut_partial[2] = h_px;
    strut_partial[8] = pane->screen_x;
    strut_partial[9] = pane->screen_x + pane->screen_w - 1;
    XChangeProperty(mb->dpy, pane->win,
                    mb->atom_net_wm_strut_partial, XA_CARDINAL, 32,
                    PropModeReplace,
                    (unsigned char *)strut_partial, 12);
    long strut[4] = { 0, 0, h_px, 0 };
    XChangeProperty(mb->dpy, pane->win,
                    mb->atom_net_wm_strut, XA_CARDINAL, 32,
                    PropModeReplace,
                    (unsigned char *)strut, 4);
}

// Spawn the X dock window for a freshly-added pane. Uses the cached
// 32-bit ARGB visual so every pane has translucency on day one.
// Assumes pane->screen_{x,y,w,h}, pane->output_name and pane->scale are
// already populated by the reconciler.
static void create_pane_window(MenuBar *mb, MenuBarPane *pane)
{
    PaneScaleSaved _saved_scale = enter_pane_scale(pane);

    XSetWindowAttributes attrs;
    attrs.override_redirect = False;
    attrs.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask
                     | LeaveWindowMask | StructureNotifyMask | KeyPressMask;
    attrs.background_pixel = 0;
    attrs.colormap = g_pane_colormap;
    attrs.border_pixel = 0;

    int h_px = MENUBAR_HEIGHT;

    pane->win = XCreateWindow(
        mb->dpy, mb->root,
        pane->screen_x, pane->screen_y,
        (unsigned int)pane->screen_w,
        h_px,
        0, g_pane_depth, InputOutput, g_pane_visual,
        CWOverrideRedirect | CWEventMask | CWBackPixel
            | CWColormap | CWBorderPixel,
        &attrs);

    XSetWindowBackgroundPixmap(mb->dpy, pane->win, None);
    write_pane_properties(mb, pane, h_px);
    XMapWindow(mb->dpy, pane->win);

    leave_pane_scale(_saved_scale);
}

// Read _COPYCATOS_MENUBAR_MODE on the root window and decode it. Anything
// that isn't the exact byte sequence "classic" — missing atom, wrong type,
// unknown string — maps to Modern, the default. systemcontrol (A.2.4)
// writes this atom via XA_STRING; the define lives in menubar.h so the
// writer and this reader can't diverge on the atom name.
static MenuBarMode read_menubar_mode(MenuBar *mb)
{
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        mb->dpy, mb->root,
        mb->atom_copycatos_menubar_mode,
        0, 64, False, XA_STRING,
        &actual_type, &actual_format,
        &nitems, &bytes_after, &data);

    MenuBarMode mode = MENUBAR_MODE_MODERN;
    if (status == Success && actual_type == XA_STRING && data &&
        nitems == 7 && memcmp(data, "classic", 7) == 0) {
        mode = MENUBAR_MODE_CLASSIC;
    }
    if (data) XFree(data);
    return mode;
}

// Pull one MoonRock scale-table row into a MenuBarPane. Returns true if
// any geometry field, the output name, or the per-pane scale changed,
// so callers can skip a pointless resize / property rewrite when the
// row didn't actually move.
static bool populate_pane_from_row(MenuBarPane *pane,
                                   const MoonRockOutputScale *row)
{
    float row_scale = (row->scale > 0.0f) ? row->scale : 1.0f;
    bool changed = (pane->screen_x != row->x) ||
                   (pane->screen_y != row->y) ||
                   (pane->screen_w != row->width) ||
                   (pane->screen_h != row->height) ||
                   pane->hidpi_scale != row_scale ||
                   strncmp(pane->output_name, row->name,
                           sizeof(pane->output_name)) != 0;
    pane->screen_x    = row->x;
    pane->screen_y    = row->y;
    pane->screen_w    = row->width;
    pane->screen_h    = row->height;
    pane->hidpi_scale = row_scale;
    recompute_pane_scale(pane);
    strncpy(pane->output_name, row->name, sizeof(pane->output_name) - 1);
    pane->output_name[sizeof(pane->output_name) - 1] = '\0';
    return changed;
}

// Build the desired pane set from the current mode and scale table, then
// mutate mb->panes[] to match — preserving existing windows where the
// output_name matches so we don't churn panes on every hotplug (reduces
// the surface area of #76). New panes get fresh X windows; panes that
// no longer have a matching output are destroyed.
//
// Contract:
//   - Classic mode: pane_count = 1, panes[0] = primary output (or (0,0)
//     fallback when the table is empty).
//   - Modern mode: pane_count = scale-table row count, panes[i] matches
//     row i (so ordering is stable with respect to MoonRock's row order
//     and every shell component that reads the same table).
//   - Any open dropdown is dismissed BEFORE pane_count changes or
//     windows are destroyed. The dropdown's host pane may be going away,
//     so dismissing first is the only sane order.
//
// Returns true if the pane set or any pane's geometry changed, so the
// caller can skip apply_menubar_resize when nothing moved.
static bool reconcile_panes_to_outputs(MenuBar *mb)
{
    // 1. Dismiss any live dropdown before mutating the pane set — the
    //    host pane may be torn down in this pass.
    if (mb->open_menu >= 0) {
        dismiss_open_menu(mb);
    }

    // 2. Stage the desired pane set into `next[]`, keyed by output_name
    //    where possible so existing windows survive.
    MenuBarPane next[MENUBAR_MAX_PANES];
    memset(next, 0, sizeof(next));
    for (int i = 0; i < MENUBAR_MAX_PANES; i++) next[i].hover_index = -1;

    int next_count = 0;

    // Reset row→pane translation. Any row not assigned by the population
    // loop below stays at -1, which the active-output seeder reads as
    // "no pane for this row" (falls back to pane 0).
    int row_to_pane_local[MOONROCK_SCALE_MAX_OUTPUTS];
    for (int i = 0; i < MOONROCK_SCALE_MAX_OUTPUTS; i++)
        row_to_pane_local[i] = -1;

    if (mb->menubar_mode == MENUBAR_MODE_CLASSIC) {
        // Single pane on the primary output. Falls back to (0,0) with the
        // virtual-root width when MoonRock hasn't published a primary
        // yet — matches the A.2.2 pre-subscription behavior so a
        // pre-MoonRock X session still draws a bar.
        next_count = 1;
        const MoonRockOutputScale *p =
            moonrock_scale_primary(&g_output_scales);
        if (p) {
            populate_pane_from_row(&next[0], p);
        } else if (g_output_scales.valid && g_output_scales.count > 0) {
            populate_pane_from_row(&next[0], &g_output_scales.outputs[0]);
        } else {
            next[0].screen_x = 0;
            next[0].screen_y = 0;
            next[0].screen_w = DisplayWidth(mb->dpy, mb->screen);
            next[0].screen_h = DisplayHeight(mb->dpy, mb->screen);
            next[0].output_name[0] = '\0';
            // No MoonRock table yet — inherit the global hidpi scale so
            // the pane wrap macros resolve at the same value the macros
            // would read with no wrap. Avoids a silent zero-scale pane
            // that paints at 0 px tall.
            next[0].hidpi_scale = menubar_hidpi_scale;
            recompute_pane_scale(&next[0]);
        }
    } else {
        // Modern: one pane per UNIQUE output viewport. Mirror mode (two
        // outputs sharing the same (x,y) origin) collapses to one pane —
        // creating a pane per row in that case made the second pane paint
        // dimmed (non-focus alpha) on top of the first, greying the whole
        // bar on every physical display. The dedup is keyed by (x,y) only
        // because mirrored outputs always share their origin, even when
        // their pixel sizes differ (one feeds a smaller panel, the other a
        // larger external). Within a mirror group we prefer the primary
        // output's row so the pane carries the user-chosen primary's name
        // and width. row_to_pane is filled so the active-output seeder can
        // map a CARDINAL row index to the surviving pane.
        //
        // If the scale table is empty (MoonRock not running yet), fall
        // back to a single pane covering the virtual root so the bar is
        // still visible — matches Classic's empty-table path.
        if (g_output_scales.valid && g_output_scales.count > 0) {
            int row_count = g_output_scales.count;
            if (row_count > MOONROCK_SCALE_MAX_OUTPUTS)
                row_count = MOONROCK_SCALE_MAX_OUTPUTS;

            // Pass over rows twice: primary rows first (priority 1),
            // non-primary rows second (priority 0). On each row, find any
            // already-created pane at the same (x,y); if one exists, this
            // row joins that mirror group. Otherwise it gets a fresh pane.
            for (int pri = 1; pri >= 0; pri--) {
                for (int i = 0; i < row_count; i++) {
                    const MoonRockOutputScale *row =
                        &g_output_scales.outputs[i];
                    int row_pri = row->primary ? 1 : 0;
                    if (row_pri != pri) continue;
                    if (row_to_pane_local[i] >= 0) continue;

                    int merge_into = -1;
                    for (int p = 0; p < next_count; p++) {
                        if (next[p].screen_x == row->x &&
                            next[p].screen_y == row->y) {
                            merge_into = p;
                            break;
                        }
                    }
                    if (merge_into >= 0) {
                        row_to_pane_local[i] = merge_into;
                    } else if (next_count < MENUBAR_MAX_PANES) {
                        populate_pane_from_row(&next[next_count], row);
                        row_to_pane_local[i] = next_count;
                        next_count++;
                    }
                }
            }
        } else {
            next_count = 1;
            next[0].screen_x = 0;
            next[0].screen_y = 0;
            next[0].screen_w = DisplayWidth(mb->dpy, mb->screen);
            next[0].screen_h = DisplayHeight(mb->dpy, mb->screen);
            next[0].output_name[0] = '\0';
            next[0].hidpi_scale = menubar_hidpi_scale;
            recompute_pane_scale(&next[0]);
        }
    }

    // 3. Rescue matching windows from mb->panes[] by output_name.
    //    `claimed[]` tracks which old pane is already transplanted so we
    //    don't steal the same window twice when two new rows somehow
    //    share a name. Per-pane app-tracking state (active_app/class +
    //    legacy_*) rides along so an output that survives the reconcile
    //    keeps its current menus instead of flashing back to Finder.
    bool claimed[MENUBAR_MAX_PANES] = {0};
    for (int i = 0; i < next_count; i++) {
        if (next[i].output_name[0] == '\0') continue;
        for (int j = 0; j < mb->pane_count; j++) {
            if (claimed[j] || mb->panes[j].win == None) continue;
            if (strncmp(mb->panes[j].output_name, next[i].output_name,
                        sizeof(next[i].output_name)) == 0) {
                next[i].win         = mb->panes[j].win;
                next[i].hover_index = mb->panes[j].hover_index;
                memcpy(next[i].active_app, mb->panes[j].active_app,
                       sizeof(next[i].active_app));
                memcpy(next[i].active_class, mb->panes[j].active_class,
                       sizeof(next[i].active_class));
                next[i].legacy_client         = mb->panes[j].legacy_client;
                next[i].legacy_wid            = mb->panes[j].legacy_wid;
                next[i].legacy_is_loading     = mb->panes[j].legacy_is_loading;
                next[i].last_seen_legacy_root =
                    mb->panes[j].last_seen_legacy_root;
                // Null the old slot's client refs so the destroy pass
                // below doesn't free a client we just handed off.
                mb->panes[j].legacy_client         = NULL;
                mb->panes[j].legacy_wid            = None;
                mb->panes[j].legacy_is_loading     = false;
                mb->panes[j].last_seen_legacy_root = NULL;
                claimed[j] = true;
                break;
            }
        }
    }

    // 4. Destroy any old pane window that didn't get rescued. Each
    //    teardown frees the pane's legacy_client first so a registered
    //    app on a disappearing output doesn't leak its DbusMenuClient.
    for (int j = 0; j < mb->pane_count; j++) {
        if (!claimed[j] && mb->panes[j].win != None) {
            appmenu_pane_destroyed(mb, &mb->panes[j]);
            XDestroyWindow(mb->dpy, mb->panes[j].win);
            mb->panes[j].win = None;
        }
    }

    // 5. Commit the new array and create windows for any brand-new panes.
    bool changed = (next_count != mb->pane_count);
    for (int i = 0; i < next_count; i++) {
        if (!changed) {
            // Float `!=` is safe here: both sides are bit-identical copies
            // of the same MoonRock scale-table entry, never the result of
            // arithmetic. If a future change introduces arithmetic on this
            // field (e.g. a layout fudge), switch to an epsilon compare.
            if (next[i].win != mb->panes[i].win ||
                next[i].screen_x != mb->panes[i].screen_x ||
                next[i].screen_y != mb->panes[i].screen_y ||
                next[i].screen_w != mb->panes[i].screen_w ||
                next[i].screen_h != mb->panes[i].screen_h ||
                next[i].hidpi_scale != mb->panes[i].hidpi_scale ||
                strncmp(next[i].output_name, mb->panes[i].output_name,
                        sizeof(next[i].output_name)) != 0) {
                changed = true;
            }
        }
        mb->panes[i] = next[i];
    }
    for (int i = next_count; i < MENUBAR_MAX_PANES; i++) {
        memset(&mb->panes[i], 0, sizeof(mb->panes[i]));
        mb->panes[i].hover_index = -1;
    }
    mb->pane_count = next_count;

    for (int i = 0; i < mb->pane_count; i++) {
        if (mb->panes[i].win == None && g_pane_visual) {
            create_pane_window(mb, &mb->panes[i]);
            changed = true;
        }
        // Seed Finder defaults on any pane whose active_app didn't
        // ride along on the rescue path (brand-new pane, or a name-
        // changed output that fell out of the claim loop). The next
        // appmenu_update_all_panes pass will overwrite this with the
        // pane's real frontmost; this just prevents an empty-string
        // app name from painting in the window between reconcile and
        // the property fan-out.
        if (mb->panes[i].active_app[0] == '\0') {
            strncpy(mb->panes[i].active_app, "Finder",
                    sizeof(mb->panes[i].active_app) - 1);
            strncpy(mb->panes[i].active_class, "dolphin",
                    sizeof(mb->panes[i].active_class) - 1);
        }
    }

    if (changed) {
        fprintf(stderr, "[menubar] reconcile: mode=%s panes=%d\n",
                mb->menubar_mode == MENUBAR_MODE_CLASSIC ? "classic" : "modern",
                mb->pane_count);
        for (int i = 0; i < mb->pane_count; i++) {
            fprintf(stderr, "[menubar]   pane[%d] win=0x%lx %s %dx%d+%d+%d\n",
                    i, mb->panes[i].win,
                    mb->panes[i].output_name[0] ? mb->panes[i].output_name : "-",
                    mb->panes[i].screen_w, mb->panes[i].screen_h,
                    mb->panes[i].screen_x, mb->panes[i].screen_y);
        }
    }

    // 6. Keep focused_pane_idx inside [0, pane_count). If the old host
    //    pane was torn down, fall back to pane 0 — the full reseed
    //    happens below when the caller reads _MOONROCK_ACTIVE_OUTPUT.
    if (mb->focused_pane_idx < 0 ||
        mb->focused_pane_idx >= mb->pane_count) {
        mb->focused_pane_idx = (mb->pane_count > 0) ? 0 : -1;
    }

    // 7. Publish row→pane translation so the active-output seeder can
    //    remap a CARDINAL row index that was collapsed into a mirror
    //    group. Outside Modern mode every entry stays -1; the seeder
    //    has its own Classic-mode shortcut so the table is unused there.
    memcpy(mb->row_to_pane, row_to_pane_local, sizeof(mb->row_to_pane));

    return changed;
}

// Translate a _MOONROCK_ACTIVE_OUTPUT row index into a pane index. Returns
// the pane index in [0, pane_count) on success, or -1 when the row didn't
// resolve (no pane for that row, or a stale row index from a prior
// reconcile). Callers should treat -1 as "no focus pane" — the dim logic
// then leaves every pane bright (count > 1 + idx >= 0 guard fails).
static int row_to_pane_index(const MenuBar *mb, int row)
{
    if (row < 0 || row >= MOONROCK_SCALE_MAX_OUTPUTS) return -1;
    int pane = mb->row_to_pane[row];
    if (pane < 0 || pane >= mb->pane_count) return -1;
    return pane;
}

// Populate the pane-local layout anchors (apple_x/w, appname_x, menus_x).
// Called from init after the pane's screen rect is resolved, and from
// apply_menubar_resize after a scale change so every point constant
// tracks the current menubar_scale.
static void compute_pane_layout(MenuBarPane *pane)
{
    PaneScaleSaved _saved_scale = enter_pane_scale(pane);

    pane->apple_x   = 0;
    pane->apple_w   = S(50);
    pane->appname_x = S(58);
    pane->appname_w = 0;
    // menus_x is written every paint (depends on live app-name width),
    // but seed it so hit_test_menu before the first paint doesn't trip.
    pane->menus_x   = 0;

    leave_pane_scale(_saved_scale);
}

// Apply each pane's scale to its dock window: resize, rewrite struts,
// recompute scale-dependent layout regions, reload cached scale-sized
// assets (Apple logo), and repaint. Called after either a SIGHUP-driven
// height change, a MoonRock-driven scale-table refresh, or a
// Modern/Classic mode toggle. The strut writes are per-pane — each pane
// reserves only its own output's top edge.
//
// Modern mode lets two panes have different scales; the wrap inside the
// per-pane loop loads each pane's scale into the globals before
// MENUBAR_HEIGHT is read, so the bar is sized correctly per output.
static void apply_menubar_resize(MenuBar *mb)
{
    for (int i = 0; i < mb->pane_count; i++) {
        MenuBarPane *pane = &mb->panes[i];

        PaneScaleSaved _saved_scale = enter_pane_scale(pane);

        // S(22) — physical pixels on THIS pane's host output. Read inside
        // the wrap so a 1.5× pane and a 1.0× pane each get their own
        // correct height.
        int h_px = MENUBAR_HEIGHT;

        // Move+resize together so the window tracks primary-output changes
        // (hotplug reassigning primary, EDID-override rerunning) without a
        // one-frame flash at the old position.
        XMoveResizeWindow(mb->dpy, pane->win,
                          pane->screen_x, pane->screen_y,
                          pane->screen_w, h_px);

        // Struts are in virtual-root coordinates. `top=h_px` reserves the
        // top strip at the root's top edge; `top_start_x/top_end_x`
        // narrow that reservation to this pane's X range so windows on a
        // secondary can still use the full top of their own output.
        long strut_partial[12] = {0};
        strut_partial[2] = h_px;
        strut_partial[8] = pane->screen_x;
        strut_partial[9] = pane->screen_x + pane->screen_w - 1;
        XChangeProperty(mb->dpy, pane->win,
                        mb->atom_net_wm_strut_partial, XA_CARDINAL, 32,
                        PropModeReplace, (unsigned char *)strut_partial, 12);
        long strut[4] = { 0, 0, h_px, 0 };
        XChangeProperty(mb->dpy, pane->win,
                        mb->atom_net_wm_strut, XA_CARDINAL, 32,
                        PropModeReplace, (unsigned char *)strut, 4);

        // Recompute point-scaled layout anchors. compute_pane_layout
        // wraps internally for safety — nested wraps balance correctly,
        // and one extra save/restore per pane is cheap.
        compute_pane_layout(pane);

        // Apple logo is pre-rasterized in apple_init; rebuild at the new
        // scale for this pane. Other submodules (render, systray) draw
        // their scale-dependent content per-frame so they don't need a
        // reload. NOTE: apple's logo cache is global today, so the LAST
        // pane wins the cache. A future slice gives apple a per-pane
        // cache; until then, two panes at different scales will share
        // the last-painted scale's logo. The bar's geometry, text, and
        // dropdowns are already per-pane correct.
        apple_reload(mb, pane);

        leave_pane_scale(_saved_scale);
    }

    menubar_paint(mb);
}

// ── Initialization ──────────────────────────────────────────────────

bool menubar_init(MenuBar *mb)
{
    // Read menubar height from shared config before anything else
    read_menubar_config();
    fprintf(stderr, "[menubar] Config: height=%d\n", menubar_height);

    // Install SIGHUP handler for live config reload.
    // Use sigaction instead of signal() — on Linux, signal() may reset
    // the handler to SIG_DFL after the first delivery (System V semantics),
    // which would cause the second SIGHUP to terminate the process.
    // SA_RESTART ensures interrupted syscalls (like select()) resume.
    {
        struct sigaction sa;
        sa.sa_handler = sighup_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGHUP, &sa, NULL);
    }

    // Zero out the entire struct, then seed the invariant fields that
    // can't legitimately be 0.
    memset(mb, 0, sizeof(MenuBar));
    mb->open_menu   = -1;
    mb->active_pane = -1;
    mb->pane_count  = 0;
    for (int i = 0; i < MENUBAR_MAX_PANES; i++) {
        mb->panes[i].hover_index = -1;
    }

    // Connect to the X server. NULL means use the DISPLAY environment
    // variable, which is the standard way to find the X server.
    mb->dpy = XOpenDisplay(NULL);
    if (!mb->dpy) {
        fprintf(stderr, "menubar: cannot open X display\n");
        return false;
    }

    // Get basic screen info — we need the root window to watch for
    // active window changes and the default screen for visual lookup.
    mb->screen   = DefaultScreen(mb->dpy);
    mb->root     = RootWindow(mb->dpy, mb->screen);

    // ── Intern atoms ────────────────────────────────────────────
    mb->atom_net_active_window       = XInternAtom(mb->dpy, "_NET_ACTIVE_WINDOW", False);
    mb->atom_net_close_window        = XInternAtom(mb->dpy, "_NET_CLOSE_WINDOW", False);
    mb->atom_wm_change_state         = XInternAtom(mb->dpy, "WM_CHANGE_STATE",  False);
    mb->atom_net_wm_window_type      = XInternAtom(mb->dpy, "_NET_WM_WINDOW_TYPE", False);
    mb->atom_net_wm_window_type_dock = XInternAtom(mb->dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    mb->atom_net_wm_strut            = XInternAtom(mb->dpy, "_NET_WM_STRUT", False);
    mb->atom_net_wm_strut_partial    = XInternAtom(mb->dpy, "_NET_WM_STRUT_PARTIAL", False);
    mb->atom_wm_class                = XInternAtom(mb->dpy, "WM_CLASS", False);
    mb->atom_utf8_string             = XInternAtom(mb->dpy, "UTF8_STRING", False);
    mb->atom_copycatos_menubar_mode  = XInternAtom(mb->dpy,
        COPYCATOS_MENUBAR_MODE_ATOM_NAME, False);
    // Intern MoonRock's active-output + frontmost atoms so our
    // PropertyNotify handler can compare by atom identity without a
    // second round-trip per event. The helpers cache the interned atoms
    // internally, so calling them once here is enough.
    (void)moonrock_active_output_atom(mb->dpy);
    (void)moonrock_frontmost_per_output_atom(mb->dpy);

    // ── Find 32-bit ARGB visual for translucency ─────────────────
    Visual *visual = NULL;
    Colormap colormap = 0;
    int depth = CopyFromParent;
    XVisualInfo tpl;
    tpl.screen = mb->screen;
    tpl.depth = 32;
    tpl.class = TrueColor;
    int n_visuals = 0;
    XVisualInfo *vis_list = XGetVisualInfo(mb->dpy,
        VisualScreenMask | VisualDepthMask | VisualClassMask,
        &tpl, &n_visuals);
    for (int i = 0; i < n_visuals; i++) {
        if (vis_list[i].red_mask == 0x00FF0000 &&
            vis_list[i].green_mask == 0x0000FF00 &&
            vis_list[i].blue_mask == 0x000000FF) {
            visual = vis_list[i].visual;
            depth = 32;
            colormap = XCreateColormap(mb->dpy, mb->root, visual, AllocNone);
            break;
        }
    }
    if (vis_list) XFree(vis_list);

    if (!visual) {
        visual = DefaultVisual(mb->dpy, mb->screen);
        depth = DefaultDepth(mb->dpy, mb->screen);
        colormap = DefaultColormap(mb->dpy, mb->screen);
    }

    // Cache the chosen visual so reconcile_panes_to_outputs can spawn
    // fresh pane windows on hotplug without repeating the search.
    g_pane_visual   = visual;
    g_pane_depth    = depth;
    g_pane_colormap = colormap;

    // ── Subscribe to root window events + MoonRock scale bridge ─
    // Done BEFORE XCreateWindow so MENUBAR_HEIGHT already reflects the
    // host output's HiDPI scale when the window is born. Without this,
    // the window would be created at 1.0x and then resized on the first
    // PropertyNotify — visible flicker on startup.
    //
    // moonrock_scale_init's XSelectInput is additive (ORs in
    // PropertyChangeMask without clobbering existing bits), so the order
    // of init vs our own XSelectInput doesn't matter — but keeping them
    // together here makes the root subscription intent explicit.
    XSelectInput(mb->dpy, mb->root, PropertyChangeMask);
    moonrock_scale_init(mb->dpy);
    moonrock_scale_refresh(mb->dpy, &g_output_scales);
    apply_hidpi_scale_from_table();
    log_scale_table("init");

    // ── Resolve initial mode + reconcile pane set ───────────────
    // The reconciler reads mb->menubar_mode and the cached scale table,
    // materializes the desired pane set, and creates one dock window per
    // pane (via create_pane_window, which uses the g_pane_* visual cache
    // we just populated). Done BEFORE XMap so every pane is born on the
    // right output at the right width.
    mb->menubar_mode = read_menubar_mode(mb);
    (void)reconcile_panes_to_outputs(mb);

    // Seed focused_pane_idx from MoonRock's _MOONROCK_ACTIVE_OUTPUT. In
    // Classic mode there's only one pane, so it collapses to 0. In
    // Modern mode the row index is translated through row_to_pane so
    // mirror groups (multiple rows → one pane) collapse correctly. -1
    // from either lookup falls back to pane 0 so some pane always shows
    // undimmed menus.
    {
        int idx = moonrock_active_output_index(mb->dpy);
        int pane_idx = row_to_pane_index(mb, idx);
        if (mb->menubar_mode == MENUBAR_MODE_CLASSIC) {
            mb->focused_pane_idx = (mb->pane_count > 0) ? 0 : -1;
        } else if (pane_idx >= 0) {
            mb->focused_pane_idx = pane_idx;
        } else {
            mb->focused_pane_idx = (mb->pane_count > 0) ? 0 : -1;
        }
    }

    // ── Compute layout regions for every pane ───────────────────
    for (int i = 0; i < mb->pane_count; i++) {
        compute_pane_layout(&mb->panes[i]);
    }

    XFlush(mb->dpy);

    // ── Initialize subsystems ───────────────────────────────────
    render_init(mb);
    apple_init(mb);
    appmenu_init(mb);
    systray_init(mb);
    // DBusMenu / AppMenu.Registrar bridge (slice 18-A). Non-fatal on
    // failure: a KDE dev box where kwin already owns the name gets a
    // warning and a nil bridge; menubar still runs.
    (void)appmenu_bridge_init();

    // ── Set initial per-pane app state ───────────────────────────
    // Reconciler already seeded each pane's active_app/active_class to
    // the Finder default on creation. Now resolve the real frontmost
    // app per pane from _MOONROCK_FRONTMOST_PER_OUTPUT (or
    // _NET_ACTIVE_WINDOW when MoonRock isn't running) so the bar paints
    // correct app names on first frame instead of flashing "Finder" for
    // every pane.
    appmenu_update_all_panes(mb);

    mb->running = true;

    fprintf(stdout,
            "menubar: initialized (mode=%s, %d pane(s), focused=%d)\n",
            mb->menubar_mode == MENUBAR_MODE_CLASSIC ? "classic" : "modern",
            mb->pane_count, mb->focused_pane_idx);
    for (int i = 0; i < mb->pane_count; i++) {
        MenuBarPane *p = &mb->panes[i];
        fprintf(stdout, "menubar:   pane[%d] %s %dx%d @ (%d,%d)\n",
                i, p->output_name[0] ? p->output_name : "<unknown>",
                p->screen_w, p->screen_h, p->screen_x, p->screen_y);
    }

    return true;
}

// ── Pointer grab helpers ────────────────────────────────────────────
// We grab the pointer when a dropdown is open so we can detect clicks
// outside the menu bar to dismiss the dropdown. The grab is on the
// root window so we get events in screen (root) coordinates.

static void grab_pointer(MenuBar *mb)
{
    XGrabPointer(mb->dpy, mb->root, True,
                 ButtonPressMask | PointerMotionMask | ButtonReleaseMask,
                 GrabModeAsync, GrabModeAsync,
                 None, None, CurrentTime);
}

static void ungrab_pointer(MenuBar *mb)
{
    XUngrabPointer(mb->dpy, CurrentTime);
    XFlush(mb->dpy);
}

// ── Helper: dismiss whichever menu is currently open ────────────────

static void dismiss_open_menu(MenuBar *mb)
{
    // Run the dismiss under the active pane's scale so any S() / SF()
    // reads inside apple_dismiss / appmenu_dismiss (today: pure window
    // teardown, no scale reads, but the wrap is cheap insurance against
    // future paint-during-dismiss work).
    MenuBarPane *active = (mb->active_pane >= 0 &&
                           mb->active_pane < mb->pane_count)
                          ? &mb->panes[mb->active_pane]
                          : NULL;
    PaneScaleSaved _saved_scale = enter_pane_scale(active);

    if (mb->open_menu == 0) {
        apple_dismiss(mb);
    } else if (mb->open_menu > 0) {
        appmenu_dismiss(mb);
    }
    mb->open_menu   = -1;
    mb->active_pane = -1;
    ungrab_pointer(mb);

    leave_pane_scale(_saved_scale);
}

// Helper for the event loop: load the active dropdown's pane scale into
// the globals so handlers reading S() / SF() (apple/appmenu dropdown
// paint, hit-test, submenu spawn, ...) resolve at the scale the dropdown
// was opened under. Returns the saved globals; caller must
// leave_pane_scale on every exit. NULL active pane is allowed and
// results in a no-op enter that still balances correctly on leave.
static PaneScaleSaved enter_active_dropdown_scale(MenuBar *mb)
{
    MenuBarPane *active = (mb->active_pane >= 0 &&
                           mb->active_pane < mb->pane_count)
                          ? &mb->panes[mb->active_pane]
                          : NULL;
    return enter_pane_scale(active);
}

// ── Helper: figure out which menu title was clicked ─────────────────
// Returns: -1 = nothing, 0 = Apple logo, 1+ = menu title index.
// `mx` is pane-local (relative to pane->win left edge).

static int hit_test_menu(MenuBar *mb, MenuBarPane *pane, int mx)
{
    PaneScaleSaved _saved_scale = enter_pane_scale(pane);

    int result = -1;

    // Check Apple logo region
    if (mx >= pane->apple_x && mx < pane->apple_x + pane->apple_w) {
        result = 0;
        goto done;
    }

    // Check each menu title region. Walk the MenuNode tree — the root's
    // children are the top-level menu titles. A NULL root happens only
    // during the first-paint gap for a legacy-menu app (see
    // appmenu_root_for's contract); no menu titles to hit-test in that
    // window.
    const MenuNode *root = appmenu_root_for(mb, pane);
    int menu_count = root ? root->n_children : 0;

    int item_x = pane->menus_x;
    for (int i = 0; i < menu_count; i++) {
        const char *title = root->children[i]->label;
        if (!title) continue;
        double w = render_measure_text(title, false);
        int item_w = (int)w + S(20);
        if (mx >= item_x && mx < item_x + item_w) {
            result = i + 1;
            goto done;
        }
        item_x += item_w;
    }

done:
    leave_pane_scale(_saved_scale);
    return result;
}

// ── Helper: fire Ctrl+Space to toggle the searchsystem overlay ─────
// The searchsystem process grabs Ctrl+Space on the root window as a
// global hotkey. Synthesising the same chord via XTest reaches the
// grab handler the same way a physical keypress would, which keeps
// the menubar's spotlight click behavior in lockstep with the Ctrl+Space
// hotkey — no second IPC path to drift out of sync.
static void fire_spotlight(MenuBar *mb)
{
    int event_base, error_base, major, minor;
    if (!XTestQueryExtension(mb->dpy,
                             &event_base, &error_base, &major, &minor)) {
        fprintf(stderr, "[menubar] XTest not available — "
                        "spotlight click cannot activate searchsystem\n");
        return;
    }

    KeyCode ctrl  = XKeysymToKeycode(mb->dpy, XK_Control_L);
    KeyCode space = XKeysymToKeycode(mb->dpy, XK_space);
    if (ctrl == 0 || space == 0) return;

    XTestFakeKeyEvent(mb->dpy, ctrl,  True,  CurrentTime);
    XTestFakeKeyEvent(mb->dpy, space, True,  CurrentTime);
    XTestFakeKeyEvent(mb->dpy, space, False, CurrentTime);
    XTestFakeKeyEvent(mb->dpy, ctrl,  False, CurrentTime);
    XFlush(mb->dpy);
}

// ── Helper: open a specific menu by index ───────────────────────────
// index 0 = Apple, 1+ = app menus. `pane` is the pane that spawned the
// click; it becomes mb->active_pane for the lifetime of the dropdown.

static void open_menu_at(MenuBar *mb, MenuBarPane *pane, int index)
{
    mb->open_menu   = index;
    mb->active_pane = (int)(pane - mb->panes);

    PaneScaleSaved _saved_scale = enter_pane_scale(pane);

    if (index == 0) {
        // apple_show_menu reads mb->active_pane to anchor the popup in
        // root-absolute coordinates under the Apple logo of that pane.
        apple_show_menu(mb);
    } else {
        // Walk the MenuNode titles to reach the same X offset the
        // paint path used when it laid them out. `dx` is pane-local
        // here and gets folded into root coordinates below.
        const MenuNode *root = appmenu_root_for(mb, pane);
        int count = root ? root->n_children : 0;

        int dx = pane->menus_x;
        for (int j = 0; j < index - 1 && j < count; j++) {
            const char *t = root->children[j]->label;
            if (t) dx += (int)render_measure_text(t, false) + S(20);
        }
        // Dropdown windows live in virtual-root space — translate the
        // pane-local anchor into root coords before handing it over.
        int root_x = pane->screen_x + dx;
        int root_y = pane->screen_y + MENUBAR_HEIGHT;
        appmenu_show_dropdown(mb, index - 1, root_x, root_y);
    }

    leave_pane_scale(_saved_scale);

    menubar_paint(mb);
}

// ── Event Loop ──────────────────────────────────────────────────────

void menubar_run(MenuBar *mb)
{
    int x11_fd = ConnectionNumber(mb->dpy);
    time_t last_clock_check = 0;
    time_t last_systray_update = 0;

    // Heartbeat watchdog — task #76 instrumentation. Stamps the wall
    // clock every time XNextEvent returns so the periodic block can
    // print "seconds since last X event" alongside live mode/pane state.
    // The delta is the discriminator: a growing age while heartbeats
    // keep firing means the X queue is dry (root cause upstream); a
    // frozen heartbeat means the menubar loop itself is wedged.
    time_t last_x_event_at = time(NULL);
    time_t last_heartbeat_at = 0;

    while (mb->running) {
        // ── Handle all pending X events ─────────────────────────
        while (XPending(mb->dpy)) {
            XEvent ev;
            XNextEvent(mb->dpy, &ev);
            last_x_event_at = time(NULL);

            // ── Route events to the dropdown if it's open ───────
            // Dropdown handlers read S() / SF() for row layout, hit-test,
            // and submenu spawn — load the host pane's scale into the
            // globals so a 1.5× dropdown doesn't get mis-measured against
            // a 1.0× active pane (or vice versa).
            if (mb->open_menu > 0) {
                bool should_dismiss = false;
                PaneScaleSaved _saved = enter_active_dropdown_scale(mb);
                bool consumed =
                    appmenu_handle_dropdown_event(mb, &ev, &should_dismiss);
                leave_pane_scale(_saved);
                if (consumed) {
                    if (should_dismiss) {
                        dismiss_open_menu(mb);
                        menubar_paint(mb);
                    }
                    continue;
                }
            }

            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) {
                    MenuBarPane *pane = mb_pane_for_window(mb, ev.xexpose.window);
                    if (pane) paint_pane(mb, pane);
                }
                break;

            case MotionNotify: {
                // When a menu is open, the pointer is grabbed on root,
                // so we get root coordinates. Convert to pane-local.
                int mx, my;
                MenuBarPane *pane;
                if (mb->open_menu >= 0) {
                    // Grabbed on root — coordinates are screen/root coords.
                    // Recover the pane from the pointer position.
                    int rx = ev.xmotion.x_root;
                    int ry = ev.xmotion.y_root;
                    pane = mb_pane_for_point(mb, rx, ry);
                    if (!pane) {
                        // Pointer outside every pane's output — forward
                        // to the dropdown below as usual via the "mouse
                        // below the bar" branch, anchored on the pane
                        // that hosts the open dropdown.
                        pane = (mb->active_pane >= 0)
                               ? &mb->panes[mb->active_pane]
                               : mb_primary_pane(mb);
                        if (!pane) break;
                    }
                    mx = rx - pane->screen_x;
                    my = ry - pane->screen_y;
                } else {
                    // Not grabbed — coordinates are relative to the pane's
                    // own window, and ev.xmotion.window identifies it.
                    pane = mb_pane_for_window(mb, ev.xmotion.window);
                    if (!pane) break;
                    mx = ev.xmotion.x;
                    my = ev.xmotion.y;
                }

                // Wrap the rest of the case in the resolved pane's scale
                // so MENUBAR_HEIGHT, hit_test_menu, and any S() / SF()
                // forwarded into apple_/appmenu_ dropdown handlers all
                // measure against the host output's effective scale.
                PaneScaleSaved _saved_scale = enter_pane_scale(pane);

                // Mouse below the bar — might be in the dropdown popup.
                // Route hover events to the active dropdown for highlight.
                if (my < 0 || my >= MENUBAR_HEIGHT) {
                    if (pane->hover_index != -1) {
                        pane->hover_index = -1;
                        paint_pane(mb, pane);
                    }

                    // Forward hover to the active dropdown if mouse is
                    // inside it. Apple menu is one-deep and uses its
                    // own helper; app menus have a multi-level submenu
                    // stack, so we ask appmenu which level the pointer
                    // is in and forward the synthetic motion to that
                    // window. Coordinates fed to the dropdown are root
                    // coords — the popup windows themselves live in
                    // root space.
                    int rx = ev.xmotion.x_root;
                    int ry = ev.xmotion.y_root;
                    if (mb->open_menu == 0) {
                        Window dropdown = apple_get_popup();
                        if (dropdown != None) {
                            XWindowAttributes dwa;
                            XGetWindowAttributes(mb->dpy, dropdown, &dwa);
                            if (rx >= dwa.x && rx < dwa.x + dwa.width &&
                                ry >= dwa.y && ry < dwa.y + dwa.height) {
                                apple_handle_motion(mb, ry - dwa.y);
                            } else {
                                apple_handle_motion(mb, -999);
                            }
                        }
                    } else if (mb->open_menu > 0) {
                        Window hit; int lx, ly;
                        if (appmenu_find_dropdown_at(mb, rx, ry, &hit, &lx, &ly)) {
                            XEvent synth = ev;
                            synth.xmotion.window = hit;
                            synth.xmotion.x = lx;
                            synth.xmotion.y = ly;
                            XSendEvent(mb->dpy, hit, True,
                                       PointerMotionMask, &synth);
                        }
                    }
                    leave_pane_scale(_saved_scale);
                    break;
                }

                int old_hover = pane->hover_index;
                int new_hover = hit_test_menu(mb, pane, mx);

                if (new_hover != old_hover) {
                    pane->hover_index = new_hover;

                    // If a menu is already open and we hover a different
                    // title, switch to that menu (menu bar scrubbing).
                    // Only allowed inside the same pane — crossing to a
                    // different pane's bar doesn't trigger scrub in
                    // Classic mode (single pane, can't happen).
                    if (mb->open_menu >= 0 && new_hover >= 0 &&
                        new_hover != mb->open_menu &&
                        mb->active_pane == (int)(pane - mb->panes)) {
                        // Dismiss the old dropdown (keep the grab!)
                        if (mb->open_menu == 0) {
                            apple_dismiss(mb);
                        } else {
                            appmenu_dismiss(mb);
                        }

                        // Open the new one (without re-grabbing)
                        open_menu_at(mb, pane, new_hover);
                    }

                    paint_pane(mb, pane);
                }
                leave_pane_scale(_saved_scale);
                break;
            }

            case ButtonPress: {
                // When pointer is grabbed on root, ButtonPress coords
                // are in root/screen coordinates.
                int mx, my;
                int rx = ev.xbutton.x_root;
                int ry = ev.xbutton.y_root;
                MenuBarPane *pane;

                if (mb->open_menu >= 0) {
                    pane = mb_pane_for_point(mb, rx, ry);
                    if (pane) {
                        mx = rx - pane->screen_x;
                        my = ry - pane->screen_y;
                    } else {
                        // Click outside every pane's output — treat as
                        // "clicked somewhere far from the bar." Fall
                        // through to the out-of-bar dismiss path.
                        mx = 0;
                        my = -1;
                    }
                } else {
                    pane = mb_pane_for_window(mb, ev.xbutton.window);
                    if (!pane) break;
                    mx = ev.xbutton.x;
                    my = ev.xbutton.y;
                }

                // Wrap the rest of the case in the resolved pane's scale
                // so MENUBAR_HEIGHT, hit_test_menu, and any S() / SF() in
                // forwarded apple_/appmenu_ click handlers all measure
                // against the host output's effective scale. NULL pane
                // (open_menu set, click off every output) is fine —
                // enter_pane_scale handles it as a no-op save.
                PaneScaleSaved _saved_scale = enter_pane_scale(pane);

                if (mb->open_menu >= 0) {
                    // A menu is currently open.

                    // Check if click is within the menu bar of the pane
                    // that currently hosts the dropdown. Clicking on a
                    // different pane's bar is treated as an out-of-bar
                    // click — the dropdown dismisses, and a re-click on
                    // that pane will open its own menu next.
                    bool in_active_bar =
                        pane && my >= 0 && my < MENUBAR_HEIGHT &&
                        mb->active_pane == (int)(pane - mb->panes);

                    if (in_active_bar) {
                        int clicked = hit_test_menu(mb, pane, mx);

                        if (clicked == mb->open_menu) {
                            // Clicked the same menu title — toggle closed
                            dismiss_open_menu(mb);
                            menubar_paint(mb);
                        } else if (clicked >= 0) {
                            // Clicked a different menu title — switch to it.
                            // Dismiss old dropdown but keep the pointer grab.
                            if (mb->open_menu == 0) {
                                apple_dismiss(mb);
                            } else {
                                appmenu_dismiss(mb);
                            }
                            open_menu_at(mb, pane, clicked);
                        } else {
                            // Clicked empty space in the menu bar — dismiss
                            dismiss_open_menu(mb);
                            menubar_paint(mb);
                        }
                    } else {
                        // Clicked outside the active pane's bar entirely.
                        // Apple menu is a single popup; app menus have a
                        // submenu stack, so ask appmenu which level (if
                        // any) contains the point. Popup coordinates are
                        // root-absolute.
                        bool handled = false;

                        if (mb->open_menu == 0) {
                            Window dropdown = apple_get_popup();
                            if (dropdown != None) {
                                XWindowAttributes dwa;
                                XGetWindowAttributes(mb->dpy, dropdown, &dwa);
                                if (rx >= dwa.x && rx < dwa.x + dwa.width &&
                                    ry >= dwa.y && ry < dwa.y + dwa.height) {
                                    if (apple_handle_click(mb,
                                                           rx - dwa.x,
                                                           ry - dwa.y)) {
                                        dismiss_open_menu(mb);
                                        menubar_paint(mb);
                                    }
                                    handled = true;
                                }
                            }
                        } else if (mb->open_menu > 0) {
                            Window hit; int lx, ly;
                            if (appmenu_find_dropdown_at(mb, rx, ry,
                                                         &hit, &lx, &ly)) {
                                XEvent synth = ev;
                                synth.xbutton.window = hit;
                                synth.xbutton.x = lx;
                                synth.xbutton.y = ly;
                                XSendEvent(mb->dpy, hit, True,
                                           ButtonPressMask, &synth);
                                XFlush(mb->dpy);
                                handled = true;
                            }
                        }

                        if (!handled) {
                            // Truly outside every popup — dismiss.
                            dismiss_open_menu(mb);
                            menubar_paint(mb);
                        }
                    }
                    leave_pane_scale(_saved_scale);
                    break;
                }

                // No menu is open — check Spotlight glyph first
                // (rightmost systray item), then fall back to menu-title
                // hit-test. The glyph's hit rect is the full menubar
                // height so a click anywhere in its column activates.
                // Spotlight lives in the systray on every pane and is
                // independent of focused_pane_idx — a click there always
                // fires the overlay, regardless of which pane hosts it.
                if (systray_hit_spotlight(mx, my)) {
                    fire_spotlight(mb);
                    leave_pane_scale(_saved_scale);
                    break;
                }

                // No menu is open — check if a menu title was clicked.
                int clicked = hit_test_menu(mb, pane, mx);
                if (clicked < 0) {
                    leave_pane_scale(_saved_scale);
                    break;
                }

                // Click-to-promote: in Modern mode, a click on a
                // non-focused pane retargets _NET_ACTIVE_WINDOW to that
                // output's frontmost window (from
                // _MOONROCK_FRONTMOST_PER_OUTPUT). MoonRock's
                // wm_focus_client republishes _MOONROCK_ACTIVE_OUTPUT,
                // our PropertyNotify handler bumps focused_pane_idx, and
                // the user's second click lands on the now-focused pane
                // and opens the dropdown normally. Skipped if the
                // target pane has no frontmost (None WID) — there's
                // nothing to promote to, so opening the dropdown would
                // trap focus on the wrong output.
                int pane_idx = (int)(pane - mb->panes);
                if (pane_idx != mb->focused_pane_idx) {
                    Window frontmost[MOONROCK_SCALE_MAX_OUTPUTS];
                    int fcount = 0;
                    bool have_front = moonrock_frontmost_per_output(
                        mb->dpy, frontmost,
                        MOONROCK_SCALE_MAX_OUTPUTS, &fcount);
                    Window target = None;
                    if (have_front && pane_idx < fcount) {
                        target = frontmost[pane_idx];
                    }
                    if (target != None) {
                        XEvent req = {0};
                        req.type = ClientMessage;
                        req.xclient.window       = target;
                        req.xclient.message_type = mb->atom_net_active_window;
                        req.xclient.format       = 32;
                        req.xclient.data.l[0]    = 2;   // source = pager
                        req.xclient.data.l[1]    = CurrentTime;
                        req.xclient.data.l[2]    = 0;
                        XSendEvent(mb->dpy, mb->root, False,
                                   SubstructureRedirectMask
                                       | SubstructureNotifyMask,
                                   &req);
                        XFlush(mb->dpy);
                    }
                    // Either way: this click was consumed as a focus
                    // promote. No grab, no dropdown. User re-clicks to
                    // open.
                    leave_pane_scale(_saved_scale);
                    break;
                }

                grab_pointer(mb);
                open_menu_at(mb, pane, clicked);
                leave_pane_scale(_saved_scale);
                break;
            }

            case LeaveNotify: {
                // Mouse left the menu bar window — clear hover on that pane
                MenuBarPane *pane = mb_pane_for_window(mb, ev.xcrossing.window);
                if (pane && mb->open_menu < 0 && pane->hover_index != -1) {
                    pane->hover_index = -1;
                    paint_pane(mb, pane);
                }
                break;
            }

            case PropertyNotify:
                if (ev.xproperty.atom == mb->atom_net_active_window) {
                    if (mb->open_menu >= 0) {
                        dismiss_open_menu(mb);
                    }
                    appmenu_update_all_panes(mb);
                    menubar_paint(mb);
                } else if (ev.xproperty.atom == moonrock_scale_atom(mb->dpy)) {
                    // MoonRock updated the per-output scale table — a
                    // hotplug, a primary switch, a per-output scale change,
                    // or a Displays-pane rotation. Refresh the cached
                    // snapshot, fold the hosting-output scale into
                    // menubar_scale, then run the reconciler which either
                    // adjusts existing panes or spawns/retires pane
                    // windows to match the new output set.
                    moonrock_scale_refresh(mb->dpy, &g_output_scales);
                    float old_hidpi = apply_hidpi_scale_from_table();
                    bool geom_changed = reconcile_panes_to_outputs(mb);
                    log_scale_table("property-notify");
                    if (old_hidpi != menubar_hidpi_scale || geom_changed) {
                        fprintf(stderr,
                                "[menubar] hidpi: %.2f → %.2f, "
                                "%d pane(s)%s\n",
                                (double)old_hidpi,
                                (double)menubar_hidpi_scale,
                                mb->pane_count,
                                geom_changed ? " (moved)" : "");
                        apply_menubar_resize(mb);
                    }
                    if (geom_changed) {
                        appmenu_update_all_panes(mb);
                        menubar_paint(mb);
                    }
                } else if (ev.xproperty.atom ==
                           mb->atom_copycatos_menubar_mode) {
                    // systemcontrol toggled Modern/Classic. Re-read the
                    // atom, run the reconciler (which creates/destroys
                    // pane windows to match), then rewrite struts and
                    // repaint. Dropdowns are dismissed by the reconciler
                    // before it mutates the pane set.
                    MenuBarMode new_mode = read_menubar_mode(mb);
                    if (new_mode != mb->menubar_mode) {
                        fprintf(stderr, "[menubar] mode: %s → %s\n",
                                mb->menubar_mode == MENUBAR_MODE_CLASSIC
                                    ? "classic" : "modern",
                                new_mode == MENUBAR_MODE_CLASSIC
                                    ? "classic" : "modern");
                        mb->menubar_mode = new_mode;
                        (void)reconcile_panes_to_outputs(mb);
                        // Reseed focused_pane_idx from MoonRock — if the
                        // focused output's row index still resolves to a
                        // valid pane, that stays focused; otherwise fall
                        // back to pane 0. Avoids a flash of wrong dimming
                        // at toggle-time before MoonRock's next republish
                        // corrects us.
                        {
                            int idx = moonrock_active_output_index(mb->dpy);
                            int pane_idx = row_to_pane_index(mb, idx);
                            if (mb->menubar_mode == MENUBAR_MODE_CLASSIC) {
                                mb->focused_pane_idx =
                                    (mb->pane_count > 0) ? 0 : -1;
                            } else if (pane_idx >= 0) {
                                mb->focused_pane_idx = pane_idx;
                            } else {
                                mb->focused_pane_idx =
                                    (mb->pane_count > 0) ? 0 : -1;
                            }
                        }
                        apply_menubar_resize(mb);
                        appmenu_update_all_panes(mb);
                        menubar_paint(mb);
                    }
                } else if (ev.xproperty.atom ==
                           moonrock_active_output_atom(mb->dpy)) {
                    // MoonRock republished the focused output. Map its
                    // row index onto our pane index and repaint so the
                    // focus-driven visuals (A.2.5 dimming) track.
                    // In Classic mode only one pane exists; we pin to 0
                    // so the dim toggle can still key off the invariant.
                    int idx = moonrock_active_output_index(mb->dpy);
                    int pane_idx = row_to_pane_index(mb, idx);
                    int new_focus;
                    if (mb->menubar_mode == MENUBAR_MODE_CLASSIC) {
                        new_focus = (mb->pane_count > 0) ? 0 : -1;
                    } else if (pane_idx >= 0) {
                        new_focus = pane_idx;
                    } else {
                        new_focus = (mb->pane_count > 0) ? 0 : -1;
                    }
                    if (new_focus != mb->focused_pane_idx) {
                        mb->focused_pane_idx = new_focus;
                        menubar_paint(mb);
                    }
                } else if (ev.xproperty.atom ==
                           moonrock_frontmost_per_output_atom(mb->dpy)) {
                    // MoonRock republished the per-output frontmost
                    // window list. Each pane reads its own row and
                    // updates active_app/class + reconciles its
                    // legacy_client; repaint follows so the new app
                    // names land on screen this round-trip.
                    appmenu_update_all_panes(mb);
                    menubar_paint(mb);
                }
                break;

            case KeyPress: {
                KeySym sym = XLookupKeysym(&ev.xkey, 0);
                if (sym == XK_Escape && mb->open_menu >= 0) {
                    // If an app menu submenu level is open, Escape
                    // closes just that one level (macOS parity). Only
                    // Escape from the top-level dropdown tears the
                    // whole stack down.
                    //
                    // appmenu_pop_submenu_level inspects the submenu
                    // stack — sized in S()/SF() against the active
                    // pane's scale — so wrap with that pane's scale
                    // before forwarding. dismiss_open_menu already
                    // wraps itself; menubar_paint iterates panes and
                    // wraps each one independently.
                    PaneScaleSaved _saved = enter_active_dropdown_scale(mb);
                    bool popped = (mb->open_menu > 0 &&
                                   appmenu_pop_submenu_level(mb));
                    leave_pane_scale(_saved);
                    if (!popped) {
                        dismiss_open_menu(mb);
                        menubar_paint(mb);
                    }
                }
                break;
            }

            default:
                break;
            }
        }

        // ── Periodic updates (clock, systray) ───────────────────
        time_t now = time(NULL);

        if (now != last_clock_check) {
            last_clock_check = now;
            menubar_paint(mb);
        }

        if (now - last_systray_update >= 10) {
            last_systray_update = now;
            systray_update(mb);
        }

        // ── Heartbeat watchdog — task #76 ────────────────────────
        // Every 12s log a one-liner showing the loop is alive and the
        // age of the most recent X event. Hotplug test cycle is ~30s,
        // so we get ≥2 ticks inside the bug window. Log goes to stderr
        // (~/.xsession-errors). If the heartbeat itself stops, the loop
        // is wedged; if it keeps firing while x_event_age grows, the X
        // queue has gone dry (events not reaching XNextEvent).
        if (now - last_heartbeat_at >= 12) {
            last_heartbeat_at = now;
            fprintf(stderr,
                    "[menubar] alive | mode=%s panes=%d focused=%d "
                    "x_event_age=%lds\n",
                    mb->menubar_mode == MENUBAR_MODE_CLASSIC
                        ? "classic" : "modern",
                    mb->pane_count,
                    mb->focused_pane_idx,
                    (long)(now - last_x_event_at));
        }

        // ── Check for SIGHUP config reload ──────────────────────
        if (reload_config) {
            reload_config = false;
            // Always log SIGHUP arrival — even when height is unchanged.
            // Without this, a SIGHUP that doesn't change the bar height
            // is invisible from the outside, which made task #76 harder
            // to diagnose ("does the loop even see SIGHUP during the
            // hang?" — now answerable from ~/.xsession-errors alone).
            fprintf(stderr, "[menubar] SIGHUP observed (config reload)\n");
            int old_height = menubar_height;
            read_menubar_config();

            if (menubar_height != old_height) {
                fprintf(stderr, "[menubar] Resizing: %d → %d points\n",
                        old_height, menubar_height);
                // recompute_pane_scale uses the global menubar_height,
                // so refresh every pane's scale field before
                // apply_menubar_resize wraps each pane and reads
                // MENUBAR_HEIGHT from the (now-stale) per-pane scale.
                for (int i = 0; i < mb->pane_count; i++) {
                    recompute_pane_scale(&mb->panes[i]);
                }
                apply_menubar_resize(mb);
            } else {
                // Height unchanged — nothing to rebuild, but still
                // repaint in case the config reload updated something
                // else we haven't hooked explicitly yet.
                menubar_paint(mb);
            }
        }

        // ── Wait for next event or timeout ──────────────────────
        // The DBusMenu bridge folds the GLib main context's file
        // descriptors into this same fd_set via appmenu_bridge_prepare_
        // select, and shortens the timeout if a GLib source has a
        // nearer deadline. After select() returns, dispatch drains any
        // ready GDBus work. Keeps the 500ms clock-tick cadence while
        // letting DBus traffic wake the loop immediately.
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(x11_fd, &fds);
        int max_fd     = x11_fd;
        int timeout_ms = 500;

        appmenu_bridge_prepare_select(&fds, &max_fd, &timeout_ms);

        struct timeval tv;
        tv.tv_sec  =  timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        select(max_fd + 1, &fds, NULL, NULL, &tv);

        appmenu_bridge_dispatch(&fds);
    }
}

// ── Painting ────────────────────────────────────────────────────────

// Paint a single pane's dock window. All coordinates are pane-local
// (0..pane->screen_w on X, 0..MENUBAR_HEIGHT on Y) — the pane's window
// sits at (screen_x, screen_y) in virtual-root space, but everything
// inside its Cairo surface is zero-based.
static void paint_pane(MenuBar *mb, MenuBarPane *pane)
{
    // Load this pane's HiDPI scale into the globals so every S() / SF()
    // read inside the paint path (and inside apple_paint, render_*,
    // systray_paint called from here) resolves at the host output's
    // scale. Restored at function exit.
    PaneScaleSaved _saved_scale = enter_pane_scale(pane);

    XWindowAttributes wa;
    XGetWindowAttributes(mb->dpy, pane->win, &wa);
    cairo_surface_t *surface = cairo_xlib_surface_create(
        mb->dpy, pane->win,
        wa.visual,
        pane->screen_w, MENUBAR_HEIGHT
    );
    cairo_t *cr = cairo_create(surface);

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // ── Background ──────────────────────────────────────────────
    // Background renders at full opacity on every pane — only the
    // bar's contents dim on non-active outputs, matching Snow Leopard's
    // treatment of a menubar on a secondary display.
    render_background(mb, pane, cr);

    // ── Active-output dimming (slice A.2.5) ─────────────────────
    // In Modern mode with 2+ panes, the pane hosting the focused app
    // paints its content (apple logo, app name, menus, systray) at
    // full opacity; every other pane paints its content at 0.5 alpha.
    // focused_pane_idx is seeded from MoonRock's _MOONROCK_ACTIVE_OUTPUT
    // and updated on every promote click. Classic mode always has
    // pane_count == 1 and skips dimming.
    int pane_idx = (int)(pane - mb->panes);
    bool dim_content =
        mb->pane_count > 1 &&
        mb->focused_pane_idx >= 0 &&
        pane_idx != mb->focused_pane_idx;
    if (dim_content) {
        cairo_push_group(cr);
    }

    // ── Apple logo (far left) ───────────────────────────────────
    apple_paint(mb, pane, cr);

    // ── Bold app name ───────────────────────────────────────────
    // Vertically center text against the layout's ACTUAL pixel height
    // (ascent + descent including leading), not a hardcoded S(16). At
    // 13pt Lucida Grande, Pango reports ~17-18px per line at 1.0× — a
    // hardcoded 16 under-estimates, which at 1.75× scale compounds
    // and pushes the text against the top edge of the bar. One y for
    // every 13pt item on the bar so menus + app name share a baseline.
    int text_y = render_text_center_y(pane->active_app, true);

    double appname_w = render_text(cr, pane->active_app,
                                   pane->appname_x, text_y,
                                   true, 0.1, 0.1, 0.1);

    // ── Menu titles ─────────────────────────────────────────────
    // Walk this pane's MenuNode root. Each pane shows the menus for its
    // own host output's frontmost window — sourced from
    // _MOONROCK_FRONTMOST_PER_OUTPUT in appmenu_update_all_panes. NULL
    // root = legacy app in first-paint gap; paint nothing but the app
    // name.
    const MenuNode *root = appmenu_root_for(mb, pane);
    int menu_count = root ? root->n_children : 0;

    pane->menus_x = pane->appname_x + (int)appname_w + S(16);

    bool pane_hosts_dropdown = (mb->active_pane == pane_idx);

    int item_x = pane->menus_x;
    for (int i = 0; i < menu_count; i++) {
        const char *title = root->children[i]->label;
        if (!title) continue;
        double w = render_measure_text(title, false);
        int item_w = (int)w + S(20);

        bool highlighted =
            pane->hover_index == i + 1 ||
            (pane_hosts_dropdown && mb->open_menu == i + 1);
        if (highlighted) {
            render_hover_highlight(cr, item_x, S(1), item_w, MENUBAR_HEIGHT - S(2));
        }

        render_text(cr, title,
                    item_x + S(10), text_y,
                    false, 0.1, 0.1, 0.1);

        item_x += item_w;
    }

    // ── Hover highlight for Apple logo ──────────────────────────
    bool apple_highlighted =
        pane->hover_index == 0 ||
        (pane_hosts_dropdown && mb->open_menu == 0);
    if (apple_highlighted) {
        render_hover_highlight(cr, pane->apple_x, S(1),
                               pane->apple_w, MENUBAR_HEIGHT - S(2));
    }

    // ── System tray (right side) ────────────────────────────────
    systray_paint(mb, pane, cr);

    // ── Active-output dimming (slice A.2.5) — apply ─────────────
    // Pop the group we pushed before apple_paint and composite it
    // back at reduced alpha. 0.5 matches Snow Leopard's inactive-bar
    // feel on a second display — content is clearly deprioritized
    // without disappearing. First-click-to-promote still lands
    // (hit tests ignore the overlay).
    if (dim_content) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.5);
    }

    // ── Clean up Cairo resources ────────────────────────────────
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    XFlush(mb->dpy);

    leave_pane_scale(_saved_scale);
}

void menubar_paint(MenuBar *mb)
{
    for (int i = 0; i < mb->pane_count; i++) {
        paint_pane(mb, &mb->panes[i]);
    }
}

// ── Shutdown ────────────────────────────────────────────────────────

void menubar_shutdown(MenuBar *mb)
{
    appmenu_bridge_shutdown();
    systray_cleanup();
    appmenu_cleanup(mb);
    apple_cleanup();
    render_cleanup();

    for (int i = 0; i < mb->pane_count; i++) {
        if (mb->panes[i].win) {
            XDestroyWindow(mb->dpy, mb->panes[i].win);
            mb->panes[i].win = None;
        }
    }
    if (mb->dpy) {
        XCloseDisplay(mb->dpy);
    }

    fprintf(stdout, "menubar: shut down cleanly\n");
}
