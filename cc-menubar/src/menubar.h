// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

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

// Default height matching macOS Snow Leopard. Configurable 22-88 via
// ~/.config/copicatos/desktop.conf [menubar] section.
// Range 22-44 is standard desktop use; 44-88 enables touch-friendly sizing
// for handheld devices like the Lenovo Legion Go.
#define DEFAULT_MENUBAR_HEIGHT 22

// Runtime height — set during init from config file, used everywhere.
// External modules (render.c, systray.c) reference this via extern.
extern int menubar_height;

// Proportional scale factor: menubar_height / 22.0 (1.0 at standard size).
// All pixel dimensions throughout the menubar use this to scale proportionally,
// so the UI looks correct whether the bar is 22px (desktop) or 88px (touch).
extern double menubar_scale;

// Convenience macro so existing code doesn't need to change
#define MENUBAR_HEIGHT menubar_height

// Scale a pixel value proportionally and round to the nearest integer.
// Use for all hardcoded pixel dimensions (padding, widths, heights, offsets).
#define S(x) ((int)((x) * menubar_scale + 0.5))

// Scale a floating-point value proportionally (no rounding).
// Use for Cairo coordinates, corner radii, and other fractional values.
#define SF(x) ((x) * menubar_scale)

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

    // X11 atoms — pre-interned for performance.
    // Atoms are unique identifiers for property names in X11. We look them
    // up once at init time and reuse them throughout the session.
    Atom atom_net_active_window;
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
