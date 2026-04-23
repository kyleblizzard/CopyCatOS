// CopyCatOS — by Kyle Blizzard at Blizzard.show

// menubar.c — Core menu bar lifecycle and event handling
//
// This is the heart of the menu bar. It manages:
//   - The X11 dock-type window pinned to the top of the screen
//   - The main event loop (mouse, expose, property changes)
//   - Layout computation (where each clickable region is)
//   - Coordination between all subsystems
//
// The window uses _NET_WM_WINDOW_TYPE_DOCK so the window manager knows
// to keep it always on top and not give it decorations. The _NET_WM_STRUT
// properties reserve screen space so other windows don't overlap the bar.

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
// its hosting output. Task 3 wiring is proof-of-life only — the logged value
// will drive real pixel-size math once Phase E task 2 converts our
// hardcoded constants to points.
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
// The bar anchors to the primary output, so its scale is the primary's
// scale. A point-based lookup at (0,0) ambiguously matches every output
// that shares the origin — e.g. mirror layouts where eDP-1 and DP-2 both
// sit at (0,0) — and would otherwise return whichever output walked
// first, not necessarily the primary.
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
static int  hit_test_menu(MenuBar *mb, int mx);
static void grab_pointer(MenuBar *mb);
static void ungrab_pointer(MenuBar *mb);

// Pull the primary-output geometry out of the cached MoonRock scale
// table and stuff it into mb->screen_{x,y,w,h}. Falls back to the
// virtual-root dimensions when the table is empty or has no primary
// entry — a single-display setup or a MoonRock that pre-dates the
// primary field still lands somewhere sensible.
//
// Returns true if any of the four fields changed, so callers can skip
// the resize path when nothing moved.
static bool lookup_primary_geometry(MenuBar *mb)
{
    int nx = 0, ny = 0;
    int nw = DisplayWidth(mb->dpy, mb->screen);
    int nh = DisplayHeight(mb->dpy, mb->screen);

    const MoonRockOutputScale *p = moonrock_scale_primary(&g_output_scales);
    if (p) {
        nx = p->x;
        ny = p->y;
        nw = p->width;
        nh = p->height;
    } else if (g_output_scales.valid && g_output_scales.count > 0) {
        // No primary flag set (shouldn't normally happen, but guard it).
        // Prefer the first entry over the whole virtual-root span.
        const MoonRockOutputScale *o = &g_output_scales.outputs[0];
        nx = o->x;
        ny = o->y;
        nw = o->width;
        nh = o->height;
    }

    bool changed = (nx != mb->screen_x) || (ny != mb->screen_y) ||
                   (nw != mb->screen_w) || (nh != mb->screen_h);
    mb->screen_x = nx;
    mb->screen_y = ny;
    mb->screen_w = nw;
    mb->screen_h = nh;
    return changed;
}

// Apply the current menubar_scale to the live window: resize, rewrite
// struts, recompute scale-dependent layout regions, reload any cached
// scale-sized assets (Apple logo), and repaint. Called after either a
// SIGHUP-driven height change or a MoonRock-driven HiDPI change.
static void apply_menubar_resize(MenuBar *mb)
{
    int h_px = MENUBAR_HEIGHT;  // S(22) — physical pixels on host output

    // Move+resize together so the window tracks primary-output changes
    // (hotplug reassigning primary, EDID-override rerunning) without a
    // one-frame flash at the old position.
    XMoveResizeWindow(mb->dpy, mb->win,
                      mb->screen_x, mb->screen_y,
                      mb->screen_w, h_px);

    // Struts are in virtual-root coordinates. `top=h_px` reserves the
    // top strip at the root's top edge; `top_start_x/top_end_x` narrow
    // that reservation to the primary output's X range so windows on a
    // secondary can still use the full top of their own output.
    long strut_partial[12] = {0};
    strut_partial[2] = h_px;
    strut_partial[8] = mb->screen_x;
    strut_partial[9] = mb->screen_x + mb->screen_w - 1;
    XChangeProperty(mb->dpy, mb->win,
                    mb->atom_net_wm_strut_partial, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)strut_partial, 12);
    long strut[4] = { 0, 0, h_px, 0 };
    XChangeProperty(mb->dpy, mb->win,
                    mb->atom_net_wm_strut, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)strut, 4);

    // Recompute point-scaled layout anchors
    mb->apple_w   = S(50);
    mb->appname_x = S(58);

    // Apple logo is pre-rasterized at S(22)×S(15) in apple_init; rebuild
    // at the new scale. Other submodules (render, systray) draw their
    // scale-dependent content per-frame so they don't need a reload.
    apple_reload(mb);

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

    // Zero out the entire struct
    memset(mb, 0, sizeof(MenuBar));
    mb->hover_index = -1;
    mb->open_menu   = -1;

    // Connect to the X server. NULL means use the DISPLAY environment
    // variable, which is the standard way to find the X server.
    mb->dpy = XOpenDisplay(NULL);
    if (!mb->dpy) {
        fprintf(stderr, "menubar: cannot open X display\n");
        return false;
    }

    // Get basic screen info — we need the dimensions to make a
    // full-width window and the root window to watch for active
    // window changes.
    mb->screen   = DefaultScreen(mb->dpy);
    mb->root     = RootWindow(mb->dpy, mb->screen);
    // Geometry is populated from the MoonRock scale bridge further down
    // in this function, after moonrock_scale_refresh() runs. A
    // DisplayWidth/Height seed here is only useful as a stale fallback
    // if MoonRock isn't publishing yet, and lookup_primary_geometry()
    // covers that path on its own.
    mb->screen_x = 0;
    mb->screen_y = 0;
    mb->screen_w = 0;
    mb->screen_h = 0;

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

    mb->win = XCreateWindow(
        mb->dpy, mb->root,
        mb->screen_x, mb->screen_y,
        (unsigned int)mb->screen_w,
        MENUBAR_HEIGHT,
        0,
        depth,
        InputOutput,
        visual,
        CWOverrideRedirect | CWEventMask | CWBackPixel | CWColormap | CWBorderPixel,
        &attrs
    );

    XSetWindowBackgroundPixmap(mb->dpy, mb->win, None);

    // ── Set window type to DOCK ─────────────────────────────────
    Atom dock_type = mb->atom_net_wm_window_type_dock;
    XChangeProperty(mb->dpy, mb->win,
                    mb->atom_net_wm_window_type, XA_ATOM,
                    32, PropModeReplace,
                    (unsigned char *)&dock_type, 1);

    // ── Reserve screen space with struts ────────────────────────
    // top_start_x / top_end_x narrow the "top strut" to the primary
    // output only; other monitors stay unreserved at their top.
    long strut_partial[12] = {
        0, 0, MENUBAR_HEIGHT, 0,
        0, 0, 0, 0,
        mb->screen_x, mb->screen_x + mb->screen_w - 1, 0, 0
    };
    XChangeProperty(mb->dpy, mb->win,
                    mb->atom_net_wm_strut_partial, XA_CARDINAL,
                    32, PropModeReplace,
                    (unsigned char *)strut_partial, 12);

    long strut[4] = { 0, 0, MENUBAR_HEIGHT, 0 };
    XChangeProperty(mb->dpy, mb->win,
                    mb->atom_net_wm_strut, XA_CARDINAL,
                    32, PropModeReplace,
                    (unsigned char *)strut, 4);

    // ── Map (show) the window ───────────────────────────────────
    XMapWindow(mb->dpy, mb->win);
    XFlush(mb->dpy);

    // ── Compute layout regions ──────────────────────────────────
    // All positions scale proportionally so the bar looks correct
    // at any height from 22px to 88px.
    mb->apple_x = 0;
    mb->apple_w = S(50);

    mb->appname_x = S(58);
    mb->appname_w = 0;

    mb->menus_x = 0;

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

    fprintf(stdout, "menubar: initialized (%dx%d screen)\n",
            mb->screen_w, mb->screen_h);

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
    mb->open_menu = -1;
    ungrab_pointer(mb);
}

// ── Helper: figure out which menu title was clicked ─────────────────
// Returns: -1 = nothing, 0 = Apple logo, 1+ = menu title index

static int hit_test_menu(MenuBar *mb, int mx)
{
    // Check Apple logo region
    if (mx >= mb->apple_x && mx < mb->apple_x + mb->apple_w) {
        return 0;
    }

    // Check each menu title region. Walk the MenuNode tree — the root's
    // children are the top-level menu titles. A NULL root happens only
    // during the first-paint gap for a legacy-menu app (see
    // appmenu_root_for's contract); no menu titles to hit-test in that
    // window.
    const MenuNode *root = appmenu_root_for(mb);
    int menu_count = root ? root->n_children : 0;

    int item_x = mb->menus_x;
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
// index 0 = Apple, 1+ = app menus.

static void open_menu_at(MenuBar *mb, int index)
{
    mb->open_menu = index;

    if (index == 0) {
        apple_show_menu(mb);
    } else {
        // Walk the MenuNode titles to reach the same X offset the
        // paint path used when it laid them out.
        const MenuNode *root = appmenu_root_for(mb);
        int count = root ? root->n_children : 0;

        int dx = mb->menus_x;
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
                    menubar_paint(mb);
                }
                break;

            case MotionNotify: {
                // When a menu is open, the pointer is grabbed on root,
                // so we get root coordinates. Convert to menubar-relative.
                int mx, my;
                if (mb->open_menu >= 0) {
                    // Grabbed on root — coordinates are screen/root coords
                    mx = ev.xmotion.x_root;
                    my = ev.xmotion.y_root;
                } else {
                    // Not grabbed — coordinates are relative to mb->win
                    mx = ev.xmotion.x;
                    my = ev.xmotion.y;
                }

                // Mouse below the bar — might be in the dropdown popup.
                // Route hover events to the active dropdown for highlight.
                if (my < 0 || my >= MENUBAR_HEIGHT) {
                    if (mb->hover_index != -1) {
                        mb->hover_index = -1;
                        menubar_paint(mb);
                    }

                    // Forward hover to the active dropdown if mouse is inside it.
                    // Apple menu is one-deep and uses its own helper; app
                    // menus have a multi-level submenu stack, so we ask
                    // appmenu which level the pointer is in and forward
                    // the synthetic motion to that window.
                    if (mb->open_menu == 0) {
                        Window dropdown = apple_get_popup();
                        if (dropdown != None) {
                            XWindowAttributes dwa;
                            XGetWindowAttributes(mb->dpy, dropdown, &dwa);
                            if (mx >= dwa.x && mx < dwa.x + dwa.width &&
                                my >= dwa.y && my < dwa.y + dwa.height) {
                                apple_handle_motion(mb, my - dwa.y);
                            } else {
                                apple_handle_motion(mb, -999);
                            }
                        }
                    } else if (mb->open_menu > 0) {
                        Window hit; int lx, ly;
                        if (appmenu_find_dropdown_at(mb, mx, my, &hit, &lx, &ly)) {
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

                int old_hover = mb->hover_index;
                int new_hover = hit_test_menu(mb, mx);

                if (new_hover != old_hover) {
                    mb->hover_index = new_hover;

                    // If a menu is already open and we hover a different
                    // title, switch to that menu (menu bar scrubbing).
                    if (mb->open_menu >= 0 && new_hover >= 0 &&
                        new_hover != mb->open_menu) {
                        // Dismiss the old dropdown (keep the grab!)
                        if (mb->open_menu == 0) {
                            apple_dismiss(mb);
                        } else {
                            appmenu_dismiss(mb);
                        }

                        // Open the new one (without re-grabbing)
                        open_menu_at(mb, new_hover);
                    }

                    menubar_paint(mb);
                }
                break;
            }

            case ButtonPress: {
                // When pointer is grabbed on root, ButtonPress coords
                // are in root/screen coordinates.
                int mx, my;
                if (mb->open_menu >= 0) {
                    mx = ev.xbutton.x_root;
                    my = ev.xbutton.y_root;
                } else {
                    mx = ev.xbutton.x;
                    my = ev.xbutton.y;
                }


                if (mb->open_menu >= 0) {
                    // A menu is currently open.

                    // Check if click is within the menu bar
                    if (my >= 0 && my < MENUBAR_HEIGHT) {
                        int clicked = hit_test_menu(mb, mx);

                        if (clicked == mb->open_menu) {
                            // Clicked the same menu title — toggle it closed
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
                            open_menu_at(mb, clicked);
                        } else {
                            // Clicked empty space in the menu bar — dismiss
                            dismiss_open_menu(mb);
                            menubar_paint(mb);
                        }
                    } else {
                        // Clicked outside the menu bar entirely. Apple
                        // menu is a single popup; app menus have a
                        // submenu stack, so ask appmenu which level
                        // (if any) contains the point.
                        bool handled = false;

                        if (mb->open_menu == 0) {
                            Window dropdown = apple_get_popup();
                            if (dropdown != None) {
                                XWindowAttributes dwa;
                                XGetWindowAttributes(mb->dpy, dropdown, &dwa);
                                if (mx >= dwa.x && mx < dwa.x + dwa.width &&
                                    my >= dwa.y && my < dwa.y + dwa.height) {
                                    if (apple_handle_click(mb,
                                                           mx - dwa.x,
                                                           my - dwa.y)) {
                                        dismiss_open_menu(mb);
                                        menubar_paint(mb);
                                    }
                                    handled = true;
                                }
                            }
                        } else if (mb->open_menu > 0) {
                            Window hit; int lx, ly;
                            if (appmenu_find_dropdown_at(mb, mx, my,
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
                int clicked = hit_test_menu(mb, mx);
                if (clicked >= 0) {
                    grab_pointer(mb);
                    open_menu_at(mb, clicked);
                }
                break;
            }

            case LeaveNotify:
                // Mouse left the menu bar window — clear hover
                if (mb->open_menu < 0 && mb->hover_index != -1) {
                    mb->hover_index = -1;
                    menubar_paint(mb);
                }
                break;

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
                        fprintf(stderr,
                                "[menubar] hidpi: %.2f → %.2f, "
                                "primary: %d,%d %dx%d%s\n",
                                (double)old_hidpi,
                                (double)menubar_hidpi_scale,
                                mb->screen_x, mb->screen_y,
                                mb->screen_w, mb->screen_h,
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

void menubar_paint(MenuBar *mb)
{
    XWindowAttributes wa;
    XGetWindowAttributes(mb->dpy, mb->win, &wa);
    cairo_surface_t *surface = cairo_xlib_surface_create(
        mb->dpy, mb->win,
        wa.visual,
        mb->screen_w, MENUBAR_HEIGHT
    );
    cairo_t *cr = cairo_create(surface);

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // ── Background ──────────────────────────────────────────────
    render_background(mb, cr);

    // ── Apple logo (far left) ───────────────────────────────────
    apple_paint(mb, cr);

    // ── Bold app name ───────────────────────────────────────────
    // Vertically center text, then nudge up one point. Real Snow Leopard
    // has slightly more padding below the glyphs than above (measured
    // 5:6 ratio in menu.png — "Finder" ink y=5..15 in a 22px bar). A
    // symmetric center at 1.0× looks fine, but at 1.75× the gap under
    // the text collapses to ~4px; the -S(1) nudge restores the bottom
    // padding so the bar reads correctly at any scale. With height≥22
    // guaranteed by config, text_y stays ≥ 0 so no floor clamp is needed.
    int text_y = (MENUBAR_HEIGHT - S(16)) / 2 - S(1);

    double appname_w = render_text(cr, mb->active_app,
                                   mb->appname_x, text_y,
                                   true, 0.1, 0.1, 0.1);

    // ── Menu titles ─────────────────────────────────────────────
    // Walk the active app's MenuNode root. NULL root = legacy app in
    // first-paint gap; paint nothing but the app name.
    const MenuNode *root = appmenu_root_for(mb);
    int menu_count = root ? root->n_children : 0;

    mb->menus_x = mb->appname_x + (int)appname_w + S(16);

    int item_x = mb->menus_x;
    for (int i = 0; i < menu_count; i++) {
        const char *title = root->children[i]->label;
        if (!title) continue;
        double w = render_measure_text(title, false);
        int item_w = (int)w + S(20);

        if (mb->hover_index == i + 1 || mb->open_menu == i + 1) {
            render_hover_highlight(cr, item_x, S(1), item_w, MENUBAR_HEIGHT - S(2));
        }

        render_text(cr, title,
                    item_x + S(10), text_y,
                    false, 0.1, 0.1, 0.1);

        item_x += item_w;
    }

    // ── Hover highlight for Apple logo ──────────────────────────
    if (mb->hover_index == 0 || mb->open_menu == 0) {
        render_hover_highlight(cr, mb->apple_x, S(1), mb->apple_w, MENUBAR_HEIGHT - S(2));
    }

    // ── System tray (right side) ────────────────────────────────
    systray_paint(mb, cr, mb->screen_w);

    // ── Clean up Cairo resources ────────────────────────────────
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    XFlush(mb->dpy);
}

// ── Shutdown ────────────────────────────────────────────────────────

void menubar_shutdown(MenuBar *mb)
{
    appmenu_bridge_shutdown(mb);
    systray_cleanup();
    appmenu_cleanup(mb);
    apple_cleanup();
    render_cleanup();

    if (mb->win) {
        XDestroyWindow(mb->dpy, mb->win);
    }
    if (mb->dpy) {
        XCloseDisplay(mb->dpy);
    }

    fprintf(stdout, "menubar: shut down cleanly\n");
}
