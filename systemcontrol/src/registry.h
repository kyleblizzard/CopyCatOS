// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// registry.h — Preference pane definitions
// ============================================================================
//
// Registers all 27 Snow Leopard preference panes with their display names,
// categories, and icon file paths. The ordering matches the real Snow Leopard
// NSPrefPaneGroups.xml exactly.
// ============================================================================

#ifndef CC_SYSPREFS_REGISTRY_H
#define CC_SYSPREFS_REGISTRY_H

#include "sysprefs.h"

// Register all preference panes into state->panes[].
// Also computes state->categories[] from the registered panes.
// Call this after sysprefs_init() but before the first paint.
void registry_init(SysPrefsState *state);

// Load all 32x32 icon surfaces for registered panes.
// Call after registry_init().
void registry_load_icons(SysPrefsState *state);

// Load the 128x128 icon for a specific pane (lazy loading for stub view).
void registry_load_icon_128(SysPrefsState *state, int pane_index);

#endif // CC_SYSPREFS_REGISTRY_H
