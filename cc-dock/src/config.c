// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// config.c — Dock configuration persistence (load/save/defaults)
//
// This file implements the dock's configuration system. The dock stores its
// item list in a simple text file so that changes persist across sessions.
//
// File format (pipe-delimited, one item per line):
//   type|name|exec_path|icon_name|process_name|separator_after
//
// Example:
//   app|Finder|cc-finder|org.kde.dolphin|cc-finder|0
//   app|Brave|brave-browser|com.brave.Browser|brave|0
//   folder|Downloads|~/Downloads|folder|downloads|0
//
// The "type" field is either "app" (a launchable application) or "folder"
// (a directory shown as a stack). The separator_after field is "1" or "0".
// ============================================================================

#define _GNU_SOURCE  // For M_PI, POSIX extensions

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#include <cairo/cairo.h>

// ---------------------------------------------------------------------------
// The config file lives at ~/.config/cc-dock/dock.conf
// We build this path at runtime using the HOME environment variable.
// ---------------------------------------------------------------------------
#define CONFIG_DIR_NAME   "cc-dock"
#define CONFIG_FILE_NAME  "dock.conf"

// Maximum length for a single line in the config file.
// Each line holds: type + name + exec_path + icon_name + process_name + sep
// With the longest fields being paths (512 chars), 2048 is more than enough.
#define MAX_LINE_LENGTH 2048

// ---------------------------------------------------------------------------
// Forward declarations for helper functions defined later in this file.
// ---------------------------------------------------------------------------
static bool get_config_dir(char *out, size_t out_size);
static bool get_config_path(char *out, size_t out_size);
static bool ensure_config_dir_exists(void);

// ---------------------------------------------------------------------------
// Icon resolution — find an icon file on disk
//
// This is the same search logic that was originally in dock.c. It checks
// multiple icon theme directories in order of preference:
// 1. AquaKDE custom theme (the project's own icon set)
// 2. hicolor theme (the freedesktop standard fallback)
// 3. Other system themes (Breeze, Oxygen, Adwaita)
// 4. pixmaps directory (legacy location)
// 5. Lowercase variants of the icon name
// 6. .desktop file Icon= field (for apps with non-obvious icon names)
//
// We prefer larger sizes (128, 64, 48) because we load once and scale down.
// This gives crisp rendering even at magnified sizes.
//
// Parameters:
//   icon_name — the theme icon name (e.g., "org.kde.dolphin")
//   out_path  — buffer to write the resolved file path into
//   out_size  — size of the out_path buffer
//
// Returns true if an icon file was found, false otherwise.
// ---------------------------------------------------------------------------
static bool resolve_icon_path(const char *icon_name, char *out_path,
                              size_t out_size)
{
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    // Sizes to try, from largest to smallest — bigger = crisper when scaled
    static const int sizes[] = {128, 64, 48};
    static const int size_count = 3;

    char path[1024];

    // Search 0: Dock-specific asset directory (~/.local/share/aqua-widgets/dock/)
    // Some icons like Trash have custom dock assets (trashempty.png, trashfull.png)
    // that should take priority over theme icons.
    snprintf(path, sizeof(path),
             "%s/.local/share/aqua-widgets/dock/%s.png", home, icon_name);
    if (access(path, R_OK) == 0) {
        snprintf(out_path, out_size, "%s", path);
        return true;
    }

    // Search 1: AquaKDE custom icon theme (project-specific icons)
    for (int i = 0; i < size_count; i++) {
        snprintf(path, sizeof(path),
                 "%s/.local/share/icons/AquaKDE-icons/%dx%d/apps/%s.png",
                 home, sizes[i], sizes[i], icon_name);
        if (access(path, R_OK) == 0) {
            snprintf(out_path, out_size, "%s", path);
            return true;
        }
    }

    // Search 2: hicolor theme (standard freedesktop location)
    for (int i = 0; i < size_count; i++) {
        snprintf(path, sizeof(path),
                 "/usr/share/icons/hicolor/%dx%d/apps/%s.png",
                 sizes[i], sizes[i], icon_name);
        if (access(path, R_OK) == 0) {
            snprintf(out_path, out_size, "%s", path);
            return true;
        }
    }

    // Search 3: other system icon themes (Breeze, Oxygen, Adwaita, etc.)
    static const char *system_themes[] = {
        "breeze", "breeze-dark", "oxygen/base", "Adwaita",
        "HighContrast", NULL
    };
    for (const char **theme = system_themes; *theme; theme++) {
        for (int i = 0; i < size_count; i++) {
            snprintf(path, sizeof(path),
                     "/usr/share/icons/%s/%dx%d/apps/%s.png",
                     *theme, sizes[i], sizes[i], icon_name);
            if (access(path, R_OK) == 0) {
                snprintf(out_path, out_size, "%s", path);
                return true;
            }
        }
    }

    // Search 4: hicolor 256x256 (some apps only ship large icons)
    snprintf(path, sizeof(path),
             "/usr/share/icons/hicolor/256x256/apps/%s.png", icon_name);
    if (access(path, R_OK) == 0) {
        snprintf(out_path, out_size, "%s", path);
        return true;
    }

    // Search 5: Breeze theme uses a non-standard directory layout
    // (apps/SIZE/ instead of SIZExSIZE/apps/)
    static const char *breeze_sizes[] = {"48", "64", "32", "22", NULL};
    for (const char **sz = breeze_sizes; *sz; sz++) {
        snprintf(path, sizeof(path),
                 "/usr/share/icons/breeze/apps/%s/%s.png", *sz, icon_name);
        if (access(path, R_OK) == 0) {
            snprintf(out_path, out_size, "%s", path);
            return true;
        }
    }

    // Search 6: pixmaps directory (legacy fallback for older apps)
    snprintf(path, sizeof(path), "/usr/share/pixmaps/%s.png", icon_name);
    if (access(path, R_OK) == 0) {
        snprintf(out_path, out_size, "%s", path);
        return true;
    }

    // Search 7: try lowercase version of the icon name
    // Some icon themes use all-lowercase filenames even when the app name
    // has mixed case (e.g., "GIMP" -> "gimp")
    char lower_name[256];
    strncpy(lower_name, icon_name, sizeof(lower_name) - 1);
    lower_name[sizeof(lower_name) - 1] = '\0';
    for (char *p = lower_name; *p; p++) *p = tolower((unsigned char)*p);

    if (strcmp(lower_name, icon_name) != 0) {
        // Only retry if the name actually changed (avoid redundant lookups)
        for (int i = 0; i < size_count; i++) {
            snprintf(path, sizeof(path),
                     "/usr/share/icons/hicolor/%dx%d/apps/%s.png",
                     sizes[i], sizes[i], lower_name);
            if (access(path, R_OK) == 0) {
                snprintf(out_path, out_size, "%s", path);
                return true;
            }
        }
        snprintf(path, sizeof(path),
                 "/usr/share/icons/hicolor/256x256/apps/%s.png", lower_name);
        if (access(path, R_OK) == 0) {
            snprintf(out_path, out_size, "%s", path);
            return true;
        }
        snprintf(path, sizeof(path), "/usr/share/pixmaps/%s.png", lower_name);
        if (access(path, R_OK) == 0) {
            snprintf(out_path, out_size, "%s", path);
            return true;
        }
    }

    // Search 8: look up the app's .desktop file to find the real icon name.
    // Some apps use an icon name that differs from their executable name.
    // The .desktop file's Icon= field tells us the actual icon name.
    char desktop_path[512];
    snprintf(desktop_path, sizeof(desktop_path),
             "/usr/share/applications/%s.desktop", icon_name);
    FILE *df = fopen(desktop_path, "r");
    if (!df) {
        // Also try with "org.kde." prefix — many KDE apps use this naming
        snprintf(desktop_path, sizeof(desktop_path),
                 "/usr/share/applications/org.kde.%s.desktop", icon_name);
        df = fopen(desktop_path, "r");
    }
    if (df) {
        char line[512];
        while (fgets(line, sizeof(line), df)) {
            if (strncmp(line, "Icon=", 5) == 0) {
                char *icon = line + 5;
                // Strip trailing newline/whitespace
                char *nl = strchr(icon, '\n');
                if (nl) *nl = '\0';
                nl = strchr(icon, '\r');
                if (nl) *nl = '\0';
                fclose(df);
                // Only recurse if the .desktop icon name is different from
                // what we already searched — prevents infinite recursion
                if (strlen(icon) > 0 && strcmp(icon, icon_name) != 0) {
                    return resolve_icon_path(icon, out_path, out_size);
                }
                return false;
            }
        }
        fclose(df);
    }

    return false;
}

// ---------------------------------------------------------------------------
// Create a fallback icon: a rounded rectangle with a gradient and the first
// letter of the app name drawn in white. Used when we can't find the real
// icon file on disk.
// ---------------------------------------------------------------------------
static cairo_surface_t *create_fallback_icon(const char *name)
{
    int size = 128;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                        size, size);
    cairo_t *cr = cairo_create(surf);

    // Draw a rounded rectangle with a blue-to-purple gradient
    double r = 24;  // Corner radius
    double x = 4, y = 4, w = size - 8, h = size - 8;

    // Build the rounded rectangle path using four arcs at the corners
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);       // top-right
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);    // bottom-right
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);     // bottom-left
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);     // top-left
    cairo_close_path(cr);

    // Fill with a vertical gradient from blue to purple
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, size);
    cairo_pattern_add_color_stop_rgb(grad, 0.0, 0.3, 0.5, 0.9);
    cairo_pattern_add_color_stop_rgb(grad, 1.0, 0.2, 0.2, 0.6);
    cairo_set_source(cr, grad);
    cairo_fill(cr);
    cairo_pattern_destroy(grad);

    // Draw the first letter of the app name in the center, large and white
    cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 64);

    // Measure the letter so we can center it properly
    char letter[2] = {name[0], '\0'};
    cairo_text_extents_t ext;
    cairo_text_extents(cr, letter, &ext);
    cairo_move_to(cr, (size - ext.width) / 2 - ext.x_bearing,
                      (size - ext.height) / 2 - ext.y_bearing);
    cairo_show_text(cr, letter);

    cairo_destroy(cr);
    cairo_surface_flush(surf);
    return surf;
}

// ===========================================================================
// Public API
// ===========================================================================

// ---------------------------------------------------------------------------
// config_resolve_and_load_icon — Resolve an icon name and load it into a
// DockItem's icon surface.
//
// This is the shared helper that both config_load() and config_set_defaults()
// call. It takes an icon_name (the freedesktop theme name, like
// "org.kde.dolphin"), searches for the actual PNG file on disk, and loads
// it as a Cairo surface into item->icon.
//
// If no matching icon file is found, it creates a generated fallback icon
// (a colored square with the first letter of the app name).
//
// It also stores the resolved filesystem path in item->icon_path and saves
// the original theme name in item->icon_name for later serialization.
//
// Returns true if a real icon was loaded, false if using the fallback.
// ---------------------------------------------------------------------------
bool config_resolve_and_load_icon(DockItem *item, const char *icon_name)
{
    // Save the icon theme name so we can write it back to the config file
    strncpy(item->icon_name, icon_name, sizeof(item->icon_name) - 1);
    item->icon_name[sizeof(item->icon_name) - 1] = '\0';

    // Try to find the actual PNG file on disk
    if (resolve_icon_path(icon_name, item->icon_path,
                          sizeof(item->icon_path))) {
        // Found it — load the PNG as a Cairo image surface
        item->icon = cairo_image_surface_create_from_png(item->icon_path);

        // Verify the surface loaded correctly (file might be corrupted)
        if (cairo_surface_status(item->icon) != CAIRO_STATUS_SUCCESS) {
            fprintf(stderr, "Warning: Failed to load icon %s\n",
                    item->icon_path);
            cairo_surface_destroy(item->icon);
            item->icon = create_fallback_icon(item->name);
            return false;
        }
        return true;
    }

    // No icon file found anywhere — generate a placeholder
    fprintf(stderr, "Warning: Icon not found for '%s', using fallback\n",
            item->name);
    item->icon = create_fallback_icon(item->name);
    return false;
}

// ---------------------------------------------------------------------------
// config_load — Read dock configuration from ~/.config/cc-dock/dock.conf
//
// The file format is one dock item per line, with fields separated by pipes:
//   type|name|exec_path|icon_name|process_name|separator_after
//
// For example:
//   app|Brave|brave-browser|com.brave.Browser|brave|0
//   folder|Downloads|~/Downloads|folder|downloads|0
//
// Blank lines and lines starting with '#' are ignored (comments).
//
// Returns true if items were loaded, false if the file doesn't exist.
// ---------------------------------------------------------------------------
bool config_load(DockState *state)
{
    char config_path[1024];
    if (!get_config_path(config_path, sizeof(config_path))) {
        return false;
    }

    // Try to open the config file — if it doesn't exist, that's fine,
    // we'll just use defaults instead
    FILE *fp = fopen(config_path, "r");
    if (!fp) {
        return false;
    }

    // Reset the item count — we'll fill it from the file
    state->item_count = 0;

    char line[MAX_LINE_LENGTH];
    while (fgets(line, sizeof(line), fp)) {
        // Strip the trailing newline character if present
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        nl = strchr(line, '\r');
        if (nl) *nl = '\0';

        // Skip empty lines and comment lines
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        // Don't overflow the items array
        if (state->item_count >= MAX_DOCK_ITEMS) {
            fprintf(stderr, "Warning: Too many dock items in config, "
                    "max is %d\n", MAX_DOCK_ITEMS);
            break;
        }

        // Parse the pipe-delimited fields.
        // We use strtok() which splits a string on a delimiter character.
        // Each call to strtok(NULL, "|") returns the next field.
        // We work on a copy because strtok modifies the string in place.
        char buf[MAX_LINE_LENGTH];
        strncpy(buf, line, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        // Field 1: type ("app" or "folder")
        char *type_str = strtok(buf, "|");
        if (!type_str) continue;  // Malformed line — skip it

        // Field 2: display name
        char *name = strtok(NULL, "|");
        if (!name) continue;

        // Field 3: exec_path (command to run) or folder path
        char *exec_path = strtok(NULL, "|");
        if (!exec_path) continue;

        // Field 4: icon_name (freedesktop theme icon name)
        char *icon_name = strtok(NULL, "|");
        if (!icon_name) continue;

        // Field 5: process_name (for checking if app is running)
        char *process_name = strtok(NULL, "|");
        if (!process_name) continue;

        // Field 6: separator_after ("1" or "0")
        char *sep_str = strtok(NULL, "|");
        if (!sep_str) continue;

        // Now populate the DockItem struct with the parsed values
        DockItem *item = &state->items[state->item_count];

        // Zero out the item first to ensure clean state
        memset(item, 0, sizeof(DockItem));

        // Copy parsed strings into the item's fixed-size buffers
        strncpy(item->name, name, sizeof(item->name) - 1);
        strncpy(item->exec_path, exec_path, sizeof(item->exec_path) - 1);
        strncpy(item->process_name, process_name,
                sizeof(item->process_name) - 1);

        // Determine if this is a folder item
        item->is_folder = (strcmp(type_str, "folder") == 0);

        // If it's a folder, store the folder path (exec_path holds the path)
        if (item->is_folder) {
            // Expand ~ to the user's home directory
            if (exec_path[0] == '~') {
                const char *home = getenv("HOME");
                if (home) {
                    snprintf(item->folder_path, sizeof(item->folder_path),
                             "%s%s", home, exec_path + 1);
                } else {
                    strncpy(item->folder_path, exec_path,
                            sizeof(item->folder_path) - 1);
                }
            } else {
                strncpy(item->folder_path, exec_path,
                        sizeof(item->folder_path) - 1);
            }
        }

        // Parse the separator flag — "1" means draw a separator after this icon
        item->separator_after = (strcmp(sep_str, "1") == 0);

        // Initialize animation/runtime fields to their default values
        item->scale = 1.0;
        item->bounce_offset = 0;
        item->running = false;
        item->bouncing = false;

        // Resolve the icon name to a file path and load the surface
        config_resolve_and_load_icon(item, icon_name);

        state->item_count++;
    }

    fclose(fp);

    // If the file existed but was empty or all lines were invalid,
    // report failure so the caller falls back to defaults
    if (state->item_count == 0) {
        fprintf(stderr, "Warning: Config file was empty or invalid\n");
        return false;
    }

    fprintf(stderr, "Loaded %d dock items from %s\n",
            state->item_count, config_path);
    return true;
}

// ---------------------------------------------------------------------------
// config_save — Write the current dock items to the config file.
//
// This creates/overwrites ~/.config/cc-dock/dock.conf with one line per
// dock item, using the pipe-delimited format described above.
//
// Called after drag-and-drop reordering, adding/removing items, or any other
// change that should persist across sessions.
//
// Returns true on success, false on failure.
// ---------------------------------------------------------------------------
bool config_save(DockState *state)
{
    // Make sure the config directory exists before trying to write
    if (!ensure_config_dir_exists()) {
        fprintf(stderr, "Error: Could not create config directory\n");
        return false;
    }

    char config_path[1024];
    if (!get_config_path(config_path, sizeof(config_path))) {
        return false;
    }

    // Open the file for writing — this creates it if it doesn't exist,
    // or truncates (empties) it if it does
    FILE *fp = fopen(config_path, "w");
    if (!fp) {
        fprintf(stderr, "Error: Could not open %s for writing: %s\n",
                config_path, strerror(errno));
        return false;
    }

    // Write a header comment so users know what this file is
    fprintf(fp, "# AuraDock configuration — auto-generated\n");
    fprintf(fp, "# Format: type|name|exec_path|icon_name|process_name"
                "|separator_after\n");
    fprintf(fp, "# type: \"app\" or \"folder\"\n");
    fprintf(fp, "# separator_after: 1 or 0\n\n");

    // Write each dock item as one pipe-delimited line
    for (int i = 0; i < state->item_count; i++) {
        DockItem *item = &state->items[i];

        // Determine the type string based on the is_folder flag
        const char *type_str = item->is_folder ? "folder" : "app";

        // For the exec_path field, use folder_path for folders.
        // We try to write the path with ~ for brevity/portability.
        const char *path_to_write = item->exec_path;
        char tilde_path[512];
        if (item->is_folder && item->folder_path[0] != '\0') {
            // Try to shorten the path by replacing $HOME with ~
            const char *home = getenv("HOME");
            if (home && strncmp(item->folder_path, home,
                                strlen(home)) == 0) {
                snprintf(tilde_path, sizeof(tilde_path), "~%s",
                         item->folder_path + strlen(home));
                path_to_write = tilde_path;
            } else {
                path_to_write = item->folder_path;
            }
        }

        fprintf(fp, "%s|%s|%s|%s|%s|%d\n",
                type_str,
                item->name,
                path_to_write,
                item->icon_name,
                item->process_name,
                item->separator_after ? 1 : 0);
    }

    fclose(fp);
    fprintf(stderr, "Saved %d dock items to %s\n",
            state->item_count, config_path);
    return true;
}

// ---------------------------------------------------------------------------
// config_set_defaults — Populate the dock with hardcoded default items.
//
// This is the same list that was previously hardcoded in dock.c's dock_init().
// It's used on first launch when no config file exists yet.
//
// After populating the items, we could optionally call config_save() to write
// the defaults to disk, but we leave that to the caller to decide.
// ---------------------------------------------------------------------------

// The default items list — matches what was in dock.c originally.
// Each entry: {name, exec_path, icon_name, process_name, separator_after}
typedef struct {
    const char *name;
    const char *exec_path;
    const char *icon_name;
    const char *process_name;
    bool separator_after;
} DefaultItemDef;

static const DefaultItemDef default_items[] = {
    // name               exec command       icon name (theme)             process name      sep?
    {"Finder",              "cc-finder",    "org.kde.dolphin",            "cc-finder",    false},
    {"Brave",               "brave-browser",  "com.brave.Browser",          "brave",          false},
    {"Kate",                "kate",           "org.kde.kate",               "kate",           false},
    {"Terminal",            "konsole",        "utilities-terminal",         "konsole",        false},
    {"Strawberry",          "strawberry",     "strawberry",                 "strawberry",     true },
    {"Krita",               "krita",          "krita",                      "krita",          false},
    {"GIMP",                "gimp",           "gimp",                       "gimp",           false},
    {"Inkscape",            "inkscape",       "org.inkscape.Inkscape",      "inkscape",       false},
    {"Kdenlive",            "kdenlive",       "kdenlive",                   "kdenlive",       false},
    {"System Preferences",  "systemsettings", "preferences-system",         "systemsettings", false},
    {"Trash",               "cc-finder trash:/", "trashempty",              "trash",          false},
};

#define DEFAULT_ITEM_COUNT (sizeof(default_items) / sizeof(default_items[0]))

void config_set_defaults(DockState *state)
{
    state->item_count = (int)DEFAULT_ITEM_COUNT;

    // Cap at MAX_DOCK_ITEMS to prevent array overflow
    if (state->item_count > MAX_DOCK_ITEMS) {
        state->item_count = MAX_DOCK_ITEMS;
    }

    for (int i = 0; i < state->item_count; i++) {
        DockItem *item = &state->items[i];
        const DefaultItemDef *def = &default_items[i];

        // Zero out the item struct for a clean start
        memset(item, 0, sizeof(DockItem));

        // Copy the basic text fields from the definition
        strncpy(item->name, def->name, sizeof(item->name) - 1);
        strncpy(item->exec_path, def->exec_path,
                sizeof(item->exec_path) - 1);
        strncpy(item->process_name, def->process_name,
                sizeof(item->process_name) - 1);

        // Check if this item is a folder (e.g., Downloads, Trash)
        // Trash points to the XDG Trash directory so the stacks popup can show its contents
        if (strcmp(def->name, "Trash") == 0) {
            item->is_folder = true;
            const char *home = getenv("HOME");
            snprintf(item->folder_path, sizeof(item->folder_path),
                     "%s/.local/share/Trash/files", home ? home : "/tmp");
        } else {
            item->is_folder = false;
            item->folder_path[0] = '\0';
        }

        // Copy the separator flag
        item->separator_after = def->separator_after;

        // Initialize animation/runtime state
        item->scale = 1.0;
        item->bounce_offset = 0;
        item->running = false;
        item->bouncing = false;

        // Resolve the icon and load it into the Cairo surface
        config_resolve_and_load_icon(item, def->icon_name);
    }

    fprintf(stderr, "Loaded %d default dock items\n", state->item_count);
}

// ===========================================================================
// Internal helper functions
// ===========================================================================

// ---------------------------------------------------------------------------
// get_config_dir — Build the path to the config directory.
//
// The config directory is ~/.config/cc-dock/ following the XDG Base
// Directory Specification. We use the HOME environment variable to find
// the user's home directory.
//
// Parameters:
//   out      — buffer to write the directory path into
//   out_size — size of the output buffer
//
// Returns true on success, false if HOME is not set.
// ---------------------------------------------------------------------------
static bool get_config_dir(char *out, size_t out_size)
{
    // Check for XDG_CONFIG_HOME first (the standard way to override
    // the config directory location)
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && xdg_config[0] != '\0') {
        snprintf(out, out_size, "%s/%s", xdg_config, CONFIG_DIR_NAME);
        return true;
    }

    // Fall back to ~/.config (the XDG default)
    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "Error: HOME environment variable not set\n");
        return false;
    }

    snprintf(out, out_size, "%s/.config/%s", home, CONFIG_DIR_NAME);
    return true;
}

// ---------------------------------------------------------------------------
// get_config_path — Build the full path to the config file.
//
// Combines the config directory with the config filename.
// ---------------------------------------------------------------------------
static bool get_config_path(char *out, size_t out_size)
{
    char dir[1024];
    if (!get_config_dir(dir, sizeof(dir))) {
        return false;
    }

    snprintf(out, out_size, "%s/%s", dir, CONFIG_FILE_NAME);
    return true;
}

// ---------------------------------------------------------------------------
// ensure_config_dir_exists — Create ~/.config/cc-dock/ if it doesn't exist.
//
// Uses mkdir() with mode 0755 (owner can read/write/execute, others can
// read/execute). We create both ~/.config and ~/.config/cc-dock if needed.
//
// Returns true if the directory exists (or was created), false on failure.
// ---------------------------------------------------------------------------
static bool ensure_config_dir_exists(void)
{
    char dir[1024];
    if (!get_config_dir(dir, sizeof(dir))) {
        return false;
    }

    // First, make sure ~/.config itself exists
    // (It almost always does, but let's be safe)
    const char *home = getenv("HOME");
    if (home) {
        char parent[1024];
        const char *xdg_config = getenv("XDG_CONFIG_HOME");
        if (xdg_config && xdg_config[0] != '\0') {
            strncpy(parent, xdg_config, sizeof(parent) - 1);
            parent[sizeof(parent) - 1] = '\0';
        } else {
            snprintf(parent, sizeof(parent), "%s/.config", home);
        }
        // mkdir returns -1 with EEXIST if the directory already exists,
        // which is fine — we only care about actual creation failures
        if (mkdir(parent, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "Error: Could not create %s: %s\n",
                    parent, strerror(errno));
            return false;
        }
    }

    // Now create the cc-dock config directory itself
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Error: Could not create %s: %s\n",
                dir, strerror(errno));
        return false;
    }

    return true;
}
