// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// CopyCatOS Window Manager — Core state and initialization

#ifndef CC_WM_H
#define CC_WM_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdbool.h>
#include <time.h>

// Snow Leopard title bar dimensions (real values from reference)
#define TITLEBAR_HEIGHT   22
#define BORDER_WIDTH       1
#define BUTTON_DIAMETER   13
#define BUTTON_SPACING     7
#define BUTTON_LEFT_PAD    8
#define BUTTON_TOP_PAD     4

// Maximum number of managed clients
#define MAX_CLIENTS 256

// Forward declarations
typedef struct Client Client;
typedef struct CCWM CCWM;

// Whether the compositor is active (ARGB visuals, shadows)
// Set by compositor_init() — checked by frame.c to decide visual type
extern bool compositor_active;

// A managed client window with its frame
struct Client {
    Window client;       // The application's window
    Window frame;        // Our frame window (parent of client)
    int x, y;            // Frame position
    int w, h;            // Client content size (frame is larger)
    bool mapped;         // Is the client currently mapped?
    bool focused;        // Is this the focused client?
    char title[256];     // Window title (_NET_WM_NAME or WM_NAME)
    Atom wm_type;        // _NET_WM_WINDOW_TYPE value
    char wm_class[128];     // WM_CLASS instance name (e.g., "kate", "dolphin")
    char wm_class_name[128]; // WM_CLASS class name (e.g., "Kate", "dolphin")
    bool unsaved;            // True if title starts with * or • (unsaved changes)
    // Ping/responsiveness tracking — _NET_WM_PING protocol.
    // The WM periodically pings the focused window. If the app doesn't
    // respond within PING_TIMEOUT_MS, its cursor becomes the spinning
    // beach ball (just like real Snow Leopard after 2-4 seconds).
    bool supports_ping;      // True if window advertises _NET_WM_PING
    bool ping_pending;       // True if we sent a ping and haven't got pong
    bool unresponsive;       // True if ping timed out (beach ball showing)
    unsigned long ping_serial; // Timestamp sent with the last ping
    struct timespec ping_sent; // When we sent the last ping
    // Saved geometry for smart zoom (toggle between this and maximized)
    int saved_x, saved_y, saved_w, saved_h;
    bool zoomed;             // True if window is currently "smart zoomed" (maximized)

    // Minimized state — set when the window has been minimized into the dock.
    // Unlike 'mapped' (which is also false for hidden app windows), 'minimized'
    // specifically means the user clicked the yellow traffic light and the
    // window is stored in the dock waiting to be restored. We use this flag
    // in on_unmap_notify to skip unframing — the frame must survive so the
    // window can be restored later.
    bool minimized;
};

// Global window manager state
struct CCWM {
    Display *dpy;
    int screen;
    Window root;
    int root_w, root_h;

    // Client tracking
    Client clients[MAX_CLIENTS];
    int num_clients;
    Client *focused;     // Currently focused client

    // EWMH atoms (pre-interned for performance)
    Atom atom_wm_protocols;
    Atom atom_wm_delete;
    Atom atom_wm_name;
    Atom atom_net_wm_name;
    Atom atom_net_wm_type;
    Atom atom_net_wm_type_normal;
    Atom atom_net_wm_type_dock;
    Atom atom_net_wm_type_desktop;
    Atom atom_net_wm_type_dialog;
    Atom atom_net_wm_type_splash;
    Atom atom_net_wm_type_utility;
    Atom atom_net_wm_state;
    Atom atom_net_wm_state_fullscreen;
    Atom atom_net_wm_state_hidden;
    Atom atom_net_active_window;
    Atom atom_net_client_list;
    Atom atom_net_client_list_stacking;
    Atom atom_net_frame_extents;
    Atom atom_net_supported;
    Atom atom_net_supporting_wm_check;
    Atom atom_net_wm_allowed_actions;
    Atom atom_net_close_window;
    Atom atom_net_wm_strut;
    Atom atom_net_wm_strut_partial;
    Atom atom_net_wm_ping;       // _NET_WM_PING for responsiveness detection
    Atom atom_utf8_string;

    // Beach ball cursor — loaded from SnowLeopard theme, set on
    // unresponsive windows to show the spinning wait animation
    unsigned long beach_ball_cursor;

    // State flags
    bool running;
    bool another_wm;     // Set if SubstructureRedirect fails

    // Traffic light hover state — when the mouse enters the button
    // region of any title bar, ALL three buttons show their glyphs
    // (x for close, - for minimize, + for zoom). This matches real
    // Snow Leopard behavior where hovering near any button reveals
    // all three glyphs simultaneously.
    Client *hover_client;    // Client whose buttons are being hovered
    bool buttons_hover;      // True if mouse is in the button region
    int pressed_button;      // 0=none, 1=close, 2=minimize, 3=zoom

    // Drag state
    bool dragging;
    bool resizing;
    int drag_start_x, drag_start_y;
    int drag_frame_x, drag_frame_y;
    int drag_frame_w, drag_frame_h;
    Client *drag_client;
};

// Initialize the window manager
bool wm_init(CCWM *wm, const char *display_name);

// Clean shutdown
void wm_shutdown(CCWM *wm);

// Intern all EWMH/ICCCM atoms
void wm_intern_atoms(CCWM *wm);

// Find a client by its client window or frame window
Client *wm_find_client(CCWM *wm, Window w);
Client *wm_find_client_by_frame(CCWM *wm, Window frame);

// Add/remove clients
Client *wm_add_client(CCWM *wm, Window client_window);
void wm_remove_client(CCWM *wm, Client *c);

// Focus management
void wm_focus_client(CCWM *wm, Client *c);
void wm_unfocus_client(CCWM *wm, Client *c);

#endif // CC_WM_H
