// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// panes/controller.c — Controller preferences pane (3-tab interface)
// ============================================================================
//
// Full gamepad configuration UI with three tabs matching the inputd
// operating modes:
//
//   1. Desktop Mode  — button mappings, stick tuning, scroll, triggers
//   2. Desktop Gaming — passthrough toggle, per-game override list
//   3. Steam Mode    — "Enter Steam Mode" launcher
//
// Uses config_editor from inputmap to read/write ~/.config/copycatos/input.conf
// and signals inputd via SIGHUP for live config reload.
//
// Tab bar follows the Snow Leopard NSSegmentedControl style from HIG Chapter 15.
// ============================================================================

#include "controller.h"
#include "../registry.h"

// config_editor and scanner are compiled from inputmap sources
#include "config_editor.h"
#include "scanner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <math.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

// ============================================================================
//  Layout constants
// ============================================================================

// Slider geometry (same as dock.c / power.c for visual consistency)
#define SLIDER_LEFT       120
#define SLIDER_RIGHT      520
#define SLIDER_TRACK_W    (SLIDER_RIGHT - SLIDER_LEFT)
#define KNOB_RADIUS        8
#define LABEL_X            30

// Tab bar geometry — sits just below the pane title
#define TAB_BAR_X          30
#define TAB_BAR_Y_OFFSET   30    // Below "Controller" title
#define TAB_BAR_H          24    // 20px inner + 2px border top/bottom
#define TAB_COUNT           3

// Tab widths are calculated to fill the available space evenly
#define TAB_TOTAL_W       540    // Total width of the segmented control
#define TAB_W             (TAB_TOTAL_W / TAB_COUNT)

// Content starts below the tab bar
#define CONTENT_Y_OFFSET  (TAB_BAR_Y_OFFSET + TAB_BAR_H + 15)

// ============================================================================
//  Tab identifiers
// ============================================================================

#define TAB_DESKTOP_MODE    0
#define TAB_DESKTOP_GAMING  1
#define TAB_STEAM_MODE      2

// ============================================================================
//  Slider identifiers
// ============================================================================

#define SLIDER_SENSITIVITY   0
#define SLIDER_DEADZONE      1
#define SLIDER_SCROLL_SPEED  2
#define SLIDER_TRIGGER_THR   3
#define SLIDER_NONE         -1

// ============================================================================
//  Module state — persists across paint/click/motion/release calls
// ============================================================================

// Which tab is currently selected
static int current_tab = TAB_DESKTOP_MODE;

// Config editor instance — loads/saves input.conf through a proper API
// instead of the old hand-rolled INI parser
static ConfigEditor *editor = NULL;

// Cached values from config_editor (updated on load and slider drag)
static double sensitivity    = 3.0;
static int    deadzone       = 4000;
static double scroll_speed   = 0.15;
static int    trigger_thresh = 128;

// Slider drag state
static int dragging_slider = SLIDER_NONE;

// Button geometry for hit testing
static double restore_btn_y = 0;
static double steam_btn_y   = 0;

// Desktop Gaming toggle state
static bool gaming_mode_enabled = false;

// ============================================================================
//  Config loading via config_editor API
// ============================================================================

// Load settings from ~/.config/copycatos/input.conf using the config_editor.
// This replaces the hand-rolled INI parser that was in the old controller.c.
static void load_config(void)
{
    if (!editor) {
        editor = config_editor_new();
        if (!editor) return;
    }

    config_editor_load(editor);

    // Pull values from the editor into our local state
    MouseSettings ms;
    config_editor_get_mouse(editor, &ms);
    sensitivity = ms.sensitivity;
    deadzone    = ms.deadzone;

    trigger_thresh = config_editor_get_trigger_threshold(editor);

    // Scroll speed is stored in a [scroll] section that config_editor
    // doesn't manage yet — read it ourselves for now. Default 0.15.
    // TODO: add scroll_speed to config_editor API in inputmap
    const char *home = getenv("HOME");
    if (home) {
        char path[512];
        snprintf(path, sizeof(path), "%s/.config/copycatos/input.conf", home);
        FILE *fp = fopen(path, "r");
        if (fp) {
            char line[256], section[32] = "";
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
                } else if (strcmp(section, "scroll") == 0) {
                    if (strncmp(p, "speed=", 6) == 0) {
                        scroll_speed = atof(p + 6);
                        if (scroll_speed < 0.01) scroll_speed = 0.01;
                        if (scroll_speed > 1.0)  scroll_speed = 1.0;
                    }
                }
            }
            fclose(fp);
        }
    }
}

// Save current values back to input.conf and signal inputd to reload.
static void save_config(void)
{
    if (!editor) return;

    // Push our local values into the editor
    MouseSettings ms;
    config_editor_get_mouse(editor, &ms);
    ms.sensitivity = sensitivity;
    ms.deadzone    = deadzone;
    config_editor_set_mouse(editor, &ms);
    config_editor_set_trigger_threshold(editor, trigger_thresh);

    // Save the full file (mouse, triggers, power, desktop_mappings)
    config_editor_save(editor);

    // config_editor doesn't know about [scroll] yet, so we handle it
    // manually.  Read the file back, replace existing [scroll] section
    // if present, or append it if not.
    const char *home = getenv("HOME");
    if (home) {
        char path[512];
        snprintf(path, sizeof(path), "%s/.config/copycatos/input.conf", home);

        // Read the entire file into memory
        FILE *fp = fopen(path, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long len = ftell(fp);
            rewind(fp);
            char *buf = malloc(len + 1);
            if (buf) {
                size_t n = fread(buf, 1, len, fp);
                buf[n] = '\0';
                fclose(fp);

                // Look for an existing [scroll] section
                char *scroll_pos = strstr(buf, "\n[scroll]");
                if (!scroll_pos) scroll_pos = (strncmp(buf, "[scroll]", 8) == 0) ? buf : NULL;

                fp = fopen(path, "w");
                if (fp) {
                    if (scroll_pos) {
                        // Write everything before [scroll], then our new section
                        fwrite(buf, 1, scroll_pos - buf, fp);
                        fprintf(fp, "\n[scroll]\nspeed=%.2f\n", scroll_speed);
                        // Skip past old [scroll] section (up to next section or EOF)
                        char *next = strstr(scroll_pos + 1, "\n[");
                        if (scroll_pos == buf) next = strstr(scroll_pos + 8, "\n[");
                        if (next) {
                            fwrite(next, 1, (buf + n) - next, fp);
                        }
                    } else {
                        // No [scroll] section yet — write file then append
                        fwrite(buf, 1, n, fp);
                        fprintf(fp, "\n[scroll]\nspeed=%.2f\n", scroll_speed);
                    }
                    fclose(fp);
                }
                free(buf);
            } else {
                fclose(fp);
            }
        }
    }

    // Tell inputd to reload config
    config_editor_signal_daemon();
}

// ============================================================================
//  Drawing helpers (shared across tabs)
// ============================================================================

// Draw a horizontal slider with label, min/max text, and draggable knob.
// Identical visual style to dock.c / power.c — white knob, gray track,
// subtle drop shadow.
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
    double frac = (value - min_val) / (max_val - min_val);
    double knob_x = SLIDER_LEFT + frac * SLIDER_TRACK_W;

    // Knob shadow
    cairo_set_source_rgba(cr, 0, 0, 0, 0.15);
    cairo_arc(cr, knob_x, track_y + 1, KNOB_RADIUS + 1, 0, 2 * M_PI);
    cairo_fill(cr);

    // Knob body (white)
    cairo_set_source_rgb(cr, 0.98, 0.98, 0.98);
    cairo_arc(cr, knob_x, track_y, KNOB_RADIUS, 0, 2 * M_PI);
    cairo_fill(cr);

    // Knob border
    cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
    cairo_set_line_width(cr, 1.0);
    cairo_arc(cr, knob_x, track_y, KNOB_RADIUS, 0, 2 * M_PI);
    cairo_stroke(cr);

    // ── Current value readout ──────────────────────────────────────
    char val_buf[16];
    if (max_val <= 1.0) {
        // Scroll speed: show two decimals
        snprintf(val_buf, sizeof(val_buf), "%.2f", value);
    } else if (max_val <= 10.0) {
        // Sensitivity: one decimal
        snprintf(val_buf, sizeof(val_buf), "%.1f", value);
    } else {
        // Deadzone / trigger threshold: integer
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

// Draw a separator line across the pane
static void draw_separator(cairo_t *cr, double y)
{
    cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, LABEL_X, y + 0.5);
    cairo_line_to(cr, SLIDER_RIGHT + 50, y + 0.5);
    cairo_stroke(cr);
}

// Draw a bold section title
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

// Draw a regular-weight info label
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

// Draw a body text paragraph (wrapping, lighter weight, for descriptions)
static void draw_body_text(cairo_t *cr, double x, double y,
                           double width, const char *text)
{
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text, -1);
    pango_layout_set_width(layout, (int)(width * PANGO_SCALE));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD);

    PangoFontDescription *font =
        pango_font_description_from_string("Lucida Grande 11");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

// ============================================================================
//  Button drawing (rounded rect, Snow Leopard style)
// ============================================================================

#define BTN_RESTORE_X   LABEL_X
#define BTN_RESTORE_W   140
#define BTN_H            24

// Draw a rounded-rect push button
static void draw_button(cairo_t *cr, double x, double y,
                         double w, double h, const char *label)
{
    double r = 4.0;

    // Background
    cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_new_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_fill(cr);

    // Border
    cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
    cairo_set_line_width(cr, 1.0);
    cairo_new_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_stroke(cr);

    // Centered text
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

// Draw a large centered button (used for "Enter Steam Mode")
static void draw_large_button(cairo_t *cr, double cx, double y,
                               double w, double h, const char *label)
{
    double x = cx - w / 2;
    double r = 6.0;

    // Gradient-like background (slightly blue tinted for emphasis)
    cairo_set_source_rgb(cr, 0.88, 0.92, 0.98);
    cairo_new_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_fill(cr);

    // Border (blue-tinted)
    cairo_set_source_rgb(cr, 0.55, 0.65, 0.78);
    cairo_set_line_width(cr, 1.0);
    cairo_new_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_stroke(cr);

    // Centered text
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, label, -1);
    PangoFontDescription *font =
        pango_font_description_from_string("Lucida Grande Bold 13");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    int tw, th;
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_set_source_rgb(cr, 0.15, 0.20, 0.35);
    cairo_move_to(cr, x + (w - tw) / 2, y + (h - th) / 2);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

// ============================================================================
//  Tab bar — Snow Leopard NSSegmentedControl style
// ============================================================================
// Three segments with rounded ends, selected segment has a darker fill.
// HIG: "Use title-style capitalization" for tab names.
// ============================================================================

static const char *tab_labels[TAB_COUNT] = {
    "Desktop Mode",
    "Desktop Gaming",
    "Steam Mode"
};

// Draw the segmented tab bar. The selected tab gets a recessed/active look.
static void draw_tab_bar(cairo_t *cr, double base_y)
{
    double x = TAB_BAR_X;
    double y = base_y;
    double h = TAB_BAR_H;
    double r = 4.0;  // Corner radius for the outer segments

    for (int i = 0; i < TAB_COUNT; i++) {
        double tx = x + i * TAB_W;
        bool selected = (i == current_tab);

        // Build the segment path — left segment gets left corners rounded,
        // right segment gets right corners rounded, middle gets none.
        cairo_new_path(cr);
        if (i == 0) {
            // Leftmost: rounded left corners
            cairo_arc(cr, tx + r, y + r, r, M_PI, 3 * M_PI / 2);
            cairo_line_to(cr, tx + TAB_W, y);
            cairo_line_to(cr, tx + TAB_W, y + h);
            cairo_arc(cr, tx + r, y + h - r, r, M_PI / 2, M_PI);
        } else if (i == TAB_COUNT - 1) {
            // Rightmost: rounded right corners
            cairo_move_to(cr, tx, y);
            cairo_arc(cr, tx + TAB_W - r, y + r, r, -M_PI / 2, 0);
            cairo_arc(cr, tx + TAB_W - r, y + h - r, r, 0, M_PI / 2);
            cairo_line_to(cr, tx, y + h);
        } else {
            // Middle: no rounded corners
            cairo_rectangle(cr, tx, y, TAB_W, h);
        }
        cairo_close_path(cr);

        // Fill — selected tab is darker (recessed), others are light
        if (selected) {
            cairo_set_source_rgb(cr, 0.72, 0.72, 0.72);
        } else {
            cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
        }
        cairo_fill_preserve(cr);

        // Border
        cairo_set_source_rgb(cr, 0.62, 0.62, 0.62);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        // Tab label text — white on selected, dark on inactive
        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, tab_labels[i], -1);
        PangoFontDescription *font = pango_font_description_from_string(
            selected ? "Lucida Grande Bold 11" : "Lucida Grande 11");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);

        int tw, th;
        pango_layout_get_pixel_size(layout, &tw, &th);

        if (selected) {
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        } else {
            cairo_set_source_rgb(cr, 0.20, 0.20, 0.20);
        }
        cairo_move_to(cr, tx + (TAB_W - tw) / 2, y + (h - th) / 2);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }
}

// ============================================================================
//  Checkbox drawing helper (for Desktop Gaming toggle)
// ============================================================================

// Draw a Snow Leopard style checkbox: 14x14 rounded rect with optional check
static void draw_checkbox(cairo_t *cr, double x, double y,
                          bool checked, const char *label)
{
    double sz = 14.0;
    double r  = 2.0;

    // Box background
    cairo_set_source_rgb(cr, checked ? 0.22 : 0.96,
                             checked ? 0.47 : 0.96,
                             checked ? 0.84 : 0.96);
    cairo_new_path(cr);
    cairo_arc(cr, x + sz - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + sz - r, y + sz - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + sz - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_fill(cr);

    // Box border
    cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
    cairo_set_line_width(cr, 1.0);
    cairo_new_path(cr);
    cairo_arc(cr, x + sz - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + sz - r, y + sz - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + sz - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_stroke(cr);

    // Checkmark (white on checked background)
    if (checked) {
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_set_line_width(cr, 2.0);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(cr, x + 3, y + 7);
        cairo_line_to(cr, x + 6, y + 11);
        cairo_line_to(cr, x + 11, y + 3);
        cairo_stroke(cr);
    }

    // Label text to the right
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, label, -1);
    PangoFontDescription *font =
        pango_font_description_from_string("Lucida Grande 12");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_move_to(cr, x + sz + 8, y - 1);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

// ============================================================================
//  Tab content: Desktop Mode
// ============================================================================
// Shows all the tuning sliders: sensitivity, deadzone, scroll speed,
// trigger threshold. Also shows the current button mappings from
// config_editor (read-only for now — editable table is a later phase).
// ============================================================================

static void paint_desktop_mode(cairo_t *cr, double base_y)
{
    double y = base_y;

    // ── Section: Right Stick — Mouse ──────────────────────────────
    draw_section_title(cr, LABEL_X, y, "Right Stick \xe2\x80\x94 Mouse");

    // Sensitivity slider (1.0 - 10.0)
    y += 30;
    draw_slider(cr, y, "Sensitivity:", "Slow", "Fast",
                sensitivity, 1.0, 10.0);

    // Deadzone slider (1000 - 8000)
    y += 55;
    draw_slider(cr, y, "Deadzone:", "Small", "Large",
                (double)deadzone, 1000.0, 8000.0);

    // ── Separator ─────────────────────────────────────────────────
    y += 35;
    draw_separator(cr, y);

    // ── Section: Left Stick — Scroll ──────────────────────────────
    y += 15;
    draw_section_title(cr, LABEL_X, y, "Left Stick \xe2\x80\x94 Scroll");

    // Scroll speed slider (0.01 - 1.0)
    y += 30;
    draw_slider(cr, y, "Scroll Speed:", "Slow", "Fast",
                scroll_speed, 0.01, 1.0);

    // ── Separator ─────────────────────────────────────────────────
    y += 35;
    draw_separator(cr, y);

    // ── Section: Triggers ─────────────────────────────────────────
    y += 15;
    draw_section_title(cr, LABEL_X, y, "Triggers");

    // Trigger threshold slider (1 - 255)
    y += 30;
    draw_slider(cr, y, "Threshold:", "Light", "Heavy",
                (double)trigger_thresh, 1.0, 255.0);

    y += 8;
    draw_info_label(cr, SLIDER_LEFT, y + 12,
                    "Right Trigger: Left Click    Left Trigger: Right Click");

    // ── Separator ─────────────────────────────────────────────────
    y += 35;
    draw_separator(cr, y);

    // ── Section: Default Buttons (read-only) ──────────────────────
    y += 15;
    draw_section_title(cr, LABEL_X, y, "Button Mappings");

    // Show mappings from config_editor, or defaults if no mappings loaded
    y += 22;
    int map_count = editor ? config_editor_get_mapping_count(editor) : 0;

    if (map_count > 0) {
        // Show the mappings from config file
        for (int i = 0; i < map_count && i < 12; i++) {
            MappingEntry entry;
            if (config_editor_get_mapping(editor, i, &entry)) {
                char buf[128];
                // Format: "BTN_SOUTH → KEY_ENTER" using the human-readable names
                const char *src = entry.source_name;
                const char *tgt = entry.target_name;

                // Convert internal names to friendly names for display
                const char *friendly_src = src;
                const char *friendly_tgt = tgt;

                // Map common button codes to user-friendly names
                if (strcmp(src, "BTN_SOUTH") == 0)  friendly_src = "A Button";
                else if (strcmp(src, "BTN_EAST") == 0)   friendly_src = "B Button";
                else if (strcmp(src, "BTN_NORTH") == 0)  friendly_src = "X Button";
                else if (strcmp(src, "BTN_WEST") == 0)   friendly_src = "Y Button";
                else if (strcmp(src, "BTN_TL") == 0)     friendly_src = "LB";
                else if (strcmp(src, "BTN_TR") == 0)     friendly_src = "RB";
                else if (strcmp(src, "BTN_SELECT") == 0) friendly_src = "View";
                else if (strcmp(src, "BTN_START") == 0)  friendly_src = "Menu";
                else if (strcmp(src, "BTN_THUMBL") == 0) friendly_src = "L3";
                else if (strcmp(src, "BTN_THUMBR") == 0) friendly_src = "R3";

                if (strcmp(tgt, "KEY_ENTER") == 0)       friendly_tgt = "Enter";
                else if (strcmp(tgt, "KEY_ESC") == 0)    friendly_tgt = "Escape";
                else if (strcmp(tgt, "KEY_SPACE") == 0)  friendly_tgt = "Space";
                else if (strcmp(tgt, "KEY_TAB") == 0)    friendly_tgt = "Tab";
                else if (strncmp(tgt, "copycatos:", 10) == 0) friendly_tgt = tgt + 10;

                snprintf(buf, sizeof(buf), "%-16s  \xe2\x86\x92  %s",
                         friendly_src, friendly_tgt);
                draw_info_label(cr, LABEL_X + 20, y, buf);
                y += 18;
            }
        }
    } else {
        // Show hardcoded defaults (same as inputd's built-in mappings)
        draw_info_label(cr, LABEL_X + 20, y,
            "A Button         \xe2\x86\x92  Enter");
        y += 18;
        draw_info_label(cr, LABEL_X + 20, y,
            "B Button         \xe2\x86\x92  Escape");
        y += 18;
        draw_info_label(cr, LABEL_X + 20, y,
            "X Button         \xe2\x86\x92  Spotlight");
        y += 18;
        draw_info_label(cr, LABEL_X + 20, y,
            "Y Button         \xe2\x86\x92  Mission Control");
        y += 18;
        draw_info_label(cr, LABEL_X + 20, y,
            "Menu             \xe2\x86\x92  On-Screen Keyboard");
    }

    // ── Restore Defaults button ───────────────────────────────────
    y += 30;
    restore_btn_y = y;
    draw_button(cr, BTN_RESTORE_X, y, BTN_RESTORE_W, BTN_H, "Restore Defaults");
}

// ============================================================================
//  Tab content: Desktop Gaming
// ============================================================================
// In Desktop Gaming mode, the controller passes through as an Xbox 360
// gamepad (XInput). The user can toggle this mode and set up per-game
// automatic switching based on WM_CLASS patterns.
// ============================================================================

static void paint_desktop_gaming(cairo_t *cr, double base_y)
{
    double y = base_y;

    draw_body_text(cr, LABEL_X, y, TAB_TOTAL_W,
        "In Desktop Gaming mode, the controller passes through as a "
        "standard Xbox 360 gamepad. Games receive raw analog stick, trigger, "
        "and button input through XInput. Mouse and keyboard emulation is "
        "disabled while this mode is active.");

    y += 65;
    draw_separator(cr, y);

    y += 15;
    draw_checkbox(cr, LABEL_X, y, gaming_mode_enabled,
                  "Enable Desktop Gaming Mode");

    y += 35;
    draw_separator(cr, y);

    // ── Game Override List ─────────────────────────────────────────
    y += 15;
    draw_section_title(cr, LABEL_X, y, "Automatic Game Detection");

    y += 25;
    draw_body_text(cr, LABEL_X, y, TAB_TOTAL_W,
        "When a window matching one of these WM_CLASS patterns gains focus, "
        "the controller automatically switches to gamepad passthrough. When "
        "the game loses focus, it switches back to desktop mode.");

    y += 55;
    draw_info_label(cr, LABEL_X + 20, y, "No game overrides configured.");

    y += 30;
    draw_body_text(cr, LABEL_X, y, TAB_TOTAL_W,
        "Game override configuration will be available in a future update. "
        "For now, use the checkbox above to manually toggle gaming mode.");
}

// ============================================================================
//  Tab content: Steam Mode
// ============================================================================
// Steam Mode launches gamescope + Steam Big Picture. All input handling
// is delegated to Steam's own controller support.
// ============================================================================

static void paint_steam_mode(cairo_t *cr, double base_y)
{
    double y = base_y;

    draw_body_text(cr, LABEL_X, y, TAB_TOTAL_W,
        "Steam Mode launches a dedicated gaming session using gamescope "
        "and Steam Big Picture. All controller input is handled by Steam's "
        "built-in controller support, including per-game configurations "
        "from the Steam community.");

    y += 65;
    draw_separator(cr, y);

    y += 30;

    // Large centered "Enter Steam Mode" button
    double center_x = LABEL_X + TAB_TOTAL_W / 2;
    steam_btn_y = y;
    draw_large_button(cr, center_x, y, 220, 36, "Enter Steam Mode");

    y += 55;
    draw_body_text(cr, LABEL_X, y, TAB_TOTAL_W,
        "You can also enter Steam Mode by pressing the Steam button on "
        "the Legion Go (once detected). To exit Steam Mode, press the "
        "Steam button again or close Steam Big Picture.");

    y += 55;
    draw_info_label(cr, LABEL_X, y,
        "Note: gamescope and Steam must be installed.");
}

// ============================================================================
//  Public API — Paint
// ============================================================================

void controller_pane_paint(SysPrefsState *state)
{
    cairo_t *cr = state->cr;

    // Load config on first paint
    static bool config_loaded = false;
    if (!config_loaded) {
        load_config();
        config_loaded = true;
    }

    // Content area starts below the toolbar
    double content_y = TOOLBAR_HEIGHT + 20;

    // ── Pane title ────────────────────────────────────────────────
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

    // ── Tab bar ───────────────────────────────────────────────────
    double tab_y = content_y + TAB_BAR_Y_OFFSET;
    draw_tab_bar(cr, tab_y);

    // ── Tab content ───────────────────────────────────────────────
    double tab_content_y = content_y + CONTENT_Y_OFFSET;

    switch (current_tab) {
    case TAB_DESKTOP_MODE:
        paint_desktop_mode(cr, tab_content_y);
        break;
    case TAB_DESKTOP_GAMING:
        paint_desktop_gaming(cr, tab_content_y);
        break;
    case TAB_STEAM_MODE:
        paint_steam_mode(cr, tab_content_y);
        break;
    }
}

// ============================================================================
//  Slider hit testing (Desktop Mode tab only)
// ============================================================================

// Compute the Y positions for each slider — must match paint_desktop_mode layout.
// Returns the slider Y coordinate for the given slider ID.
static double slider_y_for(int slider_id)
{
    double content_y = TOOLBAR_HEIGHT + 20 + CONTENT_Y_OFFSET;

    // Section: Right Stick — Mouse
    // section_title at content_y, sensitivity at +30, deadzone at +30+55
    switch (slider_id) {
    case SLIDER_SENSITIVITY:
        return content_y + 30;
    case SLIDER_DEADZONE:
        return content_y + 30 + 55;
    case SLIDER_SCROLL_SPEED:
        // After deadzone (+55), separator (+35), section_title (+15), slider (+30)
        return content_y + 30 + 55 + 35 + 15 + 30;
    case SLIDER_TRIGGER_THR:
        // After scroll_speed, separator (+35), section_title (+15), slider (+30)
        return content_y + 30 + 55 + 35 + 15 + 30 + 35 + 15 + 30;
    default:
        return 0;
    }
}

// Check if (x, y) hits any of the four sliders. Returns SLIDER_NONE if no hit.
static int hit_test_slider(int x, int y)
{
    if (current_tab != TAB_DESKTOP_MODE) return SLIDER_NONE;

    // Test each slider
    int sliders[] = { SLIDER_SENSITIVITY, SLIDER_DEADZONE,
                      SLIDER_SCROLL_SPEED, SLIDER_TRIGGER_THR };

    for (int s = 0; s < 4; s++) {
        double sy = slider_y_for(sliders[s]);

        // Check knob hit (circular)
        double frac;
        switch (sliders[s]) {
        case SLIDER_SENSITIVITY:
            frac = (sensitivity - 1.0) / (10.0 - 1.0);
            break;
        case SLIDER_DEADZONE:
            frac = (double)(deadzone - 1000) / (8000 - 1000);
            break;
        case SLIDER_SCROLL_SPEED:
            frac = (scroll_speed - 0.01) / (1.0 - 0.01);
            break;
        case SLIDER_TRIGGER_THR:
            frac = (double)(trigger_thresh - 1) / (255 - 1);
            break;
        default:
            frac = 0;
        }

        double knob_x = SLIDER_LEFT + frac * SLIDER_TRACK_W;
        double dx = x - knob_x;
        double dy_f = y - sy;
        if (dx * dx + dy_f * dy_f <= (KNOB_RADIUS + 4) * (KNOB_RADIUS + 4)) {
            return sliders[s];
        }

        // Check track hit (rectangular)
        if (y >= sy - KNOB_RADIUS - 2 && y <= sy + KNOB_RADIUS + 2 &&
            x >= SLIDER_LEFT - 4 && x <= SLIDER_RIGHT + 4) {
            return sliders[s];
        }
    }

    return SLIDER_NONE;
}

// Convert pixel X to slider value, clamped to the given range.
static double x_to_value(int x, double min_val, double max_val)
{
    if (x <= SLIDER_LEFT) return min_val;
    if (x >= SLIDER_RIGHT) return max_val;
    double frac = (double)(x - SLIDER_LEFT) / SLIDER_TRACK_W;
    return min_val + frac * (max_val - min_val);
}

// Update the correct variable based on which slider is being dragged
static void update_slider_value(int slider, int x)
{
    switch (slider) {
    case SLIDER_SENSITIVITY:
        sensitivity = x_to_value(x, 1.0, 10.0);
        break;
    case SLIDER_DEADZONE:
        deadzone = (int)(x_to_value(x, 1000.0, 8000.0) + 0.5);
        break;
    case SLIDER_SCROLL_SPEED:
        scroll_speed = x_to_value(x, 0.01, 1.0);
        break;
    case SLIDER_TRIGGER_THR:
        trigger_thresh = (int)(x_to_value(x, 1.0, 255.0) + 0.5);
        break;
    }
}

// ============================================================================
//  Restore Defaults
// ============================================================================

static void restore_defaults(void)
{
    sensitivity    = 3.0;
    deadzone       = 4000;
    scroll_speed   = 0.15;
    trigger_thresh = 128;
    save_config();
}

// ============================================================================
//  Public API — Click handler
// ============================================================================

bool controller_pane_click(SysPrefsState *state, int x, int y)
{
    (void)state;

    // ── Tab bar hit test ──────────────────────────────────────────
    double content_y = TOOLBAR_HEIGHT + 20;
    double tab_y = content_y + TAB_BAR_Y_OFFSET;

    if (y >= tab_y && y <= tab_y + TAB_BAR_H &&
        x >= TAB_BAR_X && x < TAB_BAR_X + TAB_TOTAL_W) {
        int new_tab = (x - TAB_BAR_X) / TAB_W;
        if (new_tab >= 0 && new_tab < TAB_COUNT && new_tab != current_tab) {
            current_tab = new_tab;
            return true;  // Repaint to show new tab
        }
        return false;
    }

    // ── Tab-specific click handling ───────────────────────────────
    switch (current_tab) {
    case TAB_DESKTOP_MODE: {
        // Slider hit test
        int slider = hit_test_slider(x, y);
        if (slider != SLIDER_NONE) {
            dragging_slider = slider;
            update_slider_value(slider, x);
            return true;
        }

        // Restore Defaults button
        if (x >= BTN_RESTORE_X && x <= BTN_RESTORE_X + BTN_RESTORE_W &&
            y >= restore_btn_y && y <= restore_btn_y + BTN_H) {
            restore_defaults();
            return true;
        }
        break;
    }

    case TAB_DESKTOP_GAMING: {
        // Checkbox hit test — check a ~200px wide area covering box + label
        double cb_y = content_y + CONTENT_Y_OFFSET + 65 + 15;
        if (x >= LABEL_X && x <= LABEL_X + 250 &&
            y >= cb_y - 2 && y <= cb_y + 18) {
            gaming_mode_enabled = !gaming_mode_enabled;
            // TODO: send mode switch command to inputd
            return true;
        }
        break;
    }

    case TAB_STEAM_MODE: {
        // "Enter Steam Mode" button hit test
        double btn_x = LABEL_X + TAB_TOTAL_W / 2 - 110;
        if (x >= btn_x && x <= btn_x + 220 &&
            y >= steam_btn_y && y <= steam_btn_y + 36) {
            // Launch game-mode (gamescope + Steam Big Picture)
            // fork+exec so we don't block the UI.
            // Ignore SIGCHLD so the child doesn't become a zombie.
            signal(SIGCHLD, SIG_IGN);
            fprintf(stderr, "[systemcontrol] Launching Steam Mode...\n");
            pid_t pid = fork();
            if (pid == 0) {
                execlp("game-mode", "game-mode", NULL);
                // If game-mode isn't installed, try gamescope directly
                execlp("gamescope", "gamescope", "-f", "--",
                       "steam", "-gamepadui", NULL);
                _exit(1);
            }
            return true;
        }
        break;
    }
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

    update_slider_value(dragging_slider, x);
    return true;
}

// ============================================================================
//  Public API — Release handler (commit slider value to disk)
// ============================================================================

void controller_pane_release(SysPrefsState *state)
{
    (void)state;
    if (dragging_slider != SLIDER_NONE) {
        dragging_slider = SLIDER_NONE;
        save_config();
    }
}
