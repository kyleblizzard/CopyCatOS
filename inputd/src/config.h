// CopyCatOS — by Kyle Blizzard at Blizzard.show

//
// config.h — Input daemon configuration types and API
//
// Defines the InputConfig struct that holds all user-configurable settings
// loaded from ~/.config/copycatos/input.conf. The config file uses a simple
// INI format with [section] headers, following the same pattern as the
// desktop.conf parser in systemcontrol.
//
// Sections:
//   [mouse]             — joystick-to-pointer sensitivity/response curve
//   [triggers]          — analog trigger threshold for button activation
//   [power]             — power button short/long press timing and actions
//   [desktop_mappings]  — button-to-action mappings for desktop mode
//   [game_overrides]    — per-application profile overrides for game mode
//

#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

// --------------------------------------------------------------------------
// Config action types for desktop mapping rules
// --------------------------------------------------------------------------
// These are simplified action categories used during config parsing.
// They map into the full ActionType enum in mapper.h at load time.
//
//   CFG_ACTION_KEY       — Emit a keyboard keycode (e.g. KEY_RETURN)
//   CFG_ACTION_MOUSE     — Emit a mouse button (e.g. BTN_LEFT)
//   CFG_ACTION_COPYCATOS — Trigger a CopyCatOS-specific action (e.g. Spotlight)
//                          These are sent over IPC to the session bridge.
//   CFG_ACTION_NONE      — No mapping; the event is silently dropped.
// --------------------------------------------------------------------------
typedef enum {
    CFG_ACTION_NONE = 0,
    CFG_ACTION_KEY,
    CFG_ACTION_MOUSE,
    CFG_ACTION_COPYCATOS
} CfgActionType;

// --------------------------------------------------------------------------
// MappingRule — One button-to-action mapping
// --------------------------------------------------------------------------
// Maps a Linux input event code (e.g. BTN_SOUTH = 0x130) to an action.
// For ACTION_KEY/ACTION_MOUSE, `param` is the target keycode/button code.
// For ACTION_COPYCATOS, `param_str` names the action (e.g. "spotlight").
// --------------------------------------------------------------------------
typedef struct {
    int           event_code;          // Source event code (e.g. BTN_SOUTH)
    CfgActionType action;              // What kind of action to perform
    int           param;               // Target code for KEY/MOUSE actions
    char          param_str[64];       // Action name for COPYCATOS actions
} CfgMappingRule;

// Maximum number of desktop mapping rules we support.
// 32 is far more than the Legion Go's ~16 buttons.
#define MAX_CFG_MAPPING_RULES 32

// --------------------------------------------------------------------------
// GameOverride — Per-application profile override
// --------------------------------------------------------------------------
// When the focused window's WM_CLASS or executable name matches `pattern`,
// the daemon switches to the named profile. This lets games use raw
// gamepad passthrough while the desktop uses remapped controls.
// --------------------------------------------------------------------------
typedef struct {
    char pattern[128];                 // Window class or executable to match
    char profile[64];                  // Profile name (e.g. "gamepad")
} GameOverride;

// Maximum number of game override rules.
#define MAX_GAME_OVERRIDES 16

// --------------------------------------------------------------------------
// InputConfig — All user-configurable daemon settings
// --------------------------------------------------------------------------
typedef struct InputConfig {
    // [mouse] section — joystick-to-pointer parameters
    int    deadzone;                   // Stick axis values below this are ignored
                                       // (prevents drift). Range: 0–32767.
    double sensitivity;                // Linear multiplier applied to axis values
    double exponent;                   // Response curve exponent (2.0 = quadratic,
                                       // 1.0 = linear). Higher = more precision
                                       // at small deflections.
    int    max_speed;                  // Maximum pointer speed in pixels per tick

    // [triggers] section
    int    trigger_threshold;          // Analog value (0–255) at which a trigger
                                       // is considered "pressed"

    // [power] section — power button timing
    int    short_press_ms;             // Max duration (ms) for a short press
    int    long_press_ms;              // Duration (ms) to trigger a long press
    char   short_action[32];           // Action name for short press (e.g. "suspend")
    char   long_action[32];            // Action name for long press (e.g. "restart")

    // [desktop_mappings] section
    CfgMappingRule desktop_rules[MAX_CFG_MAPPING_RULES];
    int            desktop_rule_count;

    // [game_overrides] section
    GameOverride game_overrides[MAX_GAME_OVERRIDES];
    int          game_override_count;

} InputConfig;

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------

// config_load_input — Load configuration from ~/.config/copycatos/input.conf.
// Sets sensible defaults first, then overwrites with values from the file.
// Returns true if the file was found and parsed, false if using defaults only.
bool config_load_input(InputConfig *cfg);

// config_save_input — Write the current configuration back to input.conf.
// Creates the directory if it doesn't exist. Returns true on success.
bool config_save_input(const InputConfig *cfg);

// parse_key_name — Convert a string like "KEY_RETURN" or "BTN_SOUTH" to its
// corresponding Linux input event code. Returns -1 for unrecognized names.
int parse_key_name(const char *name);

#endif // CONFIG_H
