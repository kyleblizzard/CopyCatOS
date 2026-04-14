// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// registry.c — Preference pane definitions
// ============================================================================
//
// Registers all 27 Snow Leopard preference panes in the exact order they
// appear in NSPrefPaneGroups.xml. Each pane has an internal ID, display name,
// category, and paths to its 32x32 and 128x128 icon PNGs.
//
// Icons are stored in the cc-sysprefs/assets/ directory and installed to
// ~/.local/share/aqua-widgets/sysprefs/ at runtime.
// ============================================================================

#include "registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Helper — register a single pane
// ============================================================================
static void add_pane(SysPrefsState *state,
                     const char *id,
                     const char *name,
                     const char *category,
                     const char *icon_file)
{
    if (state->pane_count >= MAX_PANES) return;

    PaneInfo *p = &state->panes[state->pane_count];
    strncpy(p->id, id, sizeof(p->id) - 1);
    strncpy(p->name, name, sizeof(p->name) - 1);
    strncpy(p->category, category, sizeof(p->category) - 1);

    // Build icon paths — look in the install location first, fall back to
    // the build directory for development
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    // Runtime path (installed assets)
    snprintf(p->icon_path_32, sizeof(p->icon_path_32),
             "%s/.local/share/aqua-widgets/sysprefs/icons/%s.png", home, icon_file);
    snprintf(p->icon_path_128, sizeof(p->icon_path_128),
             "%s/.local/share/aqua-widgets/sysprefs/icons-128/%s.png", home, icon_file);

    p->icon_32 = NULL;
    p->icon_128 = NULL;

    state->pane_count++;
}

// ============================================================================
// registry_init — Register all 27 preference panes
// ============================================================================
//
// The order within each category matches the real Snow Leopard System
// Preferences exactly. Categories are: personal, hardware, net, system.
// ============================================================================
void registry_init(SysPrefsState *state)
{
    state->pane_count = 0;
    state->category_count = 0;

    // ── Personal (7 panes) ──────────────────────────────────────────────
    add_pane(state, "appearance",         "Appearance",             "personal", "appearance");
    add_pane(state, "desktop-screensaver","Desktop &\nScreen Saver","personal", "desktop-screensaver");
    add_pane(state, "dock",              "Dock",                    "personal", "dock");
    add_pane(state, "expose-spaces",     "Exposé &\nSpaces",       "personal", "expose-spaces");
    add_pane(state, "language-text",     "Language &\nText",       "personal", "language-text");
    add_pane(state, "security",          "Security",                "personal", "security");
    add_pane(state, "spotlight",         "Spotlight",               "personal", "spotlight");

    // ── Hardware (8 panes) ──────────────────────────────────────────────
    add_pane(state, "cds-dvds",          "CDs &\nDVDs",            "hardware", "cds-dvds");
    add_pane(state, "displays",          "Displays",               "hardware", "displays");
    add_pane(state, "energy-saver",      "Energy\nSaver",          "hardware", "energy-saver");
    add_pane(state, "keyboard",          "Keyboard",               "hardware", "keyboard");
    add_pane(state, "mouse",             "Mouse",                   "hardware", "mouse");
    add_pane(state, "trackpad",          "Trackpad",               "hardware", "trackpad");
    add_pane(state, "print-fax",         "Print &\nFax",           "hardware", "print-fax");
    add_pane(state, "sound",             "Sound",                   "hardware", "sound");

    // ── Internet & Wireless (4 panes) ───────────────────────────────────
    add_pane(state, "mobileme",          "MobileMe",               "net",      "mobileme");
    add_pane(state, "network",           "Network",                 "net",      "network");
    add_pane(state, "bluetooth",         "Bluetooth",               "net",      "bluetooth");
    add_pane(state, "sharing",           "Sharing",                 "net",      "sharing");

    // ── System (8 panes) ────────────────────────────────────────────────
    add_pane(state, "accounts",          "Accounts",               "system",   "accounts");
    add_pane(state, "date-time",         "Date &\nTime",           "system",   "date-time");
    add_pane(state, "parental-controls", "Parental\nControls",     "system",   "parental-controls");
    add_pane(state, "software-update",   "Software\nUpdate",       "system",   "software-update");
    add_pane(state, "speech",            "Speech",                  "system",   "speech");
    add_pane(state, "startup-disk",      "Startup\nDisk",          "system",   "startup-disk");
    add_pane(state, "time-machine",      "Time\nMachine",          "system",   "time-machine");
    add_pane(state, "universal-access",  "Universal\nAccess",      "system",   "universal-access");

    // ── Build category index ────────────────────────────────────────────
    // Walk through panes and group them by category
    const char *cat_keys[] = {"personal", "hardware", "net", "system"};
    const char *cat_labels[] = {"Personal", "Hardware", "Internet & Wireless", "System"};
    int num_cats = 4;

    for (int c = 0; c < num_cats; c++) {
        CategoryInfo *cat = &state->categories[state->category_count];
        strncpy(cat->key, cat_keys[c], sizeof(cat->key) - 1);
        strncpy(cat->label, cat_labels[c], sizeof(cat->label) - 1);
        cat->first_pane = -1;
        cat->pane_count = 0;

        for (int p = 0; p < state->pane_count; p++) {
            if (strcmp(state->panes[p].category, cat_keys[c]) == 0) {
                if (cat->first_pane < 0) {
                    cat->first_pane = p;
                }
                cat->pane_count++;
            }
        }

        state->category_count++;
    }
}

// ============================================================================
// registry_load_icons — Load all 32x32 pane icons from PNG files
// ============================================================================
void registry_load_icons(SysPrefsState *state)
{
    int loaded = 0;

    for (int i = 0; i < state->pane_count; i++) {
        PaneInfo *p = &state->panes[i];

        p->icon_32 = cairo_image_surface_create_from_png(p->icon_path_32);
        if (cairo_surface_status(p->icon_32) != CAIRO_STATUS_SUCCESS) {
            fprintf(stderr, "[cc-sysprefs] Failed to load icon: %s\n",
                    p->icon_path_32);
            cairo_surface_destroy(p->icon_32);
            p->icon_32 = NULL;
        } else {
            loaded++;
        }
    }

    fprintf(stderr, "[cc-sysprefs] Loaded %d/%d pane icons\n",
            loaded, state->pane_count);
}

// ============================================================================
// registry_load_icon_128 — Lazy-load a 128x128 icon for the stub pane view
// ============================================================================
void registry_load_icon_128(SysPrefsState *state, int pane_index)
{
    if (pane_index < 0 || pane_index >= state->pane_count) return;

    PaneInfo *p = &state->panes[pane_index];
    if (p->icon_128) return;  // Already loaded

    p->icon_128 = cairo_image_surface_create_from_png(p->icon_path_128);
    if (cairo_surface_status(p->icon_128) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "[cc-sysprefs] Failed to load 128px icon: %s\n",
                p->icon_path_128);
        cairo_surface_destroy(p->icon_128);
        p->icon_128 = NULL;
    }
}
