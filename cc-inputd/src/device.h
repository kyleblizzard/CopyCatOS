// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

//
// device.h — Physical input device discovery and management
//
// Uses libudev to enumerate /dev/input/event* nodes at startup and to
// monitor for hotplug events (controllers plugged in / unplugged while
// the daemon is running). Each discovered device is opened via evdev
// and optionally "grabbed" (EVIOCGRAB) so that its raw events are
// consumed exclusively by cc-inputd instead of being seen by X11.
//

#ifndef DEVICE_H
#define DEVICE_H

#include <stdbool.h>

// Forward declaration so we don't need to include <libudev.h> here.
// The .c file includes the real header.
struct udev;
struct udev_monitor;

// --------------------------------------------------------------------------
// Legion Go USB vendor/product IDs
// --------------------------------------------------------------------------
// The Lenovo Legion Go exposes its gamepad controls through two USB HID
// devices. We match on these IDs to distinguish the Legion Go controllers
// from any other input devices connected to the system (keyboards, mice,
// external gamepads, touchscreens, etc.).
// --------------------------------------------------------------------------
#define LEGION_GO_VID   0x17ef   // Lenovo USB vendor ID
#define LEGION_GO_PID1  0x6182   // Legion Go controller — primary
#define LEGION_GO_PID2  0x61eb   // Legion Go controller — secondary

// --------------------------------------------------------------------------
// InputDevice — Represents one opened /dev/input/event* node
// --------------------------------------------------------------------------
typedef struct InputDevice {
    int  fd;                      // File descriptor from open(); -1 if unused
    char path[256];               // e.g. "/dev/input/event5"
    char name[128];               // Human-readable name from EVIOCGNAME ioctl

    unsigned short vendor;        // USB vendor ID (from EVIOCGID)
    unsigned short product;       // USB product ID (from EVIOCGID)

    bool is_gamepad;              // True if this is one of the Legion Go pads
    bool is_power_button;         // True if this is the system power key device

    bool grabbed;                 // True if we successfully called EVIOCGRAB
                                  // (exclusive access — X11 won't see events)
} InputDevice;

// Maximum number of physical devices we track at once.
// 8 is plenty: two Legion Go controllers + power button + a few extras.
#define MAX_DEVICES 8

// --------------------------------------------------------------------------
// DeviceManager — Owns udev context, monitor, and the device list
// --------------------------------------------------------------------------
typedef struct DeviceManager {
    struct udev         *udev;       // libudev context handle
    struct udev_monitor *monitor;    // Watches for add/remove events
    int                  mon_fd;     // Monitor's file descriptor (for epoll)

    InputDevice devices[MAX_DEVICES]; // Fixed-size array of opened devices
    int         device_count;         // How many slots are currently in use
} DeviceManager;

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------

// device_init — Create udev context, start monitoring, enumerate existing
// devices and open any that match our criteria (Legion Go controllers,
// power button). Returns 0 on success, -1 on failure.
int  device_init(DeviceManager *dm);

// device_open — Open a single /dev/input/event* node by path, identify it,
// and add it to the devices array if it's interesting (gamepad or power).
// Returns the slot index on success, -1 if not interesting or on error.
int  device_open(DeviceManager *dm, const char *path);

// device_handle_hotplug — Called when the udev monitor fd is readable.
// Reads the udev event and either opens a new device or closes a removed one.
void device_handle_hotplug(DeviceManager *dm);

// device_shutdown — Close all device fds, ungrab grabbed devices, destroy
// the udev monitor and context. Safe to call with partially initialized state.
void device_shutdown(DeviceManager *dm);

#endif // DEVICE_H
