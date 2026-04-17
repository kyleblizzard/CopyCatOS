// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

//
// hid.h — Raw HID report parser for the Legion Go S vendor interface
//
// The Legion Go S WCH.cn controller has a vendor-specific HID interface
// (usage page 0xFFA0) that streams 64-byte reports at ~100Hz. These
// reports contain button state and IMU data that isn't available through
// the standard XInput evdev interface.
//
// In "FPS mode" (the default firmware mode), this interface reports:
//   - Bytes 0-2: Digital button state (24 individual bits)
//   - Bytes 14-25: IMU data (accelerometer + gyroscope, 6 x int16 LE)
//
// Analog sticks and triggers are NOT in these reports — they come from
// the XInput evdev interface (/dev/input/event11) which works alongside
// the vendor hidraw. This is the same hybrid approach used by the hhd
// (Handheld Daemon) project.
//
// The button bit positions were reverse-engineered by the hhd project
// using the BM (Button Map) format from the WCH.cn firmware:
//   loc = (byte_offset << 3) + bit_position
//   extraction: byte = loc / 8, bit = 7 - (loc % 8)   [MSB-first]
//

#ifndef HID_H
#define HID_H

#include <stdbool.h>
#include <stdint.h>
#include <linux/input.h>

// --------------------------------------------------------------------------
// Report structure constants
// --------------------------------------------------------------------------

// Each vendor HID report is 32 bytes (one sub-frame).
// An earlier test with a 64-byte read returned two 32-byte reports
// concatenated; the actual per-report size is 32 bytes.
#define HID_REPORT_SIZE 32

// Button state occupies the first 3 bytes (24 bits)
#define HID_BUTTON_BYTES 3

// --------------------------------------------------------------------------
// Button bit extraction macro
// --------------------------------------------------------------------------
// Given a report buffer and a bit location, extract a single button bit.
// The WCH.cn firmware uses MSB-first bit ordering within each byte:
//   bit 7 = leftmost/highest, bit 0 = rightmost/lowest
//
// Example: Legion L is at loc=7, which means byte 0, bit 7 (MSB of byte 0)
// --------------------------------------------------------------------------
#define HID_BTN(report, loc) \
    (((report)[(loc) / 8] >> (7 - ((loc) % 8))) & 1)

// --------------------------------------------------------------------------
// Button bit locations within the 3-byte button field
// --------------------------------------------------------------------------
// These are "loc" values for the HID_BTN macro. Each loc encodes both
// the byte offset (loc/8) and the bit position within that byte (7-loc%8).
//
// Byte 0 (bits 0-7):
//   Bit 7 = Legion L (the left Lenovo button, acts as "mode" key)
//   Bit 6 = Legion R (the right Lenovo button, acts as "share" key)
//   Bit 5 = L3 (left stick click)
//   Bit 4 = R3 (right stick click)
//   Bit 3 = D-pad Up
//   Bit 2 = D-pad Down
//   Bit 1 = D-pad Left
//   Bit 0 = D-pad Right
//
// Byte 1 (bits 8-15):
//   Bit 7 = A (BTN_SOUTH equivalent)
//   Bit 6 = B (BTN_EAST equivalent)
//   Bit 5 = X (BTN_WEST equivalent)
//   Bit 4 = Y (BTN_NORTH equivalent)
//   Bit 3 = LB (left bumper)
//   Bit 2 = LT digital (left trigger fully pressed)
//   Bit 1 = RB (right bumper)
//   Bit 0 = RT digital (right trigger fully pressed)
//
// Byte 2 (bits 16-23):
//   Bit 7 = Y1 (left back paddle)
//   Bit 6 = Y2 (right back paddle)
//   Bit 1 = View/Select
//   Bit 0 = Menu/Start
// --------------------------------------------------------------------------

// Byte 0 buttons
#define HID_LOC_LEGION_L     7    // Legion L button (mode)
#define HID_LOC_LEGION_R     6    // Legion R button (share)
#define HID_LOC_L3           5    // Left stick click
#define HID_LOC_R3           4    // Right stick click
#define HID_LOC_DPAD_UP      3    // D-pad up
#define HID_LOC_DPAD_DOWN    2    // D-pad down
#define HID_LOC_DPAD_LEFT    1    // D-pad left
#define HID_LOC_DPAD_RIGHT   0    // D-pad right

// Byte 1 buttons
#define HID_LOC_A           15    // A / South face button
#define HID_LOC_B           14    // B / East face button
#define HID_LOC_X           13    // X / West face button
#define HID_LOC_Y           12    // Y / North face button
#define HID_LOC_LB          11    // Left bumper
#define HID_LOC_LT_DIG      10    // Left trigger digital (full pull)
#define HID_LOC_RB           9    // Right bumper
#define HID_LOC_RT_DIG       8    // Right trigger digital (full pull)

// Byte 2 buttons
#define HID_LOC_Y1          23    // Y1 back paddle (left)
#define HID_LOC_Y2          22    // Y2 back paddle (right)
#define HID_LOC_VIEW        17    // View / Select button
#define HID_LOC_MENU        16    // Menu / Start button

// --------------------------------------------------------------------------
// Total number of buttons we track for change detection
// --------------------------------------------------------------------------
#define HID_NUM_BUTTONS 18

// --------------------------------------------------------------------------
// HidButton — Maps a HID bit location to an evdev event code
// --------------------------------------------------------------------------
// Each entry in the button table links a physical button (identified by
// its bit position in the HID report) to the evdev code we'll use when
// generating synthetic input_event structs. The mapper then processes
// these synthetic events identically to real evdev events.
// --------------------------------------------------------------------------
typedef struct HidButton {
    int     loc;         // Bit location in HID report (for HID_BTN macro)
    uint16_t ev_type;    // evdev event type (EV_KEY or EV_ABS for d-pad)
    uint16_t ev_code;    // evdev event code (e.g. BTN_SOUTH, KEY_UP)
    int     ev_value;    // For d-pad: the axis value (-1 or +1); for buttons: unused (0)
} HidButton;

// --------------------------------------------------------------------------
// HidParser — Tracks button state and detects changes between reports
// --------------------------------------------------------------------------
typedef struct HidParser {
    // Previous button state for each tracked button (0 or 1).
    // We compare current vs previous to detect press/release transitions.
    uint8_t prev_state[HID_NUM_BUTTONS];

    // The button mapping table — links HID bit positions to evdev codes.
    // Initialized once by hid_init().
    HidButton buttons[HID_NUM_BUTTONS];

    // True after the first report has been processed. We skip change
    // detection on the very first report to avoid spurious events from
    // whatever random state the hardware starts in.
    bool initialized;

    // Debug: print raw bytes when any button state changes.
    // Useful for verifying button bit positions during development.
    bool debug_log;
} HidParser;

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------

// hid_init — Initialize the parser and populate the button mapping table.
// Call once at startup.
void hid_init(HidParser *parser);

// hid_parse — Parse a 64-byte vendor HID report and generate synthetic
// input_event structs for any buttons that changed state since the last
// report.
//
// Parameters:
//   parser  — the parser state (tracks previous button state)
//   report  — pointer to the 64-byte HID report buffer
//   len     — number of bytes actually read (must be >= HID_BUTTON_BYTES)
//   events  — output array to fill with synthetic input_events
//   max_events — size of the events array
//
// Returns the number of events written to the events array.
// Each event has type EV_KEY (for buttons) or EV_ABS (for d-pad axes),
// ready to be fed directly into mapper_process().
int hid_parse(HidParser *parser, const uint8_t *report, int len,
              struct input_event *events, int max_events);

#endif // HID_H
