// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// AuraOS Window Manager — X11 event dispatch

#ifndef AURA_EVENTS_H
#define AURA_EVENTS_H

#include "wm.h"

// Main event loop — blocks on XNextEvent, dispatches to handlers
void events_run(AuraWM *wm);

#endif // AURA_EVENTS_H
