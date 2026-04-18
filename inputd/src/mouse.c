// CopyCatOS — by Kyle Blizzard at Blizzard.show

//
// mouse.c — Right stick to mouse cursor conversion
//
// Turns the right analog stick (ABS_RX / ABS_RY) into smooth, precise
// pointer movement. The pipeline works like this:
//
//   1. Raw axis values come in from the controller (-32768 to +32767)
//   2. We subtract the center offset to get displacement from neutral
//   3. If displacement is within the deadzone, we ignore it (prevents drift)
//   4. Beyond the deadzone, we normalize the direction and compute speed
//      using a power curve: speed = (deflection ^ exponent) * sensitivity
//   5. We accumulate fractional pixel movement and only report whole pixels
//   6. This runs on a 120Hz timer for consistent, frame-rate-independent motion
//
// The power curve (exponent) is key to making stick control feel good:
//   - Small deflections → very slow, precise movement (for clicking small targets)
//   - Large deflections → fast movement (for traversing the screen quickly)
//   - Linear would feel too fast at small deflections and too slow at large ones
//

#include "mouse.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/timerfd.h>
#include <linux/input-event-codes.h>

// --------------------------------------------------------------------------
// mouse_init — Set up default tuning values and create the 120Hz timer
// --------------------------------------------------------------------------
// Initializes the mouse emulator with sensible defaults for the Legion Go
// right stick. Creates a timerfd that fires at 120Hz — the main epoll loop
// should watch this fd and call mouse_tick() each time it becomes readable.
//
// Returns true on success, false if the timerfd couldn't be created.
// --------------------------------------------------------------------------
bool mouse_init(MouseEmulator *mouse)
{
    // Zero out the entire struct first — ensures all fields start clean.
    // This sets raw_x, raw_y, accum_x, accum_y all to 0.
    memset(mouse, 0, sizeof(MouseEmulator));

    // --- Stick center calibration ---
    // Xbox-style controllers (including the Legion Go) report 0 at center
    // with a range of -32768 to +32767. If a controller had a different
    // center point (e.g. some older controllers center at 128), you'd
    // change these values.
    mouse->center_x = 0;
    mouse->center_y = 0;

    // --- Tuning parameters ---

    // Deadzone: stick deflection below this magnitude is treated as "centered".
    // This prevents stick drift (most analog sticks don't perfectly return to 0).
    // 4000 out of 32767 is about 12% — generous enough to eliminate drift
    // without making the stick feel unresponsive.
    mouse->deadzone = 4000;

    // Sensitivity: overall speed multiplier. Higher = faster cursor.
    // 3.0 is a comfortable default for 1280x800 on the Legion Go.
    mouse->sensitivity = 3.0;

    // Exponent: controls the response curve shape.
    // 2.0 = quadratic: small movements are very slow (precision),
    // full tilt is fast. 1.0 would be linear. 3.0 would be even more
    // precision-biased. 2.0 is the sweet spot for desktop use.
    mouse->exponent = 2.0;

    // Max speed: cap on pixels-per-tick at maximum stick deflection.
    // At 120Hz, 20 pixels/tick = 2400 pixels/second, which crosses
    // a 1280-wide screen in about half a second.
    mouse->max_speed = 20.0;

    // --- Left stick scroll tuning ---

    // Scroll deadzone: larger than the pointer deadzone because accidental
    // scroll input is more disruptive than accidental cursor nudges.
    // 6000 out of 32767 is about 18%.
    mouse->scroll_deadzone = 6000;

    // Scroll speed: how many scroll notches per tick at full deflection.
    // At 120Hz, 0.15 notches/tick = ~18 notches/second, which scrolls
    // at a comfortable reading pace. Much higher feels out of control;
    // much lower feels sluggish.
    mouse->scroll_speed = 0.15;

    // --- Create the 120Hz timer ---
    // timerfd gives us a file descriptor that becomes readable at a fixed
    // interval. We use CLOCK_MONOTONIC (unaffected by system time changes)
    // and TFD_NONBLOCK (so reads don't block if we're late).
    mouse->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (mouse->timer_fd < 0) {
        perror("mouse: timerfd_create failed");
        return false;
    }

    // Set the timer to fire every 8,333,333 nanoseconds (= 1/120 second).
    // it_interval = repeat period (how often it fires)
    // it_value    = initial delay (first fire — same as interval for immediate start)
    struct itimerspec ts = {
        .it_interval = { .tv_sec = 0, .tv_nsec = 8333333 },   // 120Hz repeat
        .it_value    = { .tv_sec = 0, .tv_nsec = 8333333 }    // First fire
    };

    if (timerfd_settime(mouse->timer_fd, 0, &ts, NULL) < 0) {
        perror("mouse: timerfd_settime failed");
        close(mouse->timer_fd);
        mouse->timer_fd = -1;
        return false;
    }

    fprintf(stderr, "mouse: initialized — deadzone=%d, sensitivity=%.1f, "
            "exponent=%.1f, max_speed=%.1f, timer_fd=%d (120Hz)\n",
            mouse->deadzone, mouse->sensitivity, mouse->exponent,
            mouse->max_speed, mouse->timer_fd);

    return true;
}

// --------------------------------------------------------------------------
// mouse_update_axis — Store a new axis reading from the controller
// --------------------------------------------------------------------------
// Called by mapper_process() whenever an ABS_RX or ABS_RY event arrives.
// We just store the value — the actual pointer math happens in mouse_tick().
//
// This separation is important because axis events arrive at irregular
// intervals (whenever the stick position changes), but we want pointer
// movement at a steady 120Hz. The timer-driven tick reads whatever the
// latest axis values are.
// --------------------------------------------------------------------------
void mouse_update_axis(MouseEmulator *mouse, int axis, int value)
{
    if (axis == ABS_RX) {
        mouse->raw_x = value;
    } else if (axis == ABS_RY) {
        mouse->raw_y = value;
    }
    // Silently ignore any other axis (ABS_X, ABS_Y, etc. are not our concern)
}

// --------------------------------------------------------------------------
// mouse_update_scroll_axis — Store a new left stick axis reading
// --------------------------------------------------------------------------
// Called by the main loop whenever an ABS_X or ABS_Y event arrives from
// the left stick. We store the value here; the actual scroll math happens
// in mouse_scroll_tick() on the 120Hz timer.
// --------------------------------------------------------------------------
void mouse_update_scroll_axis(MouseEmulator *mouse, int axis, int value)
{
    if (axis == ABS_X) {
        mouse->raw_lx = value;
    } else if (axis == ABS_Y) {
        mouse->raw_ly = value;
    }
}

// --------------------------------------------------------------------------
// mouse_tick — Compute pointer movement for one 120Hz frame
// --------------------------------------------------------------------------
// This is the heart of the mouse emulation. Called every ~8.3ms when the
// timerfd fires. It converts the current stick position into pixel deltas.
//
// The algorithm:
//   1. Compute offset from center (how far the stick is deflected)
//   2. Compute magnitude (distance from center, regardless of direction)
//   3. If within deadzone → no movement, clear accumulators
//   4. Normalize to get unit direction vector (which way the stick points)
//   5. Compute "active range" — how far past the deadzone we are, as 0..1
//   6. Apply power curve: speed = active^exponent * sensitivity * max_speed
//   7. Accumulate the resulting fractional pixel movement
//   8. Extract and return integer pixels, keep the fraction for next tick
//
// Parameters:
//   dx, dy — output: integer pixel deltas to inject via uinput
//
// Returns:
//   true if there is movement to inject (dx or dy != 0), false otherwise
// --------------------------------------------------------------------------
bool mouse_tick(MouseEmulator *mouse, int *dx, int *dy)
{
    // Step 1: Compute how far the stick is from its center position.
    // Positive ox = stick pushed right, positive oy = stick pushed down.
    double ox = (double)(mouse->raw_x - mouse->center_x);
    double oy = (double)(mouse->raw_y - mouse->center_y);

    // Step 2: Compute the magnitude (distance from center).
    // This tells us how far the stick is deflected, regardless of direction.
    // We use the Pythagorean theorem: distance = sqrt(x² + y²)
    double mag = sqrt(ox * ox + oy * oy);

    // Step 3: Deadzone check.
    // If the stick is within the deadzone, treat it as centered.
    // This is crucial for preventing drift — even when you're not touching
    // the stick, it rarely reads exactly 0.
    if (mag <= (double)mouse->deadzone) {
        // Clear accumulators so we don't have leftover momentum.
        // Without this, releasing the stick could cause one last tiny
        // movement from accumulated fractions.
        mouse->accum_x = 0.0;
        mouse->accum_y = 0.0;
        *dx = 0;
        *dy = 0;
        return false;   // No movement
    }

    // Step 4: Normalize the offset to get a unit direction vector.
    // nx and ny now tell us WHICH DIRECTION the stick points, with a
    // combined magnitude of 1.0. We'll multiply by speed separately.
    double nx = ox / mag;
    double ny = oy / mag;

    // Step 5: Compute the "active range" — how far past the deadzone
    // we are, normalized to 0.0 (just past deadzone) to 1.0 (full tilt).
    // This removes the deadzone from the usable range so the cursor
    // starts moving smoothly from 0 speed right at the deadzone edge.
    double active = (mag - (double)mouse->deadzone) /
                    (32767.0 - (double)mouse->deadzone);

    // Clamp to [0.0, 1.0] in case the stick overshoots 32767
    if (active < 0.0) active = 0.0;
    if (active > 1.0) active = 1.0;

    // Step 6: Apply the power curve to compute speed.
    // pow(active, exponent) makes small deflections very slow and large
    // deflections progressively faster. With exponent=2.0:
    //   10% deflection → 1% of max speed   (very precise)
    //   50% deflection → 25% of max speed  (moderate)
    //   100% deflection → 100% of max speed (full speed)
    double speed = pow(active, mouse->exponent) *
                   mouse->sensitivity *
                   mouse->max_speed;

    // Step 7: Accumulate fractional pixel movement.
    // Most ticks produce less than 1 pixel of movement at low stick
    // deflections. We accumulate the fractions and only report whole
    // pixels. This gives smooth, sub-pixel-accurate motion without
    // the cursor "stuttering" between 0 and 1 pixel per tick.
    mouse->accum_x += nx * speed;
    mouse->accum_y += ny * speed;

    // Step 8: Extract the integer part and leave the fraction.
    // (int) truncates toward zero, which is what we want:
    //   accum_x = 3.7 → dx = 3, accum_x becomes 0.7
    //   accum_x = -2.3 → dx = -2, accum_x becomes -0.3
    *dx = (int)mouse->accum_x;
    *dy = (int)mouse->accum_y;

    // Subtract the integer part from the accumulator, preserving
    // the fractional remainder for the next tick
    mouse->accum_x -= (double)*dx;
    mouse->accum_y -= (double)*dy;

    // Return true only if we have actual pixel movement to report
    return (*dx != 0 || *dy != 0);
}

// --------------------------------------------------------------------------
// mouse_scroll_tick — Compute scroll wheel deltas for one 120Hz frame
// --------------------------------------------------------------------------
// Converts the left stick position into scroll wheel events. Uses a linear
// response (no power curve) because scrolling should feel directly
// proportional to how far you push the stick.
//
// The vertical axis is inverted: pushing the stick DOWN (positive raw_ly)
// produces a NEGATIVE REL_WHEEL value (scroll down), matching the natural
// scrolling convention where content moves with your push direction.
//
// Parameters:
//   sx, sy — output: integer scroll notch deltas for uinput
//            sx: positive = scroll right, negative = scroll left
//            sy: positive = scroll up, negative = scroll down
//
// Returns:
//   true if there is scroll movement to inject (sx or sy != 0)
// --------------------------------------------------------------------------
bool mouse_scroll_tick(MouseEmulator *mouse, int *sx, int *sy)
{
    // Compute offset from center (same as pointer emulation)
    double ox = (double)(mouse->raw_lx - mouse->center_x);
    double oy = (double)(mouse->raw_ly - mouse->center_y);

    // Compute magnitude
    double mag = sqrt(ox * ox + oy * oy);

    // Deadzone check — left stick deadzone is typically larger than right
    // stick because scroll requires more intentional input
    if (mag <= (double)mouse->scroll_deadzone) {
        mouse->scroll_accum_x = 0.0;
        mouse->scroll_accum_y = 0.0;
        *sx = 0;
        *sy = 0;
        return false;
    }

    // Normalize direction
    double nx = ox / mag;
    double ny = oy / mag;

    // Linear active range (no exponent — scroll speed is proportional
    // to deflection, which feels more natural for scrolling than the
    // quadratic curve used for pointer movement)
    double active = (mag - (double)mouse->scroll_deadzone) /
                    (32767.0 - (double)mouse->scroll_deadzone);
    if (active < 0.0) active = 0.0;
    if (active > 1.0) active = 1.0;

    // Speed = linear deflection * scroll_speed.
    // At 120Hz with scroll_speed=0.15, full stick deflection produces
    // about 18 scroll notches per second — comfortable for browsing.
    double speed = active * mouse->scroll_speed;

    // Accumulate fractional scroll movement.
    // Horizontal: positive stick = positive HWHEEL (scroll right)
    mouse->scroll_accum_x += nx * speed;
    // Vertical: INVERT because stick down (positive Y) should scroll
    // content down, which is NEGATIVE REL_WHEEL in Linux evdev.
    mouse->scroll_accum_y += -ny * speed;

    // Extract integer scroll notches
    *sx = (int)mouse->scroll_accum_x;
    *sy = (int)mouse->scroll_accum_y;

    // Preserve fractional remainder for next tick
    mouse->scroll_accum_x -= (double)*sx;
    mouse->scroll_accum_y -= (double)*sy;

    return (*sx != 0 || *sy != 0);
}

// --------------------------------------------------------------------------
// mouse_shutdown — Close the timer fd and clean up
// --------------------------------------------------------------------------
// Called during daemon shutdown. Closes the timerfd so epoll stops watching
// it. Safe to call even if init failed (checks for valid fd first).
// --------------------------------------------------------------------------
void mouse_shutdown(MouseEmulator *mouse)
{
    if (mouse->timer_fd >= 0) {
        close(mouse->timer_fd);
        mouse->timer_fd = -1;
        fprintf(stderr, "mouse: shutdown complete\n");
    }
}
