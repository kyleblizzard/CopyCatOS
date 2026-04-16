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

#include "device.h"

// --------------------------------------------------------------------------
// Helper: test_bit — Check whether a specific bit is set in a bitfield
// --------------------------------------------------------------------------
// The kernel returns capability bitmasks as arrays of unsigned longs.
// This helper checks if bit number `bit` is set in the array `arr`.
// --------------------------------------------------------------------------
static inline bool test_bit(unsigned int bit, const unsigned long *arr) {
    return (arr[bit / (sizeof(long) * 8)] >> (bit % (sizeof(long) * 8))) & 1;
}

// --------------------------------------------------------------------------
// device_open — Open a single /dev/input/eventN device and record it
// --------------------------------------------------------------------------
// Called both during the initial scan and when a new device is hotplugged.
// We read the device name and ID to determine if it's a gamepad or power
// button. If it doesn't match either, we close it and return -1.
//
// Parameters:
//   dm   — the DeviceManager that will own this device
//   path — path to the device node, e.g. "/dev/input/event5"
//
// Returns the slot index on success, -1 if not interesting or on error.
// --------------------------------------------------------------------------
int device_open(DeviceManager *dm, const char *path) {
    // Safety check: don't overflow our fixed-size array
    if (dm->device_count >= MAX_DEVICES) {
        fprintf(stderr, "[cc-inputd] device array full, ignoring %s\n", path);
        return -1;
    }

    // Check if we already have this device open (prevents duplicates
    // during hotplug events that fire multiple times)
    for (int i = 0; i < dm->device_count; i++) {
        if (strcmp(dm->devices[i].path, path) == 0) {
            return -1;  // Already tracking this device
        }
    }

    // Open the device node for reading. O_NONBLOCK ensures read() won't
    // block if there are no events waiting — important since we use epoll.
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[cc-inputd] failed to open %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    // Read the human-readable device name from the driver.
    // Examples: "Lenovo Legion Controller for Windows", "Power Button"
    char name[128] = {0};
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
        snprintf(name, sizeof(name), "Unknown");
    }

    // Read the device ID struct — contains bus type, vendor, product, version.
    // We use vendor + product to identify the Legion Go controller.
    struct input_id id = {0};
    if (ioctl(fd, EVIOCGID, &id) < 0) {
        fprintf(stderr, "[cc-inputd] EVIOCGID failed for %s: %s\n",
                path, strerror(errno));
        close(fd);
        return -1;
    }

    // --- Determine device type ---

    // Gamepad check: must be Lenovo vendor AND one of the known product IDs
    bool is_gamepad = (id.vendor == LEGION_GO_VID) &&
                      (id.product == LEGION_GO_PID1 ||
                       id.product == LEGION_GO_PID2);

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
        return -1;
    }

    // If this is a gamepad, grab exclusive access so raw events don't
    // leak to X11/SDL — cc-inputd will be the sole reader.
    bool grabbed = false;
    if (is_gamepad) {
        if (ioctl(fd, EVIOCGRAB, 1) == 0) {
            grabbed = true;
        } else {
            fprintf(stderr, "[cc-inputd] EVIOCGRAB failed for %s: %s\n",
                    path, strerror(errno));
            // Not fatal — we can still read events, they'll just also
            // go to other consumers.
        }
    }

    // Store the device info in our array
    int slot = dm->device_count;
    InputDevice *dev = &dm->devices[slot];
    dev->fd = fd;
    snprintf(dev->path, sizeof(dev->path), "%s", path);
    snprintf(dev->name, sizeof(dev->name), "%s", name);
    dev->vendor         = id.vendor;
    dev->product        = id.product;
    dev->is_gamepad     = is_gamepad;
    dev->is_power_button = is_power;
    dev->grabbed        = grabbed;
    dm->device_count++;

    fprintf(stderr, "[cc-inputd] opened %s: \"%s\" [%04x:%04x] %s%s%s\n",
            path, name, id.vendor, id.product,
            is_gamepad ? "gamepad" : "",
            is_power   ? "power"   : "",
            grabbed    ? " (grabbed)" : "");

    return slot;
}

// --------------------------------------------------------------------------
// device_init — Set up udev, scan for existing devices, start monitoring
// --------------------------------------------------------------------------
// This is called once at daemon startup. It creates the udev context and
// monitor, scans /dev/input/ for devices that match our criteria, and
// prepares the monitor fd for use with epoll.
//
// Parameters:
//   dm — a freshly allocated DeviceManager to initialize
//
// Returns 0 on success, -1 if udev setup fails.
// --------------------------------------------------------------------------
int device_init(DeviceManager *dm) {
    // Zero out the struct so all pointers start as NULL and count is 0
    memset(dm, 0, sizeof(*dm));
    dm->mon_fd = -1;

    // --- Create the udev context ---
    // udev_new() is the entry point to libudev. We need this context
    // for all subsequent udev operations.
    dm->udev = udev_new();
    if (!dm->udev) {
        fprintf(stderr, "[cc-inputd] udev_new() failed\n");
        return -1;
    }

    // --- Create the udev monitor ---
    // This watches for kernel uevents (hardware being added/removed).
    // "udev" means we get events after udev rules have been applied,
    // so device nodes are ready to open when we receive the event.
    dm->monitor = udev_monitor_new_from_netlink(dm->udev, "udev");
    if (!dm->monitor) {
        fprintf(stderr, "[cc-inputd] udev_monitor_new_from_netlink() failed\n");
        udev_unref(dm->udev);
        dm->udev = NULL;
        return -1;
    }

    // Only listen for events from the "input" subsystem — we don't care
    // about USB hubs, network interfaces, or other device types.
    udev_monitor_filter_add_match_subsystem_devtype(dm->monitor, "input", NULL);

    // Start receiving events. Must be called before we can read from
    // the monitor fd.
    udev_monitor_enable_receiving(dm->monitor);

    // Get the file descriptor for the monitor — we'll add this to epoll
    // in the main loop so we wake up when devices are hotplugged.
    dm->mon_fd = udev_monitor_get_fd(dm->monitor);

    // --- Scan existing devices ---
    // At startup, devices may already be connected. We enumerate all
    // current /dev/input/ nodes and try to open matching ones.
    struct udev_enumerate *enumerate = udev_enumerate_new(dm->udev);
    if (!enumerate) {
        fprintf(stderr, "[cc-inputd] udev_enumerate_new() failed\n");
        // Not fatal — we can still get hotplug events
        return 0;
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
        struct udev_device *dev = udev_device_new_from_syspath(dm->udev, syspath);
        if (!dev) continue;

        // Get the device node path — this is what we actually open().
        // Some entries in the enumeration don't have device nodes
        // (e.g. the parent "input5" device vs the "event5" child).
        const char *devnode = udev_device_get_devnode(dev);
        if (!devnode) {
            udev_device_unref(dev);
            continue;
        }

        // Quick pre-filter: only open event nodes (skip input* parents)
        const char *sysname = udev_device_get_sysname(dev);
        if (sysname && strncmp(sysname, "event", 5) == 0) {
            device_open(dm, devnode);
        }

        udev_device_unref(dev);
    }

    // Free the enumeration — we're done scanning
    udev_enumerate_unref(enumerate);

    fprintf(stderr, "[cc-inputd] device scan complete: %d device(s) opened\n",
            dm->device_count);

    return 0;
}

// --------------------------------------------------------------------------
// device_handle_hotplug — Process a udev add/remove event
// --------------------------------------------------------------------------
// Called by the main loop when the udev monitor fd becomes readable.
// This handles controllers being plugged in or unplugged at runtime.
// --------------------------------------------------------------------------
void device_handle_hotplug(DeviceManager *dm) {
    // Read the pending udev event. This gives us a device object with
    // information about what just happened.
    struct udev_device *dev = udev_monitor_receive_device(dm->monitor);
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
        fprintf(stderr, "[cc-inputd] hotplug add: %s\n", devnode);
        device_open(dm, devnode);

    } else if (strcmp(action, "remove") == 0) {
        // --- Device removed ---
        // Find the device in our array by matching the path,
        // then close it and remove it from the array.
        fprintf(stderr, "[cc-inputd] hotplug remove: %s\n", devnode);

        for (int i = 0; i < dm->device_count; i++) {
            if (strcmp(dm->devices[i].path, devnode) == 0) {
                // Ungrab before closing if we had exclusive access
                if (dm->devices[i].grabbed) {
                    ioctl(dm->devices[i].fd, EVIOCGRAB, 0);
                }
                close(dm->devices[i].fd);

                fprintf(stderr, "[cc-inputd] closed %s: \"%s\"\n",
                        dm->devices[i].path, dm->devices[i].name);

                // Shift remaining devices down to fill the gap
                int remaining = dm->device_count - i - 1;
                if (remaining > 0) {
                    memmove(&dm->devices[i], &dm->devices[i + 1],
                            remaining * sizeof(InputDevice));
                }
                dm->device_count--;
                break;
            }
        }
    }

    udev_device_unref(dev);
}

// --------------------------------------------------------------------------
// device_shutdown — Clean up all devices and udev resources
// --------------------------------------------------------------------------
void device_shutdown(DeviceManager *dm) {
    if (!dm) return;

    // Close every open device
    for (int i = 0; i < dm->device_count; i++) {
        if (dm->devices[i].grabbed) {
            ioctl(dm->devices[i].fd, EVIOCGRAB, 0);
        }
        close(dm->devices[i].fd);

        fprintf(stderr, "[cc-inputd] closed %s: \"%s\"\n",
                dm->devices[i].path, dm->devices[i].name);
    }
    dm->device_count = 0;

    // Destroy the udev monitor
    if (dm->monitor) {
        udev_monitor_unref(dm->monitor);
        dm->monitor = NULL;
        dm->mon_fd = -1;
    }

    // Destroy the udev context
    if (dm->udev) {
        udev_unref(dm->udev);
        dm->udev = NULL;
    }

    fprintf(stderr, "[cc-inputd] device manager shut down\n");
}
