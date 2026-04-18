// CopyCatOS — by Kyle Blizzard at Blizzard.show

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
//    Hover highlighting is handled by tracking mouse motion inside the
//    dropdown and repainting when the hovered item changes. Keyboard
//    shortcuts are drawn right-aligned next to each item.

#define _GNU_SOURCE  // For M_PI and strcasecmp under strict C11

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#include "appmenu.h"
#include "render.h"

// ── Font scaling helper (mirrors the one in render.c) ──────────────
static char *appmenu_scaled_font(const char *base_name, int base_size)
{
    static char buf[128];
    int scaled_size = (int)(base_size * menubar_scale + 0.5);
    if (scaled_size < base_size) scaled_size = base_size;
    snprintf(buf, sizeof(buf), "%s %d", base_name, scaled_size);
    return buf;
}

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
    {"desktop",       "Finder"},

    // KDE apps — appear as their KDE identities
    {"ark",               "Archive Utility"},
    {"spectacle",         "Screenshot"},
    {"okular",            "Preview"},
    {"gwenview",          "Preview"},
    {"kolourpaint",       "Paintbrush"},
    {"kcolorchooser",     "Digital Color Meter"},
    {"kwrite",            "TextEdit"},
    {"gedit",             "TextEdit"},
    {"mousepad",          "TextEdit"},
    {"featherpad",        "TextEdit"},
    {"libreoffice",       "Pages"},
    {"soffice",           "Pages"},
    {"vlc",               "QuickTime Player"},
    {"mpv",               "QuickTime Player"},
    {"totem",             "QuickTime Player"},
    {"audacity",          "GarageBand"},
    {"obs",               "QuickTime Player"},
    {"signal",            "Messages"},
    {"thunderbird",       "Mail"},
    {"evolution",         "Mail"},
    {"nm-connection-editor", "Network Preferences"},
    {"systemcontrol",       "System Preferences"},
    {"searchsystem",      "Spotlight"},
    {"steam",             "Steam"},
    {"lutris",            "Lutris"},
    {"heroic",            "Game Center"},

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

// System Preferences menu titles
static const char *sysprefs_menus[] = {"File", "Edit", "View", "Window", "Help"};
static const int   sysprefs_menu_count = 5;

// ── Dropdown menu item definitions ──────────────────────────────────
// A menu item is either a regular item (with a label) or a separator
// (drawn as a horizontal line). The "---" string signals a separator.

// File menu items for Finder
static const char *file_items[] = {
    "New Finder Window", "New Folder", "Open", "Close Window",
    "---", "Get Info", "---", "Move to Trash"
};
static const int file_item_count = 8;

// Keyboard shortcut labels for File menu items.
// NULL means no shortcut. These are drawn right-aligned in the dropdown.
static const char *file_shortcuts[] = {
    "\xe2\x8c\x98N", "\xe2\x87\xa7\xe2\x8c\x98N", "\xe2\x8c\x98O", "\xe2\x8c\x98W",
    NULL, "\xe2\x8c\x98I", NULL, "\xe2\x8c\x98\xe2\x8c\xab"
};

// Edit menu items (shared by most apps)
static const char *edit_items[] = {
    "Undo", "Redo", "---", "Cut", "Copy", "Paste", "Select All"
};
static const int edit_item_count = 7;

// Keyboard shortcut labels for Edit menu items
static const char *edit_shortcuts[] = {
    "\xe2\x8c\x98Z", "\xe2\x87\xa7\xe2\x8c\x98Z", NULL,
    "\xe2\x8c\x98X", "\xe2\x8c\x98C", "\xe2\x8c\x98V", "\xe2\x8c\x98A"
};

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

// Keyboard shortcut labels for Window menu items.
// ⌘M = minimize. Zoom has no keyboard shortcut in Snow Leopard.
static const char *window_shortcuts[] = {
    "\xe2\x8c\x98M",   // ⌘M — Minimize
    NULL,               // Zoom (no keyboard shortcut in SL)
    NULL,               // --- separator
    NULL                // Bring All to Front
};

// Help menu items
static const char *help_items[] = {
    "Search", "---", "CopyCatOS Help"
};
static const int help_item_count = 3;

// Shell menu items for Terminal
static const char *shell_items[] = {
    "New Window", "New Tab", "---", "Close Window", "Close Tab"
};
static const int shell_item_count = 5;

// Keyboard shortcut labels for Shell menu items.
static const char *shell_shortcuts[] = {
    "\xe2\x8c\x98N",                           // ⌘N — New Window
    "\xe2\x8c\x98T",                           // ⌘T — New Tab
    NULL,                                       // --- separator
    "\xe2\x8c\x98W",                           // ⌘W — Close Window
    "\xe2\x8c\xa5\xe2\x8c\x98W"               // ⌥⌘W — Close Tab (Option+Cmd+W)
};

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

// State for the currently open dropdown — needed for hover tracking
// and repaint on mouse motion within the popup.
static const char **dropdown_items     = NULL;   // Points into static item arrays
static const char **dropdown_shortcuts = NULL;   // Keyboard shortcut labels (may be NULL)
static int          dropdown_item_count = 0;
static int          dropdown_hover      = -1;    // Which item the mouse is over (-1 = none)
static int          dropdown_w          = 0;     // Popup width in pixels
static int          dropdown_h          = 0;     // Popup height in pixels

// Event mask for the dropdown — we need mouse clicks, motion for hover
// highlighting, expose for repaints, and key presses for Escape.
static const long dropdown_events = ExposureMask | ButtonPressMask
                                  | PointerMotionMask | LeaveWindowMask
                                  | KeyPressMask;

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
        strcasecmp(app_class, "desktop") == 0) {
        *menus = finder_menus;
        *count = finder_menu_count;
    } else if (strcasecmp(app_class, "konsole") == 0) {
        *menus = terminal_menus;
        *count = terminal_menu_count;
    } else if (strcasecmp(app_class, "brave-browser") == 0 ||
               strcasecmp(app_class, "firefox") == 0) {
        *menus = browser_menus;
        *count = browser_menu_count;
    } else if (strcasecmp(app_class, "systemcontrol") == 0 ||
               strcasecmp(app_class, "systemsettings") == 0) {
        *menus = sysprefs_menus;
        *count = sysprefs_menu_count;
    } else {
        *menus = default_menus;
        *count = default_menu_count;
    }
}

// ── Dropdown rendering helpers ──────────────────────────────────────

// Get the list of items and keyboard shortcuts for a specific menu index
// within the current app. Returns the item array, shortcut array, and
// count through out-parameters. The shortcuts pointer may be NULL if the
// menu has no shortcuts defined.
static void get_dropdown_items(const char *app_class, int menu_index,
                               const char ***items, const char ***shortcuts,
                               int *count)
{
    // First, figure out which menu titles this app uses
    const char **menus;
    int menu_count;
    appmenu_get_menus(app_class, &menus, &menu_count);

    // Default: no shortcuts
    *shortcuts = NULL;

    if (menu_index < 0 || menu_index >= menu_count) {
        *items = NULL;
        *count = 0;
        return;
    }

    // Match the menu title to its items and optional shortcuts
    const char *title = menus[menu_index];

    if (strcmp(title, "File") == 0) {
        *items = file_items; *count = file_item_count;
        *shortcuts = file_shortcuts;
    } else if (strcmp(title, "Edit") == 0) {
        *items = edit_items; *count = edit_item_count;
        *shortcuts = edit_shortcuts;
    } else if (strcmp(title, "View") == 0) {
        *items = view_items; *count = view_item_count;
    } else if (strcmp(title, "Go") == 0) {
        *items = go_items; *count = go_item_count;
    } else if (strcmp(title, "Window") == 0) {
        *items = window_items; *count = window_item_count;
        *shortcuts = window_shortcuts;
    } else if (strcmp(title, "Help") == 0) {
        *items = help_items; *count = help_item_count;
    } else if (strcmp(title, "Shell") == 0) {
        *items = shell_items; *count = shell_item_count;
        *shortcuts = shell_shortcuts;
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

// Convert a y-coordinate inside the dropdown to a menu item index.
// Returns -1 if the y position is in padding or on a separator.
static int y_to_item_index(int y)
{
    int cur_y = S(4); // top padding (scaled)
    for (int i = 0; i < dropdown_item_count; i++) {
        int row_h = (strcmp(dropdown_items[i], "---") == 0) ? S(7) : S(22);
        if (y >= cur_y && y < cur_y + row_h) {
            // Only return non-separator items as hoverable
            if (strcmp(dropdown_items[i], "---") == 0) return -1;
            return i;
        }
        cur_y += row_h;
    }
    return -1;
}

// Paint the contents of the dropdown popup window.
// This is called on initial show and whenever hover state changes.
static void paint_dropdown(MenuBar *mb)
{
    if (dropdown_win == None || !dropdown_items) return;

    // Create a Cairo surface for the popup window
    cairo_surface_t *surface = cairo_xlib_surface_create(
        mb->dpy, dropdown_win,
        DefaultVisual(mb->dpy, mb->screen),
        dropdown_w, dropdown_h
    );
    cairo_t *cr = cairo_create(surface);

    // ── Clear background ────────────────────────────────────────
    // Fill with slightly transparent white to match Snow Leopard's
    // menu appearance (not fully opaque, slightly translucent).
    // Corner radius scales proportionally with the menubar height.
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 245.0 / 255.0);
    dropdown_rounded_rect(cr, 0, 0, dropdown_w, dropdown_h, SF(5.0));
    cairo_fill(cr);

    // ── Border ──────────────────────────────────────────────────
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 60.0 / 255.0);
    cairo_set_line_width(cr, 1.0);
    dropdown_rounded_rect(cr, 0.5, 0.5, dropdown_w - 1, dropdown_h - 1, SF(5.0));
    cairo_stroke(cr);

    // ── Draw each item (all dimensions scale proportionally) ────
    int y = S(4); // Top padding inside the dropdown

    for (int i = 0; i < dropdown_item_count; i++) {
        if (strcmp(dropdown_items[i], "---") == 0) {
            // Separator: a thin gray line with scaled horizontal margins
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 40.0 / 255.0);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, S(10), y + SF(3.5));
            cairo_line_to(cr, dropdown_w - S(10), y + SF(3.5));
            cairo_stroke(cr);
            y += S(7);
        } else {
            // ── Hover highlight ─────────────────────────────────
            // If this item is hovered, draw a blue rounded-rect background
            // matching Snow Leopard's selection color (#3875D7).
            bool hovered = (i == dropdown_hover);
            if (hovered) {
                // Snow Leopard uses a blue gradient for selected items.
                // We use a solid blue that matches the top of that gradient.
                cairo_set_source_rgb(cr, 56.0/255.0, 117.0/255.0, 215.0/255.0);
                dropdown_rounded_rect(cr, S(4), y, dropdown_w - S(8), S(22), SF(3.0));
                cairo_fill(cr);
            }

            // ── Item label (left side) with scaled font ─────────
            PangoLayout *layout = pango_cairo_create_layout(cr);
            pango_layout_set_text(layout, dropdown_items[i], -1);

            PangoFontDescription *desc = pango_font_description_from_string(
                appmenu_scaled_font("Lucida Grande", 13)
            );
            pango_layout_set_font_description(layout, desc);
            pango_font_description_free(desc);

            // Hovered items use white text; normal items use dark gray
            if (hovered) {
                cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            } else {
                cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
            }
            cairo_move_to(cr, S(18), y + S(2)); // Scaled left indent and top padding
            pango_cairo_show_layout(cr, layout);
            g_object_unref(layout);

            // ── Keyboard shortcut (right side) with scaled font ─
            // Draw the shortcut label right-aligned if one exists
            if (dropdown_shortcuts && dropdown_shortcuts[i]) {
                PangoLayout *sc_layout = pango_cairo_create_layout(cr);
                pango_layout_set_text(sc_layout, dropdown_shortcuts[i], -1);

                PangoFontDescription *sc_desc = pango_font_description_from_string(
                    appmenu_scaled_font("Lucida Grande", 12)
                );
                pango_layout_set_font_description(sc_layout, sc_desc);
                pango_font_description_free(sc_desc);

                // Measure the shortcut text width for right-alignment
                int sc_w, sc_h;
                pango_layout_get_pixel_size(sc_layout, &sc_w, &sc_h);

                if (hovered) {
                    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
                } else {
                    cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
                }
                cairo_move_to(cr, dropdown_w - sc_w - S(14), y + S(2));
                pango_cairo_show_layout(cr, sc_layout);
                g_object_unref(sc_layout);
            }

            y += S(22); // Each item row height scales proportionally
        }
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    XFlush(mb->dpy);
}

// ── Public API ──────────────────────────────────────────────────────

Window appmenu_get_dropdown_win(void)
{
    return dropdown_win;
}

void appmenu_show_dropdown(MenuBar *mb, int menu_index, int x)
{
    // Dismiss any existing dropdown first
    appmenu_dismiss(mb);

    // Get the items for this menu
    const char **items;
    const char **shortcuts = NULL;
    int item_count;
    get_dropdown_items(mb->active_class, menu_index, &items, &shortcuts, &item_count);

    if (!items || item_count == 0) return;

    // Store in module state for hover tracking and repaint
    dropdown_items     = items;
    dropdown_shortcuts = shortcuts;
    dropdown_item_count = item_count;
    dropdown_hover     = -1;

    // ── Calculate popup size (all dimensions scale proportionally) ──
    // Width: find the widest item + shortcut + padding. Minimum S(200) px.
    int popup_w = S(200);
    for (int i = 0; i < item_count; i++) {
        if (strcmp(items[i], "---") != 0) {
            double w = render_measure_text(items[i], false);
            // Account for shortcut width if present
            int shortcut_extra = 0;
            if (shortcuts && shortcuts[i]) {
                shortcut_extra = (int)render_measure_text(shortcuts[i], false) + S(30);
            }
            int needed = (int)w + S(40) + shortcut_extra;
            if (needed > popup_w) popup_w = needed;
        }
    }

    // Height: S(22) per item, S(7) per separator, plus scaled top/bottom padding
    int popup_h = S(8); // S(4) top + S(4) bottom padding
    for (int i = 0; i < item_count; i++) {
        popup_h += (strcmp(items[i], "---") == 0) ? S(7) : S(22);
    }

    dropdown_w = popup_w;
    dropdown_h = popup_h;

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
    paint_dropdown(mb);

}

// Get the currently active (focused) client window XID from _NET_ACTIVE_WINDOW.
// Returns None if no window is focused. Used when menu items need to target
// the active window (e.g., Minimize, Close Window).
static Window get_active_window(MenuBar *mb)
{
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        mb->dpy, mb->root,
        mb->atom_net_active_window,
        0, 1, False, XA_WINDOW,
        &actual_type, &actual_format,
        &nitems, &bytes_after, &data
    );

    if (status != Success || nitems == 0 || !data) {
        if (data) XFree(data);
        return None;
    }

    Window w = *(Window *)data;
    XFree(data);
    return w;
}

// Dispatch a menu item action when the user clicks a dropdown item.
// 'item_label' is the text of the clicked item (e.g., "Minimize", "Close Window").
// Most actions send ClientMessage events to the WM via the root window.
static void dispatch_menu_action(MenuBar *mb, const char *item_label)
{
    if (!item_label || strcmp(item_label, "---") == 0) return;

    // Get the currently focused window — most actions target it.
    Window active = get_active_window(mb);

    // ── Window menu actions ──────────────────────────────────────
    if (strcmp(item_label, "Minimize") == 0) {
        // Send WM_CHANGE_STATE with IconicState to ask the WM to minimize.
        // The WM (moonrock) handles this and triggers the genie animation.
        // IconicState = 3 per ICCCM. This mirrors what clicking the yellow
        // traffic light button does — the code path is identical in the WM.
        if (active == None) return;
        XEvent ev = {0};
        ev.type                 = ClientMessage;
        ev.xclient.window       = active;
        ev.xclient.message_type = mb->atom_wm_change_state;
        ev.xclient.format       = 32;
        ev.xclient.data.l[0]    = 3; // IconicState
        XSendEvent(mb->dpy, mb->root, False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        XFlush(mb->dpy);

    } else if (strcmp(item_label, "Close Window") == 0 ||
               strcmp(item_label, "Close Tab") == 0) {
        // Send _NET_CLOSE_WINDOW to ask the WM to close the active window.
        // The WM sends WM_DELETE_WINDOW to the app so it can save and quit cleanly.
        if (active == None) return;
        XEvent ev = {0};
        ev.type                 = ClientMessage;
        ev.xclient.window       = active;
        ev.xclient.message_type = mb->atom_net_close_window;
        ev.xclient.format       = 32;
        ev.xclient.data.l[0]    = 0; // timestamp (0 = use server time)
        ev.xclient.data.l[1]    = 0; // source indication (0 = unknown)
        XSendEvent(mb->dpy, mb->root, False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        XFlush(mb->dpy);

    } else if (strcmp(item_label, "Zoom") == 0) {
        // Request a maximize toggle via _NET_ACTIVE_WINDOW ClientMessage.
        // For now we re-activate the window to bring it front.
        // TODO: wire to _NET_WM_STATE maximize when WM supports it.
        if (active == None) return;
        XEvent ev = {0};
        ev.type                 = ClientMessage;
        ev.xclient.window       = active;
        ev.xclient.message_type = mb->atom_net_active_window;
        ev.xclient.format       = 32;
        ev.xclient.data.l[0]    = 2; // source: pager/tool
        XSendEvent(mb->dpy, mb->root, False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        XFlush(mb->dpy);

    } else if (strcmp(item_label, "Bring All to Front") == 0) {
        // Raise the active window to the top — simple implementation for now.
        if (active == None) return;
        XRaiseWindow(mb->dpy, active);
        XFlush(mb->dpy);

    }
    // For all other menu items (File, Edit, Go, Help, etc.), we currently
    // show the menu for informational purposes only. Full app integration
    // would require IPC with each application, which is app-specific.
    // The most impactful items (Minimize, Close) are handled above.
}

bool appmenu_handle_dropdown_event(MenuBar *mb, XEvent *ev, bool *should_dismiss)
{
    *should_dismiss = false;

    if (dropdown_win == None) return false;

    // Only handle events for our dropdown window
    Window ev_win = None;
    switch (ev->type) {
        case Expose:       ev_win = ev->xexpose.window; break;
        case ButtonPress:  ev_win = ev->xbutton.window; break;
        case MotionNotify: ev_win = ev->xmotion.window; break;
        case LeaveNotify:  ev_win = ev->xcrossing.window; break;
        case KeyPress:     ev_win = ev->xkey.window; break;
        default: return false;
    }

    if (ev_win != dropdown_win) return false;

    switch (ev->type) {
        case Expose:
            // Repaint the dropdown on expose
            if (ev->xexpose.count == 0) {
                paint_dropdown(mb);
            }
            return true;

        case MotionNotify: {
            // Update hover state based on mouse position
            int new_hover = y_to_item_index(ev->xmotion.y);
            if (new_hover != dropdown_hover) {
                dropdown_hover = new_hover;
                paint_dropdown(mb);
            }
            return true;
        }

        case LeaveNotify:
            // Mouse left the dropdown — clear hover
            if (dropdown_hover != -1) {
                dropdown_hover = -1;
                paint_dropdown(mb);
            }
            return true;

        case ButtonPress:
            // Click on a menu item. If the mouse is hovering over a real item
            // (not a separator, not empty space), dispatch its action before
            // dismissing the dropdown.
            if (dropdown_hover >= 0 && dropdown_hover < dropdown_item_count) {
                dispatch_menu_action(mb, dropdown_items[dropdown_hover]);
            }
            *should_dismiss = true;
            return true;

        case KeyPress: {
            // Escape key dismisses the dropdown
            KeySym sym = XLookupKeysym(&ev->xkey, 0);
            if (sym == XK_Escape) {
                *should_dismiss = true;
            }
            return true;
        }

        default:
            return false;
    }
}

void appmenu_dismiss(MenuBar *mb)
{
    if (dropdown_win != None) {
        XDestroyWindow(mb->dpy, dropdown_win);
        dropdown_win = None;
        dropdown_items = NULL;
        dropdown_shortcuts = NULL;
        dropdown_item_count = 0;
        dropdown_hover = -1;
        XFlush(mb->dpy);
    }
}

void appmenu_cleanup(void)
{
    // dropdown_win will be destroyed when the display closes,
    // but we clean up explicitly for good hygiene.
    dropdown_win = None;
    dropdown_items = NULL;
    dropdown_shortcuts = NULL;
    dropdown_item_count = 0;
    dropdown_hover = -1;
}
