// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// icons.c — Desktop icon grid manager
//
// This module manages the desktop icons that represent files and folders
// in ~/Desktop. It handles:
//
//   - Scanning ~/Desktop with opendir/readdir
//   - Loading appropriate icons from the AquaKDE icon theme
//   - Arranging icons in a grid from top-right, filling downward
//   - Rendering icons with labels (white text, drop shadow)
//   - Selection highlighting (blue rounded rectangle)
//   - Double-click to open with xdg-open
//   - Drag and drop to reposition icons
//   - Watching ~/Desktop with inotify for automatic refresh
//
// The grid layout mimics Finder from Mac OS X: icons start in the
// top-right corner and fill downward column by column, moving left.

#define _GNU_SOURCE  // For M_PI in math.h under strict C11

#include "icons.h"
#include "layout.h"
#include "labels.h"

#include <X11/Xlib.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

// ── Module state ────────────────────────────────────────────────────

// Maximum number of desktop icons we support.
// 512 icons is plenty for any reasonable desktop.
#define MAX_ICONS 512

// The array of all desktop icons
static DesktopIcon icons[MAX_ICONS];
static int icon_count = 0;

// inotify file descriptor and watch descriptor.
// inotify is a Linux kernel API that notifies us when files are
// created, deleted, or renamed in a watched directory.
static int inotify_fd = -1;
static int inotify_wd = -1;

// Debounce timer for inotify events.
// When files change rapidly (e.g., extracting a zip), we don't want
// to rescan ~/Desktop for every single event. Instead, we wait 200ms
// after the last event before rescanning.
static struct timespec last_inotify_event = {0};
static bool inotify_pending = false;
#define INOTIFY_DEBOUNCE_MS 200

// Drag state
static DesktopIcon *drag_icon = NULL;   // Icon being dragged
static int drag_offset_x = 0;          // Mouse offset from icon origin
static int drag_offset_y = 0;          // (so the icon doesn't jump to cursor)

// Cached X display pointer (needed for some operations)
static Display *cached_dpy = NULL;

// Cached screen dimensions — saved at icons_init() time so inotify
// rescan can call layout_apply() without needing the caller to pass them.
static int cached_screen_w = 0;
static int cached_screen_h = 0;

// ── .desktop file support ───────────────────────────────────────────

// Parsed fields from a freedesktop .desktop file.
// We only care about three keys from the [Desktop Entry] section:
//   Name= display name shown under the icon
//   Icon= theme icon name for icon resolution
//   Exec= command to run on double-click
typedef struct {
    char name[256];    // From Name= (empty string if not found)
    char icon[256];    // From Icon= (empty string if not found)
    char exec[1024];   // From Exec= with %f/%u/%F/%U stripped
} DesktopEntry;

// Parse a .desktop file and extract Name, Icon, and Exec fields.
// Only reads keys from the [Desktop Entry] section — stops if it hits
// another [section] header.  Returns true if at least one field was found.
static bool parse_desktop_file(const char *path, DesktopEntry *entry)
{
    memset(entry, 0, sizeof(*entry));

    FILE *fp = fopen(path, "r");
    if (!fp) return false;

    char line[1024];
    bool in_section = false;  // true once we've seen [Desktop Entry]

    while (fgets(line, sizeof(line), fp)) {
        // Strip trailing newline/carriage return
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        // Check for section headers
        if (line[0] == '[') {
            if (strncmp(line, "[Desktop Entry]", 15) == 0) {
                in_section = true;
            } else if (in_section) {
                // Hit a different section — stop parsing
                break;
            }
            continue;
        }

        if (!in_section) continue;

        // Parse key=value pairs
        if (strncmp(line, "Name=", 5) == 0 && entry->name[0] == '\0') {
            strncpy(entry->name, line + 5, sizeof(entry->name) - 1);
        } else if (strncmp(line, "Icon=", 5) == 0 && entry->icon[0] == '\0') {
            strncpy(entry->icon, line + 5, sizeof(entry->icon) - 1);
        } else if (strncmp(line, "Exec=", 5) == 0 && entry->exec[0] == '\0') {
            // Copy the command, stripping freedesktop field codes (%f %u %F %U etc.)
            // These are placeholders for file arguments we don't pass.
            const char *src = line + 5;
            char *dst = entry->exec;
            char *end = entry->exec + sizeof(entry->exec) - 1;
            while (*src && dst < end) {
                if (*src == '%' && src[1] && strchr("fFuUdDnNickvm", src[1])) {
                    src += 2;  // Skip the %X code
                    // Also skip trailing space after the code
                    if (*src == ' ') src++;
                } else {
                    *dst++ = *src++;
                }
            }
            *dst = '\0';
            // Trim trailing whitespace
            while (dst > entry->exec && (dst[-1] == ' ' || dst[-1] == '\t'))
                *--dst = '\0';
        }
    }

    fclose(fp);
    return (entry->name[0] || entry->icon[0] || entry->exec[0]);
}

// ── Icon resolution ─────────────────────────────────────────────────

// Maps file extensions to icon names in the theme.
// This is how we decide which icon to show for each file type.
typedef struct {
    const char *extension;    // File extension (e.g., ".pdf")
    const char *icon_name;    // Theme icon name (e.g., "application-pdf")
} ExtIconMap;

// Table of known extensions and their corresponding icon names.
// These names follow the freedesktop.org icon naming specification.
static const ExtIconMap ext_icon_map[] = {
    { ".pdf",  "application-pdf" },
    { ".txt",  "text-plain" },
    { ".text", "text-plain" },
    { ".md",   "text-x-generic" },
    { ".c",    "text-x-csrc" },
    { ".h",    "text-x-chdr" },
    { ".py",   "text-x-python" },
    { ".sh",   "text-x-script" },
    { ".png",  "image-png" },
    { ".jpg",  "image-jpeg" },
    { ".jpeg", "image-jpeg" },
    { ".gif",  "image-gif" },
    { ".svg",  "image-svg+xml" },
    { ".bmp",  "image-bmp" },
    { ".mp3",  "audio-mpeg" },
    { ".wav",  "audio-x-wav" },
    { ".flac", "audio-flac" },
    { ".mp4",  "video-mp4" },
    { ".mkv",  "video-x-matroska" },
    { ".avi",  "video-x-msvideo" },
    { ".zip",  "application-zip" },
    { ".tar",  "application-x-tar" },
    { ".gz",   "application-gzip" },
    { ".html", "text-html" },
    { ".css",  "text-css" },
    { ".js",   "application-javascript" },
    { ".json", "application-json" },
    { ".xml",  "application-xml" },
    { ".deb",  "application-x-deb" },
    { ".rpm",  "application-x-rpm" },
    { ".desktop", "application-x-desktop" },
    { NULL, NULL }  // Sentinel
};

// Create a generic fallback document icon using Cairo.
// This is a simple gray rectangle with a folded corner, used when
// no theme icon can be found for a file type.
static cairo_surface_t *create_fallback_icon(void)
{
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, ICON_SIZE, ICON_SIZE);
    cairo_t *cr = cairo_create(surface);

    // White document body with gray border
    double margin = 6;
    double fold = 14;  // Size of the folded corner
    double w = ICON_SIZE - margin * 2;
    double h = ICON_SIZE - margin * 2;

    // Draw the document shape with folded corner
    cairo_new_path(cr);
    cairo_move_to(cr, margin, margin);
    cairo_line_to(cr, margin + w - fold, margin);
    cairo_line_to(cr, margin + w, margin + fold);
    cairo_line_to(cr, margin + w, margin + h);
    cairo_line_to(cr, margin, margin + h);
    cairo_close_path(cr);

    // Fill with white and stroke with gray
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.6, 0.6, 0.6, 1.0);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    // Draw the fold triangle
    cairo_new_path(cr);
    cairo_move_to(cr, margin + w - fold, margin);
    cairo_line_to(cr, margin + w - fold, margin + fold);
    cairo_line_to(cr, margin + w, margin + fold);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0.85, 0.85, 0.85, 1.0);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.6, 0.6, 0.6, 1.0);
    cairo_stroke(cr);

    cairo_destroy(cr);
    return surface;
}

// Create a generic folder icon using Cairo.
// A simple folder shape with a tab on top-left.
static cairo_surface_t *create_fallback_folder_icon(void)
{
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, ICON_SIZE, ICON_SIZE);
    cairo_t *cr = cairo_create(surface);

    double m = 4;  // margin
    double w = ICON_SIZE - m * 2;
    double h = ICON_SIZE - m * 2;
    double tab_w = w * 0.35;  // Width of the folder tab
    double tab_h = h * 0.15;  // Height of the folder tab
    double r = 4;              // Corner radius

    // Draw folder body (rounded rectangle)
    double body_y = m + tab_h;
    cairo_new_sub_path(cr);
    cairo_arc(cr, m + r, body_y + r, r, M_PI, 1.5 * M_PI);
    cairo_arc(cr, m + w - r, body_y + r, r, 1.5 * M_PI, 2 * M_PI);
    cairo_arc(cr, m + w - r, m + h - r, r, 0, 0.5 * M_PI);
    cairo_arc(cr, m + r, m + h - r, r, 0.5 * M_PI, M_PI);
    cairo_close_path(cr);

    // Draw tab on top
    cairo_new_sub_path(cr);
    cairo_arc(cr, m + r, m + r, r, M_PI, 1.5 * M_PI);
    cairo_line_to(cr, m + tab_w - 4, m);
    cairo_line_to(cr, m + tab_w, body_y);
    cairo_line_to(cr, m, body_y);
    cairo_close_path(cr);

    // Fill with a blue tone (like macOS folders)
    cairo_set_source_rgba(cr, 0.45, 0.68, 0.95, 0.95);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.3, 0.5, 0.8, 1.0);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    cairo_destroy(cr);
    return surface;
}

// Try to load an icon from the theme search paths.
// Looks in the AquaKDE icon theme first, then hicolor fallback.
static cairo_surface_t *load_theme_icon(const char *icon_name,
                                         const char *subdir)
{
    const char *home = getenv("HOME");
    if (!home) return NULL;

    // Search paths in priority order — try 256x256 first (scaling down to
    // 128px looks better than scaling up from 64), then 128x128, then 64x64.
    char path[1024];
    const char *search_dirs[] = {
        "%s/.local/share/icons/AquaKDE-icons/256x256/%s/%s.png",
        "%s/.local/share/icons/hicolor/256x256/%s/%s.png",
        "/usr/share/icons/hicolor/256x256/%s/%s.png",
        "%s/.local/share/icons/AquaKDE-icons/128x128/%s/%s.png",
        "%s/.local/share/icons/hicolor/128x128/%s/%s.png",
        "/usr/share/icons/hicolor/128x128/%s/%s.png",
        "%s/.local/share/icons/AquaKDE-icons/64x64/%s/%s.png",
        "%s/.local/share/icons/hicolor/64x64/%s/%s.png",
        "/usr/share/icons/hicolor/64x64/%s/%s.png",
        NULL
    };

    for (int i = 0; search_dirs[i]; i++) {
        // The first format string uses home + subdir + icon_name.
        // The /usr/share path doesn't use home, so handle it specially.
        if (strncmp(search_dirs[i], "/usr", 4) == 0) {
            snprintf(path, sizeof(path), search_dirs[i], subdir, icon_name);
        } else {
            snprintf(path, sizeof(path), search_dirs[i], home, subdir, icon_name);
        }

        cairo_surface_t *surface = cairo_image_surface_create_from_png(path);
        if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
            return surface;
        }
        cairo_surface_destroy(surface);
    }

    return NULL;  // Not found in any search path
}

// Resolve the appropriate icon for a given file.
// Checks if it's a directory (uses folder icon), then .desktop files
// (uses Icon= field), then matches by file extension, and falls back
// to a generic document icon.
static cairo_surface_t *icons_resolve_icon(const char *path, bool is_dir)
{
    cairo_surface_t *icon = NULL;

    if (is_dir) {
        // Try to find a folder icon in the theme
        icon = load_theme_icon("folder", "places");
        if (!icon) icon = load_theme_icon("inode-directory", "places");
        if (!icon) icon = create_fallback_folder_icon();
        return icon;
    }

    // .desktop files get special treatment: use the Icon= field to find
    // the right theme icon instead of showing a generic script icon.
    const char *ext = strrchr(path, '.');
    if (ext && strcasecmp(ext, ".desktop") == 0) {
        DesktopEntry entry;
        if (parse_desktop_file(path, &entry) && entry.icon[0]) {
            // The Icon= value is a theme icon name — search all subdirs
            // since it could be in apps/, devices/, categories/, etc.
            const char *subdirs[] = { "apps", "devices", "categories",
                                      "mimetypes", "places", NULL };
            for (int i = 0; subdirs[i]; i++) {
                icon = load_theme_icon(entry.icon, subdirs[i]);
                if (icon) return icon;
            }
        }
        // If Icon= lookup failed, fall through to extension matching
    }

    // Try to match by file extension
    if (ext) {
        for (int i = 0; ext_icon_map[i].extension; i++) {
            if (strcasecmp(ext, ext_icon_map[i].extension) == 0) {
                icon = load_theme_icon(ext_icon_map[i].icon_name, "mimetypes");
                if (icon) return icon;
                break;
            }
        }
    }

    // Try generic text icon as a fallback
    icon = load_theme_icon("text-x-generic", "mimetypes");
    if (icon) return icon;

    // Last resort: create a generic document icon
    return create_fallback_icon();
}

// ── Scanning and layout ─────────────────────────────────────────────

// Comparison function for qsort — sorts filenames case-insensitively.
// This ensures icons appear in a predictable alphabetical order.
static int compare_icons(const void *a, const void *b)
{
    const DesktopIcon *ia = (const DesktopIcon *)a;
    const DesktopIcon *ib = (const DesktopIcon *)b;
    return strcasecmp(ia->name, ib->name);
}

// Scan ~/Desktop and populate the icons array.
// Skips hidden files (starting with '.') and special entries ('.' and '..').
static void scan_desktop(void)
{
    const char *home = getenv("HOME");
    if (!home) return;

    char desktop_path[1024];
    snprintf(desktop_path, sizeof(desktop_path), "%s/Desktop", home);

    DIR *dir = opendir(desktop_path);
    if (!dir) {
        fprintf(stderr, "[icons] Cannot open ~/Desktop: %s\n", desktop_path);
        return;
    }

    icon_count = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL && icon_count < MAX_ICONS) {
        // Skip hidden files and special directory entries
        if (entry->d_name[0] == '.') continue;

        DesktopIcon *icon = &icons[icon_count];
        memset(icon, 0, sizeof(DesktopIcon));

        // Build the full path first (needed for .desktop parsing below)
        snprintf(icon->path, sizeof(icon->path),
                 "%s/%s", desktop_path, entry->d_name);

        // For .desktop files, use the Name= field as the display name.
        // This hides the ".desktop" extension and shows a human-readable
        // label (e.g., "Controller Settings" instead of "Controller Settings.desktop").
        const char *dot = strrchr(entry->d_name, '.');
        if (dot && strcasecmp(dot, ".desktop") == 0) {
            DesktopEntry de;
            if (parse_desktop_file(icon->path, &de) && de.name[0]) {
                strncpy(icon->name, de.name, sizeof(icon->name) - 1);
            } else {
                // Fallback: filename without the .desktop extension
                size_t base_len = (size_t)(dot - entry->d_name);
                if (base_len >= sizeof(icon->name)) base_len = sizeof(icon->name) - 1;
                memcpy(icon->name, entry->d_name, base_len);
                icon->name[base_len] = '\0';
            }
        } else {
            // Regular file: use filename as-is
            strncpy(icon->name, entry->d_name, sizeof(icon->name) - 1);
        }

        // Check if it's a directory using stat()
        struct stat st;
        if (stat(icon->path, &st) == 0) {
            icon->is_directory = S_ISDIR(st.st_mode);
        }

        // Load the appropriate icon from the theme
        icon->icon = icons_resolve_icon(icon->path, icon->is_directory);

        // Read the color label from the file's xattr, if any.
        // This is the same per-file metadata that Snow Leopard stores in
        // com.apple.FinderInfo — we use our own key in the "user." namespace.
        icon->label = label_get(icon->path);

        icon_count++;
    }

    closedir(dir);

    // Sort alphabetically so the grid order is predictable
    if (icon_count > 1) {
        qsort(icons, icon_count, sizeof(DesktopIcon), compare_icons);
    }

    fprintf(stderr, "[icons] Found %d items in ~/Desktop\n", icon_count);
}

// Compute grid positions for all icons.
// Icons fill from top-right, going down first, then moving left.
// This matches the Finder layout from Mac OS X.
static void layout_icons(int screen_w, int screen_h)
{
    // How many rows fit on screen (accounting for the menubar area)
    int rows_per_col = (screen_h - ICON_TOP_MARGIN) / ICON_CELL_H;
    if (rows_per_col < 1) rows_per_col = 1;

    for (int i = 0; i < icon_count; i++) {
        // Compute grid column and row from the sequential index
        int col = i / rows_per_col;
        int row = i % rows_per_col;

        icons[i].grid_col = col;
        icons[i].grid_row = row;

        // Convert grid position to pixel coordinates.
        // X: starts from the right edge and moves left with each column.
        // Y: starts below the menubar area and moves down with each row.
        icons[i].x = screen_w - ICON_RIGHT_MARGIN - ICON_CELL_W - (col * ICON_CELL_W);
        icons[i].y = ICON_TOP_MARGIN + (row * ICON_CELL_H);
    }
}

// ── Public API: Init / Shutdown ─────────────────────────────────────

void icons_init(Display *dpy, int screen_w, int screen_h)
{
    cached_dpy    = dpy;
    cached_screen_w = screen_w;
    cached_screen_h = screen_h;

    // Scan ~/Desktop for files and folders
    scan_desktop();

    // Load the saved layout from disk, then apply it.
    // layout_load() fills the in-memory table; layout_apply() uses that
    // table to set each icon's grid_col/grid_row/x/y. Icons not found
    // in the table (new files) are auto-placed in the next free cell.
    layout_load();
    layout_apply(icons, icon_count, screen_w, screen_h);

    // Set up inotify to watch ~/Desktop for changes.
    // inotify is Linux-specific — on other systems, inotify_fd stays -1
    // and we just won't auto-refresh.
    const char *home = getenv("HOME");
    if (home) {
        char desktop_path[1024];
        snprintf(desktop_path, sizeof(desktop_path), "%s/Desktop", home);

        // IN_NONBLOCK makes reads non-blocking so we don't get stuck
        inotify_fd = inotify_init1(IN_NONBLOCK);
        if (inotify_fd >= 0) {
            // Watch for file creation, deletion, and renames
            inotify_wd = inotify_add_watch(inotify_fd, desktop_path,
                IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);

            if (inotify_wd < 0) {
                fprintf(stderr, "[icons] Warning: inotify_add_watch failed\n");
                close(inotify_fd);
                inotify_fd = -1;
            } else {
                fprintf(stderr, "[icons] Watching ~/Desktop for changes\n");
            }
        }
    }
}

void icons_shutdown(void)
{
    // Free all icon surfaces
    for (int i = 0; i < icon_count; i++) {
        if (icons[i].icon) {
            cairo_surface_destroy(icons[i].icon);
            icons[i].icon = NULL;
        }
    }
    icon_count = 0;

    // Clean up inotify
    if (inotify_wd >= 0 && inotify_fd >= 0) {
        inotify_rm_watch(inotify_fd, inotify_wd);
    }
    if (inotify_fd >= 0) {
        close(inotify_fd);
        inotify_fd = -1;
    }
}

// ── Public API: Painting ────────────────────────────────────────────

// Draw a rounded rectangle path (used for selection highlight).
// This is a helper that creates the path but doesn't fill or stroke it.
static void draw_rounded_rect(cairo_t *cr, double x, double y,
                               double w, double h, double r)
{
    // Four arcs at the corners connected by straight lines
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -M_PI / 2, 0);          // top-right
    cairo_arc(cr, x + w - r, y + h - r, r, 0,          M_PI / 2);   // bottom-right
    cairo_arc(cr, x + r,     y + h - r, r, M_PI / 2,   M_PI);       // bottom-left
    cairo_arc(cr, x + r,     y + r,     r, M_PI,        3 * M_PI / 2); // top-left
    cairo_close_path(cr);
}

void icons_paint(cairo_t *cr, int screen_w, int screen_h)
{
    (void)screen_w;
    (void)screen_h;

    for (int i = 0; i < icon_count; i++) {
        DesktopIcon *icon = &icons[i];

        // Center the icon image within the cell
        int img_x = icon->x + (ICON_CELL_W - ICON_SIZE) / 2;
        int img_y = icon->y + 2;  // Small top padding

        // Y position for the label text (4px below the icon image)
        int label_y = img_y + ICON_SIZE + 4;

        // ── Step 1: Build the Pango layout first ─────────────────────
        // We need the text height before drawing anything, because the
        // label color rect goes behind the text and must be sized to fit.

        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, icon->name, -1);

        // Set the font — real Snow Leopard uses Lucida Grande 12pt
        PangoFontDescription *font = pango_font_description_from_string(
            ICON_LABEL_FONT);
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);

        // Max width, centered, ellipsis for long names, word-char wrap
        pango_layout_set_width(layout, ICON_CELL_W * PANGO_SCALE);
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);

        // Measure the rendered text dimensions
        int text_w, text_h;
        pango_layout_get_pixel_size(layout, &text_w, &text_h);
        int text_x = icon->x + (ICON_CELL_W - text_w) / 2;

        // ── Step 2: Draw label color rect (behind text) ───────────────
        // Snow Leopard labels color the filename background, not the icon.
        // The colored rounded rect sits directly behind the text label.
        // When the icon is also selected, the blue highlight overlaps on
        // top because it's painted next (with transparency, so both show).
        if (icon->label > LABEL_NONE && icon->label < LABEL_COUNT) {
            const LabelColor *lc = &label_colors[icon->label];

            // Pad 4px around the text on each side
            double lrx = text_x - 4;
            double lry = label_y - 2;
            double lrw = text_w + 8;
            double lrh = text_h + 4;

            draw_rounded_rect(cr, lrx, lry, lrw, lrh, 4.0);
            // Alpha 200/255 — visible but lets the wallpaper hint through
            cairo_set_source_rgba(cr, lc->r, lc->g, lc->b, 200.0 / 255.0);
            cairo_fill(cr);
        }

        // ── Step 3: Selection highlight ───────────────────────────────
        // Semi-transparent blue rect over the whole icon cell.
        // Drawn AFTER the label rect so selection overlays it (blended).
        if (icon->selected) {
            draw_rounded_rect(cr,
                icon->x + 2, icon->y + 2,
                ICON_CELL_W - 4, ICON_CELL_H - 4,
                8.0);  // 8px corner radius

            // #3875D7 at alpha 160/255
            cairo_set_source_rgba(cr,
                0x38 / 255.0, 0x75 / 255.0, 0xD7 / 255.0,
                160.0 / 255.0);
            cairo_fill(cr);
        }

        // ── Step 4: Drop shadow ───────────────────────────────────────
        // Subtle shadow behind the icon image gives the Snow Leopard
        // "floating object" feel. Three passes: large/faint to small/solid.
        cairo_save(cr);
        for (int pass = 3; pass >= 1; pass--) {
            double alpha = 0.08 * pass;  // Stronger close to icon, fading outward
            int offset = pass + 1;
            cairo_set_source_rgba(cr, 0, 0, 0, alpha);
            // Rounded rect slightly larger than and below the icon
            double sx = img_x - pass;
            double sy = img_y - pass + offset;
            double sw = ICON_SIZE + pass * 2;
            double sh = ICON_SIZE + pass * 2;
            double r  = 8.0 + pass;
            cairo_new_sub_path(cr);
            cairo_arc(cr, sx + sw - r, sy + r,      r, -M_PI / 2, 0);
            cairo_arc(cr, sx + sw - r, sy + sh - r, r, 0,          M_PI / 2);
            cairo_arc(cr, sx + r,      sy + sh - r, r, M_PI / 2,   M_PI);
            cairo_arc(cr, sx + r,      sy + r,      r, M_PI,        3 * M_PI / 2);
            cairo_close_path(cr);
            cairo_fill(cr);
        }
        cairo_restore(cr);

        // ── Step 5: Icon image ────────────────────────────────────────
        if (icon->icon) {
            // Scale the icon to ICON_SIZE if it doesn't match
            int src_w = cairo_image_surface_get_width(icon->icon);
            int src_h = cairo_image_surface_get_height(icon->icon);

            if (src_w != ICON_SIZE || src_h != ICON_SIZE) {
                cairo_save(cr);
                double scx = (double)ICON_SIZE / src_w;
                double scy = (double)ICON_SIZE / src_h;
                cairo_translate(cr, img_x, img_y);
                cairo_scale(cr, scx, scy);
                cairo_set_source_surface(cr, icon->icon, 0, 0);
                cairo_paint(cr);
                cairo_restore(cr);
            } else {
                cairo_set_source_surface(cr, icon->icon, img_x, img_y);
                cairo_paint(cr);
            }
        }

        // ── Step 6: Label text with drop shadow ───────────────────────
        // Multiple passes create the dark halo that makes white text
        // readable on any wallpaper (light, dark, or busy).
        for (int dy = -1; dy <= 2; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0) continue;  // Center is the white text itself
                cairo_move_to(cr, text_x + dx, label_y + dy);
                cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
                pango_cairo_show_layout(cr, layout);
            }
        }

        // White text on top
        cairo_move_to(cr, text_x, label_y);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        pango_cairo_show_layout(cr, layout);

        g_object_unref(layout);
    }
}

// ── Public API: Interaction ─────────────────────────────────────────

DesktopIcon *icons_handle_click(int x, int y)
{
    // Check each icon to see if the click point is within its cell
    for (int i = 0; i < icon_count; i++) {
        if (x >= icons[i].x && x < icons[i].x + ICON_CELL_W &&
            y >= icons[i].y && y < icons[i].y + ICON_CELL_H) {
            return &icons[i];
        }
    }
    return NULL;  // Click was on empty space
}

void icons_handle_double_click(DesktopIcon *icon)
{
    if (!icon) return;

    fprintf(stderr, "[icons] Opening: %s\n", icon->path);

    // .desktop files need special handling — xdg-open doesn't reliably
    // execute them on a custom DE (no GNOME/KDE session to dispatch).
    // Parse the Exec= line and run the command directly.
    const char *ext = strrchr(icon->path, '.');
    if (ext && strcasecmp(ext, ".desktop") == 0) {
        DesktopEntry entry;
        if (parse_desktop_file(icon->path, &entry) && entry.exec[0]) {
            fprintf(stderr, "[icons] Exec from .desktop: %s\n", entry.exec);
            pid_t pid = fork();
            if (pid == 0) {
                setsid();
                // Use /bin/sh -c to handle commands with arguments.
                // execlp("sh") is safer than trying to tokenize Exec= ourselves.
                execl("/bin/sh", "sh", "-c", entry.exec, (char *)NULL);
                _exit(127);
            }
            return;
        }
        // If parsing failed, fall through to xdg-open
    }

    // For regular files: fork a child process to run xdg-open.
    // xdg-open is the standard Linux command to open a file with
    // its default application (like "open" on macOS).
    pid_t pid = fork();
    if (pid == 0) {
        // Child process: exec xdg-open
        // setsid() detaches from our process group so the child
        // survives if cc-desktop exits.
        setsid();
        execlp("xdg-open", "xdg-open", icon->path, NULL);
        _exit(127);  // Only reached if exec fails
    }
}

void icons_select(DesktopIcon *icon)
{
    // Deselect everything first (single-selection model)
    icons_deselect_all();

    if (icon) {
        icon->selected = true;
    }
}

void icons_deselect_all(void)
{
    for (int i = 0; i < icon_count; i++) {
        icons[i].selected = false;
    }
}

// ── Public API: inotify ─────────────────────────────────────────────

bool icons_check_inotify(void)
{
    if (inotify_fd < 0) return false;

    // Read all pending inotify events.
    // Each event has a variable-length name field, so we read into
    // a large buffer and parse events from it.
    char buf[4096]
        __attribute__((aligned(__alignof__(struct inotify_event))));

    ssize_t len = read(inotify_fd, buf, sizeof(buf));
    if (len <= 0) {
        // No events ready (EAGAIN) or error
        if (inotify_pending) {
            // Check if the debounce timer has expired
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long ms = (now.tv_sec - last_inotify_event.tv_sec) * 1000 +
                      (now.tv_nsec - last_inotify_event.tv_nsec) / 1000000;

            if (ms >= INOTIFY_DEBOUNCE_MS) {
                // Debounce period has passed — rescan ~/Desktop
                inotify_pending = false;

                // Free old icon surfaces before rescanning
                for (int i = 0; i < icon_count; i++) {
                    if (icons[i].icon) {
                        cairo_surface_destroy(icons[i].icon);
                        icons[i].icon = NULL;
                    }
                }

                // Rescan ~/Desktop for the current file list, then
                // restore saved positions. New files not in the saved
                // layout get auto-placed in the next free cell.
                // We use the cached screen dimensions saved at init time.
                scan_desktop();
                layout_apply(icons, icon_count,
                             cached_screen_w, cached_screen_h);
                return true;
            }
        }
        return false;
    }

    // We got events — record the time and set the pending flag.
    // We don't actually process the event contents because we're just
    // going to rescan the entire directory anyway.
    clock_gettime(CLOCK_MONOTONIC, &last_inotify_event);
    inotify_pending = true;

    return false;  // Not ready yet (still debouncing)
}

int icons_get_inotify_fd(void)
{
    return inotify_fd;
}

int icons_get_count(void)
{
    return icon_count;
}

// ── Public API: Drag and Drop ───────────────────────────────────────

void icons_drag_begin(DesktopIcon *icon, int x, int y)
{
    drag_icon = icon;
    // Record the offset from the icon's top-left corner to the mouse.
    // This way the icon moves smoothly from where you grabbed it,
    // instead of jumping so its corner is at the cursor.
    drag_offset_x = x - icon->x;
    drag_offset_y = y - icon->y;
}

void icons_drag_update(int x, int y)
{
    if (!drag_icon) return;

    // Move the icon to follow the cursor, maintaining the grab offset
    drag_icon->x = x - drag_offset_x;
    drag_icon->y = y - drag_offset_y;
}

void icons_drag_end(int screen_w, int screen_h)
{
    if (!drag_icon) return;

    // Snap the icon to the nearest grid cell.
    // We compute which grid cell the icon's center is closest to,
    // then check if that cell is free.

    int rows_per_col = (screen_h - ICON_TOP_MARGIN) / ICON_CELL_H;
    if (rows_per_col < 1) rows_per_col = 1;

    // Find the grid cell closest to the icon's current position
    int center_x = drag_icon->x + ICON_CELL_W / 2;
    int center_y = drag_icon->y + ICON_CELL_H / 2;

    // Convert pixel position to grid coordinates.
    // X axis: measured from the right edge, moving left
    int col = (screen_w - ICON_RIGHT_MARGIN - center_x) / ICON_CELL_W;
    int row = (center_y - ICON_TOP_MARGIN) / ICON_CELL_H;

    // Clamp to valid range
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    if (row >= rows_per_col) row = rows_per_col - 1;

    // Check if the target cell is occupied by another icon
    bool occupied = false;
    for (int i = 0; i < icon_count; i++) {
        if (&icons[i] == drag_icon) continue;
        if (icons[i].grid_col == col && icons[i].grid_row == row) {
            occupied = true;
            break;
        }
    }

    if (!occupied) {
        // Snap to the target cell
        drag_icon->grid_col = col;
        drag_icon->grid_row = row;
    }
    // If occupied, keep the old grid position (the icon will snap back)

    // Compute pixel position from grid coordinates
    drag_icon->x = screen_w - ICON_RIGHT_MARGIN - ICON_CELL_W -
                   (drag_icon->grid_col * ICON_CELL_W);
    drag_icon->y = ICON_TOP_MARGIN + (drag_icon->grid_row * ICON_CELL_H);

    drag_icon = NULL;

    // Persist the new layout to disk so this position survives restart.
    // We save all icons (not just the moved one) because a single file
    // write is simpler and the file is tiny.
    layout_save_all(icons, icon_count);
}

// ── Public API: Relayout ────────────────────────────────────────────

void icons_relayout(int screen_w, int screen_h)
{
    // "Clean Up": reset all icons to canonical sorted positions (top-right).
    // This is the same as a fresh auto-layout with no saved positions.
    layout_icons(screen_w, screen_h);
    fprintf(stderr, "[icons] Relayout: %d icons arranged\n", icon_count);

    // Save the new sorted positions so they persist across restarts.
    // After Clean Up, the user's explicit positions are gone — the sorted
    // grid becomes the new baseline.
    layout_save_all(icons, icon_count);
}
