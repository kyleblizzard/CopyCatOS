// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// AuraOS Window Manager — Snow Leopard decoration rendering
// Uses Cairo to paint the exact title bar gradient from the reference photos,
// traffic light buttons from real PNG assets, and centered title text.

#ifndef AURA_DECOR_H
#define AURA_DECOR_H

#include "wm.h"

// Initialize decoration resources (load assets, create Cairo surfaces)
void decor_init(AuraWM *wm);

// Paint the decoration on a client's frame window
void decor_paint(AuraWM *wm, Client *c);

// Cleanup
void decor_shutdown(void);

#endif // AURA_DECOR_H
