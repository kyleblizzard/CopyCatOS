// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

//
// mouse.h — Right-stick to mouse cursor emulator
//
// Converts analog stick axis values (ABS_RX, ABS_RY) into smooth pointer
// movement. Uses a non-linear response curve so small stick deflections
// give fine control and large deflections move the cursor quickly.
//
// The emulator runs on a timerfd that fires at 120Hz. Each tick, it reads
// the current stick position, applies the deadzone and response curve,
// and accumulates sub-pixel motion. When a full pixel of movement has
// built up, it's reported to the caller for injection via uinput.
//

#ifndef MOUSE_H
#define MOUSE_H

#include <stdbool.h>

// --------------------------------------------------------------------------
// MouseEmulator — Tracks stick state and produces pointer deltas
// --------------------------------------------------------------------------
typedef struct MouseEmulator {
    // --- Raw stick input ---
    int raw_x;               // Most recent ABS_RX value from the controller
    int raw_y;               // Most recent ABS_RY value from the controller

    // --- Stick center calibration ---
    // Xbox-style controllers report 0 at center with range -32768..32767.
    // These values let us compensate if a controller has a different center.
    int center_x;            // Expected center value for X axis
    int center_y;            // Expected center value for Y axis

    // --- Tuning parameters ---
    int    deadzone;         // Stick deflection below this is ignored (default 4000)
    double sensitivity;      // Multiplier for pointer speed (default 3.0)
    double exponent;         // Response curve exponent: higher = more precision at
                             // small deflections, faster at full tilt (default 2.0)
    double max_speed;        // Maximum pixels-per-tick at full deflection (default 20)

    // --- Sub-pixel accumulator ---
    // The response curve often produces fractional pixel movement. We
    // accumulate these fractions and only report integer pixels to uinput.
    // This prevents jitter from rounding and gives buttery-smooth motion.
    double accum_x;          // Accumulated fractional X movement
    double accum_y;          // Accumulated fractional Y movement

    // --- Left stick scroll emulation ---
    // The left stick (ABS_X / ABS_Y) drives smooth scrolling instead of
    // cursor movement. Uses a linear response (no exponent) because scroll
    // speed should feel proportional to stick deflection.
    int raw_lx;              // Most recent ABS_X value (left stick horizontal)
    int raw_ly;              // Most recent ABS_Y value (left stick vertical)
    double scroll_accum_x;   // Accumulated fractional horizontal scroll
    double scroll_accum_y;   // Accumulated fractional vertical scroll
    double scroll_speed;     // Scroll events per tick at full deflection (default 0.15)
    int    scroll_deadzone;  // Left stick deadzone for scroll (default 6000)

    // --- Timer ---
    int timer_fd;            // timerfd file descriptor, fires at 120Hz
                             // The main epoll loop watches this fd and calls
                             // mouse_tick() and mouse_scroll_tick() each time.
} MouseEmulator;

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------

// mouse_init — Set up default tuning values and create the 120Hz timer.
// Returns true on success, false if timerfd creation fails.
bool mouse_init(MouseEmulator *mouse);

// mouse_update_axis — Store a new axis reading from the controller.
// Called by the main loop whenever an ABS_RX or ABS_RY event arrives.
// `axis` should be ABS_RX or ABS_RY; `value` is the raw axis value.
void mouse_update_axis(MouseEmulator *mouse, int axis, int value);

// mouse_tick — Compute pointer movement for one 120Hz frame.
// Reads the current stick position, applies deadzone and response curve,
// and writes the integer pixel deltas into *dx and *dy.
// Returns true if there is movement to inject (dx or dy != 0).
bool mouse_tick(MouseEmulator *mouse, int *dx, int *dy);

// mouse_update_scroll_axis — Store a new left stick axis reading.
// Called by the main loop whenever an ABS_X or ABS_Y event arrives.
// `axis` should be ABS_X or ABS_Y; `value` is the raw axis value.
void mouse_update_scroll_axis(MouseEmulator *mouse, int axis, int value);

// mouse_scroll_tick — Compute scroll wheel deltas for one 120Hz frame.
// Uses the left stick position to produce smooth scrolling. Writes integer
// scroll notch deltas into *sx (horizontal) and *sy (vertical).
// Returns true if there is scroll movement to inject.
bool mouse_scroll_tick(MouseEmulator *mouse, int *sx, int *sy);

// mouse_shutdown — Close the timer fd and clean up.
void mouse_shutdown(MouseEmulator *mouse);

#endif // MOUSE_H
