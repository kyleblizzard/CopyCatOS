// CopyCatOS — by Kyle Blizzard at Blizzard.show

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
        fprintf(stderr, "[inputd] device array full, ignoring %s\n", path);
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
        fprintf(stderr, "[inputd] failed to open %s: %s\n",
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
        fprintf(stderr, "[inputd] EVIOCGID failed for %s: %s\n",
                path, strerror(errno));
        close(fd);
        return -1;
    }

    // --- Skip our own virtual devices ---
    // When inputd creates uinput devices (virtual mouse, keyboard, gamepad),
    // udev fires hotplug events that would cause us to re-open them. If we
    // grabbed our own virtual gamepad, we'd create an infinite event loop
    // (events forwarded to uinput → read back → forwarded again).
    //
    // Virtual devices created by uinput have no physical location string (PHYS).
    // Real USB devices always have one (e.g. "usb-0000:c3:00.4-1/input0").
    // We skip any device whose name starts with "CopyCatOS Virtual", and any
    // device with an empty PHYS string that claims to be a gamepad-like device.
    if (strncmp(name, "CopyCatOS Virtual", 17) == 0) {
        close(fd);
        return -1;
    }
    {
        char phys[256] = {0};
        if (ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys) < 0 || phys[0] == '\0') {
            // No PHYS string — likely a uinput virtual device.
            // Only skip if it looks like a gamepad (has our Xbox VID/PID).
            // Real power buttons and keyboards with empty PHYS are fine.
            if (id.vendor == 0x045e && id.product == 0x028e) {
                close(fd);
                return -1;
            }
        }
    }

    // --- Determine device type ---

    // Gamepad check: requires BOTH vendor/product match AND gamepad capabilities.
    //
    // The Legion Go S WCH.cn chip (0x1a86:0xe310) exposes MANY interfaces:
    //   - event11 "Legion Go S"           → real gamepad (sticks + buttons)
    //   - event2  "wch.cn Legion Go S"    → system keys (volume)
    //   - event3  "wch.cn Legion Go S Mouse"     → FPS mode mouse
    //   - event4  "wch.cn Legion Go S Keyboard"  → FPS mode keyboard
    //   - event7  "wch.cn Legion Go S Touchpad"  → touchscreen input
    //   - event6  "wch.cn Legion Go S UNKNOWN"   → unknown
    //
    // In HID takeover mode (FPS mode), the Mouse/Keyboard/Touchpad interfaces
    // are actively used by the firmware. GRABBING them kills touch input and
    // mouse functionality. We must only grab the actual gamepad (event11)
    // which has analog sticks (ABS_X/Y) AND face buttons (BTN_SOUTH).
    //
    // The capability check is REQUIRED even for VID/PID matches.
    bool is_gamepad = false;
    bool vid_match = false;

    if (id.vendor == LEGION_GO_VID &&
        (id.product == LEGION_GO_PID1 || id.product == LEGION_GO_PID2)) {
        vid_match = true;
    } else if (id.vendor == LEGION_GO_S_VID &&
               id.product == LEGION_GO_S_PID) {
        vid_match = true;
    }

    // Capability-based gamepad detection: requires analog sticks + face button.
    // This correctly identifies the real gamepad node while filtering out
    // Mouse/Keyboard/Touchpad interfaces that share the same VID/PID.
    {
        unsigned long abs_bits[(ABS_CNT + sizeof(long) * 8 - 1) / (sizeof(long) * 8)];
        unsigned long key_bits[(KEY_CNT + sizeof(long) * 8 - 1) / (sizeof(long) * 8)];
        memset(abs_bits, 0, sizeof(abs_bits));
        memset(key_bits, 0, sizeof(key_bits));

        bool has_sticks = false;
        bool has_face_button = false;

        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) >= 0) {
            has_sticks = test_bit(ABS_X, abs_bits) && test_bit(ABS_Y, abs_bits);
        }
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0) {
            has_face_button = test_bit(BTN_SOUTH, key_bits);
        }

        if (has_sticks && has_face_button) {
            is_gamepad = true;
            if (!vid_match) {
                fprintf(stderr, "[inputd] detected gamepad by capabilities: %s\n", name);
            }
        }
    }

    // Power button and system keys check.
    // We look for KEY_POWER (power button) and KEY_VOLUMEUP (volume/media
    // keys device) in the device's capability bits via EVIOCGBIT.
    bool is_power = false;
    bool is_sys_keys = false;
    if (!is_gamepad) {
        unsigned long key_bits[(KEY_CNT + sizeof(long) * 8 - 1) / (sizeof(long) * 8)];
        memset(key_bits, 0, sizeof(key_bits));

        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0) {
            is_power = test_bit(KEY_POWER, key_bits);

            // System keys device: has volume keys but is NOT a gamepad.
            // On the Legion Go S, event2 ("wch.cn Legion Go S") has
            // KEY_VOLUMEUP, KEY_VOLUMEDOWN, and KEY_MUTE.
            if (test_bit(KEY_VOLUMEUP, key_bits) &&
                test_bit(KEY_VOLUMEDOWN, key_bits)) {
                is_sys_keys = true;
            }
        }

        // Also check the name as a fallback — some devices report
        // "Power Button" but might not have KEY_POWER in their bits.
        if (!is_power && strstr(name, "Power Button") != NULL) {
            is_power = true;
        }
    }

    // If this device isn't a gamepad, power button, or system keys, skip it.
    if (!is_gamepad && !is_power && !is_sys_keys) {
        close(fd);
        return -1;
    }

    // If this is a gamepad, grab exclusive access so raw events don't
    // leak to X11/SDL — inputd will be the sole reader.
    bool grabbed = false;
    if (is_gamepad) {
        if (ioctl(fd, EVIOCGRAB, 1) == 0) {
            grabbed = true;
        } else {
            fprintf(stderr, "[inputd] EVIOCGRAB failed for %s: %s\n",
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
    dev->is_gamepad      = is_gamepad;
    dev->is_power_button = is_power;
    dev->is_system_keys  = is_sys_keys;
    dev->grabbed         = grabbed;
    dm->device_count++;

    fprintf(stderr, "[inputd] opened %s: \"%s\" [%04x:%04x] %s%s%s%s\n",
            path, name, id.vendor, id.product,
            is_gamepad  ? "gamepad " : "",
            is_power    ? "power "   : "",
            is_sys_keys ? "syskeys " : "",
            grabbed     ? "(grabbed)" : "");

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
    dm->hidraw_config_fd = -1;
    dm->hidraw_button_fd = -1;
    dm->gamepad_mode_active = false;
    dm->hid_takeover_active = false;

    // --- Create the udev context ---
    // udev_new() is the entry point to libudev. We need this context
    // for all subsequent udev operations.
    dm->udev = udev_new();
    if (!dm->udev) {
        fprintf(stderr, "[inputd] udev_new() failed\n");
        return -1;
    }

    // --- Create the udev monitor ---
    // This watches for kernel uevents (hardware being added/removed).
    // "udev" means we get events after udev rules have been applied,
    // so device nodes are ready to open when we receive the event.
    dm->monitor = udev_monitor_new_from_netlink(dm->udev, "udev");
    if (!dm->monitor) {
        fprintf(stderr, "[inputd] udev_monitor_new_from_netlink() failed\n");
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
        fprintf(stderr, "[inputd] udev_enumerate_new() failed\n");
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

    fprintf(stderr, "[inputd] device scan complete: %d device(s) opened\n",
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
        fprintf(stderr, "[inputd] hotplug add: %s\n", devnode);
        device_open(dm, devnode);

    } else if (strcmp(action, "remove") == 0) {
        // --- Device removed ---
        // Find the device in our array by matching the path,
        // then close it and remove it from the array.
        fprintf(stderr, "[inputd] hotplug remove: %s\n", devnode);

        for (int i = 0; i < dm->device_count; i++) {
            if (strcmp(dm->devices[i].path, devnode) == 0) {
                // Ungrab before closing if we had exclusive access
                if (dm->devices[i].grabbed) {
                    ioctl(dm->devices[i].fd, EVIOCGRAB, 0);
                }
                close(dm->devices[i].fd);

                fprintf(stderr, "[inputd] closed %s: \"%s\"\n",
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

    // Restore FPS/Windows mode before closing devices.
    // This ensures the controller is usable without inputd.
    device_restore_fps_mode(dm);

    // Close the hidraw button fd if open (HID takeover mode)
    if (dm->hidraw_button_fd >= 0) {
        close(dm->hidraw_button_fd);
        dm->hidraw_button_fd = -1;
        dm->hid_takeover_active = false;
        fprintf(stderr, "[inputd] closed hidraw buttons: %s\n",
                dm->hidraw_button_path);
    }

    // Close the hidraw config fd if open
    if (dm->hidraw_config_fd >= 0) {
        close(dm->hidraw_config_fd);
        dm->hidraw_config_fd = -1;
        fprintf(stderr, "[inputd] closed hidraw config: %s\n",
                dm->hidraw_config_path);
    }

    // Close every open device
    for (int i = 0; i < dm->device_count; i++) {
        if (dm->devices[i].grabbed) {
            ioctl(dm->devices[i].fd, EVIOCGRAB, 0);
        }
        close(dm->devices[i].fd);

        fprintf(stderr, "[inputd] closed %s: \"%s\"\n",
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

    fprintf(stderr, "[inputd] device manager shut down\n");
}

// --------------------------------------------------------------------------
// find_hidraw_config — Find the WCH.cn vendor-specific hidraw device
// --------------------------------------------------------------------------
// The Legion Go S has a configuration HID interface with usage page 0xFFA0.
// We identify it by scanning /sys/class/hidraw/ for devices with the right
// vendor ID and a report descriptor that starts with the vendor-specific
// usage page bytes (06 A0 FF).
//
// Returns the fd on success, -1 on failure.
// --------------------------------------------------------------------------
static int find_hidraw_config(DeviceManager *dm) {
    // Iterate through hidraw devices looking for our config interface
    for (int i = 0; i < 16; i++) {
        char sysfs_path[256];
        char devnode[64];
        snprintf(devnode, sizeof(devnode), "/dev/hidraw%d", i);

        // Check if this hidraw belongs to our WCH.cn device by reading
        // the HID uevent for the vendor/product ID
        snprintf(sysfs_path, sizeof(sysfs_path),
                 "/sys/class/hidraw/hidraw%d/device/uevent", i);
        FILE *fp = fopen(sysfs_path, "r");
        if (!fp) continue;

        bool is_legion = false;
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            // Look for HID_ID line containing our vendor:product
            if (strstr(line, "00001A86") && strstr(line, "0000E310")) {
                is_legion = true;
                break;
            }
        }
        fclose(fp);
        if (!is_legion) continue;

        // Check if the report descriptor starts with vendor-specific usage page
        // 06 A0 FF = Usage Page (Vendor Defined 0xFFA0)
        snprintf(sysfs_path, sizeof(sysfs_path),
                 "/sys/class/hidraw/hidraw%d/device/report_descriptor", i);
        int desc_fd = open(sysfs_path, O_RDONLY);
        if (desc_fd < 0) continue;

        unsigned char desc[8];
        ssize_t n = read(desc_fd, desc, sizeof(desc));
        close(desc_fd);

        if (n < 3 || desc[0] != 0x06 || desc[1] != 0xA0 || desc[2] != 0xFF) {
            continue;   // Not the vendor-specific interface
        }

        // This is a vendor-specific Legion Go S hidraw device.
        // Open it for read+write (we need to send commands).
        int fd = open(devnode, O_RDWR);
        if (fd < 0) {
            fprintf(stderr, "[inputd] failed to open %s for config: %s\n",
                    devnode, strerror(errno));
            continue;
        }

        snprintf(dm->hidraw_config_path, sizeof(dm->hidraw_config_path),
                 "%s", devnode);
        fprintf(stderr, "[inputd] found hidraw config interface: %s\n", devnode);
        return fd;
    }

    return -1;
}

// --------------------------------------------------------------------------
// find_hidraw_buttons — Find the button-reading vendor hidraw device
// --------------------------------------------------------------------------
// The Legion Go S has TWO vendor-specific hidraw devices with 21-byte
// descriptors (both start with 06 A0 FF). They differ by HID interface:
//   - Interface 0004 (hidraw3): silent — produces no reports
//   - Interface 0006 (hidraw5): streams 32-byte reports with buttons + IMU
//
// We need the LAST (highest-numbered) 21-byte vendor hidraw, which is
// consistently the button-streaming interface 0006.
//
// Returns the fd on success, -1 on failure.
// --------------------------------------------------------------------------
static int find_hidraw_buttons(DeviceManager *dm)
{
    // Track the best candidate — we want the highest-numbered match
    int best_index = -1;

    for (int i = 0; i < 16; i++) {
        char sysfs_path[256];
        char devnode[64];
        snprintf(devnode, sizeof(devnode), "/dev/hidraw%d", i);

        // Skip the config interface — we already have that one
        if (dm->hidraw_config_fd >= 0 &&
            strcmp(devnode, dm->hidraw_config_path) == 0) {
            continue;
        }

        // Check if this hidraw belongs to our WCH.cn device
        snprintf(sysfs_path, sizeof(sysfs_path),
                 "/sys/class/hidraw/hidraw%d/device/uevent", i);
        FILE *fp = fopen(sysfs_path, "r");
        if (!fp) continue;

        bool is_legion = false;
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "00001A86") && strstr(line, "0000E310")) {
                is_legion = true;
                break;
            }
        }
        fclose(fp);
        if (!is_legion) continue;

        // Check for vendor-specific descriptor prefix (06 A0 FF)
        snprintf(sysfs_path, sizeof(sysfs_path),
                 "/sys/class/hidraw/hidraw%d/device/report_descriptor", i);
        int desc_fd = open(sysfs_path, O_RDONLY);
        if (desc_fd < 0) continue;

        unsigned char desc[64];
        ssize_t desc_len = read(desc_fd, desc, sizeof(desc));
        close(desc_fd);

        if (desc_len < 3 || desc[0] != 0x06 || desc[1] != 0xA0 || desc[2] != 0xFF) {
            continue;
        }

        // Must be the 21-byte descriptor (not the 29-byte config)
        if (desc_len != 21) {
            continue;
        }

        // Don't open yet — just record as the best candidate.
        // The highest-numbered 21-byte match is the button interface.
        best_index = i;
        fprintf(stderr, "[inputd] hidraw candidate for buttons: "
                "/dev/hidraw%d (desc=%zd bytes)\n", i, desc_len);
    }

    if (best_index < 0) {
        return -1;
    }

    // Open the winning candidate read-only with non-blocking for epoll
    char devnode[64];
    snprintf(devnode, sizeof(devnode), "/dev/hidraw%d", best_index);

    int fd = open(devnode, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[inputd] failed to open %s for buttons: %s\n",
                devnode, strerror(errno));
        return -1;
    }

    snprintf(dm->hidraw_button_path, sizeof(dm->hidraw_button_path),
             "%s", devnode);
    fprintf(stderr, "[inputd] found hidraw button interface: %s\n", devnode);
    return fd;
}

// --------------------------------------------------------------------------
// device_init_hid_takeover — Set up HID takeover for desktop mode
// --------------------------------------------------------------------------
// Opens the button-reading hidraw interface. Does NOT switch the firmware
// to SteamOS mode — we stay in FPS mode so the vendor hidraw streams data.
// The XInput evdev interface (event11) provides sticks and analog triggers.
//
// Returns 0 on success, -1 if the button hidraw wasn't found.
// --------------------------------------------------------------------------
int device_init_hid_takeover(DeviceManager *dm)
{
    // First ensure the config hidraw is available (we need it for mode
    // commands even in HID takeover mode — e.g. switching to Steam mode)
    if (dm->hidraw_config_fd < 0) {
        dm->hidraw_config_fd = find_hidraw_config(dm);
    }

    // Ensure the firmware is in FPS/Windows mode. If a previous inputd
    // session was killed with SIGKILL (no cleanup), the controller may still
    // be stuck in SteamOS mode where the vendor hidraw produces no data.
    // We explicitly send the FPS mode command to guarantee the right state.
    if (dm->hidraw_config_fd >= 0) {
        unsigned char cmd_fps[] = { 0x04, 0x0a, 0x00 };
        if (write(dm->hidraw_config_fd, cmd_fps, sizeof(cmd_fps)) < 0) {
            fprintf(stderr, "[inputd] HID takeover: FPS mode cmd failed: %s\n",
                    strerror(errno));
        }
        usleep(100000);  // 100ms for firmware to process

        // Re-enable OS auto-detection so the firmware stays cooperative
        unsigned char cmd_auto[] = { 0x04, 0x09, 0x01 };
        write(dm->hidraw_config_fd, cmd_auto, sizeof(cmd_auto));
        usleep(200000);  // 200ms settle time

        fprintf(stderr, "[inputd] HID takeover: FPS mode restored\n");
    }

    // Find and open the button-reading hidraw
    dm->hidraw_button_fd = find_hidraw_buttons(dm);
    if (dm->hidraw_button_fd < 0) {
        fprintf(stderr, "[inputd] HID takeover: button hidraw not found\n");
        return -1;
    }

    dm->hid_takeover_active = true;
    fprintf(stderr, "[inputd] HID takeover mode initialized: "
            "config=%s, buttons=%s\n",
            dm->hidraw_config_path, dm->hidraw_button_path);
    return 0;
}

// --------------------------------------------------------------------------
// device_activate_gamepad_mode — Switch Legion Go S to SteamOS/gamepad mode
// --------------------------------------------------------------------------
// Sends two HID commands to the config interface:
//   1. Disable OS auto-detection (0x04 0x09 0x00)
//   2. Set SteamOS mode (0x04 0x0a 0x01)
//
// After these commands, the gamepad's evdev node (event11) starts producing
// proper button and axis events instead of routing through keyboard/mouse.
//
// Must be called AFTER device_init() so the hidraw interface is available.
// Returns 0 on success, -1 if no config interface found.
// --------------------------------------------------------------------------
int device_activate_gamepad_mode(DeviceManager *dm) {
    // Find the config hidraw if we haven't already
    if (dm->hidraw_config_fd < 0) {
        dm->hidraw_config_fd = find_hidraw_config(dm);
    }

    if (dm->hidraw_config_fd < 0) {
        fprintf(stderr, "[inputd] no Legion Go S hidraw config found "
                "— gamepad mode not available\n");
        return -1;
    }

    // Command 1: Disable OS auto-detection
    // Without this, the firmware may switch back to FPS mode on its own
    unsigned char cmd_no_autodetect[] = { 0x04, 0x09, 0x00 };
    if (write(dm->hidraw_config_fd, cmd_no_autodetect, sizeof(cmd_no_autodetect)) < 0) {
        fprintf(stderr, "[inputd] failed to disable autodetect: %s\n",
                strerror(errno));
    }

    // Small delay for the firmware to process
    usleep(100000);  // 100ms

    // Command 2: Switch to SteamOS/gamepad mode
    // This makes button and stick data flow through the gamepad HID interface
    // instead of the keyboard/mouse interfaces
    unsigned char cmd_steamos[] = { 0x04, 0x0a, 0x01 };
    if (write(dm->hidraw_config_fd, cmd_steamos, sizeof(cmd_steamos)) < 0) {
        fprintf(stderr, "[inputd] failed to set SteamOS mode: %s\n",
                strerror(errno));
        return -1;
    }

    dm->gamepad_mode_active = true;
    fprintf(stderr, "[inputd] Legion Go S switched to gamepad mode\n");

    // Give the firmware time to apply the mode switch before we try
    // reading from the gamepad evdev node
    usleep(500000);  // 500ms

    return 0;
}

// --------------------------------------------------------------------------
// device_restore_fps_mode — Switch back to FPS/Windows mode on shutdown
// --------------------------------------------------------------------------
// Restores the default mode so the controller works normally without inputd.
// --------------------------------------------------------------------------
void device_restore_fps_mode(DeviceManager *dm) {
    if (!dm->gamepad_mode_active || dm->hidraw_config_fd < 0) return;

    // Switch back to Windows/FPS mode
    unsigned char cmd_windows[] = { 0x04, 0x0a, 0x00 };
    if (write(dm->hidraw_config_fd, cmd_windows, sizeof(cmd_windows)) >= 0) {
        fprintf(stderr, "[inputd] Legion Go S restored to FPS mode\n");
    }

    // Re-enable OS auto-detection
    unsigned char cmd_autodetect[] = { 0x04, 0x09, 0x01 };
    write(dm->hidraw_config_fd, cmd_autodetect, sizeof(cmd_autodetect));

    dm->gamepad_mode_active = false;
}
