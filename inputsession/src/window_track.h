// CopyCatOS — by Kyle Blizzard at Blizzard.show

//
// window_track.h — X11 active window tracking utilities
//
// These functions read EWMH (Extended Window Manager Hints) and ICCCM
// properties from X11 windows to determine which application is currently
// focused and what its identity is.
//
// EWMH is a standard that window managers follow so that other programs
// can query things like "which window is active?" without needing to
// track focus events themselves.
//

#ifndef CC_WINDOW_TRACK_H
#define CC_WINDOW_TRACK_H

#include <X11/Xlib.h>
#include <stdbool.h>

// window_track_get_active — Read the _NET_ACTIVE_WINDOW property from the
// root window to find out which window the WM considers "active" (focused).
//
// Returns the Window ID of the active window, or None (0) if no window
// is active or the property couldn't be read.
Window window_track_get_active(Display *dpy, Window root, Atom net_active_window);

// window_track_get_wm_class — Read the WM_CLASS property from a window.
//
// WM_CLASS contains two null-terminated strings packed together:
//   1. Instance name — usually the program's argv[0] or a resource name
//   2. Class name — the broader application class (e.g., "Firefox")
//
// These are copied into the provided output buffers.
//
// Parameters:
//   dpy        — X display connection
//   win        — the window to query
//   instance   — output buffer for the instance name
//   inst_len   — size of the instance buffer
//   class_name — output buffer for the class name
//   class_len  — size of the class_name buffer
//
// Returns true if WM_CLASS was successfully read, false otherwise.
bool window_track_get_wm_class(Display *dpy, Window win,
                                char *instance, int inst_len,
                                char *class_name, int class_len);

#endif // CC_WINDOW_TRACK_H
