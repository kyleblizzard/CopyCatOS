// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase_power.h — power and thermal state (Phase A scaffold).
//
// Exposes battery level, time-to-empty, AC state, detached-controller
// battery levels, current `powerd` power profile (Balanced / Performance
// / Silent / Battery Saver), and the four-level thermal state
// (Nominal / Fair / Serious / Critical). Apps subscribe and voluntarily
// throttle background work at high thermal levels, mirroring iOS's
// thermal-state pattern.
//
// Phase A status: **reserved**. No public symbols ship in v1. Core
// `moonbase.h` already carries `mb_thermal_state_t`, `MB_EV_THERMAL_CHANGED`,
// and `MB_EV_POWER_CHANGED` so any app can observe the basics without
// this header being populated. Richer queries (profile enum, detached
// controller batteries, brightness hints) land here in Phase E.

#ifndef MOONBASE_POWER_H
#define MOONBASE_POWER_H

#include <CopyCatAppKit.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOONBASE_POWER_API_VERSION 0

// (Reserved for:
//   mb_power_profile_t (balanced / performance / silent / battery_saver),
//   moonbase_power_profile, moonbase_power_battery,
//   moonbase_power_time_remaining, moonbase_power_controllers,
//   moonbase_power_subscribe.)

#ifdef __cplusplus
}
#endif

#endif // MOONBASE_POWER_H
