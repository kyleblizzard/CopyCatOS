// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase_a11y.h — accessibility tree and event surface (Phase A scaffold).
//
// When populated, this header will expose a MoonBase-native accessibility
// API: UI elements declare role, label, value, bounds, enabled/focused
// state, and invokable actions; the framework bridges into AT-SPI so
// Orca and other existing Linux assistive technologies keep working
// without every app having to know about AT-SPI directly.
//
// Phase A status: **reserved**. No public symbols ship in v1. The
// API-version macro exists so clients can compile-gate without pulling
// the surface in early. Symbols land alongside the first reference app
// that exercises a non-trivial a11y tree (TextEdit).

#ifndef MOONBASE_A11Y_H
#define MOONBASE_A11Y_H

#include <CopyCatAppKit.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOONBASE_A11Y_API_VERSION 0

// (Reserved for:
//   mb_a11y_node_t, mb_a11y_role_t, mb_a11y_event_t,
//   moonbase_a11y_root, moonbase_a11y_add_child,
//   moonbase_a11y_set_role/label/value/bounds/state,
//   moonbase_a11y_post_event.)

#ifdef __cplusplus
}
#endif

#endif // MOONBASE_A11Y_H
