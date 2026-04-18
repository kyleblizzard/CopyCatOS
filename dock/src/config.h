// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// config.h — Dock configuration persistence
//
// This module handles loading and saving the dock's item list so that changes
// (like drag-and-drop reordering or adding/removing items) survive across
// sessions. The configuration is stored as a simple pipe-delimited text file
// at ~/.config/dock/dock.conf.
//
// When the dock starts up, it tries to load the config file. If the file
// doesn't exist (first launch), it falls back to a hardcoded list of default
// items. Whenever the user modifies the dock, we save the current state back
// to the same file.
// ============================================================================

#ifndef DOCK_CONFIG_H
#define DOCK_CONFIG_H

#include "dock.h"

// ---------------------------------------------------------------------------
// config_load — Read dock items from the config file on disk.
//
// Parses ~/.config/dock/dock.conf and populates state->items[].
// Each line in the file represents one dock item with pipe-delimited fields.
//
// Returns true if the config file was found and loaded successfully.
// Returns false if the file doesn't exist or can't be read — the caller
// should then call config_set_defaults() to use the hardcoded items.
// ---------------------------------------------------------------------------
bool config_load(DockState *state);

// ---------------------------------------------------------------------------
// config_save — Write the current dock items to the config file.
//
// Iterates over state->items[] and writes each one as a pipe-delimited line
// to ~/.config/dock/dock.conf. Creates the directory if it doesn't
// exist yet (e.g., on first save after rearranging).
//
// Returns true on success, false if writing failed.
// ---------------------------------------------------------------------------
bool config_save(DockState *state);

// ---------------------------------------------------------------------------
// config_set_defaults — Fill the dock with the hardcoded default items.
//
// This is called when no config file exists (first launch). It populates
// state->items[] from the built-in default list and loads icons for each.
// ---------------------------------------------------------------------------
void config_set_defaults(DockState *state);

// ---------------------------------------------------------------------------
// config_resolve_and_load_icon — Find and load an icon by its theme name.
//
// Given an icon_name (e.g., "org.kde.dolphin"), this function:
// 1. Searches multiple icon theme directories to find the PNG file
// 2. Loads it as a Cairo image surface into item->icon
// 3. Falls back to a generated placeholder icon if nothing is found
//
// This is shared between config_load() (loading saved items) and
// config_set_defaults() (loading default items) so both paths use the
// same icon resolution logic.
//
// Returns true if a real icon was found, false if using the fallback.
// ---------------------------------------------------------------------------
bool config_resolve_and_load_icon(DockItem *item, const char *icon_name);

#endif // DOCK_CONFIG_H
