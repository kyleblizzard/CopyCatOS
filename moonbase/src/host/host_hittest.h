// CopyCatOS — by Kyle Blizzard at Blizzard.show

// host_hittest.h — chrome hit-testing for compositor-internal MoonBase
// surfaces. Pure geometry against the chrome's traffic-light buttons —
// no compositor state, no surface table. Shared by moonrock (X
// InputOnly proxy clicks) and moonrock-lite (top-level X window
// clicks).

#ifndef MB_HOST_HITTEST_H
#define MB_HOST_HITTEST_H

#include <stdbool.h>

#include "host_chrome.h"  // MB_CHROME_BUTTON_* constants

#ifdef __cplusplus
extern "C" {
#endif

// Hit-test the three traffic-light buttons at (x, y) in chrome-relative
// pixels at the given backing scale. Returns:
//   1 — close, 2 — minimize, 3 — zoom, 0 — none of the above.
int mb_host_chrome_hit_button(int x, int y, float scale);

// True when (x, y) is anywhere inside the bounding rectangle around
// all three button discs — drives the "show all glyphs on hover" state.
bool mb_host_chrome_hit_button_region(int x, int y, float scale);

#ifdef __cplusplus
}
#endif

#endif // MB_HOST_HITTEST_H
