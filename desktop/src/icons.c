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
#include <X11/Xutil.h>           // XVisualInfo, XMatchVisualInfo (ghost popup)
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>    // cairo_xlib_surface_create (ghost paint)
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

// Z-order machinery for stacked free-form icons.
//
// Each icon carries a z_order value (higher = visually on top). The icons
// array stays in alphabetical order — auto-place / clean-up depend on a
// stable iteration order — so we keep a separate paint_order[] index that
// is sorted by z_order. Paint walks paint_order ascending (top-of-stack is
// last). Hit-test walks it descending (top-of-stack wins). next_z is the
// next unused stacking value: incremented and assigned every time an icon
// is clicked, so a click on a buried icon raises it above its neighbors.
//
// next_z is ephemeral. On relaunch icons get sequential z values from
// scan_desktop (alphabetical = initial paint order), which matches the
// classic Snow Leopard look at first paint.
static int paint_order[MAX_ICONS];
static int paint_order_count = 0;
static int next_z = 0;

static int compare_by_z(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    int za = icons[ia].z_order;
    int zb = icons[ib].z_order;
    if (za != zb) return za - zb;
    // Tiebreak on array index for total ordering — only matters if two
    // icons somehow share a z_order (shouldn't happen at runtime, but
    // qsort needs deterministic behavior for equal keys).
    return ia - ib;
}

static void rebuild_paint_order(void)
{
    paint_order_count = icon_count;
    for (int i = 0; i < icon_count; i++) paint_order[i] = i;
    if (paint_order_count > 1) {
        qsort(paint_order, paint_order_count, sizeof(int), compare_by_z);
    }
}

// Promote an icon to the top of the stacking order. Called on every click
// that selects an icon (single-click) so a click on a buried icon surfaces
// it — the spatial-workspace promise is "click what you see", and that
// requires the clicked icon to also become the painted-on-top icon for
// the next paint pass.
static void icon_promote_z(DesktopIcon *icon)
{
    if (!icon) return;
    icon->z_order = ++next_z;
    rebuild_paint_order();
}

// Build a perimeter hit-mask for an icon's source surface.
//
// The rule we want: clicks inside the icon's outer perimeter — even on
// fully-transparent interior pixels (e.g. a donut's hole, the punched
// counter inside an "O") — count as hits on this icon. Clicks on the
// transparent space outside the perimeter fall through to the icon
// underneath. Per-pixel alpha alone can't tell those two kinds of
// transparency apart, so we precompute a mask that does.
//
// Algorithm: 4-connected flood fill from the four corners. Any
// transparent pixel reachable from a corner is exterior (mask = 0).
// Every other pixel — opaque pixels and transparent pixels enclosed by
// opaque ones — is interior (mask = 1). Threshold 32/255 keeps
// anti-aliased perimeter edges as opaque so the fill stops there.
//
// One mask per icon, computed at scan time, freed on rescan / shutdown.
// Uses one byte per source pixel — at 256×256 that's 64 KiB per icon,
// which is fine at MAX_ICONS=512 worst case (32 MiB) but most icons are
// smaller.
static uint8_t *build_hit_mask(cairo_surface_t *surface,
                                int *out_w, int *out_h)
{
    if (!surface || !out_w || !out_h) return NULL;
    if (cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE) return NULL;

    cairo_format_t fmt = cairo_image_surface_get_format(surface);
    if (fmt != CAIRO_FORMAT_ARGB32 && fmt != CAIRO_FORMAT_RGB24) return NULL;

    int w = cairo_image_surface_get_width(surface);
    int h = cairo_image_surface_get_height(surface);
    if (w <= 0 || h <= 0) return NULL;

    cairo_surface_flush(surface);
    unsigned char *data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    if (!data || stride <= 0) return NULL;

    // Step 1: seed mask = 1 on opaque, 0 on transparent. RGB24 has no
    // alpha channel, so every pixel counts as opaque (no fill propagates
    // and the whole rect is "inside") — fine, since RGB24 icons never
    // appear in the Aqua theme but the fallback path can hit it.
    size_t cells = (size_t)w * (size_t)h;
    uint8_t *mask = (uint8_t *)malloc(cells);
    if (!mask) return NULL;

    if (fmt == CAIRO_FORMAT_RGB24) {
        memset(mask, 1, cells);
        *out_w = w; *out_h = h;
        return mask;
    }

    for (int y = 0; y < h; y++) {
        unsigned char *row = data + y * stride;
        uint8_t *mrow = mask + y * w;
        for (int x = 0; x < w; x++) {
            mrow[x] = (row[x * 4 + 3] >= 32) ? 1 : 0;
        }
    }

    // Step 2: flood fill from corners. Stack-based so we don't blow the
    // call stack on a large icon. Cells touched go from 0 to 2 (sentinel
    // for "exterior transparent").
    int *stack = (int *)malloc(cells * sizeof(int));
    if (!stack) {
        free(mask);
        return NULL;
    }
    int sp = 0;
    int corners[4][2] = {{0, 0}, {w - 1, 0}, {0, h - 1}, {w - 1, h - 1}};
    for (int c = 0; c < 4; c++) {
        int cx = corners[c][0], cy = corners[c][1];
        int idx = cy * w + cx;
        if (mask[idx] == 0) {
            mask[idx] = 2;
            stack[sp++] = idx;
        }
    }

    while (sp > 0) {
        int idx = stack[--sp];
        int x = idx % w;
        int y = idx / w;
        static const int dxs[4] = {-1, 1, 0, 0};
        static const int dys[4] = { 0, 0,-1, 1};
        for (int k = 0; k < 4; k++) {
            int nx = x + dxs[k];
            int ny = y + dys[k];
            if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
            int nidx = ny * w + nx;
            if (mask[nidx] == 0) {
                mask[nidx] = 2;
                stack[sp++] = nidx;
            }
        }
    }
    free(stack);

    // Step 3: collapse to a clean 0/1 mask. Exterior (was 2) becomes 0;
    // interior (opaque was 1, hole was 0-not-reached) becomes 1.
    for (size_t i = 0; i < cells; i++) {
        mask[i] = (mask[i] == 2) ? 0 : 1;
    }

    *out_w = w;
    *out_h = h;
    return mask;
}

// Hit-test against the precomputed perimeter mask. (local_x, local_y) are
// pixel coordinates inside the rendered icon rect; we map them to the
// source-surface grid and look up the bit. Returns false if the icon has
// no mask (degraded fallback) or the click is outside its perimeter.
static bool icon_pixel_inside_perimeter(const DesktopIcon *icon,
                                         int local_x, int local_y,
                                         int target_w, int target_h)
{
    if (!icon || !icon->hit_mask || target_w <= 0 || target_h <= 0) return false;
    int sw = icon->hit_mask_w;
    int sh = icon->hit_mask_h;
    if (sw <= 0 || sh <= 0) return false;
    int sx = (local_x * sw) / target_w;
    int sy = (local_y * sh) / target_h;
    if (sx < 0 || sx >= sw || sy < 0 || sy >= sh) return false;
    return icon->hit_mask[sy * sw + sx] != 0;
}

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
//
// The drag system has two visible parts:
//   1. The "original" icon at drag_origin_x/y, painted at 50% alpha by
//      icons_paint() while drag_visible is true. This is the Snow Leopard
//      "where it came from" hint.
//   2. The ghost — a 32-bit ARGB override-redirect popup window created
//      and mapped on the first icons_drag_update() call (i.e. only after
//      the drag threshold crosses). It paints once after XMapRaised and
//      then just gets XMoveWindow'd per motion event, so the per-frame
//      cost during a drag is one round-trip instead of a full wallpaper
//      + icon-grid blit.
//
// drag_icon->x/y still gets updated by icons_drag_update() so that
// icons_drag_end()'s final clamp uses the cursor's release point — but
// icons_paint() reads drag_origin_x/y for the dragged icon while
// drag_visible is true, so the original stays put and only the ghost
// follows the cursor.
static DesktopIcon *drag_icon = NULL;   // Icon being dragged
static int drag_offset_x = 0;          // Mouse offset from icon origin
static int drag_offset_y = 0;          // (so the icon doesn't jump to cursor)
static int drag_origin_x = 0;          // Icon's x at drag_begin time
static int drag_origin_y = 0;          // Icon's y at drag_begin time
static bool drag_visible = false;       // True once threshold crossed and
                                        // ghost is mapped — gates the
                                        // 50% alpha original render
static Window ghost_win = None;          // The ARGB override-redirect popup
static Visual *ghost_visual = NULL;      // Cached 32-bit TrueColor visual
static Colormap ghost_colormap = 0;      // Colormap matched to ghost_visual

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

    const char *ext = strrchr(path, '.');

    // PNG files self-preview: paint the file's own pixels as the icon
    // rather than the generic image-png mimetype glyph. Matches Snow
    // Leopard's "image files on the desktop look like the image" UX,
    // and is also what makes hit-test stress tests possible — without
    // it, three distinct PNGs all render with the same generic mimetype
    // shape and you can't tell them apart in a stack. Cairo only ships
    // PNG out of the box, so JPEG/GIF/SVG still fall through to the
    // theme icon below until a thumbnailer slice lands.
    if (ext && strcasecmp(ext, ".png") == 0) {
        cairo_surface_t *thumb = cairo_image_surface_create_from_png(path);
        if (cairo_surface_status(thumb) == CAIRO_STATUS_SUCCESS) {
            return thumb;
        }
        cairo_surface_destroy(thumb);
        // Fall through to mimetype lookup if the PNG was unreadable.
    }

    // .desktop files get special treatment: use the Icon= field to find
    // the right theme icon instead of showing a generic script icon.
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

        // Precompute a perimeter hit-mask off the source surface. Used by
        // icons_handle_click so a click on a transparent corner pixel of
        // the topmost icon falls through to a lower icon — but a click
        // on an interior hole (e.g. a donut center) still counts as a
        // hit on this icon.
        icon->hit_mask = build_hit_mask(icon->icon,
                                         &icon->hit_mask_w,
                                         &icon->hit_mask_h);

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

    // Initial z-order: sequential, matching the alphabetical paint order.
    // Reset next_z so the first click after scan promotes above all initial
    // values. Rebuild the paint_order index so icons_paint and
    // icons_handle_click have a valid view on first use.
    for (int i = 0; i < icon_count; i++) icons[i].z_order = i + 1;
    next_z = icon_count;
    rebuild_paint_order();

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
        if (icons[i].hit_mask) {
            free(icons[i].hit_mask);
            icons[i].hit_mask = NULL;
            icons[i].hit_mask_w = 0;
            icons[i].hit_mask_h = 0;
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
    int icon_px = S(ICON_SIZE);

    // Walk by stacking order so icons clicked more recently render on top
    // of older ones. paint_order is sorted ascending by z_order — the
    // last entry is the top of the stack, painted last so its pixels win.
    if (paint_order_count != icon_count) rebuild_paint_order();
    for (int p = 0; p < paint_order_count; p++) {
        DesktopIcon *icon = &icons[paint_order[p]];

        // While the user is dragging, the dragged icon is also being
        // tracked by a separate ghost popup window that follows the
        // cursor. The cell painted here stays at the icon's pre-drag
        // position (drag_origin_x/y), faded to 50% alpha — Snow Leopard's
        // "this is where it came from" hint. icon->x/y is still valid
        // (icons_drag_update keeps it pointed at the cursor for the
        // eventual drop clamp), but reading it during a drag would put
        // the faded preview under the cursor and the ghost on top of
        // it, which doubles the visual cell instead of leaving an
        // origin marker.
        bool dragging_this = (icon == drag_icon) && drag_visible;
        int draw_x = dragging_this ? drag_origin_x : icon->x;
        int draw_y = dragging_this ? drag_origin_y : icon->y;

        // Center the icon image within the cell
        int img_x = draw_x + (cell_w - icon_px) / 2;
        int img_y = draw_y + S(2);  // Small top padding

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
        int text_visible_x = draw_x + (cell_w - text_w) / 2;

        // Snow Leopard parity: while an icon is being dragged, render the
        // entire icon cell (image + label pill + filename) at ~50% alpha
        // so the user can see the wallpaper / underlying icons through
        // the moving glyph. We push a group, paint normally below, then
        // pop_group_to_source + paint_with_alpha so every layer of this
        // cell composites together at one consistent opacity. The cell
        // is drawn at drag_origin_x/y (the icon's pre-drag spot) — the
        // ghost popup, mapped by icons_drag_update, follows the cursor
        // separately.
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
        // Pill widening: 14% on top of the tight text-rect bounds. Was 20%
        // — narrowed by 5% of the previous overall pill width to bring it
        // closer to a snug capsule without losing the "more breathing room
        // than a tight hug" feel that matches Snow Leopard.
        double extra_w     = base_pill_w * 0.14;
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
        // layout, so we move_to draw_x (NOT text_visible_x — that
        // would double-center and shift the label off to the right).
        // draw_x equals icon->x normally and drag_origin_x for the
        // dragged-icon's faded source cell.
        // Multiple passes create the dark halo that makes white text
        // readable on any wallpaper (light, dark, or busy). Offsets are
        // scaled so the halo stays proportional to the rendered text on
        // HiDPI outputs.
        int halo = S(1);
        if (halo < 1) halo = 1;
        for (int dy = -halo; dy <= halo * 2; dy += halo) {
            for (int dx = -halo; dx <= halo; dx += halo) {
                if (dx == 0 && dy == 0) continue;  // Center is the white text itself
                cairo_move_to(cr, draw_x + dx, label_y + dy);
                cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
                pango_cairo_show_layout(cr, layout);
            }
        }

        // White text on top
        cairo_move_to(cr, draw_x, label_y);
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
    // Hit-test against the actual painted regions of an icon, NOT the full
    // 140×140 cell. The cell is the layout slot; the painted glyph fills only
    // (a) the icon image rect — icon_px × icon_px centered horizontally,
    //     S(2) below the cell top
    // (b) the label band — a strip below the image roughly 2 lines tall,
    //     spanning the cell width (the label pill itself is narrower but the
    //     band gives Finder-style generous click targets on short labels).
    //
    // Why this matters for stacked icons: with three icons free-form placed
    // close together, the topmost icon's *empty* cell padding (the gap
    // between its image and the cell edge, and below the label) used to
    // capture clicks that visually landed on an exposed corner of a lower
    // icon. Restricting the hit rect to just the painted pixels lets those
    // clicks fall through to the icon the user can actually see.
    //
    // Snow Leopard parity: clicking empty wallpaper between icons does
    // nothing; clicking on visible icon art or its filename label selects
    // the icon you can see.
    int cell_w  = S(ICON_CELL_W);
    int icon_px = S(ICON_SIZE);

    // Image rect (matches the paint geometry exactly)
    int img_off_x = (cell_w - icon_px) / 2;
    int img_off_y = S(2);

    // Label band — tracks the paint geometry. label_y in paint is
    // img_y + icon_px + S(4); we extend a bit past it to cover a 2-line
    // label without measuring text. 32pt is a comfortable cap for two lines
    // of 12pt Lucida Grande at any scale once run through S().
    int label_off_y = img_off_y + icon_px + S(4);
    int label_h     = S(32);

    // Walk in reverse stacking order so the visually-topmost icon at
    // (x, y) wins. paint_order is sorted ascending by z_order, so the
    // last entry is the top of the stack.
    if (paint_order_count != icon_count) rebuild_paint_order();
    for (int p = paint_order_count - 1; p >= 0; p--) {
        DesktopIcon *icon = &icons[paint_order[p]];

        // (a) icon image rect — pixel-accurate. The icon image fills only
        // a portion of the icon_px × icon_px rect; the rest is alpha=0
        // padding the source PNG carries (rounded corners, transparent
        // borders). A click on those transparent pixels of a topmost
        // icon must fall through to whatever icon is actually visible
        // beneath it. icon_pixel_opaque samples the source surface and
        // returns false on transparent pixels, so the loop continues to
        // the next-lower icon.
        int ix = icon->x + img_off_x;
        int iy = icon->y + img_off_y;
        if (x >= ix && x < ix + icon_px &&
            y >= iy && y < iy + icon_px) {
            int local_x = x - ix;
            int local_y = y - iy;
            if (icon_pixel_inside_perimeter(icon, local_x, local_y,
                                             icon_px, icon_px)) {
                return icon;
            }
            // Fall through to the next-lower icon.
        }

        // (b) label band (full cell width, fixed height). Kept as a
        // rect — text has internal gaps (between glyphs, between
        // ascenders) that pixel-accurate testing would let through, and
        // a "miss the wrong pixel between two letters" UX is worse than
        // a slightly generous label-band. The label band only matches
        // the cell width, not the icon image, so it doesn't fight
        // pixel-accurate image hits.
        int ly = icon->y + label_off_y;
        if (x >= icon->x && x < icon->x + cell_w &&
            y >= ly && y < ly + label_h) {
            return icon;
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
        // Surface the clicked icon — clicking what you see should also
        // raise it. Without this, a click on the bottom of a stack
        // selects it but leaves it visually buried, which contradicts
        // the "click what you see" promise. icon_promote_z bumps the
        // z_order and rebuilds paint_order for the next paint pass.
        icon_promote_z(icon);
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

                // Free old icon surfaces and hit masks before rescanning
                for (int i = 0; i < icon_count; i++) {
                    if (icons[i].icon) {
                        cairo_surface_destroy(icons[i].icon);
                        icons[i].icon = NULL;
                    }
                    if (icons[i].hit_mask) {
                        free(icons[i].hit_mask);
                        icons[i].hit_mask = NULL;
                        icons[i].hit_mask_w = 0;
                        icons[i].hit_mask_h = 0;
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

// ── Ghost popup (drag preview) ──────────────────────────────────────
//
// While an icon is being dragged we paint two things:
//   • the original cell at drag_origin_x/y, rendered at 50% alpha by
//     icons_paint() (Snow Leopard's "this is where it came from" hint);
//   • the ghost — a small ARGB override-redirect popup that contains the
//     icon image and filename label, painted once and then moved via
//     XMoveWindow per cursor motion.
//
// Painting the entire desktop wallpaper + icon grid every motion event
// was the bottleneck on the Legion's 2880×1800 panel. Letting moonrock
// composite a cached pixmap for the ghost gets us back to a single
// per-frame X round-trip instead of a full ARGB blit + Pango layout.

// Compute the height the ghost popup needs in physical pixels: enough to
// fit the icon image plus a two-line filename label without clipping the
// label's bottom. Used by both ghost_create (window size) and
// ghost_paint_content (no clipping rect — but the Pango layout naturally
// fits in this height). Kept as a helper so the two sites can't drift.
static int ghost_window_height(int icon_px)
{
    // Two-line cap on label height in points (12pt × 2 lines + leading).
    // S() folds in the HiDPI scale so the ghost is tall enough at any DPI.
    int label_band = S(36);
    return S(2) + icon_px + S(4) + label_band;
}

// Paint the dragged icon's image and filename label into the ghost
// window's content area. Called once after XMapRaised — moonrock
// redirects the window on map, so this paint goes into the backing
// pixmap and survives without us having to handle Expose events.
//
// The geometry mirrors the cell layout used by icons_paint(): icon image
// centered horizontally with S(2) top padding, filename label below at
// label_y = S(2) + icon_px + S(4), centered via Pango ALIGN_CENTER over
// the full cell width with the same dark halo for legibility.
//
// Snow Leopard parity: the moving glyph is rendered at ~50% alpha so the
// user can see the wallpaper / underlying icons through it. We push a
// group around the whole cell paint and pop_with_alpha so every layer
// (icon image + halo + label text) composites at one consistent opacity,
// matching how icons_paint historically handled the moving cell before
// this slice split it into ghost + origin.
static void ghost_paint_content(cairo_t *cr, DesktopIcon *icon,
                                 int cell_w, int icon_px)
{
    int img_x   = (cell_w - icon_px) / 2;
    int img_y   = S(2);
    int label_y = img_y + icon_px + S(4);

    cairo_push_group(cr);

    // Icon image — same scale-on-paint approach icons_paint uses.
    if (icon->icon) {
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

    // Filename label with halo. PangoLayout width = cell_w + ALIGN_CENTER
    // does the horizontal centering, so move_to origin is the cell's
    // left edge (0), not cell_w/2 — see the matching note in icons_paint.
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, icon->name, -1);
    PangoFontDescription *font = pango_font_description_from_string(
        icon_scaled_font(12));
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    pango_layout_set_width(layout, cell_w * PANGO_SCALE);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_height(layout, -2 * PANGO_SCALE);

    int halo = S(1);
    if (halo < 1) halo = 1;
    for (int dy = -halo; dy <= halo * 2; dy += halo) {
        for (int dx = -halo; dx <= halo; dx += halo) {
            if (dx == 0 && dy == 0) continue;
            cairo_move_to(cr, 0 + dx, label_y + dy);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
            pango_cairo_show_layout(cr, layout);
        }
    }
    cairo_move_to(cr, 0, label_y);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);

    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, 0.5);
}

// Create, paint, and map the ghost popup at the given root-coords
// top-left. Sized one icon cell wide and tall so there's room for both
// the icon image and a two-line filename label without clipping.
//
// The window is 32-bit ARGB + override-redirect so moonrock leaves it
// alone (no chrome, no decoration) and lets the icon's PNG alpha bleed
// through to whatever's behind. Override-redirect does not bypass the
// compositor — XComposite still redirects on map, so the single direct
// paint after XMapRaised is captured into the backing pixmap.
static void ghost_create(Display *dpy, DesktopIcon *icon,
                         int root_x, int root_y)
{
    if (!dpy || !icon || ghost_win != None) return;

    int screen   = DefaultScreen(dpy);
    Window root  = RootWindow(dpy, screen);
    int cell_w   = S(ICON_CELL_W);
    int icon_px  = S(ICON_SIZE);
    // Ghost window height = enough room for image + 2-line filename label.
    // Using the full ICON_CELL_H here would clip the bottom of a wrapped
    // label because cell_h is sized for the layout grid (where label
    // overflow is fine), not for the ghost's standalone window.
    int ghost_h  = ghost_window_height(icon_px);

    // Find a 32-bit TrueColor visual on the default screen. Anything less
    // (e.g. the root visual on a 24-bit display) won't carry the icon's
    // alpha channel and would render the ghost on a black square.
    XVisualInfo vinfo;
    if (!XMatchVisualInfo(dpy, screen, 32, TrueColor, &vinfo)) {
        fprintf(stderr,
                "[icons] Ghost popup: no 32-bit TrueColor visual; drag "
                "will fall back to repaint-only.\n");
        return;
    }
    ghost_visual   = vinfo.visual;
    ghost_colormap = XCreateColormap(dpy, root, vinfo.visual, AllocNone);

    XSetWindowAttributes sa;
    sa.colormap          = ghost_colormap;
    sa.background_pixel  = 0;       // Fully transparent
    sa.border_pixel      = 0;
    sa.override_redirect = True;    // No WM management, no decoration
    sa.event_mask        = NoEventMask;
    unsigned long mask =
        CWColormap | CWBackPixel | CWBorderPixel |
        CWOverrideRedirect | CWEventMask;

    ghost_win = XCreateWindow(dpy, root,
        root_x, root_y, cell_w, ghost_h,
        0, 32, InputOutput, vinfo.visual,
        mask, &sa);

    if (ghost_win == None) {
        XFreeColormap(dpy, ghost_colormap);
        ghost_colormap = 0;
        return;
    }

    // Map first, then paint directly into the window. moonrock redirects
    // the window on map, so the cairo paint below lands in the backing
    // pixmap that the compositor will draw from on every frame —
    // XMoveWindow per motion event won't trigger a re-paint by us.
    XMapRaised(dpy, ghost_win);

    cairo_surface_t *xsurf = cairo_xlib_surface_create(
        dpy, ghost_win, vinfo.visual, cell_w, ghost_h);
    cairo_t *cr = cairo_create(xsurf);

    // Clear to fully transparent before painting (background_pixel is 0
    // but explicit CAIRO_OPERATOR_SOURCE makes the alpha state explicit).
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    ghost_paint_content(cr, icon, cell_w, icon_px);

    cairo_destroy(cr);
    cairo_surface_destroy(xsurf);
    XFlush(dpy);
}

// Tear down the ghost popup. Safe to call even when no ghost was created
// (e.g. on a click+release that never crossed the drag threshold).
static void ghost_destroy(Display *dpy)
{
    if (!dpy) return;
    if (ghost_win != None) {
        XDestroyWindow(dpy, ghost_win);
        ghost_win = None;
    }
    if (ghost_colormap != 0) {
        XFreeColormap(dpy, ghost_colormap);
        ghost_colormap = 0;
    }
    ghost_visual = NULL;
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
    // Snapshot the icon's pre-drag position. icons_paint() draws the
    // dragged icon at this point (at 50% alpha, once drag_visible flips)
    // even though icon->x/y will move with the cursor.
    drag_origin_x = icon->x;
    drag_origin_y = icon->y;
    drag_visible  = false;  // Stays false until threshold crosses
}

void icons_drag_update(int local_x, int local_y, int root_x, int root_y)
{
    if (!drag_icon) return;

    // Move the icon's logical position so icons_drag_end's clamp uses
    // the cursor's final spot. icons_paint won't actually use this
    // value while drag_visible is true (it draws the dragged icon at
    // drag_origin instead) — but the value still has to be right at
    // drop time for the clamp + xattr save to land in the right place.
    drag_icon->x = local_x - drag_offset_x;
    drag_icon->y = local_y - drag_offset_y;

    // Lazy ghost creation — first call after threshold cross. We pass
    // the ghost's top-left in root coords because override-redirect
    // windows are positioned in the virtual-screen-root frame.
    int ghost_x = root_x - drag_offset_x;
    int ghost_y = root_y - drag_offset_y;

    if (ghost_win == None) {
        ghost_create(cached_dpy, drag_icon, ghost_x, ghost_y);
        drag_visible = true;
    } else {
        XMoveWindow(cached_dpy, ghost_win, ghost_x, ghost_y);
        XFlush(cached_dpy);
    }
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

    icons_drag_cancel();
}

void icons_drag_cancel(void)
{
    // Tear down the ghost popup if one was mapped, then clear all drag
    // state. Always safe to call — handles simple click+release (no
    // ghost ever created), XDND-handled drop (we don't clamp), and
    // ButtonRelease before threshold cross.
    ghost_destroy(cached_dpy);
    drag_icon     = NULL;
    drag_visible  = false;
    drag_offset_x = 0;
    drag_offset_y = 0;
    drag_origin_x = 0;
    drag_origin_y = 0;
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
