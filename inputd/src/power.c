// CopyCatOS — by Kyle Blizzard at Blizzard.show

//
// power.c — Power button state machine implementation
//
// Handles the system power button (KEY_POWER from /dev/input/event*).
// The Legion Go has a physical power button that Linux sees as a normal
// input key event. We intercept it to provide custom behavior:
//
//   Short press  (<700ms):           Suspend the system (sleep)
//   Medium press (700ms - 3000ms):   Ignored (no action, safety gap)
//   Long press   (>=3000ms):         Reboot the system
//
// The "medium press" dead zone prevents accidental reboots. If you hold
// the button for a second or two trying to suspend, you won't accidentally
// trigger a reboot. You have to deliberately hold it for 3 full seconds.
//
// The state machine:
//
//   IDLE ──press──> PRESSED ──release(<700ms)──> IDLE + SUSPEND
//                     │       ──release(700-3000ms)──> IDLE (no action)
//                     │       ──hold(>=3000ms)──> LONG_PENDING + RESTART
//                     │
//                   LONG_PENDING ──release──> IDLE (action already taken)
//
// power_check_hold() allows the main loop to detect long presses WITHOUT
// waiting for the button to be released. This is important because a user
// holding the power button for 3 seconds expects the reboot to happen
// immediately, not after they lift their finger.
//

#include "power.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

// --------------------------------------------------------------------------
// Helper: Compute elapsed milliseconds since a given timespec
// --------------------------------------------------------------------------
// Uses CLOCK_MONOTONIC (same clock we record press_time with) to get
// the current time, then computes the difference in milliseconds.
// CLOCK_MONOTONIC is important because it's unaffected by system time
// changes (NTP adjustments, manual date changes, etc.).
// --------------------------------------------------------------------------
static long elapsed_ms(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // Convert both timestamps to milliseconds and subtract.
    // tv_sec is whole seconds, tv_nsec is the nanosecond remainder.
    // 1 second = 1,000 milliseconds = 1,000,000,000 nanoseconds.
    long ms = (now.tv_sec - start->tv_sec) * 1000L +
              (now.tv_nsec - start->tv_nsec) / 1000000L;

    return ms;
}

// --------------------------------------------------------------------------
// power_init — Initialize the power button state machine
// --------------------------------------------------------------------------
// Sets the state to IDLE and configures the default press duration
// thresholds. Call this once during daemon startup.
// --------------------------------------------------------------------------
void power_init(PowerButton *pb)
{
    // Zero everything first for clean state
    memset(pb, 0, sizeof(PowerButton));

    // Start in idle state — no button pressed
    pb->state = POWER_IDLE;

    // Short press threshold: 700 milliseconds.
    // A quick tap (under 700ms) triggers suspend. This gives enough
    // time to register a deliberate press without being too fast
    // for natural button presses.
    pb->short_press_ms = 700;

    // Long press threshold: 3000 milliseconds (3 seconds).
    // Holding the button for this long triggers a reboot. The gap
    // between 700ms and 3000ms is intentionally dead — it prevents
    // accidental reboots when you meant to suspend but held the
    // button a bit too long.
    pb->long_press_ms = 3000;

    fprintf(stderr, "power: initialized — short_press=%dms, long_press=%dms\n",
            pb->short_press_ms, pb->long_press_ms);
}

// --------------------------------------------------------------------------
// power_handle — Process a KEY_POWER press or release
// --------------------------------------------------------------------------
// Called by the main loop when a KEY_POWER event arrives from evdev.
//
// Parameters:
//   value — 1 for key down (press), 0 for key up (release)
//
// Returns:
//   The PowerAction that should be executed, if any. The caller is
//   responsible for passing this to power_execute() if it's not NONE.
// --------------------------------------------------------------------------
PowerAction power_handle(PowerButton *pb, int value)
{
    // ---- Key down (button pressed) ----
    if (value == 1) {
        // Record the exact time the button was pressed.
        // We'll compare against this when the button is released (or when
        // power_check_hold detects a long press) to determine which action
        // to take.
        clock_gettime(CLOCK_MONOTONIC, &pb->press_time);
        pb->state = POWER_PRESSED;

        fprintf(stderr, "power: button pressed, timing...\n");
        return POWER_ACTION_NONE;   // No action yet — wait for release or timeout
    }

    // ---- Key up (button released) ----
    if (value == 0) {
        if (pb->state == POWER_PRESSED) {
            // Button was released while we were timing it.
            // Compute how long it was held and decide what to do.
            long held = elapsed_ms(&pb->press_time);

            // Return to idle state regardless of which action we take
            pb->state = POWER_IDLE;

            if (held < pb->short_press_ms) {
                // Quick tap — suspend the system
                fprintf(stderr, "power: short press (%ldms) — suspend\n", held);
                return POWER_ACTION_SUSPEND;
            } else if (held >= pb->long_press_ms) {
                // Very long hold — but this case should normally be caught
                // by power_check_hold() before release. If the main loop
                // doesn't poll check_hold fast enough, we catch it here
                // as a fallback.
                fprintf(stderr, "power: long press (%ldms) — restart\n", held);
                return POWER_ACTION_RESTART;
            } else {
                // Medium press (between thresholds) — intentionally ignored.
                // This dead zone prevents accidental reboots.
                fprintf(stderr, "power: medium press (%ldms) — no action\n", held);
                return POWER_ACTION_NONE;
            }
        }

        if (pb->state == POWER_LONG_PENDING) {
            // The long-press action was already triggered by
            // power_check_hold() while the button was still held.
            // Just return to idle; the action has already been dispatched.
            pb->state = POWER_IDLE;
            fprintf(stderr, "power: released after long-press (already handled)\n");
            return POWER_ACTION_NONE;
        }
    }

    // Shouldn't reach here under normal operation, but handle gracefully
    return POWER_ACTION_NONE;
}

// --------------------------------------------------------------------------
// power_check_hold — Detect long press while button is still held
// --------------------------------------------------------------------------
// Called periodically from the main epoll loop (e.g. each iteration or on
// a timer). If the button has been held for >= long_press_ms, triggers the
// reboot action immediately without waiting for the button to be released.
//
// This is important for user experience: when you hold the power button
// for 3 seconds expecting a reboot, you want it to happen RIGHT THEN,
// not after you release the button.
//
// Returns POWER_ACTION_RESTART if the threshold was just crossed, or
// POWER_ACTION_NONE otherwise.
// --------------------------------------------------------------------------
PowerAction power_check_hold(PowerButton *pb)
{
    // Only check if the button is currently being held
    if (pb->state != POWER_PRESSED) {
        return POWER_ACTION_NONE;
    }

    long held = elapsed_ms(&pb->press_time);

    if (held >= pb->long_press_ms) {
        // The button has been held long enough — trigger reboot.
        // Transition to LONG_PENDING so we don't trigger again on
        // subsequent calls or on release.
        pb->state = POWER_LONG_PENDING;
        fprintf(stderr, "power: long hold detected (%ldms) — restart\n", held);
        return POWER_ACTION_RESTART;
    }

    return POWER_ACTION_NONE;
}

// --------------------------------------------------------------------------
// power_execute — Carry out a power action using systemctl
// --------------------------------------------------------------------------
// Actually performs the suspend or reboot by calling systemctl. This is
// separated from the decision logic (power_handle / power_check_hold)
// so the caller can do any cleanup (release grabs, flush buffers, etc.)
// before the system goes down.
//
// Uses system() which blocks until the command completes. For suspend,
// this means the call blocks until the system wakes back up. For reboot,
// the system goes down and this function never returns.
// --------------------------------------------------------------------------
void power_execute(PowerAction action)
{
    switch (action) {
    case POWER_ACTION_SUSPEND:
        fprintf(stderr, "power: executing suspend (systemctl suspend)\n");
        // system() runs the command in a subshell. For suspend, the kernel
        // freezes the system after this call and resumes when the user
        // presses the power button again.
        system("systemctl suspend");
        break;

    case POWER_ACTION_RESTART:
        fprintf(stderr, "power: executing reboot (systemctl reboot)\n");
        // This will shut down the system. The process won't continue
        // past this point under normal circumstances.
        system("systemctl reboot");
        break;

    case POWER_ACTION_NONE:
        // Nothing to do — this shouldn't normally be called with NONE,
        // but handle it gracefully just in case.
        break;
    }
}
