// CopyCatOS — by Kyle Blizzard at Blizzard.show

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
#include "desktop.h"
#include "labels.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ── Menu geometry (point values, scaled to physical px via S()/SF()) ──
//
// Each constant expands to an S()/SF() expression so it evaluates at call
// time against the current desktop_hidpi_scale. That means every use site
// produces correct physical pixels on a 1.0× external or a 1.75× panel,
// with no per-function local scaling dance.
#define MENU_WIDTH          S(290)    // Total menu width
#define MENU_ITEM_HEIGHT    S(22)     // Height of each menu item
#define MENU_PAD_H          S(4)      // Horizontal padding inside items
#define MENU_PAD_V          S(3)      // Vertical padding above/below items
#define MENU_CORNER_RADIUS  SF(6.0)   // Corner radius for the menu background
#define MENU_SHADOW_SIZE    S(8)      // Drop-shadow ring thickness
#define MENU_SEP_MARGIN     S(12)     // Horizontal margin for separator lines
#define MENU_SEP_HEIGHT     S(7)      // Total height of a separator row
#define MENU_SEL_INSET      S(4)      // Horizontal inset for selection pill
#define MENU_SEL_RADIUS     SF(4.0)   // Selection pill corner radius
#define MENU_ICON_GUTTER    S(16)     // Leading space before item label text
#define MENU_TEXT_Y_OFF     S(3)      // Baseline offset for item text
#define MENU_ARROW_GUTTER   S(8)      // Trailing gap before submenu arrow
#define MENU_LINE_WIDTH     SF(1.0)   // Stroke width for separators + borders

// Build the "Lucida Grande <pt>" string with the point size scaled to the
// active hidpi factor — then Pango will render the glyphs at the right
// physical-pixel size instead of leaving 13pt on every output.
static void set_menu_font(PangoLayout *layout)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "Lucida Grande %d", S(13));
    PangoFontDescription *fd = pango_font_description_from_string(buf);
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);
}

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

    // 1pt border (scaled)
    cairo_set_source_rgba(cr, 160.0/255, 160.0/255, 160.0/255, 1.0);
    cairo_set_line_width(cr, MENU_LINE_WIDTH);
    cairo_stroke(cr);

    // Draw each menu item
    int y = MENU_PAD_V + MENU_SHADOW_SIZE;

    for (int i = 0; i < (int)MENU_ITEM_COUNT; i++) {
        int h = item_height(i);

        if (menu_items[i].is_separator) {
            // Draw separator line: thin horizontal line with margins
            int sep_y = y + MENU_SEP_HEIGHT / 2;
            cairo_set_source_rgba(cr, 200.0/255, 200.0/255, 200.0/255, 160.0/255);
            cairo_set_line_width(cr, MENU_LINE_WIDTH);
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
                    MENU_SHADOW_SIZE + MENU_SEL_INSET,
                    y,
                    menu_w - 2 * MENU_SHADOW_SIZE - 2 * MENU_SEL_INSET,
                    MENU_ITEM_HEIGHT,
                    MENU_SEL_RADIUS);
                cairo_set_source_rgba(cr, 0x34/255.0, 0x78/255.0, 0xF6/255.0, 1.0);
                cairo_fill(cr);
            }

            // Draw the menu item text
            PangoLayout *layout = pango_cairo_create_layout(cr);
            pango_layout_set_text(layout, menu_items[i].label, -1);
            set_menu_font(layout);

            // Text color: white on highlight, dark gray otherwise
            if (highlighted) {
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
            } else {
                cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 1.0);
            }

            // Position text within the item
            cairo_move_to(cr, MENU_SHADOW_SIZE + MENU_PAD_H + MENU_ICON_GUTTER,
                          y + MENU_TEXT_Y_OFF);
            pango_cairo_show_layout(cr, layout);

            // If this item has a submenu, draw a ">" arrow on the right
            if (menu_items[i].is_submenu) {
                pango_layout_set_text(layout, "\xe2\x96\xb8", -1);  // Unicode right-pointing triangle
                int tw, th;
                pango_layout_get_pixel_size(layout, &tw, &th);
                cairo_move_to(cr,
                    menu_w - MENU_SHADOW_SIZE - MENU_PAD_H - tw - MENU_ARROW_GUTTER,
                    y + MENU_TEXT_Y_OFF);
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

    // The opening right-click's ButtonRelease is delivered to this loop
    // (the grab transferred to menu_win mid-click). We must ignore it,
    // otherwise the menu picks/dismisses itself the instant the user
    // releases the button. Snow Leopard parity: a quick right-click
    // leaves the menu posted; the user then left-clicks an item.
    // We only honor a ButtonRelease that follows a ButtonPress *inside*
    // this loop.
    bool seen_press_in_loop = false;

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
            // Skip the leftover release from the click that opened us.
            if (!seen_press_in_loop) break;

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
            seen_press_in_loop = true;
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

// ── Icon context menu ────────────────────────────────────────────────

// Menu items for the icon right-click menu.
// Label ▶ is a submenu trigger — selecting it closes this menu and
// opens the label picker popup.
typedef struct {
    const char *label;
    bool is_separator;
    bool is_submenu;
} IconMenuItem;

static const IconMenuItem icon_menu_items[] = {
    { "Open",            false, false },  // 0  -> ICON_ACTION_OPEN
    { NULL,              true,  false },  // 1 separator
    { "Get Info",        false, false },  // 2  -> ICON_ACTION_INFO
    { NULL,              true,  false },  // 3 separator
    { "Label",           false, true  },  // 4  -> triggers label picker
    { NULL,              true,  false },  // 5 separator
    { "Move to Trash",   false, false },  // 6  -> ICON_ACTION_TRASH
};
#define ICON_MENU_ITEM_COUNT \
    (sizeof(icon_menu_items) / sizeof(icon_menu_items[0]))

// Same geometry helpers as the desktop menu, just using icon_menu_items.

static int calc_icon_menu_height(void)
{
    int h = MENU_PAD_V * 2;
    for (size_t i = 0; i < ICON_MENU_ITEM_COUNT; i++) {
        h += icon_menu_items[i].is_separator ? MENU_SEP_HEIGHT : MENU_ITEM_HEIGHT;
    }
    return h;
}

static int icon_item_y_offset(int index)
{
    int y = MENU_PAD_V;
    for (int i = 0; i < index && i < (int)ICON_MENU_ITEM_COUNT; i++) {
        y += icon_menu_items[i].is_separator ? MENU_SEP_HEIGHT : MENU_ITEM_HEIGHT;
    }
    return y;
}

static int icon_hit_test(int mx, int my, int menu_h)
{
    (void)menu_h;
    if (mx < 0 || mx >= MENU_WIDTH) return -1;

    int y = MENU_PAD_V;
    for (int i = 0; i < (int)ICON_MENU_ITEM_COUNT; i++) {
        int h = icon_menu_items[i].is_separator ? MENU_SEP_HEIGHT : MENU_ITEM_HEIGHT;
        if (my >= y && my < y + h) {
            return icon_menu_items[i].is_separator ? -1 : i;
        }
        y += h;
    }
    return -1;
}

// Render the icon context menu.
static void paint_icon_menu(cairo_t *cr, int menu_w, int menu_h,
                             int highlighted_index)
{
    // Clear
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // Drop shadow
    for (int i = MENU_SHADOW_SIZE; i > 0; i--) {
        double alpha = 0.2 * (1.0 - (double)i / MENU_SHADOW_SIZE);
        draw_rounded_rect(cr,
            MENU_SHADOW_SIZE - i,
            MENU_SHADOW_SIZE - i + 2,
            menu_w - 2 * (MENU_SHADOW_SIZE - i),
            menu_h - 2 * (MENU_SHADOW_SIZE - i),
            MENU_CORNER_RADIUS + i);
        cairo_set_source_rgba(cr, 0, 0, 0, alpha);
        cairo_fill(cr);
    }

    // Menu background
    draw_rounded_rect(cr,
        MENU_SHADOW_SIZE, MENU_SHADOW_SIZE,
        menu_w - 2 * MENU_SHADOW_SIZE,
        menu_h - 2 * MENU_SHADOW_SIZE,
        MENU_CORNER_RADIUS);
    cairo_set_source_rgba(cr, 240.0/255, 240.0/255, 240.0/255, 245.0/255);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 160.0/255, 160.0/255, 160.0/255, 1.0);
    cairo_set_line_width(cr, MENU_LINE_WIDTH);
    cairo_stroke(cr);

    // Items
    int y = MENU_PAD_V + MENU_SHADOW_SIZE;
    for (int i = 0; i < (int)ICON_MENU_ITEM_COUNT; i++) {
        int h = icon_menu_items[i].is_separator ? MENU_SEP_HEIGHT : MENU_ITEM_HEIGHT;

        if (icon_menu_items[i].is_separator) {
            int sep_y = y + MENU_SEP_HEIGHT / 2;
            cairo_set_source_rgba(cr, 200.0/255, 200.0/255, 200.0/255, 160.0/255);
            cairo_set_line_width(cr, MENU_LINE_WIDTH);
            cairo_move_to(cr, MENU_SHADOW_SIZE + MENU_SEP_MARGIN, sep_y + 0.5);
            cairo_line_to(cr, menu_w - MENU_SHADOW_SIZE - MENU_SEP_MARGIN, sep_y + 0.5);
            cairo_stroke(cr);
        } else {
            bool highlighted = (i == highlighted_index);

            if (highlighted) {
                draw_rounded_rect(cr,
                    MENU_SHADOW_SIZE + MENU_SEL_INSET, y,
                    menu_w - 2 * MENU_SHADOW_SIZE - 2 * MENU_SEL_INSET, MENU_ITEM_HEIGHT,
                    MENU_SEL_RADIUS);
                cairo_set_source_rgba(cr, 0x34/255.0, 0x78/255.0, 0xF6/255.0, 1.0);
                cairo_fill(cr);
            }

            PangoLayout *layout = pango_cairo_create_layout(cr);
            pango_layout_set_text(layout, icon_menu_items[i].label, -1);
            set_menu_font(layout);

            cairo_set_source_rgba(cr, highlighted ? 1.0 : 0.1,
                                      highlighted ? 1.0 : 0.1,
                                      highlighted ? 1.0 : 0.1, 1.0);
            cairo_move_to(cr, MENU_SHADOW_SIZE + MENU_PAD_H + MENU_ICON_GUTTER,
                          y + MENU_TEXT_Y_OFF);
            pango_cairo_show_layout(cr, layout);

            // Submenu arrow "▸"
            if (icon_menu_items[i].is_submenu) {
                pango_layout_set_text(layout, "\xe2\x96\xb8", -1);
                int tw, th;
                pango_layout_get_pixel_size(layout, &tw, &th);
                cairo_move_to(cr,
                    menu_w - MENU_SHADOW_SIZE - MENU_PAD_H - tw - MENU_ARROW_GUTTER,
                    y + MENU_TEXT_Y_OFF);
                pango_cairo_show_layout(cr, layout);
            }

            g_object_unref(layout);
        }
        y += h;
    }
}

// ── Label picker popup ───────────────────────────────────────────────

// A small popup window showing 8 circles: one for each label color (1-7)
// plus an "X" (none) circle. The user clicks a circle to set the label.
//
// Layout: circles arranged in a horizontal row, each PICKER_CIRCLE_D pt.
// All geometry is in points — S() scales to physical pixels per output.
#define PICKER_CIRCLE_D    S(24)   // Diameter of each color circle
#define PICKER_CIRCLE_GAP  S(6)    // Gap between circles
#define PICKER_PAD_H       S(10)   // Horizontal padding inside the window
#define PICKER_PAD_V       S(8)    // Vertical padding inside the window
#define PICKER_SHADOW      S(6)    // Shadow ring thickness

// Total number of circles: 7 colors + 1 "none" = LABEL_COUNT (8)
#define PICKER_CIRCLE_COUNT LABEL_COUNT

// Compute the total width of the picker content (no shadow).
static int picker_content_w(void)
{
    return PICKER_PAD_H * 2 +
           PICKER_CIRCLE_COUNT * PICKER_CIRCLE_D +
           (PICKER_CIRCLE_COUNT - 1) * PICKER_CIRCLE_GAP;
}

// X pixel position of circle center for index i (relative to content origin).
static int picker_circle_cx(int i)
{
    return PICKER_PAD_H + PICKER_CIRCLE_D / 2 +
           i * (PICKER_CIRCLE_D + PICKER_CIRCLE_GAP);
}

// Y pixel center of all circles (relative to content origin).
static int picker_circle_cy(void)
{
    return PICKER_PAD_V + PICKER_CIRCLE_D / 2;
}

// Hit-test: which circle (0-7) is the mouse over? -1 if none.
static int picker_hit_test(int mx, int my, int content_w, int content_h)
{
    (void)content_w; (void)content_h;
    int cy = picker_circle_cy();
    int r  = PICKER_CIRCLE_D / 2;

    for (int i = 0; i < PICKER_CIRCLE_COUNT; i++) {
        int cx = picker_circle_cx(i);
        int dx = mx - cx;
        int dy = my - cy;
        if (dx*dx + dy*dy <= r*r) return i;
    }
    return -1;
}

// Paint the label picker popup.
static void paint_picker(cairo_t *cr, int win_w, int win_h, int highlighted)
{
    int cw = picker_content_w();
    int ch = PICKER_PAD_V * 2 + PICKER_CIRCLE_D;

    // Clear
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // Drop shadow
    for (int i = PICKER_SHADOW; i > 0; i--) {
        double alpha = 0.15 * (1.0 - (double)i / PICKER_SHADOW);
        draw_rounded_rect(cr,
            PICKER_SHADOW - i,
            PICKER_SHADOW - i + 1,
            win_w - 2*(PICKER_SHADOW - i),
            win_h - 2*(PICKER_SHADOW - i),
            MENU_CORNER_RADIUS + i);
        cairo_set_source_rgba(cr, 0, 0, 0, alpha);
        cairo_fill(cr);
    }

    // Background
    draw_rounded_rect(cr,
        PICKER_SHADOW, PICKER_SHADOW,
        cw, ch,
        MENU_CORNER_RADIUS);
    cairo_set_source_rgba(cr, 240.0/255, 240.0/255, 240.0/255, 250.0/255);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 160.0/255, 160.0/255, 160.0/255, 1.0);
    cairo_set_line_width(cr, MENU_LINE_WIDTH);
    cairo_stroke(cr);

    // Draw each circle
    int cy = PICKER_SHADOW + picker_circle_cy();
    int r  = PICKER_CIRCLE_D / 2;
    int hover_bump = S(2);  // selection ring extends 2pt past the circle edge

    for (int i = 0; i < PICKER_CIRCLE_COUNT; i++) {
        int cx = PICKER_SHADOW + picker_circle_cx(i);

        if (i == 0) {
            // Circle 0 = "None" — draw a grey ring with an X inside
            if (highlighted == 0) {
                // Blue selection ring
                cairo_arc(cr, cx, cy, r + hover_bump, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, 0x34/255.0, 0x78/255.0, 0xF6/255.0, 1.0);
                cairo_fill(cr);
            }
            cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 0.85, 0.85, 0.85, 1.0);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 0.55, 0.55, 0.55, 1.0);
            cairo_set_line_width(cr, SF(1.5));
            cairo_stroke(cr);

            // "X" inside the circle
            double margin = r * 0.35;
            cairo_set_source_rgba(cr, 0.4, 0.4, 0.4, 1.0);
            cairo_set_line_width(cr, SF(1.5));
            cairo_move_to(cr, cx - margin, cy - margin);
            cairo_line_to(cr, cx + margin, cy + margin);
            cairo_move_to(cr, cx + margin, cy - margin);
            cairo_line_to(cr, cx - margin, cy + margin);
            cairo_stroke(cr);
        } else {
            // Label color circles 1-7
            const LabelColor *lc = &label_colors[i];

            if (highlighted == i) {
                // Slightly larger ring in the label color to show hover
                cairo_arc(cr, cx, cy, r + hover_bump, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, 0x34/255.0, 0x78/255.0, 0xF6/255.0, 1.0);
                cairo_fill(cr);
            }

            // Filled circle in the label color
            cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, lc->r, lc->g, lc->b, 1.0);
            cairo_fill_preserve(cr);

            // Slightly darker border
            cairo_set_source_rgba(cr, lc->r * 0.7, lc->g * 0.7, lc->b * 0.7, 1.0);
            cairo_set_line_width(cr, MENU_LINE_WIDTH);
            cairo_stroke(cr);
        }
    }

    (void)cw; (void)ch;
}

// Show the label picker popup adjacent to the menu position.
// Returns the selected label index (0 = none, 1-7 = color), or -1 if dismissed.
static int show_label_picker(Display *dpy, Window root,
                             int menu_x, int menu_y,
                             int label_item_y,
                             int screen_w, int screen_h)
{
    int cw = picker_content_w();
    int ch = PICKER_PAD_V * 2 + PICKER_CIRCLE_D;
    int win_w = cw + 2 * PICKER_SHADOW;
    int win_h = ch + 2 * PICKER_SHADOW;

    // Position picker to the right of the menu, aligned with the Label item.
    // If it would go off-screen, flip to the left.
    int edge_nudge = S(4);  // visual overlap into the menu's shadow
    int win_x = menu_x + MENU_WIDTH + 2 * MENU_SHADOW_SIZE - edge_nudge;
    int win_y = menu_y + label_item_y;

    if (win_x + win_w > screen_w) {
        win_x = menu_x - win_w + edge_nudge;
    }
    if (win_y + win_h > screen_h) {
        win_y = screen_h - win_h;
    }
    if (win_x < 0) win_x = 0;
    if (win_y < 0) win_y = 0;

    // Find 32-bit ARGB visual (same logic as the main menu)
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

    if (vis_list && nvisuals > 0) {
        for (int i = 0; i < nvisuals; i++) {
            if (vis_list[i].red_mask   == 0x00FF0000 &&
                vis_list[i].green_mask == 0x0000FF00 &&
                vis_list[i].blue_mask  == 0x000000FF) {
                visual = vis_list[i].visual;
                depth  = 32;
                cmap   = XCreateColormap(dpy, root, visual, AllocNone);
                own_cmap = true;
                break;
            }
        }
        XFree(vis_list);
    }

    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.colormap     = cmap;
    attrs.border_pixel = 0;
    attrs.background_pixel = 0;
    attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                       PointerMotionMask | LeaveWindowMask;

    unsigned long amask = CWOverrideRedirect | CWColormap | CWBorderPixel |
                          CWBackPixel | CWEventMask;

    Window picker_win = XCreateWindow(dpy, root,
        win_x, win_y, win_w, win_h,
        0, depth, InputOutput, visual, amask, &attrs);

    XMapRaised(dpy, picker_win);
    XGrabPointer(dpy, picker_win, True,
        ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
        GrabModeAsync, GrabModeAsync, None, None, CurrentTime);

    cairo_surface_t *surface = cairo_xlib_surface_create(
        dpy, picker_win, visual, win_w, win_h);
    cairo_t *cr = cairo_create(surface);

    paint_picker(cr, win_w, win_h, -1);
    XFlush(dpy);

    int result    = -1;
    bool open     = true;
    int  highlight = -1;
    // Same opening-click race as the main menu: ignore the leftover
    // ButtonRelease that arrives before any in-loop press.
    bool seen_press_in_loop = false;

    while (open) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        switch (ev.type) {
        case Expose:
            if (ev.xexpose.count == 0) {
                paint_picker(cr, win_w, win_h, highlight);
            }
            break;

        case MotionNotify:
        {
            // Adjust coordinates relative to the content area (inside shadow)
            int mx = ev.xmotion.x - PICKER_SHADOW;
            int my = ev.xmotion.y - PICKER_SHADOW;
            int new_hl = picker_hit_test(mx, my, cw, ch);
            if (new_hl != highlight) {
                highlight = new_hl;
                paint_picker(cr, win_w, win_h, highlight);
                XFlush(dpy);
            }
            break;
        }

        case ButtonRelease:
        {
            if (!seen_press_in_loop) break;
            int mx = ev.xbutton.x - PICKER_SHADOW;
            int my = ev.xbutton.y - PICKER_SHADOW;
            int hit = picker_hit_test(mx, my, cw, ch);
            if (hit >= 0) {
                result = hit;  // 0=none, 1-7=color
                open   = false;
            } else {
                // Click outside → dismiss without selecting
                open = false;
            }
            break;
        }

        case ButtonPress:
        {
            seen_press_in_loop = true;
            int mx = ev.xbutton.x - PICKER_SHADOW;
            int my = ev.xbutton.y - PICKER_SHADOW;
            if (picker_hit_test(mx, my, cw, ch) < 0) {
                open = false;
            }
            break;
        }

        case LeaveNotify:
            if (highlight != -1) {
                highlight = -1;
                paint_picker(cr, win_w, win_h, highlight);
                XFlush(dpy);
            }
            break;

        default:
            break;
        }
    }

    XUngrabPointer(dpy, CurrentTime);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    XDestroyWindow(dpy, picker_win);
    if (own_cmap) XFreeColormap(dpy, cmap);
    XFlush(dpy);

    return result;
}

// ── Public: contextmenu_show_icon ────────────────────────────────────

int contextmenu_show_icon(Display *dpy, Window root,
                          int root_x, int root_y,
                          int screen_w, int screen_h,
                          DesktopIcon *icon)
{
    (void)icon;  // Used by caller to apply the returned action

    int menu_content_h = calc_icon_menu_height();
    int win_w = MENU_WIDTH + 2 * MENU_SHADOW_SIZE;
    int win_h = menu_content_h + 2 * MENU_SHADOW_SIZE;

    int win_x = root_x;
    int win_y = root_y;
    if (win_x + win_w > screen_w) win_x = screen_w - win_w;
    if (win_y + win_h > screen_h) win_y = screen_h - win_h;
    if (win_x < 0) win_x = 0;
    if (win_y < 0) win_y = 0;

    // Find ARGB visual
    XVisualInfo tmpl;
    tmpl.screen = DefaultScreen(dpy);
    tmpl.depth  = 32;
    tmpl.class  = TrueColor;
    int nvisuals = 0;
    XVisualInfo *vis_list = XGetVisualInfo(dpy,
        VisualScreenMask | VisualDepthMask | VisualClassMask,
        &tmpl, &nvisuals);

    Visual *visual = DefaultVisual(dpy, DefaultScreen(dpy));
    int depth = DefaultDepth(dpy, DefaultScreen(dpy));
    Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
    bool own_cmap = false;

    if (vis_list && nvisuals > 0) {
        for (int i = 0; i < nvisuals; i++) {
            if (vis_list[i].red_mask   == 0x00FF0000 &&
                vis_list[i].green_mask == 0x0000FF00 &&
                vis_list[i].blue_mask  == 0x000000FF) {
                visual = vis_list[i].visual;
                depth  = 32;
                cmap   = XCreateColormap(dpy, root, visual, AllocNone);
                own_cmap = true;
                break;
            }
        }
        XFree(vis_list);
    }

    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.colormap     = cmap;
    attrs.border_pixel = 0;
    attrs.background_pixel = 0;
    attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                       PointerMotionMask | LeaveWindowMask;

    unsigned long amask = CWOverrideRedirect | CWColormap | CWBorderPixel |
                          CWBackPixel | CWEventMask;

    Window menu_win = XCreateWindow(dpy, root,
        win_x, win_y, win_w, win_h,
        0, depth, InputOutput, visual, amask, &attrs);

    XMapRaised(dpy, menu_win);
    XGrabPointer(dpy, menu_win, True,
        ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
        GrabModeAsync, GrabModeAsync, None, None, CurrentTime);

    cairo_surface_t *surface = cairo_xlib_surface_create(
        dpy, menu_win, visual, win_w, win_h);
    cairo_t *cr = cairo_create(surface);

    paint_icon_menu(cr, win_w, win_h, -1);
    XFlush(dpy);

    int  result      = ICON_ACTION_NONE;
    bool menu_open   = true;
    int  highlighted = -1;
    bool want_label  = false;  // Set when user picks "Label ▶"
    int  label_item_screen_y = 0;  // Absolute y of the Label item (for picker placement)
    // Same opening-click race as the main menu: ignore the leftover
    // ButtonRelease that arrives before any in-loop press.
    bool seen_press_in_loop = false;

    while (menu_open) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        switch (ev.type) {
        case Expose:
            if (ev.xexpose.count == 0) {
                paint_icon_menu(cr, win_w, win_h, highlighted);
            }
            break;

        case MotionNotify:
        {
            int mx = ev.xmotion.x - MENU_SHADOW_SIZE;
            int my = ev.xmotion.y - MENU_SHADOW_SIZE;
            int new_hl = icon_hit_test(mx, my, menu_content_h);
            if (new_hl != highlighted) {
                highlighted = new_hl;
                paint_icon_menu(cr, win_w, win_h, highlighted);
                XFlush(dpy);
            }
            break;
        }

        case ButtonRelease:
        {
            if (!seen_press_in_loop) break;
            int mx = ev.xbutton.x - MENU_SHADOW_SIZE;
            int my = ev.xbutton.y - MENU_SHADOW_SIZE;
            int hit = icon_hit_test(mx, my, menu_content_h);

            if (hit < 0) {
                // Outside all items — dismiss
                if (mx < 0 || mx >= MENU_WIDTH ||
                    my < 0 || my >= menu_content_h) {
                    menu_open = false;
                }
            } else if (icon_menu_items[hit].is_submenu) {
                // "Label ▶" — record that we want to open the picker,
                // then close this menu first so they don't overlap.
                want_label = true;
                // Absolute Y of the label item's top edge in root coordinates
                label_item_screen_y = win_y + MENU_SHADOW_SIZE +
                                      icon_item_y_offset(hit);
                menu_open = false;
            } else {
                // Map internal index to ICON_ACTION_* constant
                switch (hit) {
                case 0:  result = ICON_ACTION_OPEN;  break;
                case 2:  result = ICON_ACTION_INFO;  break;
                case 6:  result = ICON_ACTION_TRASH; break;
                default: result = ICON_ACTION_NONE;  break;
                }
                menu_open = false;
            }
            break;
        }

        case ButtonPress:
        {
            seen_press_in_loop = true;
            int mx = ev.xbutton.x - MENU_SHADOW_SIZE;
            int my = ev.xbutton.y - MENU_SHADOW_SIZE;
            if (mx < 0 || mx >= MENU_WIDTH ||
                my < 0 || my >= menu_content_h) {
                menu_open = false;
            }
            break;
        }

        case LeaveNotify:
            if (highlighted != -1) {
                highlighted = -1;
                paint_icon_menu(cr, win_w, win_h, highlighted);
                XFlush(dpy);
            }
            break;

        default:
            break;
        }
    }

    // Tear down the icon menu
    XUngrabPointer(dpy, CurrentTime);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    XDestroyWindow(dpy, menu_win);
    if (own_cmap) XFreeColormap(dpy, cmap);
    XFlush(dpy);

    // If the user picked "Label ▶", show the label picker now that the
    // main menu is gone. Position it where the Label item was.
    if (want_label) {
        int picked = show_label_picker(dpy, root,
                                       win_x, label_item_screen_y,
                                       0,       // label_item_y already absolute
                                       screen_w, screen_h);
        if (picked >= 0) {
            result = ICON_ACTION_LABEL_BASE + picked;
        }
    }

    return result;
}
