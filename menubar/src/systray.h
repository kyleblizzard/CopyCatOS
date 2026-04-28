// CopyCatOS — by Kyle Blizzard at Blizzard.show

// systray.h — System tray (right side of menu bar)
//
// The system tray occupies the right portion of the menu bar and displays
// status indicators. Items are painted right-to-left:
//   1. Spotlight — magnifying glass icon (rightmost)
//   2. Clock — current time formatted as "Tue 3:58 PM"
//   3. Bluetooth — rune symbol (static, always connected)
//   4. WiFi — signal fan with 3 arcs (static, full signal)
//   5. Volume — speaker icon with sound wave arcs showing level
//   6. Battery — battery outline with fill level (only if hardware exists)
//
// Volume is read from PulseAudio via `pactl`. Battery is read from
// /sys/class/power_supply/. Both are polled periodically to avoid
// hammering the system with constant reads.

#ifndef MENUBAR_SYSTRAY_H
#define MENUBAR_SYSTRAY_H

#include <stdbool.h>
#include <cairo/cairo.h>
#include "menubar.h"

// Initialize the system tray — reads initial volume and battery state.
void systray_init(MenuBar *mb);

// Paint all system tray items onto one pane's menu bar.
// Draws right-to-left starting from (pane->screen_w - 8px margin).
// Returns the total pixel width consumed by the tray, so the caller
// knows where the available space for menu items ends.
int systray_paint(MenuBar *mb, MenuBarPane *pane, cairo_t *cr);

// Re-read battery and volume levels. Called periodically from the
// event loop so the display stays current without constant polling.
void systray_update(MenuBar *mb);

// Hit-test: true if (mx, my) — pane-local coordinates — falls inside
// this pane's Spotlight glyph hit-rect. Each pane owns its own rect
// (written by systray_paint into MenuBarPane), so a click on a
// secondary bar's Spotlight always tests against that pane's geometry,
// not whichever pane painted last. Returns false on a pane that hasn't
// been painted yet (degenerate rect from memset-zero init).
bool systray_hit_spotlight(const MenuBarPane *pane, int mx, int my);

// Clean up any resources.
void systray_cleanup(void);

#endif // MENUBAR_SYSTRAY_H
