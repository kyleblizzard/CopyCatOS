// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase_display.h — output enumeration and scale queries (reserved filename).
//
// Most apps never need this header — they read backing scale per-window
// through core `moonbase.h` (`moonbase_window_backing_scale`,
// `MB_EV_BACKING_SCALE_CHANGED`) and let MoonRock handle the rest.
//
// Reserved for apps that genuinely need the output model: the Displays
// preferences pane, screen recorders, wall-display controllers. When
// populated, exposes output enumeration, per-output scale/geometry/
// refresh, and scale-change broadcasts. No symbols ship in v1.

#ifndef MOONBASE_DISPLAY_H
#define MOONBASE_DISPLAY_H

#define MOONBASE_DISPLAY_API_VERSION 0

#endif // MOONBASE_DISPLAY_H
