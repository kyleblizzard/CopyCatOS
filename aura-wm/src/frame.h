// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// AuraOS Window Manager — Frame management and decoration rendering

#ifndef AURA_FRAME_H
#define AURA_FRAME_H

#include "wm.h"

// Frame (reparent) a new client window
void frame_window(AuraWM *wm, Window client);

// Unframe a client (reparent back to root)
void unframe_window(AuraWM *wm, Client *c);

// Frame all pre-existing windows at WM startup
void frame_existing_windows(AuraWM *wm);

// Redraw the decoration (title bar + border) on a frame
void frame_redraw_decor(AuraWM *wm, Client *c);

#endif // AURA_FRAME_H
