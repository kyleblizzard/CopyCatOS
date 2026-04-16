// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// panes/power.c — Energy Saver preferences pane
// ============================================================================
//
// Provides controls for the physical power button behavior on the Lenovo
// Legion Go. The user can adjust the timing thresholds that distinguish a
// short press from a long press, and see what action each triggers.
//
// Changes are written to ~/.config/copicatos/input.conf [power] section
// and applied live by sending SIGHUP to cc-inputd.
// ============================================================================

#include "power.h"
#include "../registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <math.h>
#include <sys/stat.h>

// ============================================================================
//  Slider geometry constants (same as dock.c and controller.c)
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
#define SLIDER_SHORT_MS     0   // Short press threshold slider
#define SLIDER_LONG_MS      1   // Long press threshold slider
#define SLIDER_NONE        -1

// Current config values — read from input.conf [power] section
static int  short_press_ms  = 300;    // Short press threshold (300-1500 ms)
static int  long_press_ms   = 3000;   // Long press threshold (1500-5000 ms)
static int  dragging_slider = SLIDER_NONE;

// The actions are read-only display for now — these show what the power
// button does on short vs long press
static const char *short_press_action = "Suspend";
static const char *long_press_action  = "Restart";

// ============================================================================
//  Config reading
// ============================================================================

// Read current values from ~/.config/copicatos/input.conf [power] section
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

        // Parse section headers like [power]
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
        // Parse key=value pairs within the [power] section
        else if (strcmp(section, "power") == 0) {
            if (strncmp(p, "short_press_ms=", 15) == 0) {
                short_press_ms = atoi(p + 15);
                if (short_press_ms < 300)  short_press_ms = 300;
                if (short_press_ms > 1500) short_press_ms = 1500;
            } else if (strncmp(p, "long_press_ms=", 14) == 0) {
                long_press_ms = atoi(p + 14);
                if (long_press_ms < 1500) long_press_ms = 1500;
                if (long_press_ms > 5000) long_press_ms = 5000;
            }
        }
    }
    fclose(fp);
}

// ============================================================================
//  Config writing + live apply
// ============================================================================

// Write current power values to the [power] section of input.conf.
// Preserves [mouse] and [triggers] sections that are managed by controller.c.
static void apply_config(void)
{
    const char *home = getenv("HOME");
    if (!home) return;

    // Ensure config directory exists
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config/copicatos", home);
    mkdir(dir, 0755);

    char path[512];
    snprintf(path, sizeof(path), "%s/.config/copicatos/input.conf", home);

    // Read existing content to preserve [mouse] and [triggers] sections
    char mouse_block[1024] = "";
    char triggers_block[1024] = "";

    FILE *fp = fopen(path, "r");
    if (fp) {
        char line[256];
        char section[32] = "";
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
                if (strcmp(section, "mouse") == 0) {
                    write_target = mouse_block;
                    strncat(mouse_block, line,
                            sizeof(mouse_block) - strlen(mouse_block) - 1);
                } else if (strcmp(section, "triggers") == 0) {
                    write_target = triggers_block;
                    strncat(triggers_block, line,
                            sizeof(triggers_block) - strlen(triggers_block) - 1);
                } else {
                    write_target = NULL;
                }
            } else if (write_target) {
                size_t tgt_len = strlen(write_target);
                size_t max_len = (write_target == mouse_block)
                    ? sizeof(mouse_block) : sizeof(triggers_block);
                strncat(write_target, line, max_len - tgt_len - 1);
            }
        }
        fclose(fp);
    }

    // Rewrite the file with all sections
    fp = fopen(path, "w");
    if (!fp) return;

    // Preserve [mouse] section if it existed
    if (mouse_block[0] != '\0') {
        fprintf(fp, "%s\n", mouse_block);
    }

    // Preserve [triggers] section if it existed
    if (triggers_block[0] != '\0') {
        fprintf(fp, "%s\n", triggers_block);
    }

    // Write our [power] section
    fprintf(fp, "[power]\n");
    fprintf(fp, "short_press_ms=%d\n", short_press_ms);
    fprintf(fp, "long_press_ms=%d\n", long_press_ms);
    fprintf(fp, "\n");

    fclose(fp);

    // Send SIGHUP to cc-inputd so it reloads the config immediately
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
// Same visual style as dock.c and controller.c for consistency.
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

    // Knob body (white with gray border, Snow Leopard style)
    cairo_set_source_rgb(cr, 0.98, 0.98, 0.98);
    cairo_arc(cr, knob_x, track_y, KNOB_RADIUS, 0, 2 * M_PI);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
    cairo_set_line_width(cr, 1.0);
    cairo_arc(cr, knob_x, track_y, KNOB_RADIUS, 0, 2 * M_PI);
    cairo_stroke(cr);

    // ── Current value text (ms) ───────────────────────────────────
    char val_buf[16];
    snprintf(val_buf, sizeof(val_buf), "%d ms", value);

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

// Draw a separator line across the pane
static void draw_separator(cairo_t *cr, double y)
{
    cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, LABEL_X, y + 0.5);
    cairo_line_to(cr, SLIDER_RIGHT + 50, y + 0.5);
    cairo_stroke(cr);
}

// Draw a section title in bold
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

// Draw an info label (used for showing the current action assignment)
static void draw_info_label(cairo_t *cr, double x, double y,
                             const char *key, const char *value)
{
    // Key part in regular weight
    PangoLayout *layout = pango_cairo_create_layout(cr);
    char combined[256];
    snprintf(combined, sizeof(combined), "%s  %s", key, value);
    pango_layout_set_text(layout, combined, -1);
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
//  Public API — Paint
// ============================================================================

void power_pane_paint(SysPrefsState *state)
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
    pango_layout_set_text(title, "Energy Saver", -1);
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

    // ── Section: Power Button ─────────────────────────────────────
    content_y += 15;
    draw_section_title(cr, LABEL_X, content_y, "Power Button");

    // Show current action assignments (read-only for now)
    content_y += 22;
    draw_info_label(cr, LABEL_X + 20, content_y,
                    "Short Press:", short_press_action);
    content_y += 20;
    draw_info_label(cr, LABEL_X + 20, content_y,
                    "Long Press:", long_press_action);

    // ── Separator ─────────────────────────────────────────────────
    content_y += 30;
    draw_separator(cr, content_y);

    // ── Section: Timing ───────────────────────────────────────────
    content_y += 15;
    draw_section_title(cr, LABEL_X, content_y, "Timing");

    // Short press threshold slider
    content_y += 30;
    draw_slider(cr, content_y,
                "Short Press:", "Quick (300ms)", "Slow (1500ms)",
                short_press_ms, 300, 1500);

    // Long press threshold slider
    content_y += 60;
    draw_slider(cr, content_y,
                "Long Press:", "Quick (1500ms)", "Long (5000ms)",
                long_press_ms, 1500, 5000);

    // ── Description text ──────────────────────────────────────────
    content_y += 50;
    PangoLayout *desc = pango_cairo_create_layout(cr);
    pango_layout_set_text(desc,
        "Adjust how quickly the power button responds.\n"
        "A short press suspends; a long press restarts.", -1);
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

// Check if (x, y) is on one of the two slider knobs or tracks.
// The Y positions must match the paint layout exactly.
static int hit_test_slider(int x, int y)
{
    // Layout: title(+30) + sep(+15) + section(+22) + label(+20) + sep_gap(+30)
    //         + sep(+15) + section(+30) = short press slider Y
    double content_y = TOOLBAR_HEIGHT + 20 + 30 + 15 + 22 + 20 + 30 + 15 + 30;

    // Short press threshold slider
    {
        double frac = (double)(short_press_ms - 300) / (1500 - 300);
        double knob_x = SLIDER_LEFT + frac * SLIDER_TRACK_W;
        double dx = x - knob_x;
        double dy = y - content_y;
        if (dx * dx + dy * dy <= (KNOB_RADIUS + 4) * (KNOB_RADIUS + 4)) {
            return SLIDER_SHORT_MS;
        }
        if (y >= content_y - KNOB_RADIUS - 2 && y <= content_y + KNOB_RADIUS + 2 &&
            x >= SLIDER_LEFT - 4 && x <= SLIDER_RIGHT + 4) {
            return SLIDER_SHORT_MS;
        }
    }

    // Long press threshold slider (60px below short press)
    content_y += 60;
    {
        double frac = (double)(long_press_ms - 1500) / (5000 - 1500);
        double knob_x = SLIDER_LEFT + frac * SLIDER_TRACK_W;
        double dx = x - knob_x;
        double dy = y - content_y;
        if (dx * dx + dy * dy <= (KNOB_RADIUS + 4) * (KNOB_RADIUS + 4)) {
            return SLIDER_LONG_MS;
        }
        if (y >= content_y - KNOB_RADIUS - 2 && y <= content_y + KNOB_RADIUS + 2 &&
            x >= SLIDER_LEFT - 4 && x <= SLIDER_RIGHT + 4) {
            return SLIDER_LONG_MS;
        }
    }

    return SLIDER_NONE;
}

// Convert a pixel X position on the slider track to an integer value
static int x_to_value(int x, int min_val, int max_val)
{
    if (x <= SLIDER_LEFT) return min_val;
    if (x >= SLIDER_RIGHT) return max_val;
    double frac = (double)(x - SLIDER_LEFT) / SLIDER_TRACK_W;
    return min_val + (int)(frac * (max_val - min_val) + 0.5);
}

// ============================================================================
//  Public API — Click handler
// ============================================================================

bool power_pane_click(SysPrefsState *state, int x, int y)
{
    (void)state;

    int slider = hit_test_slider(x, y);
    if (slider != SLIDER_NONE) {
        dragging_slider = slider;

        // Immediately update value to where the user clicked
        if (slider == SLIDER_SHORT_MS) {
            short_press_ms = x_to_value(x, 300, 1500);
        } else if (slider == SLIDER_LONG_MS) {
            long_press_ms = x_to_value(x, 1500, 5000);
        }

        return true;
    }

    return false;
}

// ============================================================================
//  Public API — Motion handler (slider dragging)
// ============================================================================

bool power_pane_motion(SysPrefsState *state, int x, int y)
{
    (void)state;
    (void)y;
    if (dragging_slider == SLIDER_NONE) return false;

    if (dragging_slider == SLIDER_SHORT_MS) {
        short_press_ms = x_to_value(x, 300, 1500);
    } else if (dragging_slider == SLIDER_LONG_MS) {
        long_press_ms = x_to_value(x, 1500, 5000);
    }

    return true;  // Request repaint
}

// ============================================================================
//  Public API — Release handler (commit value to disk)
// ============================================================================

void power_pane_release(SysPrefsState *state)
{
    (void)state;
    if (dragging_slider != SLIDER_NONE) {
        dragging_slider = SLIDER_NONE;
        // Write config and send SIGHUP to cc-inputd on release
        apply_config();
    }
}
