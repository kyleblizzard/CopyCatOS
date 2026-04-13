// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// AuraOS Window Manager — EWMH/ICCCM compliance

#ifndef AURA_EWMH_H
#define AURA_EWMH_H

#include "wm.h"

// Set up EWMH properties on root window
void ewmh_setup(AuraWM *wm);

// Update _NET_CLIENT_LIST and _NET_CLIENT_LIST_STACKING
void ewmh_update_client_list(AuraWM *wm);

// Set _NET_FRAME_EXTENTS on a client window
void ewmh_set_frame_extents(AuraWM *wm, Window client);

// Get the _NET_WM_WINDOW_TYPE for a window
Atom ewmh_get_window_type(AuraWM *wm, Window w);

// Check if a window supports WM_DELETE_WINDOW protocol
bool ewmh_supports_delete(AuraWM *wm, Window w);

// Send WM_DELETE_WINDOW to a client
void ewmh_send_delete(AuraWM *wm, Window w);

// Get window title (_NET_WM_NAME falling back to WM_NAME)
void ewmh_get_title(AuraWM *wm, Window w, char *buf, int buflen);

#endif // AURA_EWMH_H
