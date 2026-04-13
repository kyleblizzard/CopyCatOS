// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// AuraOS Window Manager — Edge and corner resize support
// Allows the user to grab any edge or corner of a window frame
// and drag to resize it. Corners take priority over edges.

#ifndef AURA_RESIZE_H
#define AURA_RESIZE_H

#include "wm.h"

// Minimum dimensions for the client content area (in pixels).
// The user can't resize a window smaller than this — prevents
// the window from collapsing into an unusable sliver.
#define MIN_CLIENT_WIDTH  200
#define MIN_CLIENT_HEIGHT 100

// How many pixels from the edge count as a "grab zone" for resizing.
// If the cursor is within this many pixels of an edge, clicking will
// start a resize instead of interacting with the client content.
#define EDGE_GRAB_SIZE     5

// Corner zones are a bit larger than edges — a 10x10 square in each
// corner of the frame. Corners take priority so you can grab a diagonal
// resize even when two edges overlap.
#define CORNER_GRAB_SIZE  10

// Which edge or corner the user is resizing from.
// RESIZE_NONE means the click was in the interior (no resize).
// Corners are combinations of two edges (e.g. TOP + LEFT = TOP_LEFT).
typedef enum {
    RESIZE_NONE = 0,
    RESIZE_TOP,
    RESIZE_BOTTOM,
    RESIZE_LEFT,
    RESIZE_RIGHT,
    RESIZE_TOP_LEFT,
    RESIZE_TOP_RIGHT,
    RESIZE_BOTTOM_LEFT,
    RESIZE_BOTTOM_RIGHT,
} ResizeDir;

// Detect which edge or corner a click falls on, given frame-relative coords.
// Returns RESIZE_NONE if the click is in the title bar or client interior.
ResizeDir resize_detect_edge(Client *c, int frame_x, int frame_y);

// Start a resize operation. Sets up all the drag state and grabs the pointer
// with an appropriate directional cursor (e.g. double arrow for left/right).
void resize_begin(AuraWM *wm, Client *c, int root_x, int root_y, ResizeDir dir);

// Called every time the mouse moves while a resize is active.
// Calculates the new window size/position based on which edge we're dragging.
void resize_update(AuraWM *wm, int root_x, int root_y);

// End the resize operation — ungrab the pointer and reset state.
void resize_end(AuraWM *wm);

// Return the X11 cursor font constant (e.g. XC_top_left_corner) that
// matches the given resize direction. Used for both hover cursors and
// the pointer grab during active resize.
unsigned int resize_get_cursor(ResizeDir dir);

// Update the cursor shape while hovering over a frame (no resize active).
// Detects which edge the cursor is near and changes the frame's cursor
// to the appropriate resize arrow, or back to the default arrow if the
// cursor is over the interior or title bar.
void resize_update_cursor(AuraWM *wm, Client *c, int frame_x, int frame_y);

#endif // AURA_RESIZE_H
