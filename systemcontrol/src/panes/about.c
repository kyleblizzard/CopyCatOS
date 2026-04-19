// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// panes/about.c — About MoonBase pane
// ============================================================================
//
// Shows the installed libmoonbase.so.1 runtime version, pulled from the
// `moonbase-version` CLI helper that ships with moonbase. The CLI prints
// MAJOR.MINOR.PATCH from moonbase_runtime_version(); one fork per session
// is cheap and keeps systemcontrol from having to link libmoonbase itself.
//
// Layout mirrors the stub pane: centered icon + name + supporting text.
// The runtime string is cached on first paint so repaints don't re-fork.
// ============================================================================

#include "about.h"
#include "../registry.h"

#include <stdio.h>
#include <string.h>

// Cached version string — populated on first paint. "unknown" if the CLI
// is missing or errored; the pane still draws cleanly in that case.
static char g_runtime[64] = {0};

// Shell out to `moonbase-version` and capture the first line of stdout.
// Reason this isn't a direct link against libmoonbase: systemcontrol does
// not otherwise depend on moonbase, and the CLI already exists for this
// exact purpose (menubar/src/apple.c uses the same pattern).
static void load_runtime_version(void)
{
    if (g_runtime[0] != '\0') return;

    FILE *fp = popen("moonbase-version 2>/dev/null", "r");
    if (!fp) {
        strncpy(g_runtime, "unknown", sizeof(g_runtime) - 1);
        return;
    }

    if (fgets(g_runtime, sizeof(g_runtime), fp) == NULL) {
        strncpy(g_runtime, "unknown", sizeof(g_runtime) - 1);
    } else {
        // Strip trailing newline/whitespace
        size_t len = strlen(g_runtime);
        while (len > 0 && (g_runtime[len - 1] == '\n' ||
                           g_runtime[len - 1] == '\r' ||
                           g_runtime[len - 1] == ' ')) {
            g_runtime[--len] = '\0';
        }
        if (len == 0) {
            strncpy(g_runtime, "unknown", sizeof(g_runtime) - 1);
        }
    }

    pclose(fp);
}

// ============================================================================
// about_paint — Render the About MoonBase pane
// ============================================================================
void about_paint(SysPrefsState *state, int pane_index)
{
    cairo_t *cr = state->cr;
    PaneInfo *pane = &state->panes[pane_index];

    load_runtime_version();

    // Lazy-load the 128x128 icon if needed
    registry_load_icon_128(state, pane_index);

    // Content area starts below the toolbar, centered horizontally
    double content_y = TOOLBAR_HEIGHT;
    double content_h = state->win_h - TOOLBAR_HEIGHT;
    double center_x = state->win_w / 2.0;
    double center_y = content_y + content_h / 2.0 - 40;

    // ── 128x128 icon ────────────────────────────────────────────────────
    if (pane->icon_128) {
        double icon_x = center_x - 64;
        double icon_y = center_y - 80;

        cairo_set_source_surface(cr, pane->icon_128, icon_x, icon_y);
        cairo_paint(cr);
    }

    // ── "MoonBase" title (bold, centered) ───────────────────────────────
    PangoLayout *title_layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(title_layout, "MoonBase", -1);
    PangoFontDescription *title_font =
        pango_font_description_from_string("Lucida Grande Bold 17");
    pango_layout_set_font_description(title_layout, title_font);
    pango_font_description_free(title_font);

    int title_w, title_h;
    pango_layout_get_pixel_size(title_layout, &title_w, &title_h);

    cairo_set_source_rgb(cr, 0x1A / 255.0, 0x1A / 255.0, 0x1A / 255.0);
    cairo_move_to(cr, center_x - title_w / 2.0, center_y + 60);
    pango_cairo_show_layout(cr, title_layout);
    g_object_unref(title_layout);

    // ── Subtitle: "Application Framework" ───────────────────────────────
    PangoLayout *sub_layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(sub_layout, "Application Framework", -1);
    PangoFontDescription *sub_font =
        pango_font_description_from_string("Lucida Grande 11");
    pango_layout_set_font_description(sub_layout, sub_font);
    pango_font_description_free(sub_font);

    int sub_w, sub_h;
    pango_layout_get_pixel_size(sub_layout, &sub_w, &sub_h);

    cairo_set_source_rgb(cr, 0x73 / 255.0, 0x73 / 255.0, 0x73 / 255.0);
    cairo_move_to(cr, center_x - sub_w / 2.0, center_y + 60 + title_h + 2);
    pango_cairo_show_layout(cr, sub_layout);
    g_object_unref(sub_layout);

    // ── Runtime version line ────────────────────────────────────────────
    char version_line[128];
    snprintf(version_line, sizeof(version_line), "Runtime v%s", g_runtime);

    PangoLayout *ver_layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(ver_layout, version_line, -1);
    PangoFontDescription *ver_font =
        pango_font_description_from_string("Lucida Grande 13");
    pango_layout_set_font_description(ver_layout, ver_font);
    pango_font_description_free(ver_font);

    int ver_w, ver_h;
    pango_layout_get_pixel_size(ver_layout, &ver_w, &ver_h);
    (void)ver_h;

    cairo_set_source_rgb(cr, 0x1A / 255.0, 0x1A / 255.0, 0x1A / 255.0);
    cairo_move_to(cr, center_x - ver_w / 2.0,
                  center_y + 60 + title_h + 2 + sub_h + 14);
    pango_cairo_show_layout(cr, ver_layout);
    g_object_unref(ver_layout);

    // ── Copyright line ──────────────────────────────────────────────────
    PangoLayout *copy_layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(copy_layout,
        "by Kyle Blizzard @ Blizzard.show", -1);
    PangoFontDescription *copy_font =
        pango_font_description_from_string("Lucida Grande 11");
    pango_layout_set_font_description(copy_layout, copy_font);
    pango_font_description_free(copy_font);

    int copy_w, copy_h;
    pango_layout_get_pixel_size(copy_layout, &copy_w, &copy_h);
    (void)copy_h;

    cairo_set_source_rgb(cr, 0x73 / 255.0, 0x73 / 255.0, 0x73 / 255.0);
    cairo_move_to(cr, center_x - copy_w / 2.0,
                  center_y + 60 + title_h + 2 + sub_h + 14 + ver_h + 10);
    pango_cairo_show_layout(cr, copy_layout);
    g_object_unref(copy_layout);
}
