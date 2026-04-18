// CopyCatOS — by Kyle Blizzard at Blizzard.show

//
// actions.h — CopyCatOS desktop action dispatch
//
// When inputd sends a COPYCATOS_ACTION message, this module figures
// out what to do with it. Actions are high-level desktop operations
// like "open Spotlight" or "show desktop" that get triggered by
// hardware buttons, gestures, or hotkeys on the input device.
//
// The session bridge is the right place for this because these actions
// need X11 access (e.g., sending ClientMessages to the root window)
// or need to signal other CopyCatOS shell components.
//

#ifndef CC_ACTIONS_H
#define CC_ACTIONS_H

#include <X11/Xlib.h>

// actions_dispatch — Execute a named desktop action.
//
// Supported actions:
//   "spotlight"        — Toggle searchsystem search overlay
//   "mission_control"  — Show Mission Control (TODO)
//   "show_desktop"     — Toggle showing the desktop (minimize all windows)
//   "volume_up"        — Increase system volume by 5%
//   "volume_down"      — Decrease system volume by 5%
//   "brightness_up"    — Increase screen brightness by 10%
//   "brightness_down"  — Decrease screen brightness by 10%
//
// Parameters:
//   action_name — the name string received from inputd
//   dpy         — X11 display connection (needed for X11-based actions)
//   root        — root window (needed for sending ClientMessages)
void actions_dispatch(const char *action_name, Display *dpy, Window root);

#endif // CC_ACTIONS_H
