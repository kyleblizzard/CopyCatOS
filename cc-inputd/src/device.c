// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

//
// device.c — Physical device discovery and hotplug monitoring
//
// This file finds gamepad and power-button devices under /dev/input/ using
// libudev, opens them for reading, and watches for hotplug add/remove events
// so the daemon can handle controllers being plugged in or unplugged at
// runtime.
//
// The Lenovo Legion Go's built-in gamepad identifies as vendor 0x17ef
// (Lenovo) with product IDs 0x6182 or 0x61eb. We also track the system
// power button so the daemon can handle short/long presses.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

// Linux input subsystem headers — these define struct input_event,
// ioctl codes like EVIOCGNAME, and key/button constants.
#include <linux/input.h>

// libudev — the standard Linux library for querying and monitoring
// hardware devices. We use it to enumerate existing input devices at
// startup and to receive notifications when devices are added or removed.
#include <libudev.h>

#include "inputd.h"

// --------------------------------------------------------------------------
// Constants
// --------------------------------------------------------------------------

// Maximum number of physical input devices we'll track at once.
// 16 is generous — we typically have 1-2 gamepad nodes + 1 power button.
#define MAX_DEVICES 16

// Lenovo's USB vendor ID — the Legion Go's built-in controller uses this.
#define LENOVO_VENDOR_ID 0x17ef

// The Legion Go controller appears as one of these two product IDs
// depending on the firmware mode (xinput vs dinput).
#define LEGION_GO_PRODUCT_XINPUT  0x6182
#define LEGION_GO_PRODUCT_DINPUT  0x61eb

// --------------------------------------------------------------------------
// DeviceInfo — Metadata for a single opened input device
// --------------------------------------------------------------------------
// We keep this alongside the file descriptor so we can log useful info
// and know whether to ungrab the device on shutdown.
// --------------------------------------------------------------------------
typedef struct {
    int  fd;                           // Open file descriptor for /dev/input/eventN
    char devnode[256];                 // Path like "/dev/input/event5"
    char name[256];                    // Human-readable name from the driver
    bool is_gamepad;                   // true if this is the Legion Go controller
    bool is_power;                     // true if this is the power button
    bool grabbed;                      // true if we called EVIOCGRAB on it
} DeviceInfo;

// --------------------------------------------------------------------------
// DeviceManager — Owns all physical device state
// --------------------------------------------------------------------------
// This struct is forward-declared in inputd.h and fully defined here.
// It holds the udev context/monitor for hotplug, plus the array of
// currently-opened devices.
// --------------------------------------------------------------------------
struct DeviceManager {
    struct udev         *udev;         // libudev context — must stay alive
    struct udev_monitor *mon;          // Monitors kernel uevents for input devices
    int                  mon_fd;       // File descriptor for the monitor (for epoll)

    DeviceInfo           devices[MAX_DEVICES]; // Opened physical devices
    int                  count;        // How many entries in devices[] are valid

    bool                 grab_gamepads; // If true, grab exclusive access to gamepads
                                        // so their raw events don't leak to other apps
};

// --------------------------------------------------------------------------
// Helper: test_bit — Check whether a specific bit is set in a bitfield
// --------------------------------------------------------------------------
// The kernel returns capability bitmasks as arrays of unsigned longs.
// This helper checks if bit number `bit` is set in the array `arr`.
//
// How it works:
//   - `bit / (sizeof(long)*8)` finds which long in the array holds our bit
//   - `bit % (sizeof(long)*8)` finds which bit within that long
//   - We shift a 1 to that position and AND it with the array element
// --------------------------------------------------------------------------
static inline bool test_bit(unsigned int bit, const unsigned long *arr) {
    return (arr[bit / (sizeof(long) * 8)] >> (bit % (sizeof(long) * 8))) & 1;
}

// --------------------------------------------------------------------------
// device_open — Open a single /dev/input/eventN device and record it
// --------------------------------------------------------------------------
// Called both during the initial scan and when a new device is hotplugged.
// We read the device name and ID to determine if it's a gamepad or power
// button. If it doesn't match either, we close it and return false.
//
// Parameters:
//   mgr     — the DeviceManager that will own this device
//   devnode — path to the device node, e.g. "/dev/input/event5"
//
// Returns true if the device was successfully opened and is one we care about.
// --------------------------------------------------------------------------
bool device_open(DeviceManager *mgr, const char *devnode) {
    // Safety check: don't overflow our fixed-size array
    if (mgr->count >= MAX_DEVICES) {
        fprintf(stderr, "[cc-inputd] device array full, ignoring %s\n", devnode);
        return false;
    }

    // Check if we already have this device open (prevents duplicates
    // during hotplug events that fire multiple times)
    for (int i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->devices[i].devnode, devnode) == 0) {
            return false;  // Already tracking this device
        }
    }

    // Open the device node for reading. O_NONBLOCK ensures read() won't
    // block if there are no events waiting — important since we use epoll.
    int fd = open(devnode, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[cc-inputd] failed to open %s: %s\n",
                devnode, strerror(errno));
        return false;
    }

    // Read the human-readable device name from the driver.
    // Examples: "Lenovo Legion Controller for Windows", "Power Button"
    char name[256] = {0};
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
        snprintf(name, sizeof(name), "Unknown");
    }

    // Read the device ID struct — contains bus type, vendor, product, version.
    // We use vendor + product to identify the Legion Go controller.
    struct input_id id = {0};
    if (ioctl(fd, EVIOCGID, &id) < 0) {
        fprintf(stderr, "[cc-inputd] EVIOCGID failed for %s: %s\n",
                devnode, strerror(errno));
        close(fd);
        return false;
    }

    // --- Determine device type ---

    // Gamepad check: must be Lenovo vendor AND one of the known product IDs
    bool is_gamepad = (id.vendor == LENOVO_VENDOR_ID) &&
                      (id.product == LEGION_GO_PRODUCT_XINPUT ||
                       id.product == LEGION_GO_PRODUCT_DINPUT);

    // Power button check: look for KEY_POWER in the device's capability bits.
    // The kernel exposes which keys a device can generate via EVIOCGBIT.
    bool is_power = false;
    if (!is_gamepad) {
        // Allocate a bitmask large enough to hold all key bits.
        // KEY_CNT is the total number of key codes the kernel defines.
        unsigned long key_bits[(KEY_CNT + sizeof(long) * 8 - 1) / (sizeof(long) * 8)];
        memset(key_bits, 0, sizeof(key_bits));

        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0) {
            is_power = test_bit(KEY_POWER, key_bits);
        }

        // Also check the name as a fallback — some devices report
        // "Power Button" but might not have KEY_POWER in their bits.
        if (!is_power && strstr(name, "Power Button") != NULL) {
            is_power = true;
        }
    }

    // If this device isn't a gamepad or power button, we don't need it.
    if (!is_gamepad && !is_power) {
        close(fd);
        return false;
    }

    // If this is a gamepad and we want exclusive access, grab it.
    // EVIOCGRAB prevents the raw gamepad events from reaching other
    // programs (like X11 or SDL) — cc-inputd will be the sole reader.
    bool grabbed = false;
    if (is_gamepad && mgr->grab_gamepads) {
        if (ioctl(fd, EVIOCGRAB, 1) == 0) {
            grabbed = true;
        } else {
            fprintf(stderr, "[cc-inputd] EVIOCGRAB failed for %s: %s\n",
                    devnode, strerror(errno));
            // Not fatal — we can still read events, they'll just also
            // go to other consumers.
        }
    }

    // Store the device info in our array
    DeviceInfo *dev = &mgr->devices[mgr->count];
    dev->fd = fd;
    snprintf(dev->devnode, sizeof(dev->devnode), "%s", devnode);
    snprintf(dev->name, sizeof(dev->name), "%s", name);
    dev->is_gamepad = is_gamepad;
    dev->is_power   = is_power;
    dev->grabbed    = grabbed;
    mgr->count++;

    fprintf(stderr, "[cc-inputd] opened %s: \"%s\" [%04x:%04x] %s%s%s\n",
            devnode, name, id.vendor, id.product,
            is_gamepad ? "gamepad" : "",
            is_power   ? "power"   : "",
            grabbed    ? " (grabbed)" : "");

    return true;
}

// --------------------------------------------------------------------------
// device_init — Set up udev, scan for existing devices, start monitoring
// --------------------------------------------------------------------------
// This is called once at daemon startup. It creates the udev context and
// monitor, scans /dev/input/ for devices that match our criteria, and
// prepares the monitor fd for use with epoll.
//
// Parameters:
//   mgr — a freshly allocated DeviceManager to initialize
//
// Returns true on success, false if udev setup fails.
// --------------------------------------------------------------------------
bool device_init(DeviceManager *mgr) {
    // Zero out the struct so all pointers start as NULL and count is 0
    memset(mgr, 0, sizeof(*mgr));

    // Default to grabbing gamepads — this gives us exclusive access so
    // the raw controller events don't interfere with X11 / Wayland.
    mgr->grab_gamepads = true;

    // --- Create the udev context ---
    // udev_new() is the entry point to libudev. We need this context
    // for all subsequent udev operations.
    mgr->udev = udev_new();
    if (!mgr->udev) {
        fprintf(stderr, "[cc-inputd] udev_new() failed\n");
        return false;
    }

    // --- Create the udev monitor ---
    // This watches for kernel uevents (hardware being added/removed).
    // "udev" means we get events after udev rules have been applied,
    // so device nodes are ready to open when we receive the event.
    mgr->mon = udev_monitor_new_from_netlink(mgr->udev, "udev");
    if (!mgr->mon) {
        fprintf(stderr, "[cc-inputd] udev_monitor_new_from_netlink() failed\n");
        udev_unref(mgr->udev);
        mgr->udev = NULL;
        return false;
    }

    // Only listen for events from the "input" subsystem — we don't care
    // about USB hubs, network interfaces, or other device types.
    udev_monitor_filter_add_match_subsystem_devtype(mgr->mon, "input", NULL);

    // Start receiving events. Must be called before we can read from
    // the monitor fd.
    udev_monitor_enable_receiving(mgr->mon);

    // Get the file descriptor for the monitor — we'll add this to epoll
    // in the main loop so we wake up when devices are hotplugged.
    mgr->mon_fd = udev_monitor_get_fd(mgr->mon);

    // --- Scan existing devices ---
    // At startup, devices may already be connected. We enumerate all
    // current /dev/input/ nodes and try to open matching ones.
    struct udev_enumerate *enumerate = udev_enumerate_new(mgr->udev);
    if (!enumerate) {
        fprintf(stderr, "[cc-inputd] udev_enumerate_new() failed\n");
        // Not fatal — we can still get hotplug events
        return true;
    }

    // Filter the enumeration to only list "input" subsystem devices
    udev_enumerate_add_match_subsystem(enumerate, "input");

    // Perform the scan — this populates an internal list
    udev_enumerate_scan_devices(enumerate);

    // Walk through every matching device
    struct udev_list_entry *entry;
    struct udev_list_entry *list = udev_enumerate_get_list_entry(enumerate);
    udev_list_entry_foreach(entry, list) {
        // Get the sysfs path for this device (e.g. /sys/devices/.../event5)
        const char *syspath = udev_list_entry_get_name(entry);

        // Create a udev_device object from the sysfs path so we can
        // read its properties and find its /dev/input/eventN node.
        struct udev_device *dev = udev_device_new_from_syspath(mgr->udev, syspath);
        if (!dev) continue;

        // Get the device node path — this is what we actually open().
        // Some entries in the enumeration don't have device nodes
        // (e.g. the parent "input5" device vs the "event5" child).
        const char *devnode = udev_device_get_devnode(dev);
        if (!devnode) {
            udev_device_unref(dev);
            continue;
        }

        // Quick pre-filter using udev properties before we open() the node.
        // ID_INPUT_JOYSTICK is set by udev rules for gamepad-type devices.
        const char *is_joystick = udev_device_get_property_value(dev, "ID_INPUT_JOYSTICK");
        const char *vendor_str  = udev_device_get_property_value(dev, "ID_VENDOR_ID");

        bool dominated_gamepad = false;
        if (is_joystick && strcmp(is_joystick, "1") == 0 && vendor_str) {
            // Parse the hex vendor ID string (e.g. "17ef")
            unsigned int vendor_id = 0;
            sscanf(vendor_str, "%x", &vendor_id);
            if (vendor_id == LENOVO_VENDOR_ID) {
                dominated_gamepad = true;
            }
        }

        // We also want to detect power buttons, which won't have the
        // joystick property. For those, we open optimistically and let
        // device_open() check the KEY_POWER capability bit.
        //
        // To avoid opening every single input device (keyboards, mice,
        // touchscreens), we check the device name from udev first.
        const char *sysname = udev_device_get_sysname(dev);
        bool might_be_power = false;

        // Power buttons usually have sysfs names starting with "event"
        // (they're event devices). We can't easily tell from sysfs alone,
        // so we just try to open anything that isn't obviously unrelated.
        // device_open() will close it quickly if it doesn't match.
        if (sysname && strncmp(sysname, "event", 5) == 0) {
            might_be_power = true;
        }

        // Try to open if it looks like a gamepad or might be a power button
        if (dominated_gamepad || might_be_power) {
            device_open(mgr, devnode);
        }

        udev_device_unref(dev);
    }

    // Free the enumeration — we're done scanning
    udev_enumerate_unref(enumerate);

    fprintf(stderr, "[cc-inputd] device scan complete: %d device(s) opened\n",
            mgr->count);

    return true;
}

// --------------------------------------------------------------------------
// device_handle_hotplug — Process a udev add/remove event
// --------------------------------------------------------------------------
// Called by the main loop when the udev monitor fd becomes readable.
// This handles controllers being plugged in or unplugged at runtime.
//
// Parameters:
//   mgr — the DeviceManager to update
// --------------------------------------------------------------------------
void device_handle_hotplug(DeviceManager *mgr) {
    // Read the pending udev event. This gives us a device object with
    // information about what just happened.
    struct udev_device *dev = udev_monitor_receive_device(mgr->mon);
    if (!dev) return;

    // Every udev event has an "action" string: "add", "remove", "change", etc.
    const char *action  = udev_device_get_action(dev);
    const char *devnode = udev_device_get_devnode(dev);

    // We need both an action and a device node to do anything useful
    if (!action || !devnode) {
        udev_device_unref(dev);
        return;
    }

    if (strcmp(action, "add") == 0) {
        // --- Device added ---
        // A new input device appeared. Try to open it — device_open()
        // will check if it's a gamepad or power button and ignore it
        // if it's not one we care about.
        fprintf(stderr, "[cc-inputd] hotplug add: %s\n", devnode);
        device_open(mgr, devnode);

    } else if (strcmp(action, "remove") == 0) {
        // --- Device removed ---
        // Find the device in our array by matching the devnode path,
        // then close it and remove it from the array.
        fprintf(stderr, "[cc-inputd] hotplug remove: %s\n", devnode);

        for (int i = 0; i < mgr->count; i++) {
            if (strcmp(mgr->devices[i].devnode, devnode) == 0) {
                // Ungrab before closing if we had exclusive access
                if (mgr->devices[i].grabbed) {
                    ioctl(mgr->devices[i].fd, EVIOCGRAB, 0);
                }
                close(mgr->devices[i].fd);

                fprintf(stderr, "[cc-inputd] closed %s: \"%s\"\n",
                        mgr->devices[i].devnode, mgr->devices[i].name);

                // Shift the remaining devices down to fill the gap.
                // This keeps the array compact with no holes.
                // Example: if we remove index 1 from [A, B, C, D],
                // we memmove C and D left to get [A, C, D].
                int remaining = mgr->count - i - 1;
                if (remaining > 0) {
                    memmove(&mgr->devices[i], &mgr->devices[i + 1],
                            remaining * sizeof(DeviceInfo));
                }
                mgr->count--;
                break;
            }
        }
    }

    // Always free the udev device when done — libudev allocates
    // internally and we must release it.
    udev_device_unref(dev);
}

// --------------------------------------------------------------------------
// device_shutdown — Clean up all devices and udev resources
// --------------------------------------------------------------------------
// Called during daemon shutdown. Ungrabs and closes every open device,
// then destroys the udev monitor and context.
//
// Parameters:
//   mgr — the DeviceManager to tear down
// --------------------------------------------------------------------------
void device_shutdown(DeviceManager *mgr) {
    if (!mgr) return;

    // Close every open device
    for (int i = 0; i < mgr->count; i++) {
        // Release the exclusive grab before closing, if we had one.
        // This is polite — the kernel would do it automatically when
        // the fd is closed, but being explicit is clearer.
        if (mgr->devices[i].grabbed) {
            ioctl(mgr->devices[i].fd, EVIOCGRAB, 0);
        }
        close(mgr->devices[i].fd);

        fprintf(stderr, "[cc-inputd] closed %s: \"%s\"\n",
                mgr->devices[i].devnode, mgr->devices[i].name);
    }
    mgr->count = 0;

    // Destroy the udev monitor — stops watching for hotplug events
    if (mgr->mon) {
        udev_monitor_unref(mgr->mon);
        mgr->mon = NULL;
        mgr->mon_fd = -1;
    }

    // Destroy the udev context — frees all libudev internal state
    if (mgr->udev) {
        udev_unref(mgr->udev);
        mgr->udev = NULL;
    }

    fprintf(stderr, "[cc-inputd] device manager shut down\n");
}
