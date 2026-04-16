// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// config.c — INI config parser for cc-inputd
// ============================================================================
//
// Reads and writes ~/.config/copicatos/input.conf using a simple INI format.
// Follows the same line-by-line parsing pattern used in cc-sysprefs/panes/dock.c
// for desktop.conf — track the current [section] name, then match "key=value"
// pairs within each section.
//
// The config file controls:
//   [mouse]             — joystick-to-pointer behavior (deadzone, speed, curve)
//   [triggers]          — analog trigger activation threshold
//   [power]             — power button press timing and actions
//   [desktop_mappings]  — gamepad button to desktop action mappings
//   [game_overrides]    — per-application game mode overrides
//
// If the config file doesn't exist, sensible defaults are used and the daemon
// runs fine out of the box.
// ============================================================================

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <linux/input-event-codes.h>

// ============================================================================
//  Key name lookup table
// ============================================================================
//
// Maps human-readable Linux input code names (like "KEY_RETURN") to their
// numeric values from <linux/input-event-codes.h>. This lets the config file
// use readable names instead of raw numbers.
//
// The table is sorted alphabetically for readability, not for performance.
// With ~60 entries, a linear scan is perfectly fast.
// ============================================================================

typedef struct {
    const char *name;     // String name as it appears in the config file
    int         code;     // Corresponding Linux input event code
} KeyNameEntry;

// Static lookup table — add entries here as needed for new mappings.
// Covers common keyboard keys, gamepad buttons, mouse buttons, and axes.
static const KeyNameEntry key_name_table[] = {
    // --- Absolute axes (analog sticks, triggers) ---
    { "ABS_HAT0X",     ABS_HAT0X },
    { "ABS_HAT0Y",     ABS_HAT0Y },
    { "ABS_RX",        ABS_RX },
    { "ABS_RY",        ABS_RY },
    { "ABS_X",         ABS_X },
    { "ABS_Y",         ABS_Y },
    { "ABS_Z",         ABS_Z },
    { "ABS_RZ",        ABS_RZ },

    // --- Gamepad buttons (Xbox layout names) ---
    { "BTN_EAST",      BTN_EAST },
    { "BTN_MODE",      BTN_MODE },
    { "BTN_NORTH",     BTN_NORTH },
    { "BTN_SELECT",    BTN_SELECT },
    { "BTN_SOUTH",     BTN_SOUTH },
    { "BTN_START",     BTN_START },
    { "BTN_THUMBL",    BTN_THUMBL },
    { "BTN_THUMBR",    BTN_THUMBR },
    { "BTN_TL",        BTN_TL },
    { "BTN_TL2",       BTN_TL2 },
    { "BTN_TR",        BTN_TR },
    { "BTN_TR2",       BTN_TR2 },
    { "BTN_WEST",      BTN_WEST },

    // --- Mouse buttons ---
    { "BTN_LEFT",      BTN_LEFT },
    { "BTN_MIDDLE",    BTN_MIDDLE },
    { "BTN_RIGHT",     BTN_RIGHT },
    { "BTN_SIDE",      BTN_SIDE },
    { "BTN_EXTRA",     BTN_EXTRA },

    // --- Common keyboard keys ---
    { "KEY_A",         KEY_A },
    { "KEY_B",         KEY_B },
    { "KEY_BACKSPACE", KEY_BACKSPACE },
    { "KEY_C",         KEY_C },
    { "KEY_D",         KEY_D },
    { "KEY_DELETE",    KEY_DELETE },
    { "KEY_DOWN",      KEY_DOWN },
    { "KEY_END",       KEY_END },
    { "KEY_ENTER",     KEY_ENTER },
    { "KEY_ESC",       KEY_ESC },
    { "KEY_F1",        KEY_F1 },
    { "KEY_F2",        KEY_F2 },
    { "KEY_F3",        KEY_F3 },
    { "KEY_F4",        KEY_F4 },
    { "KEY_F5",        KEY_F5 },
    { "KEY_F10",       KEY_F10 },
    { "KEY_F11",       KEY_F11 },
    { "KEY_F12",       KEY_F12 },
    { "KEY_HOME",      KEY_HOME },
    { "KEY_LEFT",      KEY_LEFT },
    { "KEY_LEFTALT",   KEY_LEFTALT },
    { "KEY_LEFTCTRL",  KEY_LEFTCTRL },
    { "KEY_LEFTMETA",  KEY_LEFTMETA },
    { "KEY_LEFTSHIFT", KEY_LEFTSHIFT },
    { "KEY_PAGEDOWN",  KEY_PAGEDOWN },
    { "KEY_PAGEUP",    KEY_PAGEUP },
    { "KEY_RETURN",    KEY_ENTER },    // Alias — many people write "RETURN"
    { "KEY_RIGHT",     KEY_RIGHT },
    { "KEY_SPACE",     KEY_SPACE },
    { "KEY_TAB",       KEY_TAB },
    { "KEY_UP",        KEY_UP },
    { "KEY_V",         KEY_V },
    { "KEY_W",         KEY_W },
    { "KEY_X",         KEY_X },
    { "KEY_Z",         KEY_Z },

    // Sentinel — marks the end of the table
    { NULL, -1 }
};

// ============================================================================
//  parse_key_name — Convert a string name to a Linux input code
// ============================================================================
// Walks the lookup table and does a case-sensitive strcmp on each entry.
// Returns -1 if the name isn't recognized.

int parse_key_name(const char *name)
{
    if (!name || !*name) return -1;

    for (int i = 0; key_name_table[i].name != NULL; i++) {
        if (strcmp(name, key_name_table[i].name) == 0) {
            return key_name_table[i].code;
        }
    }

    // If the name isn't in our table, try parsing it as a raw integer.
    // This allows advanced users to use numeric codes directly.
    char *endptr = NULL;
    long val = strtol(name, &endptr, 0);
    if (endptr && *endptr == '\0' && val >= 0) {
        return (int)val;
    }

    return -1;
}

// ============================================================================
//  parse_action_string — Parse "type:param" action from desktop_mappings
// ============================================================================
// The config file format for mappings is:
//   BTN_SOUTH=key:KEY_RETURN
//   BTN_EAST=mouse:BTN_RIGHT
//   BTN_MODE=copicatos:spotlight
//
// This function splits the "type:param" string and fills in the rule fields.
// Returns true if parsing succeeded, false on error.
// ============================================================================

static bool parse_action_string(const char *str, CfgMappingRule *rule)
{
    // Find the colon separator between action type and parameter
    const char *colon = strchr(str, ':');
    if (!colon) {
        fprintf(stderr, "config: invalid action format (missing ':'): %s\n", str);
        return false;
    }

    // Extract the action type prefix (everything before the colon)
    int prefix_len = (int)(colon - str);
    char prefix[32];
    if (prefix_len >= (int)sizeof(prefix)) prefix_len = (int)sizeof(prefix) - 1;
    strncpy(prefix, str, prefix_len);
    prefix[prefix_len] = '\0';

    // The parameter is everything after the colon
    const char *param = colon + 1;

    if (strcmp(prefix, "key") == 0) {
        // Action type: emit a keyboard keycode
        rule->action = CFG_ACTION_KEY;
        rule->param  = parse_key_name(param);
        if (rule->param < 0) {
            fprintf(stderr, "config: unknown key name: %s\n", param);
            return false;
        }
    } else if (strcmp(prefix, "mouse") == 0) {
        // Action type: emit a mouse button
        rule->action = CFG_ACTION_MOUSE;
        rule->param  = parse_key_name(param);
        if (rule->param < 0) {
            fprintf(stderr, "config: unknown button name: %s\n", param);
            return false;
        }
    } else if (strcmp(prefix, "copicatos") == 0) {
        // Action type: trigger a CopiCatOS shell action (sent via IPC)
        rule->action = CFG_ACTION_COPICATOS;
        rule->param  = 0;
        // Store the action name string (e.g. "spotlight", "mission_control")
        strncpy(rule->param_str, param, sizeof(rule->param_str) - 1);
        rule->param_str[sizeof(rule->param_str) - 1] = '\0';
    } else {
        fprintf(stderr, "config: unknown action type: %s\n", prefix);
        return false;
    }

    return true;
}

// ============================================================================
//  Strip trailing whitespace/newline from a string in-place
// ============================================================================

static void strip_trailing(char *s)
{
    int len = (int)strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       s[len - 1] == ' '  || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
}

// ============================================================================
//  config_load_input — Load config from ~/.config/copicatos/input.conf
// ============================================================================
// Sets sensible defaults first, then reads the config file line by line.
// Each line is either:
//   - A section header: [mouse], [triggers], [power], etc.
//   - A key=value pair within the current section
//   - A comment (starting with # or ;) or blank line (ignored)
//
// Returns true if the file was found and parsed. Returns false if the file
// doesn't exist (defaults are still applied and the daemon works fine).
// ============================================================================

bool config_load_input(InputConfig *cfg)
{
    // ------------------------------------------------------------------
    // Step 1: Set sensible defaults
    // ------------------------------------------------------------------
    // These values work well on the Legion Go out of the box.
    // Users can tweak them later via System Preferences or editing the file.
    // ------------------------------------------------------------------

    cfg->deadzone         = 4000;
    cfg->sensitivity      = 3.0;
    cfg->exponent         = 2.0;
    cfg->max_speed        = 20;
    cfg->trigger_threshold = 128;

    cfg->short_press_ms   = 700;
    cfg->long_press_ms    = 3000;
    strncpy(cfg->short_action, "suspend", sizeof(cfg->short_action));
    strncpy(cfg->long_action,  "restart", sizeof(cfg->long_action));

    cfg->desktop_rule_count  = 0;
    cfg->game_override_count = 0;

    // ------------------------------------------------------------------
    // Step 2: Build the config file path
    // ------------------------------------------------------------------
    // The config lives at ~/.config/copicatos/input.conf, following the
    // XDG Base Directory Specification (sort of — we use .config directly
    // rather than $XDG_CONFIG_HOME for simplicity).
    // ------------------------------------------------------------------

    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "config: $HOME not set, using defaults\n");
        return false;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/.config/copicatos/input.conf", home);

    // ------------------------------------------------------------------
    // Step 3: Open the file (it's OK if it doesn't exist)
    // ------------------------------------------------------------------

    FILE *fp = fopen(path, "r");
    if (!fp) {
        // Not an error — first boot or user hasn't customized anything.
        // The defaults we set above are perfectly usable.
        return false;
    }

    // ------------------------------------------------------------------
    // Step 4: Parse line by line, tracking the current section
    // ------------------------------------------------------------------
    // This follows the exact same pattern as cc-sysprefs/panes/dock.c:
    //   - If a line starts with '[', extract the section name
    //   - Otherwise, match "key=value" pairs based on the current section
    // ------------------------------------------------------------------

    char line[512];
    char section[64] = "";   // Current section name (e.g. "mouse", "power")

    while (fgets(line, sizeof(line), fp)) {
        // Skip leading whitespace
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        // Skip empty lines and comments
        if (*p == '\n' || *p == '\r' || *p == '\0') continue;
        if (*p == '#'  || *p == ';') continue;

        // Strip trailing whitespace/newline for cleaner value parsing
        strip_trailing(p);

        // Check for section header: [section_name]
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end) {
                int len = (int)(end - p - 1);
                if (len > 0 && len < (int)sizeof(section)) {
                    strncpy(section, p + 1, len);
                    section[len] = '\0';
                }
            }
            continue;
        }

        // ── [mouse] section ───────────────────────────────────────────
        // Controls how the right analog stick moves the mouse pointer.
        if (strcmp(section, "mouse") == 0) {
            if (strncmp(p, "deadzone=", 9) == 0) {
                cfg->deadzone = atoi(p + 9);
                // Clamp to valid range (0 = no deadzone, 32767 = max)
                if (cfg->deadzone < 0)     cfg->deadzone = 0;
                if (cfg->deadzone > 32767) cfg->deadzone = 32767;
            }
            else if (strncmp(p, "sensitivity=", 12) == 0) {
                cfg->sensitivity = atof(p + 12);
                if (cfg->sensitivity < 0.1) cfg->sensitivity = 0.1;
                if (cfg->sensitivity > 20.0) cfg->sensitivity = 20.0;
            }
            else if (strncmp(p, "exponent=", 9) == 0) {
                cfg->exponent = atof(p + 9);
                if (cfg->exponent < 1.0) cfg->exponent = 1.0;
                if (cfg->exponent > 5.0) cfg->exponent = 5.0;
            }
            else if (strncmp(p, "max_speed=", 10) == 0) {
                cfg->max_speed = atoi(p + 10);
                if (cfg->max_speed < 1)   cfg->max_speed = 1;
                if (cfg->max_speed > 100) cfg->max_speed = 100;
            }
        }

        // ── [triggers] section ────────────────────────────────────────
        // Sets how far you need to pull the analog trigger before it
        // registers as "pressed."
        else if (strcmp(section, "triggers") == 0) {
            if (strncmp(p, "threshold=", 10) == 0) {
                cfg->trigger_threshold = atoi(p + 10);
                if (cfg->trigger_threshold < 1)   cfg->trigger_threshold = 1;
                if (cfg->trigger_threshold > 255) cfg->trigger_threshold = 255;
            }
        }

        // ── [power] section ───────────────────────────────────────────
        // Controls what happens when you press the physical power button.
        else if (strcmp(section, "power") == 0) {
            if (strncmp(p, "short_press_ms=", 15) == 0) {
                cfg->short_press_ms = atoi(p + 15);
                if (cfg->short_press_ms < 100)  cfg->short_press_ms = 100;
                if (cfg->short_press_ms > 5000) cfg->short_press_ms = 5000;
            }
            else if (strncmp(p, "long_press_ms=", 14) == 0) {
                cfg->long_press_ms = atoi(p + 14);
                if (cfg->long_press_ms < 500)   cfg->long_press_ms = 500;
                if (cfg->long_press_ms > 10000) cfg->long_press_ms = 10000;
            }
            else if (strncmp(p, "short_press_action=", 19) == 0) {
                strncpy(cfg->short_action, p + 19, sizeof(cfg->short_action) - 1);
                cfg->short_action[sizeof(cfg->short_action) - 1] = '\0';
            }
            else if (strncmp(p, "long_press_action=", 18) == 0) {
                strncpy(cfg->long_action, p + 18, sizeof(cfg->long_action) - 1);
                cfg->long_action[sizeof(cfg->long_action) - 1] = '\0';
            }
        }

        // ── [desktop_mappings] section ────────────────────────────────
        // Each line maps a gamepad event code to a desktop action.
        // Format: EVENT_CODE=action_type:param
        // Example: BTN_SOUTH=key:KEY_RETURN
        else if (strcmp(section, "desktop_mappings") == 0) {
            // Find the '=' separator
            char *eq = strchr(p, '=');
            if (!eq) continue;

            // Split into event name and action string
            *eq = '\0';
            const char *event_name  = p;
            const char *action_str  = eq + 1;

            // Make sure we haven't exceeded the rule array
            if (cfg->desktop_rule_count >= MAX_CFG_MAPPING_RULES) {
                fprintf(stderr, "config: too many desktop mappings (max %d)\n",
                        MAX_CFG_MAPPING_RULES);
                continue;
            }

            // Parse the event code name (e.g. "BTN_SOUTH" -> 0x130)
            int code = parse_key_name(event_name);
            if (code < 0) {
                fprintf(stderr, "config: unknown event code: %s\n", event_name);
                continue;
            }

            // Parse the action string (e.g. "key:KEY_RETURN")
            CfgMappingRule *rule = &cfg->desktop_rules[cfg->desktop_rule_count];
            rule->event_code = code;
            rule->param      = 0;
            rule->param_str[0] = '\0';

            if (parse_action_string(action_str, rule)) {
                cfg->desktop_rule_count++;
            }
        }

        // ── [game_overrides] section ──────────────────────────────────
        // Each line maps a window class pattern to a profile name.
        // Format: pattern=profile_name
        // Example: steam_app_*=gamepad
        else if (strcmp(section, "game_overrides") == 0) {
            char *eq = strchr(p, '=');
            if (!eq) continue;

            if (cfg->game_override_count >= MAX_GAME_OVERRIDES) {
                fprintf(stderr, "config: too many game overrides (max %d)\n",
                        MAX_GAME_OVERRIDES);
                continue;
            }

            *eq = '\0';
            GameOverride *ov = &cfg->game_overrides[cfg->game_override_count];

            strncpy(ov->pattern, p, sizeof(ov->pattern) - 1);
            ov->pattern[sizeof(ov->pattern) - 1] = '\0';

            strncpy(ov->profile, eq + 1, sizeof(ov->profile) - 1);
            ov->profile[sizeof(ov->profile) - 1] = '\0';

            cfg->game_override_count++;
        }
    }

    fclose(fp);
    return true;
}

// ============================================================================
//  action_type_to_string — Convert a CfgActionType to its config prefix
// ============================================================================

static const char *action_type_to_string(CfgActionType type)
{
    switch (type) {
        case CFG_ACTION_KEY:       return "key";
        case CFG_ACTION_MOUSE:     return "mouse";
        case CFG_ACTION_COPICATOS: return "copicatos";
        default:                   return "none";
    }
}

// ============================================================================
//  code_to_name — Reverse lookup: find the string name for a numeric code
// ============================================================================
// Walks the lookup table and returns the first matching name.
// If no match is found, returns NULL (caller should format as a number).
// ============================================================================

static const char *code_to_name(int code)
{
    for (int i = 0; key_name_table[i].name != NULL; i++) {
        if (key_name_table[i].code == code) {
            return key_name_table[i].name;
        }
    }
    return NULL;
}

// ============================================================================
//  config_save_input — Write config back to ~/.config/copicatos/input.conf
// ============================================================================
// Creates the config directory if it doesn't exist, then writes all sections
// in a clean INI format. This is called by System Preferences when the user
// changes input settings.
// ============================================================================

bool config_save_input(const InputConfig *cfg)
{
    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "config: $HOME not set, cannot save\n");
        return false;
    }

    // Ensure the config directory exists (mkdir -p equivalent)
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config/copicatos", home);
    mkdir(dir, 0755);

    // Build the full file path
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/copicatos/input.conf", home);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "config: failed to open %s for writing\n", path);
        return false;
    }

    // ── Header comment ────────────────────────────────────────────────
    fprintf(fp, "# CopiCatOS input daemon configuration\n");
    fprintf(fp, "# Edit this file or use System Preferences to change settings.\n");
    fprintf(fp, "# Changes take effect on SIGHUP or daemon restart.\n\n");

    // ── [mouse] section ───────────────────────────────────────────────
    fprintf(fp, "[mouse]\n");
    fprintf(fp, "deadzone=%d\n",      cfg->deadzone);
    fprintf(fp, "sensitivity=%.1f\n", cfg->sensitivity);
    fprintf(fp, "exponent=%.1f\n",    cfg->exponent);
    fprintf(fp, "max_speed=%d\n",     cfg->max_speed);
    fprintf(fp, "\n");

    // ── [triggers] section ────────────────────────────────────────────
    fprintf(fp, "[triggers]\n");
    fprintf(fp, "threshold=%d\n", cfg->trigger_threshold);
    fprintf(fp, "\n");

    // ── [power] section ───────────────────────────────────────────────
    fprintf(fp, "[power]\n");
    fprintf(fp, "short_press_ms=%d\n",     cfg->short_press_ms);
    fprintf(fp, "long_press_ms=%d\n",      cfg->long_press_ms);
    fprintf(fp, "short_press_action=%s\n", cfg->short_action);
    fprintf(fp, "long_press_action=%s\n",  cfg->long_action);
    fprintf(fp, "\n");

    // ── [desktop_mappings] section ────────────────────────────────────
    if (cfg->desktop_rule_count > 0) {
        fprintf(fp, "[desktop_mappings]\n");
        for (int i = 0; i < cfg->desktop_rule_count; i++) {
            const CfgMappingRule *rule = &cfg->desktop_rules[i];

            // Look up readable name for the event code
            const char *code_name = code_to_name(rule->event_code);
            char code_buf[16];
            if (!code_name) {
                // Fall back to numeric representation
                snprintf(code_buf, sizeof(code_buf), "%d", rule->event_code);
                code_name = code_buf;
            }

            // Format depends on action type
            const char *type_str = action_type_to_string(rule->action);
            if (rule->action == CFG_ACTION_COPICATOS) {
                // CopiCatOS actions use the string parameter
                fprintf(fp, "%s=%s:%s\n", code_name, type_str, rule->param_str);
            } else {
                // Key and mouse actions use the numeric parameter
                const char *param_name = code_to_name(rule->param);
                char param_buf[16];
                if (!param_name) {
                    snprintf(param_buf, sizeof(param_buf), "%d", rule->param);
                    param_name = param_buf;
                }
                fprintf(fp, "%s=%s:%s\n", code_name, type_str, param_name);
            }
        }
        fprintf(fp, "\n");
    }

    // ── [game_overrides] section ──────────────────────────────────────
    if (cfg->game_override_count > 0) {
        fprintf(fp, "[game_overrides]\n");
        for (int i = 0; i < cfg->game_override_count; i++) {
            const GameOverride *ov = &cfg->game_overrides[i];
            fprintf(fp, "%s=%s\n", ov->pattern, ov->profile);
        }
        fprintf(fp, "\n");
    }

    fclose(fp);
    return true;
}
