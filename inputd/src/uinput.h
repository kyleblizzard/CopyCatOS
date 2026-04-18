// CopyCatOS — by Kyle Blizzard at Blizzard.show

//
// uinput.h — Virtual input devices via Linux uinput
//
// The kernel's uinput subsystem (/dev/uinput) lets userspace programs
// create "virtual" input devices. When inputd grabs the real Legion
// Go controllers, X11/XLibre can no longer see them. Instead, inputd
// injects translated events through these virtual devices, which X11
// sees as ordinary mouse, keyboard, and gamepad hardware.
//
// We create three virtual devices:
//   1. Mouse    — receives pointer movement and button clicks
//   2. Keyboard — receives key presses (mapped from gamepad buttons)
//   3. Gamepad  — forwards raw gamepad events in the GAME profile passthrough
//

#ifndef UINPUT_H
#define UINPUT_H

#include <linux/input.h>

// --------------------------------------------------------------------------
// VirtualDevices — File descriptors for the three uinput devices
// --------------------------------------------------------------------------
// Each fd corresponds to an open /dev/uinput device that we configured
// with the appropriate event types (EV_REL for mouse, EV_KEY for keyboard,
// EV_ABS + EV_KEY for gamepad). A value of -1 means "not yet created."
// --------------------------------------------------------------------------
typedef struct VirtualDevices {
    int mouse_fd;      // Virtual mouse: relative axes (REL_X, REL_Y) + buttons
    int keyboard_fd;   // Virtual keyboard: standard keycodes (KEY_A, KEY_SPACE, etc.)
    int gamepad_fd;    // Virtual gamepad: axes + buttons, forwarded 1:1 in game mode
} VirtualDevices;

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------

// uinput_init — Create all three virtual devices via /dev/uinput.
// Configures supported event types and codes for each device, then
// issues the UI_DEV_CREATE ioctl. Returns 0 on success, -1 on failure.
int  uinput_init(VirtualDevices *vd);

// uinput_mouse_move — Inject a relative mouse movement event.
// dx and dy are pixel deltas (positive = right/down).
// Writes EV_REL + EV_SYN to the mouse virtual device.
void uinput_mouse_move(VirtualDevices *vd, int dx, int dy);

// uinput_mouse_button — Inject a mouse button press or release.
// `button` is a Linux button code (e.g. BTN_LEFT, BTN_RIGHT).
// `pressed` is 1 for press, 0 for release.
void uinput_mouse_button(VirtualDevices *vd, int button, int pressed);

// uinput_key — Inject a keyboard key press or release.
// `keycode` is a Linux key code (e.g. KEY_SPACE, KEY_ENTER).
// `pressed` is 1 for press, 0 for release.
void uinput_key(VirtualDevices *vd, int keycode, int pressed);

// uinput_mouse_scroll — Inject scroll wheel events.
// `sx` is horizontal scroll (positive = right, negative = left).
// `sy` is vertical scroll (positive = up/away from user, negative = down).
// Maps to REL_HWHEEL and REL_WHEEL on the virtual mouse.
void uinput_mouse_scroll(VirtualDevices *vd, int sx, int sy);

// uinput_gamepad_forward — Forward a raw input event unchanged to the
// virtual gamepad. Used in game mode where we want the real controller
// events to pass through without remapping.
// `ev` points to a struct input_event read from the physical device.
void uinput_gamepad_forward(VirtualDevices *vd, const struct input_event *ev);

// uinput_shutdown — Destroy all three virtual devices (UI_DEV_DESTROY)
// and close their file descriptors. Safe to call with -1 fds.
void uinput_shutdown(VirtualDevices *vd);

#endif // UINPUT_H
