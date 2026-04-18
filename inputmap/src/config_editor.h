// CopyCatOS — by Kyle Blizzard at Blizzard.show

//
// config_editor.h — Config file editor for inputd settings
//
// Provides a high-level API for reading and writing inputd's config file
// (~/.config/copycatos/input.conf). The GUI uses this to display and modify
// button mappings, mouse sensitivity, trigger thresholds, etc.
//
// After saving changes, call config_editor_signal_daemon() to send SIGHUP
// to inputd so it reloads the config without restarting.
//
// The config file uses a simple INI format — same format that inputd's
// config.c parser reads. See inputd/src/config.h for the full spec.
//

#ifndef CONFIG_EDITOR_H
#define CONFIG_EDITOR_H

#include <stdbool.h>

// --------------------------------------------------------------------------
// Action types for desktop mapping rules (matches inputd's CfgActionType)
// --------------------------------------------------------------------------
typedef enum {
    CEDITOR_ACTION_NONE = 0,      // No mapping — event is dropped
    CEDITOR_ACTION_KEY,           // Emit a keyboard keycode
    CEDITOR_ACTION_MOUSE,         // Emit a mouse button
    CEDITOR_ACTION_COPYCATOS      // Trigger a CopyCatOS action (Spotlight, etc.)
} CEditorActionType;

// --------------------------------------------------------------------------
// MappingEntry — One button-to-action mapping in the editor
// --------------------------------------------------------------------------
typedef struct {
    int               event_code;      // Source event code (e.g. BTN_SOUTH = 0x130)
    CEditorActionType action;          // What kind of output this produces
    int               param;           // Target code for KEY/MOUSE actions
    char              param_str[64];   // Action name for COPYCATOS actions
    char              source_name[32]; // Human-readable source (e.g. "BTN_SOUTH")
    char              target_name[64]; // Human-readable target (e.g. "KEY_ENTER")
} MappingEntry;

// Maximum mappings we support (matches inputd's MAX_CFG_MAPPING_RULES)
#define MAX_EDITOR_MAPPINGS 32

// --------------------------------------------------------------------------
// MouseSettings — Joystick-to-pointer tuning parameters
// --------------------------------------------------------------------------
typedef struct {
    int    deadzone;         // Stick values below this are ignored (0-32767)
    double sensitivity;      // Linear multiplier for axis values
    double exponent;         // Response curve (1.0=linear, 2.0=quadratic)
    int    max_speed;        // Maximum pointer speed in pixels per tick
} MouseSettings;

// --------------------------------------------------------------------------
// ConfigEditor — Opaque handle to the config editor state
// --------------------------------------------------------------------------
typedef struct ConfigEditor ConfigEditor;

// --------------------------------------------------------------------------
// Public API — Lifecycle
// --------------------------------------------------------------------------

// config_editor_new — Create a new editor instance.
// The editor starts with default values (same defaults as inputd).
ConfigEditor *config_editor_new(void);

// config_editor_free — Destroy the editor and free all memory.
void config_editor_free(ConfigEditor *ed);

// --------------------------------------------------------------------------
// Public API — Load / Save
// --------------------------------------------------------------------------

// config_editor_load — Read ~/.config/copycatos/input.conf into the editor.
// Returns true if the file was found and parsed, false if using defaults.
bool config_editor_load(ConfigEditor *ed);

// config_editor_save — Write the editor's state back to input.conf.
// Creates the config directory if it doesn't exist.
// Returns true on success, false on write error.
bool config_editor_save(ConfigEditor *ed);

// --------------------------------------------------------------------------
// Public API — Desktop Mappings
// --------------------------------------------------------------------------

// config_editor_get_mapping_count — How many desktop mappings are defined.
int config_editor_get_mapping_count(const ConfigEditor *ed);

// config_editor_get_mapping — Get mapping at `index`.
// Returns false if index is out of bounds.
bool config_editor_get_mapping(const ConfigEditor *ed, int index,
                               MappingEntry *out);

// config_editor_set_mapping — Set or add a mapping for `event_code`.
// `action_str` uses the config file format: "key:KEY_ENTER", "mouse:BTN_LEFT",
// or "copycatos:spotlight". If a mapping for this event_code already exists,
// it's replaced. Returns true on success.
bool config_editor_set_mapping(ConfigEditor *ed, int event_code,
                               const char *action_str);

// config_editor_remove_mapping — Remove the mapping for `event_code`.
// Returns true if found and removed, false if not found.
bool config_editor_remove_mapping(ConfigEditor *ed, int event_code);

// --------------------------------------------------------------------------
// Public API — Mouse Settings
// --------------------------------------------------------------------------

void config_editor_get_mouse(const ConfigEditor *ed, MouseSettings *out);
void config_editor_set_mouse(ConfigEditor *ed, const MouseSettings *settings);

// --------------------------------------------------------------------------
// Public API — Trigger Threshold
// --------------------------------------------------------------------------

int  config_editor_get_trigger_threshold(const ConfigEditor *ed);
void config_editor_set_trigger_threshold(ConfigEditor *ed, int threshold);

// --------------------------------------------------------------------------
// Public API — Daemon Communication
// --------------------------------------------------------------------------

// config_editor_signal_daemon — Send SIGHUP to inputd to trigger a
// config reload. Finds the daemon's PID via pidof. Returns true if the
// signal was sent, false if inputd isn't running.
bool config_editor_signal_daemon(void);

#endif // CONFIG_EDITOR_H
