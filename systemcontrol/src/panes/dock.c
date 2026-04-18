// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// panes/dock.c — Dock & Display preferences pane
// ============================================================================
//
// Provides slider controls for dock icon size and menubar height.
// Modeled after the real Snow Leopard Dock preferences pane layout.
//
// Changes are written to ~/.config/copycatos/desktop.conf and applied
// live by sending SIGHUP to the running dock and menubar processes.
// ============================================================================

#include "dock.h"
#include "../registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <math.h>
#include <sys/stat.h>

// ============================================================================
//  Slider geometry constants
// ============================================================================

#define SLIDER_LEFT       120   // X position where slider track starts
#define SLIDER_RIGHT      520   // X position where slider track ends
#define SLIDER_TRACK_W    (SLIDER_RIGHT - SLIDER_LEFT)
#define KNOB_RADIUS        8    // Radius of the slider knob
#define LABEL_X            30   // X position for labels

// ============================================================================
//  State — current slider values and drag state
// ============================================================================

// Slider IDs
#define SLIDER_DOCK_SIZE    0
#define SLIDER_MENUBAR_H    1
#define SLIDER_NONE        -1

static int dock_icon_size    = 64;   // Current dock icon size (32-128)
static int menubar_h         = 22;   // Current menubar height (22-44)
static int dragging_slider   = SLIDER_NONE;  // Which slider is being dragged

// ============================================================================
//  Config reading
// ============================================================================

// Read current values from the shared config file
static void read_config(void)
{
    const char *home = getenv("HOME");
    if (!home) return;

    char path[512];
    snprintf(path, sizeof(path), "%s/.config/copycatos/desktop.conf", home);

    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[256];
    char section[32] = "";
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') {
            // Parse section name
            char *end = strchr(p, ']');
            if (end) {
                int len = (int)(end - p - 1);
                if (len > 0 && len < (int)sizeof(section)) {
                    strncpy(section, p + 1, len);
                    section[len] = '\0';
                }
            }
        } else if (strcmp(section, "dock") == 0 && strncmp(p, "icon_size=", 10) == 0) {
            dock_icon_size = atoi(p + 10);
            if (dock_icon_size < 32)  dock_icon_size = 32;
            if (dock_icon_size > 196) dock_icon_size = 196;
        } else if (strcmp(section, "menubar") == 0 && strncmp(p, "height=", 7) == 0) {
            menubar_h = atoi(p + 7);
            if (menubar_h < 22) menubar_h = 22;
            if (menubar_h > 88) menubar_h = 88;
        }
    }
    fclose(fp);
}

// ============================================================================
//  Config writing + live apply
// ============================================================================

// Write current values to config and send SIGHUP to dock/menubar
static void apply_config(void)
{
    const char *home = getenv("HOME");
    if (!home) return;

    // Ensure directory exists
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config/copycatos", home);
    mkdir(dir, 0755);

    // Write the config file
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/copycatos/desktop.conf", home);

    FILE *fp = fopen(path, "w");
    if (!fp) return;

    fprintf(fp, "[dock]\n");
    fprintf(fp, "icon_size=%d\n", dock_icon_size);
    fprintf(fp, "\n");
    fprintf(fp, "[menubar]\n");
    fprintf(fp, "height=%d\n", menubar_h);
    fprintf(fp, "\n");
    fclose(fp);

    // Send SIGHUP to dock and menubar for live reload
    FILE *proc;
    char pid_buf[32];

    proc = popen("pgrep dock", "r");
    if (proc) {
        if (fgets(pid_buf, sizeof(pid_buf), proc)) {
            kill(atoi(pid_buf), SIGHUP);
        }
        pclose(proc);
    }

    proc = popen("pgrep menubar", "r");
    if (proc) {
        if (fgets(pid_buf, sizeof(pid_buf), proc)) {
            kill(atoi(pid_buf), SIGHUP);
        }
        pclose(proc);
    }
}

// ============================================================================
//  Slider drawing helper
// ============================================================================

// Draw a horizontal slider with label, value text, and knob
static void draw_slider(cairo_t *cr, double y,
                         const char *label,
                         const char *min_label, const char *max_label,
                         int value, int min_val, int max_val)
{
    // ── Label on the left ──────────────────────────────────────────
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, label, -1);
    PangoFontDescription *font =
        pango_font_description_from_string("Lucida Grande Bold 12");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_move_to(cr, LABEL_X, y - 7);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);

    // ── Track (gray line) ──────────────────────────────────────────
    double track_y = y;
    cairo_set_source_rgb(cr, 0.72, 0.72, 0.72);
    cairo_set_line_width(cr, 2.0);
    cairo_move_to(cr, SLIDER_LEFT, track_y);
    cairo_line_to(cr, SLIDER_RIGHT, track_y);
    cairo_stroke(cr);

    // ── Min/Max labels ─────────────────────────────────────────────
    PangoLayout *min_lay = pango_cairo_create_layout(cr);
    pango_layout_set_text(min_lay, min_label, -1);
    PangoFontDescription *small_font =
        pango_font_description_from_string("Lucida Grande 10");
    pango_layout_set_font_description(min_lay, small_font);

    cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
    cairo_move_to(cr, SLIDER_LEFT, y + 6);
    pango_cairo_show_layout(cr, min_lay);
    g_object_unref(min_lay);

    PangoLayout *max_lay = pango_cairo_create_layout(cr);
    pango_layout_set_text(max_lay, max_label, -1);
    pango_layout_set_font_description(max_lay, small_font);

    int max_w;
    pango_layout_get_pixel_size(max_lay, &max_w, NULL);
    cairo_move_to(cr, SLIDER_RIGHT - max_w, y + 6);
    pango_cairo_show_layout(cr, max_lay);
    g_object_unref(max_lay);
    pango_font_description_free(small_font);

    // ── Knob position ──────────────────────────────────────────────
    double frac = (double)(value - min_val) / (max_val - min_val);
    double knob_x = SLIDER_LEFT + frac * SLIDER_TRACK_W;

    // Knob shadow
    cairo_set_source_rgba(cr, 0, 0, 0, 0.15);
    cairo_arc(cr, knob_x, track_y + 1, KNOB_RADIUS + 1, 0, 2 * M_PI);
    cairo_fill(cr);

    // Knob body (white with gray border, SL-style)
    cairo_set_source_rgb(cr, 0.98, 0.98, 0.98);
    cairo_arc(cr, knob_x, track_y, KNOB_RADIUS, 0, 2 * M_PI);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
    cairo_set_line_width(cr, 1.0);
    cairo_arc(cr, knob_x, track_y, KNOB_RADIUS, 0, 2 * M_PI);
    cairo_stroke(cr);

    // ── Current value text ─────────────────────────────────────────
    char val_buf[16];
    snprintf(val_buf, sizeof(val_buf), "%d", value);

    PangoLayout *val_lay = pango_cairo_create_layout(cr);
    pango_layout_set_text(val_lay, val_buf, -1);
    PangoFontDescription *val_font =
        pango_font_description_from_string("Lucida Grande 10");
    pango_layout_set_font_description(val_lay, val_font);
    pango_font_description_free(val_font);

    int val_w;
    pango_layout_get_pixel_size(val_lay, &val_w, NULL);
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_move_to(cr, SLIDER_RIGHT + 12, y - 6);
    pango_cairo_show_layout(cr, val_lay);
    g_object_unref(val_lay);
}

// ============================================================================
//  Public API
// ============================================================================

void dock_pane_paint(SysPrefsState *state)
{
    cairo_t *cr = state->cr;

    // Read current config on first paint
    static bool config_read = false;
    if (!config_read) {
        read_config();
        config_read = true;
    }

    // Content area starts below the toolbar
    double content_y = TOOLBAR_HEIGHT + 20;

    // ── Title ──────────────────────────────────────────────────────
    PangoLayout *title = pango_cairo_create_layout(cr);
    pango_layout_set_text(title, "Dock & Menu Bar", -1);
    PangoFontDescription *title_font =
        pango_font_description_from_string("Lucida Grande Bold 15");
    pango_layout_set_font_description(title, title_font);
    pango_font_description_free(title_font);

    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_move_to(cr, LABEL_X, content_y);
    pango_cairo_show_layout(cr, title);
    g_object_unref(title);

    // ── Separator line ─────────────────────────────────────────────
    content_y += 30;
    cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, LABEL_X, content_y + 0.5);
    cairo_line_to(cr, SLIDER_RIGHT + 50, content_y + 0.5);
    cairo_stroke(cr);

    // ── Dock Size slider ───────────────────────────────────────────
    content_y += 35;
    draw_slider(cr, content_y,
                "Dock Size:", "Small", "Large",
                dock_icon_size, 32, 196);

    // ── Menubar Height slider ──────────────────────────────────────
    content_y += 60;
    draw_slider(cr, content_y,
                "Menu Bar:", "Normal", "Touch",
                menubar_h, 22, 88);

    // ── Description text ───────────────────────────────────────────
    content_y += 50;
    PangoLayout *desc = pango_cairo_create_layout(cr);
    pango_layout_set_text(desc,
        "Drag the sliders to resize the Dock icons and\n"
        "Menu Bar height. Changes apply immediately.", -1);
    PangoFontDescription *desc_font =
        pango_font_description_from_string("Lucida Grande 11");
    pango_layout_set_font_description(desc, desc_font);
    pango_font_description_free(desc_font);

    cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
    cairo_move_to(cr, LABEL_X, content_y);
    pango_cairo_show_layout(cr, desc);
    g_object_unref(desc);
}

// ============================================================================
//  Slider hit testing
// ============================================================================

// Check if (x, y) is on a slider knob. Returns slider ID or SLIDER_NONE.
static int hit_test_slider(int x, int y)
{
    double content_y = TOOLBAR_HEIGHT + 20 + 30 + 35;

    // Dock size slider
    {
        double frac = (double)(dock_icon_size - 32) / (196 - 32);
        double knob_x = SLIDER_LEFT + frac * SLIDER_TRACK_W;
        double dx = x - knob_x;
        double dy = y - content_y;
        if (dx * dx + dy * dy <= (KNOB_RADIUS + 4) * (KNOB_RADIUS + 4)) {
            return SLIDER_DOCK_SIZE;
        }
        // Also allow clicking anywhere on the track
        if (y >= content_y - KNOB_RADIUS - 2 && y <= content_y + KNOB_RADIUS + 2 &&
            x >= SLIDER_LEFT - 4 && x <= SLIDER_RIGHT + 4) {
            return SLIDER_DOCK_SIZE;
        }
    }

    // Menubar height slider
    content_y += 60;
    {
        double frac = (double)(menubar_h - 22) / (88 - 22);
        double knob_x = SLIDER_LEFT + frac * SLIDER_TRACK_W;
        double dx = x - knob_x;
        double dy = y - content_y;
        if (dx * dx + dy * dy <= (KNOB_RADIUS + 4) * (KNOB_RADIUS + 4)) {
            return SLIDER_MENUBAR_H;
        }
        if (y >= content_y - KNOB_RADIUS - 2 && y <= content_y + KNOB_RADIUS + 2 &&
            x >= SLIDER_LEFT - 4 && x <= SLIDER_RIGHT + 4) {
            return SLIDER_MENUBAR_H;
        }
    }

    return SLIDER_NONE;
}

// Convert an X pixel position on the slider track to a value
static int x_to_value(int x, int min_val, int max_val)
{
    if (x <= SLIDER_LEFT) return min_val;
    if (x >= SLIDER_RIGHT) return max_val;
    double frac = (double)(x - SLIDER_LEFT) / SLIDER_TRACK_W;
    return min_val + (int)(frac * (max_val - min_val) + 0.5);
}

bool dock_pane_click(SysPrefsState *state, int x, int y)
{
    (void)state;
    int slider = hit_test_slider(x, y);
    if (slider != SLIDER_NONE) {
        dragging_slider = slider;

        // Immediately update value to click position
        if (slider == SLIDER_DOCK_SIZE) {
            dock_icon_size = x_to_value(x, 32, 196);
        } else if (slider == SLIDER_MENUBAR_H) {
            menubar_h = x_to_value(x, 22, 88);
        }

        return true;
    }
    return false;
}

bool dock_pane_motion(SysPrefsState *state, int x, int y)
{
    (void)state;
    (void)y;
    if (dragging_slider == SLIDER_NONE) return false;

    if (dragging_slider == SLIDER_DOCK_SIZE) {
        dock_icon_size = x_to_value(x, 32, 196);
    } else if (dragging_slider == SLIDER_MENUBAR_H) {
        menubar_h = x_to_value(x, 22, 88);
    }

    return true; // Needs repaint
}

void dock_pane_release(SysPrefsState *state)
{
    (void)state;
    if (dragging_slider != SLIDER_NONE) {
        dragging_slider = SLIDER_NONE;
        // Write config and send SIGHUP on release
        apply_config();
    }
}
