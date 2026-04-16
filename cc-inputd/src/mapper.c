// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

//
// mapper.c — Button/axis mapping engine implementation
//
// This file implements the mapping engine that translates raw gamepad events
// into desktop actions. It manages three profiles (LOGIN, DESKTOP, GAME)
// each with their own set of rules. The DESKTOP profile is the primary one
// with full mappings; LOGIN strips out CopiCatOS-specific actions for
// security; GAME forwards everything unchanged.
//
// The mapping process works like this:
//   1. An input event arrives from a physical controller (e.g. BTN_SOUTH press)
//   2. We look up the event's type+code in the active profile's rule list
//   3. Based on the rule's ActionType, we either:
//      - Inject a keyboard key via uinput
//      - Inject a mouse button via uinput
//      - Signal a mouse axis update (stick-to-pointer)
//      - Return a CopiCatOS action code (Spotlight, Mission Control, etc.)
//      - Forward the raw event to the virtual gamepad
//

#include "mapper.h"
#include "uinput.h"
#include "mouse.h"

#include <stdio.h>
#include <string.h>
#include <linux/input-event-codes.h>

// --------------------------------------------------------------------------
// Helper: Add a mapping rule to a profile
// --------------------------------------------------------------------------
// This is a convenience function used during initialization to keep the
// setup code readable. It appends a new rule to the profile's rule list.
// --------------------------------------------------------------------------
static void add_rule(MappingProfile *profile,
                     unsigned short ev_type, unsigned short ev_code,
                     ActionType action, int param, int param2)
{
    // Safety check: don't overflow the rules array
    if (profile->rule_count >= MAX_MAPPINGS) {
        fprintf(stderr, "mapper: WARNING — rule limit (%d) reached, ignoring rule\n",
                MAX_MAPPINGS);
        return;
    }

    // Fill in the new rule at the next available slot
    MappingRule *r = &profile->rules[profile->rule_count];
    r->ev_type = ev_type;
    r->ev_code = ev_code;
    r->action  = action;
    r->param   = param;
    r->param2  = param2;

    profile->rule_count++;
}

// --------------------------------------------------------------------------
// Helper: Find a matching rule for an input event
// --------------------------------------------------------------------------
// Scans the active profile's rules looking for one that matches the given
// event type and code. Returns a pointer to the matching rule, or NULL
// if no rule matches (meaning this event should be ignored).
// --------------------------------------------------------------------------
static const MappingRule *find_rule(const MappingProfile *profile,
                                   unsigned short ev_type,
                                   unsigned short ev_code)
{
    for (int i = 0; i < profile->rule_count; i++) {
        if (profile->rules[i].ev_type == ev_type &&
            profile->rules[i].ev_code == ev_code) {
            return &profile->rules[i];
        }
    }
    return NULL;   // No matching rule found
}

// --------------------------------------------------------------------------
// Helper: Build the LOGIN profile from the DESKTOP profile
// --------------------------------------------------------------------------
// The LOGIN profile is a copy of DESKTOP with all ACTION_COPICATOS rules
// replaced by ACTION_NONE. This prevents Spotlight, Mission Control, and
// other shell features from being triggered on the login screen where
// they don't make sense and could be a security concern.
// --------------------------------------------------------------------------
static void build_login_from_desktop(Mapper *m)
{
    MappingProfile *login   = &m->profiles[PROFILE_LOGIN];
    MappingProfile *desktop = &m->profiles[PROFILE_DESKTOP];

    // Start with a clean slate
    login->rule_count = 0;

    // Copy each desktop rule, but disable CopiCatOS-specific actions
    for (int i = 0; i < desktop->rule_count; i++) {
        MappingRule rule = desktop->rules[i];

        if (rule.action == ACTION_COPICATOS) {
            // Replace CopiCatOS actions with no-ops on the login screen
            rule.action = ACTION_NONE;
            rule.param  = 0;
            rule.param2 = 0;
        }

        // Add the (possibly modified) rule to the login profile
        if (login->rule_count < MAX_MAPPINGS) {
            login->rules[login->rule_count] = rule;
            login->rule_count++;
        }
    }
}

// --------------------------------------------------------------------------
// Helper: Build the GAME profile
// --------------------------------------------------------------------------
// The GAME profile is simple: every known event type is forwarded unchanged
// to the virtual gamepad. We don't need individual rules per button because
// mapper_process() handles PROFILE_GAME as a special case — it forwards
// everything without consulting the rules table.
// --------------------------------------------------------------------------
static void build_game_profile(Mapper *m)
{
    MappingProfile *game = &m->profiles[PROFILE_GAME];

    // The game profile doesn't need explicit rules because mapper_process()
    // short-circuits to raw forwarding when PROFILE_GAME is active.
    // We just make sure the profile is empty/clean.
    game->rule_count = 0;
}

// --------------------------------------------------------------------------
// mapper_init — Set up default mapping rules for all three profiles
// --------------------------------------------------------------------------
// Hardcodes the default desktop mappings that make the Legion Go usable
// as a desktop input device. The `threshold` parameter sets how far the
// analog triggers must be pulled before they register as a click.
//
// Default desktop mappings:
//   Face buttons:
//     A (BTN_SOUTH) → Enter        (confirm / activate)
//     B (BTN_EAST)  → Escape       (cancel / go back)
//     X (BTN_WEST)  → Spotlight    (open search overlay)
//     Y (BTN_NORTH) → Mission Ctrl (show all windows)
//
//   Shoulders:
//     LB (BTN_TL) → Page Up        (scroll up one page)
//     RB (BTN_TR) → Page Down      (scroll down one page)
//
//   Center buttons:
//     Select (BTN_SELECT) → Show Desktop
//     Start  (BTN_START)  → Space    (general-purpose action)
//
//   D-pad (via ABS_HAT0X / ABS_HAT0Y axes):
//     Left/Right → Arrow keys
//     Up/Down    → Arrow keys
//
//   Triggers (analog, via ABS_Z / ABS_RZ axes):
//     Left trigger  (ABS_Z)  → Right mouse click
//     Right trigger (ABS_RZ) → Left mouse click
//
//   Right stick (ABS_RX / ABS_RY):
//     → Mouse movement (handled by mouse emulator)
// --------------------------------------------------------------------------
void mapper_init(Mapper *m, int threshold)
{
    // Zero out everything first — all profiles empty, no state
    memset(m, 0, sizeof(Mapper));

    // Start in desktop mode (the most common use case)
    m->active_profile = PROFILE_DESKTOP;

    // Set the trigger activation threshold
    // The triggers report 0 (released) to 1023 (fully pulled).
    // The threshold determines when we consider it "pressed".
    m->trigger_threshold = threshold;

    // Neither trigger is currently pressed
    m->lt_pressed = false;
    m->rt_pressed = false;

    // --- Build the DESKTOP profile with default rules ---
    MappingProfile *desktop = &m->profiles[PROFILE_DESKTOP];

    // Face buttons → keyboard keys and CopiCatOS actions
    add_rule(desktop, EV_KEY, BTN_SOUTH, ACTION_KEY,      KEY_ENTER,  0);
    add_rule(desktop, EV_KEY, BTN_EAST,  ACTION_KEY,      KEY_ESC,    0);
    add_rule(desktop, EV_KEY, BTN_WEST,  ACTION_COPICATOS, CC_ACTION_SPOTLIGHT, 0);
    add_rule(desktop, EV_KEY, BTN_NORTH, ACTION_COPICATOS, CC_ACTION_MISSION_CONTROL, 0);

    // Shoulder buttons → page navigation
    add_rule(desktop, EV_KEY, BTN_TL, ACTION_KEY, KEY_PAGEUP,   0);
    add_rule(desktop, EV_KEY, BTN_TR, ACTION_KEY, KEY_PAGEDOWN, 0);

    // Center buttons → desktop actions
    add_rule(desktop, EV_KEY, BTN_SELECT, ACTION_COPICATOS, CC_ACTION_SHOW_DESKTOP, 0);
    add_rule(desktop, EV_KEY, BTN_START,  ACTION_KEY,       KEY_SPACE,  0);

    // D-pad axes → arrow keys
    // The d-pad reports as ABS_HAT0X (-1=left, +1=right) and
    // ABS_HAT0Y (-1=up, +1=down). We store the direction in param2:
    //   param2 = -1 means "when value is -1, emit this key"
    //   param2 = +1 means "when value is +1, emit this key"
    // For d-pad rules, we store two rules per axis (one per direction).
    add_rule(desktop, EV_ABS, ABS_HAT0X, ACTION_KEY, KEY_LEFT,  -1);
    add_rule(desktop, EV_ABS, ABS_HAT0X, ACTION_KEY, KEY_RIGHT, +1);
    add_rule(desktop, EV_ABS, ABS_HAT0Y, ACTION_KEY, KEY_UP,    -1);
    add_rule(desktop, EV_ABS, ABS_HAT0Y, ACTION_KEY, KEY_DOWN,  +1);

    // Analog triggers → mouse buttons
    // These are handled specially in mapper_process() because they're
    // analog axes that we convert to digital press/release events.
    add_rule(desktop, EV_ABS, ABS_Z,  ACTION_MOUSE_BUTTON, BTN_RIGHT, 0);
    add_rule(desktop, EV_ABS, ABS_RZ, ACTION_MOUSE_BUTTON, BTN_LEFT,  0);

    // Right stick → mouse movement
    // The actual pointer math happens in mouse.c; the mapper just needs
    // to know these axes should be routed to the mouse emulator.
    add_rule(desktop, EV_ABS, ABS_RX, ACTION_MOUSE_MOVE, 0, 0);
    add_rule(desktop, EV_ABS, ABS_RY, ACTION_MOUSE_MOVE, 0, 0);

    // --- Build LOGIN profile from DESKTOP (strip CopiCatOS actions) ---
    build_login_from_desktop(m);

    // --- Build GAME profile (passthrough) ---
    build_game_profile(m);

    fprintf(stderr, "mapper: initialized with threshold=%d, "
            "desktop=%d rules, login=%d rules\n",
            threshold,
            m->profiles[PROFILE_DESKTOP].rule_count,
            m->profiles[PROFILE_LOGIN].rule_count);
}

// --------------------------------------------------------------------------
// mapper_load_config — Apply user-defined mappings from config file
// --------------------------------------------------------------------------
// Replaces the DESKTOP profile's rules with user-defined ones. After
// updating desktop, rebuilds the LOGIN profile (same rules minus CopiCatOS
// actions). The GAME profile is never modified — it's always passthrough.
//
// Parameters:
//   rules — array of MappingRule structs loaded from input.conf
//   count — number of rules in the array
//
// If `rules` is NULL or `count` is 0, the existing desktop rules are kept.
// --------------------------------------------------------------------------
void mapper_load_config(Mapper *m, const MappingRule *rules, int count)
{
    // If no rules provided, keep the current defaults
    if (!rules || count <= 0) {
        fprintf(stderr, "mapper: no config rules provided, keeping defaults\n");
        return;
    }

    // Clamp to our maximum rule count to prevent overflow
    if (count > MAX_MAPPINGS) {
        fprintf(stderr, "mapper: WARNING — config has %d rules, clamping to %d\n",
                count, MAX_MAPPINGS);
        count = MAX_MAPPINGS;
    }

    // Replace the desktop profile's rules with the config-provided ones
    MappingProfile *desktop = &m->profiles[PROFILE_DESKTOP];
    memcpy(desktop->rules, rules, count * sizeof(MappingRule));
    desktop->rule_count = count;

    // Rebuild login profile from the new desktop rules
    build_login_from_desktop(m);

    // Game profile stays as-is (always passthrough)

    fprintf(stderr, "mapper: loaded %d rules from config, "
            "login profile rebuilt with %d rules\n",
            count, m->profiles[PROFILE_LOGIN].rule_count);
}

// --------------------------------------------------------------------------
// mapper_process — Process one input event through the active profile
// --------------------------------------------------------------------------
// This is the hot path — called for every single event from every
// controller. It looks up the event in the active profile's rules and
// executes the appropriate action.
//
// Parameters:
//   m     — the mapper state
//   ev    — the raw input event from evdev
//   vd    — virtual devices for injecting keyboard/mouse/gamepad events
//   mouse — the mouse emulator for stick-to-pointer conversion
//
// Returns:
//   A CcAction value (>= 0) if a CopiCatOS action was triggered,
//   or -1 if no CopiCatOS action was triggered (even if other actions
//   like key presses were performed).
// --------------------------------------------------------------------------
int mapper_process(Mapper *m, const struct input_event *ev,
                   VirtualDevices *vd, MouseEmulator *mouse)
{
    // --- GAME MODE: forward everything unchanged ---
    // In game mode, the mapper is a simple passthrough. Every event goes
    // straight to the virtual gamepad without any transformation.
    if (m->active_profile == PROFILE_GAME) {
        uinput_gamepad_forward(vd, ev);
        return -1;   // No CopiCatOS action
    }

    // We only process key events (button presses) and absolute axis events
    // (d-pad, triggers, sticks). Ignore everything else (EV_SYN, EV_MSC, etc.)
    if (ev->type != EV_KEY && ev->type != EV_ABS) {
        return -1;
    }

    // Get the active profile's rule set
    const MappingProfile *profile = &m->profiles[m->active_profile];

    // ======================================================================
    // Handle EV_KEY events (button presses and releases)
    // ======================================================================
    if (ev->type == EV_KEY) {
        // Look up this button in the rule table
        const MappingRule *rule = find_rule(profile, EV_KEY, ev->code);
        if (!rule) {
            return -1;   // No rule for this button — ignore it
        }

        switch (rule->action) {
        case ACTION_KEY:
            // Emit a keyboard key press or release through the virtual keyboard.
            // ev->value: 1 = press, 0 = release, 2 = repeat (we pass all through)
            uinput_key(vd, rule->param, ev->value);
            break;

        case ACTION_MOUSE_BUTTON:
            // Emit a mouse button press or release through the virtual mouse.
            uinput_mouse_button(vd, rule->param, ev->value);
            break;

        case ACTION_COPICATOS:
            // Trigger a CopiCatOS shell action, but only on press (not release).
            // We don't want to fire the action twice (once on press, once on release).
            if (ev->value == 1) {
                return rule->param;   // Return the CcAction code to the caller
            }
            break;

        case ACTION_GAMEPAD_FWD:
            // Forward the raw event to the virtual gamepad
            uinput_gamepad_forward(vd, ev);
            break;

        case ACTION_NONE:
        case ACTION_MOUSE_MOVE:
            // ACTION_NONE: intentionally disabled mapping
            // ACTION_MOUSE_MOVE: doesn't apply to EV_KEY events
            break;
        }

        return -1;   // No CopiCatOS action (or it was already returned above)
    }

    // ======================================================================
    // Handle EV_ABS events (axes: d-pad, triggers, sticks)
    // ======================================================================
    if (ev->type == EV_ABS) {

        // ------------------------------------------------------------------
        // D-pad (ABS_HAT0X and ABS_HAT0Y)
        // ------------------------------------------------------------------
        // The d-pad reports as an absolute axis with values -1, 0, or +1.
        // We convert these into key press/release events:
        //   -1 or +1 → key press (the direction key)
        //   0        → key release (d-pad returned to center)
        //
        // We need to find the rule that matches both the axis code AND the
        // direction (stored in param2). For release (value=0), we release
        // both direction keys for this axis since we don't know which
        // direction was previously held.
        // ------------------------------------------------------------------
        if (ev->code == ABS_HAT0X || ev->code == ABS_HAT0Y) {
            if (ev->value == 0) {
                // D-pad released — release both direction keys for this axis.
                // We scan all rules for this axis code and release their keys.
                for (int i = 0; i < profile->rule_count; i++) {
                    const MappingRule *r = &profile->rules[i];
                    if (r->ev_type == EV_ABS && r->ev_code == ev->code &&
                        r->action == ACTION_KEY) {
                        uinput_key(vd, r->param, 0);   // 0 = release
                    }
                }
            } else {
                // D-pad pressed in a direction — find the rule matching
                // this direction (param2 stores -1 or +1)
                for (int i = 0; i < profile->rule_count; i++) {
                    const MappingRule *r = &profile->rules[i];
                    if (r->ev_type == EV_ABS && r->ev_code == ev->code &&
                        r->action == ACTION_KEY && r->param2 == ev->value) {
                        uinput_key(vd, r->param, 1);   // 1 = press
                        break;   // Only one direction can be active at a time
                    }
                }
            }
            return -1;
        }

        // ------------------------------------------------------------------
        // Analog triggers (ABS_Z = left trigger, ABS_RZ = right trigger)
        // ------------------------------------------------------------------
        // Triggers report analog values (typically 0-1023). We convert them
        // to digital press/release by comparing against the threshold.
        // We track lt_pressed/rt_pressed to avoid sending repeated events
        // every time the analog value changes slightly while held down.
        // ------------------------------------------------------------------
        if (ev->code == ABS_Z) {
            const MappingRule *rule = find_rule(profile, EV_ABS, ABS_Z);
            if (!rule) return -1;

            bool now_pressed = (ev->value >= m->trigger_threshold);

            if (now_pressed && !m->lt_pressed) {
                // Trigger just crossed the threshold — register a press
                m->lt_pressed = true;
                if (rule->action == ACTION_MOUSE_BUTTON) {
                    uinput_mouse_button(vd, rule->param, 1);
                } else if (rule->action == ACTION_KEY) {
                    uinput_key(vd, rule->param, 1);
                }
            } else if (!now_pressed && m->lt_pressed) {
                // Trigger returned below threshold — register a release
                m->lt_pressed = false;
                if (rule->action == ACTION_MOUSE_BUTTON) {
                    uinput_mouse_button(vd, rule->param, 0);
                } else if (rule->action == ACTION_KEY) {
                    uinput_key(vd, rule->param, 0);
                }
            }
            // If the pressed state hasn't changed, do nothing (avoid repeats)
            return -1;
        }

        if (ev->code == ABS_RZ) {
            const MappingRule *rule = find_rule(profile, EV_ABS, ABS_RZ);
            if (!rule) return -1;

            bool now_pressed = (ev->value >= m->trigger_threshold);

            if (now_pressed && !m->rt_pressed) {
                // Right trigger just pressed
                m->rt_pressed = true;
                if (rule->action == ACTION_MOUSE_BUTTON) {
                    uinput_mouse_button(vd, rule->param, 1);
                } else if (rule->action == ACTION_KEY) {
                    uinput_key(vd, rule->param, 1);
                }
            } else if (!now_pressed && m->rt_pressed) {
                // Right trigger released
                m->rt_pressed = false;
                if (rule->action == ACTION_MOUSE_BUTTON) {
                    uinput_mouse_button(vd, rule->param, 0);
                } else if (rule->action == ACTION_KEY) {
                    uinput_key(vd, rule->param, 0);
                }
            }
            return -1;
        }

        // ------------------------------------------------------------------
        // Right stick axes (ABS_RX and ABS_RY)
        // ------------------------------------------------------------------
        // These feed into the mouse emulator. We just update the raw axis
        // value here; the actual pointer math happens in mouse_tick() which
        // runs on a 120Hz timer.
        // ------------------------------------------------------------------
        if (ev->code == ABS_RX || ev->code == ABS_RY) {
            const MappingRule *rule = find_rule(profile, EV_ABS, ev->code);
            if (rule && rule->action == ACTION_MOUSE_MOVE && mouse) {
                mouse_update_axis(mouse, ev->code, ev->value);
            }
            return -1;
        }
    }

    // Event type/code not handled by any rule
    return -1;
}

// --------------------------------------------------------------------------
// mapper_set_profile — Switch the active mapping profile
// --------------------------------------------------------------------------
// Called by the IPC handler when cc-wm tells us to change modes:
//   - PROFILE_DESKTOP: normal desktop navigation
//   - PROFILE_LOGIN:   restricted mode for login screen
//   - PROFILE_GAME:    raw passthrough for games
//
// Logs the switch to stderr for debugging (visible in journalctl).
// --------------------------------------------------------------------------
void mapper_set_profile(Mapper *m, InputProfile profile)
{
    // Map profile enum to a human-readable name for the log message
    static const char *names[] = { "LOGIN", "DESKTOP", "GAME" };

    // Validate the profile value to prevent out-of-bounds array access
    if (profile < 0 || profile > PROFILE_GAME) {
        fprintf(stderr, "mapper: invalid profile %d, ignoring\n", profile);
        return;
    }

    m->active_profile = profile;

    // Reset trigger state when switching profiles.
    // If a trigger was held when we switched to game mode and released
    // there, we'd never see the release event in desktop mode. Resetting
    // prevents stuck buttons.
    m->lt_pressed = false;
    m->rt_pressed = false;

    fprintf(stderr, "mapper: switched to profile %s\n", names[profile]);
}
