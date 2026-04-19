// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// paneview.c — Preference pane content rendering
// ============================================================================
//
// Dispatches to the appropriate pane renderer based on pane ID.
// Functional panes (Dock, Appearance) have their own implementations.
// All other panes show the stub "not yet available" view.
// ============================================================================

#include "paneview.h"
#include "panes/stub.h"
#include "panes/dock.h"
#include "panes/controller.h"
#include "panes/power.h"
#include "panes/about.h"

#include <string.h>

// ============================================================================
// paneview_paint — Render the active pane
// ============================================================================
void paneview_paint(SysPrefsState *state)
{
    if (state->current_pane < 0 || state->current_pane >= state->pane_count) {
        return;
    }

    const char *id = state->panes[state->current_pane].id;

    // Dispatch to functional pane implementations
    if (strcmp(id, "dock") == 0 || strcmp(id, "appearance") == 0) {
        // Both Dock and Appearance settings are in the Dock pane
        // (dock size + menubar height are the core display controls)
        dock_pane_paint(state);
    } else if (strcmp(id, "mouse") == 0 || strcmp(id, "controller") == 0) {
        // Mouse / Controller pane shows gamepad input settings
        controller_pane_paint(state);
    } else if (strcmp(id, "energy-saver") == 0) {
        // Energy Saver pane shows power button timing settings
        power_pane_paint(state);
    } else if (strcmp(id, "about-moonbase") == 0) {
        // About MoonBase pane shows the installed libmoonbase.so.1 version
        about_paint(state, state->current_pane);
    } else {
        // All other panes show the stub view
        stub_paint(state, state->current_pane);
    }
}

// ============================================================================
// paneview_handle_click — Process a click inside the pane view
// ============================================================================
bool paneview_handle_click(SysPrefsState *state, int x, int y)
{
    if (state->current_pane < 0 || state->current_pane >= state->pane_count) {
        return false;
    }

    const char *id = state->panes[state->current_pane].id;

    if (strcmp(id, "dock") == 0 || strcmp(id, "appearance") == 0) {
        return dock_pane_click(state, x, y);
    } else if (strcmp(id, "mouse") == 0 || strcmp(id, "controller") == 0) {
        return controller_pane_click(state, x, y);
    } else if (strcmp(id, "energy-saver") == 0) {
        return power_pane_click(state, x, y);
    }

    return false;
}

// ============================================================================
// paneview_handle_motion — Process mouse motion in the pane view
// ============================================================================
bool paneview_handle_motion(SysPrefsState *state, int x, int y)
{
    if (state->current_pane < 0 || state->current_pane >= state->pane_count) {
        return false;
    }

    const char *id = state->panes[state->current_pane].id;

    if (strcmp(id, "dock") == 0 || strcmp(id, "appearance") == 0) {
        return dock_pane_motion(state, x, y);
    } else if (strcmp(id, "mouse") == 0 || strcmp(id, "controller") == 0) {
        return controller_pane_motion(state, x, y);
    } else if (strcmp(id, "energy-saver") == 0) {
        return power_pane_motion(state, x, y);
    }

    return false;
}

// ============================================================================
// paneview_handle_release — Process mouse button release in the pane view
// ============================================================================
void paneview_handle_release(SysPrefsState *state)
{
    if (state->current_pane < 0 || state->current_pane >= state->pane_count) {
        return;
    }

    const char *id = state->panes[state->current_pane].id;

    if (strcmp(id, "dock") == 0 || strcmp(id, "appearance") == 0) {
        dock_pane_release(state);
    } else if (strcmp(id, "mouse") == 0 || strcmp(id, "controller") == 0) {
        controller_pane_release(state);
    } else if (strcmp(id, "energy-saver") == 0) {
        power_pane_release(state);
    }
}
