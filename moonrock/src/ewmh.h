// CopyCatOS — by Kyle Blizzard at Blizzard.show

// CopyCatOS Window Manager — EWMH/ICCCM compliance

#ifndef CC_EWMH_H
#define CC_EWMH_H

#include "wm.h"

// Set up EWMH properties on root window
void ewmh_setup(CCWM *wm);

// Update _NET_CLIENT_LIST and _NET_CLIENT_LIST_STACKING
void ewmh_update_client_list(CCWM *wm);

// Publish _MOONROCK_ACTIVE_OUTPUT (CARDINAL, row index into
// _MOONROCK_OUTPUT_SCALES of the keyboard-focused window's home output)
// and _MOONROCK_FRONTMOST_PER_OUTPUT (XA_WINDOW array, one WID per
// output in the same row order). These two atoms travel with the
// scale table — shell components (menubar in Modern mode) read all
// three and expect matching row order.
//
// Call this from:
//   - wm_focus_client() — focus change
//   - ewmh_update_client_list() — map/unmap
//   - via display_set_scales_published_cb() — hotplug / scale / primary /
//     rotation reshuffles. Registered from moonrock.c at startup.
//
// Dedup identical payloads so subscribers aren't spammed when nothing
// changed.
void ewmh_publish_output_focus_state(CCWM *wm);

// Mark a desktop pane window (`_NET_WM_WINDOW_TYPE_DESKTOP`) as the
// active "Finder" surface. Snow Leopard parity — clicking empty space
// on the desktop surfaces Finder's menu bar without raising the desktop
// or opening a Finder window. The pane WID is stored along with its
// home output (computed from window geometry), and on the next publish
// pass `_MOONROCK_FRONTMOST_PER_OUTPUT[output]` is forced to the pane
// regardless of X stacking order, and `_MOONROCK_ACTIVE_OUTPUT` is set
// to that output. Callers must have already cleared `wm->focused` and
// dropped X keyboard focus — this function only owns the per-output
// "desktop is active" slot, not the focus state.
//
// Pass `pane = None` to clear the slot (e.g. after a managed client
// regains focus on the same output).
void ewmh_set_desktop_active(CCWM *wm, Window pane);

// Register the display-module scales-published hook so
// _MOONROCK_OUTPUT_SCALES rewrites automatically trigger a matching
// rewrite of _MOONROCK_ACTIVE_OUTPUT + _MOONROCK_FRONTMOST_PER_OUTPUT.
// Stashes `wm` in a file-static so the void(void) hook can find it.
// Call once, after display_init() succeeds.
void ewmh_register_focus_state_hook(CCWM *wm);

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

// ── _NET_WM_STATE helpers ──
// Read/write the _NET_WM_STATE atom list on a client window. Used for
// fullscreen, hidden, and other state flags that apps set or toggle.

// Check if a specific state atom is present in the window's _NET_WM_STATE
bool ewmh_has_wm_state(CCWM *wm, Window w, Atom state);

// Add or remove a state atom from the window's _NET_WM_STATE property.
// If 'set' is true, adds the atom (no-op if already present).
// If false, removes it (no-op if not present).
void ewmh_set_wm_state(CCWM *wm, Window w, Atom state, bool set);

// ── Fullscreen management ──
// Enter or exit fullscreen for a client. Handles geometry save/restore,
// frame resize to root dimensions, decoration removal, and state property.
void wm_set_fullscreen(CCWM *wm, Client *c, bool enter);

// Ping timeout in milliseconds (matches macOS's 2-4 second threshold)
#define PING_TIMEOUT_MS 3000

// How often to send pings to the focused window (ms)
#define PING_INTERVAL_MS 2000

#endif // CC_EWMH_H
