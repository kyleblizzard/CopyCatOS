// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase_controller.h — gamepad / controller access (Phase A scaffold).
//
// Exposes controller enumeration, per-device state snapshots, per-device
// battery level, hotplug notifications, and the Desktop Gaming toggle
// visibility. The Legion / Guide button is never delivered here — it is
// reserved by the system (MoonRock Mission Control in the desktop
// session, gaming-session supervisor in the gaming session) per the
// controller policy in CLAUDE.md.
//
// Phase A status: **reserved**. No public symbols ship in v1. Core
// `moonbase.h` already carries `MB_EV_CONTROLLER_BUTTON`,
// `MB_EV_CONTROLLER_AXIS`, and `MB_EV_CONTROLLER_HOTPLUG` events so
// apps that only need pumped events have everything they need. The
// richer query surface lands when the inputmap UI needs it.

#ifndef MOONBASE_CONTROLLER_H
#define MOONBASE_CONTROLLER_H

#include <moonbase.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOONBASE_CONTROLLER_API_VERSION 0

// (Reserved for:
//   mb_controller_info_t, mb_controller_button_t, mb_controller_axis_t,
//   moonbase_controller_enumerate, moonbase_controller_get_state,
//   moonbase_controller_battery, moonbase_controller_rumble.)

#ifdef __cplusplus
}
#endif

#endif // MOONBASE_CONTROLLER_H
