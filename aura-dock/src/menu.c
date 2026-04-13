// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// menu.c — Right-click context menus
//
// When you right-click a dock icon, a small popup menu appears with actions:
//   - "Show In Finder" — opens the app's directory in the file manager
//   - A separator line
//   - "Quit" (only if the app is running) — sends SIGTERM to close it
//
// The menu is an "override-redirect" window, which means the window manager
// leaves it completely alone — no title bar, no borders, no snapping. This
// is the standard technique for popup menus, tooltips, and dropdowns.
//
// Visual style matches macOS context menus:
//   - Light gray background: RGBA(240, 240, 240, 245/255)
//   - Rounded corners: 6px radius
//   - Font: Lucida Grande 13pt (falls back to system sans-serif)
//   - Blue highlight on hover
// ============================================================================

#include "menu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pango/pangocairo.h>

// ---------------------------------------------------------------------------
// Menu appearance constants
// ---------------------------------------------------------------------------
#define MENU_ITEM_HEIGHT  24     // Height of each menu row in pixels
#define MENU_PADDING      6      // Padding inside the menu
#define MENU_WIDTH        180    // Fixed width of the popup menu
#define MENU_FONT         "Lucida Grande 13"  // Font for menu text
#define MENU_CORNER_RADIUS 6.0   // Rounded corner radius

// ---------------------------------------------------------------------------
// Menu state — we only ever have one menu open at a time
// ---------------------------------------------------------------------------
typedef struct {
    Window win;                  // The popup window (or None if no menu is open)
    cairo_surface_t *surface;    // Cairo surface for drawing on the popup
    cairo_t *cr;                 // Cairo context for the popup
    DockItem *target_item;       // Which dock item this menu is for
    int item_count;              // Number of menu entries
    int hover_index;             // Which entry the mouse is over (-1 = none)
    int width;                   // Menu window width
    int height;                  // Menu window height
    bool has_quit;               // Whether "Quit" is shown (app is running)
} MenuState;

// Single global menu instance
static MenuState menu = { .win = None };

// ---------------------------------------------------------------------------
// Helper: draw a rounded rectangle path in Cairo.
// Used for the menu background shape.
// ---------------------------------------------------------------------------
static void draw_rounded_rect(cairo_t *cr, double x, double y,
                               double w, double h, double r)
{
    // A rounded rectangle is made of 4 arcs (corners) and 4 lines (edges).
    // We start at the top-left corner and go clockwise.
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);          // Top-right
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);       // Bottom-right
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);        // Bottom-left
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);        // Top-left
    cairo_close_path(cr);
}

// ---------------------------------------------------------------------------
// Helper: draw the menu contents (background, items, highlights)
// ---------------------------------------------------------------------------
static void menu_paint(DockState *state)
{
    if (menu.win == None) return;

    cairo_t *cr = menu.cr;

    // Clear the surface (fully transparent)
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // --- Menu background ---
    // Light gray, nearly opaque (245/255 alpha)
    draw_rounded_rect(cr, 0, 0, menu.width, menu.height, MENU_CORNER_RADIUS);
    cairo_set_source_rgba(cr, 240.0 / 255.0, 240.0 / 255.0, 240.0 / 255.0,
                          245.0 / 255.0);
    cairo_fill(cr);

    // --- Draw each menu item ---
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_from_string(MENU_FONT);
    pango_layout_set_font_description(layout, font);

    int y = MENU_PADDING;
    int entry_index = 0;

    // Entry 0: "Show In Finder"
    if (menu.hover_index == entry_index) {
        // Blue highlight bar for the hovered item
        cairo_set_source_rgba(cr, 0.2, 0.45, 0.9, 1.0);
        cairo_rectangle(cr, MENU_PADDING, y,
                        menu.width - 2 * MENU_PADDING, MENU_ITEM_HEIGHT);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 1);  // White text on blue
    } else {
        cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 1);  // Dark text
    }

    pango_layout_set_text(layout, "Show In Finder", -1);
    cairo_move_to(cr, MENU_PADDING + 12, y + 3);
    pango_cairo_show_layout(cr, layout);

    y += MENU_ITEM_HEIGHT;
    entry_index++;

    // Separator line
    cairo_set_source_rgba(cr, 0.7, 0.7, 0.7, 1.0);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, MENU_PADDING + 4, y + 4.5);
    cairo_line_to(cr, menu.width - MENU_PADDING - 4, y + 4.5);
    cairo_stroke(cr);
    y += 9;  // Separator takes less vertical space

    // Entry 1: "Quit" (only if the app is running)
    if (menu.has_quit) {
        if (menu.hover_index == entry_index) {
            cairo_set_source_rgba(cr, 0.2, 0.45, 0.9, 1.0);
            cairo_rectangle(cr, MENU_PADDING, y,
                            menu.width - 2 * MENU_PADDING, MENU_ITEM_HEIGHT);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 1, 1, 1, 1);
        } else {
            cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 1);
        }

        pango_layout_set_text(layout, "Quit", -1);
        cairo_move_to(cr, MENU_PADDING + 12, y + 3);
        pango_cairo_show_layout(cr, layout);
    }

    pango_font_description_free(font);
    g_object_unref(layout);

    // Flush the surface so X11 sees the new pixels
    cairo_surface_flush(menu.surface);
}

void menu_show(DockState *state, DockItem *item, int x, int y)
{
    // Close any existing menu first
    menu_close(state);

    menu.target_item = item;
    menu.hover_index = -1;
    menu.has_quit = item->running;

    // Calculate menu height based on how many items we have
    // "Show In Finder" + separator + optional "Quit"
    menu.width = MENU_WIDTH;
    menu.height = 2 * MENU_PADDING + MENU_ITEM_HEIGHT + 9;  // Show + separator
    menu.item_count = 1;

    if (menu.has_quit) {
        menu.height += MENU_ITEM_HEIGHT;
        menu.item_count = 2;
    }

    // Position the menu above the cursor (menus open upward from the dock)
    int menu_x = x - menu.width / 2;
    int menu_y = y - menu.height - 5;

    // Keep menu on screen
    if (menu_x < 0) menu_x = 0;
    if (menu_x + menu.width > state->screen_w)
        menu_x = state->screen_w - menu.width;
    if (menu_y < 0) menu_y = 0;

    // Create the popup window.
    // Override-redirect means the window manager won't try to manage it
    // (no title bar, no automatic positioning, no border).
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.colormap = state->colormap;
    attrs.border_pixel = 0;
    attrs.background_pixel = 0;

    menu.win = XCreateWindow(
        state->dpy, state->root,
        menu_x, menu_y, menu.width, menu.height,
        0,                          // Border width
        32,                         // Depth (32-bit for ARGB transparency)
        InputOutput,
        state->visual,
        CWOverrideRedirect | CWColormap | CWBorderPixel | CWBackPixel,
        &attrs
    );

    // We need to receive mouse events in the menu window
    XSelectInput(state->dpy, menu.win,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask | LeaveWindowMask);

    // Create a Cairo surface for drawing on the menu window
    menu.surface = cairo_xlib_surface_create(
        state->dpy, menu.win, state->visual, menu.width, menu.height);
    menu.cr = cairo_create(menu.surface);

    // Show the window and grab the pointer so clicks outside close the menu
    XMapRaised(state->dpy, menu.win);

    XGrabPointer(state->dpy, menu.win, True,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync,
                 None, None, CurrentTime);

    // Draw the menu contents
    menu_paint(state);
    XFlush(state->dpy);
}

void menu_close(DockState *state)
{
    if (menu.win == None) return;

    // Release the pointer grab
    XUngrabPointer(state->dpy, CurrentTime);

    // Clean up Cairo resources
    if (menu.cr) {
        cairo_destroy(menu.cr);
        menu.cr = NULL;
    }
    if (menu.surface) {
        cairo_surface_destroy(menu.surface);
        menu.surface = NULL;
    }

    // Destroy the popup window
    XDestroyWindow(state->dpy, menu.win);
    menu.win = None;
    menu.target_item = NULL;

    XFlush(state->dpy);
}

bool menu_handle_event(DockState *state, XEvent *ev)
{
    // If no menu is open, don't consume the event
    if (menu.win == None) return false;

    switch (ev->type) {
    case MotionNotify: {
        // Update which item the mouse is hovering over
        if (ev->xmotion.window != menu.win) return false;

        int my = ev->xmotion.y;
        int old_hover = menu.hover_index;
        menu.hover_index = -1;

        // Check if mouse is over "Show In Finder" (first item)
        int item_y = MENU_PADDING;
        if (my >= item_y && my < item_y + MENU_ITEM_HEIGHT) {
            menu.hover_index = 0;
        }

        // Check if mouse is over "Quit" (after separator)
        item_y += MENU_ITEM_HEIGHT + 9;  // Skip separator
        if (menu.has_quit && my >= item_y && my < item_y + MENU_ITEM_HEIGHT) {
            menu.hover_index = 1;
        }

        // Repaint if hover changed
        if (menu.hover_index != old_hover) {
            menu_paint(state);
            XFlush(state->dpy);
        }
        return true;
    }

    case ButtonRelease: {
        // Handle clicks on menu items
        if (ev->xbutton.window != menu.win) {
            // Clicked outside the menu — close it
            menu_close(state);
            return true;
        }

        if (menu.hover_index == 0 && menu.target_item) {
            // "Show In Finder" — open the app's directory in the file manager.
            // We look for the executable using `which` and open its parent dir.
            char cmd[1024];
            snprintf(cmd, sizeof(cmd),
                     "dolphin \"$(dirname $(which %s))\" &",
                     menu.target_item->exec_path);
            // Use system() in a fire-and-forget way for simplicity
            if (system(cmd) == -1) {
                fprintf(stderr, "Failed to open Finder for %s\n",
                        menu.target_item->name);
            }
        } else if (menu.hover_index == 1 && menu.has_quit && menu.target_item) {
            // "Quit" — send SIGTERM to the app.
            // We use pkill to find and terminate the process by name.
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "pkill -f '%s'",
                     menu.target_item->process_name);
            if (system(cmd) == -1) {
                fprintf(stderr, "Failed to quit %s\n", menu.target_item->name);
            }
            menu.target_item->running = false;
        }

        menu_close(state);
        return true;
    }

    case ButtonPress: {
        // If the click is outside the menu window, close the menu
        if (ev->xbutton.window != menu.win) {
            menu_close(state);
            return true;
        }
        return true;
    }

    case LeaveNotify: {
        if (ev->xcrossing.window == menu.win) {
            menu.hover_index = -1;
            menu_paint(state);
            XFlush(state->dpy);
        }
        return true;
    }

    case Expose: {
        if (ev->xexpose.window == menu.win) {
            menu_paint(state);
            return true;
        }
        return false;
    }

    default:
        return false;
    }
}
