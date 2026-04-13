// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// contextmenu.c — Right-click desktop context menu
//
// Creates a floating popup menu when the user right-clicks on empty
// desktop space. The menu is rendered with Cairo in a style matching
// Mac OS X Snow Leopard context menus:
//
//   - Light gray background with subtle border and rounded corners
//   - Blue highlight on hover (#3478F6)
//   - Separator lines between groups
//   - Drop shadow around the menu
//
// The menu window is created as override_redirect, which means the
// window manager won't frame it or include it in the taskbar. This is
// the standard technique for popup menus in X11.
//
// This module runs its own mini event loop while the menu is visible.
// It captures the pointer so all mouse events go to the menu, and
// dismisses itself when the user clicks outside or selects an item.

#define _GNU_SOURCE  // For M_PI in math.h under strict C11

#include "contextmenu.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ── Menu configuration ──────────────────────────────────────────────

// Visual constants for menu rendering
#define MENU_WIDTH        240     // Total menu width in pixels
#define MENU_ITEM_HEIGHT   22     // Height of each menu item
#define MENU_PAD_H          4     // Horizontal padding inside items
#define MENU_PAD_V          3     // Vertical padding above/below items
#define MENU_CORNER_RADIUS  6     // Corner radius for the menu background
#define MENU_SHADOW_SIZE    8     // Size of the drop shadow in pixels
#define MENU_SEP_MARGIN    12     // Horizontal margin for separator lines
#define MENU_SEP_HEIGHT     7     // Total height of a separator (3px margin + 1px line + 3px margin)
#define MENU_FONT          "Lucida Grande 13"  // Font for menu items

// A single menu item. Can be a regular item, a separator, or a
// submenu placeholder.
typedef struct {
    const char *label;    // Display text (NULL for separator)
    bool is_separator;    // True if this is a divider line
    bool is_submenu;      // True if this has a submenu arrow (">")
} MenuItem;

// The menu items for the desktop context menu.
// Order matters — the return value from contextmenu_show() is the
// index into this array.
static const MenuItem menu_items[] = {
    { "New Folder",                   false, false },  // 0
    { NULL,                           true,  false },  // 1 (separator)
    { "Sort By",                      false, true  },  // 2 (submenu placeholder)
    { "Clean Up",                     false, false },  // 3  -> actually index 4 for caller
    { NULL,                           true,  false },  // 4 (separator)
    { "Change Desktop Background...", false, false },  // 5
    { NULL,                           true,  false },  // 6 (separator)
    { "Open Terminal Here",           false, false },  // 7
};
#define MENU_ITEM_COUNT (sizeof(menu_items) / sizeof(menu_items[0]))

// ── Helper: rounded rectangle ───────────────────────────────────────

static void draw_rounded_rect(cairo_t *cr, double x, double y,
                               double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0,          M_PI / 2);
    cairo_arc(cr, x + r,     y + h - r, r, M_PI / 2,   M_PI);
    cairo_arc(cr, x + r,     y + r,     r, M_PI,        3 * M_PI / 2);
    cairo_close_path(cr);
}

// ── Menu geometry ───────────────────────────────────────────────────

// Calculate the total height of the menu based on its items.
static int calc_menu_height(void)
{
    int h = MENU_PAD_V * 2;  // Top and bottom padding

    for (size_t i = 0; i < MENU_ITEM_COUNT; i++) {
        if (menu_items[i].is_separator) {
            h += MENU_SEP_HEIGHT;
        } else {
            h += MENU_ITEM_HEIGHT;
        }
    }
    return h;
}

// Get the Y offset (from menu top) for a given item index.
static int item_y_offset(int index)
{
    int y = MENU_PAD_V;
    for (int i = 0; i < index && i < (int)MENU_ITEM_COUNT; i++) {
        if (menu_items[i].is_separator) {
            y += MENU_SEP_HEIGHT;
        } else {
            y += MENU_ITEM_HEIGHT;
        }
    }
    return y;
}

// Get the height of a specific item.
static int item_height(int index)
{
    if (index < 0 || index >= (int)MENU_ITEM_COUNT) return 0;
    return menu_items[index].is_separator ? MENU_SEP_HEIGHT : MENU_ITEM_HEIGHT;
}

// Figure out which item the mouse is hovering over.
// Returns -1 if the mouse is outside all items or over a separator.
static int hit_test(int mx, int my, int menu_h)
{
    (void)menu_h;

    if (mx < 0 || mx >= MENU_WIDTH) return -1;

    int y = MENU_PAD_V;
    for (int i = 0; i < (int)MENU_ITEM_COUNT; i++) {
        int h = item_height(i);
        if (my >= y && my < y + h) {
            // Don't select separators
            if (menu_items[i].is_separator) return -1;
            return i;
        }
        y += h;
    }
    return -1;
}

// ── Menu rendering ──────────────────────────────────────────────────

// Paint the entire menu onto the given Cairo context.
// highlighted_index is the currently hovered item (-1 for none).
static void paint_menu(cairo_t *cr, int menu_w, int menu_h,
                        int highlighted_index)
{
    // Clear the surface (transparent)
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // Draw the drop shadow.
    // We draw several semi-transparent rounded rects at increasing offsets
    // to simulate a Gaussian blur shadow.
    for (int i = MENU_SHADOW_SIZE; i > 0; i--) {
        double alpha = 0.2 * (1.0 - (double)i / MENU_SHADOW_SIZE);
        draw_rounded_rect(cr,
            MENU_SHADOW_SIZE - i,
            MENU_SHADOW_SIZE - i + 2,  // Shadow is slightly below
            menu_w - 2 * (MENU_SHADOW_SIZE - i),
            menu_h - 2 * (MENU_SHADOW_SIZE - i),
            MENU_CORNER_RADIUS + i);
        cairo_set_source_rgba(cr, 0, 0, 0, alpha);
        cairo_fill(cr);
    }

    // Draw the menu background (light gray, nearly opaque).
    // The slight transparency lets the wallpaper show through faintly.
    draw_rounded_rect(cr,
        MENU_SHADOW_SIZE, MENU_SHADOW_SIZE,
        menu_w - 2 * MENU_SHADOW_SIZE,
        menu_h - 2 * MENU_SHADOW_SIZE,
        MENU_CORNER_RADIUS);
    // RGBA(240, 240, 240, 245/255)
    cairo_set_source_rgba(cr, 240.0/255, 240.0/255, 240.0/255, 245.0/255);
    cairo_fill_preserve(cr);

    // 1px border
    cairo_set_source_rgba(cr, 160.0/255, 160.0/255, 160.0/255, 1.0);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    // Draw each menu item
    int y = MENU_PAD_V + MENU_SHADOW_SIZE;

    for (int i = 0; i < (int)MENU_ITEM_COUNT; i++) {
        int h = item_height(i);

        if (menu_items[i].is_separator) {
            // Draw separator line: thin horizontal line with margins
            int sep_y = y + MENU_SEP_HEIGHT / 2;
            cairo_set_source_rgba(cr, 200.0/255, 200.0/255, 200.0/255, 160.0/255);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, MENU_SHADOW_SIZE + MENU_SEP_MARGIN, sep_y + 0.5);
            cairo_line_to(cr, menu_w - MENU_SHADOW_SIZE - MENU_SEP_MARGIN, sep_y + 0.5);
            cairo_stroke(cr);
        } else {
            // Check if this item is highlighted (mouse hovering)
            bool highlighted = (i == highlighted_index);

            if (highlighted) {
                // Draw blue selection background with rounded corners.
                // #3478F6 is the macOS selection blue.
                draw_rounded_rect(cr,
                    MENU_SHADOW_SIZE + 4,
                    y,
                    menu_w - 2 * MENU_SHADOW_SIZE - 8,
                    MENU_ITEM_HEIGHT,
                    4.0);  // 4px corner radius on selection
                cairo_set_source_rgba(cr, 0x34/255.0, 0x78/255.0, 0xF6/255.0, 1.0);
                cairo_fill(cr);
            }

            // Draw the menu item text
            PangoLayout *layout = pango_cairo_create_layout(cr);
            pango_layout_set_text(layout, menu_items[i].label, -1);

            PangoFontDescription *font = pango_font_description_from_string(MENU_FONT);
            pango_layout_set_font_description(layout, font);
            pango_font_description_free(font);

            // Text color: white on highlight, dark gray otherwise
            if (highlighted) {
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
            } else {
                cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 1.0);
            }

            // Position text within the item
            cairo_move_to(cr, MENU_SHADOW_SIZE + MENU_PAD_H + 16, y + 3);
            pango_cairo_show_layout(cr, layout);

            // If this item has a submenu, draw a ">" arrow on the right
            if (menu_items[i].is_submenu) {
                pango_layout_set_text(layout, "\xe2\x96\xb8", -1);  // Unicode right-pointing triangle
                int tw, th;
                pango_layout_get_pixel_size(layout, &tw, &th);
                cairo_move_to(cr,
                    menu_w - MENU_SHADOW_SIZE - MENU_PAD_H - tw - 8,
                    y + 3);
                pango_cairo_show_layout(cr, layout);
            }

            g_object_unref(layout);
        }

        y += h;
    }
}

// ── Public API ──────────────────────────────────────────────────────

int contextmenu_show(Display *dpy, Window root, int root_x, int root_y,
                     int screen_w, int screen_h)
{
    int menu_content_h = calc_menu_height();

    // Total window size includes shadow padding on all sides
    int win_w = MENU_WIDTH + 2 * MENU_SHADOW_SIZE;
    int win_h = menu_content_h + 2 * MENU_SHADOW_SIZE;

    // Position the menu at the click location.
    // If the menu would go off-screen, shift it to stay visible.
    int win_x = root_x;
    int win_y = root_y;
    if (win_x + win_w > screen_w) win_x = screen_w - win_w;
    if (win_y + win_h > screen_h) win_y = screen_h - win_h;
    if (win_x < 0) win_x = 0;
    if (win_y < 0) win_y = 0;

    // Find a 32-bit ARGB visual for the menu window.
    // We need this for the shadow to have proper alpha transparency.
    XVisualInfo tmpl;
    tmpl.screen = DefaultScreen(dpy);
    tmpl.depth = 32;
    tmpl.class = TrueColor;
    int nvisuals = 0;
    XVisualInfo *vis_list = XGetVisualInfo(dpy,
        VisualScreenMask | VisualDepthMask | VisualClassMask,
        &tmpl, &nvisuals);

    Visual *visual = DefaultVisual(dpy, DefaultScreen(dpy));
    int depth = DefaultDepth(dpy, DefaultScreen(dpy));
    Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
    bool own_cmap = false;

    // Try to find an ARGB visual for the menu
    if (vis_list && nvisuals > 0) {
        for (int i = 0; i < nvisuals; i++) {
            if (vis_list[i].red_mask   == 0x00FF0000 &&
                vis_list[i].green_mask == 0x0000FF00 &&
                vis_list[i].blue_mask  == 0x000000FF) {
                visual = vis_list[i].visual;
                depth = 32;
                cmap = XCreateColormap(dpy, root, visual, AllocNone);
                own_cmap = true;
                break;
            }
        }
        XFree(vis_list);
    }

    // Create the popup window.
    // override_redirect = True means the WM won't frame this window.
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;     // No WM decoration
    attrs.colormap = cmap;
    attrs.border_pixel = 0;
    attrs.background_pixel = 0;
    attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                       PointerMotionMask | LeaveWindowMask;

    unsigned long mask = CWOverrideRedirect | CWColormap | CWBorderPixel |
                         CWBackPixel | CWEventMask;

    Window menu_win = XCreateWindow(dpy, root,
        win_x, win_y, win_w, win_h,
        0, depth, InputOutput, visual,
        mask, &attrs);

    // Map (show) the window and raise it to the top
    XMapRaised(dpy, menu_win);

    // Grab the pointer so all mouse events go to our menu.
    // This ensures clicks outside the menu are caught for dismissal.
    XGrabPointer(dpy, menu_win, True,
        ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
        GrabModeAsync, GrabModeAsync,
        None, None, CurrentTime);

    // Create a Cairo surface for rendering the menu
    cairo_surface_t *surface = cairo_xlib_surface_create(
        dpy, menu_win, visual, win_w, win_h);
    cairo_t *cr = cairo_create(surface);

    // Initial paint with no highlight
    paint_menu(cr, win_w, win_h, -1);
    XFlush(dpy);

    // Mini event loop for the menu.
    // We handle mouse movement (hover highlights), clicks (selection),
    // and clicks outside the menu (dismissal).
    int result = -1;       // Which item was selected (-1 = dismissed)
    bool menu_open = true;
    int highlighted = -1;  // Currently highlighted item index

    while (menu_open) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        switch (ev.type) {
        case Expose:
            // Repaint the menu (e.g., if another window was temporarily on top)
            if (ev.xexpose.count == 0) {
                paint_menu(cr, win_w, win_h, highlighted);
            }
            break;

        case MotionNotify:
        {
            // Update the highlight based on mouse position.
            // The coordinates are relative to the menu window, but we need
            // to account for the shadow padding.
            int mx = ev.xmotion.x - MENU_SHADOW_SIZE;
            int my = ev.xmotion.y - MENU_SHADOW_SIZE;
            int new_highlight = hit_test(mx, my, menu_content_h);

            if (new_highlight != highlighted) {
                highlighted = new_highlight;
                paint_menu(cr, win_w, win_h, highlighted);
                XFlush(dpy);
            }
            break;
        }

        case ButtonRelease:
        {
            // Check if the release was on a menu item
            int mx = ev.xbutton.x - MENU_SHADOW_SIZE;
            int my = ev.xbutton.y - MENU_SHADOW_SIZE;
            int hit = hit_test(mx, my, menu_content_h);

            if (hit >= 0 && !menu_items[hit].is_submenu) {
                // Selected a regular item — return its index
                result = hit;
                menu_open = false;
            } else if (mx < 0 || mx >= MENU_WIDTH ||
                       my < 0 || my >= menu_content_h) {
                // Clicked outside the menu — dismiss
                menu_open = false;
            }
            break;
        }

        case ButtonPress:
        {
            // Check if the press was outside the menu (dismiss)
            int mx = ev.xbutton.x - MENU_SHADOW_SIZE;
            int my = ev.xbutton.y - MENU_SHADOW_SIZE;
            if (mx < 0 || mx >= MENU_WIDTH ||
                my < 0 || my >= menu_content_h) {
                menu_open = false;
            }
            break;
        }

        case LeaveNotify:
            // Mouse left the menu window — clear highlight
            if (highlighted != -1) {
                highlighted = -1;
                paint_menu(cr, win_w, win_h, highlighted);
                XFlush(dpy);
            }
            break;

        default:
            break;
        }
    }

    // Clean up: release pointer grab, destroy window and resources
    XUngrabPointer(dpy, CurrentTime);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    XDestroyWindow(dpy, menu_win);
    if (own_cmap) {
        XFreeColormap(dpy, cmap);
    }
    XFlush(dpy);

    // Map internal menu indices to the action indices the caller expects.
    // The caller uses hardcoded indices (0=New Folder, 4=Clean Up, etc.)
    // but our internal array has separators that shift the numbering.
    // We return the raw internal index — the caller's switch statement
    // in desktop.c matches against these.
    //
    // Internal indices:
    //   0 = New Folder
    //   1 = separator
    //   2 = Sort By (submenu, not selectable)
    //   3 = Clean Up        -> caller checks for 3
    //   4 = separator
    //   5 = Change Desktop Background...  -> caller checks for 5
    //   6 = separator
    //   7 = Open Terminal Here  -> caller checks for 7
    return result;
}
