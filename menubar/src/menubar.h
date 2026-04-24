// CopyCatOS — by Kyle Blizzard at Blizzard.show

// menubar.h — Core menu bar state and lifecycle
//
// This is the central module for the menu bar. It owns the X display
// connection and the dock-type window(s) pinned to the top edge of each
// hosting output. Other modules (render, apple, appmenu, systray) are
// initialized and driven from here.
//
// In Classic mode (A.2.2 baseline) there is exactly one pane on the
// primary output. In Modern mode (A.2.3+) there is one pane per output.
// Everything in this header is shaped so both cases use the same code
// paths — callers pass a MenuBarPane* everywhere paint or hit-test work
// happens, and the outer loop iterates panes[0..pane_count-1].
//
// The menu bar watches the root window for _NET_ACTIVE_WINDOW property
// changes — that's how it knows which application is in the foreground
// and which menus to show.

#ifndef AURA_MENUBAR_H
#define AURA_MENUBAR_H

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdbool.h>

#include "dbusmenu_client.h"

// Default height matching macOS Snow Leopard. Configurable 22-88 via
// ~/.config/copycatos/desktop.conf [menubar] section.
// Range 22-44 is standard desktop use; 44-88 enables touch-friendly sizing
// for handheld devices like the Lenovo Legion Go.
#define DEFAULT_MENUBAR_HEIGHT 22

// Maximum panes supported by the bar. Classic mode always uses 1 on the
// primary; Modern mode uses one per connected output. 8 is generous —
// covers any realistic multi-monitor wall without dynamic allocation.
#define MENUBAR_MAX_PANES 8

// Atom systemcontrol writes (A.2.4) and menubar subscribes to here.
// Value is an XA_STRING of exactly "modern" or "classic"; any other
// value — including an unset atom — is treated as Modern (the default).
// Exposed here so the A.2.4 writer can include this header and use the
// same literal, avoiding a silent divergence between writer and reader.
#define COPYCATOS_MENUBAR_MODE_ATOM_NAME "_COPYCATOS_MENUBAR_MODE"

// Menu-bar mode. Modern is the default — every connected output hosts its
// own pane. Classic keeps a single pane pinned to the primary output,
// matching the pre-A.2.3 baseline; users who prefer the Snow Leopard
// "one global bar on primary" flow toggle to Classic in systemcontrol's
// Appearance pane.
typedef enum {
    MENUBAR_MODE_MODERN = 0,
    MENUBAR_MODE_CLASSIC = 1,
} MenuBarMode;

// Logical bar height in points, from user config. 22 at Snow Leopard
// baseline, 22–88 for handheld/touch. This value is in points — it does
// NOT fold in per-output HiDPI scale. See menubar_scale for the combined
// points-to-physical-pixels factor.
extern int menubar_height;

// Per-output HiDPI scale for the menu bar at the global level. In Classic
// mode this is the primary output's scale (the bar's only host). In Modern
// mode this tracks the primary output as a paint-context default; the
// active pane's scale is loaded into this global on entry to any
// pane-scoped routine (paint_pane, hit_test_menu, dropdown event
// dispatch, ...) and restored on exit. The S() / SF() macros read the
// global, which keeps the macro API unchanged while letting each pane
// resolve to its own host output's scale.
extern float menubar_hidpi_scale;

// Combined points-to-physical-pixels scale:
//   menubar_scale = (menubar_height / 22.0) * menubar_hidpi_scale
// Every pixel constant in the codebase was written against the 22pt
// baseline. S() and SF() multiply by this to produce physical pixels that
// are correct for both the user's height preference and the HiDPI scale
// of the output hosting the bar.
extern double menubar_scale;

// Scale a point value to physical pixels, rounded to nearest int.
// Use for padding, widths, heights, offsets — anything written against
// the 22pt baseline that needs a concrete pixel count.
#define S(x) ((int)((x) * menubar_scale + 0.5))

// Scale a point value to physical pixels as a double (no rounding).
// Use for Cairo coordinates, corner radii, and other fractional values.
#define SF(x) ((x) * menubar_scale)

// Physical-pixel height of the bar window on its host output. Equal to
// menubar_height × menubar_hidpi_scale. Use for window geometry (X
// Create/Resize), struts, and the paint Cairo surface.
#define MENUBAR_HEIGHT S(22)

// One menubar on one output. Populated from the MoonRock scale table at
// init and on every _MOONROCK_OUTPUT_SCALES PropertyNotify. Layout
// positions (apple_x, menus_x, ...) are pane-local — the dock window
// sits at (screen_x, screen_y) on its host output, and painting starts
// at (0, 0) inside that window.
typedef struct {
    Window win;                // Dock window for this pane
    int    screen_x;           // Host output origin X in virtual-root coords
    int    screen_y;           // Host output origin Y in virtual-root coords
    int    screen_w;           // Host output width in pixels
    int    screen_h;           // Host output height in pixels

    // Layout regions — pixel positions relative to this pane's window.
    int    apple_x, apple_w;   // Apple logo clickable region
    int    appname_x, appname_w; // Bold application name region
    int    menus_x;            // X position where menu item titles begin

    // Hover state for this pane only: -1 = pointer is not over any item
    // in this pane, 0 = apple logo, 1+ = menu title index. At most one
    // pane has hover_index >= 0 at a time (the pane currently under the
    // pointer); the others stay at -1.
    int    hover_index;

    // RandR name of the output this pane anchors to (e.g. "eDP-1",
    // "DP-2"). Used in A.2.3 for _COPYCATOS_MENUBAR_MODE subscription
    // and for logging. Empty string in Classic mode.
    char   output_name[32];

    // Per-pane app tracking (A.2.3 deferred — slice 78). Each pane shows
    // the frontmost window of its own host output. Sourced from
    // _MOONROCK_FRONTMOST_PER_OUTPUT (indexed by pane row) when MoonRock
    // is running; falls back to _NET_ACTIVE_WINDOW broadcast to all panes
    // in Classic mode or when the frontmost array is empty.
    char active_app[128];    // Human-readable name of the active app on this pane
    char active_class[128];  // Raw WM_CLASS string for the pane's active window

    // Legacy Mode DBusMenu client for this pane's active window. When
    // the pane's frontmost is registered with the AppMenu.Registrar
    // (GTK3 / Qt5 / Qt6-appmenu), we spin up a DbusMenuClient pointed
    // at its endpoint — independent per pane so two Qt apps on two
    // outputs each get their own imported menu tree.
    DbusMenuClient *legacy_client;
    Window          legacy_wid;
    bool            legacy_is_loading;

    // Tracks the MenuNode root last seen from this pane's legacy_client,
    // so the change callback can distinguish a full tree replacement
    // (pointer differs — dropdown dismiss required) from an in-place
    // property patch (pointer unchanged — repaint only).
    const void     *last_seen_legacy_root;

    // Per-pane HiDPI scale, sourced from the same MoonRock scale-table
    // row that populated screen_x/screen_y/etc. Modern mode lets a 1.0×
    // external and a 1.5× Legion panel each host a correctly-sized bar.
    // Classic mode collapses every pane to the primary output's scale —
    // the single bar still gets the right value through this field.
    float           hidpi_scale;

    // Combined points-to-physical-pixels factor for THIS pane:
    //   pane->scale = (menubar_height / 22.0) * pane->hidpi_scale
    // Mirrors the global menubar_scale formula but per-pane. Loaded into
    // menubar_scale on entry to every pane-scoped routine; the S()/SF()
    // macros still read the global, so all existing call sites resolve
    // at the active pane's correct scale without churning the macro
    // signatures.
    double          scale;
} MenuBarPane;

// Core state for the entire menu bar.
// A single instance is created in main.c and shared with every module.
typedef struct {
    // X11 connection and screen info
    Display *dpy;            // Connection to the X server
    int      screen;         // Default screen number (usually 0)
    Window   root;           // Root window of the screen

    // Active panes. Only panes[0..pane_count-1] are valid; anything past
    // pane_count may carry stale fields from a previous configuration.
    // Classic mode always has pane_count == 1 on the primary output.
    MenuBarPane panes[MENUBAR_MAX_PANES];
    int         pane_count;

    // Index of the pane whose dropdown is currently open, or -1 when no
    // dropdown is live. Dropdown anchoring, click routing, and the
    // submenu stack in appmenu all read the host pane through this.
    // Invariant: dismiss_open_menu is called before any open, so at most
    // one pane has a live dropdown at any moment.
    int         active_pane;

    // Which dropdown level is open on active_pane: -1 = none, 0 = Apple
    // menu, 1+ = app menu title index. Stays global (not per-pane)
    // because the active_pane invariant already guarantees uniqueness —
    // duplicating it per pane would just waste memory and invite skew.
    int         open_menu;

    bool        running;     // Set to false to exit the event loop

    // Current menu-bar mode. Read from _COPYCATOS_MENUBAR_MODE at init and
    // on every PropertyNotify on that atom. Drives reconcile_panes_to_outputs
    // — Modern spawns one pane per connected output, Classic collapses back
    // to a single pane on the primary. Default Modern on startup.
    MenuBarMode menubar_mode;

    // Index of the pane hosting the focused application — the one that shows
    // the non-dimmed menus and receives dropdown clicks on the first press.
    // Sourced from _MOONROCK_ACTIVE_OUTPUT via the scale-table row index.
    // A non-focused pane requires one promoting click (which retargets
    // _NET_ACTIVE_WINDOW to that output's frontmost) before a second click
    // can open a dropdown. Clamped to 0 in Classic mode where only one pane
    // exists. Stays valid across reconciler runs — reseeded when the host
    // pane's output disappears.
    int         focused_pane_idx;

    // X11 atoms — pre-interned for performance.
    // Atoms are unique identifiers for property names in X11. We look them
    // up once at init time and reuse them throughout the session.
    Atom atom_net_active_window;
    Atom atom_net_close_window;   // Send to WM to close active window
    Atom atom_wm_change_state;    // Send to WM to minimize active window (ICCCM)
    Atom atom_net_wm_window_type;
    Atom atom_net_wm_window_type_dock;
    Atom atom_net_wm_strut;
    Atom atom_net_wm_strut_partial;
    Atom atom_wm_class;
    Atom atom_utf8_string;

    // Mode atom — listens for systemcontrol's Modern/Classic toggle write
    // on the root window. PropertyNotify on this atom triggers a
    // reconcile_panes_to_outputs pass, which either spawns or retires
    // pane windows to match the new mode.
    Atom atom_copycatos_menubar_mode;
} MenuBar;

// Initialize the menu bar: open X display, create one dock window on the
// primary output, set up struts, and init all subsystems. Populates
// panes[0] and sets pane_count = 1. Returns true on success.
bool menubar_init(MenuBar *mb);

// Run the main event loop. Blocks until mb->running is set to false.
// Handles X events (expose, mouse, property changes) and periodic
// updates (clock, battery, volume).
void menubar_run(MenuBar *mb);

// Repaint every pane. Called on Expose events and whenever the visual
// state changes (hover, active app change, clock tick). Iterates
// panes[0..pane_count-1] internally.
void menubar_paint(MenuBar *mb);

// Clean up all resources: destroy pane windows, free surfaces, close display.
void menubar_shutdown(MenuBar *mb);

// Find the pane whose dock window is `w`. Returns NULL if `w` is not a
// menubar pane window. Used for event routing when the pointer is not
// grabbed and motion/click coordinates arrive on the pane's own window.
MenuBarPane *mb_pane_for_window(MenuBar *mb, Window w);

// Find the pane whose host output contains root coordinates (rx, ry).
// Returns NULL if the point lies outside every pane's screen rect. Used
// while the pointer is grabbed on root — every event carries absolute
// coordinates and the pane has to be recovered by geometry.
MenuBarPane *mb_pane_for_point(MenuBar *mb, int rx, int ry);

// Convenience for paths that don't yet care which pane. Returns the
// first pane (panes[0]) if pane_count >= 1, else NULL. Classic mode
// calls this to pull the single primary pane.
MenuBarPane *mb_primary_pane(MenuBar *mb);

#endif // AURA_MENUBAR_H
