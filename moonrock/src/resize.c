// CopyCatOS — by Kyle Blizzard at Blizzard.show

// CopyCatOS Window Manager — Edge and corner resize implementation
//
// This module handles everything related to resizing windows by dragging
// their edges or corners. The basic flow is:
//
//   1. User moves the cursor near a frame edge  -> resize_update_cursor()
//      changes the cursor to a resize arrow so they know they can drag.
//   2. User clicks on that edge                 -> resize_detect_edge()
//      figures out which edge/corner was clicked.
//   3. events.c calls resize_begin()            -> we grab the pointer and
//      record the starting position and size.
//   4. Mouse moves                              -> resize_update() computes
//      the new size based on the drag delta.
//   5. User releases the button                 -> resize_end() ungrabs
//      the pointer and cleans up.
//
// Since we can't modify wm.h, we store the current resize direction and
// cached cursor ID as module-level static variables.

#include "resize.h"
#include "frame.h"
#include <X11/cursorfont.h>
#include <X11/Xcursor/Xcursor.h>
#include <stdio.h>

// ─── Module-static state ───────────────────────────────────────
// These would ideally live in the CCWM struct, but since we
// can't modify wm.h, we keep them here as file-scoped globals.

// Which direction the current resize is going (set in resize_begin,
// read in resize_update, cleared in resize_end).
static ResizeDir current_resize_dir = RESIZE_NONE;

// The last cursor ID we set on a frame window, so we don't keep
// calling XDefineCursor with the same value every time the mouse
// moves. XDefineCursor is a round-trip to the server, so avoiding
// redundant calls helps performance.
static unsigned int cached_cursor_id = 0;

// The frame window that currently has our custom cursor set.
// If the cursor moves to a different frame, we need to reset this.
static Window cached_cursor_frame = 0;


// ─── Edge/Corner Detection ─────────────────────────────────────

ResizeDir resize_detect_edge(Client *c, int frame_x, int frame_y)
{
    // The frame is larger than the client content area.
    // Frame width  = client width  + 2 * BORDER_WIDTH
    // Frame height = client height + TITLEBAR_HEIGHT + BORDER_WIDTH
    int frame_w = c->w + 2 * BORDER_WIDTH;
    int frame_h = c->h + TITLEBAR_HEIGHT + BORDER_WIDTH;

    // If the click is in the title bar area, it's NOT a resize zone.
    // The title bar is used for dragging the whole window, not resizing.
    if (frame_y < TITLEBAR_HEIGHT) {
        return RESIZE_NONE;
    }

    // Determine which edges the click is near.
    // "near_top" means within EDGE_GRAB_SIZE pixels of the top of the
    // resizable area (which starts at TITLEBAR_HEIGHT, not at 0).
    // However, since we already returned RESIZE_NONE for y < TITLEBAR_HEIGHT,
    // "near top" for resize purposes means the top edge of the frame itself.
    // But we need to check the actual frame edges regardless of title bar.
    // Re-check: the spec says title bar is NOT a resize zone, so top-edge
    // resize is only possible below the title bar? No — the top edge of
    // the frame (y near 0) IS in the title bar, which we skip. So top
    // resize can only be triggered from the very top few pixels where
    // y < EDGE_GRAB_SIZE, but we already returned NONE for y < TITLEBAR_HEIGHT.
    //
    // Wait — EDGE_GRAB_SIZE is 5 and TITLEBAR_HEIGHT is 22. So top-edge
    // resize from the top of the frame is impossible because those pixels
    // are all inside the title bar. Let's still detect them for the side
    // edges and bottom. The top edge resize would require the user to
    // grab the very top of the frame, but that overlaps with the title bar.
    //
    // Actually re-reading the spec: "Title bar area (y < TITLEBAR_HEIGHT) is
    // NOT a resize zone — that's for dragging." This means we already
    // returned NONE above, so we only need to check left, right, and bottom
    // edges (and bottom-left / bottom-right corners). But the spec also
    // lists RESIZE_TOP, RESIZE_TOP_LEFT, RESIZE_TOP_RIGHT as valid values.
    // These could theoretically be triggered if EDGE_GRAB_SIZE > TITLEBAR_HEIGHT,
    // but with current values they won't fire. We'll keep the full detection
    // logic for correctness — if someone changes the constants, it'll work.

    bool near_top    = (frame_y < EDGE_GRAB_SIZE);
    bool near_bottom = (frame_y >= frame_h - EDGE_GRAB_SIZE);
    bool near_left   = (frame_x < EDGE_GRAB_SIZE);
    bool near_right  = (frame_x >= frame_w - EDGE_GRAB_SIZE);

    // Check corners first — they take priority over edges.
    // A corner is a CORNER_GRAB_SIZE x CORNER_GRAB_SIZE square
    // in each corner of the frame.
    bool corner_top    = (frame_y < CORNER_GRAB_SIZE);
    bool corner_bottom = (frame_y >= frame_h - CORNER_GRAB_SIZE);
    bool corner_left   = (frame_x < CORNER_GRAB_SIZE);
    bool corner_right  = (frame_x >= frame_w - CORNER_GRAB_SIZE);

    // Top-left corner
    if (corner_top && corner_left)
        return RESIZE_TOP_LEFT;

    // Top-right corner
    if (corner_top && corner_right)
        return RESIZE_TOP_RIGHT;

    // Bottom-left corner
    if (corner_bottom && corner_left)
        return RESIZE_BOTTOM_LEFT;

    // Bottom-right corner
    if (corner_bottom && corner_right)
        return RESIZE_BOTTOM_RIGHT;

    // Now check single edges (no corner match).
    if (near_top)    return RESIZE_TOP;
    if (near_bottom) return RESIZE_BOTTOM;
    if (near_left)   return RESIZE_LEFT;
    if (near_right)  return RESIZE_RIGHT;

    // Click is in the interior — not a resize zone.
    return RESIZE_NONE;
}


// ─── Cursor Lookup ─────────────────────────────────────────────

unsigned int resize_get_cursor(ResizeDir dir)
{
    // Returns the X11 font cursor constant for a given resize direction.
    // Used as a fallback if the themed cursor can't be loaded.
    switch (dir) {
        case RESIZE_TOP:          return XC_top_side;
        case RESIZE_BOTTOM:       return XC_bottom_side;
        case RESIZE_LEFT:         return XC_left_side;
        case RESIZE_RIGHT:        return XC_right_side;
        case RESIZE_TOP_LEFT:     return XC_top_left_corner;
        case RESIZE_TOP_RIGHT:    return XC_top_right_corner;
        case RESIZE_BOTTOM_LEFT:  return XC_bottom_left_corner;
        case RESIZE_BOTTOM_RIGHT: return XC_bottom_right_corner;
        default:                  return XC_left_ptr;
    }
}

// Returns the Xcursor theme name string for a given resize direction.
// These names match the freedesktop cursor spec and are what themed
// cursor files are named in ~/.local/share/icons/<theme>/cursors/.
// Using names instead of font IDs lets XcursorLibraryLoadCursor find
// the themed version (e.g., the Snow Leopard arrow instead of generic X11).
static const char *resize_cursor_name(ResizeDir dir)
{
    switch (dir) {
        case RESIZE_TOP:          return "top_side";
        case RESIZE_BOTTOM:       return "bottom_side";
        case RESIZE_LEFT:         return "left_side";
        case RESIZE_RIGHT:        return "right_side";
        case RESIZE_TOP_LEFT:     return "top_left_corner";
        case RESIZE_TOP_RIGHT:    return "top_right_corner";
        case RESIZE_BOTTOM_LEFT:  return "bottom_left_corner";
        case RESIZE_BOTTOM_RIGHT: return "bottom_right_corner";
        default:                  return "left_ptr";
    }
}


// ─── Begin Resize ──────────────────────────────────────────────

void resize_begin(CCWM *wm, Client *c, int root_x, int root_y, ResizeDir dir)
{
    // Store the resize direction in our module-static variable
    // (can't put it in CCWM since we don't own that struct).
    current_resize_dir = dir;

    // Mark the WM as actively resizing
    wm->resizing = true;
    wm->drag_client = c;

    // Record the starting mouse position — we'll compute deltas from this
    // on every MotionNotify event during the resize.
    wm->drag_start_x = root_x;
    wm->drag_start_y = root_y;

    // Record the frame's starting position and the client's starting size.
    // We need both because resizing from the top or left edge changes
    // the frame's position (it moves while growing/shrinking).
    wm->drag_frame_x = c->x;
    wm->drag_frame_y = c->y;
    wm->drag_frame_w = c->w;
    wm->drag_frame_h = c->h;

    // Create a cursor that matches the resize direction — gives the user
    // visual feedback about which edge they're dragging.
    unsigned int cursor_id = resize_get_cursor(dir);
    Cursor cursor = XCreateFontCursor(wm->dpy, cursor_id);

    // Grab the pointer so we get all mouse events even if the cursor
    // moves outside the frame. This is the same pattern used for
    // title bar dragging in events.c.
    XGrabPointer(wm->dpy, c->frame, True,
                 PointerMotionMask | ButtonReleaseMask,
                 GrabModeAsync, GrabModeAsync,
                 None, cursor, CurrentTime);

    // Free the cursor object — X keeps a reference internally while
    // the grab is active, so we don't need to hold onto it.
    XFreeCursor(wm->dpy, cursor);
}


// ─── Update (called on MotionNotify during resize) ─────────────

void resize_update(CCWM *wm, int root_x, int root_y)
{
    Client *c = wm->drag_client;
    if (!c) return;

    // Calculate how far the mouse has moved from where we started
    int dx = root_x - wm->drag_start_x;
    int dy = root_y - wm->drag_start_y;

    // Start from the original position and size. We always compute from
    // the original values (not the current ones) to avoid drift caused by
    // clamping on previous frames.
    int new_x = wm->drag_frame_x;
    int new_y = wm->drag_frame_y;
    int new_w = wm->drag_frame_w;
    int new_h = wm->drag_frame_h;

    // Apply the delta based on which edge/corner we're dragging.
    //
    // For RIGHT and BOTTOM edges: only the size changes (frame stays put).
    // For LEFT and TOP edges: both position AND size change, because the
    // frame moves in the opposite direction of the growth. Think of it
    // like stretching a rubber band — pulling the left edge left makes
    // the window wider but also moves its origin.

    switch (current_resize_dir) {
        case RESIZE_RIGHT:
            // Dragging the right edge — only width changes
            new_w = wm->drag_frame_w + dx;
            break;

        case RESIZE_LEFT:
            // Dragging the left edge — frame moves right as width shrinks
            new_x = wm->drag_frame_x + dx;
            new_w = wm->drag_frame_w - dx;
            break;

        case RESIZE_BOTTOM:
            // Dragging the bottom edge — only height changes
            new_h = wm->drag_frame_h + dy;
            break;

        case RESIZE_TOP:
            // Dragging the top edge — frame moves down as height shrinks
            new_y = wm->drag_frame_y + dy;
            new_h = wm->drag_frame_h - dy;
            break;

        case RESIZE_TOP_LEFT:
            // Corner: combine top + left behavior
            new_x = wm->drag_frame_x + dx;
            new_w = wm->drag_frame_w - dx;
            new_y = wm->drag_frame_y + dy;
            new_h = wm->drag_frame_h - dy;
            break;

        case RESIZE_TOP_RIGHT:
            // Corner: combine top + right behavior
            new_w = wm->drag_frame_w + dx;
            new_y = wm->drag_frame_y + dy;
            new_h = wm->drag_frame_h - dy;
            break;

        case RESIZE_BOTTOM_LEFT:
            // Corner: combine bottom + left behavior
            new_x = wm->drag_frame_x + dx;
            new_w = wm->drag_frame_w - dx;
            new_h = wm->drag_frame_h + dy;
            break;

        case RESIZE_BOTTOM_RIGHT:
            // Corner: combine bottom + right behavior
            new_w = wm->drag_frame_w + dx;
            new_h = wm->drag_frame_h + dy;
            break;

        case RESIZE_NONE:
            // Shouldn't happen during an active resize, but handle it
            return;
    }

    // ── Enforce minimum size ──
    // Don't let the client area shrink below the minimums. If the user
    // tries to make the window too small, we clamp the size and adjust
    // the position so the opposite edge doesn't jump.

    if (new_w < MIN_CLIENT_WIDTH) {
        // If we're dragging from the left, the position needs to be
        // recalculated so the right edge stays anchored.
        if (current_resize_dir == RESIZE_LEFT ||
            current_resize_dir == RESIZE_TOP_LEFT ||
            current_resize_dir == RESIZE_BOTTOM_LEFT) {
            // Anchor the right edge: x + w should stay the same
            new_x = wm->drag_frame_x + wm->drag_frame_w - MIN_CLIENT_WIDTH;
        }
        new_w = MIN_CLIENT_WIDTH;
    }

    if (new_h < MIN_CLIENT_HEIGHT) {
        // If we're dragging from the top, anchor the bottom edge.
        if (current_resize_dir == RESIZE_TOP ||
            current_resize_dir == RESIZE_TOP_LEFT ||
            current_resize_dir == RESIZE_TOP_RIGHT) {
            // Anchor the bottom edge: y + h should stay the same
            new_y = wm->drag_frame_y + wm->drag_frame_h - MIN_CLIENT_HEIGHT;
        }
        new_h = MIN_CLIENT_HEIGHT;
    }

    // Update the client's stored geometry
    c->x = new_x;
    c->y = new_y;
    c->w = new_w;
    c->h = new_h;

    // Apply the new geometry to X11 windows.
    // The frame includes the title bar and borders, so its dimensions
    // are larger than the client content area.
    int frame_w = c->w + 2 * BORDER_WIDTH;
    int frame_h = c->h + TITLEBAR_HEIGHT + BORDER_WIDTH;

    XMoveResizeWindow(wm->dpy, c->frame, c->x, c->y, frame_w, frame_h);

    // The client window sits inside the frame, offset by the border
    // on the left and the title bar on top.
    XMoveResizeWindow(wm->dpy, c->client,
                      BORDER_WIDTH, TITLEBAR_HEIGHT,
                      c->w, c->h);

    // Repaint the title bar and decorations so they match the new size
    frame_redraw_decor(wm, c);
}


// ─── End Resize ────────────────────────────────────────────────

void resize_end(CCWM *wm)
{
    // Release the pointer grab so events go back to normal
    XUngrabPointer(wm->dpy, CurrentTime);

    // Clear all resize state
    wm->resizing = false;
    wm->drag_client = NULL;
    current_resize_dir = RESIZE_NONE;
}


// ─── Hover Cursor Update ───────────────────────────────────────

void resize_update_cursor(CCWM *wm, Client *c, int frame_x, int frame_y)
{
    // Figure out which edge (if any) the cursor is near
    ResizeDir dir = resize_detect_edge(c, frame_x, frame_y);

    // Look up the matching X11 cursor constant (used as fallback ID
    // and for the caching check)
    unsigned int cursor_id = resize_get_cursor(dir);

    // Optimization: don't call XDefineCursor if we already set this
    // exact cursor on this exact frame. XDefineCursor sends a request
    // to the X server, and doing it on every single MotionNotify event
    // would be wasteful.
    if (cursor_id == cached_cursor_id && c->frame == cached_cursor_frame) {
        return;
    }

    // Try to load the cursor from the active theme (e.g., SnowLeopard)
    // so that frame cursors match the themed root/desktop cursor.
    // Falls back to the X11 font cursor if the theme doesn't provide
    // this particular cursor shape.
    const char *name = resize_cursor_name(dir);
    Cursor cursor = XcursorLibraryLoadCursor(wm->dpy, name);
    if (!cursor) {
        cursor = XCreateFontCursor(wm->dpy, cursor_id);
    }
    XDefineCursor(wm->dpy, c->frame, cursor);
    XFreeCursor(wm->dpy, cursor);

    // Remember what we set so we can skip redundant calls next time
    cached_cursor_id = cursor_id;
    cached_cursor_frame = c->frame;
}
