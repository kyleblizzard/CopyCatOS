// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// layout.c — Persistent desktop icon layout (spatial memory)
//
// Snow Leopard's defining characteristic: icons stay exactly where you
// put them. You place your project folder in the top-right, a year later
// it's still there. This module implements that guarantee.
//
// Every time you drag an icon to a new position, layout_save_all() writes
// the complete grid layout to disk atomically. Every time cc-desktop starts,
// layout_load() reads it back and layout_apply() restores every icon to
// exactly where you left it.
//
// New files (not in the saved layout) are auto-placed in the next free
// grid cell, maintaining the same top-right → down → left ordering that
// Finder uses for auto-arranged icons.

#define _GNU_SOURCE  // For strndup

#include "layout.h"
#include "icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>   // mkdir
#include <unistd.h>     // rename
#include <errno.h>

// ── Internal layout store ─────────────────────────────────────────────

// One entry per file we've seen placed. We use filenames (not full paths)
// as keys so the layout survives if you move your home directory.
typedef struct {
    char name[256];  // Filename (basename only, e.g. "report.pdf")
    int  col;        // Grid column (0 = rightmost)
    int  row;        // Grid row (0 = topmost)
} LayoutEntry;

// In-memory table loaded by layout_load() and used by layout_apply().
// 512 entries covers any sane number of desktop files.
#define MAX_LAYOUT_ENTRIES 512
static LayoutEntry entries[MAX_LAYOUT_ENTRIES];
static int         entry_count = 0;

// ── Path helpers ──────────────────────────────────────────────────────

// Build the path to the layout file and ensure its parent directory exists.
// Path: ~/.local/share/copycatos/desktop-layout.ini
static void get_layout_path(char *buf, size_t bufsize)
{
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    // Ensure the directory exists. mkdir() with -p equivalent:
    // we create each component manually.
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/.local/share/copycatos", home);

    // Create ~/.local if missing
    char local_dir[1024];
    snprintf(local_dir, sizeof(local_dir), "%s/.local", home);
    mkdir(local_dir, 0755);  // Ignore error — may already exist

    // Create ~/.local/share if missing
    char share_dir[1024];
    snprintf(share_dir, sizeof(share_dir), "%s/.local/share", home);
    mkdir(share_dir, 0755);

    // Create ~/.local/share/copycatos if missing
    mkdir(dir, 0755);

    snprintf(buf, bufsize, "%s/desktop-layout.ini", dir);
}

// ── layout_load ───────────────────────────────────────────────────────

void layout_load(void)
{
    entry_count = 0;

    char path[1024];
    get_layout_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        // File doesn't exist yet — first run. That's fine.
        fprintf(stderr, "[layout] No saved layout (first run or cleared)\n");
        return;
    }

    char line[512];
    int  loaded = 0;

    while (fgets(line, sizeof(line), f) && entry_count < MAX_LAYOUT_ENTRIES) {
        // Skip blank lines and comment lines (starting with '#')
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;

        // Format: filename=col:row
        // The filename can contain any character except '=' and newline.
        // We find the last '=' to handle filenames that contain '='.
        char *eq = strrchr(p, '=');
        if (!eq) continue;

        // Split at '='
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;

        // Parse col:row from value
        int col, row;
        if (sscanf(val, "%d:%d", &col, &row) != 2) continue;
        if (col < 0 || row < 0 || col > 200 || row > 200) continue;

        // Strip any trailing whitespace from the key (filename)
        size_t klen = strlen(key);
        while (klen > 0 && (key[klen-1] == ' ' || key[klen-1] == '\t')) {
            key[--klen] = '\0';
        }
        if (klen == 0 || klen >= 256) continue;

        // Store the entry
        strncpy(entries[entry_count].name, key, 255);
        entries[entry_count].name[255] = '\0';
        entries[entry_count].col = col;
        entries[entry_count].row = row;
        entry_count++;
        loaded++;
    }

    fclose(f);
    fprintf(stderr, "[layout] Loaded %d saved icon positions from %s\n",
            loaded, path);
}

// ── layout_apply ─────────────────────────────────────────────────────

// Look up a filename in the loaded entry table.
// Returns the entry pointer, or NULL if not found.
static LayoutEntry *layout_find(const char *name)
{
    for (int i = 0; i < entry_count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

// Convert grid (col, row) to screen pixel position.
// This is the single source of truth for the grid-to-pixel mapping.
// col=0 is the rightmost column; row=0 is the topmost row.
static void grid_to_pixel(int col, int row, int screen_w,
                           int *out_x, int *out_y)
{
    *out_x = screen_w - ICON_RIGHT_MARGIN - ICON_CELL_W - (col * ICON_CELL_W);
    *out_y = ICON_TOP_MARGIN + (row * ICON_CELL_H);
}

void layout_apply(DesktopIcon *icons, int count, int screen_w, int screen_h)
{
    if (count == 0) return;

    // How many rows fit on screen, accounting for the menubar margin.
    int rows_per_col = (screen_h - ICON_TOP_MARGIN) / ICON_CELL_H;
    if (rows_per_col < 1) rows_per_col = 1;

    // Build an occupancy grid so we know which cells are already taken
    // by icons with saved positions. We need this to auto-place new icons.
    // Use a flat array: occupied[col * rows_per_col + row]
    // We cap at 50 columns which is far more than any screen will show.
    #define MAX_GRID_COLS 50
    bool occupied[MAX_GRID_COLS * rows_per_col];
    memset(occupied, 0, sizeof(bool) * MAX_GRID_COLS * rows_per_col);

    // ── Pass 1: restore icons that have a saved position ──────────────
    int restored = 0;
    for (int i = 0; i < count; i++) {
        LayoutEntry *e = layout_find(icons[i].name);
        if (!e) continue;

        int col = e->col;
        int row = e->row;

        // Validate: clamp to grid bounds in case screen shrank
        if (col >= MAX_GRID_COLS) col = MAX_GRID_COLS - 1;
        if (row >= rows_per_col)  row = rows_per_col - 1;

        // If two saved icons claim the same cell (shouldn't happen but
        // be defensive), skip this one — it'll get auto-placed below.
        if (col >= 0 && col < MAX_GRID_COLS &&
            occupied[col * rows_per_col + row]) {
            fprintf(stderr, "[layout] Collision at col=%d row=%d for '%s', "
                    "will auto-place\n", col, row, icons[i].name);
            continue;
        }

        // Restore position
        icons[i].grid_col = col;
        icons[i].grid_row = row;
        grid_to_pixel(col, row, screen_w, &icons[i].x, &icons[i].y);

        // Mark cell occupied
        if (col >= 0 && col < MAX_GRID_COLS)
            occupied[col * rows_per_col + row] = true;

        restored++;
    }

    // ── Pass 2: auto-place icons with no saved position ───────────────
    // Walk cells in Snow Leopard order (col=0 row=0, col=0 row=1, ...
    // col=1 row=0, ...) and assign the first free cell to each new icon.
    int auto_col = 0;
    int auto_row = 0;

    // Helper: advance (auto_col, auto_row) to the next free cell.
    // Returns false if we've run out of grid space.
    // (Defined as a nested lambda-equivalent using a local goto target.)
    for (int i = 0; i < count; i++) {
        LayoutEntry *e = layout_find(icons[i].name);
        if (e) continue;  // Already placed in pass 1

        // Find next free cell
        bool found = false;
        while (auto_col < MAX_GRID_COLS) {
            if (!occupied[auto_col * rows_per_col + auto_row]) {
                found = true;
                break;
            }
            // Advance: go down first, then to next column
            auto_row++;
            if (auto_row >= rows_per_col) {
                auto_row = 0;
                auto_col++;
            }
        }

        if (!found) {
            // No more grid space — stack at last column
            auto_col = MAX_GRID_COLS - 1;
            auto_row = 0;
        }

        // Place the icon
        icons[i].grid_col = auto_col;
        icons[i].grid_row = auto_row;
        grid_to_pixel(auto_col, auto_row, screen_w, &icons[i].x, &icons[i].y);
        occupied[auto_col * rows_per_col + auto_row] = true;

        // Advance cursor for next auto-placed icon
        auto_row++;
        if (auto_row >= rows_per_col) {
            auto_row = 0;
            auto_col++;
        }
    }

    fprintf(stderr, "[layout] Applied: %d restored, %d auto-placed\n",
            restored, count - restored);
}

// ── layout_save_all ───────────────────────────────────────────────────

void layout_save_all(const DesktopIcon *icons, int count)
{
    char path[1024];
    get_layout_path(path, sizeof(path));

    // Write to a temp file first, then atomically rename into place.
    // This ensures the layout file is never in a half-written state
    // even if cc-desktop is killed during the write.
    char tmp_path[1056];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        fprintf(stderr, "[layout] Cannot write layout: %s (errno=%d)\n",
                tmp_path, errno);
        return;
    }

    // Write a header comment so the file is human-readable
    fprintf(f, "# CopyCatOS desktop icon layout\n");
    fprintf(f, "# Format: filename=col:row  (col 0 = rightmost, row 0 = top)\n");
    fprintf(f, "# Edit this file to pre-position icons, or just drag them.\n");
    fprintf(f, "#\n");

    for (int i = 0; i < count; i++) {
        // Write: filename=col:row
        // The filename is the basename only (no path prefix).
        fprintf(f, "%s=%d:%d\n", icons[i].name, icons[i].grid_col, icons[i].grid_row);
    }

    fclose(f);

    // Atomic rename: replaces the old file in one syscall.
    // If rename() fails for some reason, we at least have the .tmp file.
    if (rename(tmp_path, path) != 0) {
        fprintf(stderr, "[layout] rename() failed: %s → %s (errno=%d)\n",
                tmp_path, path, errno);
        return;
    }

    fprintf(stderr, "[layout] Saved %d icon positions to %s\n", count, path);

    // Keep the in-memory table in sync with what we just wrote.
    // This way subsequent layout_find() calls see up-to-date data.
    entry_count = 0;
    for (int i = 0; i < count && entry_count < MAX_LAYOUT_ENTRIES; i++) {
        strncpy(entries[entry_count].name, icons[i].name, 255);
        entries[entry_count].name[255] = '\0';
        entries[entry_count].col = icons[i].grid_col;
        entries[entry_count].row = icons[i].grid_row;
        entry_count++;
    }
}
