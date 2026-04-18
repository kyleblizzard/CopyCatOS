// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// scanner.c — Physical input device scanner implementation
// ============================================================================
//
// Enumerates Linux input devices using libudev (same approach as inputd's
// device.c), but classifies them by capability rather than vendor ID. This
// makes the scanner hardware-agnostic — it works with any gamepad, not just
// the Legion Go.
//
// Capability-based detection logic:
//   Gamepad = has EV_ABS (analog axes) + ABS_X + ABS_Y + EV_KEY + BTN_SOUTH
//   System keys = has KEY_VOLUMEUP + KEY_VOLUMEDOWN (but is NOT a gamepad)
//   Power button = has KEY_POWER
//
// ============================================================================

#include "scanner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>

#include <linux/input.h>
#include <libudev.h>

// ============================================================================
//  Helper: test_bit — Check if a specific bit is set in a bitfield array
// ============================================================================
// The kernel returns capability bitmasks as arrays of unsigned longs.
// Each long holds (sizeof(long) * 8) bits. This checks if bit `bit` is set.
// ============================================================================

static inline bool test_bit(unsigned int bit, const unsigned long *arr) {
    return (arr[bit / (sizeof(long) * 8)] >> (bit % (sizeof(long) * 8))) & 1;
}

// ============================================================================
//  Name/code lookup tables
// ============================================================================
// Maps between human-readable names (like "BTN_SOUTH") and their numeric
// Linux input event codes. Used by scanner_code_to_name() and
// scanner_name_to_code() for display and config parsing.
//
// Organized by category: gamepad buttons, axes, keyboard keys, mouse buttons.
// ============================================================================

typedef struct {
    const char *name;
    int         ev_type;      // EV_KEY, EV_ABS, etc.
    int         code;
} CodeEntry;

static const CodeEntry code_table[] = {
    // --- Gamepad face buttons ---
    { "BTN_SOUTH",   EV_KEY, BTN_SOUTH },
    { "BTN_EAST",    EV_KEY, BTN_EAST },
    { "BTN_NORTH",   EV_KEY, BTN_NORTH },
    { "BTN_WEST",    EV_KEY, BTN_WEST },

    // --- Gamepad shoulder buttons ---
    { "BTN_TL",      EV_KEY, BTN_TL },
    { "BTN_TR",      EV_KEY, BTN_TR },
    { "BTN_TL2",     EV_KEY, BTN_TL2 },
    { "BTN_TR2",     EV_KEY, BTN_TR2 },

    // --- Gamepad center buttons ---
    { "BTN_SELECT",  EV_KEY, BTN_SELECT },
    { "BTN_START",   EV_KEY, BTN_START },
    { "BTN_MODE",    EV_KEY, BTN_MODE },

    // --- Stick clicks ---
    { "BTN_THUMBL",  EV_KEY, BTN_THUMBL },
    { "BTN_THUMBR",  EV_KEY, BTN_THUMBR },

    // --- Absolute axes (analog sticks, triggers, d-pad) ---
    { "ABS_X",       EV_ABS, ABS_X },
    { "ABS_Y",       EV_ABS, ABS_Y },
    { "ABS_RX",      EV_ABS, ABS_RX },
    { "ABS_RY",      EV_ABS, ABS_RY },
    { "ABS_Z",       EV_ABS, ABS_Z },
    { "ABS_RZ",      EV_ABS, ABS_RZ },
    { "ABS_HAT0X",   EV_ABS, ABS_HAT0X },
    { "ABS_HAT0Y",   EV_ABS, ABS_HAT0Y },

    // --- Mouse buttons ---
    { "BTN_LEFT",    EV_KEY, BTN_LEFT },
    { "BTN_RIGHT",   EV_KEY, BTN_RIGHT },
    { "BTN_MIDDLE",  EV_KEY, BTN_MIDDLE },

    // --- Common keyboard keys ---
    { "KEY_ENTER",       EV_KEY, KEY_ENTER },
    { "KEY_ESC",         EV_KEY, KEY_ESC },
    { "KEY_SPACE",       EV_KEY, KEY_SPACE },
    { "KEY_TAB",         EV_KEY, KEY_TAB },
    { "KEY_BACKSPACE",   EV_KEY, KEY_BACKSPACE },
    { "KEY_UP",          EV_KEY, KEY_UP },
    { "KEY_DOWN",        EV_KEY, KEY_DOWN },
    { "KEY_LEFT",        EV_KEY, KEY_LEFT },
    { "KEY_RIGHT",       EV_KEY, KEY_RIGHT },
    { "KEY_PAGEUP",      EV_KEY, KEY_PAGEUP },
    { "KEY_PAGEDOWN",    EV_KEY, KEY_PAGEDOWN },
    { "KEY_HOME",        EV_KEY, KEY_HOME },
    { "KEY_END",         EV_KEY, KEY_END },
    { "KEY_DELETE",      EV_KEY, KEY_DELETE },

    // --- System keys ---
    { "KEY_POWER",       EV_KEY, KEY_POWER },
    { "KEY_VOLUMEUP",    EV_KEY, KEY_VOLUMEUP },
    { "KEY_VOLUMEDOWN",  EV_KEY, KEY_VOLUMEDOWN },
    { "KEY_MUTE",        EV_KEY, KEY_MUTE },

    // --- Modifier keys ---
    { "KEY_LEFTCTRL",    EV_KEY, KEY_LEFTCTRL },
    { "KEY_LEFTSHIFT",   EV_KEY, KEY_LEFTSHIFT },
    { "KEY_LEFTALT",     EV_KEY, KEY_LEFTALT },
    { "KEY_LEFTMETA",    EV_KEY, KEY_LEFTMETA },

    // Sentinel
    { NULL, 0, -1 }
};

// ============================================================================
//  scanner_code_to_name — Look up the human-readable name for an event code
// ============================================================================

const char *scanner_code_to_name(int ev_type, int ev_code) {
    for (int i = 0; code_table[i].name != NULL; i++) {
        if (code_table[i].ev_type == ev_type &&
            code_table[i].code == ev_code) {
            return code_table[i].name;
        }
    }

    // Unknown code — return a static buffer with the numeric value.
    // We use a small ring of buffers so multiple calls don't overwrite
    // each other (up to 4 concurrent uses are safe).
    static char bufs[4][32];
    static int idx = 0;
    char *buf = bufs[idx++ % 4];

    const char *type_str = (ev_type == EV_KEY) ? "KEY" :
                           (ev_type == EV_ABS) ? "ABS" : "EVT";
    snprintf(buf, 32, "%s_%d", type_str, ev_code);
    return buf;
}

// ============================================================================
//  scanner_name_to_code — Reverse lookup: name string to numeric code
// ============================================================================

int scanner_name_to_code(const char *name) {
    if (!name || !*name) return -1;

    for (int i = 0; code_table[i].name != NULL; i++) {
        if (strcmp(name, code_table[i].name) == 0) {
            return code_table[i].code;
        }
    }

    // Try parsing as a raw integer (e.g. "304" for BTN_SOUTH)
    char *endptr = NULL;
    long val = strtol(name, &endptr, 0);
    if (endptr && *endptr == '\0' && val >= 0) {
        return (int)val;
    }

    return -1;
}

// ============================================================================
//  classify_device — Open a device node and determine its type
// ============================================================================
// Opens the device, reads its name and ID via ioctl, then checks its
// capability bits to classify it as a gamepad, system keys device,
// power button, or none of the above.
// ============================================================================

static bool classify_device(const char *path, ScannedDevice *out) {
    // Open the device node in read-only, non-blocking mode.
    // We don't grab it — we're just inspecting capabilities.
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;

    // Read the human-readable name
    if (ioctl(fd, EVIOCGNAME(sizeof(out->name)), out->name) < 0) {
        snprintf(out->name, sizeof(out->name), "Unknown");
    }

    // Read the vendor and product IDs
    struct input_id id = {0};
    if (ioctl(fd, EVIOCGID, &id) >= 0) {
        out->vendor  = id.vendor;
        out->product = id.product;
    }

    // Store the path
    snprintf(out->path, sizeof(out->path), "%s", path);

    // --- Read capability bitmasks ---
    // The kernel exposes what each device can do through EVIOCGBIT ioctls.
    // We query which event types it supports, then which specific codes
    // within each type.

    unsigned long abs_bits[(ABS_CNT + sizeof(long) * 8 - 1) / (sizeof(long) * 8)];
    unsigned long key_bits[(KEY_CNT + sizeof(long) * 8 - 1) / (sizeof(long) * 8)];
    memset(abs_bits, 0, sizeof(abs_bits));
    memset(key_bits, 0, sizeof(key_bits));

    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);

    // --- Classify ---

    // Gamepad: has both analog sticks (ABS_X + ABS_Y) and at least one
    // standard face button (BTN_SOUTH). This catches Xbox, PlayStation,
    // and generic controllers without needing vendor-specific logic.
    bool has_sticks = test_bit(ABS_X, abs_bits) && test_bit(ABS_Y, abs_bits);
    bool has_face   = test_bit(BTN_SOUTH, key_bits);
    out->is_gamepad = has_sticks && has_face;

    // System keys: has volume controls but is NOT a gamepad.
    // On the Legion Go S, this is event2 ("wch.cn Legion Go S").
    if (!out->is_gamepad) {
        out->is_system_keys = test_bit(KEY_VOLUMEUP, key_bits) &&
                              test_bit(KEY_VOLUMEDOWN, key_bits);
    }

    // Power button: has KEY_POWER capability
    out->is_power_button = test_bit(KEY_POWER, key_bits);

    close(fd);
    return true;
}

// ============================================================================
//  scanner_scan_all — Enumerate all input devices using libudev
// ============================================================================
// Creates a udev context, enumerates the "input" subsystem, and classifies
// each event* device node. This is the same enumeration pattern used by
// inputd's device_init(), but we classify by capability instead of
// matching hardcoded vendor/product IDs.
// ============================================================================

int scanner_scan_all(ScanResult *result) {
    memset(result, 0, sizeof(*result));

    // Create the libudev context — the entry point for all udev operations
    struct udev *udev = udev_new();
    if (!udev) {
        fprintf(stderr, "scanner: udev_new() failed\n");
        return -1;
    }

    // Set up enumeration filtered to only "input" subsystem devices
    struct udev_enumerate *enumerate = udev_enumerate_new(udev);
    if (!enumerate) {
        udev_unref(udev);
        return -1;
    }

    udev_enumerate_add_match_subsystem(enumerate, "input");
    udev_enumerate_scan_devices(enumerate);

    // Walk through every matching device
    struct udev_list_entry *entry;
    struct udev_list_entry *list = udev_enumerate_get_list_entry(enumerate);
    udev_list_entry_foreach(entry, list) {
        // Bail if we've filled our array
        if (result->count >= MAX_SCANNED_DEVICES) break;

        const char *syspath = udev_list_entry_get_name(entry);
        struct udev_device *dev = udev_device_new_from_syspath(udev, syspath);
        if (!dev) continue;

        const char *devnode = udev_device_get_devnode(dev);
        if (!devnode) {
            udev_device_unref(dev);
            continue;
        }

        // Only process event* nodes (skip input* parent devices)
        const char *sysname = udev_device_get_sysname(dev);
        if (!sysname || strncmp(sysname, "event", 5) != 0) {
            udev_device_unref(dev);
            continue;
        }

        // Classify this device and add it if it's interesting
        ScannedDevice *sd = &result->devices[result->count];
        if (classify_device(devnode, sd)) {
            // Only keep devices that are gamepads, system keys, or power buttons
            if (sd->is_gamepad || sd->is_system_keys || sd->is_power_button) {
                result->count++;
            }
        }

        udev_device_unref(dev);
    }

    udev_enumerate_unref(enumerate);
    udev_unref(udev);

    return 0;
}

// ============================================================================
//  scanner_identify_button — "Press to identify" interactive mode
// ============================================================================
// Opens all gamepad devices from a previous scan, uses poll() to wait for
// the next button press or significant axis movement, then reports back
// what was pressed.
//
// This is the key feature for the button mapper UI — the user presses a
// physical button and sees exactly what event code it generates.
//
// Parameters:
//   devices    — result of a prior scanner_scan_all() call
//   out        — filled with the identified event details on success
//   timeout_ms — how long to wait in milliseconds (0 = wait forever)
//
// Returns 0 on successful identification, -1 on timeout or error.
// ============================================================================

int scanner_identify_button(const ScanResult *devices,
                            IdentifiedEvent *out,
                            int timeout_ms) {
    // Open all gamepad devices for reading
    struct pollfd fds[MAX_SCANNED_DEVICES];
    int dev_indices[MAX_SCANNED_DEVICES];   // Maps fd slot → devices[] index
    int nfds = 0;

    for (int i = 0; i < devices->count; i++) {
        if (!devices->devices[i].is_gamepad) continue;

        int fd = open(devices->devices[i].path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        fds[nfds].fd      = fd;
        fds[nfds].events  = POLLIN;
        fds[nfds].revents = 0;
        dev_indices[nfds]  = i;
        nfds++;
    }

    if (nfds == 0) {
        fprintf(stderr, "scanner: no gamepad devices to monitor\n");
        return -1;
    }

    // Wait for the first interesting event
    int result = -1;
    int poll_timeout = (timeout_ms > 0) ? timeout_ms : -1;  // -1 = block forever
    int ret = poll(fds, nfds, poll_timeout);

    if (ret > 0) {
        // At least one fd is ready — read the first non-SYN event
        for (int i = 0; i < nfds; i++) {
            if (!(fds[i].revents & POLLIN)) continue;

            struct input_event ev;
            while (read(fds[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
                // Skip synchronization events (EV_SYN) — they don't carry
                // useful button/axis information
                if (ev.type == EV_SYN) continue;

                // For EV_KEY, only report press events (value=1), not release
                if (ev.type == EV_KEY && ev.value != 1) continue;

                // For EV_ABS, ignore small values (noise/drift near center).
                // Only report when the axis is significantly deflected.
                if (ev.type == EV_ABS) {
                    int abs_val = ev.value < 0 ? -ev.value : ev.value;
                    if (abs_val < 8000) continue;   // Below threshold, skip
                }

                // Found an interesting event — record it
                int di = dev_indices[i];
                out->ev_type  = ev.type;
                out->ev_code  = ev.code;
                out->ev_value = ev.value;
                snprintf(out->device_path, sizeof(out->device_path),
                         "%s", devices->devices[di].path);
                snprintf(out->device_name, sizeof(out->device_name),
                         "%s", devices->devices[di].name);

                result = 0;
                goto cleanup;
            }
        }
    }

cleanup:
    // Close all opened fds
    for (int i = 0; i < nfds; i++) {
        close(fds[i].fd);
    }

    return result;
}
