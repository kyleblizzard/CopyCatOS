// CopyCatOS — by Kyle Blizzard at Blizzard.show

// icons.c — Desktop icon grid manager
//
// This module manages the desktop icons that represent files and folders
// in ~/Desktop. It handles:
//
//   - Scanning ~/Desktop with opendir/readdir
//   - Loading appropriate icons from the Aqua icon theme
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
#include "desktop.h"  // desktop_hidpi_scale, S(), SF()
#include "layout.h"
#include "labels.h"

#include <X11/Xlib.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
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
// Looks in the Aqua icon theme first, then hicolor fallback.
static cairo_surface_t *load_theme_icon(const char *icon_name,
                                         const char *subdir)
{
    const char *home = getenv("HOME");
    if (!home) return NULL;

    // Search paths in priority order — try 256x256 first (scaling down to
    // 128px looks better than scaling up from 64), then 128x128, then 64x64.
    char path[1024];
    const char *search_dirs[] = {
        "%s/.local/share/icons/Aqua/256x256/%s/%s.png",
        "%s/.local/share/icons/hicolor/256x256/%s/%s.png",
        "/usr/share/icons/hicolor/256x256/%s/%s.png",
        "%s/.local/share/icons/Aqua/128x128/%s/%s.png",
        "%s/.local/share/icons/hicolor/128x128/%s/%s.png",
        "/usr/share/icons/hicolor/128x128/%s/%s.png",
        "%s/.local/share/icons/Aqua/64x64/%s/%s.png",
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
    // Cache the scaled cell geometry once per call. ICON_* constants are
    // points at the 1.0x baseline; S() folds in the current HiDPI scale
    // so the grid math below is in physical pixels from here on.
    int cell_w = S(ICON_CELL_W);
    int cell_h = S(ICON_CELL_H);
    int top    = S(ICON_TOP_MARGIN);
    int right  = S(ICON_RIGHT_MARGIN);

    // How many rows fit on screen (accounting for the menubar area)
    int rows_per_col = (screen_h - top) / cell_h;
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
        icons[i].x = screen_w - right - cell_w - (col * cell_w);
        icons[i].y = top + (row * cell_h);
    }
}

// ── Public API: Init / Shutdown ─────────────────────────────────────

void icons_init(Display *dpy, int screen_w, int screen_h)
{
    cached_dpy    = dpy;
    cached_screen_w = screen_w;
    cached_screen_h = screen_h;

    // One-shot cleanup of the legacy central-index file. The pre-spatial
    // build wrote ~/.local/share/copycatos/desktop-layout.ini on every drag;
    // free-form positions now live in per-file user.moonbase.position xattrs
    // (see layout.c) so the .ini is dead weight. Best-effort unlink — if it
    // never existed or already vanished, ENOENT is fine.
    const char *xdg_data = getenv("XDG_DATA_HOME");
    char legacy_ini[1024];
    if (xdg_data && xdg_data[0]) {
        snprintf(legacy_ini, sizeof(legacy_ini),
                 "%s/copycatos/desktop-layout.ini", xdg_data);
    } else {
        const char *h = getenv("HOME");
        snprintf(legacy_ini, sizeof(legacy_ini),
                 "%s/.local/share/copycatos/desktop-layout.ini",
                 h ? h : "");
    }
    if (unlink(legacy_ini) == 0) {
        fprintf(stderr, "[icons] Removed legacy %s\n", legacy_ini);
    }

    // Scan ~/Desktop for files and folders
    scan_desktop();

    // Apply the spatial layout. layout_apply reads each file's
    // user.moonbase.position xattr; files with no xattr (new files,
    // first run) auto-place into the next free Snow Leopard grid cell.
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

// Build a Pango font description string with the base point size multiplied
// by desktop_hidpi_scale. At 1.0x this is "Lucida Grande 12"; at 1.75x it
// becomes "Lucida Grande 21" so icon labels remain visually proportional
// to the scaled cell on dense displays. Returned pointer is a static
// buffer — only valid until the next call.
static const char *icon_scaled_font(int base_size)
{
    static char buf[96];
    int scaled = (int)(base_size * desktop_hidpi_scale + 0.5);
    if (scaled < base_size) scaled = base_size;
    snprintf(buf, sizeof(buf), "Lucida Grande %d", scaled);
    return buf;
}

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

    // Scaled cell / icon geometry — computed once per paint. These mirror
    // the ICON_* constants folded through S(), so the paint code below
    // stays in physical pixels without threading the scale through every
    // call site.
    int cell_w  = S(ICON_CELL_W);
    int cell_h  = S(ICON_CELL_H);
    int icon_px = S(ICON_SIZE);

    for (int i = 0; i < icon_count; i++) {
        DesktopIcon *icon = &icons[i];

        // Center the icon image within the cell
        int img_x = icon->x + (cell_w - icon_px) / 2;
        int img_y = icon->y + S(2);  // Small top padding

        // Y position for the label text (4pt below the icon image)
        int label_y = img_y + icon_px + S(4);

        // ── Step 1: Build the Pango layout first ─────────────────────
        // We need the text height before drawing anything, because the
        // label color rect goes behind the text and must be sized to fit.

        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, icon->name, -1);

        // Set the font — real Snow Leopard uses Lucida Grande 12pt at the
        // 1.0x baseline; icon_scaled_font() grows the point size with the
        // output's HiDPI scale so labels stay proportional to the cell.
        PangoFontDescription *font = pango_font_description_from_string(
            icon_scaled_font(12));
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);

        // Max width, centered, word-char wrap. Snow Leopard caps the
        // filename at TWO lines and only ellipsizes if the second line
        // also overflows — pango_layout_set_height with a NEGATIVE value
        // means "limit to N lines, ellipsize after that", so -2 is the
        // exact two-line behavior.
        pango_layout_set_width(layout, cell_w * PANGO_SCALE);
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_height(layout, -2 * PANGO_SCALE);

        // Measure the rendered text dimensions. Pango ALIGN_CENTER +
        // set_width(cell_w) does the horizontal centering for us when the
        // layout is shown — the cairo_move_to origin must be the cell's
        // left edge, NOT cell_w/2 - text_w/2 (that double-centers).
        // text_visible_x is only used for sizing the colored label pill,
        // since the pill must hug the actual rendered text rectangle.
        int text_w, text_h;
        pango_layout_get_pixel_size(layout, &text_w, &text_h);
        int text_visible_x = icon->x + (cell_w - text_w) / 2;

        // Snow Leopard parity: while an icon is being dragged, render the
        // entire icon cell (image + label pill + filename) at ~50% alpha
        // so the user can see the wallpaper / underlying icons through
        // the moving glyph. We push a group, paint normally below, then
        // pop_group_to_source + paint_with_alpha so every layer of this
        // cell composites together at one consistent opacity.
        bool dragging_this = (icon == drag_icon);
        if (dragging_this) cairo_push_group(cr);

        // ── Step 2: Draw label color rect (behind text) ───────────────
        // Snow Leopard labels color the filename background, not the icon.
        // The colored rounded rect sits directly behind the text label.
        // When the icon is also selected, the blue highlight overlaps on
        // top because it's painted next (with transparency, so both show).
        // Snow Leopard's filename pill (label color OR selection blue) is
        // capsule-rounded — half the pill height for the corner radius
        // makes the ends true semicircles. Start with a base 7pt pad on
        // each side, then widen the entire pill by 20% around its center
        // so the capsule has noticeably more breathing room than the
        // text rectangle (matches the SL look more closely than a tight
        // hug). When the filename word-wraps to two lines the pill grows
        // taller automatically because pill_h tracks text_h.
        double base_pill_x = text_visible_x - SF(7);
        double base_pill_w = text_w + SF(14);
        double extra_w     = base_pill_w * 0.20;
        double pill_x = base_pill_x - extra_w / 2.0;
        double pill_y = label_y - SF(2);
        double pill_w = base_pill_w + extra_w;
        double pill_h = text_h + SF(4);
        double pill_r = pill_h / 2.0;

        if (icon->label > LABEL_NONE && icon->label < LABEL_COUNT) {
            const LabelColor *lc = &label_colors[icon->label];
            draw_rounded_rect(cr, pill_x, pill_y, pill_w, pill_h, pill_r);
            // Alpha 200/255 — visible but lets the wallpaper hint through
            cairo_set_source_rgba(cr, lc->r, lc->g, lc->b, 200.0 / 255.0);
            cairo_fill(cr);
        }

        // ── Step 3: Selection highlight ───────────────────────────────
        // Snow Leopard parity: selection paints a solid blue capsule
        // BEHIND THE FILENAME ONLY — never a box around the icon image.
        // The icon image remains untouched (no tint, no halo); the
        // filename pill is the entire visual selection cue. Same
        // geometry as the color-label pill so a labelled+selected icon
        // transitions cleanly (the blue pill replaces the label pill
        // while selected — color label still wins when not selected).
        if (icon->selected) {
            draw_rounded_rect(cr, pill_x, pill_y, pill_w, pill_h, pill_r);
            // SL selection blue #3875D7, fully opaque so the white
            // filename pops cleanly against it.
            cairo_set_source_rgba(cr,
                0x38 / 255.0, 0x75 / 255.0, 0xD7 / 255.0, 1.0);
            cairo_fill(cr);
        }

        // ── Step 4: Icon image ────────────────────────────────────────
        // Snow Leopard icons paint directly on the wallpaper with no
        // bounding rectangle behind them — soft edges and any drop
        // shadow come from the .icns PNG's own alpha channel. We don't
        // synthesize a manual shadow rect; that creates the visible
        // dark box Snow Leopard never had.
        if (icon->icon) {
            // Source surfaces are loaded at their PNG's original size; scale
            // per-paint to icon_px (the scaled target). cairo_scale collapses
            // to identity when src_w == icon_px and scale == 1.0, so one
            // branch handles every case — matches the dock's approach.
            int src_w = cairo_image_surface_get_width(icon->icon);
            int src_h = cairo_image_surface_get_height(icon->icon);

            cairo_save(cr);
            double scx = (double)icon_px / src_w;
            double scy = (double)icon_px / src_h;
            cairo_translate(cr, img_x, img_y);
            cairo_scale(cr, scx, scy);
            cairo_set_source_surface(cr, icon->icon, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
        }

        // ── Step 5: Label text with drop shadow ───────────────────────
        // The cairo origin is the cell's left edge; Pango ALIGN_CENTER
        // + set_width(cell_w) handles horizontal centering inside the
        // layout, so we move_to icon->x (NOT text_visible_x — that
        // would double-center and shift the label off to the right).
        // Multiple passes create the dark halo that makes white text
        // readable on any wallpaper (light, dark, or busy). Offsets are
        // scaled so the halo stays proportional to the rendered text on
        // HiDPI outputs.
        int halo = S(1);
        if (halo < 1) halo = 1;
        for (int dy = -halo; dy <= halo * 2; dy += halo) {
            for (int dx = -halo; dx <= halo; dx += halo) {
                if (dx == 0 && dy == 0) continue;  // Center is the white text itself
                cairo_move_to(cr, icon->x + dx, label_y + dy);
                cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
                pango_cairo_show_layout(cr, layout);
            }
        }

        // White text on top
        cairo_move_to(cr, icon->x, label_y);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        pango_cairo_show_layout(cr, layout);

        if (dragging_this) {
            cairo_pop_group_to_source(cr);
            cairo_paint_with_alpha(cr, 0.5);
        }

        g_object_unref(layout);
    }
}

// ── Public API: Interaction ─────────────────────────────────────────

DesktopIcon *icons_handle_click(int x, int y)
{
    // Cell size in physical pixels at the current scale — hit-test uses the
    // same scaled geometry that paint/layout use.
    int cell_w = S(ICON_CELL_W);
    int cell_h = S(ICON_CELL_H);

    // Check each icon to see if the click point is within its cell
    for (int i = 0; i < icon_count; i++) {
        if (x >= icons[i].x && x < icons[i].x + cell_w &&
            y >= icons[i].y && y < icons[i].y + cell_h) {
            return &icons[i];
        }
    }
    return NULL;  // Click was on empty space
}

// A MoonBase bundle is a directory whose name ends in ".app" (shipping;
// single-file ELF-stub form lands in its own slice — until then .app is
// still a directory) or ".appdev" (developer directory). Desktop icons
// that point at bundles are handed to moonbase-launch so the bwrap
// sandbox + consent flow run. Name kept as path_is_appc_bundle because
// the metadata file it checks for is Info.appc, not a reference to the
// retired .appc bundle suffix.
static int path_is_appc_bundle(const char *path)
{
    if (!path) return 0;
    size_t len = strlen(path);
    int is_app    = (len >= 4 && strcmp(path + len - 4, ".app")    == 0);
    int is_appdev = (len >= 7 && strcmp(path + len - 7, ".appdev") == 0);
    if (!is_app && !is_appdev) return 0;

    char info[1024];
    int n = snprintf(info, sizeof(info), "%s/Contents/Info.appc", path);
    if (n < 0 || (size_t)n >= sizeof(info)) return 0;

    struct stat st;
    return stat(info, &st) == 0 && S_ISREG(st.st_mode);
}

void icons_handle_double_click(DesktopIcon *icon)
{
    if (!icon) return;

    fprintf(stderr, "[icons] Opening: %s\n", icon->path);

    // MoonBase .app bundles go through moonbase-launch so the sandbox,
    // entitlements, and consent flow all run.
    if (path_is_appc_bundle(icon->path)) {
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            execlp("moonbase-launch", "moonbase-launch",
                   icon->path, (char *)NULL);
            _exit(127);
        }
        return;
    }

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
        // survives if desktop exits.
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

    // Free-form drop: no grid snap. The icon stays at exactly the pixel
    // the user released the mouse — that's the spatial-workspace promise.
    // We only clamp so the icon doesn't end up entirely off-screen, where
    // the user couldn't grab it again.
    int cell_w = S(ICON_CELL_W);
    int cell_h = S(ICON_CELL_H);
    int top    = S(ICON_TOP_MARGIN);

    int min_x = 0;
    int max_x = screen_w - cell_w;
    int min_y = top;                 // Don't overlap the menubar workarea
    int max_y = screen_h - cell_h;
    if (max_x < min_x) max_x = min_x;
    if (max_y < min_y) max_y = min_y;

    if (drag_icon->x < min_x) drag_icon->x = min_x;
    if (drag_icon->x > max_x) drag_icon->x = max_x;
    if (drag_icon->y < min_y) drag_icon->y = min_y;
    if (drag_icon->y > max_y) drag_icon->y = max_y;

    // Tag the icon as user-positioned so a later layout_apply (e.g. after
    // an inotify rescan or a scale change) restores its free-form spot
    // instead of stealing it for the auto-place pass.
    drag_icon->has_pos = true;

    // Persist this single icon's pixel position to its xattr in points.
    // Per-file storage means renames and moves carry the position with
    // the file; nothing else on disk needs to know the icon moved.
    layout_save_position(drag_icon);

    drag_icon = NULL;
}

// ── Public API: Rescale ─────────────────────────────────────────────

void icons_rescale(void)
{
    // A scale change doesn't change which files are on the desktop — only
    // the pixel math. Re-run layout_apply against the cached screen size
    // so every icon's (x, y) is recomputed from the current S()-scaled
    // ICON_* constants. The xattr stores points, so free-form positions
    // re-resolve to the right pixels at the new scale automatically.
    layout_apply(icons, icon_count, cached_screen_w, cached_screen_h);
}

// ── Public API: Relayout ────────────────────────────────────────────

void icons_relayout(int screen_w, int screen_h)
{
    // "Clean Up": revert every icon to canonical auto-place. We clear
    // each file's position xattr and rerun the auto-place pass — that
    // way any new file added later auto-places into the same grid the
    // user just snapped back to, instead of fighting a saved free-form
    // ghost from before.
    for (int i = 0; i < icon_count; i++) {
        layout_clear_position(&icons[i]);
        icons[i].has_pos = false;
    }
    layout_icons(screen_w, screen_h);
    fprintf(stderr, "[icons] Relayout: %d icons arranged\n", icon_count);
}
