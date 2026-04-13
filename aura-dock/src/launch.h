// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// launch.h — App launching, process detection, and window activation
//
// This module handles three related tasks:
// 1. Launching apps: fork+exec when the user clicks an icon
// 2. Detecting running apps: periodically parsing `ps` output to update
//    each item's `running` flag
// 3. Activating windows: when clicking an already-running app, bring its
//    window to the front using the _NET_ACTIVE_WINDOW protocol
// ============================================================================

#ifndef LAUNCH_H
#define LAUNCH_H

#include "dock.h"

// Launch the application associated with a dock item.
// If the app is already running, activate its window instead.
// If not running, fork+exec the command and start the bounce animation.
void launch_app(DockState *state, DockItem *item);

// Check which apps are currently running by parsing `ps -eo comm=` output.
// Updates each item's `running` field. Also stops bounce animations for
// items that have been detected as running.
//
// This should be called periodically (every PROCESS_CHECK_INTERVAL seconds).
void launch_check_running(DockItem *items, int count);

// Try to activate (raise and focus) the window belonging to the given app.
// Uses the _NET_ACTIVE_WINDOW X11 client message protocol.
// Falls back to wmctrl if the direct X message doesn't work.
void launch_activate_app(DockState *state, DockItem *item);

// Find a window belonging to the given item by scanning _NET_CLIENT_LIST
// and matching WM_CLASS properties. Returns the window ID or None if not found.
Window launch_find_window(DockState *state, DockItem *item);

#endif // LAUNCH_H
