// CopyCatOS — by Kyle Blizzard at Blizzard.show

//
// scanner.h — Physical input device scanner
//
// Scans all /dev/input/event* devices and classifies them by capability.
// Unlike inputd's device.c which uses hardcoded vendor IDs, the scanner
// detects gamepads by what they can do (has analog sticks + face buttons)
// rather than who made them. This makes it work with any controller.
//
// Also provides a "press to identify" mode where the user physically
// presses a button and we report back its event type and code.
//

#ifndef SCANNER_H
#define SCANNER_H

#include <stdbool.h>

// --------------------------------------------------------------------------
// ScannedDevice — Information about one discovered input device
// --------------------------------------------------------------------------
typedef struct {
    char           path[256];      // Device node path (e.g. "/dev/input/event5")
    char           name[128];      // Human-readable name from EVIOCGNAME
    unsigned short vendor;         // USB vendor ID (from EVIOCGID)
    unsigned short product;        // USB product ID (from EVIOCGID)

    bool is_gamepad;               // Has analog sticks (ABS_X+Y) and face buttons
    bool is_system_keys;           // Has volume/media keys (KEY_VOLUMEUP, etc.)
    bool is_power_button;          // Has KEY_POWER capability
} ScannedDevice;

// We scan up to 32 input devices — most systems have 10-20.
#define MAX_SCANNED_DEVICES 32

// --------------------------------------------------------------------------
// ScanResult — All devices found during a scan
// --------------------------------------------------------------------------
typedef struct {
    ScannedDevice devices[MAX_SCANNED_DEVICES];
    int           count;           // Number of devices found
} ScanResult;

// --------------------------------------------------------------------------
// IdentifiedEvent — Result of "press to identify" mode
// --------------------------------------------------------------------------
typedef struct {
    int  ev_type;                  // Event type (EV_KEY, EV_ABS, etc.)
    int  ev_code;                  // Event code (BTN_SOUTH, ABS_X, etc.)
    int  ev_value;                 // Event value (1=press, 0=release, or axis value)
    char device_path[256];         // Which device it came from
    char device_name[128];         // Name of the device
} IdentifiedEvent;

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------

// scanner_scan_all — Enumerate all /dev/input/event* devices using libudev.
// Reads each device's capabilities via ioctl and classifies it.
// Fills `result` with the discovered devices.
// Returns 0 on success, -1 if libudev initialization fails.
int scanner_scan_all(ScanResult *result);

// scanner_identify_button — "Press to identify" mode.
// Opens all detected gamepad devices and waits for the next button press
// or axis movement. When an event arrives, fills `out` with the details.
// `timeout_ms` is how long to wait (0 = block forever).
// Returns 0 on success, -1 on timeout or error.
int scanner_identify_button(const ScanResult *devices,
                            IdentifiedEvent *out,
                            int timeout_ms);

// scanner_code_to_name — Convert an event type+code pair to a human-readable
// string like "BTN_SOUTH" or "ABS_X". Returns "UNKNOWN_XXX" for unrecognized
// codes. The returned string is statically allocated (do not free).
const char *scanner_code_to_name(int ev_type, int ev_code);

// scanner_name_to_code — Reverse lookup: convert a string like "BTN_SOUTH"
// to its numeric event code. Returns -1 for unrecognized names.
int scanner_name_to_code(const char *name);

#endif // SCANNER_H
