// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// stacks.c — Folder stacks grid popup (Snow Leopard style)
//
// This module implements the folder "stacks" popup that appears when the user
// clicks a folder icon in the dock. It shows the folder's contents in a grid,
// similar to Mac OS X Snow Leopard's "Grid" stack view.
//
// How it works:
//   1. When a folder is clicked, stacks_show() scans the directory.
//   2. Each file gets an icon resolved from the AquaKDE theme (or fallback).
//   3. The popup window is created as an override-redirect ARGB window
//      (no window manager decorations, supports transparency).
//   4. A nine-patch background is drawn from eccl_*.png assets, giving
//      the popup its dark semi-transparent rounded-corner look.
//   5. A callout arrow points down at the folder icon in the dock.
//   6. Items are laid out in a 5-column grid with hover highlighting.
//   7. Clicking an item opens it with xdg-open; clicking outside or
//      pressing Escape closes the popup.
//
// Integration into dock.c (add these changes manually):
//
//   In ButtonPress/ButtonRelease for left click:
//     if (items[idx].is_folder) {
//         double cx = state->win_x + dock_get_icon_center_x(state, idx);
//         stacks_show(state, &items[idx], cx);
//     } else {
//         launch_app(state, &items[idx]);
//     }
//
//   In the event dispatch loop, before menu_handle_event:
//     if (stacks_is_open() && stacks_handle_event(state, &ev)) continue;
//
//   In dock_init():   stacks_load_assets(state);
//   In dock_cleanup(): stacks_cleanup(state);
//   In meson.build:   add 'src/stacks.c' to the sources list
// ============================================================================

#define _GNU_SOURCE  // For M_PI in math.h under strict C11

#include "stacks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>       // opendir, readdir, closedir — for scanning folders
#include <sys/stat.h>     // stat() — to check if an entry is a file or directory

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>   // XK_Escape — for keyboard event handling
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>  // Pango — for text rendering with ellipsis truncation

// ---------------------------------------------------------------------------
// Grid layout constants
//
// These define the visual layout of the stacks popup. The grid arranges
// items in rows of STACK_GRID_COLS, with each cell being STACK_CELL_W x
// STACK_CELL_H pixels. The nine-patch border pieces are 40px wide/tall.
// ---------------------------------------------------------------------------
#define STACK_GRID_COLS         5       // Number of items per row in the grid
#define STACK_ICON_SIZE         48      // Icon size in pixels within each grid cell
#define STACK_CELL_W            80      // Width of one grid cell (icon + label space)
#define STACK_CELL_H            80      // Height of one grid cell
#define STACK_PADDING           20      // Inner padding between nine-patch border and grid content
#define STACK_MAX_ITEMS         40      // Maximum number of items to display in the popup
#define STACK_NINEPATCH_BORDER  40      // Width/height of the nine-patch corner and edge pieces
#define STACK_CALLOUT_W         45      // Width of the callout arrow asset in pixels
#define STACK_CALLOUT_H         40      // Height of the callout arrow asset in pixels
#define STACK_LABEL_FONT        "Lucida Grande 10"  // Font for item labels below icons

// ---------------------------------------------------------------------------
// StackEntry — One file or directory inside the folder being displayed
//
// Each entry stores its display name, full filesystem path, a loaded
// Cairo icon surface, and whether it's a directory (for icon selection).
// ---------------------------------------------------------------------------
typedef struct {
    char name[256];             // Display name (filename without path)
    char path[1024];            // Full filesystem path to this entry
    cairo_surface_t *icon;      // Loaded icon surface (scaled to 48x48), or NULL
    bool is_directory;          // True if this entry is a subdirectory
} StackEntry;

// ---------------------------------------------------------------------------
// Module-level state — all the data for the currently-open stacks popup.
//
// Only one stacks popup can be open at a time. This static struct holds
// everything: the X window, the Cairo drawing context, the list of entries,
// and the nine-patch background assets.
// ---------------------------------------------------------------------------
static struct {
    // X11 window and Cairo drawing context for the popup
    Window win;                         // The popup window (None if closed)
    cairo_surface_t *surface;           // Cairo surface bound to the X window
    cairo_t *cr;                        // Cairo drawing context

    // Position and size of the popup window on screen
    int win_x, win_y, win_w, win_h;

    // The folder entries displayed in the grid
    StackEntry entries[STACK_MAX_ITEMS];
    int entry_count;                    // How many entries are in the array

    // Mouse hover tracking — which grid cell the cursor is over (-1 = none)
    int hover_index;

    // Nine-patch background surfaces (loaded from eccl_*.png files).
    // The nine-patch is a technique for stretching a background image:
    // the four corners stay fixed, the edges tile/stretch, and the center
    // fills the remaining space. This gives us rounded corners and a
    // consistent border at any popup size.
    cairo_surface_t *np_tl, *np_t, *np_tr;   // Top-left, top, top-right
    cairo_surface_t *np_l, *np_c, *np_r;     // Left, center, right
    cairo_surface_t *np_bl, *np_b, *np_br;   // Bottom-left, bottom, bottom-right
    cairo_surface_t *np_callout;              // The arrow pointing down at the dock

    // The screen X coordinate where the callout arrow should point
    // (relative to the popup window's left edge)
    double callout_x;

    // Whether the popup is currently visible
    bool open;
} stacks = {0};

// ---------------------------------------------------------------------------
// Forward declarations for internal helper functions
// ---------------------------------------------------------------------------
static void stacks_paint(void);
static void stacks_draw_ninepatch(cairo_t *cr, int w, int h);
static void stacks_draw_grid(cairo_t *cr);
static int  stacks_hit_test(int mx, int my);
static void stacks_open_entry(DockState *state, StackEntry *entry);
static bool stacks_resolve_icon(StackEntry *entry);
static int  entry_compare(const void *a, const void *b);

// ---------------------------------------------------------------------------
// stacks_load_assets — Load the nine-patch PNG files for the popup background.
//
// The nine-patch consists of 9 PNG images (corners, edges, center) plus
// a callout arrow image. They live in:
//   ~/.local/share/aqua-widgets/dock/stacks/
//
// If any file is missing, we set its surface to NULL and the drawing code
// will use a solid dark fallback instead. This way the popup still works
// even if the assets aren't installed yet.
// ---------------------------------------------------------------------------
bool stacks_load_assets(DockState *state)
{
    (void)state;  // Not needed for loading PNGs, but kept for API consistency

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    char path[1024];

    // Helper macro: build the full path for a nine-patch piece and load it.
    // cairo_image_surface_create_from_png returns a "nil" surface on failure
    // (not NULL), so we check the status and set to NULL if it failed.
    #define LOAD_NP(field, filename) do { \
        snprintf(path, sizeof(path), \
                 "%s/.local/share/aqua-widgets/dock/stacks/%s", home, filename); \
        stacks.field = cairo_image_surface_create_from_png(path); \
        if (cairo_surface_status(stacks.field) != CAIRO_STATUS_SUCCESS) { \
            fprintf(stderr, "[stacks] Warning: could not load %s\n", path); \
            cairo_surface_destroy(stacks.field); \
            stacks.field = NULL; \
        } \
    } while (0)

    // Load all 9 background pieces and the callout arrow
    LOAD_NP(np_tl, "eccl_top_left.png");
    LOAD_NP(np_t,  "eccl_top.png");
    LOAD_NP(np_tr, "eccl_top_right.png");
    LOAD_NP(np_l,  "eccl_left.png");
    LOAD_NP(np_c,  "eccl_center.png");
    LOAD_NP(np_r,  "eccl_right.png");
    LOAD_NP(np_bl, "eccl_bottom_left.png");
    LOAD_NP(np_b,  "eccl_bottom.png");
    LOAD_NP(np_br, "eccl_bottom_right.png");
    LOAD_NP(np_callout, "eccl_callout_bottom.png");

    #undef LOAD_NP

    return true;
}

// ---------------------------------------------------------------------------
// stacks_resolve_icon — Find and load an appropriate icon for a file entry.
//
// We use a simple mapping from file extension to freedesktop icon name,
// then search the AquaKDE theme and system themes for a matching PNG.
// Directories always get the "folder" icon. Unknown types get a generic
// document icon.
//
// Returns true if an icon was loaded, false if we couldn't find one.
// ---------------------------------------------------------------------------
static bool stacks_resolve_icon(StackEntry *entry)
{
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    // Determine which icon name to look for based on the entry type.
    // Directories always use the "folder" icon. For files, we check
    // the extension and map it to a freedesktop icon name.
    const char *icon_name = NULL;

    if (entry->is_directory) {
        icon_name = "folder";
    } else {
        // Extract the file extension (everything after the last '.')
        const char *dot = strrchr(entry->name, '.');
        if (dot) {
            dot++;  // Skip past the '.' character

            // Map common file extensions to freedesktop icon names.
            // These icon names correspond to icons in the AquaKDE, Breeze,
            // and hicolor themes installed on the system.
            if (strcasecmp(dot, "pdf") == 0) {
                icon_name = "application-pdf";
            } else if (strcasecmp(dot, "png") == 0 ||
                       strcasecmp(dot, "jpg") == 0 ||
                       strcasecmp(dot, "jpeg") == 0 ||
                       strcasecmp(dot, "gif") == 0 ||
                       strcasecmp(dot, "bmp") == 0 ||
                       strcasecmp(dot, "svg") == 0 ||
                       strcasecmp(dot, "webp") == 0) {
                icon_name = "image-x-generic";
            } else if (strcasecmp(dot, "mp3") == 0 ||
                       strcasecmp(dot, "wav") == 0 ||
                       strcasecmp(dot, "flac") == 0 ||
                       strcasecmp(dot, "ogg") == 0 ||
                       strcasecmp(dot, "aac") == 0) {
                icon_name = "audio-x-generic";
            } else if (strcasecmp(dot, "mp4") == 0 ||
                       strcasecmp(dot, "mkv") == 0 ||
                       strcasecmp(dot, "avi") == 0 ||
                       strcasecmp(dot, "mov") == 0 ||
                       strcasecmp(dot, "webm") == 0) {
                icon_name = "video-x-generic";
            } else if (strcasecmp(dot, "txt") == 0 ||
                       strcasecmp(dot, "md") == 0 ||
                       strcasecmp(dot, "log") == 0) {
                icon_name = "text-plain";
            } else if (strcasecmp(dot, "zip") == 0 ||
                       strcasecmp(dot, "tar") == 0 ||
                       strcasecmp(dot, "gz") == 0 ||
                       strcasecmp(dot, "bz2") == 0 ||
                       strcasecmp(dot, "xz") == 0 ||
                       strcasecmp(dot, "7z") == 0 ||
                       strcasecmp(dot, "rar") == 0) {
                icon_name = "package-x-generic";
            } else if (strcasecmp(dot, "html") == 0 ||
                       strcasecmp(dot, "htm") == 0) {
                icon_name = "text-html";
            } else if (strcasecmp(dot, "c") == 0 ||
                       strcasecmp(dot, "h") == 0 ||
                       strcasecmp(dot, "cpp") == 0 ||
                       strcasecmp(dot, "py") == 0 ||
                       strcasecmp(dot, "js") == 0 ||
                       strcasecmp(dot, "sh") == 0) {
                icon_name = "text-x-script";
            } else if (strcasecmp(dot, "deb") == 0 ||
                       strcasecmp(dot, "rpm") == 0) {
                icon_name = "application-x-deb";
            } else if (strcasecmp(dot, "iso") == 0 ||
                       strcasecmp(dot, "img") == 0) {
                icon_name = "application-x-cd-image";
            }
        }

        // If we couldn't determine a specific icon, use a generic document
        if (!icon_name) {
            icon_name = "text-x-generic";
        }
    }

    // Search for the icon in various theme directories.
    // We try multiple sizes (largest first for best quality) and multiple
    // theme locations, following the same search order as config.c.
    static const int sizes[] = {48, 64, 128, 32};
    static const int size_count = 4;

    // Category directories to search — "mimetypes" for file type icons,
    // "places" for folder icons, and "apps" as a fallback
    static const char *categories[] = {"mimetypes", "places", "apps", "status", NULL};

    // Theme directories to search (same order as config.c's resolve_icon_path)
    static const char *themes[] = {
        "AquaKDE-icons", "hicolor", "breeze", "breeze-dark",
        "oxygen/base", "Adwaita", NULL
    };

    char path[1024];

    // Try each theme, category, and size combination
    for (const char **theme = themes; *theme; theme++) {
        for (const char **cat = categories; *cat; cat++) {
            for (int s = 0; s < size_count; s++) {
                // User-local themes (AquaKDE is usually here)
                snprintf(path, sizeof(path),
                         "%s/.local/share/icons/%s/%dx%d/%s/%s.png",
                         home, *theme, sizes[s], sizes[s], *cat, icon_name);
                if (access(path, R_OK) == 0) goto found;

                // System-wide themes
                snprintf(path, sizeof(path),
                         "/usr/share/icons/%s/%dx%d/%s/%s.png",
                         *theme, sizes[s], sizes[s], *cat, icon_name);
                if (access(path, R_OK) == 0) goto found;
            }
        }
    }

    // Last resort: try the generic pixmaps directory
    snprintf(path, sizeof(path), "/usr/share/pixmaps/%s.png", icon_name);
    if (access(path, R_OK) == 0) goto found;

    // No icon found — the entry will be drawn without one
    entry->icon = NULL;
    return false;

found:
    // Load the PNG file as a Cairo image surface
    entry->icon = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(entry->icon) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(entry->icon);
        entry->icon = NULL;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// entry_compare — Comparison function for qsort to sort entries alphabetically.
// Directories are sorted before files, then by name (case-insensitive).
// ---------------------------------------------------------------------------
static int entry_compare(const void *a, const void *b)
{
    const StackEntry *ea = (const StackEntry *)a;
    const StackEntry *eb = (const StackEntry *)b;

    // Directories come before files (like Finder)
    if (ea->is_directory && !eb->is_directory) return -1;
    if (!ea->is_directory && eb->is_directory) return  1;

    // Within the same type, sort alphabetically (case-insensitive)
    return strcasecmp(ea->name, eb->name);
}

// ---------------------------------------------------------------------------
// stacks_show — Open the stacks grid popup for a folder.
//
// This is the main entry point. It:
//   1. Scans the folder directory to build the entry list
//   2. Sorts entries (directories first, then alphabetical)
//   3. Loads an icon for each entry
//   4. Calculates the popup dimensions based on entry count
//   5. Creates an override-redirect ARGB window above the dock
//   6. Grabs the mouse pointer so we get click-outside events
//   7. Paints the popup
// ---------------------------------------------------------------------------
void stacks_show(DockState *state, DockItem *folder_item, double icon_center_x)
{
    // Close any existing popup first
    if (stacks.open) {
        stacks_close(state);
    }

    // -----------------------------------------------------------------------
    // Step 1: Scan the folder directory
    //
    // opendir/readdir is the standard POSIX way to list files in a directory.
    // We skip "." (current dir), ".." (parent dir), and hidden files
    // (names starting with ".") to match Finder's default behavior.
    // -----------------------------------------------------------------------
    DIR *dir = opendir(folder_item->folder_path);
    if (!dir) {
        fprintf(stderr, "[stacks] Cannot open folder: %s\n",
                folder_item->folder_path);
        return;
    }

    stacks.entry_count = 0;
    struct dirent *de;

    while ((de = readdir(dir)) != NULL && stacks.entry_count < STACK_MAX_ITEMS) {
        // Skip the current directory entry "." and parent ".."
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        // Skip hidden files (names starting with a dot, like .bashrc)
        if (de->d_name[0] == '.')
            continue;

        StackEntry *entry = &stacks.entries[stacks.entry_count];

        // Store the display name (just the filename, no path)
        snprintf(entry->name, sizeof(entry->name), "%s", de->d_name);

        // Build the full filesystem path for this entry
        snprintf(entry->path, sizeof(entry->path), "%s/%s",
                 folder_item->folder_path, de->d_name);

        // Use stat() to determine if this entry is a directory or a file.
        // stat() follows symlinks, so a symlink to a directory will show
        // as a directory (which is the expected behavior).
        struct stat st;
        if (stat(entry->path, &st) == 0) {
            entry->is_directory = S_ISDIR(st.st_mode);
        } else {
            entry->is_directory = false;
        }

        entry->icon = NULL;  // Will be loaded after sorting
        stacks.entry_count++;
    }
    closedir(dir);

    // If the folder is empty, there's nothing to show
    if (stacks.entry_count == 0) {
        fprintf(stderr, "[stacks] Folder is empty: %s\n",
                folder_item->folder_path);
        return;
    }

    // -----------------------------------------------------------------------
    // Step 2: Sort entries — directories first, then alphabetical
    // -----------------------------------------------------------------------
    qsort(stacks.entries, stacks.entry_count, sizeof(StackEntry), entry_compare);

    // -----------------------------------------------------------------------
    // Step 3: Load icons for each entry
    // -----------------------------------------------------------------------
    for (int i = 0; i < stacks.entry_count; i++) {
        stacks_resolve_icon(&stacks.entries[i]);
    }

    // -----------------------------------------------------------------------
    // Step 4: Calculate popup dimensions
    //
    // The grid has STACK_GRID_COLS columns and as many rows as needed.
    // The content area is surrounded by STACK_PADDING (inner) and
    // STACK_NINEPATCH_BORDER (the nine-patch edges). The callout arrow
    // adds STACK_CALLOUT_H pixels below the bottom edge.
    // -----------------------------------------------------------------------
    int rows = (stacks.entry_count + STACK_GRID_COLS - 1) / STACK_GRID_COLS;
    int content_w = STACK_GRID_COLS * STACK_CELL_W;
    int content_h = rows * STACK_CELL_H;

    stacks.win_w = content_w + 2 * STACK_PADDING + 2 * STACK_NINEPATCH_BORDER;
    stacks.win_h = content_h + 2 * STACK_PADDING + 2 * STACK_NINEPATCH_BORDER
                   + STACK_CALLOUT_H;

    // -----------------------------------------------------------------------
    // Step 5: Position the popup
    //
    // The popup is centered horizontally on the folder icon and positioned
    // so its bottom edge (including the callout arrow) sits just above the
    // dock window. The callout arrow points down at the folder icon.
    // -----------------------------------------------------------------------
    stacks.win_x = (int)(icon_center_x - stacks.win_w / 2.0);
    stacks.win_y = state->win_y - stacks.win_h + STACK_CALLOUT_H - 8;

    // Keep the popup on screen horizontally
    if (stacks.win_x < 0)
        stacks.win_x = 0;
    if (stacks.win_x + stacks.win_w > state->screen_w)
        stacks.win_x = state->screen_w - stacks.win_w;

    // Keep it on screen vertically (shouldn't happen unless screen is tiny)
    if (stacks.win_y < 0)
        stacks.win_y = 0;

    // Calculate the callout arrow's X position relative to the popup window.
    // It should point at the folder icon's center on screen.
    stacks.callout_x = icon_center_x - stacks.win_x;

    // -----------------------------------------------------------------------
    // Step 6: Create the override-redirect ARGB window
    //
    // Override-redirect tells the window manager not to add decorations
    // (title bar, borders) to this window. The 32-bit depth with the ARGB
    // visual lets us have per-pixel transparency for the rounded corners.
    // -----------------------------------------------------------------------
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;       // No WM decorations
    attrs.colormap = state->colormap;     // Use our ARGB colormap
    attrs.border_pixel = 0;               // No border
    attrs.background_pixel = 0;           // Transparent background

    stacks.win = XCreateWindow(
        state->dpy, state->root,
        stacks.win_x, stacks.win_y,
        stacks.win_w, stacks.win_h,
        0,                                // No border width
        32,                               // 32-bit depth for ARGB transparency
        InputOutput,
        state->visual,
        CWOverrideRedirect | CWColormap | CWBorderPixel | CWBackPixel,
        &attrs
    );

    // Tell X not to paint any default background — we handle all drawing
    XSetWindowBackgroundPixmap(state->dpy, stacks.win, None);

    // We need mouse events (hover, clicks) and key events (Escape to close)
    XSelectInput(state->dpy, stacks.win,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask | KeyPressMask | LeaveWindowMask);

    // Create the Cairo drawing surface bound to our X window
    stacks.surface = cairo_xlib_surface_create(
        state->dpy, stacks.win, state->visual,
        stacks.win_w, stacks.win_h);
    stacks.cr = cairo_create(stacks.surface);

    // Show the popup window on screen (raised above other windows)
    XMapRaised(state->dpy, stacks.win);

    // -----------------------------------------------------------------------
    // Step 7: Grab the pointer
    //
    // A pointer grab means ALL mouse events go to our window, even if the
    // mouse moves outside it. This is essential for detecting "click outside
    // to close" — without the grab, clicks outside would go to other windows
    // and we'd never know about them.
    // -----------------------------------------------------------------------
    XGrabPointer(state->dpy, stacks.win, True,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync,
                 None, None, CurrentTime);

    // Also grab the keyboard so we can catch Escape key presses
    XGrabKeyboard(state->dpy, stacks.win, True,
                  GrabModeAsync, GrabModeAsync, CurrentTime);

    stacks.hover_index = -1;
    stacks.open = true;

    // -----------------------------------------------------------------------
    // Step 8: Paint the popup contents
    // -----------------------------------------------------------------------
    stacks_paint();
    XFlush(state->dpy);
}

// ---------------------------------------------------------------------------
// stacks_paint — Redraw the entire popup contents.
//
// This is called whenever the popup needs to be redrawn (initial display,
// hover changes, etc.). It clears the window to transparent, draws the
// nine-patch background, then draws the grid of icons and labels.
// ---------------------------------------------------------------------------
static void stacks_paint(void)
{
    if (!stacks.cr) return;

    cairo_t *cr = stacks.cr;
    int w = stacks.win_w;
    int h = stacks.win_h;

    // Clear to fully transparent (OPERATOR_SOURCE replaces instead of blending)
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);

    // Switch back to normal alpha blending for all drawing
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // Draw the nine-patch background (dark semi-transparent with rounded corners)
    stacks_draw_ninepatch(cr, w, h);

    // Draw the grid of file icons and labels on top
    stacks_draw_grid(cr);

    // Flush drawing to the X window
    cairo_surface_flush(stacks.surface);
}

// ---------------------------------------------------------------------------
// stacks_draw_ninepatch — Draw the popup background using nine-patch assets.
//
// A nine-patch is a way to stretch a background image to any size:
//   - The 4 corners (40x40 each) are drawn at fixed size
//   - The 4 edges are tiled to fill the gaps between corners
//   - The center is tiled to fill the interior
//   - The callout arrow is drawn below the bottom edge, pointing at the icon
//
// If the nine-patch assets aren't loaded, we fall back to a solid dark
// semi-transparent rounded rectangle.
//
// Parameters:
//   cr — Cairo context to draw into
//   w  — total popup width
//   h  — total popup height (including callout area)
// ---------------------------------------------------------------------------
static void stacks_draw_ninepatch(cairo_t *cr, int w, int h)
{
    // The "body" of the popup is everything above the callout arrow.
    // The callout hangs below the body's bottom edge.
    int body_h = h - STACK_CALLOUT_H;

    // Check if we have the essential nine-patch pieces loaded
    bool have_np = (stacks.np_tl && stacks.np_t && stacks.np_tr &&
                    stacks.np_l && stacks.np_c && stacks.np_r &&
                    stacks.np_bl && stacks.np_b && stacks.np_br);

    if (!have_np) {
        // ---------------------------------------------------------------
        // Fallback: draw a solid dark semi-transparent rounded rectangle
        // if the nine-patch assets aren't available.
        // ---------------------------------------------------------------
        cairo_new_sub_path(cr);
        double r = 12.0;  // Corner radius for fallback
        cairo_arc(cr, w - r, r, r, -M_PI / 2, 0);
        cairo_arc(cr, w - r, body_h - r, r, 0, M_PI / 2);
        cairo_arc(cr, r, body_h - r, r, M_PI / 2, M_PI);
        cairo_arc(cr, r, r, r, M_PI, 3 * M_PI / 2);
        cairo_close_path(cr);

        cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 0.85);
        cairo_fill(cr);

        // Draw a simple triangular callout arrow as fallback
        double ax = stacks.callout_x;
        cairo_move_to(cr, ax - 15, body_h);
        cairo_line_to(cr, ax, body_h + 20);
        cairo_line_to(cr, ax + 15, body_h);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 0.85);
        cairo_fill(cr);

        return;
    }

    int b = STACK_NINEPATCH_BORDER;  // Shorthand for the 40px border size

    // -----------------------------------------------------------------------
    // Draw the four corners — these are fixed-size 40x40 images placed at
    // each corner of the popup body.
    // -----------------------------------------------------------------------

    // Top-left corner at (0, 0)
    cairo_set_source_surface(cr, stacks.np_tl, 0, 0);
    cairo_paint(cr);

    // Top-right corner at (w - 40, 0)
    cairo_set_source_surface(cr, stacks.np_tr, w - b, 0);
    cairo_paint(cr);

    // Bottom-left corner at (0, body_h - 40)
    cairo_set_source_surface(cr, stacks.np_bl, 0, body_h - b);
    cairo_paint(cr);

    // Bottom-right corner at (w - 40, body_h - 40)
    cairo_set_source_surface(cr, stacks.np_br, w - b, body_h - b);
    cairo_paint(cr);

    // -----------------------------------------------------------------------
    // Draw the four edges — these are 1-pixel-thick strips that get tiled
    // (repeated) to fill the space between corners.
    //
    // The trick with Cairo tiling:
    //   1. Set the source surface at position (0, 0)
    //   2. Set CAIRO_EXTEND_REPEAT so the 1px strip repeats infinitely
    //   3. Clip to a rectangle where we want the edge to appear
    //   4. Cairo fills the rectangle by repeating the source pattern
    // -----------------------------------------------------------------------

    // Top edge: tiles horizontally from x=40 to x=w-40, at y=0, height=40
    if (stacks.np_t) {
        cairo_save(cr);
        cairo_set_source_surface(cr, stacks.np_t, 0, 0);
        cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
        cairo_rectangle(cr, b, 0, w - 2 * b, b);
        cairo_fill(cr);
        cairo_restore(cr);
    }

    // Bottom edge: tiles horizontally from x=40 to x=w-40, at y=body_h-40
    if (stacks.np_b) {
        cairo_save(cr);
        cairo_set_source_surface(cr, stacks.np_b, 0, body_h - b);
        cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
        cairo_rectangle(cr, b, body_h - b, w - 2 * b, b);
        cairo_fill(cr);
        cairo_restore(cr);
    }

    // Left edge: tiles vertically from y=40 to y=body_h-40, at x=0, width=40
    if (stacks.np_l) {
        cairo_save(cr);
        cairo_set_source_surface(cr, stacks.np_l, 0, 0);
        cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
        cairo_rectangle(cr, 0, b, b, body_h - 2 * b);
        cairo_fill(cr);
        cairo_restore(cr);
    }

    // Right edge: tiles vertically from y=40 to y=body_h-40, at x=w-40
    if (stacks.np_r) {
        cairo_save(cr);
        cairo_set_source_surface(cr, stacks.np_r, w - b, 0);
        cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
        cairo_rectangle(cr, w - b, b, b, body_h - 2 * b);
        cairo_fill(cr);
        cairo_restore(cr);
    }

    // -----------------------------------------------------------------------
    // Fill the center — the 1x1 center tile gets repeated to fill the
    // entire interior area between all four edges.
    // -----------------------------------------------------------------------
    if (stacks.np_c) {
        cairo_save(cr);
        cairo_set_source_surface(cr, stacks.np_c, 0, 0);
        cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
        cairo_rectangle(cr, b, b, w - 2 * b, body_h - 2 * b);
        cairo_fill(cr);
        cairo_restore(cr);
    }

    // -----------------------------------------------------------------------
    // Draw the callout arrow — positioned below the bottom edge, centered
    // on callout_x. The arrow is 45x40 pixels and points downward toward
    // the folder icon in the dock.
    // -----------------------------------------------------------------------
    if (stacks.np_callout) {
        double arrow_x = stacks.callout_x - STACK_CALLOUT_W / 2.0;

        // Clamp the arrow so it doesn't go outside the popup bounds
        if (arrow_x < b) arrow_x = b;
        if (arrow_x + STACK_CALLOUT_W > w - b)
            arrow_x = w - b - STACK_CALLOUT_W;

        // Position the callout just below the body's bottom edge.
        // The +1 overlap ensures there's no gap between the body and arrow.
        cairo_set_source_surface(cr, stacks.np_callout,
                                 arrow_x, body_h - 1);
        cairo_paint(cr);
    }
}

// ---------------------------------------------------------------------------
// stacks_draw_grid — Draw all the file icons and labels in the grid.
//
// Each entry occupies one cell in the grid. The cell contains:
//   - A hover highlight (blue rounded rectangle, drawn behind the icon)
//   - The file icon (scaled to 48x48, centered at the top of the cell)
//   - A text label below the icon (Lucida Grande 10pt, white with black
//     shadow, truncated with ellipsis if too wide)
// ---------------------------------------------------------------------------
static void stacks_draw_grid(cairo_t *cr)
{
    // Create a Pango layout for rendering text labels.
    // Pango handles font selection, text shaping, and ellipsis truncation.
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_from_string(STACK_LABEL_FONT);
    pango_layout_set_font_description(layout, font);

    // Set the label width to the cell width so Pango can truncate with "..."
    pango_layout_set_width(layout, STACK_CELL_W * PANGO_SCALE);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

    for (int i = 0; i < stacks.entry_count; i++) {
        StackEntry *entry = &stacks.entries[i];

        // Calculate which row and column this entry falls in
        int col = i % STACK_GRID_COLS;
        int row = i / STACK_GRID_COLS;

        // Calculate the top-left corner of this cell.
        // The grid starts after the nine-patch border and inner padding.
        int cell_x = STACK_NINEPATCH_BORDER + STACK_PADDING + col * STACK_CELL_W;
        int cell_y = STACK_NINEPATCH_BORDER + STACK_PADDING + row * STACK_CELL_H;

        // Center the icon horizontally within the cell, with a small top margin
        int icon_x = cell_x + (STACK_CELL_W - STACK_ICON_SIZE) / 2;
        int icon_y = cell_y + 4;

        // Position the label below the icon
        int label_x = cell_x;
        int label_y = cell_y + STACK_ICON_SIZE + 6;

        // -------------------------------------------------------------------
        // Draw hover highlight — a rounded blue rectangle behind the icon
        // when the mouse is over this cell. Uses the macOS selection blue
        // (#3875D7) at 40% opacity for a subtle highlight effect.
        // -------------------------------------------------------------------
        if (stacks.hover_index == i) {
            cairo_save(cr);

            // Draw a rounded rectangle that covers the icon and label area
            double hx = cell_x + 2;
            double hy = cell_y + 2;
            double hw = STACK_CELL_W - 4;
            double hh = STACK_CELL_H - 4;
            double hr = 6.0;  // Corner radius

            cairo_new_sub_path(cr);
            cairo_arc(cr, hx + hw - hr, hy + hr, hr, -M_PI / 2, 0);
            cairo_arc(cr, hx + hw - hr, hy + hh - hr, hr, 0, M_PI / 2);
            cairo_arc(cr, hx + hr, hy + hh - hr, hr, M_PI / 2, M_PI);
            cairo_arc(cr, hx + hr, hy + hr, hr, M_PI, 3 * M_PI / 2);
            cairo_close_path(cr);

            // #3875D7 at 40% alpha — the classic macOS selection blue
            cairo_set_source_rgba(cr, 0.22, 0.46, 0.84, 0.40);
            cairo_fill(cr);

            cairo_restore(cr);
        }

        // -------------------------------------------------------------------
        // Draw the file icon (scaled to 48x48 pixels)
        // -------------------------------------------------------------------
        if (entry->icon) {
            cairo_save(cr);

            int src_w = cairo_image_surface_get_width(entry->icon);
            int src_h = cairo_image_surface_get_height(entry->icon);

            // Scale the icon to fit in STACK_ICON_SIZE x STACK_ICON_SIZE
            cairo_translate(cr, icon_x, icon_y);
            cairo_scale(cr, (double)STACK_ICON_SIZE / src_w,
                            (double)STACK_ICON_SIZE / src_h);
            cairo_set_source_surface(cr, entry->icon, 0, 0);

            // Bilinear filtering for smooth icon scaling
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);

            cairo_restore(cr);
        } else {
            // No icon loaded — draw a gray placeholder square so the user
            // can still see and click the entry
            cairo_save(cr);
            cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.5);
            cairo_rectangle(cr, icon_x, icon_y,
                            STACK_ICON_SIZE, STACK_ICON_SIZE);
            cairo_fill(cr);
            cairo_restore(cr);
        }

        // -------------------------------------------------------------------
        // Draw the label text below the icon
        //
        // We draw the text twice: first in black offset by 1px (shadow),
        // then in white on top. This gives the text a subtle drop shadow
        // that makes it readable against the dark background.
        // -------------------------------------------------------------------
        pango_layout_set_text(layout, entry->name, -1);

        // Shadow pass: offset 1px down and right, in black at 60% opacity
        cairo_save(cr);
        cairo_move_to(cr, label_x + 1, label_y + 1);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
        pango_cairo_show_layout(cr, layout);
        cairo_restore(cr);

        // Main pass: white text
        cairo_save(cr);
        cairo_move_to(cr, label_x, label_y);
        cairo_set_source_rgba(cr, 1, 1, 1, 1);
        pango_cairo_show_layout(cr, layout);
        cairo_restore(cr);
    }

    // Clean up Pango resources
    pango_font_description_free(font);
    g_object_unref(layout);
}

// ---------------------------------------------------------------------------
// stacks_hit_test — Determine which grid cell the mouse is over.
//
// Given mouse coordinates relative to the popup window, figure out which
// entry index the cursor is hovering over. Returns -1 if the mouse is
// outside the grid area or beyond the last entry.
//
// Parameters:
//   mx, my — mouse coordinates relative to the popup window's top-left
//
// Returns: entry index (0 to entry_count-1), or -1 if not over any entry
// ---------------------------------------------------------------------------
static int stacks_hit_test(int mx, int my)
{
    // Calculate the grid's top-left corner (after border and padding)
    int grid_x = STACK_NINEPATCH_BORDER + STACK_PADDING;
    int grid_y = STACK_NINEPATCH_BORDER + STACK_PADDING;

    // Check if the mouse is within the grid area at all
    if (mx < grid_x || my < grid_y) return -1;

    // Figure out which column and row the mouse is in
    int col = (mx - grid_x) / STACK_CELL_W;
    int row = (my - grid_y) / STACK_CELL_H;

    // Check bounds — make sure we're within the grid columns
    if (col < 0 || col >= STACK_GRID_COLS) return -1;

    // Convert row/col to a linear index
    int index = row * STACK_GRID_COLS + col;

    // Check that this index actually has an entry
    if (index < 0 || index >= stacks.entry_count) return -1;

    return index;
}

// ---------------------------------------------------------------------------
// stacks_open_entry — Open a file or directory using xdg-open.
//
// xdg-open is the standard way on Linux to open a file with its default
// application (like Finder's "Open" on macOS). We fork a child process
// to run it so the dock doesn't block waiting for the application.
// ---------------------------------------------------------------------------
static void stacks_open_entry(DockState *state, StackEntry *entry)
{
    (void)state;  // Not needed, but kept for future use

    // Fork a child process to run xdg-open.
    // fork() creates an exact copy of the current process. The child
    // (pid == 0) calls exec to replace itself with xdg-open. The parent
    // continues running the dock.
    pid_t pid = fork();
    if (pid == 0) {
        // Child process — replace with xdg-open
        // execlp searches PATH for the command, like typing it in a terminal
        execlp("xdg-open", "xdg-open", entry->path, (char *)NULL);

        // If exec fails (e.g., xdg-open not installed), exit the child
        _exit(127);
    } else if (pid < 0) {
        fprintf(stderr, "[stacks] Failed to fork for xdg-open\n");
    }
    // Parent process continues running — don't wait for the child
}

// ---------------------------------------------------------------------------
// stacks_close — Close the stacks popup and free all resources.
//
// This releases the pointer and keyboard grabs, destroys the X window,
// frees all entry icon surfaces, and resets the module state.
// ---------------------------------------------------------------------------
void stacks_close(DockState *state)
{
    if (!stacks.open) return;

    // Release the pointer and keyboard grabs so normal input resumes
    XUngrabPointer(state->dpy, CurrentTime);
    XUngrabKeyboard(state->dpy, CurrentTime);

    // Free the Cairo drawing context and surface
    if (stacks.cr) {
        cairo_destroy(stacks.cr);
        stacks.cr = NULL;
    }
    if (stacks.surface) {
        cairo_surface_destroy(stacks.surface);
        stacks.surface = NULL;
    }

    // Destroy the X window
    if (stacks.win) {
        XDestroyWindow(state->dpy, stacks.win);
        stacks.win = None;
    }

    // Free all entry icon surfaces to prevent memory leaks
    for (int i = 0; i < stacks.entry_count; i++) {
        if (stacks.entries[i].icon) {
            cairo_surface_destroy(stacks.entries[i].icon);
            stacks.entries[i].icon = NULL;
        }
    }
    stacks.entry_count = 0;

    stacks.hover_index = -1;
    stacks.open = false;

    XFlush(state->dpy);
}

// ---------------------------------------------------------------------------
// stacks_handle_event — Process X11 events for the stacks popup.
//
// This is called from the dock's main event loop. It handles:
//   - MotionNotify: update hover highlighting as the mouse moves
//   - ButtonPress: open the hovered item or close if clicked outside
//   - KeyPress: close on Escape
//   - LeaveNotify: clear hover when mouse leaves the popup
//
// Returns true if the event was consumed (so the dock doesn't also process it).
// ---------------------------------------------------------------------------
bool stacks_handle_event(DockState *state, XEvent *ev)
{
    if (!stacks.open) return false;

    switch (ev->type) {

    // ----- MOUSE MOTION: update hover highlighting -----
    case MotionNotify: {
        // Convert screen coordinates to window-local coordinates.
        // When we have a pointer grab, motion events report coordinates
        // relative to the grab window even if the mouse is outside it.
        int mx = ev->xmotion.x_root - stacks.win_x;
        int my = ev->xmotion.y_root - stacks.win_y;

        int old_hover = stacks.hover_index;
        stacks.hover_index = stacks_hit_test(mx, my);

        // Only repaint if the hover changed (avoid unnecessary redraws)
        if (stacks.hover_index != old_hover) {
            stacks_paint();
            XFlush(state->dpy);
        }
        return true;
    }

    // ----- MOUSE CLICK: open item or close popup -----
    case ButtonPress: {
        // Check if the click is inside the popup window
        int mx = ev->xbutton.x_root - stacks.win_x;
        int my = ev->xbutton.y_root - stacks.win_y;

        bool inside = (mx >= 0 && mx < stacks.win_w &&
                       my >= 0 && my < stacks.win_h);

        if (inside && ev->xbutton.button == 1) {
            // Left click inside the popup — check if it's on an entry
            int idx = stacks_hit_test(mx, my);
            if (idx >= 0) {
                // Open the clicked entry with xdg-open
                stacks_open_entry(state, &stacks.entries[idx]);
            }
            // Close the popup after any click inside
            stacks_close(state);
        } else {
            // Click outside the popup or non-left-click — just close
            stacks_close(state);
        }
        return true;
    }

    // ----- KEY PRESS: close on Escape -----
    case KeyPress: {
        // Look up which key was pressed using X11's keysym system
        KeySym key = XLookupKeysym(&ev->xkey, 0);
        if (key == XK_Escape) {
            stacks_close(state);
        }
        return true;
    }

    // ----- MOUSE LEAVE: clear hover when mouse exits the window -----
    case LeaveNotify: {
        if (stacks.hover_index != -1) {
            stacks.hover_index = -1;
            stacks_paint();
            XFlush(state->dpy);
        }
        return true;
    }

    // ----- EXPOSE: repaint when the window needs to be redrawn -----
    case Expose: {
        if (ev->xexpose.window == stacks.win && ev->xexpose.count == 0) {
            stacks_paint();
            XFlush(state->dpy);
        }
        return true;
    }

    default:
        break;
    }

    return false;
}

// ---------------------------------------------------------------------------
// stacks_is_open — Check whether a stacks popup is currently visible.
// Used by the dock's event loop to decide whether to route events here first.
// ---------------------------------------------------------------------------
bool stacks_is_open(void)
{
    return stacks.open;
}

// ---------------------------------------------------------------------------
// stacks_cleanup — Free all stacks module resources.
//
// Called during dock_cleanup() to release the nine-patch surfaces and
// any remaining entry icons. Safe to call even if assets were never loaded.
// ---------------------------------------------------------------------------
void stacks_cleanup(DockState *state)
{
    // Close the popup if it's still open
    if (stacks.open) {
        stacks_close(state);
    }

    // Free all nine-patch surfaces.
    // Helper macro: destroy a surface if it's not NULL, then set it to NULL.
    #define FREE_NP(field) do { \
        if (stacks.field) { \
            cairo_surface_destroy(stacks.field); \
            stacks.field = NULL; \
        } \
    } while (0)

    FREE_NP(np_tl);
    FREE_NP(np_t);
    FREE_NP(np_tr);
    FREE_NP(np_l);
    FREE_NP(np_c);
    FREE_NP(np_r);
    FREE_NP(np_bl);
    FREE_NP(np_b);
    FREE_NP(np_br);
    FREE_NP(np_callout);

    #undef FREE_NP
}
