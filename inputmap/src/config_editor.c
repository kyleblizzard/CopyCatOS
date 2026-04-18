// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// config_editor.c — Config file editor implementation
// ============================================================================
//
// Reads and writes ~/.config/copycatos/input.conf in the same INI format
// that inputd expects. The parsing logic is intentionally compatible
// with inputd's config.c — both read the same file, so they must agree
// on the format.
//
// The editor maintains an in-memory copy of all settings. The GUI modifies
// this copy, then calls config_editor_save() to flush it to disk and
// config_editor_signal_daemon() to tell inputd to reload.
//
// ============================================================================

#include "config_editor.h"
#include "scanner.h"      // For scanner_code_to_name / scanner_name_to_code

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <linux/input-event-codes.h>

// ============================================================================
//  Internal state
// ============================================================================

struct ConfigEditor {
    // Mouse settings
    MouseSettings mouse;

    // Trigger threshold
    int trigger_threshold;

    // Power button settings
    int  short_press_ms;
    int  long_press_ms;
    char short_action[32];
    char long_action[32];

    // Desktop mappings
    MappingEntry mappings[MAX_EDITOR_MAPPINGS];
    int          mapping_count;
};

// ============================================================================
//  config_editor_new — Create editor with sensible defaults
// ============================================================================

ConfigEditor *config_editor_new(void) {
    ConfigEditor *ed = calloc(1, sizeof(ConfigEditor));
    if (!ed) return NULL;

    // Same defaults as inputd's config_load_input()
    ed->mouse.deadzone    = 4000;
    ed->mouse.sensitivity = 3.0;
    ed->mouse.exponent    = 2.0;
    ed->mouse.max_speed   = 20;

    ed->trigger_threshold = 128;

    ed->short_press_ms = 700;
    ed->long_press_ms  = 3000;
    strncpy(ed->short_action, "suspend", sizeof(ed->short_action));
    strncpy(ed->long_action,  "restart", sizeof(ed->long_action));

    ed->mapping_count = 0;

    return ed;
}

void config_editor_free(ConfigEditor *ed) {
    free(ed);
}

// ============================================================================
//  Helper: Strip trailing whitespace from a string
// ============================================================================

static void strip_trailing(char *s) {
    int len = (int)strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       s[len - 1] == ' '  || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
}

// ============================================================================
//  Helper: Parse a "type:param" action string into a MappingEntry
// ============================================================================
// Format: "key:KEY_ENTER", "mouse:BTN_LEFT", "copycatos:spotlight"
// ============================================================================

static bool parse_action(const char *str, MappingEntry *entry) {
    const char *colon = strchr(str, ':');
    if (!colon) return false;

    int prefix_len = (int)(colon - str);
    char prefix[32];
    if (prefix_len >= (int)sizeof(prefix)) prefix_len = (int)sizeof(prefix) - 1;
    strncpy(prefix, str, prefix_len);
    prefix[prefix_len] = '\0';

    const char *param = colon + 1;

    if (strcmp(prefix, "key") == 0) {
        entry->action = CEDITOR_ACTION_KEY;
        entry->param  = scanner_name_to_code(param);
        if (entry->param < 0) return false;
        snprintf(entry->target_name, sizeof(entry->target_name), "%s", param);
    } else if (strcmp(prefix, "mouse") == 0) {
        entry->action = CEDITOR_ACTION_MOUSE;
        entry->param  = scanner_name_to_code(param);
        if (entry->param < 0) return false;
        snprintf(entry->target_name, sizeof(entry->target_name), "%s", param);
    } else if (strcmp(prefix, "copycatos") == 0) {
        entry->action = CEDITOR_ACTION_COPYCATOS;
        entry->param  = 0;
        snprintf(entry->param_str, sizeof(entry->param_str), "%s", param);
        snprintf(entry->target_name, sizeof(entry->target_name),
                 "copycatos:%s", param);
    } else {
        return false;
    }

    return true;
}

// ============================================================================
//  config_editor_load — Parse ~/.config/copycatos/input.conf
// ============================================================================

bool config_editor_load(ConfigEditor *ed) {
    const char *home = getenv("HOME");
    if (!home) return false;

    char path[512];
    snprintf(path, sizeof(path), "%s/.config/copycatos/input.conf", home);

    FILE *fp = fopen(path, "r");
    if (!fp) return false;

    char line[512];
    char section[64] = "";

    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\n' || *p == '\r' || *p == '\0') continue;
        if (*p == '#'  || *p == ';') continue;

        strip_trailing(p);

        // Section header
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

        // [mouse] section
        if (strcmp(section, "mouse") == 0) {
            if (strncmp(p, "deadzone=", 9) == 0)
                ed->mouse.deadzone = atoi(p + 9);
            else if (strncmp(p, "sensitivity=", 12) == 0)
                ed->mouse.sensitivity = atof(p + 12);
            else if (strncmp(p, "exponent=", 9) == 0)
                ed->mouse.exponent = atof(p + 9);
            else if (strncmp(p, "max_speed=", 10) == 0)
                ed->mouse.max_speed = atoi(p + 10);
        }

        // [triggers] section
        else if (strcmp(section, "triggers") == 0) {
            if (strncmp(p, "threshold=", 10) == 0)
                ed->trigger_threshold = atoi(p + 10);
        }

        // [power] section
        else if (strcmp(section, "power") == 0) {
            if (strncmp(p, "short_press_ms=", 15) == 0)
                ed->short_press_ms = atoi(p + 15);
            else if (strncmp(p, "long_press_ms=", 14) == 0)
                ed->long_press_ms = atoi(p + 14);
            else if (strncmp(p, "short_press_action=", 19) == 0) {
                strncpy(ed->short_action, p + 19, sizeof(ed->short_action) - 1);
                ed->short_action[sizeof(ed->short_action) - 1] = '\0';
            }
            else if (strncmp(p, "long_press_action=", 18) == 0) {
                strncpy(ed->long_action, p + 18, sizeof(ed->long_action) - 1);
                ed->long_action[sizeof(ed->long_action) - 1] = '\0';
            }
        }

        // [desktop_mappings] section
        else if (strcmp(section, "desktop_mappings") == 0) {
            char *eq = strchr(p, '=');
            if (!eq) continue;
            if (ed->mapping_count >= MAX_EDITOR_MAPPINGS) continue;

            *eq = '\0';
            const char *event_name = p;
            const char *action_str = eq + 1;

            int code = scanner_name_to_code(event_name);
            if (code < 0) continue;

            MappingEntry *entry = &ed->mappings[ed->mapping_count];
            memset(entry, 0, sizeof(*entry));
            entry->event_code = code;
            snprintf(entry->source_name, sizeof(entry->source_name),
                     "%s", event_name);

            if (parse_action(action_str, entry)) {
                ed->mapping_count++;
            }
        }
    }

    fclose(fp);
    return true;
}

// ============================================================================
//  config_editor_save — Write config back to input.conf
// ============================================================================

bool config_editor_save(ConfigEditor *ed) {
    const char *home = getenv("HOME");
    if (!home) return false;

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config/copycatos", home);
    mkdir(dir, 0755);

    char path[512];
    snprintf(path, sizeof(path), "%s/.config/copycatos/input.conf", home);

    FILE *fp = fopen(path, "w");
    if (!fp) return false;

    fprintf(fp, "# CopyCatOS input daemon configuration\n");
    fprintf(fp, "# Edit this file or use the Button Mapper to change settings.\n");
    fprintf(fp, "# Changes take effect on SIGHUP or daemon restart.\n\n");

    // [mouse]
    fprintf(fp, "[mouse]\n");
    fprintf(fp, "deadzone=%d\n",      ed->mouse.deadzone);
    fprintf(fp, "sensitivity=%.1f\n", ed->mouse.sensitivity);
    fprintf(fp, "exponent=%.1f\n",    ed->mouse.exponent);
    fprintf(fp, "max_speed=%d\n",     ed->mouse.max_speed);
    fprintf(fp, "\n");

    // [triggers]
    fprintf(fp, "[triggers]\n");
    fprintf(fp, "threshold=%d\n", ed->trigger_threshold);
    fprintf(fp, "\n");

    // [power]
    fprintf(fp, "[power]\n");
    fprintf(fp, "short_press_ms=%d\n",     ed->short_press_ms);
    fprintf(fp, "long_press_ms=%d\n",      ed->long_press_ms);
    fprintf(fp, "short_press_action=%s\n", ed->short_action);
    fprintf(fp, "long_press_action=%s\n",  ed->long_action);
    fprintf(fp, "\n");

    // [desktop_mappings]
    if (ed->mapping_count > 0) {
        fprintf(fp, "[desktop_mappings]\n");
        for (int i = 0; i < ed->mapping_count; i++) {
            MappingEntry *e = &ed->mappings[i];

            // Use the human-readable source name if we have it,
            // otherwise look it up from the code
            const char *src = e->source_name[0] ? e->source_name :
                              scanner_code_to_name(EV_KEY, e->event_code);

            switch (e->action) {
            case CEDITOR_ACTION_KEY: {
                const char *tgt = e->target_name[0] ? e->target_name :
                                  scanner_code_to_name(EV_KEY, e->param);
                fprintf(fp, "%s=key:%s\n", src, tgt);
                break;
            }
            case CEDITOR_ACTION_MOUSE: {
                const char *tgt = e->target_name[0] ? e->target_name :
                                  scanner_code_to_name(EV_KEY, e->param);
                fprintf(fp, "%s=mouse:%s\n", src, tgt);
                break;
            }
            case CEDITOR_ACTION_COPYCATOS:
                fprintf(fp, "%s=copycatos:%s\n", src, e->param_str);
                break;
            case CEDITOR_ACTION_NONE:
                // Don't write disabled mappings
                break;
            }
        }
        fprintf(fp, "\n");
    }

    fclose(fp);
    return true;
}

// ============================================================================
//  Mapping accessors
// ============================================================================

int config_editor_get_mapping_count(const ConfigEditor *ed) {
    return ed->mapping_count;
}

bool config_editor_get_mapping(const ConfigEditor *ed, int index,
                               MappingEntry *out) {
    if (index < 0 || index >= ed->mapping_count) return false;
    *out = ed->mappings[index];
    return true;
}

bool config_editor_set_mapping(ConfigEditor *ed, int event_code,
                               const char *action_str) {
    // Look for an existing mapping with this event code
    for (int i = 0; i < ed->mapping_count; i++) {
        if (ed->mappings[i].event_code == event_code) {
            // Replace existing mapping
            MappingEntry *entry = &ed->mappings[i];
            entry->event_code = event_code;
            const char *src = scanner_code_to_name(EV_KEY, event_code);
            snprintf(entry->source_name, sizeof(entry->source_name), "%s", src);
            return parse_action(action_str, entry);
        }
    }

    // No existing mapping — add a new one
    if (ed->mapping_count >= MAX_EDITOR_MAPPINGS) return false;

    MappingEntry *entry = &ed->mappings[ed->mapping_count];
    memset(entry, 0, sizeof(*entry));
    entry->event_code = event_code;
    const char *src = scanner_code_to_name(EV_KEY, event_code);
    snprintf(entry->source_name, sizeof(entry->source_name), "%s", src);

    if (parse_action(action_str, entry)) {
        ed->mapping_count++;
        return true;
    }
    return false;
}

bool config_editor_remove_mapping(ConfigEditor *ed, int event_code) {
    for (int i = 0; i < ed->mapping_count; i++) {
        if (ed->mappings[i].event_code == event_code) {
            // Shift remaining entries down to fill the gap
            int remaining = ed->mapping_count - i - 1;
            if (remaining > 0) {
                memmove(&ed->mappings[i], &ed->mappings[i + 1],
                        remaining * sizeof(MappingEntry));
            }
            ed->mapping_count--;
            return true;
        }
    }
    return false;
}

// ============================================================================
//  Mouse settings accessors
// ============================================================================

void config_editor_get_mouse(const ConfigEditor *ed, MouseSettings *out) {
    *out = ed->mouse;
}

void config_editor_set_mouse(ConfigEditor *ed, const MouseSettings *settings) {
    ed->mouse = *settings;
}

// ============================================================================
//  Trigger threshold accessors
// ============================================================================

int config_editor_get_trigger_threshold(const ConfigEditor *ed) {
    return ed->trigger_threshold;
}

void config_editor_set_trigger_threshold(ConfigEditor *ed, int threshold) {
    ed->trigger_threshold = threshold;
}

// ============================================================================
//  config_editor_signal_daemon — Send SIGHUP to inputd
// ============================================================================
// Uses popen("pidof inputd") to find the daemon's PID, then sends
// SIGHUP to trigger a config reload. This avoids needing a PID file.
// ============================================================================

bool config_editor_signal_daemon(void) {
    // Find inputd's PID using pidof
    FILE *fp = popen("pidof inputd", "r");
    if (!fp) return false;

    char buf[64];
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        pclose(fp);
        fprintf(stderr, "config_editor: inputd not running\n");
        return false;
    }
    pclose(fp);

    // Parse the PID (pidof may return multiple PIDs, we take the first)
    int pid = atoi(buf);
    if (pid <= 0) return false;

    // Send SIGHUP — inputd's signal handler writes 'H' to its self-pipe,
    // which triggers config_load_input() on the next main loop iteration.
    if (kill(pid, SIGHUP) < 0) {
        fprintf(stderr, "config_editor: failed to signal PID %d\n", pid);
        return false;
    }

    fprintf(stderr, "config_editor: sent SIGHUP to inputd (PID %d)\n", pid);
    return true;
}
