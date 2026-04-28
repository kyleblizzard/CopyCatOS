// CopyCatOS — by Kyle Blizzard at Blizzard.show

// CopyCatOS Window Manager — Snow Leopard decoration rendering
// Uses Cairo to paint the exact title bar gradient from the reference photos,
// traffic light buttons from real PNG assets, and centered title text.

#ifndef MOONROCK_DECOR_H
#define MOONROCK_DECOR_H

#include "wm.h"

// Initialize decoration resources (load assets, create Cairo surfaces)
void decor_init(CCWM *wm);

// Paint the decoration on a client's frame window
void decor_paint(CCWM *wm, Client *c);

// Cleanup
void decor_shutdown(void);

#endif // MOONROCK_DECOR_H
