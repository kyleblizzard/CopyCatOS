// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// appmenu.c — Application menu tracking and dropdown rendering
//
// This module does two jobs:
//
// 1. ACTIVE WINDOW TRACKING
//    The X11 window manager updates a property called _NET_ACTIVE_WINDOW
//    on the root window whenever the focused app changes. We read that
//    property, then look up the window's WM_CLASS to figure out which
//    application it is (e.g., "dolphin" -> "Finder").
//
// 2. DROPDOWN MENUS
//    When the user clicks a menu title, we create an override-redirect
//    popup window below it and fill it with menu items. Override-redirect
//    means the window manager ignores the window — it appears instantly
//    with no decorations, just like a real menu.
//
//    Menu items are currently non-functional (clicking dismisses the menu).
//    In the future, these could send commands to the active application
//    via D-Bus or custom X11 messages.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include "appmenu.h"
#include "render.h"

// ── App name mapping ────────────────────────────────────────────────
// Maps WM_CLASS strings to human-readable names. WM_CLASS is a
// standardized X11 property that identifies which program owns a window.
// We match case-insensitively to handle variations.

static const struct {
    const char *wm_class;  // What the X11 window reports
    const char *name;      // What we display in the menu bar
} app_names[] = {
    {"dolphin",          "Finder"},
    {"konsole",          "Terminal"},
    {"kate",             "Kate"},
    {"brave-browser",    "Brave"},
    {"firefox",          "Firefox"},
    {"krita",            "Krita"},
    {"gimp",             "GIMP"},
    {"inkscape",         "Inkscape"},
    {"kdenlive",         "Kdenlive"},
    {"strawberry",       "Strawberry"},
    {"systemsettings",   "System Preferences"},
    {"aura-desktop",     "Finder"},
    {NULL,               NULL}
};

// ── Menu title definitions ──────────────────────────────────────────
// Each app has a specific set of menu titles shown in the bar.
// These are static arrays that live for the entire program lifetime.

static const char *default_menus[] = {"File", "Edit", "View", "Window", "Help"};
static const int   default_menu_count = 5;

static const char *finder_menus[] = {"File", "Edit", "View", "Go", "Window", "Help"};
static const int   finder_menu_count = 6;

static const char *terminal_menus[] = {"Shell", "Edit", "View", "Window", "Help"};
static const int   terminal_menu_count = 5;

static const char *browser_menus[] = {"File", "Edit", "View", "History", "Bookmarks", "Window", "Help"};
static const int   browser_menu_count = 7;

// ── Dropdown menu item definitions ──────────────────────────────────
// A menu item is either a regular item (with a label) or a separator
// (drawn as a horizontal line). The "---" string signals a separator.

// File menu items for Finder
static const char *file_items[] = {
    "New Finder Window", "New Folder", "Open", "Close Window",
    "Get Info", "---", "Move to Trash"
};
static const int file_item_count = 7;

// Edit menu items (shared by most apps)
static const char *edit_items[] = {
    "Undo", "Redo", "---", "Cut", "Copy", "Paste", "Select All"
};
static const int edit_item_count = 7;

// View menu items for Finder
static const char *view_items[] = {
    "as Icons", "as List", "as Columns", "as Cover Flow",
    "---", "Show Path Bar", "Show Status Bar"
};
static const int view_item_count = 7;

// Go menu items for Finder
static const char *go_items[] = {
    "Back", "Forward", "Enclosing Folder", "---",
    "Computer", "Home", "Desktop", "Downloads", "Applications"
};
static const int go_item_count = 9;

// Window menu items
static const char *window_items[] = {
    "Minimize", "Zoom", "---", "Bring All to Front"
};
static const int window_item_count = 4;

// Help menu items
static const char *help_items[] = {
    "Search", "---", "AuraOS Help"
};
static const int help_item_count = 3;

// Shell menu items for Terminal
static const char *shell_items[] = {
    "New Window", "New Tab", "---", "Close Window", "Close Tab"
};
static const int shell_item_count = 5;

// History menu items for browsers
static const char *history_items[] = {
    "Show All History", "---", "Recently Closed"
};
static const int history_item_count = 3;

// Bookmarks menu items for browsers
static const char *bookmarks_items[] = {
    "Show All Bookmarks", "Bookmark This Page...", "---", "Bookmarks Bar"
};
static const int bookmarks_item_count = 4;

// ── Module state ────────────────────────────────────────────────────

// The currently open dropdown popup window. None if no menu is open.
static Window dropdown_win = None;

// Event mask for the dropdown — we need to know about mouse clicks
// so we can dismiss the menu.
static const long dropdown_events = ExposureMask | ButtonPressMask
                                  | PointerMotionMask | LeaveWindowMask;

// ── Initialization ──────────────────────────────────────────────────

void appmenu_init(MenuBar *mb)
{
    (void)mb; // Nothing to initialize yet
}

// ── Active window tracking ──────────────────────────────────────────

void appmenu_update_active(MenuBar *mb)
{
    // Read _NET_ACTIVE_WINDOW from the root window.
    // This property contains the XID (window ID) of the currently
    // focused window, set by the window manager.
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        mb->dpy, mb->root,
        mb->atom_net_active_window,
        0, 1,              // Read 1 long (the window ID)
        False,             // Don't delete the property
        XA_WINDOW,         // Expected type
        &actual_type, &actual_format,
        &nitems, &bytes_after, &data
    );

    if (status != Success || nitems == 0 || !data) {
        // No active window — fall back to Finder
        if (data) XFree(data);
        strncpy(mb->active_app, "Finder", sizeof(mb->active_app) - 1);
        strncpy(mb->active_class, "dolphin", sizeof(mb->active_class) - 1);
        return;
    }

    // Extract the window ID from the property data
    Window active = *(Window *)data;
    XFree(data);

    // A window ID of None or 0 means no window is focused (e.g., the
    // user clicked the desktop background).
    if (active == None || active == 0) {
        strncpy(mb->active_app, "Finder", sizeof(mb->active_app) - 1);
        strncpy(mb->active_class, "dolphin", sizeof(mb->active_class) - 1);
        return;
    }

    // ── Read WM_CLASS from the active window ────────────────────
    // WM_CLASS is a pair of null-terminated strings: "instance\0class\0".
    // We want the second one (the class name), which identifies the
    // application type.
    data = NULL;
    status = XGetWindowProperty(
        mb->dpy, active,
        mb->atom_wm_class,
        0, 256,            // Read up to 256 longs (more than enough)
        False,
        XA_STRING,
        &actual_type, &actual_format,
        &nitems, &bytes_after, &data
    );

    if (status != Success || nitems == 0 || !data) {
        if (data) XFree(data);
        strncpy(mb->active_app, "Finder", sizeof(mb->active_app) - 1);
        strncpy(mb->active_class, "dolphin", sizeof(mb->active_class) - 1);
        return;
    }

    // WM_CLASS contains two null-terminated strings packed together.
    // Skip past the first string (instance name) to get the class name.
    const char *instance = (const char *)data;
    const char *classname = instance;

    // Find the end of the first string
    size_t len = strlen(instance);
    if (len + 1 < nitems) {
        classname = instance + len + 1; // Second string = class name
    }

    // Store the raw class name (lowercased for matching)
    char lower_class[128];
    strncpy(lower_class, classname, sizeof(lower_class) - 1);
    lower_class[sizeof(lower_class) - 1] = '\0';
    for (int i = 0; lower_class[i]; i++) {
        lower_class[i] = (char)tolower((unsigned char)lower_class[i]);
    }
    strncpy(mb->active_class, lower_class, sizeof(mb->active_class) - 1);

    // ── Map class name to display name ──────────────────────────
    // Check our lookup table for a known mapping.
    const char *display_name = NULL;
    for (int i = 0; app_names[i].wm_class != NULL; i++) {
        if (strcasecmp(lower_class, app_names[i].wm_class) == 0) {
            display_name = app_names[i].name;
            break;
        }
    }

    if (display_name) {
        strncpy(mb->active_app, display_name, sizeof(mb->active_app) - 1);
    } else {
        // Unknown app — capitalize the first letter and use that.
        // e.g., "vlc" -> "Vlc"
        strncpy(mb->active_app, lower_class, sizeof(mb->active_app) - 1);
        if (mb->active_app[0]) {
            mb->active_app[0] = (char)toupper((unsigned char)mb->active_app[0]);
        }
    }

    XFree(data);
}

// ── Menu title lookup ───────────────────────────────────────────────

void appmenu_get_menus(const char *app_class, const char ***menus, int *count)
{
    // Match the app class against known apps and return the appropriate
    // set of menu titles. Default to the generic set for unknown apps.

    if (strcasecmp(app_class, "dolphin") == 0 ||
        strcasecmp(app_class, "aura-desktop") == 0) {
        *menus = finder_menus;
        *count = finder_menu_count;
    } else if (strcasecmp(app_class, "konsole") == 0) {
        *menus = terminal_menus;
        *count = terminal_menu_count;
    } else if (strcasecmp(app_class, "brave-browser") == 0 ||
               strcasecmp(app_class, "firefox") == 0) {
        *menus = browser_menus;
        *count = browser_menu_count;
    } else {
        *menus = default_menus;
        *count = default_menu_count;
    }
}

// ── Dropdown rendering helpers ──────────────────────────────────────

// Get the list of items for a specific menu index within the current app.
// Returns the item array and count through out-parameters.
static void get_dropdown_items(const char *app_class, int menu_index,
                               const char ***items, int *count)
{
    // First, figure out which menu titles this app uses
    const char **menus;
    int menu_count;
    appmenu_get_menus(app_class, &menus, &menu_count);

    if (menu_index < 0 || menu_index >= menu_count) {
        *items = NULL;
        *count = 0;
        return;
    }

    // Match the menu title to its items
    const char *title = menus[menu_index];

    if (strcmp(title, "File") == 0) {
        *items = file_items; *count = file_item_count;
    } else if (strcmp(title, "Edit") == 0) {
        *items = edit_items; *count = edit_item_count;
    } else if (strcmp(title, "View") == 0) {
        *items = view_items; *count = view_item_count;
    } else if (strcmp(title, "Go") == 0) {
        *items = go_items; *count = go_item_count;
    } else if (strcmp(title, "Window") == 0) {
        *items = window_items; *count = window_item_count;
    } else if (strcmp(title, "Help") == 0) {
        *items = help_items; *count = help_item_count;
    } else if (strcmp(title, "Shell") == 0) {
        *items = shell_items; *count = shell_item_count;
    } else if (strcmp(title, "History") == 0) {
        *items = history_items; *count = history_item_count;
    } else if (strcmp(title, "Bookmarks") == 0) {
        *items = bookmarks_items; *count = bookmarks_item_count;
    } else {
        // Unknown menu — show nothing
        *items = NULL;
        *count = 0;
    }
}

// Helper: draw a rounded rectangle path (same as render.c but local).
static void dropdown_rounded_rect(cairo_t *cr, double x, double y,
                                  double w, double h, double radius)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - radius, y + radius,     radius, -M_PI / 2, 0);
    cairo_arc(cr, x + w - radius, y + h - radius, radius, 0,          M_PI / 2);
    cairo_arc(cr, x + radius,     y + h - radius, radius, M_PI / 2,   M_PI);
    cairo_arc(cr, x + radius,     y + radius,     radius, M_PI,       3 * M_PI / 2);
    cairo_close_path(cr);
}

// Paint the contents of a dropdown popup window.
static void paint_dropdown(MenuBar *mb, Window win, const char **items,
                           int item_count, int popup_w, int popup_h)
{
    // Create a Cairo surface for the popup window
    cairo_surface_t *surface = cairo_xlib_surface_create(
        mb->dpy, win,
        DefaultVisual(mb->dpy, mb->screen),
        popup_w, popup_h
    );
    cairo_t *cr = cairo_create(surface);

    // ── Clear background ────────────────────────────────────────
    // Fill with slightly transparent white to match Snow Leopard's
    // menu appearance (not fully opaque, slightly translucent).
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 245.0 / 255.0);
    dropdown_rounded_rect(cr, 0, 0, popup_w, popup_h, 5.0);
    cairo_fill(cr);

    // ── Border ──────────────────────────────────────────────────
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 60.0 / 255.0);
    cairo_set_line_width(cr, 1.0);
    dropdown_rounded_rect(cr, 0.5, 0.5, popup_w - 1, popup_h - 1, 5.0);
    cairo_stroke(cr);

    // ── Draw each item ──────────────────────────────────────────
    int y = 4; // Top padding inside the dropdown

    for (int i = 0; i < item_count; i++) {
        if (strcmp(items[i], "---") == 0) {
            // Separator: a thin gray line with horizontal margins
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 40.0 / 255.0);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, 10, y + 3.5); // 3px vertical margin, 10px horizontal
            cairo_line_to(cr, popup_w - 10, y + 3.5);
            cairo_stroke(cr);
            y += 7; // 3px margin + 1px line + 3px margin
        } else {
            // Regular menu item: text on a 22px row
            // Create a Pango layout for the item text
            PangoLayout *layout = pango_cairo_create_layout(cr);
            pango_layout_set_text(layout, items[i], -1);

            PangoFontDescription *desc = pango_font_description_from_string(
                "Lucida Grande 13"
            );
            pango_layout_set_font_description(layout, desc);
            pango_font_description_free(desc);

            // Draw the text in dark gray
            cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
            cairo_move_to(cr, 18, y + 2); // 18px left indent, 2px top padding
            pango_cairo_show_layout(cr, layout);

            g_object_unref(layout);
            y += 22; // Each item row is 22px tall
        }
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    XFlush(mb->dpy);
}

// ── Public API ──────────────────────────────────────────────────────

void appmenu_show_dropdown(MenuBar *mb, int menu_index, int x)
{
    // Dismiss any existing dropdown first
    appmenu_dismiss(mb);

    // Get the items for this menu
    const char **items;
    int item_count;
    get_dropdown_items(mb->active_class, menu_index, &items, &item_count);

    if (!items || item_count == 0) return;

    // ── Calculate popup size ────────────────────────────────────
    // Width: find the widest item + padding. Minimum 180px.
    int popup_w = 180;
    for (int i = 0; i < item_count; i++) {
        if (strcmp(items[i], "---") != 0) {
            double w = render_measure_text(items[i], false);
            int needed = (int)w + 40; // 18px left indent + 22px right padding
            if (needed > popup_w) popup_w = needed;
        }
    }

    // Height: 22px per item, 7px per separator, plus top/bottom padding
    int popup_h = 8; // 4px top + 4px bottom padding
    for (int i = 0; i < item_count; i++) {
        popup_h += (strcmp(items[i], "---") == 0) ? 7 : 22;
    }

    // ── Create the popup window ─────────────────────────────────
    // Override-redirect = true means the window manager won't touch
    // this window (no decorations, no placement, no focus stealing).
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.event_mask = dropdown_events;
    attrs.background_pixel = WhitePixel(mb->dpy, mb->screen);

    dropdown_win = XCreateWindow(
        mb->dpy, mb->root,
        x, MENUBAR_HEIGHT,        // Position just below the menu bar
        (unsigned int)popup_w,
        (unsigned int)popup_h,
        0,                         // No border
        CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWEventMask | CWBackPixel,
        &attrs
    );

    XMapRaised(mb->dpy, dropdown_win);

    // Paint the dropdown contents
    paint_dropdown(mb, dropdown_win, items, item_count, popup_w, popup_h);
}

void appmenu_dismiss(MenuBar *mb)
{
    if (dropdown_win != None) {
        XDestroyWindow(mb->dpy, dropdown_win);
        dropdown_win = None;
        XFlush(mb->dpy);
    }
}

void appmenu_cleanup(void)
{
    // dropdown_win will be destroyed when the display closes,
    // but we clean up explicitly for good hygiene.
    dropdown_win = None;
}
