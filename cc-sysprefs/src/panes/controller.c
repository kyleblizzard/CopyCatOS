// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// panes/controller.c — Controller / Mouse preferences pane
// ============================================================================
//
// Provides slider controls for gamepad right-stick mouse sensitivity and
// deadzone, read-only display of trigger assignments and default button
// mappings. Modeled after the Snow Leopard Mouse preferences layout.
//
// Changes are written to ~/.config/copicatos/input.conf and applied
// live by sending SIGHUP to the running cc-inputd process.
// ============================================================================

#include "controller.h"
#include "../registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <math.h>
#include <sys/stat.h>

// ============================================================================
//  Slider geometry constants (same as dock.c for visual consistency)
// ============================================================================

#define SLIDER_LEFT       120   // X position where slider track starts
#define SLIDER_RIGHT      520   // X position where slider track ends
#define SLIDER_TRACK_W    (SLIDER_RIGHT - SLIDER_LEFT)
#define KNOB_RADIUS        8    // Radius of the slider knob
#define LABEL_X            30   // X position for labels

// ============================================================================
//  State — current slider values and drag state
// ============================================================================

// Slider IDs for identifying which slider the user is interacting with
#define SLIDER_SENSITIVITY  0
#define SLIDER_DEADZONE     1
#define SLIDER_NONE        -1

// Current config values — these are read from input.conf on first paint
// and updated as the user drags the sliders
static double sensitivity    = 5.0;    // Right stick sensitivity (1.0 - 10.0)
static int    deadzone       = 4000;   // Right stick deadzone (1000 - 8000)
static int    dragging_slider = SLIDER_NONE;  // Which slider is being dragged

// ============================================================================
//  Config reading
// ============================================================================

// Read current values from the input config file.
// The file uses INI format with [mouse] and [triggers] sections.
static void read_config(void)
{
    const char *home = getenv("HOME");
    if (!home) return;

    char path[512];
    snprintf(path, sizeof(path), "%s/.config/copicatos/input.conf", home);

    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[256];
    char section[32] = "";

    while (fgets(line, sizeof(line), fp)) {
        // Skip leading whitespace
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        // Parse section headers like [mouse]
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end) {
                int len = (int)(end - p - 1);
                if (len > 0 && len < (int)sizeof(section)) {
                    strncpy(section, p + 1, len);
                    section[len] = '\0';
                }
            }
        }
        // Parse key=value pairs within the [mouse] section
        else if (strcmp(section, "mouse") == 0) {
            if (strncmp(p, "sensitivity=", 12) == 0) {
                sensitivity = atof(p + 12);
                if (sensitivity < 1.0)  sensitivity = 1.0;
                if (sensitivity > 10.0) sensitivity = 10.0;
            } else if (strncmp(p, "deadzone=", 9) == 0) {
                deadzone = atoi(p + 9);
                if (deadzone < 1000) deadzone = 1000;
                if (deadzone > 8000) deadzone = 8000;
            }
        }
    }
    fclose(fp);
}

// ============================================================================
//  Config writing + live apply
// ============================================================================

// Write current values to config and send SIGHUP to cc-inputd.
// This preserves the full file structure including [triggers] and [power]
// sections that may already exist.
static void apply_config(void)
{
    const char *home = getenv("HOME");
    if (!home) return;

    // Ensure the config directory exists
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config/copicatos", home);
    mkdir(dir, 0755);

    // Read the existing file to preserve sections we don't manage here.
    // We'll rewrite [mouse] but keep everything else intact.
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/copicatos/input.conf", home);

    // Read existing content to preserve [triggers], [power], etc.
    char existing[4096] = "";
    char triggers_block[1024] = "";
    char power_block[1024] = "";

    FILE *fp = fopen(path, "r");
    if (fp) {
        char line[256];
        char section[32] = "";
        // We'll capture [triggers] and [power] sections to preserve them
        char *write_target = NULL;

        while (fgets(line, sizeof(line), fp)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;

            if (*p == '[') {
                char *end = strchr(p, ']');
                if (end) {
                    int len = (int)(end - p - 1);
                    if (len > 0 && len < (int)sizeof(section)) {
                        strncpy(section, p + 1, len);
                        section[len] = '\0';
                    }
                }
                // Direct subsequent lines to the right buffer
                if (strcmp(section, "triggers") == 0) {
                    write_target = triggers_block;
                    strncat(triggers_block, line, sizeof(triggers_block) - strlen(triggers_block) - 1);
                } else if (strcmp(section, "power") == 0) {
                    write_target = power_block;
                    strncat(power_block, line, sizeof(power_block) - strlen(power_block) - 1);
                } else {
                    write_target = NULL;
                }
            } else if (write_target) {
                size_t tgt_len = strlen(write_target);
                size_t max_len = (write_target == triggers_block)
                    ? sizeof(triggers_block) : sizeof(power_block);
                strncat(write_target, line, max_len - tgt_len - 1);
            }
        }
        fclose(fp);
    }

    // Write the updated config
    fp = fopen(path, "w");
    if (!fp) return;

    fprintf(fp, "[mouse]\n");
    fprintf(fp, "sensitivity=%.1f\n", sensitivity);
    fprintf(fp, "deadzone=%d\n", deadzone);
    fprintf(fp, "\n");

    // Preserve [triggers] section if it existed, otherwise write defaults
    if (triggers_block[0] != '\0') {
        fprintf(fp, "%s\n", triggers_block);
    } else {
        fprintf(fp, "[triggers]\n");
        fprintf(fp, "right=left_click\n");
        fprintf(fp, "left=right_click\n");
        fprintf(fp, "\n");
    }

    // Preserve [power] section if it existed
    if (power_block[0] != '\0') {
        fprintf(fp, "%s\n", power_block);
    }

    fclose(fp);

    // Send SIGHUP to cc-inputd so it reloads the config immediately.
    // pgrep finds the process by name, then we signal it.
    FILE *proc = popen("pgrep cc-inputd", "r");
    if (proc) {
        char pid_buf[32];
        if (fgets(pid_buf, sizeof(pid_buf), proc)) {
            kill(atoi(pid_buf), SIGHUP);
        }
        pclose(proc);
    }
}

// ============================================================================
//  Slider drawing helper
// ============================================================================

// Draw a horizontal slider with label, min/max labels, and a draggable knob.
// This is the same visual style used in the Dock pane — white knob with
// gray border on a thin gray track.
static void draw_slider(cairo_t *cr, double y,
                         const char *label,
                         const char *min_label, const char *max_label,
                         double value, double min_val, double max_val)
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
    // Map the current value to a pixel position along the track
    double frac = (value - min_val) / (max_val - min_val);
    double knob_x = SLIDER_LEFT + frac * SLIDER_TRACK_W;

    // Knob shadow (subtle drop shadow for depth)
    cairo_set_source_rgba(cr, 0, 0, 0, 0.15);
    cairo_arc(cr, knob_x, track_y + 1, KNOB_RADIUS + 1, 0, 2 * M_PI);
    cairo_fill(cr);

    // Knob body (white with gray border, Snow Leopard style)
    cairo_set_source_rgb(cr, 0.98, 0.98, 0.98);
    cairo_arc(cr, knob_x, track_y, KNOB_RADIUS, 0, 2 * M_PI);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
    cairo_set_line_width(cr, 1.0);
    cairo_arc(cr, knob_x, track_y, KNOB_RADIUS, 0, 2 * M_PI);
    cairo_stroke(cr);

    // ── Current value text (displayed to the right of the track) ──
    char val_buf[16];
    // Show one decimal place for sensitivity, whole number for deadzone
    if (max_val <= 10.0) {
        snprintf(val_buf, sizeof(val_buf), "%.1f", value);
    } else {
        snprintf(val_buf, sizeof(val_buf), "%d", (int)value);
    }

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
//  Section drawing helpers
// ============================================================================

// Draw a separator line across the pane (used between sections)
static void draw_separator(cairo_t *cr, double y)
{
    cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, LABEL_X, y + 0.5);
    cairo_line_to(cr, SLIDER_RIGHT + 50, y + 0.5);
    cairo_stroke(cr);
}

// Draw a section title in bold (e.g. "Right Stick — Mouse")
static void draw_section_title(cairo_t *cr, double x, double y, const char *text)
{
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text, -1);
    PangoFontDescription *font =
        pango_font_description_from_string("Lucida Grande Bold 12");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

// Draw an info label in regular weight (e.g. "Right Trigger: Left Click")
static void draw_info_label(cairo_t *cr, double x, double y, const char *text)
{
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text, -1);
    PangoFontDescription *font =
        pango_font_description_from_string("Lucida Grande 11");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

// ============================================================================
//  Restore Defaults button geometry
// ============================================================================

// Button rectangle — used for both painting and hit testing
#define BTN_X       LABEL_X
#define BTN_W       140
#define BTN_H       24

// The Y position is computed dynamically during paint, so we store it
static double restore_btn_y = 0;

// Draw a rounded-rect push button (Snow Leopard style)
static void draw_button(cairo_t *cr, double x, double y,
                         double w, double h, const char *label)
{
    double r = 4.0;  // Corner radius

    // Button background (light gradient feel)
    cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_new_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_fill(cr);

    // Button border
    cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
    cairo_set_line_width(cr, 1.0);
    cairo_new_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_stroke(cr);

    // Button text (centered)
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, label, -1);
    PangoFontDescription *font =
        pango_font_description_from_string("Lucida Grande 11");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    int tw, th;
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_move_to(cr, x + (w - tw) / 2, y + (h - th) / 2);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

// ============================================================================
//  Public API — Paint
// ============================================================================

void controller_pane_paint(SysPrefsState *state)
{
    cairo_t *cr = state->cr;

    // Read config from disk on first paint
    static bool config_read = false;
    if (!config_read) {
        read_config();
        config_read = true;
    }

    // Content area starts below the toolbar
    double content_y = TOOLBAR_HEIGHT + 20;

    // ── Title ──────────────────────────────────────────────────────
    PangoLayout *title = pango_cairo_create_layout(cr);
    pango_layout_set_text(title, "Controller", -1);
    PangoFontDescription *title_font =
        pango_font_description_from_string("Lucida Grande Bold 15");
    pango_layout_set_font_description(title, title_font);
    pango_font_description_free(title_font);

    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_move_to(cr, LABEL_X, content_y);
    pango_cairo_show_layout(cr, title);
    g_object_unref(title);

    // ── Separator below title ─────────────────────────────────────
    content_y += 30;
    draw_separator(cr, content_y);

    // ── Section: Right Stick — Mouse ──────────────────────────────
    content_y += 15;
    draw_section_title(cr, LABEL_X, content_y, "Right Stick \xe2\x80\x94 Mouse");

    // Sensitivity slider
    content_y += 30;
    draw_slider(cr, content_y,
                "Sensitivity:", "Slow", "Fast",
                sensitivity, 1.0, 10.0);

    // Deadzone slider
    content_y += 60;
    draw_slider(cr, content_y,
                "Deadzone:", "Small", "Large",
                (double)deadzone, 1000.0, 8000.0);

    // ── Separator ─────────────────────────────────────────────────
    content_y += 40;
    draw_separator(cr, content_y);

    // ── Section: Triggers ─────────────────────────────────────────
    content_y += 15;
    draw_section_title(cr, LABEL_X, content_y, "Triggers");

    content_y += 22;
    draw_info_label(cr, LABEL_X + 20, content_y, "Right Trigger:  Left Click");
    content_y += 20;
    draw_info_label(cr, LABEL_X + 20, content_y, "Left Trigger:   Right Click");

    // ── Separator ─────────────────────────────────────────────────
    content_y += 30;
    draw_separator(cr, content_y);

    // ── Section: Default Buttons (read-only) ──────────────────────
    content_y += 15;
    draw_section_title(cr, LABEL_X, content_y, "Default Buttons");

    content_y += 22;
    draw_info_label(cr, LABEL_X + 20, content_y, "A = Enter");
    content_y += 18;
    draw_info_label(cr, LABEL_X + 20, content_y, "B = Escape");
    content_y += 18;
    draw_info_label(cr, LABEL_X + 20, content_y, "X = Spotlight");
    content_y += 18;
    draw_info_label(cr, LABEL_X + 20, content_y, "Y = Mission Control");

    // ── Restore Defaults button ───────────────────────────────────
    content_y += 35;
    restore_btn_y = content_y;
    draw_button(cr, BTN_X, content_y, BTN_W, BTN_H, "Restore Defaults");
}

// ============================================================================
//  Slider hit testing
// ============================================================================

// Check if (x, y) is on one of the two slider knobs or tracks.
// Returns the slider ID or SLIDER_NONE if no hit.
static int hit_test_slider(int x, int y)
{
    // The Y positions must match the paint layout exactly:
    // title(+30) + separator(+15) + section_title(+30) = sensitivity slider Y
    double content_y = TOOLBAR_HEIGHT + 20 + 30 + 15 + 30;

    // Sensitivity slider
    {
        double frac = (sensitivity - 1.0) / (10.0 - 1.0);
        double knob_x = SLIDER_LEFT + frac * SLIDER_TRACK_W;
        double dx = x - knob_x;
        double dy = y - content_y;
        if (dx * dx + dy * dy <= (KNOB_RADIUS + 4) * (KNOB_RADIUS + 4)) {
            return SLIDER_SENSITIVITY;
        }
        // Allow clicking anywhere on the track
        if (y >= content_y - KNOB_RADIUS - 2 && y <= content_y + KNOB_RADIUS + 2 &&
            x >= SLIDER_LEFT - 4 && x <= SLIDER_RIGHT + 4) {
            return SLIDER_SENSITIVITY;
        }
    }

    // Deadzone slider (60px below sensitivity)
    content_y += 60;
    {
        double frac = (double)(deadzone - 1000) / (8000 - 1000);
        double knob_x = SLIDER_LEFT + frac * SLIDER_TRACK_W;
        double dx = x - knob_x;
        double dy = y - content_y;
        if (dx * dx + dy * dy <= (KNOB_RADIUS + 4) * (KNOB_RADIUS + 4)) {
            return SLIDER_DEADZONE;
        }
        if (y >= content_y - KNOB_RADIUS - 2 && y <= content_y + KNOB_RADIUS + 2 &&
            x >= SLIDER_LEFT - 4 && x <= SLIDER_RIGHT + 4) {
            return SLIDER_DEADZONE;
        }
    }

    return SLIDER_NONE;
}

// Convert a pixel X position on the slider track to a value within the range.
// Clamps to [min_val, max_val].
static double x_to_double_value(int x, double min_val, double max_val)
{
    if (x <= SLIDER_LEFT) return min_val;
    if (x >= SLIDER_RIGHT) return max_val;
    double frac = (double)(x - SLIDER_LEFT) / SLIDER_TRACK_W;
    return min_val + frac * (max_val - min_val);
}

// ============================================================================
//  Restore Defaults — reset all values to factory settings
// ============================================================================

static void restore_defaults(void)
{
    sensitivity = 5.0;
    deadzone = 4000;
    apply_config();
}

// ============================================================================
//  Public API — Click handler
// ============================================================================

bool controller_pane_click(SysPrefsState *state, int x, int y)
{
    (void)state;

    // Check slider hits first
    int slider = hit_test_slider(x, y);
    if (slider != SLIDER_NONE) {
        dragging_slider = slider;

        // Immediately update value to where the user clicked
        if (slider == SLIDER_SENSITIVITY) {
            sensitivity = x_to_double_value(x, 1.0, 10.0);
        } else if (slider == SLIDER_DEADZONE) {
            deadzone = (int)(x_to_double_value(x, 1000.0, 8000.0) + 0.5);
        }

        return true;
    }

    // Check Restore Defaults button hit
    if (x >= BTN_X && x <= BTN_X + BTN_W &&
        y >= restore_btn_y && y <= restore_btn_y + BTN_H) {
        restore_defaults();
        return true;
    }

    return false;
}

// ============================================================================
//  Public API — Motion handler (slider dragging)
// ============================================================================

bool controller_pane_motion(SysPrefsState *state, int x, int y)
{
    (void)state;
    (void)y;
    if (dragging_slider == SLIDER_NONE) return false;

    // Update the value based on the current mouse X position
    if (dragging_slider == SLIDER_SENSITIVITY) {
        sensitivity = x_to_double_value(x, 1.0, 10.0);
    } else if (dragging_slider == SLIDER_DEADZONE) {
        deadzone = (int)(x_to_double_value(x, 1000.0, 8000.0) + 0.5);
    }

    return true;  // Request repaint to show the updated knob position
}

// ============================================================================
//  Public API — Release handler (commit value to disk)
// ============================================================================

void controller_pane_release(SysPrefsState *state)
{
    (void)state;
    if (dragging_slider != SLIDER_NONE) {
        dragging_slider = SLIDER_NONE;
        // Write config and send SIGHUP to cc-inputd on release
        apply_config();
    }
}
