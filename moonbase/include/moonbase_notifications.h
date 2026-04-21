// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase_notifications.h — native Aqua notifications (Phase A scaffold).
//
// Posts banners and alerts in the Aqua style, with click callbacks, a
// local history, and two distinct entitlements:
//   * `notifications:post` — normal apps, post their own.
//   * `notifications:receive-all` — privileged listeners (system
//     notification center) only.
//
// Phase A status: **reserved**. No public symbols ship in v1. The
// API-version macro exists so clients can compile-gate without pulling
// the surface in early.

#ifndef MOONBASE_NOTIFICATIONS_H
#define MOONBASE_NOTIFICATIONS_H

#include <moonbase.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOONBASE_NOTIFICATIONS_API_VERSION 0

// (Reserved for:
//   mb_notification_t, mb_notification_style_t,
//   moonbase_notification_post, moonbase_notification_cancel,
//   moonbase_notification_on_click, moonbase_notification_history.)

#ifdef __cplusplus
}
#endif

#endif // MOONBASE_NOTIFICATIONS_H
