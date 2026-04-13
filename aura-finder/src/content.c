// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// content.c — Finder content area (file icon grid)
//
// The content area is the main panel of the Finder — everything to the
// right of the sidebar and below the toolbar. It displays the contents
// of the current directory as an icon grid.
//
// Each file/folder is shown as a 64x64 icon in a 90x90 grid cell, with
// a filename label below. Icons are loaded from the AquaKDE theme or
// fall back to Cairo-drawn generic icons.
//
// The content area has a white background (#FFFFFF) and supports:
//   - Directory scanning with opendir/readdir
//   - Icon resolution by file extension → theme icon name
//   - Grid layout filling left-to-right, top-to-bottom
//   - Click to select, double-click to open/navigate

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

// ── Module state ────────────────────────────────────────────────────

// Array of files in the current directory
static ContentFile files[CONTENT_MAX_FILES];
static int file_count = 0;

// ── Icon resolution ─────────────────────────────────────────────────
//
// Maps file extensions to icon names in the AquaKDE / freedesktop
// icon theme. Same approach as aura-desktop's icons.c.

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

// ── Helper: Load a theme icon ───────────────────────────────────────
//
// Searches the AquaKDE and hicolor icon themes for the named icon.
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
        // AquaKDE theme first
        snprintf(path, sizeof(path),
            "%s/.local/share/icons/AquaKDE-icons/%s/%s/%s.png",
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

        // Check if it's a directory
        struct stat st;
        if (stat(f->path, &st) == 0) {
            f->is_directory = S_ISDIR(st.st_mode);
        }

        // Load the icon
        f->icon = resolve_icon(f->path, f->is_directory);

        file_count++;
    }

    closedir(dir);

    // Sort: directories first, then alphabetical
    if (file_count > 1) {
        qsort(files, file_count, sizeof(ContentFile), compare_files);
    }

    fprintf(stderr, "[content] Scanned %s: %d items\n", dir_path, file_count);
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
    int ch = fs->win_h - oy;          // Available height

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

    // ── 3. Icon grid ────────────────────────────────────────────
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
    int pad_left = 10;
    int pad_top  = 10;

    int cols = cw / CONTENT_CELL_W;
    if (cols < 1) cols = 1;

    // Deselect all files first
    for (int i = 0; i < file_count; i++) {
        files[i].selected = false;
    }

    // Check if the click hit any file cell
    for (int i = 0; i < file_count; i++) {
        int col = i % cols;
        int row = i / cols;

        int cx = ox + pad_left + col * CONTENT_CELL_W;
        int cy = oy + pad_top  + row * CONTENT_CELL_H;

        if (x >= cx && x < cx + CONTENT_CELL_W &&
            y >= cy && y < cy + CONTENT_CELL_H) {
            files[i].selected = true;
            fprintf(stderr, "[content] Selected: %s\n", files[i].name);
            return true;
        }
    }

    return false;  // Click on empty space
}

void content_handle_double_click(FinderState *fs, int x, int y)
{
    int ox = fs->sidebar_w;
    int oy = fs->toolbar_h;
    int cw = fs->win_w - ox;
    int pad_left = 10;
    int pad_top  = 10;

    int cols = cw / CONTENT_CELL_W;
    if (cols < 1) cols = 1;

    // Find which file was double-clicked
    for (int i = 0; i < file_count; i++) {
        int col = i % cols;
        int row = i / cols;

        int cx = ox + pad_left + col * CONTENT_CELL_W;
        int cy = oy + pad_top  + row * CONTENT_CELL_H;

        if (x >= cx && x < cx + CONTENT_CELL_W &&
            y >= cy && y < cy + CONTENT_CELL_H) {

            if (files[i].is_directory) {
                // Navigate into the directory
                fprintf(stderr, "[content] Navigate into: %s\n", files[i].path);
                finder_navigate(fs, files[i].path);
            } else {
                // Open the file with xdg-open
                fprintf(stderr, "[content] Opening: %s\n", files[i].path);
                pid_t pid = fork();
                if (pid == 0) {
                    setsid();
                    execlp("xdg-open", "xdg-open", files[i].path, NULL);
                    _exit(127);
                }
            }
            return;
        }
    }
}

int content_get_count(void)
{
    return file_count;
}

void content_shutdown(void)
{
    // Free all icon surfaces
    for (int i = 0; i < file_count; i++) {
        if (files[i].icon) {
            cairo_surface_destroy(files[i].icon);
            files[i].icon = NULL;
        }
    }
    file_count = 0;
}
