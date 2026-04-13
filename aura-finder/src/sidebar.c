// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// sidebar.c — Snow Leopard Finder source list (sidebar)
//
// The sidebar is the left panel of the Finder window, showing a
// categorized list of locations the user can navigate to. It mimics
// the real Snow Leopard Finder source list:
//
//   DEVICES
//     Macintosh HD   → /
//
//   PLACES
//     Home           → ~/
//     Desktop        → ~/Desktop
//     Applications   → /usr/share/applications
//     Documents      → ~/Documents
//     Downloads      → ~/Downloads
//     Music          → ~/Music
//     Pictures       → ~/Pictures
//
// The sidebar has the distinctive Snow Leopard blue-grey background
// (#DFE5ED), section headers in small caps, and Aqua-blue selection
// highlighting (#3875D7).

#define _GNU_SOURCE  // For M_PI

#include "sidebar.h"

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ── Sidebar layout constants ────────────────────────────────────────

#define SB_BG_R  (0xDF / 255.0)  // Background red   (#DFE5ED)
#define SB_BG_G  (0xE5 / 255.0)  // Background green
#define SB_BG_B  (0xED / 255.0)  // Background blue

#define SB_PAD_LEFT   12   // Left padding for item text
#define SB_ITEM_H     24   // Height of each sidebar item row
#define SB_HEADER_H   24   // Height of each section header row
#define SB_ICON_SIZE   16  // Size of the small icon next to each item
#define SB_ICON_PAD     6  // Gap between the icon and the text

// Aqua selection highlight color (#3875D7)
#define SB_SEL_R  (0x38 / 255.0)
#define SB_SEL_G  (0x75 / 255.0)
#define SB_SEL_B  (0xD7 / 255.0)

// Separator line on the right edge (#B8B8B8)
#define SB_SEP_R  (0xB8 / 255.0)
#define SB_SEP_G  (0xB8 / 255.0)
#define SB_SEP_B  (0xB8 / 255.0)

// ── Sidebar item data ───────────────────────────────────────────────

// Type of entry in the sidebar — either a section header (not clickable)
// or a navigable item (clickable).
typedef enum {
    SB_TYPE_HEADER,   // Section header: "DEVICES", "PLACES"
    SB_TYPE_ITEM      // Clickable item: "Home", "Documents", etc.
} SidebarEntryType;

// A single entry in the sidebar list.
typedef struct {
    SidebarEntryType type;       // Header or item
    const char      *label;      // Display text (e.g., "Home", "DEVICES")
    char             path[1024]; // Navigation target (empty for headers)
    const char      *icon_name;  // Freedesktop icon name (e.g., "folder")
    cairo_surface_t *icon;       // Loaded icon surface (16x16), or NULL
} SidebarEntry;

// Maximum sidebar entries — we only have ~10, but leave room for growth
#define SB_MAX_ENTRIES 32

// The sidebar entry list and count
static SidebarEntry entries[SB_MAX_ENTRIES];
static int entry_count = 0;

// Index of the currently selected (highlighted) item, or -1 for none
static int selected_index = -1;

// ── Helper: load a 16x16 icon from the AquaKDE theme ───────────────
//
// Looks for the icon in the places/ or devices/ subdirectory of the
// AquaKDE icon theme at multiple sizes, preferring 64x64 (scaled down
// to 16x16 looks fine for sidebar icons).

static cairo_surface_t *load_sidebar_icon(const char *icon_name,
                                           const char *subdir)
{
    const char *home = getenv("HOME");
    if (!home) return NULL;

    // Search paths — we try several sizes because themes vary in
    // which sizes they actually provide.
    char path[1024];
    const char *sizes[] = { "64x64", "48x48", "32x32", "22x22", "16x16", NULL };

    for (int i = 0; sizes[i]; i++) {
        snprintf(path, sizeof(path),
            "%s/.local/share/icons/AquaKDE-icons/%s/%s/%s.png",
            home, sizes[i], subdir, icon_name);

        cairo_surface_t *surface = cairo_image_surface_create_from_png(path);
        if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
            return surface;
        }
        cairo_surface_destroy(surface);
    }

    // Try hicolor fallback
    for (int i = 0; sizes[i]; i++) {
        snprintf(path, sizeof(path),
            "%s/.local/share/icons/hicolor/%s/%s/%s.png",
            home, sizes[i], subdir, icon_name);

        cairo_surface_t *surface = cairo_image_surface_create_from_png(path);
        if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
            return surface;
        }
        cairo_surface_destroy(surface);
    }

    return NULL;  // Icon not found
}

// ── Helper: Create a fallback folder icon using Cairo ───────────────
//
// When we can't find a theme icon, draw a simple blue folder.

static cairo_surface_t *create_mini_folder_icon(void)
{
    int size = SB_ICON_SIZE;
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surface);

    // Simple folder shape: rectangle with a tab
    double m = 1;
    double w = size - m * 2;
    double h = size - m * 2;
    double tab_w = w * 0.4;
    double tab_h = h * 0.2;

    // Tab on top-left
    cairo_move_to(cr, m, m + tab_h);
    cairo_line_to(cr, m, m);
    cairo_line_to(cr, m + tab_w, m);
    cairo_line_to(cr, m + tab_w + 2, m + tab_h);
    cairo_close_path(cr);

    // Body below
    cairo_rectangle(cr, m, m + tab_h, w, h - tab_h);

    // Fill with Aqua-style blue
    cairo_set_source_rgb(cr, 0.45, 0.68, 0.95);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.3, 0.5, 0.8);
    cairo_set_line_width(cr, 0.5);
    cairo_stroke(cr);

    cairo_destroy(cr);
    return surface;
}

// ── Helper: Create a fallback hard drive icon ───────────────────────
//
// When we can't find a drive icon in the theme, draw a simple one.

static cairo_surface_t *create_mini_drive_icon(void)
{
    int size = SB_ICON_SIZE;
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surface);

    // Rounded rectangle representing a hard drive
    double m = 1;
    double w = size - m * 2;
    double h = size - m * 2;
    double r = 2;

    // Rounded rect path
    cairo_new_sub_path(cr);
    cairo_arc(cr, m + w - r, m + r,     r, -M_PI / 2, 0);
    cairo_arc(cr, m + w - r, m + h - r, r, 0,          M_PI / 2);
    cairo_arc(cr, m + r,     m + h - r, r, M_PI / 2,   M_PI);
    cairo_arc(cr, m + r,     m + r,     r, M_PI,        3 * M_PI / 2);
    cairo_close_path(cr);

    // Silver/grey fill
    cairo_set_source_rgb(cr, 0.8, 0.8, 0.82);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
    cairo_set_line_width(cr, 0.5);
    cairo_stroke(cr);

    // Small LED indicator dot in bottom-right
    cairo_arc(cr, m + w - 4, m + h - 4, 1.5, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 0.3, 0.7, 1.0);
    cairo_fill(cr);

    cairo_destroy(cr);
    return surface;
}

// ── Helper: Create a home icon ──────────────────────────────────────

static cairo_surface_t *create_mini_home_icon(void)
{
    int size = SB_ICON_SIZE;
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surface);

    double cx = size / 2.0;

    // House shape: triangle roof + rectangle body
    // Roof
    cairo_move_to(cr, cx, 2);
    cairo_line_to(cr, size - 2, 8);
    cairo_line_to(cr, 2, 8);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0.6, 0.4, 0.2);
    cairo_fill(cr);

    // Body
    cairo_rectangle(cr, 3, 8, size - 6, size - 10);
    cairo_set_source_rgb(cr, 0.85, 0.75, 0.6);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.5, 0.4, 0.3);
    cairo_set_line_width(cr, 0.5);
    cairo_stroke(cr);

    // Door
    cairo_rectangle(cr, cx - 2, size - 6, 4, 4);
    cairo_set_source_rgb(cr, 0.45, 0.3, 0.15);
    cairo_fill(cr);

    cairo_destroy(cr);
    return surface;
}

// ── Initialization ──────────────────────────────────────────────────
//
// Build the list of sidebar entries. Each entry is either a section
// header (DEVICES, PLACES) or a clickable item with a path target.

void sidebar_init(void)
{
    const char *home = getenv("HOME");
    if (!home) home = "/";

    entry_count = 0;

    // ── DEVICES section ─────────────────────────────────────────

    entries[entry_count++] = (SidebarEntry){
        .type = SB_TYPE_HEADER,
        .label = "DEVICES",
        .path = "",
        .icon_name = NULL,
        .icon = NULL,
    };

    // Macintosh HD → root filesystem
    SidebarEntry *hd = &entries[entry_count++];
    hd->type = SB_TYPE_ITEM;
    hd->label = "Macintosh HD";
    strncpy(hd->path, "/", sizeof(hd->path) - 1);
    hd->icon_name = "drive-harddisk";
    hd->icon = load_sidebar_icon("drive-harddisk", "devices");
    if (!hd->icon) {
        hd->icon = create_mini_drive_icon();
    }

    // ── PLACES section ──────────────────────────────────────────

    entries[entry_count++] = (SidebarEntry){
        .type = SB_TYPE_HEADER,
        .label = "PLACES",
        .path = "",
        .icon_name = NULL,
        .icon = NULL,
    };

    // Helper struct for building PLACES items
    struct {
        const char *label;
        const char *subpath;   // Appended to $HOME (or absolute if starts with /)
        const char *icon_name;
        const char *icon_subdir;
    } places[] = {
        { "Home",         "",              "user-home",           "places"   },
        { "Desktop",      "/Desktop",      "user-desktop",        "places"   },
        { "Applications", NULL,            "folder-applications", "places"   },
        { "Documents",    "/Documents",    "folder-documents",    "places"   },
        { "Downloads",    "/Downloads",    "folder-downloads",    "places"   },
        { "Music",        "/Music",        "folder-music",        "places"   },
        { "Pictures",     "/Pictures",     "folder-pictures",     "places"   },
        { NULL, NULL, NULL, NULL }
    };

    for (int i = 0; places[i].label; i++) {
        if (entry_count >= SB_MAX_ENTRIES) break;

        SidebarEntry *e = &entries[entry_count++];
        e->type = SB_TYPE_ITEM;
        e->label = places[i].label;
        e->icon_name = places[i].icon_name;

        // Build the target path
        if (strcmp(places[i].label, "Applications") == 0) {
            // Applications points to /usr/share/applications
            strncpy(e->path, "/usr/share/applications", sizeof(e->path) - 1);
        } else if (places[i].subpath && places[i].subpath[0] != '\0') {
            snprintf(e->path, sizeof(e->path), "%s%s", home, places[i].subpath);
        } else {
            strncpy(e->path, home, sizeof(e->path) - 1);
        }

        // Try to load the icon from the theme
        e->icon = load_sidebar_icon(places[i].icon_name, places[i].icon_subdir);

        // Fall back to a simple folder icon if not found
        if (!e->icon) {
            if (strcmp(places[i].label, "Home") == 0) {
                e->icon = create_mini_home_icon();
            } else {
                e->icon = create_mini_folder_icon();
            }
        }
    }

    // Select "Home" by default (index 3 — after DEVICES header, HD, PLACES header)
    selected_index = 3;

    fprintf(stderr, "[sidebar] Initialized with %d entries\n", entry_count);
}

// ── Painting ────────────────────────────────────────────────────────

void sidebar_paint(FinderState *fs)
{
    cairo_t *cr = fs->cr;
    int sb_w = fs->sidebar_w;
    int tb_h = fs->toolbar_h;
    int win_h = fs->win_h;

    cairo_save(cr);

    // ── 1. Blue-grey background ─────────────────────────────────
    //
    // This is THE defining visual feature of the Snow Leopard sidebar.
    // The color #DFE5ED is distinctive and immediately recognizable.
    cairo_rectangle(cr, 0, tb_h, sb_w, win_h - tb_h);
    cairo_set_source_rgb(cr, SB_BG_R, SB_BG_G, SB_BG_B);
    cairo_fill(cr);

    // ── 2. Right edge separator line ────────────────────────────
    //
    // A 1px vertical line on the right edge separates the sidebar
    // from the content area.
    cairo_set_source_rgb(cr, SB_SEP_R, SB_SEP_G, SB_SEP_B);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, sb_w - 0.5, tb_h);
    cairo_line_to(cr, sb_w - 0.5, win_h);
    cairo_stroke(cr);

    // ── 3. Draw each entry ──────────────────────────────────────
    //
    // Walk through the entry list, drawing headers and items.
    // The Y position is tracked incrementally as we go.
    int y = tb_h + 8;  // Start 8px below the toolbar

    for (int i = 0; i < entry_count; i++) {
        SidebarEntry *e = &entries[i];

        if (e->type == SB_TYPE_HEADER) {
            // ── Section header ──────────────────────────────────
            //
            // ALL CAPS, small bold font, grey color.
            // Headers are not clickable.

            // Add extra space before headers (except the first one)
            if (i > 0) y += 8;

            PangoLayout *layout = pango_cairo_create_layout(cr);
            pango_layout_set_text(layout, e->label, -1);
            PangoFontDescription *font = pango_font_description_from_string(
                "Lucida Grande Bold 9");
            pango_layout_set_font_description(layout, font);
            pango_font_description_free(font);

            cairo_move_to(cr, SB_PAD_LEFT, y + 4);
            cairo_set_source_rgb(cr, 0x8C / 255.0, 0x8C / 255.0, 0x8C / 255.0);
            pango_cairo_show_layout(cr, layout);
            g_object_unref(layout);

            y += SB_HEADER_H;

        } else {
            // ── Clickable item ──────────────────────────────────
            //
            // Each item has: optional selection highlight, 16x16 icon,
            // and text label.

            // Draw selection highlight if this item is selected
            if (i == selected_index) {
                // Aqua blue rounded rectangle highlight
                double rx = 4;
                double ry = y;
                double rw = sb_w - 8;
                double rh = SB_ITEM_H;
                double rr = 4;  // Small corner radius

                cairo_new_sub_path(cr);
                cairo_arc(cr, rx + rw - rr, ry + rr,      rr, -M_PI / 2, 0);
                cairo_arc(cr, rx + rw - rr, ry + rh - rr, rr, 0,          M_PI / 2);
                cairo_arc(cr, rx + rr,      ry + rh - rr, rr, M_PI / 2,   M_PI);
                cairo_arc(cr, rx + rr,      ry + rr,      rr, M_PI,        3 * M_PI / 2);
                cairo_close_path(cr);

                cairo_set_source_rgb(cr, SB_SEL_R, SB_SEL_G, SB_SEL_B);
                cairo_fill(cr);
            }

            // Draw the icon (16x16, positioned to the left of the text)
            int icon_x = SB_PAD_LEFT;
            int icon_y = y + (SB_ITEM_H - SB_ICON_SIZE) / 2;

            if (e->icon) {
                // Scale the icon down to 16x16 if needed
                int src_w = cairo_image_surface_get_width(e->icon);
                int src_h = cairo_image_surface_get_height(e->icon);

                cairo_save(cr);
                if (src_w != SB_ICON_SIZE || src_h != SB_ICON_SIZE) {
                    double sx = (double)SB_ICON_SIZE / src_w;
                    double sy = (double)SB_ICON_SIZE / src_h;
                    cairo_translate(cr, icon_x, icon_y);
                    cairo_scale(cr, sx, sy);
                    cairo_set_source_surface(cr, e->icon, 0, 0);
                } else {
                    cairo_set_source_surface(cr, e->icon, icon_x, icon_y);
                }
                cairo_paint(cr);
                cairo_restore(cr);
            }

            // Draw the text label
            int text_x = SB_PAD_LEFT + SB_ICON_SIZE + SB_ICON_PAD;
            int text_y = y + 4;

            PangoLayout *layout = pango_cairo_create_layout(cr);
            pango_layout_set_text(layout, e->label, -1);
            PangoFontDescription *font = pango_font_description_from_string(
                "Lucida Grande 11");
            pango_layout_set_font_description(layout, font);
            pango_font_description_free(font);

            // Text color: white if selected, dark grey otherwise
            if (i == selected_index) {
                cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            } else {
                cairo_set_source_rgb(cr, 0x1A / 255.0, 0x1A / 255.0, 0x1A / 255.0);
            }

            cairo_move_to(cr, text_x, text_y);
            pango_cairo_show_layout(cr, layout);
            g_object_unref(layout);

            y += SB_ITEM_H;
        }
    }

    cairo_restore(cr);
}

// ── Click handling ──────────────────────────────────────────────────

bool sidebar_handle_click(FinderState *fs, int x, int y)
{
    (void)x;  // We only care about vertical position for sidebar clicks

    int tb_h = fs->toolbar_h;
    int cur_y = tb_h + 8;  // Must match the Y tracking in sidebar_paint

    for (int i = 0; i < entry_count; i++) {
        SidebarEntry *e = &entries[i];

        if (e->type == SB_TYPE_HEADER) {
            // Headers take up space but aren't clickable
            if (i > 0) cur_y += 8;
            cur_y += SB_HEADER_H;
        } else {
            // Check if the click landed on this item
            if (y >= cur_y && y < cur_y + SB_ITEM_H) {
                // Select this item and navigate to its path
                selected_index = i;
                finder_navigate(fs, e->path);
                fprintf(stderr, "[sidebar] Selected: %s → %s\n",
                        e->label, e->path);
                return true;
            }
            cur_y += SB_ITEM_H;
        }
    }

    return false;  // Click missed all items
}

// ── Shutdown ────────────────────────────────────────────────────────

void sidebar_shutdown(void)
{
    // Free all loaded icon surfaces
    for (int i = 0; i < entry_count; i++) {
        if (entries[i].icon) {
            cairo_surface_destroy(entries[i].icon);
            entries[i].icon = NULL;
        }
    }
    entry_count = 0;
    selected_index = -1;
}
