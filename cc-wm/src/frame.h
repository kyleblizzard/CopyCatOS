// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// CopiCatOS Window Manager — Frame management and decoration rendering

#ifndef AURA_FRAME_H
#define AURA_FRAME_H

#include "wm.h"

// Frame (reparent) a new client window
void frame_window(CCWM *wm, Window client);

// Unframe a client (reparent back to root)
void unframe_window(CCWM *wm, Client *c);

// Frame all pre-existing windows at WM startup
void frame_existing_windows(CCWM *wm);

// Redraw the decoration (title bar + border) on a frame
void frame_redraw_decor(CCWM *wm, Client *c);

#endif // AURA_FRAME_H
