// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// dnd.c — Drag-and-drop implementation for the dock
//
// This file implements the core drag-and-drop logic that lets users reorder
// icons in the dock by dragging them, and remove icons by dragging them out
// of the dock (upward, away from the shelf).
//
// The drag lifecycle is:
//   1. ButtonPress on an icon  →  "pending" state (might be a click OR a drag)
//   2. MotionNotify > 5px away →  "active" drag (icon removed from array,
//                                  ghost follows cursor, gap appears)
//   3. ButtonRelease inside    →  icon inserted at gap position (reorder)
//      ButtonRelease outside   →  icon discarded (removal + poof animation)
//
// IMPORTANT: dock_hit_test() in dock.c must be made non-static (remove the
// `static` keyword) and declared in dock.h for this module to call it.
// ============================================================================

#define _GNU_SOURCE  // For M_PI and other math constants under strict C11

#include "dnd.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <cairo/cairo.h>

// ---------------------------------------------------------------------------
// Forward declaration for the poof animation.
//
// poof_start() will be implemented in a separate module later. It plays a
// "puff of smoke" animation at the given screen coordinates when an icon is
// dragged out of the dock and released. For now, we just log a message.
// ---------------------------------------------------------------------------
extern void poof_start(DockState *state, int screen_x, int screen_y) __attribute__((weak));

// ---------------------------------------------------------------------------
// Forward declaration for dock_hit_test (defined in dock.c).
//
// IMPORTANT: For this to link, dock_hit_test() in dock.c must have its
// `static` keyword removed, and a declaration must be added to dock.h:
//     int dock_hit_test(DockState *state, int mx, int my);
// ---------------------------------------------------------------------------
extern int dock_hit_test(DockState *state, int mx, int my);

// ---------------------------------------------------------------------------
// Drag threshold in pixels. If the mouse moves less than this distance from
// the initial ButtonPress, we treat it as a normal click, not a drag.
// This prevents accidental drags when the user just clicks a little sloppily.
// ---------------------------------------------------------------------------
#define DRAG_THRESHOLD   5

// ---------------------------------------------------------------------------
// How far above the dock (in pixels) the cursor must be to count as "outside"
// the dock. When the icon is released outside, it gets removed (poof).
// 50px above the dock window's top edge is a comfortable distance — the user
// has to clearly drag upward, not just overshoot slightly.
// ---------------------------------------------------------------------------
#define OUTSIDE_DISTANCE 50

// ---------------------------------------------------------------------------
// dnd_init — Set everything to a clean "no drag happening" state.
// ---------------------------------------------------------------------------
void dnd_init(DndState *dnd)
{
    // Zero out the entire struct. This sets all bools to false, all ints
    // to 0, and all pointers to NULL — exactly the "idle" state we want.
    memset(dnd, 0, sizeof(DndState));
    dnd->icon_idx = -1;
    dnd->insert_pos = -1;
}

// ---------------------------------------------------------------------------
// dnd_handle_button_press — Record a potential drag start.
//
// We only care about button 1 (left click). If the click lands on a dock
// icon, we record the starting position and which icon was hit, then set
// "pending" to true. The actual drag doesn't start until the mouse moves
// far enough (see dnd_handle_motion).
//
// Returns true if we captured the press (caller should NOT launch the app).
// Returns false if the click missed all icons (not our business).
// ---------------------------------------------------------------------------
bool dnd_handle_button_press(DockState *state, DndState *dnd, int button,
                             int root_x, int root_y)
{
    // Only handle left mouse button — right-click is for context menus
    if (button != 1) {
        return false;
    }

    // Convert the root (screen-global) coordinates to dock-local coordinates
    // by subtracting the dock window's position on screen.
    int local_x = root_x - state->win_x;
    int local_y = root_y - state->win_y;

    // Ask dock_hit_test which icon (if any) is under the cursor.
    // It returns -1 if the click didn't land on any icon.
    int idx = dock_hit_test(state, local_x, local_y);
    if (idx < 0) {
        return false;  // Clicked on empty dock space, not an icon
    }

    // Record the starting state for a potential drag
    dnd->pending = true;
    dnd->active = false;
    dnd->start_x = root_x;
    dnd->start_y = root_y;
    dnd->icon_idx = idx;
    dnd->cursor_x = root_x;
    dnd->cursor_y = root_y;
    dnd->insert_pos = -1;
    dnd->outside_dock = false;

    return true;  // We captured this press — don't launch the app yet
}

// ---------------------------------------------------------------------------
// dnd_handle_motion — Update drag state as the mouse moves.
//
// If we're in "pending" state and the mouse has moved far enough from the
// initial click, we transition to "active" drag:
//   1. Copy the icon's cairo surface to use as a translucent ghost
//   2. Copy the DockItem data so we can re-insert it later
//   3. Remove the item from the dock's items array
//   4. Set active = true
//
// If we're already in "active" state, we just update the cursor position
// and recalculate where the insertion gap should appear.
//
// Returns true if a drag is active (caller should skip magnification).
// ---------------------------------------------------------------------------
bool dnd_handle_motion(DockState *state, DndState *dnd, int root_x, int root_y)
{
    // If no press was recorded on an icon, nothing to do
    if (!dnd->pending && !dnd->active) {
        return false;
    }

    // --- Check if we should transition from "pending" to "active" ---
    if (dnd->pending && !dnd->active) {
        // Calculate the distance the mouse has moved from the initial click
        int dx = root_x - dnd->start_x;
        int dy = root_y - dnd->start_y;
        double distance = sqrt((double)(dx * dx + dy * dy));

        // If the mouse hasn't moved far enough, stay in pending state.
        // This prevents accidental drags from slightly shaky clicks.
        if (distance <= DRAG_THRESHOLD) {
            return false;
        }

        // --- Transition to active drag ---

        int idx = dnd->icon_idx;

        // Safety check: make sure the index is still valid
        if (idx < 0 || idx >= state->item_count) {
            dnd_cleanup(dnd);
            return false;
        }

        DockItem *item = &state->items[idx];

        // Create the ghost image — a copy of the icon surface that will
        // follow the cursor during the drag. We use cairo_surface_reference()
        // to increment the reference count on the existing surface rather
        // than making a full pixel copy. This is safe because the original
        // surface won't be destroyed while we hold a reference.
        if (item->icon) {
            dnd->ghost = cairo_surface_reference(item->icon);
        } else {
            dnd->ghost = NULL;
        }

        // Save a full copy of the DockItem struct. We need this to re-insert
        // the item if the user drops it back into the dock.
        dnd->held_item = *item;

        // IMPORTANT: We must NULL out the icon pointer in the copy because
        // the ghost reference is tracked separately. When we re-insert the
        // held_item later, we'll restore the icon pointer from the ghost.
        // (The original surface stays alive via our ghost reference.)

        // Remove the item from the dock's array by shifting all items after
        // it one position to the left. This closes the gap left by the
        // removed icon.
        //
        // Example: if we remove index 2 from [A, B, C, D, E]:
        //   memmove shifts D and E left → [A, B, D, E, E]
        //   item_count-- → [A, B, D, E] (the trailing E is ignored)
        if (idx < state->item_count - 1) {
            memmove(&state->items[idx],
                    &state->items[idx + 1],
                    (size_t)(state->item_count - idx - 1) * sizeof(DockItem));
        }
        state->item_count--;

        dnd->active = true;
        dnd->pending = false;
    }

    // --- Update cursor position and calculate insert gap ---
    if (dnd->active) {
        dnd->cursor_x = root_x;
        dnd->cursor_y = root_y;

        // Check if the cursor is far enough above the dock to count as
        // "outside." state->win_y is the top edge of the dock window.
        // If the cursor is OUTSIDE_DISTANCE pixels above that, the user
        // is clearly dragging the icon away from the dock.
        dnd->outside_dock = (root_y < state->win_y - OUTSIDE_DISTANCE);

        if (dnd->outside_dock) {
            // Cursor is outside the dock — no insertion gap to show
            dnd->insert_pos = -1;
        } else {
            // Cursor is still near the dock — figure out which gap the
            // cursor is closest to so we can show the insertion indicator.
            //
            // We walk through the remaining icons (the dragged icon has
            // already been removed from the array) and compare the cursor's
            // X position to each icon's center. The insert position is the
            // index where the dragged icon would be inserted.

            // Convert cursor X from screen coordinates to dock-local
            double local_x = (double)(root_x - state->win_x);

            // Calculate where each icon starts (same logic as dock_paint)
            int total_w = dock_calculate_total_width(state);
            double x = (state->win_w - total_w) / 2.0;

            // Default: insert at the end if cursor is past all icons
            int pos = state->item_count;

            for (int i = 0; i < state->item_count; i++) {
                double icon_size = BASE_ICON_SIZE * state->items[i].scale;
                double icon_center = x + icon_size / 2.0;

                // If the cursor is to the left of this icon's center,
                // the insert position is before this icon
                if (local_x < icon_center) {
                    pos = i;
                    break;
                }

                // Move past this icon and its spacing
                x += icon_size;
                if (i < state->item_count - 1) {
                    x += state->items[i].separator_after
                         ? SEPARATOR_WIDTH : ICON_SPACING;
                }
            }

            dnd->insert_pos = pos;
        }

        return true;  // Drag is active
    }

    return false;
}

// ---------------------------------------------------------------------------
// dnd_handle_button_release — Complete or cancel the drag.
//
// If a drag was "active":
//   - If the cursor is outside the dock: the icon is removed permanently.
//     We call poof_start() for the smoke animation and config_save() to
//     persist the change. The ghost surface is freed.
//   - If the cursor is inside the dock: re-insert the held item at the
//     calculated insert_pos. Shift existing items to make room, copy the
//     held item in, and save the config.
//
// If we were only "pending" (no drag happened): return false so the caller
// knows this was just a normal click and should launch the app.
//
// Returns true if a drop was completed (caller should NOT launch).
// Returns false if no drag happened (caller should launch the app).
// ---------------------------------------------------------------------------
bool dnd_handle_button_release(DockState *state, DndState *dnd,
                               int root_x, int root_y)
{
    // --- Case 1: No drag was ever started ---
    if (!dnd->active && !dnd->pending) {
        return false;
    }

    // --- Case 2: Pending but never reached the threshold ---
    // The user clicked and released without moving much. This is a normal
    // click — return false so the caller can launch the app.
    if (dnd->pending && !dnd->active) {
        dnd_cleanup(dnd);
        return false;
    }

    // --- Case 3: Active drag — handle the drop ---

    if (dnd->outside_dock) {
        // The user dragged the icon out of the dock — remove it.
        // In a real macOS dock this plays a "poof" smoke animation.

        // poof_start is declared as a weak symbol — if the poof module
        // isn't linked yet, the symbol will be NULL and we skip it.
        if (poof_start) {
            poof_start(state, root_x, root_y);
        } else {
            fprintf(stderr, "[cc-dock] Icon '%s' removed from dock "
                    "(poof animation not yet implemented)\n",
                    dnd->held_item.name);
        }

        // The icon was already removed from items[] when the drag started.
        // We just need to free its resources and save the new layout.

        // Free the held item's icon surface (our ghost reference)
        if (dnd->ghost) {
            cairo_surface_destroy(dnd->ghost);
            dnd->ghost = NULL;
        }

        // Persist the removal to disk so it survives a restart
        config_save(state);

        dnd_cleanup(dnd);
        return true;  // Drop completed — don't launch
    }

    // The cursor is inside (or near) the dock — re-insert the icon at
    // the calculated insertion position.
    int pos = dnd->insert_pos;

    // Clamp the position to valid bounds. This handles edge cases like
    // the cursor being at the very start or end of the dock.
    if (pos < 0) pos = 0;
    if (pos > state->item_count) pos = state->item_count;

    // Make sure we don't exceed the dock's maximum capacity
    if (state->item_count >= MAX_DOCK_ITEMS) {
        fprintf(stderr, "[cc-dock] Cannot insert: dock is full (%d items)\n",
                MAX_DOCK_ITEMS);
        // Drop the icon — it's lost. This should be very rare.
        if (dnd->ghost) {
            cairo_surface_destroy(dnd->ghost);
            dnd->ghost = NULL;
        }
        dnd_cleanup(dnd);
        return true;
    }

    // Shift items to the right to make room at the insert position.
    //
    // Example: inserting at position 2 in [A, B, D, E]:
    //   memmove shifts D, E right → [A, B, _, D, E]
    //   copy held_item into [2]   → [A, B, C, D, E]
    //   item_count++ → now 5
    if (pos < state->item_count) {
        memmove(&state->items[pos + 1],
                &state->items[pos],
                (size_t)(state->item_count - pos) * sizeof(DockItem));
    }

    // Copy the held item into the newly opened slot
    state->items[pos] = dnd->held_item;

    // Restore the icon surface pointer from our ghost reference.
    // The ghost was a reference to the original surface — now the
    // re-inserted item owns it again.
    state->items[pos].icon = dnd->ghost;
    dnd->ghost = NULL;  // Don't let cleanup free it — the item owns it now

    state->item_count++;

    // Persist the new order to disk
    config_save(state);

    dnd_cleanup(dnd);
    return true;  // Drop completed — don't launch
}

// ---------------------------------------------------------------------------
// dnd_draw_ghost — Draw the translucent ghost icon following the cursor.
//
// Called from dock_paint() after all regular icons are drawn. The ghost
// floats above everything else so the user can clearly see what they're
// dragging.
//
// When the cursor is outside the dock (about to remove the icon), we
// scale the ghost up to 1.2x to give visual feedback that releasing
// will "poof" the icon away.
// ---------------------------------------------------------------------------
void dnd_draw_ghost(DockState *state, DndState *dnd, cairo_t *cr)
{
    // Only draw when a drag is actively in progress
    if (!dnd->active || !dnd->ghost) {
        return;
    }

    cairo_save(cr);

    // Get the size of the ghost icon surface (typically 128x128)
    int src_w = cairo_image_surface_get_width(dnd->ghost);
    int src_h = cairo_image_surface_get_height(dnd->ghost);

    // The display size of the ghost. Normally it's BASE_ICON_SIZE, but
    // when outside the dock we scale it up slightly to signal "this icon
    // will be removed if you let go."
    double display_size = BASE_ICON_SIZE;
    if (dnd->outside_dock) {
        display_size = BASE_ICON_SIZE * 1.2;
    }

    // Convert cursor position from screen coordinates to dock-local
    // coordinates (the cairo context is relative to the dock window).
    // Center the ghost icon on the cursor position.
    double draw_x = dnd->cursor_x - state->win_x - display_size / 2.0;
    double draw_y = dnd->cursor_y - state->win_y - display_size / 2.0;

    // Position and scale the ghost icon
    cairo_translate(cr, draw_x, draw_y);
    cairo_scale(cr, display_size / src_w, display_size / src_h);
    cairo_set_source_surface(cr, dnd->ghost, 0, 0);

    // Use bilinear filtering for smooth scaling (same as regular icons)
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);

    // Paint at 70% opacity — makes it look like a translucent "ghost"
    // so the user can see the icons underneath and the insertion gap.
    cairo_paint_with_alpha(cr, 0.7);

    cairo_restore(cr);
}

// ---------------------------------------------------------------------------
// dnd_get_gap_position — Return where the visual gap should appear.
//
// dock_paint() calls this to know if it should add extra spacing between
// icons during a drag. When a drag is active and the cursor is over the
// dock, this returns the index where the gap should appear. dock_paint()
// should then add ICON_SPACING * 3 worth of extra space at that position
// in its icon layout loop.
//
// Returns -1 if no gap should be shown (no active drag, or cursor is
// outside the dock).
// ---------------------------------------------------------------------------
int dnd_get_gap_position(DndState *dnd)
{
    if (dnd->active) {
        return dnd->insert_pos;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// dnd_cleanup — Reset the drag state to idle and free resources.
//
// This is called after a drop completes (either reorder or removal) and
// also when a pending drag is cancelled (user clicked without dragging).
// ---------------------------------------------------------------------------
void dnd_cleanup(DndState *dnd)
{
    // Free the ghost surface if we still own it.
    // (After a successful reorder, ghost is NULL because ownership was
    // transferred back to the DockItem. After a removal, the ghost was
    // already freed. But for safety, always check.)
    if (dnd->ghost) {
        cairo_surface_destroy(dnd->ghost);
        dnd->ghost = NULL;
    }

    // Reset everything to the idle state
    dnd->pending = false;
    dnd->active = false;
    dnd->start_x = 0;
    dnd->start_y = 0;
    dnd->icon_idx = -1;
    dnd->insert_pos = -1;
    dnd->cursor_x = 0;
    dnd->cursor_y = 0;
    dnd->outside_dock = false;

    // Note: held_item is a value copy (not a pointer), so it doesn't need
    // to be freed — it just gets overwritten on the next drag.
    memset(&dnd->held_item, 0, sizeof(DockItem));
}
