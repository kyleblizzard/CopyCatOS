// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// content.h — Finder content area (file icon grid / list view)
//
// The content area is the main panel of the Finder window — everything
// to the right of the sidebar and below the toolbar. It displays the
// contents of the current directory as an icon grid (Phase 1).
//
// In icon view:
//   - Files are displayed as 64x64 icons in a grid
//   - Grid cells are 90x90 pixels
//   - White background (#FFFFFF)
//   - Icons loaded from AquaKDE theme, falling back to Cairo-drawn generic icons
//
// In list view (future):
//   - Alternating row colors: #FFFFFF / #D8E6F5 (per Apple HIG spec)
//   - Columns: Name, Date Modified, Size, Kind

#ifndef AURA_CONTENT_H
#define AURA_CONTENT_H

#include "finder.h"

// Maximum number of files we can display in a single directory.
// 1024 is more than enough for any reasonable folder.
#define CONTENT_MAX_FILES 1024

// Icon size in the content grid (smaller than desktop icons)
#define CONTENT_ICON_SIZE   64

// Grid cell dimensions — each file occupies this much space
#define CONTENT_CELL_W      90
#define CONTENT_CELL_H      90

// Represents a single file entry in the content view.
typedef struct {
    char name[256];              // Display name (filename)
    char path[1024];             // Full filesystem path
    cairo_surface_t *icon;       // Pre-loaded icon surface (64x64)
    bool is_directory;           // true for folders, false for files
    bool selected;               // true if this item is currently selected
} ContentFile;

// Scan a directory and populate the internal file list.
// Called when the Finder navigates to a new path.
void content_scan(const char *dir_path);

// Paint the content area onto the Finder's Cairo context.
// Draws the white background, then the icon grid with labels.
void content_paint(FinderState *fs);

// Handle a mouse click in the content area.
// x, y are in window coordinates (not content-area-local).
// Returns true if a file was clicked.
bool content_handle_click(FinderState *fs, int x, int y);

// Handle a double-click in the content area.
// If a directory was double-clicked, navigates into it.
// If a file was double-clicked, opens it with xdg-open.
void content_handle_double_click(FinderState *fs, int x, int y);

// Get the number of files currently loaded.
int content_get_count(void);

// Free all loaded file resources (icon surfaces, etc.).
void content_shutdown(void);

#endif // AURA_CONTENT_H
