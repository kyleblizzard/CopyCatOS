// CopyCatOS — by Kyle Blizzard at Blizzard.show

// layout.c — Desktop spatial workspace
//
// Per-file xattr `user.moonbase.position` holds the icon's free-form
// pixel position, stored as ASCII "x y" in points. layout_apply reads
// those xattrs, auto-places anything without one, and the desktop paints
// every icon at exactly the spot the user dropped it — across reboots,
// across renames, across HiDPI scale changes.

#define _GNU_SOURCE

#include "layout.h"
#include "icons.h"
#include "desktop.h"  // desktop_hidpi_scale, S()

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/xattr.h>

// Linux mandates the `user.` prefix on xattr keys settable by unprivileged
// users on most filesystems. Public, namespaced under MoonBase.
#define XATTR_POSITION "user.moonbase.position"

// Read one file's saved position xattr. Returns true and fills (*px, *py)
// in points when the xattr exists and parses cleanly.
static bool read_position_xattr(const char *path, int *px, int *py)
{
    char buf[64];
    ssize_t n = getxattr(path, XATTR_POSITION, buf, sizeof(buf) - 1);
    if (n <= 0) return false;
    buf[n] = '\0';

    int x, y;
    if (sscanf(buf, "%d %d", &x, &y) != 2) return false;

    *px = x;
    *py = y;
    return true;
}

// Convert points to physical pixels through the current desktop scale.
// Same expression as the S() macro but operates on a runtime int, so we
// inline it here rather than abuse the macro on stored data.
static inline int pts_to_px(int pts)
{
    float s = desktop_hidpi_scale;
    if (s <= 0.0f) s = 1.0f;
    return (int)(pts * s + 0.5f);
}

void layout_apply(DesktopIcon *icons, int count, int screen_w, int screen_h)
{
    if (count == 0) return;

    int cell_w = S(ICON_CELL_W);
    int cell_h = S(ICON_CELL_H);
    int top    = S(ICON_TOP_MARGIN);
    int right  = S(ICON_RIGHT_MARGIN);

    int rows_per_col = (screen_h - top) / cell_h;
    if (rows_per_col < 1) rows_per_col = 1;

    // Occupancy grid for auto-place. Free-form icons mark the canonical
    // cell their center maps to as occupied, so a fresh file scanned on
    // first login doesn't auto-place underneath an icon the user dragged.
    #define MAX_GRID_COLS 50
    bool occupied[MAX_GRID_COLS * rows_per_col];
    memset(occupied, 0, sizeof(bool) * MAX_GRID_COLS * rows_per_col);

    // ── Pass 1: place icons that have a saved xattr position ──────────
    int restored = 0;
    for (int i = 0; i < count; i++) {
        int x_pt, y_pt;
        if (!read_position_xattr(icons[i].path, &x_pt, &y_pt)) {
            icons[i].has_pos = false;
            continue;
        }

        // Stored value is in points — multiply through the current scale
        // so the user gets the same logical drop point on the 1.0× HDMI
        // monitor and the 1.75× Legion panel.
        icons[i].x = pts_to_px(x_pt);
        icons[i].y = pts_to_px(y_pt);
        icons[i].has_pos = true;

        // Tag the icon with the canonical grid cell its center maps onto
        // so Clean Up has a starting cell, and so the auto-place pass
        // below can avoid landing a new icon on top of this one.
        int cx = icons[i].x + cell_w / 2;
        int cy = icons[i].y + cell_h / 2;
        int col = (screen_w - right - cx) / cell_w;
        int row = (cy - top) / cell_h;
        if (col < 0) col = 0;
        if (col >= MAX_GRID_COLS) col = MAX_GRID_COLS - 1;
        if (row < 0) row = 0;
        if (row >= rows_per_col) row = rows_per_col - 1;
        icons[i].grid_col = col;
        icons[i].grid_row = row;
        occupied[col * rows_per_col + row] = true;
        restored++;
    }

    // ── Pass 2: auto-place icons with no saved xattr ──────────────────
    int auto_col = 0, auto_row = 0;
    for (int i = 0; i < count; i++) {
        if (icons[i].has_pos) continue;

        bool found = false;
        while (auto_col < MAX_GRID_COLS) {
            if (!occupied[auto_col * rows_per_col + auto_row]) {
                found = true;
                break;
            }
            auto_row++;
            if (auto_row >= rows_per_col) {
                auto_row = 0;
                auto_col++;
            }
        }
        if (!found) {
            // Out of grid space — stack at the last column, top.
            // 50 columns is a hard ceiling; nobody hits it in practice.
            auto_col = MAX_GRID_COLS - 1;
            auto_row = 0;
        }

        icons[i].grid_col = auto_col;
        icons[i].grid_row = auto_row;
        icons[i].x = screen_w - right - cell_w - (auto_col * cell_w);
        icons[i].y = top + (auto_row * cell_h);
        occupied[auto_col * rows_per_col + auto_row] = true;

        auto_row++;
        if (auto_row >= rows_per_col) {
            auto_row = 0;
            auto_col++;
        }
    }

    fprintf(stderr, "[layout] Applied: %d xattr-placed, %d auto-placed\n",
            restored, count - restored);
}

void layout_save_position(const DesktopIcon *icon)
{
    if (!icon || !icon->path[0]) return;

    float scale = desktop_hidpi_scale;
    if (scale <= 0.0f) scale = 1.0f;

    int x_pt = (int)(icon->x / scale + 0.5f);
    int y_pt = (int)(icon->y / scale + 0.5f);

    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%d %d", x_pt, y_pt);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return;

    if (setxattr(icon->path, XATTR_POSITION, buf, (size_t)n, 0) != 0) {
        fprintf(stderr, "[layout] setxattr %s on %s failed: %s\n",
                XATTR_POSITION, icon->path, strerror(errno));
        return;
    }

    fprintf(stderr, "[layout] Saved %s position (%d, %d) pts\n",
            icon->name, x_pt, y_pt);
}

void layout_clear_position(const DesktopIcon *icon)
{
    if (!icon || !icon->path[0]) return;

    if (removexattr(icon->path, XATTR_POSITION) != 0 && errno != ENODATA) {
        fprintf(stderr, "[layout] removexattr on %s failed: %s\n",
                icon->path, strerror(errno));
    }
}
