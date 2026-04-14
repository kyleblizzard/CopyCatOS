// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// paneview.c — Preference pane content rendering
// ============================================================================
//
// Renders the currently selected preference pane's content area. For Phase 1,
// all panes use the stub view (see panes/stub.c). Functional panes (Desktop,
// Dock, Displays) will be added in Phase 3.
// ============================================================================

#include "paneview.h"
#include "panes/stub.h"

// ============================================================================
// paneview_paint — Render the active pane
// ============================================================================
void paneview_paint(SysPrefsState *state)
{
    if (state->current_pane < 0 || state->current_pane >= state->pane_count) {
        return;
    }

    // For Phase 1, all panes use the stub renderer
    stub_paint(state, state->current_pane);
}

// ============================================================================
// paneview_handle_click — Process a click inside the pane view
// ============================================================================
bool paneview_handle_click(SysPrefsState *state, int x, int y)
{
    (void)state;
    (void)x;
    (void)y;

    // No interactive elements in the stub pane
    return false;
}
