// CopyCatOS — by Kyle Blizzard at Blizzard.show

// CopyCatOS Window Manager — Keyboard input handling

#ifndef MOONROCK_INPUT_H
#define MOONROCK_INPUT_H

#include "wm.h"

// Set up key grabs (Super+Q close, Super+M minimize, etc.)
void input_setup_grabs(CCWM *wm);

// Handle a key press event
void input_handle_key(CCWM *wm, XKeyEvent *e);

#endif // MOONROCK_INPUT_H
