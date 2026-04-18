// CopyCatOS — by Kyle Blizzard at Blizzard.show

//
// uinput.c — Virtual device creation and event injection via /dev/uinput
//
// The input daemon reads raw events from the Legion Go's physical gamepad,
// then translates them into standard mouse, keyboard, and gamepad events
// that the rest of the desktop environment understands. To do this, we
// create three "virtual" input devices using the kernel's uinput interface:
//
//   1. Virtual Mouse    — emits pointer movement and button clicks
//   2. Virtual Keyboard — emits key presses (Enter, Escape, arrow keys, etc.)
//   3. Virtual Gamepad  — passes through gamepad events for game mode
//
// Other programs (X11, libinput, SDL) see these virtual devices as if they
// were real hardware. The daemon is the only thing that writes to them.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

// Linux uinput header — defines the ioctl commands and structs for
// creating virtual input devices. This is the kernel's official way
// to inject input events from userspace.
#include <linux/uinput.h>

// Linux input header — defines struct input_event, event types (EV_KEY,
// EV_REL, EV_ABS), key codes (KEY_A, BTN_LEFT), and axis codes (ABS_X).
#include <linux/input.h>

#include "uinput.h"

// --------------------------------------------------------------------------
// emit_event — Write a single input event to a uinput device
// --------------------------------------------------------------------------
// Every uinput event is a struct input_event written to the device fd.
// After writing one or more events, we must send a SYN_REPORT to tell
// the kernel "this batch of events is complete, process them now."
//
// Parameters:
//   fd    — the uinput file descriptor to write to
//   type  — event type (EV_REL for mouse movement, EV_KEY for buttons, etc.)
//   code  — which specific axis or key (REL_X, KEY_A, BTN_LEFT, etc.)
//   value — the event value (for keys: 1=press, 0=release; for axes: delta)
//
// Returns true on success, false on write error.
// --------------------------------------------------------------------------
static bool emit_event(int fd, unsigned short type, unsigned short code, int value) {
    struct input_event ev;

    // Zero the struct to clear padding and timestamp fields.
    // The kernel fills in the timestamp automatically.
    memset(&ev, 0, sizeof(ev));

    ev.type  = type;
    ev.code  = code;
    ev.value = value;

    ssize_t written = write(fd, &ev, sizeof(ev));
    if (written < 0) {
        fprintf(stderr, "[inputd] uinput write failed: %s\n", strerror(errno));
        return false;
    }

    return true;
}

// --------------------------------------------------------------------------
// emit_syn — Send a SYN_REPORT event to finalize a batch of events
// --------------------------------------------------------------------------
// The kernel groups input events between SYN_REPORT markers. Nothing
// actually happens (no cursor movement, no key registration) until the
// SYN_REPORT tells the kernel the batch is complete.
// --------------------------------------------------------------------------
static bool emit_syn(int fd) {
    return emit_event(fd, EV_SYN, SYN_REPORT, 0);
}

// --------------------------------------------------------------------------
// create_virtual_mouse — Set up a virtual mouse device
// --------------------------------------------------------------------------
// Creates a uinput device that can emit relative mouse movements (REL_X,
// REL_Y), scroll wheel events (REL_WHEEL), and button clicks (left,
// right, middle).
//
// Returns the uinput fd on success, or -1 on failure.
// --------------------------------------------------------------------------
static int create_virtual_mouse(void) {
    // Open the uinput control device. O_WRONLY because we only write
    // events to it. O_NONBLOCK so writes don't block.
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[inputd] failed to open /dev/uinput: %s\n",
                strerror(errno));
        return -1;
    }

    // --- Tell the kernel which event types this device supports ---

    // EV_REL — relative axis events (mouse movement, scroll wheel)
    if (ioctl(fd, UI_SET_EVBIT, EV_REL) < 0) goto fail;

    // EV_KEY — button/key events (mouse buttons)
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) goto fail;

    // EV_SYN — synchronization events (every device needs this)
    if (ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) goto fail;

    // --- Specify which relative axes we support ---

    // REL_X and REL_Y — horizontal and vertical pointer movement
    if (ioctl(fd, UI_SET_RELBIT, REL_X) < 0) goto fail;
    if (ioctl(fd, UI_SET_RELBIT, REL_Y) < 0) goto fail;

    // REL_WHEEL — vertical scroll wheel
    if (ioctl(fd, UI_SET_RELBIT, REL_WHEEL) < 0) goto fail;

    // REL_HWHEEL — horizontal scroll wheel (for left stick horizontal scrolling)
    if (ioctl(fd, UI_SET_RELBIT, REL_HWHEEL) < 0) goto fail;

    // --- Specify which buttons we support ---

    // Standard three mouse buttons
    if (ioctl(fd, UI_SET_KEYBIT, BTN_LEFT)   < 0) goto fail;
    if (ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT)  < 0) goto fail;
    if (ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE) < 0) goto fail;

    // --- Configure the device identity ---
    // This is what shows up in /proc/bus/input/devices and in
    // tools like evtest or libinput list-devices.
    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "CopyCatOS Virtual Mouse");
    setup.id.bustype = BUS_VIRTUAL;   // Mark it as a virtual device
    setup.id.vendor  = 0x0000;        // No real vendor
    setup.id.product = 0x0001;        // Arbitrary product ID
    setup.id.version = 1;

    // Apply the setup and create the device
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) goto fail;
    if (ioctl(fd, UI_DEV_CREATE, 0)     < 0) goto fail;

    fprintf(stderr, "[inputd] created virtual mouse\n");
    return fd;

fail:
    fprintf(stderr, "[inputd] virtual mouse setup failed: %s\n",
            strerror(errno));
    close(fd);
    return -1;
}

// --------------------------------------------------------------------------
// create_virtual_keyboard — Set up a virtual keyboard device
// --------------------------------------------------------------------------
// Creates a uinput device that can emit key press and release events for
// all the keys we might need: letters, modifiers, function keys, navigation
// keys, etc. We register every key we could conceivably emit so we don't
// have to recreate the device if mappings change.
//
// Returns the uinput fd on success, or -1 on failure.
// --------------------------------------------------------------------------
static int create_virtual_keyboard(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[inputd] failed to open /dev/uinput: %s\n",
                strerror(errno));
        return -1;
    }

    // This device only emits key events (no axes, no relative movement)
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) goto fail;
    if (ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) goto fail;

    // --- Register all keys we might emit ---
    // We cast a wide net here. It's harmless to register keys we never
    // use, and it saves us from having to modify this code every time
    // we add a new key mapping.

    // Navigation and editing keys
    int nav_keys[] = {
        KEY_ENTER, KEY_ESC, KEY_TAB, KEY_BACKSPACE, KEY_SPACE,
        KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
        KEY_PAGEUP, KEY_PAGEDOWN, KEY_HOME, KEY_END
    };
    for (int i = 0; i < (int)(sizeof(nav_keys) / sizeof(nav_keys[0])); i++) {
        if (ioctl(fd, UI_SET_KEYBIT, nav_keys[i]) < 0) goto fail;
    }

    // Modifier keys — needed for keyboard shortcuts like Ctrl+C
    int mod_keys[] = {
        KEY_LEFTCTRL, KEY_LEFTALT, KEY_LEFTSHIFT, KEY_LEFTMETA,
        KEY_RIGHTCTRL, KEY_RIGHTALT, KEY_RIGHTSHIFT, KEY_RIGHTMETA
    };
    for (int i = 0; i < (int)(sizeof(mod_keys) / sizeof(mod_keys[0])); i++) {
        if (ioctl(fd, UI_SET_KEYBIT, mod_keys[i]) < 0) goto fail;
    }

    // Letter keys A-Z — KEY_A is 30, and the codes are sequential
    // through KEY_Z (which is 44), but they're NOT contiguous in the
    // Linux input header. We list them explicitly to be safe.
    int letter_keys[] = {
        KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G,
        KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N,
        KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U,
        KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z
    };
    for (int i = 0; i < (int)(sizeof(letter_keys) / sizeof(letter_keys[0])); i++) {
        if (ioctl(fd, UI_SET_KEYBIT, letter_keys[i]) < 0) goto fail;
    }

    // Function keys F1-F12
    for (int key = KEY_F1; key <= KEY_F12; key++) {
        if (ioctl(fd, UI_SET_KEYBIT, key) < 0) goto fail;
    }

    // --- Configure the device identity ---
    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "CopyCatOS Virtual Keyboard");
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor  = 0x0000;
    setup.id.product = 0x0002;
    setup.id.version = 1;

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) goto fail;
    if (ioctl(fd, UI_DEV_CREATE, 0)     < 0) goto fail;

    fprintf(stderr, "[inputd] created virtual keyboard\n");
    return fd;

fail:
    fprintf(stderr, "[inputd] virtual keyboard setup failed: %s\n",
            strerror(errno));
    close(fd);
    return -1;
}

// --------------------------------------------------------------------------
// create_virtual_gamepad — Set up a virtual gamepad device
// --------------------------------------------------------------------------
// Creates a uinput device that looks like a standard gamepad with:
//   - Two analog sticks (ABS_X/Y and ABS_RX/RY)
//   - Two analog triggers (ABS_Z and ABS_RZ)
//   - A D-pad (ABS_HAT0X and ABS_HAT0Y)
//   - Standard face buttons, bumpers, and meta buttons
//
// This virtual gamepad is used in "game mode" to pass controller events
// through to games, potentially with remapped buttons.
//
// Returns the uinput fd on success, or -1 on failure.
// --------------------------------------------------------------------------
static int create_virtual_gamepad(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[inputd] failed to open /dev/uinput: %s\n",
                strerror(errno));
        return -1;
    }

    // Gamepad needs button events, absolute axis events, and sync
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) goto fail;
    if (ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0) goto fail;
    if (ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) goto fail;

    // --- Register gamepad buttons ---
    // These use the BTN_* constants from linux/input-event-codes.h.
    // The naming follows the Xbox convention (SOUTH=A, EAST=B, etc.)
    // but the physical layout depends on the controller.
    int buttons[] = {
        BTN_SOUTH,   // A / Cross
        BTN_EAST,    // B / Circle
        BTN_NORTH,   // Y / Triangle
        BTN_WEST,    // X / Square
        BTN_TL,      // Left bumper (LB)
        BTN_TR,      // Right bumper (RB)
        BTN_TL2,     // Y1 back paddle (mapped from HID vendor interface)
        BTN_TR2,     // Y2 back paddle (mapped from HID vendor interface)
        BTN_SELECT,  // Back / Select / View
        BTN_START,   // Start / Menu
        BTN_MODE,    // Guide / Home / Legion L button
        BTN_Z,       // Legion R button (mapped from HID vendor interface)
        BTN_THUMBL,  // Left stick click (L3)
        BTN_THUMBR   // Right stick click (R3)
    };
    for (int i = 0; i < (int)(sizeof(buttons) / sizeof(buttons[0])); i++) {
        if (ioctl(fd, UI_SET_KEYBIT, buttons[i]) < 0) goto fail;
    }

    // --- Register absolute axes ---
    // Each axis needs a UI_SET_ABSBIT call to declare it, plus a
    // uinput_abs_setup to define its range and properties.

    // The axes we need and their ranges:
    //   Sticks (ABS_X, ABS_Y, ABS_RX, ABS_RY): -32768 to 32767
    //   Triggers (ABS_Z, ABS_RZ): 0 to 255
    //   D-pad (ABS_HAT0X, ABS_HAT0Y): -1 to 1

    // Stick axes — full signed 16-bit range
    int stick_axes[] = { ABS_X, ABS_Y, ABS_RX, ABS_RY };
    for (int i = 0; i < 4; i++) {
        if (ioctl(fd, UI_SET_ABSBIT, stick_axes[i]) < 0) goto fail;

        // uinput_abs_setup tells the kernel the range and characteristics
        // of this axis. Flat is the deadzone, fuzz is the noise filter.
        struct uinput_abs_setup abs_setup;
        memset(&abs_setup, 0, sizeof(abs_setup));
        abs_setup.code = stick_axes[i];
        abs_setup.absinfo.minimum    = -32768;  // Leftmost / topmost position
        abs_setup.absinfo.maximum    =  32767;  // Rightmost / bottommost position
        abs_setup.absinfo.fuzz       = 16;      // Small noise filter
        abs_setup.absinfo.flat       = 128;     // Deadzone around center
        abs_setup.absinfo.resolution = 0;       // Not applicable for gamepads

        if (ioctl(fd, UI_ABS_SETUP, &abs_setup) < 0) goto fail;
    }

    // Trigger axes — unsigned 8-bit range (not pressed = 0, fully pressed = 255)
    int trigger_axes[] = { ABS_Z, ABS_RZ };
    for (int i = 0; i < 2; i++) {
        if (ioctl(fd, UI_SET_ABSBIT, trigger_axes[i]) < 0) goto fail;

        struct uinput_abs_setup abs_setup;
        memset(&abs_setup, 0, sizeof(abs_setup));
        abs_setup.code = trigger_axes[i];
        abs_setup.absinfo.minimum = 0;
        abs_setup.absinfo.maximum = 255;
        abs_setup.absinfo.fuzz    = 0;
        abs_setup.absinfo.flat    = 0;

        if (ioctl(fd, UI_ABS_SETUP, &abs_setup) < 0) goto fail;
    }

    // D-pad axes — three-state: -1 (left/up), 0 (center), 1 (right/down)
    int hat_axes[] = { ABS_HAT0X, ABS_HAT0Y };
    for (int i = 0; i < 2; i++) {
        if (ioctl(fd, UI_SET_ABSBIT, hat_axes[i]) < 0) goto fail;

        struct uinput_abs_setup abs_setup;
        memset(&abs_setup, 0, sizeof(abs_setup));
        abs_setup.code = hat_axes[i];
        abs_setup.absinfo.minimum = -1;
        abs_setup.absinfo.maximum =  1;
        abs_setup.absinfo.fuzz    = 0;
        abs_setup.absinfo.flat    = 0;

        if (ioctl(fd, UI_ABS_SETUP, &abs_setup) < 0) goto fail;
    }

    // --- Configure the device identity ---
    // We masquerade as an Xbox 360 wired controller so that Steam, SDL,
    // and other game input libraries recognize the device and apply the
    // correct default button mappings. SDL computes a GUID from bustype +
    // vendor + product, and Xbox 360 is in every built-in mapping database.
    // Without this, our virtual gamepad shows up as "Unknown" and games
    // get scrambled button assignments.
    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "Microsoft X-Box 360 pad");
    setup.id.bustype = BUS_USB;       // Must be USB so SDL GUID matches real xpad
    setup.id.vendor  = 0x045e;        // Microsoft
    setup.id.product = 0x028e;        // Xbox 360 Controller
    setup.id.version = 0x0110;

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) goto fail;
    if (ioctl(fd, UI_DEV_CREATE, 0)     < 0) goto fail;

    fprintf(stderr, "[inputd] created virtual gamepad\n");
    return fd;

fail:
    fprintf(stderr, "[inputd] virtual gamepad setup failed: %s\n",
            strerror(errno));
    close(fd);
    return -1;
}

// --------------------------------------------------------------------------
// uinput_init — Create all three virtual devices
// --------------------------------------------------------------------------
// Called once at daemon startup. Creates the virtual mouse, keyboard, and
// gamepad devices. The mouse and keyboard are required — if either fails,
// the daemon can't function. The gamepad is optional (only needed for
// game mode passthrough).
//
// Parameters:
//   vdev — a freshly allocated VirtualDevices struct to populate
//
// Returns true if at least the mouse and keyboard were created.
// --------------------------------------------------------------------------
int uinput_init(VirtualDevices *vdev) {
    // Initialize all fds to -1 (invalid) so we know which ones to
    // skip during shutdown if creation fails partway through.
    vdev->mouse_fd    = -1;
    vdev->keyboard_fd = -1;
    vdev->gamepad_fd  = -1;

    // Create each virtual device
    vdev->mouse_fd    = create_virtual_mouse();
    vdev->keyboard_fd = create_virtual_keyboard();
    vdev->gamepad_fd  = create_virtual_gamepad();

    // Mouse and keyboard are essential — without them the desktop mode
    // has no way to move the cursor or type. The gamepad is nice-to-have.
    if (vdev->mouse_fd < 0 || vdev->keyboard_fd < 0) {
        fprintf(stderr, "[inputd] FATAL: failed to create essential "
                "virtual devices (mouse=%d, keyboard=%d)\n",
                vdev->mouse_fd, vdev->keyboard_fd);
        uinput_shutdown(vdev);
        return -1;
    }

    if (vdev->gamepad_fd < 0) {
        fprintf(stderr, "[inputd] WARNING: virtual gamepad not created, "
                "game mode passthrough will be unavailable\n");
    }

    fprintf(stderr, "[inputd] virtual devices ready\n");
    return 0;
}

// --------------------------------------------------------------------------
// uinput_mouse_move — Emit a relative mouse movement
// --------------------------------------------------------------------------
// Moves the virtual mouse pointer by (dx, dy) pixels. Positive dx moves
// right, positive dy moves down (standard screen coordinates).
//
// Parameters:
//   vdev — the virtual devices struct
//   dx   — horizontal movement in pixels (negative = left, positive = right)
//   dy   — vertical movement in pixels (negative = up, positive = down)
// --------------------------------------------------------------------------
void uinput_mouse_move(VirtualDevices *vdev, int dx, int dy) {
    if (vdev->mouse_fd < 0) return;

    // Emit X and Y movement as separate events, then a SYN_REPORT to
    // tell the kernel both values belong to the same "frame" of movement.
    // Only emit an axis event if there's actual movement on that axis.
    if (dx != 0) emit_event(vdev->mouse_fd, EV_REL, REL_X, dx);
    if (dy != 0) emit_event(vdev->mouse_fd, EV_REL, REL_Y, dy);

    // Always send SYN even if both are zero — this ensures any
    // previously buffered events get flushed.
    emit_syn(vdev->mouse_fd);
}

// --------------------------------------------------------------------------
// uinput_mouse_button — Emit a mouse button press or release
// --------------------------------------------------------------------------
// Parameters:
//   vdev   — the virtual devices struct
//   button — which button (BTN_LEFT, BTN_RIGHT, or BTN_MIDDLE)
//   value  — 1 for press, 0 for release
// --------------------------------------------------------------------------
void uinput_mouse_button(VirtualDevices *vdev, int button, int value) {
    if (vdev->mouse_fd < 0) return;

    emit_event(vdev->mouse_fd, EV_KEY, button, value);
    emit_syn(vdev->mouse_fd);
}

// --------------------------------------------------------------------------
// uinput_mouse_scroll — Emit scroll wheel events
// --------------------------------------------------------------------------
// Injects horizontal and/or vertical scroll wheel events through the virtual
// mouse. Used by the left stick scroll emulator to produce smooth scrolling.
//
// Parameters:
//   vdev — the virtual devices struct
//   sx   — horizontal scroll (positive = scroll right, negative = scroll left)
//   sy   — vertical scroll (positive = scroll up/away, negative = scroll down)
// --------------------------------------------------------------------------
void uinput_mouse_scroll(VirtualDevices *vdev, int sx, int sy) {
    if (vdev->mouse_fd < 0) return;

    // Emit horizontal scroll if non-zero
    if (sx != 0) emit_event(vdev->mouse_fd, EV_REL, REL_HWHEEL, sx);

    // Emit vertical scroll if non-zero
    if (sy != 0) emit_event(vdev->mouse_fd, EV_REL, REL_WHEEL, sy);

    // Send SYN_REPORT to finalize the scroll event batch
    emit_syn(vdev->mouse_fd);
}

// --------------------------------------------------------------------------
// uinput_key — Emit a keyboard key press or release
// --------------------------------------------------------------------------
// Parameters:
//   vdev    — the virtual devices struct
//   keycode — which key (KEY_A, KEY_ENTER, KEY_LEFTCTRL, etc.)
//   value   — 1 for press, 0 for release, 2 for repeat (auto-repeat)
// --------------------------------------------------------------------------
void uinput_key(VirtualDevices *vdev, int keycode, int value) {
    if (vdev->keyboard_fd < 0) return;

    emit_event(vdev->keyboard_fd, EV_KEY, keycode, value);
    emit_syn(vdev->keyboard_fd);
}

// --------------------------------------------------------------------------
// uinput_gamepad_forward — Forward a raw gamepad event to the virtual gamepad
// --------------------------------------------------------------------------
// In game mode, we want to pass controller events through to games with
// minimal transformation. This function takes a raw input_event read from
// the physical controller and writes it to the virtual gamepad.
//
// If the event is already a SYN_REPORT, we just forward it as-is (no need
// to send a second SYN_REPORT). For all other event types, we forward the
// event and then send our own SYN_REPORT to ensure timely delivery.
//
// Parameters:
//   vdev — the virtual devices struct
//   ev   — pointer to the raw input_event from the physical controller
// --------------------------------------------------------------------------
void uinput_gamepad_forward(VirtualDevices *vdev, const struct input_event *ev) {
    if (vdev->gamepad_fd < 0 || !ev) return;

    // Write the event directly to the virtual gamepad
    ssize_t written = write(vdev->gamepad_fd, ev, sizeof(*ev));
    if (written < 0) {
        fprintf(stderr, "[inputd] gamepad forward failed: %s\n",
                strerror(errno));
        return;
    }

    // If the event we just forwarded is NOT a SYN_REPORT, we need to
    // send one so the kernel processes the event promptly. If it IS
    // a SYN_REPORT, don't send a duplicate — that would create an
    // empty event frame.
    if (ev->type != EV_SYN) {
        emit_syn(vdev->gamepad_fd);
    }
}

// --------------------------------------------------------------------------
// uinput_shutdown — Destroy all virtual devices and close fds
// --------------------------------------------------------------------------
// Called during daemon shutdown. Sends UI_DEV_DESTROY to cleanly remove
// each virtual device from the kernel, then closes the file descriptors.
//
// Parameters:
//   vdev — the virtual devices struct to tear down
// --------------------------------------------------------------------------
void uinput_shutdown(VirtualDevices *vdev) {
    if (!vdev) return;

    // Helper macro to avoid repeating the same pattern three times.
    // UI_DEV_DESTROY tells the kernel to remove the virtual device,
    // then we close the fd to release the file handle.
    #define DESTROY_VDEV(fd_field, label) do {                          \
        if (vdev->fd_field >= 0) {                                      \
            if (ioctl(vdev->fd_field, UI_DEV_DESTROY) < 0) {           \
                fprintf(stderr, "[inputd] UI_DEV_DESTROY " label    \
                        " failed: %s\n", strerror(errno));             \
            }                                                           \
            close(vdev->fd_field);                                      \
            vdev->fd_field = -1;                                        \
            fprintf(stderr, "[inputd] destroyed virtual " label "\n"); \
        }                                                               \
    } while (0)

    DESTROY_VDEV(mouse_fd,    "mouse");
    DESTROY_VDEV(keyboard_fd, "keyboard");
    DESTROY_VDEV(gamepad_fd,  "gamepad");

    #undef DESTROY_VDEV

    fprintf(stderr, "[inputd] virtual devices shut down\n");
}
