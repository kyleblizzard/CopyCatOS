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

// ── Responsiveness detection (_NET_WM_PING) ──
// Snow Leopard shows the spinning beach ball after 2-4 seconds of
// unresponsiveness. We implement this via the EWMH ping protocol:
// the WM sends a _NET_WM_PING message to the focused window, and
// if the app doesn't respond within the timeout, we set the frame
// cursor to the animated beach ball from the SnowLeopard theme.

// Check if a window supports _NET_WM_PING in its WM_PROTOCOLS
bool ewmh_supports_ping(CCWM *wm, Window w);

// Send a _NET_WM_PING to a client window (records send time in Client)
void ewmh_send_ping(CCWM *wm, Client *c);

// Check if a ClientMessage is a _NET_WM_PING response (pong)
// Returns the matching Client if it is, NULL otherwise
Client *ewmh_handle_pong(CCWM *wm, XClientMessageEvent *cm);

// Check all clients for ping timeouts. Called periodically from the
// event loop. Sets beach ball cursor on timed-out windows.
void ewmh_check_ping_timeouts(CCWM *wm);

// Ping timeout in milliseconds (matches macOS's 2-4 second threshold)
#define PING_TIMEOUT_MS 3000

// How often to send pings to the focused window (ms)
#define PING_INTERVAL_MS 2000

#endif // CC_EWMH_H
