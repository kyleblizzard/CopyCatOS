// CopyCatOS — by Kyle Blizzard at Blizzard.show

// CopyCatOS Window Manager — X11 event dispatch

#ifndef MOONROCK_EVENTS_H
#define MOONROCK_EVENTS_H

#include "wm.h"

// Main event loop — blocks on XNextEvent, dispatches to handlers
void events_run(CCWM *wm);

#endif // MOONROCK_EVENTS_H
