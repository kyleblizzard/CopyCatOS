// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// menu.c — Right-click context menus (Snow Leopard style)
//
// This implements the dock's right-click context menu, modeled after the
// Mac OS X Snow Leopard dock menu. Features include:
//
//   - Dynamic menu items based on whether the target is an app or folder
//   - A bold header row showing the app/folder name (non-clickable)
//   - Submenus that slide out to the right on hover (Options, Sort By, etc.)
//   - Checkbox items with "✓" marks (Keep in Dock, Open at Login)
//   - Separator lines between logical groups
//   - "Remove from Dock" to delete items and persist the change
//
// The menu uses override-redirect X11 windows (no window manager decoration).
// Submenus are a second override-redirect window positioned to the right
// of the parent menu, aligned with the triggering submenu row.
//
// Visual style: light gray background, 6px rounded corners, Lucida Grande font,
// blue highlight on hover — matching the macOS aesthetic.
// ============================================================================

#define _GNU_SOURCE  // For M_PI in math.h under strict C11

#include "menu.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <math.h>
#include <time.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>

// ---------------------------------------------------------------------------
// Menu appearance constants
// ---------------------------------------------------------------------------
#define MENU_ITEM_HEIGHT    24      // Height of each clickable/label row in pixels
#define MENU_SEPARATOR_HEIGHT 9     // Height of a separator line row
#define MENU_PADDING        6       // Padding inside the menu (top, bottom, left, right)
#define MENU_WIDTH          220     // Fixed width of the popup menu (wider to fit "Remove from Dock")
#define MENU_FONT           "Lucida Grande 13"       // Normal font for menu text
#define MENU_FONT_BOLD      "Lucida Grande Bold 13"  // Bold font for the header label
#define MENU_CORNER_RADIUS  6.0     // Rounded corner radius in pixels
#define MENU_TEXT_LEFT       24     // Left edge of normal text (leaves room for checkmarks)
#define MENU_ARROW_RIGHT     10    // Right margin for the submenu arrow "►"
#define SUBMENU_HOVER_DELAY_MS 200  // Milliseconds before a submenu opens on hover

// Maximum number of items a single menu (or submenu) can hold
#define MAX_MENU_ITEMS 16

// ---------------------------------------------------------------------------
// MenuState — Tracks one popup menu window (main menu or submenu)
//
// We have two of these: one for the main (parent) menu and one for
// the currently-open submenu. Only one submenu can be open at a time.
// ---------------------------------------------------------------------------
typedef struct {
    Window win;                     // X11 window for this menu (None if closed)
    cairo_surface_t *surface;       // Cairo drawing surface
    cairo_t *cr;                    // Cairo drawing context
    MenuItem items[MAX_MENU_ITEMS]; // The rows in this menu
    int item_count;                 // How many rows are populated
    int hover_index;                // Which row the mouse is over (-1 = none)
    int width;                      // Window width in pixels
    int height;                     // Window height in pixels
    int win_x;                      // Window X position on screen
    int win_y;                      // Window Y position on screen
} MenuState;

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

// The main (parent) context menu
static MenuState menu = { .win = None };

// The submenu that slides out from a MTYPE_SUBMENU item
static MenuState submenu = { .win = None };

// Which dock item the menu was opened for
static DockItem *menu_target = NULL;

// Pointer back to the dock state (needed for config_save, screen bounds, etc.)
static DockState *dock_state_ref = NULL;

// Tracks which parent menu item index the submenu is currently attached to.
// -1 means no submenu is open.
static int submenu_parent_index = -1;

// Timer tracking for the 200ms hover delay before opening a submenu.
// We store the millisecond timestamp when the user first hovered over
// a submenu item. If they stay for SUBMENU_HOVER_DELAY_MS, we open it.
static long submenu_hover_start_ms = 0;
static int submenu_hover_pending = -1;  // Which item we're waiting to open (-1 = none)

// ---------------------------------------------------------------------------
// Forward declarations for internal helpers
// ---------------------------------------------------------------------------
static void menu_paint_state(MenuState *ms);
static void submenu_close(void);
static void submenu_open(int parent_index);
static int  hit_test_menu(MenuState *ms, int mouse_y);
static int  menu_item_y(MenuState *ms, int index);
static void dispatch_action(int action);
static void build_app_menu(DockItem *item);
static void build_folder_menu(DockItem *item);
static void build_options_submenu(DockItem *item);
static void build_sort_by_submenu(void);
static void build_display_as_submenu(void);
static int  calculate_menu_height(MenuState *ms);
static void create_menu_window(MenuState *ms, DockState *state, int x, int y);
static void destroy_menu_window(MenuState *ms, DockState *state);

// ---------------------------------------------------------------------------
// Helper: get current time in milliseconds (monotonic clock)
// Used for the submenu hover delay timer.
// ---------------------------------------------------------------------------
static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// ---------------------------------------------------------------------------
// Helper: draw a rounded rectangle path in Cairo.
// A rounded rectangle is 4 arcs (corners) connected by straight lines.
// ---------------------------------------------------------------------------
static void draw_rounded_rect(cairo_t *cr, double x, double y,
                               double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);          // Top-right corner
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);       // Bottom-right corner
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);        // Bottom-left corner
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);        // Top-left corner
    cairo_close_path(cr);
}

// ---------------------------------------------------------------------------
// Calculate the total pixel height of a menu based on its items.
// Each item type has a different height: normal/header/checkbox/submenu rows
// are MENU_ITEM_HEIGHT pixels, separators are MENU_SEPARATOR_HEIGHT pixels.
// ---------------------------------------------------------------------------
static int calculate_menu_height(MenuState *ms)
{
    int h = 2 * MENU_PADDING;  // Top and bottom padding
    for (int i = 0; i < ms->item_count; i++) {
        if (ms->items[i].type == MTYPE_SEPARATOR) {
            h += MENU_SEPARATOR_HEIGHT;
        } else {
            h += MENU_ITEM_HEIGHT;
        }
    }
    return h;
}

// ---------------------------------------------------------------------------
// Get the Y pixel coordinate of a specific menu item (by index).
// We walk through all items before it, adding their heights.
// ---------------------------------------------------------------------------
static int menu_item_y(MenuState *ms, int index)
{
    int y = MENU_PADDING;
    for (int i = 0; i < index && i < ms->item_count; i++) {
        if (ms->items[i].type == MTYPE_SEPARATOR) {
            y += MENU_SEPARATOR_HEIGHT;
        } else {
            y += MENU_ITEM_HEIGHT;
        }
    }
    return y;
}

// ---------------------------------------------------------------------------
// Hit-test: given a mouse Y coordinate inside the menu, figure out which
// item index it corresponds to. Returns -1 if it's not over any clickable
// item (e.g., over padding, a separator, or a disabled header).
// ---------------------------------------------------------------------------
static int hit_test_menu(MenuState *ms, int mouse_y)
{
    int y = MENU_PADDING;
    for (int i = 0; i < ms->item_count; i++) {
        int item_h = (ms->items[i].type == MTYPE_SEPARATOR)
                     ? MENU_SEPARATOR_HEIGHT : MENU_ITEM_HEIGHT;

        if (mouse_y >= y && mouse_y < y + item_h) {
            // Separators and disabled headers aren't selectable
            if (ms->items[i].type == MTYPE_SEPARATOR) return -1;
            if (ms->items[i].type == MTYPE_HEADER)    return -1;
            if (!ms->items[i].enabled)                 return -1;
            return i;
        }
        y += item_h;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Build the main menu items for an APP dock item.
//
// Snow Leopard layout:
//   [App Name]              ← bold header, not clickable
//   ─────────────────────
//   Options                ► ← submenu
//   Show In Finder
//   ─────────────────────
//   Remove from Dock
//   ─────────────────────   ← only if running
//   Quit [App Name]         ← only if running
// ---------------------------------------------------------------------------
static void build_app_menu(DockItem *item)
{
    int i = 0;

    // Row 0: App name as a bold, disabled header
    snprintf(menu.items[i].label, sizeof(menu.items[i].label), "%s", item->name);
    menu.items[i].type    = MTYPE_HEADER;
    menu.items[i].enabled = false;
    menu.items[i].checked = false;
    menu.items[i].action  = ACTION_NONE;
    i++;

    // Row 1: Separator
    menu.items[i].type    = MTYPE_SEPARATOR;
    menu.items[i].enabled = false;
    menu.items[i].action  = ACTION_NONE;
    i++;

    // Row 2: "Options" submenu (opens Keep in Dock, Open at Login, Show In Finder)
    snprintf(menu.items[i].label, sizeof(menu.items[i].label), "Options");
    menu.items[i].type    = MTYPE_SUBMENU;
    menu.items[i].enabled = true;
    menu.items[i].checked = false;
    menu.items[i].action  = ACTION_NONE;
    i++;

    // Row 3: "Show In Finder"
    snprintf(menu.items[i].label, sizeof(menu.items[i].label), "Show In Finder");
    menu.items[i].type    = MTYPE_NORMAL;
    menu.items[i].enabled = true;
    menu.items[i].checked = false;
    menu.items[i].action  = ACTION_SHOW_IN_FINDER;
    i++;

    // Row 4: Separator
    menu.items[i].type    = MTYPE_SEPARATOR;
    menu.items[i].enabled = false;
    menu.items[i].action  = ACTION_NONE;
    i++;

    // Row 5: "Remove from Dock"
    snprintf(menu.items[i].label, sizeof(menu.items[i].label), "Remove from Dock");
    menu.items[i].type    = MTYPE_NORMAL;
    menu.items[i].enabled = true;
    menu.items[i].checked = false;
    menu.items[i].action  = ACTION_REMOVE_FROM_DOCK;
    i++;

    // If the app is currently running, add a separator and Quit option
    if (item->running) {
        // Row 6: Separator
        menu.items[i].type    = MTYPE_SEPARATOR;
        menu.items[i].enabled = false;
        menu.items[i].action  = ACTION_NONE;
        i++;

        // Row 7: "Quit [App Name]"
        snprintf(menu.items[i].label, sizeof(menu.items[i].label),
                 "Quit %s", item->name);
        menu.items[i].type    = MTYPE_NORMAL;
        menu.items[i].enabled = true;
        menu.items[i].checked = false;
        menu.items[i].action  = ACTION_QUIT;
        i++;
    }

    menu.item_count = i;
}

// ---------------------------------------------------------------------------
// Build the main menu items for a FOLDER dock item (future stacks).
//
// Snow Leopard layout:
//   [Folder Name]           ← bold header
//   ─────────────────────
//   Sort By                ► ← submenu
//   Display As             ► ← submenu
//   ─────────────────────
//   Show In Finder
//   Remove from Dock
// ---------------------------------------------------------------------------
static void build_folder_menu(DockItem *item)
{
    int i = 0;

    // Row 0: Folder name header
    snprintf(menu.items[i].label, sizeof(menu.items[i].label), "%s", item->name);
    menu.items[i].type    = MTYPE_HEADER;
    menu.items[i].enabled = false;
    menu.items[i].checked = false;
    menu.items[i].action  = ACTION_NONE;
    i++;

    // Row 1: Separator
    menu.items[i].type    = MTYPE_SEPARATOR;
    menu.items[i].enabled = false;
    menu.items[i].action  = ACTION_NONE;
    i++;

    // Row 2: "Sort By" submenu
    snprintf(menu.items[i].label, sizeof(menu.items[i].label), "Sort By");
    menu.items[i].type    = MTYPE_SUBMENU;
    menu.items[i].enabled = true;
    menu.items[i].checked = false;
    menu.items[i].action  = ACTION_NONE;
    i++;

    // Row 3: "Display As" submenu
    snprintf(menu.items[i].label, sizeof(menu.items[i].label), "Display As");
    menu.items[i].type    = MTYPE_SUBMENU;
    menu.items[i].enabled = true;
    menu.items[i].checked = false;
    menu.items[i].action  = ACTION_NONE;
    i++;

    // Row 4: Separator
    menu.items[i].type    = MTYPE_SEPARATOR;
    menu.items[i].enabled = false;
    menu.items[i].action  = ACTION_NONE;
    i++;

    // Row 5: "Show In Finder"
    snprintf(menu.items[i].label, sizeof(menu.items[i].label), "Show In Finder");
    menu.items[i].type    = MTYPE_NORMAL;
    menu.items[i].enabled = true;
    menu.items[i].checked = false;
    menu.items[i].action  = ACTION_SHOW_IN_FINDER;
    i++;

    // Row 6: "Remove from Dock"
    snprintf(menu.items[i].label, sizeof(menu.items[i].label), "Remove from Dock");
    menu.items[i].type    = MTYPE_NORMAL;
    menu.items[i].enabled = true;
    menu.items[i].checked = false;
    menu.items[i].action  = ACTION_REMOVE_FROM_DOCK;
    i++;

    menu.item_count = i;
}

// ---------------------------------------------------------------------------
// Build submenu items for the "Options" submenu (apps only).
//
// Contents:
//   Keep in Dock       ✓   ← checkbox (always checked for pinned items)
//   Open at Login          ← checkbox (unchecked by default)
//   Show In Finder
// ---------------------------------------------------------------------------
static void build_options_submenu(DockItem *item)
{
    int i = 0;

    // "Keep in Dock" — checked because the item is pinned in the dock.
    // In the future, dynamically-appearing running apps won't have this checked.
    snprintf(submenu.items[i].label, sizeof(submenu.items[i].label), "Keep in Dock");
    submenu.items[i].type    = MTYPE_CHECKBOX;
    submenu.items[i].enabled = true;
    submenu.items[i].checked = true;   // All current dock items are "kept"
    submenu.items[i].action  = ACTION_KEEP_IN_DOCK;
    i++;

    // "Open at Login" — not yet implemented, but the menu item is here
    snprintf(submenu.items[i].label, sizeof(submenu.items[i].label), "Open at Login");
    submenu.items[i].type    = MTYPE_CHECKBOX;
    submenu.items[i].enabled = true;
    submenu.items[i].checked = false;
    submenu.items[i].action  = ACTION_OPEN_AT_LOGIN;
    i++;

    // "Show In Finder" — same as the one in the parent menu
    snprintf(submenu.items[i].label, sizeof(submenu.items[i].label), "Show In Finder");
    submenu.items[i].type    = MTYPE_NORMAL;
    submenu.items[i].enabled = true;
    submenu.items[i].checked = false;
    submenu.items[i].action  = ACTION_SHOW_IN_FINDER;
    i++;

    submenu.item_count = i;
}

// ---------------------------------------------------------------------------
// Build submenu items for "Sort By" (folders only).
//
// Contents: Name, Date Added, Date Modified, Date Created, Kind
// ---------------------------------------------------------------------------
static void build_sort_by_submenu(void)
{
    int i = 0;

    // Each sort option. "Name" is checked by default.
    struct { const char *label; int action; } opts[] = {
        { "Name",          ACTION_SORT_NAME },
        { "Date Added",    ACTION_SORT_DATE_ADDED },
        { "Date Modified", ACTION_SORT_DATE_MODIFIED },
        { "Date Created",  ACTION_SORT_DATE_CREATED },
        { "Kind",          ACTION_SORT_KIND },
    };

    for (int j = 0; j < 5; j++) {
        snprintf(submenu.items[i].label, sizeof(submenu.items[i].label),
                 "%s", opts[j].label);
        submenu.items[i].type    = MTYPE_CHECKBOX;
        submenu.items[i].enabled = true;
        submenu.items[i].checked = (j == 0);  // "Name" selected by default
        submenu.items[i].action  = opts[j].action;
        i++;
    }

    submenu.item_count = i;
}

// ---------------------------------------------------------------------------
// Build submenu items for "Display As" (folders only).
//
// Contents: Fan, Grid, Automatic
// ---------------------------------------------------------------------------
static void build_display_as_submenu(void)
{
    int i = 0;

    struct { const char *label; int action; } opts[] = {
        { "Fan",       ACTION_DISPLAY_FAN },
        { "Grid",      ACTION_DISPLAY_GRID },
        { "Automatic", ACTION_DISPLAY_AUTO },
    };

    for (int j = 0; j < 3; j++) {
        snprintf(submenu.items[i].label, sizeof(submenu.items[i].label),
                 "%s", opts[j].label);
        submenu.items[i].type    = MTYPE_CHECKBOX;
        submenu.items[i].enabled = true;
        submenu.items[i].checked = (j == 2);  // "Automatic" selected by default
        submenu.items[i].action  = opts[j].action;
        i++;
    }

    submenu.item_count = i;
}

// ---------------------------------------------------------------------------
// Paint a single menu (works for both the main menu and the submenu).
// Draws the rounded-rect background, then iterates over each item to render
// headers, separators, normal items, submenus (with arrows), and checkboxes.
// ---------------------------------------------------------------------------
static void menu_paint_state(MenuState *ms)
{
    if (ms->win == None) return;

    cairo_t *cr = ms->cr;

    // Clear the surface (fully transparent so the rounded corners show)
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // --- Menu background: light gray, nearly opaque ---
    draw_rounded_rect(cr, 0, 0, ms->width, ms->height, MENU_CORNER_RADIUS);
    cairo_set_source_rgba(cr, 240.0/255.0, 240.0/255.0, 240.0/255.0, 245.0/255.0);
    cairo_fill(cr);

    // Create Pango layouts for normal and bold text
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font_normal = pango_font_description_from_string(MENU_FONT);
    PangoFontDescription *font_bold   = pango_font_description_from_string(MENU_FONT_BOLD);

    int y = MENU_PADDING;

    for (int i = 0; i < ms->item_count; i++) {
        MenuItem *mi = &ms->items[i];

        // ------ SEPARATOR ------
        // Draw a thin horizontal line and advance by MENU_SEPARATOR_HEIGHT
        if (mi->type == MTYPE_SEPARATOR) {
            cairo_set_source_rgba(cr, 0.7, 0.7, 0.7, 1.0);
            cairo_set_line_width(cr, 1.0);
            // Center the line vertically within the separator height
            double line_y = y + (MENU_SEPARATOR_HEIGHT / 2.0) + 0.5;
            cairo_move_to(cr, MENU_PADDING + 4, line_y);
            cairo_line_to(cr, ms->width - MENU_PADDING - 4, line_y);
            cairo_stroke(cr);
            y += MENU_SEPARATOR_HEIGHT;
            continue;
        }

        // ------ HEADER (bold, non-hoverable label) ------
        if (mi->type == MTYPE_HEADER) {
            // Header text: bold, dark color, no hover highlight
            cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 1.0);
            pango_layout_set_font_description(layout, font_bold);
            pango_layout_set_text(layout, mi->label, -1);
            cairo_move_to(cr, MENU_TEXT_LEFT, y + 3);
            pango_cairo_show_layout(cr, layout);
            y += MENU_ITEM_HEIGHT;
            continue;
        }

        // ------ HOVERABLE ITEMS (normal, submenu, checkbox) ------
        bool hovered = (ms->hover_index == i);

        if (hovered) {
            // Blue highlight bar behind the hovered item
            cairo_set_source_rgba(cr, 0.2, 0.45, 0.9, 1.0);
            cairo_rectangle(cr, MENU_PADDING, y,
                            ms->width - 2 * MENU_PADDING, MENU_ITEM_HEIGHT);
            cairo_fill(cr);
            // White text on the blue background
            cairo_set_source_rgba(cr, 1, 1, 1, 1);
        } else {
            // Default: dark text
            cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 1.0);
        }

        // Use normal-weight font for all clickable items
        pango_layout_set_font_description(layout, font_normal);

        // If this is a checkbox item and it's checked, draw a "✓" on the left
        if (mi->type == MTYPE_CHECKBOX && mi->checked) {
            pango_layout_set_text(layout, "✓", -1);
            cairo_move_to(cr, MENU_PADDING + 6, y + 3);
            pango_cairo_show_layout(cr, layout);
        }

        // Draw the item label text
        pango_layout_set_text(layout, mi->label, -1);
        cairo_move_to(cr, MENU_TEXT_LEFT, y + 3);
        pango_cairo_show_layout(cr, layout);

        // If this is a submenu item, draw a "►" arrow on the right side
        if (mi->type == MTYPE_SUBMENU) {
            pango_layout_set_text(layout, "►", -1);
            // Position the arrow near the right edge of the menu
            cairo_move_to(cr, ms->width - MENU_PADDING - MENU_ARROW_RIGHT - 8, y + 3);
            pango_cairo_show_layout(cr, layout);
        }

        y += MENU_ITEM_HEIGHT;
    }

    // Clean up Pango resources
    pango_font_description_free(font_normal);
    pango_font_description_free(font_bold);
    g_object_unref(layout);

    // Flush so X11 picks up the new pixel data
    cairo_surface_flush(ms->surface);
}

// ---------------------------------------------------------------------------
// Create an override-redirect popup window for a menu at the given position.
// Sets up the X11 window, Cairo surface, and selects events.
// ---------------------------------------------------------------------------
static void create_menu_window(MenuState *ms, DockState *state, int x, int y)
{
    ms->width  = MENU_WIDTH;
    ms->height = calculate_menu_height(ms);
    ms->win_x  = x;
    ms->win_y  = y;

    // Override-redirect: the window manager won't add a title bar or border
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.colormap = state->colormap;
    attrs.border_pixel = 0;
    attrs.background_pixel = 0;

    ms->win = XCreateWindow(
        state->dpy, state->root,
        x, y, ms->width, ms->height,
        0,                          // No border
        32,                         // 32-bit depth for ARGB transparency
        InputOutput,
        state->visual,
        CWOverrideRedirect | CWColormap | CWBorderPixel | CWBackPixel,
        &attrs
    );

    // We need mouse events so we can track hover and clicks
    XSelectInput(state->dpy, ms->win,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask | LeaveWindowMask | EnterWindowMask);

    // Create Cairo drawing surface for this window
    ms->surface = cairo_xlib_surface_create(
        state->dpy, ms->win, state->visual, ms->width, ms->height);
    ms->cr = cairo_create(ms->surface);

    // Show the window on screen
    XMapRaised(state->dpy, ms->win);
}

// ---------------------------------------------------------------------------
// Destroy a menu window and free its Cairo resources.
// ---------------------------------------------------------------------------
static void destroy_menu_window(MenuState *ms, DockState *state)
{
    if (ms->win == None) return;

    if (ms->cr) {
        cairo_destroy(ms->cr);
        ms->cr = NULL;
    }
    if (ms->surface) {
        cairo_surface_destroy(ms->surface);
        ms->surface = NULL;
    }

    XDestroyWindow(state->dpy, ms->win);
    ms->win = None;
    ms->hover_index = -1;
    ms->item_count = 0;
}

// ---------------------------------------------------------------------------
// Close just the submenu (leaves the parent menu open).
// ---------------------------------------------------------------------------
static void submenu_close(void)
{
    if (submenu.win == None) return;
    if (!dock_state_ref) return;

    destroy_menu_window(&submenu, dock_state_ref);
    submenu_parent_index = -1;
    submenu_hover_pending = -1;
}

// ---------------------------------------------------------------------------
// Open a submenu attached to the given parent menu item index.
// Builds the submenu items based on which parent item triggered it, then
// creates the submenu window to the right of the parent menu.
// ---------------------------------------------------------------------------
static void submenu_open(int parent_index)
{
    if (!dock_state_ref || !menu_target) return;

    // Close any existing submenu first
    submenu_close();

    // Determine which submenu to build based on the parent item's label
    const char *label = menu.items[parent_index].label;

    if (strcmp(label, "Options") == 0) {
        build_options_submenu(menu_target);
    } else if (strcmp(label, "Sort By") == 0) {
        build_sort_by_submenu();
    } else if (strcmp(label, "Display As") == 0) {
        build_display_as_submenu();
    } else {
        return;  // Unknown submenu — shouldn't happen
    }

    submenu.hover_index = -1;

    // Position the submenu: right edge of parent, aligned vertically with
    // the submenu trigger item
    int sub_x = menu.win_x + menu.width - 2;  // Slight overlap for visual continuity
    int sub_y = menu.win_y + menu_item_y(&menu, parent_index);

    // Clamp to screen bounds so the submenu doesn't go off-screen
    submenu.width = MENU_WIDTH;
    submenu.height = calculate_menu_height(&submenu);

    if (sub_x + MENU_WIDTH > dock_state_ref->screen_w) {
        // Not enough room on the right — open to the left instead
        sub_x = menu.win_x - MENU_WIDTH + 2;
    }
    if (sub_y + submenu.height > dock_state_ref->screen_h) {
        sub_y = dock_state_ref->screen_h - submenu.height;
    }
    if (sub_y < 0) sub_y = 0;

    create_menu_window(&submenu, dock_state_ref, sub_x, sub_y);
    submenu_parent_index = parent_index;

    // Paint the submenu contents
    menu_paint_state(&submenu);
    XFlush(dock_state_ref->dpy);
}

// ---------------------------------------------------------------------------
// Dispatch an action when the user clicks a menu item.
// This is where the actual work happens: opening Finder, quitting apps,
// removing items from the dock, etc.
// ---------------------------------------------------------------------------
static void dispatch_action(int action)
{
    if (!menu_target || !dock_state_ref) return;

    switch (action) {

    case ACTION_SHOW_IN_FINDER: {
        // Open the app's directory (or the folder itself) in the file manager.
        // For apps: use `which` to find the binary, then open its parent directory.
        // For folders: open the folder path directly.
        char cmd[1024];
        if (menu_target->is_folder && menu_target->folder_path[0]) {
            snprintf(cmd, sizeof(cmd), "dolphin \"%s\" &", menu_target->folder_path);
        } else {
            snprintf(cmd, sizeof(cmd),
                     "dolphin \"$(dirname $(which %s))\" &",
                     menu_target->exec_path);
        }
        if (system(cmd) == -1) {
            fprintf(stderr, "menu: Failed to open Finder for %s\n",
                    menu_target->name);
        }
        break;
    }

    case ACTION_QUIT: {
        // Send SIGTERM to the running app using pkill.
        // pkill -f matches the process name against running processes.
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "pkill -f '%s'", menu_target->process_name);
        if (system(cmd) == -1) {
            fprintf(stderr, "menu: Failed to quit %s\n", menu_target->name);
        }
        menu_target->running = false;
        break;
    }

    case ACTION_REMOVE_FROM_DOCK: {
        // Remove this item from the dock's item array and save the config.
        // We find the item's index by pointer comparison, then shift all
        // subsequent items down by one slot.
        DockState *ds = dock_state_ref;
        int idx = -1;
        for (int i = 0; i < ds->item_count; i++) {
            if (&ds->items[i] == menu_target) {
                idx = i;
                break;
            }
        }
        if (idx >= 0) {
            // Free the icon surface before removing
            if (ds->items[idx].icon) {
                cairo_surface_destroy(ds->items[idx].icon);
                ds->items[idx].icon = NULL;
            }
            // Shift all items after this one to fill the gap
            for (int i = idx; i < ds->item_count - 1; i++) {
                ds->items[i] = ds->items[i + 1];
            }
            ds->item_count--;
            // Persist the change to disk
            config_save(ds);
            fprintf(stderr, "menu: Removed '%s' from dock\n", menu_target->name);
        }
        // Clear the target since it's been removed
        menu_target = NULL;
        break;
    }

    case ACTION_KEEP_IN_DOCK: {
        // Toggle "Keep in Dock". Currently all dock items are pinned,
        // so this is a placeholder for when dynamic (running-only) items
        // are supported. For now it just logs.
        fprintf(stderr, "menu: Keep in Dock toggled for %s\n", menu_target->name);
        break;
    }

    case ACTION_OPEN_AT_LOGIN: {
        // Placeholder for "Open at Login" functionality.
        // Would add/remove the app from an autostart config file.
        fprintf(stderr, "menu: Open at Login toggled for %s\n", menu_target->name);
        break;
    }

    // Folder sort modes — placeholders for future stacks feature
    case ACTION_SORT_NAME:
    case ACTION_SORT_DATE_ADDED:
    case ACTION_SORT_DATE_MODIFIED:
    case ACTION_SORT_DATE_CREATED:
    case ACTION_SORT_KIND:
        fprintf(stderr, "menu: Sort mode changed (action %d) for %s\n",
                action, menu_target->name);
        break;

    // Folder display modes — placeholders for future stacks feature
    case ACTION_DISPLAY_FAN:
    case ACTION_DISPLAY_GRID:
    case ACTION_DISPLAY_AUTO:
        fprintf(stderr, "menu: Display mode changed (action %d) for %s\n",
                action, menu_target->name);
        break;

    default:
        break;
    }
}

// ===========================================================================
// PUBLIC API
// ===========================================================================

// ---------------------------------------------------------------------------
// menu_show — Open the context menu for a dock item at the given position.
//
// Builds the appropriate menu (app vs folder), creates the popup window,
// grabs the pointer so clicks outside close the menu, and paints it.
// ---------------------------------------------------------------------------
void menu_show(DockState *state, DockItem *item, int x, int y)
{
    // Close any existing menu first
    menu_close(state);

    // Store references for use by internal helpers
    menu_target = item;
    dock_state_ref = state;
    menu.hover_index = -1;
    submenu_hover_pending = -1;

    // Build the menu items based on whether this is an app or a folder
    if (item->is_folder) {
        build_folder_menu(item);
    } else {
        build_app_menu(item);
    }

    // Calculate the menu size
    menu.width  = MENU_WIDTH;
    menu.height = calculate_menu_height(&menu);

    // Position the menu above the cursor (dock menus open upward)
    int menu_x = x - menu.width / 2;
    int menu_y = y - menu.height - 5;

    // Keep the menu on screen
    if (menu_x < 0) menu_x = 0;
    if (menu_x + menu.width > state->screen_w)
        menu_x = state->screen_w - menu.width;
    if (menu_y < 0) menu_y = 0;

    // Create the popup window
    create_menu_window(&menu, state, menu_x, menu_y);

    // Grab the pointer so that any click outside the menu windows
    // is still delivered to us (and we can close the menu)
    XGrabPointer(state->dpy, menu.win, True,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync,
                 None, None, CurrentTime);

    // Paint the initial menu contents
    menu_paint_state(&menu);
    XFlush(state->dpy);
}

// ---------------------------------------------------------------------------
// menu_close — Close the context menu and any open submenu.
// Releases the pointer grab and destroys all popup windows.
// ---------------------------------------------------------------------------
void menu_close(DockState *state)
{
    // Close the submenu first (if open)
    if (submenu.win != None) {
        destroy_menu_window(&submenu, state);
        submenu_parent_index = -1;
        submenu_hover_pending = -1;
    }

    if (menu.win == None) return;

    // Release the pointer grab so the user can click normally again
    XUngrabPointer(state->dpy, CurrentTime);

    // Destroy the main menu window
    destroy_menu_window(&menu, state);
    menu_target = NULL;
    dock_state_ref = NULL;

    XFlush(state->dpy);
}

// ---------------------------------------------------------------------------
// menu_handle_event — Process X11 events for the context menu system.
//
// Handles mouse motion (hover highlighting, submenu open/close),
// button clicks (action dispatch), and window enter/leave events.
// Returns true if the event was consumed by the menu.
// ---------------------------------------------------------------------------
bool menu_handle_event(DockState *state, XEvent *ev)
{
    // If no menu is open, we don't consume the event
    if (menu.win == None) return false;

    switch (ev->type) {

    // ----- MOUSE MOTION: update hover index, manage submenus -----
    case MotionNotify: {
        Window event_win = ev->xmotion.window;
        int mx = ev->xmotion.x;
        int my = ev->xmotion.y;

        // --- Motion in the MAIN MENU ---
        if (event_win == menu.win) {
            int old_hover = menu.hover_index;
            menu.hover_index = hit_test_menu(&menu, my);

            // If the hovered item changed, repaint
            if (menu.hover_index != old_hover) {
                menu_paint_state(&menu);
                XFlush(state->dpy);
            }

            // Submenu logic: if hovering over a MTYPE_SUBMENU item, start
            // the hover timer. If the timer expires (200ms), open the submenu.
            if (menu.hover_index >= 0 &&
                menu.items[menu.hover_index].type == MTYPE_SUBMENU) {

                if (submenu_hover_pending != menu.hover_index) {
                    // Started hovering a new submenu trigger — reset timer
                    submenu_hover_pending = menu.hover_index;
                    submenu_hover_start_ms = now_ms();
                }

                // Check if 200ms have elapsed since we started hovering
                long elapsed = now_ms() - submenu_hover_start_ms;
                if (elapsed >= SUBMENU_HOVER_DELAY_MS &&
                    submenu_parent_index != menu.hover_index) {
                    submenu_open(menu.hover_index);
                }
            } else {
                // Mouse moved off a submenu trigger — close submenu
                // BUT only if the mouse isn't inside the submenu window
                if (submenu.win != None) {
                    // Check if cursor is inside the submenu window bounds
                    // (translate screen coords: motion event is relative to menu.win)
                    int screen_x = menu.win_x + mx;
                    int screen_y = menu.win_y + my;
                    bool in_submenu =
                        screen_x >= submenu.win_x &&
                        screen_x < submenu.win_x + submenu.width &&
                        screen_y >= submenu.win_y &&
                        screen_y < submenu.win_y + submenu.height;

                    if (!in_submenu) {
                        submenu_close();
                    }
                }
                submenu_hover_pending = -1;
            }

            return true;
        }

        // --- Motion in the SUBMENU ---
        if (submenu.win != None && event_win == submenu.win) {
            int old_hover = submenu.hover_index;
            submenu.hover_index = hit_test_menu(&submenu, my);

            if (submenu.hover_index != old_hover) {
                menu_paint_state(&submenu);
                XFlush(state->dpy);
            }
            return true;
        }

        // Motion is in neither window — handled by grab, just ignore
        return false;
    }

    // ----- MOUSE BUTTON RELEASE: dispatch action on the clicked item -----
    case ButtonRelease: {
        Window event_win = ev->xbutton.window;

        // --- Click in the SUBMENU ---
        if (submenu.win != None && event_win == submenu.win) {
            if (submenu.hover_index >= 0 &&
                submenu.items[submenu.hover_index].enabled) {
                dispatch_action(submenu.items[submenu.hover_index].action);
            }
            menu_close(state);
            return true;
        }

        // --- Click in the MAIN MENU ---
        if (event_win == menu.win) {
            if (menu.hover_index >= 0 && menu.items[menu.hover_index].enabled) {
                MenuItem *mi = &menu.items[menu.hover_index];

                // Don't "click" submenu triggers — they open on hover
                if (mi->type != MTYPE_SUBMENU) {
                    dispatch_action(mi->action);
                    menu_close(state);
                }
            }
            // If clicked on a non-actionable item (separator, header, submenu),
            // just consume the event without closing.
            return true;
        }

        // --- Click OUTSIDE both menus: close everything ---
        menu_close(state);
        return true;
    }

    // ----- MOUSE BUTTON PRESS: consume or close -----
    case ButtonPress: {
        Window event_win = ev->xbutton.window;

        // If click is inside either menu window, consume it (release handles action)
        if (event_win == menu.win) return true;
        if (submenu.win != None && event_win == submenu.win) return true;

        // Click outside — close the menu
        menu_close(state);
        return true;
    }

    // ----- ENTER/LEAVE: handle mouse entering/leaving menu windows -----
    case EnterNotify: {
        // When the mouse enters the submenu window, we want to keep it open
        // (don't close it just because it left the main menu)
        return true;
    }

    case LeaveNotify: {
        Window event_win = ev->xcrossing.window;

        if (event_win == menu.win) {
            // Mouse left the main menu. If there's no submenu open,
            // clear the hover. If a submenu IS open, keep the parent
            // item highlighted so the user sees which submenu is active.
            if (submenu.win == None) {
                menu.hover_index = -1;
                menu_paint_state(&menu);
                XFlush(state->dpy);
            }
            submenu_hover_pending = -1;
        }

        if (submenu.win != None && event_win == submenu.win) {
            // Mouse left the submenu — check if it went back to the
            // main menu. If not, we could close the submenu, but
            // Snow Leopard keeps it open as long as the parent item
            // is highlighted. We'll just clear the submenu hover.
            submenu.hover_index = -1;
            menu_paint_state(&submenu);
            XFlush(state->dpy);
        }

        return true;
    }

    // ----- EXPOSE: repaint whichever window needs it -----
    case Expose: {
        if (ev->xexpose.window == menu.win) {
            menu_paint_state(&menu);
            return true;
        }
        if (submenu.win != None && ev->xexpose.window == submenu.win) {
            menu_paint_state(&submenu);
            return true;
        }
        return false;
    }

    default:
        return false;
    }
}
