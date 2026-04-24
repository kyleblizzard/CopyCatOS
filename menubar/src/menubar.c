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

// Recompute the combined scale factor. Must run any time menubar_height
// (user config / SIGHUP) or menubar_hidpi_scale (MoonRock output scale)
// changes.
static void recompute_menubar_scale(void)
{
    menubar_scale =
        ((double)menubar_height / 22.0) * (double)menubar_hidpi_scale;
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

// Pull the primary-output geometry out of the cached MoonRock scale
// table and stuff it into panes[0].screen_{x,y,w,h}. Falls back to the
// virtual-root dimensions when the table is empty or has no primary
// entry — a single-display setup or a MoonRock that pre-dates the
// primary field still lands somewhere sensible.
//
// Returns true if any of the four fields changed, so callers can skip
// the resize path when nothing moved.
//
// TODO A.2.3: generalize to populate panes[0..N-1] from every output
// when Modern mode is active.
static bool lookup_primary_geometry(MenuBar *mb)
{
    MenuBarPane *pane = &mb->panes[0];

    int nx = 0, ny = 0;
    int nw = DisplayWidth(mb->dpy, mb->screen);
    int nh = DisplayHeight(mb->dpy, mb->screen);
    const char *out_name = "";

    const MoonRockOutputScale *p = moonrock_scale_primary(&g_output_scales);
    if (p) {
        nx = p->x;
        ny = p->y;
        nw = p->width;
        nh = p->height;
        out_name = p->name;
    } else if (g_output_scales.valid && g_output_scales.count > 0) {
        // No primary flag set (shouldn't normally happen, but guard it).
        // Prefer the first entry over the whole virtual-root span.
        const MoonRockOutputScale *o = &g_output_scales.outputs[0];
        nx = o->x;
        ny = o->y;
        nw = o->width;
        nh = o->height;
        out_name = o->name;
    }

    bool changed = (nx != pane->screen_x) || (ny != pane->screen_y) ||
                   (nw != pane->screen_w) || (nh != pane->screen_h);
    pane->screen_x = nx;
    pane->screen_y = ny;
    pane->screen_w = nw;
    pane->screen_h = nh;
    strncpy(pane->output_name, out_name, sizeof(pane->output_name) - 1);
    pane->output_name[sizeof(pane->output_name) - 1] = '\0';
    return changed;
}

// Populate the pane-local layout anchors (apple_x/w, appname_x, menus_x).
// Called from init after the pane's screen rect is resolved, and from
// apply_menubar_resize after a scale change so every point constant
// tracks the current menubar_scale.
static void compute_pane_layout(MenuBarPane *pane)
{
    pane->apple_x   = 0;
    pane->apple_w   = S(50);
    pane->appname_x = S(58);
    pane->appname_w = 0;
    // menus_x is written every paint (depends on live app-name width),
    // but seed it so hit_test_menu before the first paint doesn't trip.
    pane->menus_x   = 0;
}

// Apply the current menubar_scale to every live pane: resize the dock
// window, rewrite its struts, recompute scale-dependent layout regions,
// reload any cached scale-sized assets (Apple logo), and repaint.
// Called after either a SIGHUP-driven height change or a MoonRock-driven
// HiDPI change. The strut writes are per-pane — each pane reserves only
// its own output's top edge.
static void apply_menubar_resize(MenuBar *mb)
{
    int h_px = MENUBAR_HEIGHT;  // S(22) — physical pixels on host output

    for (int i = 0; i < mb->pane_count; i++) {
        MenuBarPane *pane = &mb->panes[i];

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

        // Recompute point-scaled layout anchors.
        compute_pane_layout(pane);

        // Apple logo is pre-rasterized in apple_init; rebuild at the new
        // scale for this pane. Other submodules (render, systray) draw
        // their scale-dependent content per-frame so they don't need a
        // reload.
        apple_reload(mb, pane);
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

    // Classic mode: single pane on the primary. A.2.3 promotes this to
    // one pane per output when _COPYCATOS_MENUBAR_MODE is Modern.
    mb->pane_count  = 1;
    MenuBarPane *pane = &mb->panes[0];

    // Resolve primary-output geometry from the refreshed table. Done
    // BEFORE XCreateWindow so the window is born on the right output at
    // the right width — avoids a visible jump from (0,0,virtual_w,22)
    // to (primary_x, primary_y, primary_w, 22) on the first event pump.
    (void)lookup_primary_geometry(mb);

    // ── Create the menu bar window ──────────────────────────────
    XSetWindowAttributes attrs;
    attrs.override_redirect = False;
    attrs.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask
                     | LeaveWindowMask | StructureNotifyMask | KeyPressMask;
    attrs.background_pixel = 0;
    attrs.colormap = colormap;
    attrs.border_pixel = 0;

    pane->win = XCreateWindow(
        mb->dpy, mb->root,
        pane->screen_x, pane->screen_y,
        (unsigned int)pane->screen_w,
        MENUBAR_HEIGHT,
        0,
        depth,
        InputOutput,
        visual,
        CWOverrideRedirect | CWEventMask | CWBackPixel | CWColormap | CWBorderPixel,
        &attrs
    );

    XSetWindowBackgroundPixmap(mb->dpy, pane->win, None);

    // ── Set window type to DOCK ─────────────────────────────────
    Atom dock_type = mb->atom_net_wm_window_type_dock;
    XChangeProperty(mb->dpy, pane->win,
                    mb->atom_net_wm_window_type, XA_ATOM,
                    32, PropModeReplace,
                    (unsigned char *)&dock_type, 1);

    // ── Reserve screen space with struts ────────────────────────
    // top_start_x / top_end_x narrow the "top strut" to this pane's
    // output only; other monitors stay unreserved at their top.
    long strut_partial[12] = {
        0, 0, MENUBAR_HEIGHT, 0,
        0, 0, 0, 0,
        pane->screen_x, pane->screen_x + pane->screen_w - 1, 0, 0
    };
    XChangeProperty(mb->dpy, pane->win,
                    mb->atom_net_wm_strut_partial, XA_CARDINAL,
                    32, PropModeReplace,
                    (unsigned char *)strut_partial, 12);

    long strut[4] = { 0, 0, MENUBAR_HEIGHT, 0 };
    XChangeProperty(mb->dpy, pane->win,
                    mb->atom_net_wm_strut, XA_CARDINAL,
                    32, PropModeReplace,
                    (unsigned char *)strut, 4);

    // ── Map (show) the window ───────────────────────────────────
    XMapWindow(mb->dpy, pane->win);
    XFlush(mb->dpy);

    // ── Compute layout regions ──────────────────────────────────
    compute_pane_layout(pane);

    // ── Initialize subsystems ───────────────────────────────────
    render_init(mb);
    apple_init(mb);
    appmenu_init(mb);
    systray_init(mb);
    // DBusMenu / AppMenu.Registrar bridge (slice 18-A). Non-fatal on
    // failure: a KDE dev box where kwin already owns the name gets a
    // warning and a nil bridge; menubar still runs.
    (void)appmenu_bridge_init(mb);

    // ── Set initial state ───────────────────────────────────────
    strncpy(mb->active_app, "Finder", sizeof(mb->active_app) - 1);
    strncpy(mb->active_class, "dolphin", sizeof(mb->active_class) - 1);

    mb->running = true;

    fprintf(stdout, "menubar: initialized (%d pane(s); primary %dx%d on %s)\n",
            mb->pane_count, pane->screen_w, pane->screen_h,
            pane->output_name[0] ? pane->output_name : "<unknown>");

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
    if (mb->open_menu == 0) {
        apple_dismiss(mb);
    } else if (mb->open_menu > 0) {
        appmenu_dismiss(mb);
    }
    mb->open_menu   = -1;
    mb->active_pane = -1;
    ungrab_pointer(mb);
}

// ── Helper: figure out which menu title was clicked ─────────────────
// Returns: -1 = nothing, 0 = Apple logo, 1+ = menu title index.
// `mx` is pane-local (relative to pane->win left edge).

static int hit_test_menu(MenuBar *mb, MenuBarPane *pane, int mx)
{
    // Check Apple logo region
    if (mx >= pane->apple_x && mx < pane->apple_x + pane->apple_w) {
        return 0;
    }

    // Check each menu title region. Walk the MenuNode tree — the root's
    // children are the top-level menu titles. A NULL root happens only
    // during the first-paint gap for a legacy-menu app (see
    // appmenu_root_for's contract); no menu titles to hit-test in that
    // window.
    const MenuNode *root = appmenu_root_for(mb);
    int menu_count = root ? root->n_children : 0;

    int item_x = pane->menus_x;
    for (int i = 0; i < menu_count; i++) {
        const char *title = root->children[i]->label;
        if (!title) continue;
        double w = render_measure_text(title, false);
        int item_w = (int)w + S(20);
        if (mx >= item_x && mx < item_x + item_w) {
            return i + 1;
        }
        item_x += item_w;
    }

    return -1;
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

    if (index == 0) {
        apple_show_menu(mb);
    } else {
        // Walk the MenuNode titles to reach the same X offset the
        // paint path used when it laid them out.
        const MenuNode *root = appmenu_root_for(mb);
        int count = root ? root->n_children : 0;

        int dx = pane->menus_x;
        for (int j = 0; j < index - 1 && j < count; j++) {
            const char *t = root->children[j]->label;
            if (t) dx += (int)render_measure_text(t, false) + S(20);
        }
        appmenu_show_dropdown(mb, index - 1, dx);
    }

    menubar_paint(mb);
}

// ── Event Loop ──────────────────────────────────────────────────────

void menubar_run(MenuBar *mb)
{
    int x11_fd = ConnectionNumber(mb->dpy);
    time_t last_clock_check = 0;
    time_t last_systray_update = 0;

    while (mb->running) {
        // ── Handle all pending X events ─────────────────────────
        while (XPending(mb->dpy)) {
            XEvent ev;
            XNextEvent(mb->dpy, &ev);

            // ── Route events to the dropdown if it's open ───────
            if (mb->open_menu > 0) {
                bool should_dismiss = false;
                if (appmenu_handle_dropdown_event(mb, &ev, &should_dismiss)) {
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
                    break;
                }

                // No menu is open — check Spotlight glyph first
                // (rightmost systray item), then fall back to menu-title
                // hit-test. The glyph's hit rect is the full menubar
                // height so a click anywhere in its column activates.
                if (systray_hit_spotlight(mx, my)) {
                    fire_spotlight(mb);
                    break;
                }

                // No menu is open — check if a menu title was clicked
                int clicked = hit_test_menu(mb, pane, mx);
                if (clicked >= 0) {
                    grab_pointer(mb);
                    open_menu_at(mb, pane, clicked);
                }
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
                    appmenu_update_active(mb);
                    menubar_paint(mb);
                } else if (ev.xproperty.atom == moonrock_scale_atom(mb->dpy)) {
                    // MoonRock updated the per-output scale table — either
                    // a hotplug changed the output set / primary, or the
                    // user moved a Displays-pane slider. Refresh our cached
                    // snapshot, fold the hosting-output scale into
                    // menubar_scale, re-read primary geometry, and resize
                    // if either axis changed.
                    moonrock_scale_refresh(mb->dpy, &g_output_scales);
                    float old_hidpi = apply_hidpi_scale_from_table();
                    bool geom_changed = lookup_primary_geometry(mb);
                    log_scale_table("property-notify");
                    if (old_hidpi != menubar_hidpi_scale || geom_changed) {
                        MenuBarPane *p = mb_primary_pane(mb);
                        fprintf(stderr,
                                "[menubar] hidpi: %.2f → %.2f, "
                                "primary: %d,%d %dx%d%s\n",
                                (double)old_hidpi,
                                (double)menubar_hidpi_scale,
                                p ? p->screen_x : 0,
                                p ? p->screen_y : 0,
                                p ? p->screen_w : 0,
                                p ? p->screen_h : 0,
                                geom_changed ? " (moved)" : "");
                        apply_menubar_resize(mb);
                    }
                }
                break;

            case KeyPress: {
                KeySym sym = XLookupKeysym(&ev.xkey, 0);
                if (sym == XK_Escape && mb->open_menu >= 0) {
                    // If an app menu submenu level is open, Escape
                    // closes just that one level (macOS parity). Only
                    // Escape from the top-level dropdown tears the
                    // whole stack down.
                    if (mb->open_menu > 0 &&
                        appmenu_pop_submenu_level(mb)) {
                        // submenu popped; top-level stays open
                    } else {
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

        // ── Check for SIGHUP config reload ──────────────────────
        if (reload_config) {
            reload_config = false;
            int old_height = menubar_height;
            read_menubar_config();

            if (menubar_height != old_height) {
                fprintf(stderr, "[menubar] Resizing: %d → %d points\n",
                        old_height, menubar_height);
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
    render_background(mb, pane, cr);

    // ── Apple logo (far left) ───────────────────────────────────
    apple_paint(mb, pane, cr);

    // ── Bold app name ───────────────────────────────────────────
    // Vertically center text against the layout's ACTUAL pixel height
    // (ascent + descent including leading), not a hardcoded S(16). At
    // 13pt Lucida Grande, Pango reports ~17-18px per line at 1.0× — a
    // hardcoded 16 under-estimates, which at 1.75× scale compounds
    // and pushes the text against the top edge of the bar. One y for
    // every 13pt item on the bar so menus + app name share a baseline.
    int text_y = render_text_center_y(mb->active_app, true);

    double appname_w = render_text(cr, mb->active_app,
                                   pane->appname_x, text_y,
                                   true, 0.1, 0.1, 0.1);

    // ── Menu titles ─────────────────────────────────────────────
    // Walk the active app's MenuNode root. NULL root = legacy app in
    // first-paint gap; paint nothing but the app name.
    const MenuNode *root = appmenu_root_for(mb);
    int menu_count = root ? root->n_children : 0;

    pane->menus_x = pane->appname_x + (int)appname_w + S(16);

    int pane_idx = (int)(pane - mb->panes);
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

    // ── Clean up Cairo resources ────────────────────────────────
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    XFlush(mb->dpy);
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
    appmenu_bridge_shutdown(mb);
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
