// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

//
// mapper.h — Input mapping engine
//
// The mapper is the brain of cc-inputd. It takes raw evdev events from
// physical devices and decides what to do with them based on the active
// profile. Each profile contains a set of MappingRules that say things
// like "when BTN_SOUTH is pressed, emit KEY_RETURN" or "when ABS_X
// changes, move the mouse."
//
// Three profiles exist:
//   - LOGIN:   Minimal mappings for the login screen (sticks = mouse, A = click)
//   - DESKTOP: Full desktop mappings (sticks = mouse, triggers = scroll,
//              face buttons = keys, bumpers = modifier combos)
//   - GAME:    Raw passthrough — forward everything to the virtual gamepad
//
// The active profile can be switched at runtime by cc-wm via IPC,
// e.g. when a game window gains focus.
//

#ifndef MAPPER_H
#define MAPPER_H

#include <linux/input.h>

// Forward declarations — these structs are defined in their own headers
typedef struct VirtualDevices VirtualDevices;
typedef struct MouseEmulator  MouseEmulator;

// --------------------------------------------------------------------------
// InputProfile — Which mapping set is currently active
// --------------------------------------------------------------------------
typedef enum InputProfile {
    PROFILE_LOGIN   = 0,   // Login screen: minimal, just mouse + click
    PROFILE_DESKTOP = 1,   // Full desktop: mouse, keyboard shortcuts, scrolling
    PROFILE_GAME    = 2    // Game mode: raw passthrough to virtual gamepad
} InputProfile;

// --------------------------------------------------------------------------
// ActionType — What kind of output a mapping rule produces
// --------------------------------------------------------------------------
typedef enum ActionType {
    ACTION_NONE         = 0,  // No action (rule is disabled / placeholder)
    ACTION_KEY          = 1,  // Emit a keyboard keycode (e.g. KEY_RETURN)
    ACTION_MOUSE_BUTTON = 2,  // Emit a mouse button (e.g. BTN_LEFT)
    ACTION_MOUSE_MOVE   = 3,  // Feed an axis value to the mouse emulator
    ACTION_COPICATOS    = 4,  // Trigger a CopiCatOS shell action (e.g. Spotlight)
    ACTION_GAMEPAD_FWD  = 5   // Forward event unchanged to virtual gamepad
} ActionType;

// --------------------------------------------------------------------------
// CcAction — CopiCatOS-specific actions (ACTION_COPICATOS targets)
// --------------------------------------------------------------------------
// These are high-level desktop actions that cc-inputd sends to cc-wm
// via IPC. They don't map to simple keycodes — they invoke shell features.
// --------------------------------------------------------------------------
typedef enum CcAction {
    CC_ACTION_SPOTLIGHT       = 0,  // Open/close Spotlight search overlay
    CC_ACTION_MISSION_CONTROL = 1,  // Show all windows (Expose equivalent)
    CC_ACTION_SHOW_DESKTOP    = 2,  // Minimize all windows, reveal desktop
    CC_ACTION_VOLUME_UP       = 3,  // Increase system volume
    CC_ACTION_VOLUME_DOWN     = 4,  // Decrease system volume
    CC_ACTION_BRIGHTNESS_UP   = 5,  // Increase screen brightness
    CC_ACTION_BRIGHTNESS_DOWN = 6   // Decrease screen brightness
} CcAction;

// --------------------------------------------------------------------------
// MappingRule — One input-to-output translation
// --------------------------------------------------------------------------
// A rule says: "When I see an evdev event with this type and code,
// perform this action with these parameters."
//
// Examples:
//   { EV_KEY, BTN_SOUTH, ACTION_KEY, KEY_RETURN, 0 }
//     -> "A button press emits Enter key"
//
//   { EV_ABS, ABS_X, ACTION_MOUSE_MOVE, 0, 0 }
//     -> "Left stick X axis drives horizontal mouse movement"
//
//   { EV_KEY, BTN_MODE, ACTION_COPICATOS, CC_ACTION_SPOTLIGHT, 0 }
//     -> "Legion button opens Spotlight"
// --------------------------------------------------------------------------
typedef struct MappingRule {
    unsigned short ev_type;   // Input event type (EV_KEY, EV_ABS, etc.)
    unsigned short ev_code;   // Input event code (BTN_SOUTH, ABS_X, etc.)
    ActionType     action;    // What to do when this event arrives
    int            param;     // Action-specific: keycode, button code, CcAction, or axis index
    int            param2;    // Second parameter (e.g. modifier key, or unused = 0)
} MappingRule;

// Maximum number of mapping rules in one profile.
// 64 is generous — most profiles need ~20 rules.
#define MAX_MAPPINGS 64

// --------------------------------------------------------------------------
// MappingProfile — A named set of mapping rules
// --------------------------------------------------------------------------
typedef struct MappingProfile {
    MappingRule rules[MAX_MAPPINGS];  // The rules for this profile
    int         rule_count;           // How many rules are active (0..MAX_MAPPINGS)
} MappingProfile;

// --------------------------------------------------------------------------
// Mapper — The mapping engine state
// --------------------------------------------------------------------------
typedef struct Mapper {
    // One profile per InputProfile enum value
    MappingProfile profiles[3];

    // Which profile is currently being used to process events
    InputProfile active_profile;

    // Trigger state tracking — used for scroll emulation in desktop mode.
    // When LT is held, the right stick becomes a scroll wheel.
    bool lt_pressed;
    bool rt_pressed;

    // Analog triggers report values 0..1023. We consider the trigger
    // "pressed" when its value exceeds this threshold. A low threshold
    // makes it hair-trigger; a high one requires a firm pull.
    int trigger_threshold;

} Mapper;

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------

// mapper_init — Set up default mapping rules for all three profiles.
// Hardcodes sensible defaults (A=click, B=Escape, sticks=mouse, etc.).
// `threshold` sets the analog trigger activation threshold.
void mapper_init(Mapper *m, int threshold);

// mapper_load_config — Overwrite the DESKTOP profile's rules with
// user-defined mappings loaded from the config file. Called at startup
// and on SIGHUP reload. `rules` and `count` come from config_load_input().
void mapper_load_config(Mapper *m, const MappingRule *rules, int count);

// mapper_process — Process one input event through the active profile.
// Looks up the event's type+code in the active profile's rules and
// executes the matching action (inject key, move mouse, fire CC action, etc.).
// `ev` is the raw event from evdev.
// `vd` is where keyboard/mouse/gamepad output goes.
// `mouse` is the mouse emulator (for axis-to-pointer conversion).
// Returns the CcAction if one was triggered, or -1 if not.
int mapper_process(Mapper *m, const struct input_event *ev,
                   VirtualDevices *vd, MouseEmulator *mouse);

// mapper_set_profile — Switch the active mapping profile.
// Typically called by IPC when cc-wm detects a game window gaining focus
// (switch to PROFILE_GAME) or losing focus (switch back to PROFILE_DESKTOP).
void mapper_set_profile(Mapper *m, InputProfile profile);

#endif // MAPPER_H
