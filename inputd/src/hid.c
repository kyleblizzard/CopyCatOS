// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

//
// hid.c — Raw HID report parser for the Legion Go S vendor interface
//
// Parses 64-byte vendor HID reports from the WCH.cn controller and
// converts button state changes into synthetic evdev input_event structs.
// These synthetic events are then fed into the mapper engine exactly like
// real evdev events, so the rest of cc-inputd doesn't need to know whether
// a button press came from evdev or hidraw.
//
// The approach:
//   1. Read the 3 button bytes from the front of each report
//   2. Extract each tracked button's state using the HID_BTN macro
//   3. Compare against the previous report's state
//   4. For any button that changed, generate an input_event:
//      - Regular buttons: EV_KEY with value 1 (press) or 0 (release)
//      - D-pad buttons: EV_ABS with value -1/+1 (press) or 0 (release)
//   5. Store current state as "previous" for next comparison
//
// D-pad handling is slightly tricky: the HID report has 4 individual bits
// (up/down/left/right), but evdev represents the d-pad as two axes
// (ABS_HAT0X for left/right, ABS_HAT0Y for up/down). We convert from
// individual bits to axis values so the mapper's existing d-pad logic works.
//

#include "hid.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <linux/input-event-codes.h>

// --------------------------------------------------------------------------
// hid_init — Populate the button mapping table
// --------------------------------------------------------------------------
// Sets up the mapping between HID bit locations and evdev event codes.
// The order of entries in the table doesn't matter for correctness, but
// we group them logically (face buttons, shoulders, d-pad, etc.) for
// readability.
// --------------------------------------------------------------------------
void hid_init(HidParser *parser)
{
    memset(parser, 0, sizeof(HidParser));

    int i = 0;

    // --- Face buttons → standard gamepad button codes ---
    // These map 1:1 to the evdev codes the mapper already knows about.
    // BTN_SOUTH/EAST/WEST/NORTH are the canonical Linux names for
    // A/B/X/Y on an Xbox-layout controller.
    parser->buttons[i++] = (HidButton){ HID_LOC_A,  EV_KEY, BTN_SOUTH, 0 };
    parser->buttons[i++] = (HidButton){ HID_LOC_B,  EV_KEY, BTN_EAST,  0 };
    parser->buttons[i++] = (HidButton){ HID_LOC_X,  EV_KEY, BTN_WEST,  0 };
    parser->buttons[i++] = (HidButton){ HID_LOC_Y,  EV_KEY, BTN_NORTH, 0 };

    // --- Shoulder buttons ---
    parser->buttons[i++] = (HidButton){ HID_LOC_LB, EV_KEY, BTN_TL, 0 };
    parser->buttons[i++] = (HidButton){ HID_LOC_RB, EV_KEY, BTN_TR, 0 };

    // --- Stick clicks ---
    parser->buttons[i++] = (HidButton){ HID_LOC_L3, EV_KEY, BTN_THUMBL, 0 };
    parser->buttons[i++] = (HidButton){ HID_LOC_R3, EV_KEY, BTN_THUMBR, 0 };

    // --- Center buttons ---
    parser->buttons[i++] = (HidButton){ HID_LOC_VIEW, EV_KEY, BTN_SELECT, 0 };
    parser->buttons[i++] = (HidButton){ HID_LOC_MENU, EV_KEY, BTN_START,  0 };

    // --- Legion buttons (unique to Legion Go, no standard evdev code) ---
    // We use BTN_MODE for Legion L (the more prominent button, used for
    // Spotlight) and BTN_Z for Legion R (less commonly used). BTN_Z is
    // a standard gamepad code that's otherwise unused on this controller.
    parser->buttons[i++] = (HidButton){ HID_LOC_LEGION_L, EV_KEY, BTN_MODE,   0 };
    parser->buttons[i++] = (HidButton){ HID_LOC_LEGION_R, EV_KEY, BTN_Z,      0 };

    // --- Back paddles (unique to Legion Go) ---
    // Y1 and Y2 are extra buttons on the back. We map them to BTN_TL2
    // and BTN_TR2 (typically used for "extra" trigger buttons in Linux).
    parser->buttons[i++] = (HidButton){ HID_LOC_Y1, EV_KEY, BTN_TL2, 0 };
    parser->buttons[i++] = (HidButton){ HID_LOC_Y2, EV_KEY, BTN_TR2, 0 };

    // --- D-pad → axis events ---
    // The HID report has individual bits for each direction. We convert
    // them to ABS_HAT0X/Y axis values to match what the mapper expects.
    // ev_value stores the axis value to emit when the button is pressed:
    //   Up    → ABS_HAT0Y = -1
    //   Down  → ABS_HAT0Y = +1
    //   Left  → ABS_HAT0X = -1
    //   Right → ABS_HAT0X = +1
    // On release, we emit 0 for the axis (handled in hid_parse).
    parser->buttons[i++] = (HidButton){ HID_LOC_DPAD_UP,    EV_ABS, ABS_HAT0Y, -1 };
    parser->buttons[i++] = (HidButton){ HID_LOC_DPAD_DOWN,  EV_ABS, ABS_HAT0Y, +1 };
    parser->buttons[i++] = (HidButton){ HID_LOC_DPAD_LEFT,  EV_ABS, ABS_HAT0X, -1 };
    parser->buttons[i++] = (HidButton){ HID_LOC_DPAD_RIGHT, EV_ABS, ABS_HAT0X, +1 };

    // Note: We intentionally skip HID_LOC_LT_DIG and HID_LOC_RT_DIG.
    // The digital trigger bits only indicate "fully pressed" — they don't
    // provide analog values. We get proper analog trigger data (ABS_Z/RZ
    // with 0-1023 range) from the XInput evdev interface, which is far
    // more useful for both desktop scroll emulation and game passthrough.

    parser->initialized = false;
    parser->debug_log   = true;   // Enable during development

    fprintf(stderr, "[cc-inputd] HID parser initialized: %d buttons tracked\n", i);
}

// --------------------------------------------------------------------------
// hid_parse — Process one HID report and emit change events
// --------------------------------------------------------------------------
// Compares the current button state against the previous report and
// generates synthetic input_event structs for any changed buttons.
//
// For regular buttons (EV_KEY):
//   - Press:   ev.value = 1
//   - Release: ev.value = 0
//
// For d-pad buttons (EV_ABS):
//   - Press:   ev.value = the direction value (-1 or +1)
//   - Release: ev.value = 0 (axis returned to center)
//
// Returns the number of events written to the output array.
// --------------------------------------------------------------------------
int hid_parse(HidParser *parser, const uint8_t *report, int len,
              struct input_event *events, int max_events)
{
    // Need at least the button bytes to do anything
    if (len < HID_BUTTON_BYTES) {
        return 0;
    }

    int event_count = 0;

    // Get the current timestamp for generated events.
    // Using CLOCK_MONOTONIC is fine — the mapper doesn't care about
    // wall-clock time, it just needs a monotonically increasing value.
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    // Track whether any button changed (for debug logging)
    bool any_changed = false;

    // Check each tracked button for state changes
    for (int i = 0; i < HID_NUM_BUTTONS; i++) {
        const HidButton *btn = &parser->buttons[i];

        // Extract current state: 0 (released) or 1 (pressed)
        uint8_t current = HID_BTN(report, btn->loc);

        // On the first report, just record state without generating events.
        // This prevents spurious press events from whatever state the
        // controller was in when we started reading.
        if (!parser->initialized) {
            parser->prev_state[i] = current;
            continue;
        }

        // Compare with previous state — only generate events on transitions
        if (current != parser->prev_state[i]) {
            any_changed = true;

            if (event_count < max_events) {
                struct input_event *ev = &events[event_count];
                ev->time.tv_sec  = ts.tv_sec;
                ev->time.tv_usec = ts.tv_nsec / 1000;
                ev->type  = btn->ev_type;
                ev->code  = btn->ev_code;

                if (btn->ev_type == EV_ABS) {
                    // D-pad axis: emit the direction value on press, 0 on release
                    ev->value = current ? btn->ev_value : 0;
                } else {
                    // Regular button: 1 = pressed, 0 = released
                    ev->value = current;
                }

                event_count++;
            }

            // Update stored state
            parser->prev_state[i] = current;
        }
    }

    // Mark as initialized after processing the first report
    if (!parser->initialized) {
        parser->initialized = true;
        fprintf(stderr, "[cc-inputd] HID parser: first report received, "
                "buttons armed (b0=%02x b1=%02x b2=%02x)\n",
                report[0], report[1], report[2]);
    }

    // Debug: log raw button bytes and which button changed.
    // This is invaluable for verifying button bit positions — the first
    // time someone presses a button, the log shows exactly which bits
    // flipped, making it easy to spot mapping errors.
    if (any_changed && parser->debug_log) {
        fprintf(stderr, "[cc-inputd] HID buttons changed: "
                "%02x %02x %02x (%d events) →",
                report[0], report[1], report[2], event_count);
        // Show which buttons are currently pressed
        for (int i = 0; i < HID_NUM_BUTTONS; i++) {
            if (parser->prev_state[i]) {
                static const char *btn_names[] = {
                    "A", "B", "X", "Y", "LB", "RB", "L3", "R3",
                    "View", "Menu", "LegionL", "LegionR",
                    "Y1", "Y2", "DUp", "DDown", "DLeft", "DRight"
                };
                fprintf(stderr, " %s", btn_names[i]);
            }
        }
        fprintf(stderr, "\n");
    }

    return event_count;
}
