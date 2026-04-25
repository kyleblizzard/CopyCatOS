// CopyCatOS — by Kyle Blizzard at Blizzard.show

// host_hittest.c — implementation of the chrome traffic-light hit-test
// math. See host_hittest.h for the contract. Mirrors the geometry
// constants and rounding used by host_chrome.c so the click region
// lines up with the painted pixels at every scale.

#include "host_hittest.h"

// Round a scaled dimension the same way host_chrome.c's px() does, so
// the click discs and the painted discs line up at fractional scales.
static inline int hit_px(float v) {
    int r = (int)(v + 0.5f);
    return r < 1 ? 1 : r;
}

int mb_host_chrome_hit_button(int x, int y, float scale)
{
    int left_pad = hit_px(MB_CHROME_BUTTON_LEFT_PAD * scale);
    int top_pad  = hit_px(MB_CHROME_BUTTON_TOP_PAD  * scale);
    int diameter = hit_px(MB_CHROME_BUTTON_DIAMETER * scale);
    int spacing  = hit_px(MB_CHROME_BUTTON_SPACING  * scale);
    if (y < top_pad || y > top_pad + diameter) return 0;

    int bx = left_pad;
    if (x >= bx && x <= bx + diameter) return 1;
    bx += diameter + spacing;
    if (x >= bx && x <= bx + diameter) return 2;
    bx += diameter + spacing;
    if (x >= bx && x <= bx + diameter) return 3;
    return 0;
}

bool mb_host_chrome_hit_button_region(int x, int y, float scale)
{
    int left_pad = hit_px(MB_CHROME_BUTTON_LEFT_PAD * scale);
    int top_pad  = hit_px(MB_CHROME_BUTTON_TOP_PAD  * scale);
    int diameter = hit_px(MB_CHROME_BUTTON_DIAMETER * scale);
    int spacing  = hit_px(MB_CHROME_BUTTON_SPACING  * scale);
    int region_left  = left_pad;
    int region_right = left_pad + 3 * diameter + 2 * spacing;
    return y >= top_pad && y <= top_pad + diameter
        && x >= region_left && x <= region_right;
}
