// CopyCatOS — by Kyle Blizzard at Blizzard.show

// menubar.h — Core menu bar state and lifecycle
//
// This is the central module for the menu bar. It owns the X display
// connection, the 22px dock-type window pinned to the top of the screen,
// and the main event loop. Other modules (render, apple, appmenu, systray)
// are initialized and driven from here.
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

// Logical bar height in points, from user config. 22 at Snow Leopard
// baseline, 22–88 for handheld/touch. This value is in points — it does
// NOT fold in per-output HiDPI scale. See menubar_scale for the combined
// points-to-physical-pixels factor.
extern int menubar_height;

// Per-output HiDPI scale published by MoonRock on _MOONROCK_OUTPUT_SCALES
// for the bar's hosting output. 1.0 when MoonRock isn't running (the
// menubar still draws, just without hidpi awareness).
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

// Core state for the entire menu bar.
// A single instance is created in main.c and shared with every module.
typedef struct {
    // X11 connection and screen info
    Display *dpy;            // Connection to the X server
    int      screen;         // Default screen number (usually 0)
    Window   root;           // Root window of the screen
    Window   win;            // Our menu bar window (dock type, top of screen)
    int      screen_w;       // Screen width in pixels
    int      screen_h;       // Screen height in pixels

    // Layout regions — pixel positions computed during init.
    // These define where each section of the menu bar starts and how wide it is.
    int apple_x, apple_w;    // Apple logo clickable region
    int appname_x, appname_w;// Bold application name region
    int menus_x;             // X position where menu item titles begin

    // Application tracking state
    char active_app[128];    // Human-readable name of the active app (e.g. "Finder")
    char active_class[128];  // Raw WM_CLASS string (e.g. "dolphin")

    // Interaction state
    int  hover_index;        // Which item the mouse is over: -1=none, 0=apple, 1+=menus
    int  open_menu;          // Which dropdown is open: -1=none, 0=apple, 1+=menus
    bool running;            // Set to false to exit the event loop

    // Legacy Mode DBusMenu import (slice 18-C).
    //
    // When the active window's wid is registered with the AppMenu registrar
    // (GTK3, Qt5, Qt6 with the appmenu module), we build a DbusMenuClient
    // pointed at the app's dbusmenu endpoint. `legacy_client` is non-NULL
    // for the lifetime of that registration — replaced when focus changes
    // to another registered window, freed when focus moves to a window
    // without a registration or to a native MoonBase app.
    //
    // `legacy_wid` tracks which window the current client is pointed at so
    // we can skip rebuild churn when focus bounces between two windows of
    // the same app (same wid → same client). `legacy_is_loading` is true
    // between client creation and the first GetLayout reply — the bar
    // shows the app name only during that ~200–400ms window.
    DbusMenuClient *legacy_client;
    Window          legacy_wid;
    bool            legacy_is_loading;

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
} MenuBar;

// Initialize the menu bar: open X display, create the dock window,
// set up struts (so other windows don't overlap us), and init all
// subsystems. Returns true on success.
bool menubar_init(MenuBar *mb);

// Run the main event loop. Blocks until mb->running is set to false.
// Handles X events (expose, mouse, property changes) and periodic
// updates (clock, battery, volume).
void menubar_run(MenuBar *mb);

// Repaint the entire menu bar. Called on Expose events and whenever
// the visual state changes (hover, active app change, clock tick).
void menubar_paint(MenuBar *mb);

// Clean up all resources: destroy windows, free surfaces, close display.
void menubar_shutdown(MenuBar *mb);

#endif // AURA_MENUBAR_H
