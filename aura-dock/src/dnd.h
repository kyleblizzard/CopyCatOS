// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// dnd.h — Drag-and-drop support for the dock
//
// This module implements the macOS Snow Leopard-style drag-and-drop behavior
// for the dock. Users can:
//   1. Drag an icon to a new position within the dock to reorder it.
//   2. Drag an icon OUT of the dock (upward, away from the shelf) to remove
//      it entirely — this triggers a "poof" animation in the real macOS dock.
//
// How it works:
//   - On ButtonPress, we record the click position and which icon was hit.
//     We set "pending" but do NOT start the drag yet — the user might just
//     be clicking to launch an app.
//   - On MotionNotify, if the mouse has moved more than 5 pixels from the
//     original click, we transition from "pending" to "active" drag. At this
//     point we remove the icon from the dock's item array and start drawing
//     a translucent "ghost" icon that follows the cursor.
//   - While dragging, we calculate where a gap should appear between the
//     remaining icons (the "insert position") so the user can see where the
//     icon will land if they drop it.
//   - On ButtonRelease, if the cursor is still near the dock, we insert the
//     icon at the gap position. If the cursor is far above the dock, we
//     discard the icon (removal / "poof").
//
// IMPORTANT integration notes for dock.c:
//   - dock_hit_test() must be made NON-static (remove the `static` keyword
//     in dock.c) and declared in dock.h so this module can call it.
//   - The current ButtonPress handler in dock.c calls launch_app() directly.
//     This MUST be changed so that launch_app() is called on ButtonRelease
//     ONLY if no drag occurred. See the pattern documented in
//     dnd_handle_button_release().
//   - dock_paint() should call dnd_get_gap_position() and insert extra
//     spacing (ICON_SPACING * 3) at that index in the icon layout loop.
//     This creates the visual gap where the dragged icon will be dropped.
// ============================================================================

#ifndef DOCK_DND_H
#define DOCK_DND_H

#include "dock.h"

// ---------------------------------------------------------------------------
// DndState — All the state needed to track a drag-and-drop operation.
//
// This struct is owned by the caller (dock.c's event loop). It gets
// initialized once at startup and then updated by the dnd_handle_* functions
// as mouse events come in.
// ---------------------------------------------------------------------------
typedef struct {
    bool pending;              // ButtonPress received, waiting for motion threshold
    bool active;               // Currently dragging (threshold exceeded)
    int start_x, start_y;     // Screen coords of initial ButtonPress
    int icon_idx;              // Index of the icon being dragged (-1 if none)
    int insert_pos;            // Where the gap shows during drag (-1 = outside dock)
    double cursor_x, cursor_y; // Current cursor screen position
    cairo_surface_t *ghost;    // Copy of the dragged icon surface
    DockItem held_item;        // Copy of the item being dragged (removed from items array)
    bool outside_dock;         // True if cursor is far enough from dock to trigger removal
} DndState;

// ---------------------------------------------------------------------------
// Initialize the drag-and-drop state to a clean "no drag in progress" state.
// Call this once when the dock starts up.
// ---------------------------------------------------------------------------
void dnd_init(DndState *dnd);

// ---------------------------------------------------------------------------
// Handle a ButtonPress event. Returns true if the press started a potential
// drag (the caller should NOT launch the app yet — wait for ButtonRelease).
// Returns false if the click didn't hit any icon.
// ---------------------------------------------------------------------------
bool dnd_handle_button_press(DockState *state, DndState *dnd, int button, int root_x, int root_y);

// ---------------------------------------------------------------------------
// Handle a MotionNotify event. Returns true if a drag is currently active
// (the caller should skip normal magnification updates for the dragged icon).
// ---------------------------------------------------------------------------
bool dnd_handle_motion(DockState *state, DndState *dnd, int root_x, int root_y);

// ---------------------------------------------------------------------------
// Handle a ButtonRelease event. Returns true if a drop completed (the caller
// should NOT launch the app). Returns false if the press was just a normal
// click (no drag happened) — the caller should then launch the app.
//
// Integration pattern for dock.c:
//
//   ButtonPress:
//     if (dnd_handle_button_press(...)) {
//         // Potential drag started — don't launch yet
//     } else {
//         // Normal press handling (right-click menu, etc.)
//     }
//
//   ButtonRelease:
//     if (dnd_handle_button_release(...)) {
//         // Drop completed (reorder or removal) — don't launch
//     } else if (dnd.pending) {
//         // Was just a click, not a drag — NOW launch the app
//         int idx = dock_hit_test(state, ev.xbutton.x, ev.xbutton.y);
//         if (idx >= 0) launch_app(state, &state->items[idx]);
//         dnd_cleanup(&dnd);  // Reset pending state
//     }
// ---------------------------------------------------------------------------
bool dnd_handle_button_release(DockState *state, DndState *dnd, int root_x, int root_y);

// ---------------------------------------------------------------------------
// Draw the ghost icon during an active drag. Call this from dock_paint()
// AFTER all regular icons have been drawn, so the ghost floats on top.
// Does nothing if no drag is active.
// ---------------------------------------------------------------------------
void dnd_draw_ghost(DockState *state, DndState *dnd, cairo_t *cr);

// ---------------------------------------------------------------------------
// Get the current insert gap position. Returns the index where the gap
// should appear in the icon layout loop, or -1 if no gap (no active drag
// or the cursor is outside the dock).
//
// dock_paint() should check this value and, when it's >= 0, add extra
// spacing (ICON_SPACING * 3) at that index in the icon layout loop.
// ---------------------------------------------------------------------------
int dnd_get_gap_position(DndState *dnd);

// ---------------------------------------------------------------------------
// Clean up / reset the drag-and-drop state. Frees the ghost surface if one
// exists and resets all fields to their default values.
// ---------------------------------------------------------------------------
void dnd_cleanup(DndState *dnd);

#endif // DOCK_DND_H
