// CopyCatOS — by Kyle Blizzard at Blizzard.show

// content.h — Finder content area (icon grid / list view / column view)
//
// The content area is the main panel of the Finder window — everything
// to the right of the sidebar and below the toolbar. It displays the
// contents of the current directory in one of three view modes:
//
// Icon view (VIEW_ICONS):
//   - Files displayed as 64x64 icons in a grid
//   - Grid cells are 90x90 pixels
//   - White background (#FFFFFF)
//   - Selected icon: blue rounded rect highlight behind it
//   - Labels: Lucida Grande 12pt, centered, truncated with "..."
//
// List view (VIEW_LIST):
//   - Columns: Name, Date Modified, Size, Kind
//   - Column headers: grey gradient background, bold text
//   - Alternating row colors: #FFFFFF / #D8E6F5 (per Apple HIG spec)
//   - Each row: 16x16 icon + filename + date + size + kind
//   - Selected row: full-width blue highlight (#386C9D)
//
// Column view (VIEW_COLUMNS):
//   - Reserved for future implementation

#ifndef AURA_CONTENT_H
#define AURA_CONTENT_H

#include "finder.h"

// Maximum number of files we can display in a single directory.
// 1024 is more than enough for any reasonable folder.
#define CONTENT_MAX_FILES 1024

// ── Icon view constants ────────────────────────────────────────────
// Icon size in the content grid (smaller than desktop icons)
#define CONTENT_ICON_SIZE   64

// Grid cell dimensions — each file occupies this much space
#define CONTENT_CELL_W      90
#define CONTENT_CELL_H      90

// ── List view constants ────────────────────────────────────────────
// Row height for list view — enough for a 16x16 icon plus padding
#define CONTENT_LIST_ROW_H      20

// Small icon size used in list view rows
#define CONTENT_LIST_ICON_SIZE  16

// Height of the column header bar at the top of list view
#define CONTENT_LIST_HEADER_H   22

// Column widths for list view (Name is flexible, others are fixed)
#define CONTENT_LIST_COL_DATE   140
#define CONTENT_LIST_COL_SIZE    70
#define CONTENT_LIST_COL_KIND   100

// ── Alternating row colors (Apple HIG, Snow Leopard) ───────────────
// Even rows: pure white
#define CONTENT_ROW_EVEN_R  1.0
#define CONTENT_ROW_EVEN_G  1.0
#define CONTENT_ROW_EVEN_B  1.0

// Odd rows: light blue tint (#D8E6F5)
#define CONTENT_ROW_ODD_R   (0xD8 / 255.0)
#define CONTENT_ROW_ODD_G   (0xE6 / 255.0)
#define CONTENT_ROW_ODD_B   (0xF5 / 255.0)

// Selection blue for list view rows (#386C9D)
#define CONTENT_SEL_R       (0x38 / 255.0)
#define CONTENT_SEL_G       (0x6C / 255.0)
#define CONTENT_SEL_B       (0x9D / 255.0)

// ── View mode enum ─────────────────────────────────────────────────
// Determines how the content area displays files. The user can toggle
// between these via the toolbar view buttons (icon/list/column).

// ViewMode is defined in finder.h (the root header that everyone includes).

// Represents a single file entry in the content view.
typedef struct {
    char name[256];              // Display name (filename)
    char path[1024];             // Full filesystem path
    cairo_surface_t *icon;       // Pre-loaded icon surface (64x64 or 16x16)
    cairo_surface_t *icon_small; // 16x16 icon for list view (NULL until needed)
    bool is_directory;           // true for folders, false for files
    bool selected;               // true if this item is currently selected
    off_t file_size;             // File size in bytes (0 for directories)
    time_t mod_time;             // Last modification time (Unix timestamp)
} ContentFile;

// Scan a directory and populate the internal file list.
// Called when the Finder navigates to a new path.
void content_scan(const char *dir_path);

// Paint the content area onto the Finder's Cairo context.
// Draws the white background, then the icon grid or list with labels.
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

// Get the current view mode.
ViewMode content_get_view_mode(void);

// Set the view mode (icons, list, or columns).
// Triggers a repaint on the next paint cycle.
void content_set_view_mode(ViewMode mode);

// Free all loaded file resources (icon surfaces, etc.).
void content_shutdown(void);

#endif // AURA_CONTENT_H
