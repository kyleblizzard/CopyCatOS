// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

//
// power.h — Power button state machine
//
// Handles the system power button (KEY_POWER from /dev/input). Implements
// a state machine that distinguishes between:
//   - Short press  (<700ms): suspend the system
//   - Long press   (>=3000ms): reboot the system
//   - Medium press (700ms-3000ms): ignored (prevents accidental actions)
//
// The state machine has three states:
//   IDLE     — waiting for a press
//   PRESSED  — button is held, timing how long
//   LONG_PENDING — long-press threshold reached, action queued
//

#ifndef POWER_H
#define POWER_H

#include <stdbool.h>
#include <time.h>

// --------------------------------------------------------------------------
// PowerAction — What the power button handler wants the daemon to do
// --------------------------------------------------------------------------
typedef enum PowerAction {
    POWER_ACTION_NONE    = 0,  // No action needed
    POWER_ACTION_SUSPEND = 1,  // Put the system to sleep
    POWER_ACTION_RESTART = 2   // Reboot the system
} PowerAction;

// --------------------------------------------------------------------------
// PowerState — Internal state machine states
// --------------------------------------------------------------------------
typedef enum PowerState {
    POWER_IDLE         = 0,  // Not pressed, waiting for input
    POWER_PRESSED      = 1,  // Button held down, timing duration
    POWER_LONG_PENDING = 2   // Long-press threshold reached, waiting for release
} PowerState;

// --------------------------------------------------------------------------
// PowerButton — State machine for the power key
// --------------------------------------------------------------------------
typedef struct PowerButton {
    PowerState state;            // Current state machine state

    struct timespec press_time;  // When the button was pressed (monotonic clock)

    int short_press_ms;          // Threshold for short press in milliseconds
                                 // Presses shorter than this trigger suspend
    int long_press_ms;           // Threshold for long press in milliseconds
                                 // Holds longer than this trigger reboot
} PowerButton;

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------

// power_init — Initialize the power button state machine.
// Sets default thresholds (700ms short, 3000ms long).
void power_init(PowerButton *pb);

// power_handle — Process a KEY_POWER press (value=1) or release (value=0).
// Returns what action should be taken, if any.
PowerAction power_handle(PowerButton *pb, int value);

// power_check_hold — Called periodically from the main loop to detect
// long presses while the button is still held. If the button has been
// held for >= long_press_ms, returns POWER_ACTION_RESTART immediately
// (don't wait for release). Returns POWER_ACTION_NONE otherwise.
PowerAction power_check_hold(PowerButton *pb);

// power_execute — Carry out a power action by calling systemctl.
// Logs the action to stderr before executing.
void power_execute(PowerAction action);

#endif // POWER_H
