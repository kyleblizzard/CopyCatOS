// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// CopiCatOS Window Manager — EWMH/ICCCM compliance

#ifndef CC_EWMH_H
#define CC_EWMH_H

#include "wm.h"

// Set up EWMH properties on root window
void ewmh_setup(CCWM *wm);

// Update _NET_CLIENT_LIST and _NET_CLIENT_LIST_STACKING
void ewmh_update_client_list(CCWM *wm);

// Set _NET_FRAME_EXTENTS on a client window
void ewmh_set_frame_extents(CCWM *wm, Window client);

// Get the _NET_WM_WINDOW_TYPE for a window
Atom ewmh_get_window_type(CCWM *wm, Window w);

// Check if a window supports WM_DELETE_WINDOW protocol
bool ewmh_supports_delete(CCWM *wm, Window w);

// Send WM_DELETE_WINDOW to a client
void ewmh_send_delete(CCWM *wm, Window w);

// Get window title (_NET_WM_NAME falling back to WM_NAME)
void ewmh_get_title(CCWM *wm, Window w, char *buf, int buflen);

#endif // CC_EWMH_H
