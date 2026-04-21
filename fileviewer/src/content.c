// CopyCatOS — by Kyle Blizzard at Blizzard.show

// content.c — Finder content area (icon grid / list view / column view)
//
// The content area is the main panel of the Finder — everything to the
// right of the sidebar and below the toolbar. It displays the contents
// of the current directory in icon grid or list view mode.
//
// Icon view: Each file/folder is shown as a 64x64 icon in a 90x90 grid
// cell, with a filename label below. Selected items get a blue rounded
// rect highlight (Snow Leopard style).
//
// List view: Files are shown as rows in a columnar table with headers
// (Name, Date Modified, Size, Kind). Rows alternate between white and
// light blue (#D8E6F5) per the Apple HIG. Selected rows get a full-width
// blue highlight (#386C9D).
//
// Icons are loaded from the Aqua theme or fall back to Cairo-drawn
// generic icons. The content area has a white background (#FFFFFF).

#define _GNU_SOURCE  // For M_PI and strcasecmp

#include "content.h"

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>
#include <time.h>

// ── Module state ────────────────────────────────────────────────────

// Array of files in the current directory
static ContentFile files[CONTENT_MAX_FILES];
static int file_count = 0;

// Current view mode — defaults to icon grid
static ViewMode current_view_mode = VIEW_MODE_ICON;

// ── Icon resolution ─────────────────────────────────────────────────
//
// Maps file extensions to icon names in the Aqua / freedesktop
// icon theme. Same approach as desktop's icons.c.

typedef struct {
    const char *extension;    // e.g., ".pdf"
    const char *icon_name;    // e.g., "application-pdf"
} ExtIconMap;

static const ExtIconMap ext_icon_map[] = {
    { ".pdf",      "application-pdf" },
    { ".txt",      "text-plain" },
    { ".text",     "text-plain" },
    { ".md",       "text-x-generic" },
    { ".c",        "text-x-csrc" },
    { ".h",        "text-x-chdr" },
    { ".py",       "text-x-python" },
    { ".sh",       "text-x-script" },
    { ".png",      "image-png" },
    { ".jpg",      "image-jpeg" },
    { ".jpeg",     "image-jpeg" },
    { ".gif",      "image-gif" },
    { ".svg",      "image-svg+xml" },
    { ".bmp",      "image-bmp" },
    { ".mp3",      "audio-mpeg" },
    { ".wav",      "audio-x-wav" },
    { ".flac",     "audio-flac" },
    { ".mp4",      "video-mp4" },
    { ".mkv",      "video-x-matroska" },
    { ".avi",      "video-x-msvideo" },
    { ".zip",      "application-zip" },
    { ".tar",      "application-x-tar" },
    { ".gz",       "application-gzip" },
    { ".html",     "text-html" },
    { ".css",      "text-css" },
    { ".js",       "application-javascript" },
    { ".json",     "application-json" },
    { ".xml",      "application-xml" },
    { ".deb",      "application-x-deb" },
    { ".rpm",      "application-x-rpm" },
    { ".desktop",  "application-x-desktop" },
    { NULL, NULL }
};

// ── Helper: MoonBase bundle detection ───────────────────────────────
//
// A MoonBase bundle is a directory whose name ends in ".app" (future
// single-file shipping format, still a directory today) or ".appd"
// (developer directory) and that has a Contents/Info.appc file inside.
// We still accept the legacy ".appc" / ".appcd" names during the
// rename-pass transition. We only check structure here — the launcher
// re-validates the full bundle-spec §8 rules (realpath escapes,
// absolute-symlink rejects, executable location) before actually
// running anything. No point duplicating those checks in fileviewer's
// double-click hot path.

static bool is_appc_bundle(const char *path)
{
    if (!path) return false;
    size_t len = strlen(path);
    // Accept all four bundle suffixes during the .appc/.appcd → .app/.appd
    // rename. Once the reference apps ship and the ABI freezes, .appc and
    // .appcd drop out and .app becomes single-file-only.
    bool is_app   = (len >= 4 && strcmp(path + len - 4, ".app")   == 0);
    bool is_appd  = (len >= 5 && strcmp(path + len - 5, ".appd")  == 0);
    bool is_appc  = (len >= 5 && strcmp(path + len - 5, ".appc")  == 0);
    bool is_appcd = (len >= 6 && strcmp(path + len - 6, ".appcd") == 0);
    if (!is_app && !is_appd && !is_appc && !is_appcd) return false;

    char info[1024];
    int n = snprintf(info, sizeof(info), "%s/Contents/Info.appc", path);
    if (n < 0 || (size_t)n >= sizeof(info)) return false;

    struct stat st;
    return stat(info, &st) == 0 && S_ISREG(st.st_mode);
}

// ── Helper: Load a theme icon ───────────────────────────────────────
//
// Searches the Aqua and hicolor icon themes for the named icon.
// Tries 64x64 first (best match for CONTENT_ICON_SIZE), then larger
// sizes that can be scaled down.

static cairo_surface_t *load_theme_icon(const char *icon_name,
                                         const char *subdir)
{
    const char *home = getenv("HOME");
    if (!home) return NULL;

    char path[1024];
    // Try sizes in order of preference: exact match first, then scale down
    const char *sizes[] = { "64x64", "128x128", "256x256", "48x48", "32x32", NULL };

    for (int i = 0; sizes[i]; i++) {
        // Aqua theme first
        snprintf(path, sizeof(path),
            "%s/.local/share/icons/Aqua/%s/%s/%s.png",
            home, sizes[i], subdir, icon_name);

        cairo_surface_t *surface = cairo_image_surface_create_from_png(path);
        if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
            return surface;
        }
        cairo_surface_destroy(surface);

        // hicolor fallback
        snprintf(path, sizeof(path),
            "%s/.local/share/icons/hicolor/%s/%s/%s.png",
            home, sizes[i], subdir, icon_name);

        surface = cairo_image_surface_create_from_png(path);
        if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
            return surface;
        }
        cairo_surface_destroy(surface);
    }

    return NULL;
}

// ── Helper: Create a generic document icon ──────────────────────────
//
// A simple grey rectangle with a folded corner, used when no theme
// icon can be found.

static cairo_surface_t *create_fallback_icon(void)
{
    int size = CONTENT_ICON_SIZE;
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surface);

    double m = 4;     // Margin
    double fold = 10; // Folded corner size
    double w = size - m * 2;
    double h = size - m * 2;

    // Document shape with folded corner
    cairo_new_path(cr);
    cairo_move_to(cr, m, m);
    cairo_line_to(cr, m + w - fold, m);
    cairo_line_to(cr, m + w, m + fold);
    cairo_line_to(cr, m + w, m + h);
    cairo_line_to(cr, m, m + h);
    cairo_close_path(cr);

    // White fill with grey border
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.6, 0.6, 0.6, 1.0);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    // Fold triangle
    cairo_new_path(cr);
    cairo_move_to(cr, m + w - fold, m);
    cairo_line_to(cr, m + w - fold, m + fold);
    cairo_line_to(cr, m + w, m + fold);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0.85, 0.85, 0.85, 1.0);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.6, 0.6, 0.6, 1.0);
    cairo_stroke(cr);

    cairo_destroy(cr);
    return surface;
}

// ── Helper: Create a generic folder icon ────────────────────────────

static cairo_surface_t *create_fallback_folder_icon(void)
{
    int size = CONTENT_ICON_SIZE;
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surface);

    double m = 3;
    double w = size - m * 2;
    double h = size - m * 2;
    double tab_w = w * 0.35;
    double tab_h = h * 0.15;
    double r = 3;

    // Folder body (rounded rectangle)
    double body_y = m + tab_h;
    cairo_new_sub_path(cr);
    cairo_arc(cr, m + r, body_y + r, r, M_PI, 1.5 * M_PI);
    cairo_arc(cr, m + w - r, body_y + r, r, 1.5 * M_PI, 2 * M_PI);
    cairo_arc(cr, m + w - r, m + h - r, r, 0, 0.5 * M_PI);
    cairo_arc(cr, m + r, m + h - r, r, 0.5 * M_PI, M_PI);
    cairo_close_path(cr);

    // Tab on top-left
    cairo_new_sub_path(cr);
    cairo_arc(cr, m + r, m + r, r, M_PI, 1.5 * M_PI);
    cairo_line_to(cr, m + tab_w - 3, m);
    cairo_line_to(cr, m + tab_w, body_y);
    cairo_line_to(cr, m, body_y);
    cairo_close_path(cr);

    // Aqua blue fill
    cairo_set_source_rgba(cr, 0.45, 0.68, 0.95, 0.95);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.3, 0.5, 0.8, 1.0);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    cairo_destroy(cr);
    return surface;
}

// ── Helper: Resolve icon for a file entry ───────────────────────────
//
// Determines the correct icon for a file based on whether it's a
// directory and what extension it has.

static cairo_surface_t *resolve_icon(const char *filepath, bool is_dir)
{
    cairo_surface_t *icon = NULL;

    if (is_dir) {
        icon = load_theme_icon("folder", "places");
        if (!icon) icon = load_theme_icon("inode-directory", "places");
        if (!icon) icon = create_fallback_folder_icon();
        return icon;
    }

    // Match by file extension
    const char *ext = strrchr(filepath, '.');
    if (ext) {
        for (int i = 0; ext_icon_map[i].extension; i++) {
            if (strcasecmp(ext, ext_icon_map[i].extension) == 0) {
                icon = load_theme_icon(ext_icon_map[i].icon_name, "mimetypes");
                if (icon) return icon;
                break;
            }
        }
    }

    // Generic fallback
    icon = load_theme_icon("text-x-generic", "mimetypes");
    if (icon) return icon;

    return create_fallback_icon();
}

// ── Comparison for sorting ──────────────────────────────────────────
//
// Sort directories first, then alphabetically by name (case-insensitive).
// This matches the default Finder behavior.

static int compare_files(const void *a, const void *b)
{
    const ContentFile *fa = (const ContentFile *)a;
    const ContentFile *fb = (const ContentFile *)b;

    // Directories sort before files
    if (fa->is_directory != fb->is_directory) {
        return fa->is_directory ? -1 : 1;
    }

    // Within the same type, sort alphabetically
    return strcasecmp(fa->name, fb->name);
}

// ── Public API ──────────────────────────────────────────────────────

void content_scan(const char *dir_path)
{
    // Free any existing icons before rescanning
    for (int i = 0; i < file_count; i++) {
        if (files[i].icon) {
            cairo_surface_destroy(files[i].icon);
            files[i].icon = NULL;
        }
        if (files[i].icon_small) {
            cairo_surface_destroy(files[i].icon_small);
            files[i].icon_small = NULL;
        }
    }
    file_count = 0;

    DIR *dir = opendir(dir_path);
    if (!dir) {
        fprintf(stderr, "[content] Cannot open directory: %s\n", dir_path);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && file_count < CONTENT_MAX_FILES) {
        // Skip hidden files (starting with '.') and the special entries
        // '.' (current dir) and '..' (parent dir)
        if (entry->d_name[0] == '.') continue;

        ContentFile *f = &files[file_count];
        memset(f, 0, sizeof(ContentFile));

        // Store the display name
        strncpy(f->name, entry->d_name, sizeof(f->name) - 1);

        // Build the full path
        snprintf(f->path, sizeof(f->path), "%s/%s", dir_path, entry->d_name);

        // Stat the file to determine type, size, and modification time
        struct stat st;
        if (stat(f->path, &st) == 0) {
            f->is_directory = S_ISDIR(st.st_mode);
            f->file_size    = st.st_size;
            f->mod_time     = st.st_mtime;
        }

        // Load the icon (64x64 for icon view; list view uses scaled version)
        f->icon = resolve_icon(f->path, f->is_directory);
        f->icon_small = NULL;  // Lazily created when list view paints

        file_count++;
    }

    closedir(dir);

    // Sort: directories first, then alphabetical
    if (file_count > 1) {
        qsort(files, file_count, sizeof(ContentFile), compare_files);
    }

    fprintf(stderr, "[content] Scanned %s: %d items\n", dir_path, file_count);
}


// Get the current view mode
ViewMode content_get_view_mode(void)
{
    return current_view_mode;
}

// ── Helper: Get or create a 16x16 icon for list view ─────────────────
//
// List view needs smaller icons (16x16) than icon view (64x64).
// We lazily create a scaled-down copy the first time it's needed,
// then cache it in the ContentFile's icon_small field.

static cairo_surface_t *get_small_icon(ContentFile *f)
{
    if (f->icon_small) return f->icon_small;
    if (!f->icon) return NULL;

    // Create a new 16x16 surface and scale the 64x64 icon into it
    f->icon_small = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
        CONTENT_LIST_ICON_SIZE, CONTENT_LIST_ICON_SIZE);
    cairo_t *cr = cairo_create(f->icon_small);

    int src_w = cairo_image_surface_get_width(f->icon);
    int src_h = cairo_image_surface_get_height(f->icon);
    double sx = (double)CONTENT_LIST_ICON_SIZE / src_w;
    double sy = (double)CONTENT_LIST_ICON_SIZE / src_h;

    cairo_scale(cr, sx, sy);
    cairo_set_source_surface(cr, f->icon, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);

    return f->icon_small;
}

// ── Helper: Format file size as human-readable string ────────────────
//
// Converts bytes to a friendly format like "4 KB", "1.2 MB", "3.5 GB".
// Matches the Snow Leopard Finder's size column format.

static void format_file_size(off_t bytes, char *buf, size_t buf_size)
{
    if (bytes < 1024) {
        snprintf(buf, buf_size, "%ld B", (long)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(buf, buf_size, "%ld KB", (long)(bytes / 1024));
    } else if (bytes < 1024L * 1024 * 1024) {
        snprintf(buf, buf_size, "%.1f MB", bytes / (1024.0 * 1024.0));
    } else {
        snprintf(buf, buf_size, "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    }
}

// ── Helper: Determine file "kind" string ─────────────────────────────
//
// Returns a human-readable kind string like "Folder", "PNG Image",
// "C Source File", etc. based on the file extension.

static const char *get_file_kind(const ContentFile *f)
{
    if (f->is_directory) return "Folder";

    // Find the file extension
    const char *dot = strrchr(f->name, '.');
    if (!dot || dot == f->name) return "Document";

    // Map extensions to kind names
    if (strcasecmp(dot, ".c") == 0)    return "C Source File";
    if (strcasecmp(dot, ".h") == 0)    return "C Header File";
    if (strcasecmp(dot, ".py") == 0)   return "Python Script";
    if (strcasecmp(dot, ".sh") == 0)   return "Shell Script";
    if (strcasecmp(dot, ".txt") == 0)  return "Plain Text";
    if (strcasecmp(dot, ".md") == 0)   return "Markdown";
    if (strcasecmp(dot, ".pdf") == 0)  return "PDF Document";
    if (strcasecmp(dot, ".png") == 0)  return "PNG Image";
    if (strcasecmp(dot, ".jpg") == 0)  return "JPEG Image";
    if (strcasecmp(dot, ".jpeg") == 0) return "JPEG Image";
    if (strcasecmp(dot, ".gif") == 0)  return "GIF Image";
    if (strcasecmp(dot, ".svg") == 0)  return "SVG Image";
    if (strcasecmp(dot, ".mp3") == 0)  return "MP3 Audio";
    if (strcasecmp(dot, ".wav") == 0)  return "WAV Audio";
    if (strcasecmp(dot, ".mp4") == 0)  return "MPEG-4 Video";
    if (strcasecmp(dot, ".mkv") == 0)  return "MKV Video";
    if (strcasecmp(dot, ".zip") == 0)  return "ZIP Archive";
    if (strcasecmp(dot, ".tar") == 0)  return "TAR Archive";
    if (strcasecmp(dot, ".gz") == 0)   return "GZip Archive";
    if (strcasecmp(dot, ".html") == 0) return "HTML Document";
    if (strcasecmp(dot, ".css") == 0)  return "CSS Stylesheet";
    if (strcasecmp(dot, ".js") == 0)   return "JavaScript";
    if (strcasecmp(dot, ".json") == 0) return "JSON File";
    if (strcasecmp(dot, ".xml") == 0)  return "XML Document";
    if (strcasecmp(dot, ".desktop") == 0) return "Application";
    if (strcasecmp(dot, ".rpm") == 0)  return "RPM Package";
    if (strcasecmp(dot, ".deb") == 0)  return "DEB Package";

    return "Document";
}

// ── List view painting ──────────────────────────────────────────────
//
// Draws files in a columnar table with headers (Name, Date Modified,
// Size, Kind). Alternating row colors match the Snow Leopard Finder.
// Each row has a 16x16 icon followed by the filename, then metadata
// columns at fixed widths.

static void content_paint_list(FinderState *fs, cairo_t *cr,
                                int ox, int oy, int cw, int ch)
{
    // ── Column header bar ─────────────────────────────────────────
    //
    // Grey gradient background matching Snow Leopard's column headers.
    // Headers: Name | Date Modified | Size | Kind
    int hdr_y = oy;
    int hdr_h = CONTENT_LIST_HEADER_H;

    // Header gradient: light grey top to slightly darker bottom
    cairo_pattern_t *hdr_grad = cairo_pattern_create_linear(
        0, hdr_y, 0, hdr_y + hdr_h);
    cairo_pattern_add_color_stop_rgb(hdr_grad, 0.0,
        240 / 255.0, 240 / 255.0, 240 / 255.0);
    cairo_pattern_add_color_stop_rgb(hdr_grad, 1.0,
        218 / 255.0, 218 / 255.0, 218 / 255.0);
    cairo_set_source(cr, hdr_grad);
    cairo_rectangle(cr, ox, hdr_y, cw, hdr_h);
    cairo_fill(cr);
    cairo_pattern_destroy(hdr_grad);

    // 1px bottom border on header
    cairo_set_source_rgb(cr, 180 / 255.0, 180 / 255.0, 180 / 255.0);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, ox, hdr_y + hdr_h - 0.5);
    cairo_line_to(cr, ox + cw, hdr_y + hdr_h - 0.5);
    cairo_stroke(cr);

    // Name column gets the remaining space after the fixed columns
    int name_w = cw - CONTENT_LIST_COL_DATE - CONTENT_LIST_COL_SIZE
                     - CONTENT_LIST_COL_KIND;
    if (name_w < 100) name_w = 100;

    // Column X positions
    int col_name_x = ox;
    int col_date_x = ox + name_w;
    int col_size_x = col_date_x + CONTENT_LIST_COL_DATE;
    int col_kind_x = col_size_x + CONTENT_LIST_COL_SIZE;

    // Draw header labels
    const char *headers[] = { "Name", "Date Modified", "Size", "Kind" };
    int col_xs[] = { col_name_x + 24, col_date_x + 4, col_size_x + 4, col_kind_x + 4 };

    for (int i = 0; i < 4; i++) {
        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, headers[i], -1);

        PangoFontDescription *font = pango_font_description_from_string(
            "Lucida Grande Bold 10");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);

        cairo_move_to(cr, col_xs[i], hdr_y + 4);
        cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }

    // Draw vertical column separator lines in the header
    int sep_xs[] = { col_date_x, col_size_x, col_kind_x };
    cairo_set_source_rgb(cr, 190 / 255.0, 190 / 255.0, 190 / 255.0);
    for (int i = 0; i < 3; i++) {
        cairo_move_to(cr, sep_xs[i] + 0.5, hdr_y + 3);
        cairo_line_to(cr, sep_xs[i] + 0.5, hdr_y + hdr_h - 3);
        cairo_stroke(cr);
    }

    // ── File rows ──────────────────────────────────────────────────
    int row_y = hdr_y + hdr_h;

    for (int i = 0; i < file_count; i++) {
        ContentFile *f = &files[i];
        int ry = row_y + i * CONTENT_LIST_ROW_H;

        // Stop if we've gone past the visible area
        if (ry > oy + ch) break;

        // ── Row background: alternating white / light blue ─────
        if (f->selected) {
            // Selected: blue highlight row
            cairo_set_source_rgb(cr, CONTENT_SEL_R, CONTENT_SEL_G, CONTENT_SEL_B);
        } else if (i % 2 == 1) {
            // Odd rows: light blue (#D8E6F5)
            cairo_set_source_rgb(cr, CONTENT_ROW_ODD_R,
                                 CONTENT_ROW_ODD_G, CONTENT_ROW_ODD_B);
        } else {
            // Even rows: white
            cairo_set_source_rgb(cr, CONTENT_ROW_EVEN_R,
                                 CONTENT_ROW_EVEN_G, CONTENT_ROW_EVEN_B);
        }
        cairo_rectangle(cr, ox, ry, cw, CONTENT_LIST_ROW_H);
        cairo_fill(cr);

        // Text color: white on selected rows, dark grey otherwise
        double text_r = f->selected ? 1.0 : 0.1;
        double text_g = f->selected ? 1.0 : 0.1;
        double text_b = f->selected ? 1.0 : 0.1;

        // ── 16x16 icon ────────────────────────────────────────
        cairo_surface_t *small = get_small_icon(f);
        if (small) {
            int icon_x = col_name_x + 4;
            int icon_y = ry + (CONTENT_LIST_ROW_H - CONTENT_LIST_ICON_SIZE) / 2;
            cairo_set_source_surface(cr, small, icon_x, icon_y);
            cairo_paint(cr);
        }

        // ── Name column ───────────────────────────────────────
        {
            PangoLayout *layout = pango_cairo_create_layout(cr);
            pango_layout_set_text(layout, f->name, -1);

            PangoFontDescription *font = pango_font_description_from_string(
                "Lucida Grande 10");
            pango_layout_set_font_description(layout, font);
            pango_font_description_free(font);

            pango_layout_set_width(layout, (name_w - 28) * PANGO_SCALE);
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

            cairo_move_to(cr, col_name_x + 24, ry + 3);
            cairo_set_source_rgb(cr, text_r, text_g, text_b);
            pango_cairo_show_layout(cr, layout);
            g_object_unref(layout);
        }

        // ── Date Modified column ──────────────────────────────
        {
            char date_buf[64];
            struct tm *tm_info = localtime(&f->mod_time);
            if (tm_info) {
                strftime(date_buf, sizeof(date_buf), "%b %d, %Y %I:%M %p", tm_info);
            } else {
                strncpy(date_buf, "--", sizeof(date_buf));
            }

            PangoLayout *layout = pango_cairo_create_layout(cr);
            pango_layout_set_text(layout, date_buf, -1);

            PangoFontDescription *font = pango_font_description_from_string(
                "Lucida Grande 10");
            pango_layout_set_font_description(layout, font);
            pango_font_description_free(font);

            pango_layout_set_width(layout, (CONTENT_LIST_COL_DATE - 8) * PANGO_SCALE);
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

            cairo_move_to(cr, col_date_x + 4, ry + 3);
            cairo_set_source_rgb(cr, text_r, text_g, text_b);
            pango_cairo_show_layout(cr, layout);
            g_object_unref(layout);
        }

        // ── Size column ───────────────────────────────────────
        {
            char size_buf[32];
            if (f->is_directory) {
                strncpy(size_buf, "--", sizeof(size_buf));
            } else {
                format_file_size(f->file_size, size_buf, sizeof(size_buf));
            }

            PangoLayout *layout = pango_cairo_create_layout(cr);
            pango_layout_set_text(layout, size_buf, -1);

            PangoFontDescription *font = pango_font_description_from_string(
                "Lucida Grande 10");
            pango_layout_set_font_description(layout, font);
            pango_font_description_free(font);

            cairo_move_to(cr, col_size_x + 4, ry + 3);
            cairo_set_source_rgb(cr, text_r, text_g, text_b);
            pango_cairo_show_layout(cr, layout);
            g_object_unref(layout);
        }

        // ── Kind column ───────────────────────────────────────
        {
            const char *kind = get_file_kind(f);

            PangoLayout *layout = pango_cairo_create_layout(cr);
            pango_layout_set_text(layout, kind, -1);

            PangoFontDescription *font = pango_font_description_from_string(
                "Lucida Grande 10");
            pango_layout_set_font_description(layout, font);
            pango_font_description_free(font);

            pango_layout_set_width(layout, (CONTENT_LIST_COL_KIND - 8) * PANGO_SCALE);
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

            cairo_move_to(cr, col_kind_x + 4, ry + 3);
            cairo_set_source_rgb(cr, text_r, text_g, text_b);
            pango_cairo_show_layout(cr, layout);
            g_object_unref(layout);
        }
    }

    // ── Empty folder message ──────────────────────────────────────
    if (file_count == 0) {
        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, "This folder is empty.", -1);

        PangoFontDescription *font = pango_font_description_from_string(
            "Lucida Grande 12");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);

        int text_w, text_h;
        pango_layout_get_pixel_size(layout, &text_w, &text_h);

        cairo_move_to(cr, ox + (cw - text_w) / 2, oy + (ch - text_h) / 2);
        cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }
}

void content_paint(FinderState *fs)
{
    cairo_t *cr = fs->cr;

    // Content area origin and dimensions.
    // The content area starts to the right of the sidebar and below
    // the toolbar.
    int ox = fs->sidebar_w;           // Left edge of content area
    int oy = fs->toolbar_h;           // Top edge of content area
    int cw = fs->win_w - ox;          // Available width
    int ch = fs->win_h - oy - fs->statusbar_h - fs->pathbar_h;  // Height minus bottom bars

    cairo_save(cr);

    // ── 1. White background ─────────────────────────────────────
    cairo_rectangle(cr, ox, oy, cw, ch);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_fill(cr);

    // ── 2. Clip to the content area ─────────────────────────────
    //
    // Prevents drawing from bleeding into the sidebar or toolbar.
    cairo_rectangle(cr, ox, oy, cw, ch);
    cairo_clip(cr);

    // ── 3. Dispatch to the active view mode ─────────────────────
    //
    // If list view is active, call the list painter and skip the
    // icon grid logic. Column and Cover Flow are not yet implemented,
    // so they fall through to icon view.
    if (current_view_mode == VIEW_MODE_LIST) {
        content_paint_list(fs, cr, ox, oy, cw, ch);
        cairo_restore(cr);
        return;
    }

    // ── Icon grid (VIEW_MODE_ICON, and fallback for unimplemented modes)
    //
    // Files are laid out left-to-right, top-to-bottom in grid cells.
    // Each cell is CONTENT_CELL_W x CONTENT_CELL_H pixels.
    int cols = cw / CONTENT_CELL_W;
    if (cols < 1) cols = 1;

    int pad_left = 10;  // Left padding within the content area
    int pad_top  = 10;  // Top padding

    for (int i = 0; i < file_count; i++) {
        ContentFile *f = &files[i];

        // Grid position
        int col = i % cols;
        int row = i / cols;

        // Pixel position of this cell
        int cx = ox + pad_left + col * CONTENT_CELL_W;
        int cy = oy + pad_top  + row * CONTENT_CELL_H;

        // Center the icon within the cell
        int img_x = cx + (CONTENT_CELL_W - CONTENT_ICON_SIZE) / 2;
        int img_y = cy + 2;

        // ── Selection highlight ─────────────────────────────────
        if (f->selected) {
            cairo_new_sub_path(cr);
            double rx = cx + 2;
            double ry = cy + 2;
            double rw = CONTENT_CELL_W - 4;
            double rh = CONTENT_CELL_H - 4;
            double rr = 6;

            cairo_arc(cr, rx + rw - rr, ry + rr,      rr, -M_PI / 2, 0);
            cairo_arc(cr, rx + rw - rr, ry + rh - rr, rr, 0,          M_PI / 2);
            cairo_arc(cr, rx + rr,      ry + rh - rr, rr, M_PI / 2,   M_PI);
            cairo_arc(cr, rx + rr,      ry + rr,      rr, M_PI,        3 * M_PI / 2);
            cairo_close_path(cr);

            // Semi-transparent Aqua blue
            cairo_set_source_rgba(cr, 0x38 / 255.0, 0x75 / 255.0,
                                  0xD7 / 255.0, 0.2);
            cairo_fill(cr);
        }

        // ── Draw the icon ───────────────────────────────────────
        if (f->icon) {
            int src_w = cairo_image_surface_get_width(f->icon);
            int src_h = cairo_image_surface_get_height(f->icon);

            cairo_save(cr);
            if (src_w != CONTENT_ICON_SIZE || src_h != CONTENT_ICON_SIZE) {
                // Scale the icon to fit CONTENT_ICON_SIZE
                double sx = (double)CONTENT_ICON_SIZE / src_w;
                double sy = (double)CONTENT_ICON_SIZE / src_h;
                cairo_translate(cr, img_x, img_y);
                cairo_scale(cr, sx, sy);
                cairo_set_source_surface(cr, f->icon, 0, 0);
            } else {
                cairo_set_source_surface(cr, f->icon, img_x, img_y);
            }
            cairo_paint(cr);
            cairo_restore(cr);
        }

        // ── Draw the filename label ─────────────────────────────
        int label_y = img_y + CONTENT_ICON_SIZE + 2;

        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, f->name, -1);

        PangoFontDescription *font = pango_font_description_from_string(
            "Lucida Grande 9");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);

        // Center the text within the cell, truncate with ellipsis
        pango_layout_set_width(layout, (CONTENT_CELL_W - 4) * PANGO_SCALE);
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);

        // Get text size for centering
        int text_w, text_h;
        pango_layout_get_pixel_size(layout, &text_w, &text_h);
        (void)text_h;

        int text_x = cx + (CONTENT_CELL_W - text_w) / 2;

        // Dark text on white background
        if (f->selected) {
            // Selected: white text on blue bg
            // First draw a blue text highlight behind the label
            cairo_rectangle(cr, text_x - 2, label_y - 1,
                            text_w + 4, text_h + 2);
            cairo_set_source_rgb(cr, 0x38 / 255.0, 0x75 / 255.0, 0xD7 / 255.0);
            cairo_fill(cr);

            cairo_move_to(cr, text_x, label_y);
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        } else {
            cairo_move_to(cr, text_x, label_y);
            cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        }

        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }

    // ── 4. Empty folder message ─────────────────────────────────
    if (file_count == 0) {
        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, "This folder is empty.", -1);

        PangoFontDescription *font = pango_font_description_from_string(
            "Lucida Grande 12");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);

        int text_w, text_h;
        pango_layout_get_pixel_size(layout, &text_w, &text_h);

        cairo_move_to(cr, ox + (cw - text_w) / 2, oy + (ch - text_h) / 2);
        cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }

    cairo_restore(cr);
}

bool content_handle_click(FinderState *fs, int x, int y)
{
    int ox = fs->sidebar_w;
    int oy = fs->toolbar_h;
    int cw = fs->win_w - ox;

    fprintf(stderr, "[content] Click at (%d, %d), mode=%d, file_count=%d\n",
            x, y, (int)current_view_mode, file_count);

    // Deselect all files first
    for (int i = 0; i < file_count; i++) {
        files[i].selected = false;
    }

    if (current_view_mode == VIEW_MODE_LIST) {
        // ── List view click handling ──────────────────────────
        // Each row is CONTENT_LIST_ROW_H pixels tall, starting
        // after the column header bar.
        int row_start_y = oy + CONTENT_LIST_HEADER_H;

        if (y < row_start_y) {
            fprintf(stderr, "[content] Click on list header\n");
            return false;
        }

        int row_idx = (y - row_start_y) / CONTENT_LIST_ROW_H;
        if (row_idx >= 0 && row_idx < file_count) {
            files[row_idx].selected = true;
            fprintf(stderr, "[content] Selected (list): %s row=%d\n",
                    files[row_idx].name, row_idx);
            return true;
        }
    } else {
        // ── Icon view click handling ──────────────────────────
        int pad_left = 10;
        int pad_top  = 10;
        int cols = cw / CONTENT_CELL_W;
        if (cols < 1) cols = 1;

        for (int i = 0; i < file_count; i++) {
            int col = i % cols;
            int row = i / cols;

            int cx = ox + pad_left + col * CONTENT_CELL_W;
            int cy = oy + pad_top  + row * CONTENT_CELL_H;

            if (x >= cx && x < cx + CONTENT_CELL_W &&
                y >= cy && y < cy + CONTENT_CELL_H) {
                files[i].selected = true;
                fprintf(stderr, "[content] Selected (icon): %s at cell (%d,%d)\n",
                        files[i].name, col, row);
                return true;
            }
        }
    }

    fprintf(stderr, "[content] Click on empty space\n");
    return false;  // Click on empty space
}

void content_handle_double_click(FinderState *fs, int x, int y)
{
    int ox = fs->sidebar_w;
    int oy = fs->toolbar_h;
    int cw = fs->win_w - ox;
    int hit_idx = -1;

    if (current_view_mode == VIEW_MODE_LIST) {
        // ── List view: determine row from Y coordinate ────────
        int row_start_y = oy + CONTENT_LIST_HEADER_H;
        if (y >= row_start_y) {
            int row_idx = (y - row_start_y) / CONTENT_LIST_ROW_H;
            if (row_idx >= 0 && row_idx < file_count) {
                hit_idx = row_idx;
            }
        }
    } else {
        // ── Icon view: determine cell from grid coordinates ───
        int pad_left = 10;
        int pad_top  = 10;
        int cols = cw / CONTENT_CELL_W;
        if (cols < 1) cols = 1;

        for (int i = 0; i < file_count; i++) {
            int col = i % cols;
            int row = i / cols;

            int cx = ox + pad_left + col * CONTENT_CELL_W;
            int cy = oy + pad_top  + row * CONTENT_CELL_H;

            if (x >= cx && x < cx + CONTENT_CELL_W &&
                y >= cy && y < cy + CONTENT_CELL_H) {
                hit_idx = i;
                break;
            }
        }
    }

    // If we found a file, open it. Three branches:
    //   1. MoonBase bundle (.app / .appd directory with Contents/Info.appc): hand to
    //      moonbase-launch so bwrap + entitlements + consent run.
    //   2. Plain directory: navigate into it.
    //   3. Regular file: xdg-open.
    if (hit_idx >= 0) {
        if (files[hit_idx].is_directory) {
            if (is_appc_bundle(files[hit_idx].path)) {
                fprintf(stderr, "[content] Launching bundle: %s\n", files[hit_idx].path);
                pid_t pid = fork();
                if (pid == 0) {
                    setsid();
                    execlp("moonbase-launch", "moonbase-launch",
                           files[hit_idx].path, NULL);
                    _exit(127);
                }
            } else {
                fprintf(stderr, "[content] Navigate into: %s\n", files[hit_idx].path);
                finder_navigate(fs, files[hit_idx].path);
            }
        } else {
            fprintf(stderr, "[content] Opening: %s\n", files[hit_idx].path);
            pid_t pid = fork();
            if (pid == 0) {
                setsid();
                execlp("xdg-open", "xdg-open", files[hit_idx].path, NULL);
                _exit(127);
            }
        }
    }
}

int content_get_count(void)
{
    return file_count;
}

void content_shutdown(void)
{
    // Free all icon surfaces (both full-size and small list view icons)
    for (int i = 0; i < file_count; i++) {
        if (files[i].icon) {
            cairo_surface_destroy(files[i].icon);
            files[i].icon = NULL;
        }
        if (files[i].icon_small) {
            cairo_surface_destroy(files[i].icon_small);
            files[i].icon_small = NULL;
        }
    }
    file_count = 0;
}

// Set the current view mode (icon grid vs list)
void content_set_view_mode(ViewMode mode)
{
    current_view_mode = mode;
    fprintf(stderr, "[content] View mode changed to %d\n", (int)mode);
}
