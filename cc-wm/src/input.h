// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// CopyCatOS Window Manager — Keyboard input handling

#ifndef AURA_INPUT_H
#define AURA_INPUT_H

#include "wm.h"

// Set up key grabs (Super+Q close, Super+M minimize, etc.)
void input_setup_grabs(CCWM *wm);

// Handle a key press event
void input_handle_key(CCWM *wm, XKeyEvent *e);

#endif // AURA_INPUT_H
