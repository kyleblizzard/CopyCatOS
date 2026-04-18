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

#ifndef AURA_SYSTRAY_H
#define AURA_SYSTRAY_H

#include <cairo/cairo.h>
#include "menubar.h"

// Initialize the system tray — reads initial volume and battery state.
void systray_init(MenuBar *mb);

// Paint all system tray items onto the menu bar.
// Draws right-to-left starting from (right_edge - 8px margin).
// Returns the total pixel width consumed by the tray, so the caller
// knows where the available space for menu items ends.
int systray_paint(MenuBar *mb, cairo_t *cr, int right_edge);

// Re-read battery and volume levels. Called periodically from the
// event loop so the display stays current without constant polling.
void systray_update(MenuBar *mb);

// Clean up any resources.
void systray_cleanup(void);

#endif // AURA_SYSTRAY_H
