// CopyCatOS — by Kyle Blizzard at Blizzard.show

//
// device.h — Physical input device discovery and management
//
// Uses libudev to enumerate /dev/input/event* nodes at startup and to
// monitor for hotplug events (controllers plugged in / unplugged while
// the daemon is running). Each discovered device is opened via evdev
// and optionally "grabbed" (EVIOCGRAB) so that its raw events are
// consumed exclusively by inputd instead of being seen by X11.
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
// The Lenovo Legion Go family exposes gamepad controls through USB HID.
// Different hardware revisions use different USB chips:
//
//   Original Legion Go: Lenovo vendor (0x17ef), products 0x6182 / 0x61eb
//   Legion Go S:        WCH.cn USB chip (0x1a86), product 0xe310
//
// We match on ALL known combinations so both models are detected.
// --------------------------------------------------------------------------
#define LEGION_GO_VID       0x17ef   // Lenovo USB vendor ID (original)
#define LEGION_GO_PID1      0x6182   // Legion Go controller — primary
#define LEGION_GO_PID2      0x61eb   // Legion Go controller — secondary

#define LEGION_GO_S_VID     0x1a86   // WCH.cn USB vendor ID (Legion Go S)
#define LEGION_GO_S_PID     0xe310   // Legion Go S controller

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
    bool is_system_keys;          // True if this has volume/media keys (not gamepad)

    bool grabbed;                 // True if we successfully called EVIOCGRAB
                                  // (exclusive access — X11 won't see events)
} InputDevice;

// Maximum number of physical devices we track at once.
// 16 is generous: gamepad + power button + system keys + extras.
// We raised this from 8 because the Legion Go S exposes many HID
// interfaces that all get detected as system keys devices.
#define MAX_DEVICES 16

// --------------------------------------------------------------------------
// DeviceManager — Owns udev context, monitor, and the device list
// --------------------------------------------------------------------------
typedef struct DeviceManager {
    struct udev         *udev;       // libudev context handle
    struct udev_monitor *monitor;    // Watches for add/remove events
    int                  mon_fd;     // Monitor's file descriptor (for epoll)

    InputDevice devices[MAX_DEVICES]; // Fixed-size array of opened devices
    int         device_count;         // How many slots are currently in use

    // ---------- Legion Go S HID interfaces ----------
    // The Legion Go S WCH.cn controller exposes multiple vendor-specific
    // HID interfaces (usage page 0xFFA0). We use two of them:
    //
    // 1. Config interface (hidraw2, 29-byte descriptor):
    //    Write-only — used to send firmware mode commands (SteamOS mode,
    //    FPS mode, autodetect on/off). Not polled by epoll.
    //
    // 2. Button interface (hidraw5, 21-byte descriptor):
    //    Read-only — streams 64-byte reports at ~100Hz containing button
    //    state (bytes 0-2) and IMU data (bytes 14-25). Only produces data
    //    in FPS mode; goes silent in SteamOS mode.
    //
    // In desktop mode (HID takeover), we stay in FPS mode and read buttons
    // from the button hidraw while getting sticks/triggers from XInput evdev.
    // In Steam mode, we send the SteamOS command and let Steam handle input.
    int  hidraw_config_fd;           // fd to the config hidraw (write cmds), or -1
    char hidraw_config_path[256];    // e.g. "/dev/hidraw2"

    int  hidraw_button_fd;           // fd to the button hidraw (read reports), or -1
    char hidraw_button_path[256];    // e.g. "/dev/hidraw5"

    bool gamepad_mode_active;        // true if we sent the SteamOS mode cmd
    bool hid_takeover_active;        // true if reading buttons from hidraw
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
// Also restores the Legion Go S to FPS/Windows mode if we switched it.
void device_shutdown(DeviceManager *dm);

// device_activate_gamepad_mode — Send the HID command to switch a Legion Go S
// from FPS/Windows mode to SteamOS/gamepad mode. Must be called AFTER
// device_init() so the hidraw config interface has been discovered.
// Returns 0 on success, -1 if no config interface found.
int device_activate_gamepad_mode(DeviceManager *dm);

// device_restore_fps_mode — Send the HID command to switch back to FPS/Windows
// mode. Called during shutdown so the controller is usable without inputd.
void device_restore_fps_mode(DeviceManager *dm);

// device_init_hid_takeover — Set up HID takeover mode for desktop use.
// Finds the button-reading hidraw interface (hidraw5), opens it read-only
// with O_NONBLOCK, and stores the fd in dm->hidraw_button_fd.
// Does NOT send the SteamOS mode command — stays in FPS mode so the
// vendor hidraw streams button data.
// Returns 0 on success, -1 if the button hidraw wasn't found.
int device_init_hid_takeover(DeviceManager *dm);

#endif // DEVICE_H
