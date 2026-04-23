// CopyCatOS — by Kyle Blizzard at Blizzard.show

// appmenu.c — Application menu tracking + dropdown rendering.
//
// Three responsibilities:
//
// 1. ACTIVE WINDOW TRACKING
//    Read _NET_ACTIVE_WINDOW off the root, resolve WM_CLASS into a
//    display name, write both back to mb->active_app / ->active_class.
//
// 2. MENU MODEL
//    Native fallback: five built-in MenuNode trees, one per app class
//    we know (Finder, Terminal, browser, sysprefs, default). Built once
//    in appmenu_init from the Snow Leopard defaults.
//    Legacy mode: when the active window is registered with the
//    AppMenu.Registrar bridge, we spin up a DbusMenuClient pointed at
//    that endpoint. Its imported root replaces the built-in fallback
//    while that window is active. Both paths hand the renderer the
//    same MenuNode shape — the source-tag flag on each node decides
//    which dispatch branch fires (legacy → dbusmenu Event; built-in →
//    label-based ICCCM/EWMH ClientMessage).
//
// 3. DROPDOWN RENDERING
//    Submenus need to drill in (File → Recent Files → individual file
//    names on Qt's AppMenu). A bounded stack of popup windows lets the
//    user hover-to-open arbitrarily deep. Each level paints its own
//    Cairo surface; hover / spawn / dismiss logic is per-level.

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
#include "appmenu_bridge.h"
#include "dbusmenu_client.h"
#include "menu_model.h"
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
// WM_CLASS → display name. Case-insensitive match.

static const struct {
    const char *wm_class;
    const char *name;
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
    {"desktop",          "Finder"},
    {"ark",              "Archive Utility"},
    {"spectacle",        "Screenshot"},
    {"okular",           "Preview"},
    {"gwenview",         "Preview"},
    {"kolourpaint",      "Paintbrush"},
    {"kcolorchooser",    "Digital Color Meter"},
    {"kwrite",           "TextEdit"},
    {"gedit",            "TextEdit"},
    {"mousepad",         "TextEdit"},
    {"featherpad",       "TextEdit"},
    {"libreoffice",      "Pages"},
    {"soffice",          "Pages"},
    {"vlc",              "QuickTime Player"},
    {"mpv",              "QuickTime Player"},
    {"totem",            "QuickTime Player"},
    {"audacity",         "GarageBand"},
    {"obs",              "QuickTime Player"},
    {"signal",           "Messages"},
    {"thunderbird",      "Mail"},
    {"evolution",        "Mail"},
    {"nm-connection-editor", "Network Preferences"},
    {"systemcontrol",    "System Preferences"},
    {"searchsystem",     "Spotlight"},
    {"steam",            "Steam"},
    {"lutris",           "Lutris"},
    {"heroic",           "Game Center"},
    {NULL, NULL}
};

// ── Apple-key accelerator glyphs ────────────────────────────────────
// UTF-8 byte sequences for the four modifier glyphs the Aqua menu
// renders. Ctrl is mapped to ⌘ (the standard Snow Leopard substitution
// for Linux apps — a Linux-native Ctrl+S becomes ⌘S on our bar).

#define GLYPH_CMD        "\xe2\x8c\x98" // ⌘
#define GLYPH_OPT        "\xe2\x8c\xa5" // ⌥
#define GLYPH_SHIFT      "\xe2\x87\xa7" // ⇧
#define GLYPH_CTRL_CARET "\xe2\x8c\x83" // ⌃ (unused — see shortcut_to_aqua note)
#define GLYPH_BACKSPACE  "\xe2\x8c\xab" // ⌫

// ── Built-in MenuNode trees ─────────────────────────────────────────
// One root per app class we recognise. These replace the static char *
// arrays that used to live here — now both the native and the legacy
// render paths walk MenuNodes, which keeps dropdown rendering source-
// agnostic.

static MenuNode *builtin_default_root;
static MenuNode *builtin_finder_root;
static MenuNode *builtin_terminal_root;
static MenuNode *builtin_browser_root;
static MenuNode *builtin_sysprefs_root;

static MenuNode *mk_item(const char *label, const char *shortcut)
{
    MenuNode *n = menu_node_new_item(label);
    if (shortcut) n->shortcut = strdup(shortcut);
    // Built-in trees dispatch by label (legacy ICCCM/EWMH paths from
    // the Snow Leopard fallback). action_kind stays MENU_ACTION_NONE —
    // dispatch_menu_action strcmp's the label.
    return n;
}

// Shared File submenu used by all non-Terminal roots.
static MenuNode *build_finder_file_menu(void)
{
    MenuNode *file = menu_node_new_item("File");
    menu_node_add_child(file, mk_item("New Finder Window", GLYPH_CMD "N"));
    menu_node_add_child(file, mk_item("New Folder",        GLYPH_SHIFT GLYPH_CMD "N"));
    menu_node_add_child(file, mk_item("Open",              GLYPH_CMD "O"));
    menu_node_add_child(file, mk_item("Close Window",      GLYPH_CMD "W"));
    menu_node_add_child(file, menu_node_new_separator());
    menu_node_add_child(file, mk_item("Get Info",          GLYPH_CMD "I"));
    menu_node_add_child(file, menu_node_new_separator());
    menu_node_add_child(file, mk_item("Move to Trash",     GLYPH_CMD GLYPH_BACKSPACE));
    return file;
}

static MenuNode *build_generic_file_menu(void)
{
    MenuNode *file = menu_node_new_item("File");
    menu_node_add_child(file, mk_item("New",          GLYPH_CMD "N"));
    menu_node_add_child(file, mk_item("Open",         GLYPH_CMD "O"));
    menu_node_add_child(file, mk_item("Close Window", GLYPH_CMD "W"));
    menu_node_add_child(file, menu_node_new_separator());
    menu_node_add_child(file, mk_item("Save",         GLYPH_CMD "S"));
    return file;
}

static MenuNode *build_edit_menu(void)
{
    MenuNode *edit = menu_node_new_item("Edit");
    menu_node_add_child(edit, mk_item("Undo",       GLYPH_CMD "Z"));
    menu_node_add_child(edit, mk_item("Redo",       GLYPH_SHIFT GLYPH_CMD "Z"));
    menu_node_add_child(edit, menu_node_new_separator());
    menu_node_add_child(edit, mk_item("Cut",        GLYPH_CMD "X"));
    menu_node_add_child(edit, mk_item("Copy",       GLYPH_CMD "C"));
    menu_node_add_child(edit, mk_item("Paste",      GLYPH_CMD "V"));
    menu_node_add_child(edit, mk_item("Select All", GLYPH_CMD "A"));
    return edit;
}

static MenuNode *build_finder_view_menu(void)
{
    MenuNode *view = menu_node_new_item("View");
    menu_node_add_child(view, mk_item("as Icons",       NULL));
    menu_node_add_child(view, mk_item("as List",        NULL));
    menu_node_add_child(view, mk_item("as Columns",     NULL));
    menu_node_add_child(view, mk_item("as Cover Flow",  NULL));
    menu_node_add_child(view, menu_node_new_separator());
    menu_node_add_child(view, mk_item("Show Path Bar",   NULL));
    menu_node_add_child(view, mk_item("Show Status Bar", NULL));
    return view;
}

static MenuNode *build_generic_view_menu(void)
{
    MenuNode *view = menu_node_new_item("View");
    menu_node_add_child(view, mk_item("Show Toolbar", NULL));
    menu_node_add_child(view, mk_item("Show Sidebar", NULL));
    return view;
}

static MenuNode *build_go_menu(void)
{
    MenuNode *go = menu_node_new_item("Go");
    menu_node_add_child(go, mk_item("Back",             NULL));
    menu_node_add_child(go, mk_item("Forward",          NULL));
    menu_node_add_child(go, mk_item("Enclosing Folder", NULL));
    menu_node_add_child(go, menu_node_new_separator());
    menu_node_add_child(go, mk_item("Computer",     NULL));
    menu_node_add_child(go, mk_item("Home",         NULL));
    menu_node_add_child(go, mk_item("Desktop",      NULL));
    menu_node_add_child(go, mk_item("Downloads",    NULL));
    menu_node_add_child(go, mk_item("Applications", NULL));
    return go;
}

static MenuNode *build_window_menu(void)
{
    MenuNode *win = menu_node_new_item("Window");
    menu_node_add_child(win, mk_item("Minimize",           GLYPH_CMD "M"));
    menu_node_add_child(win, mk_item("Zoom",               NULL));
    menu_node_add_child(win, menu_node_new_separator());
    menu_node_add_child(win, mk_item("Bring All to Front", NULL));
    return win;
}

static MenuNode *build_help_menu(void)
{
    MenuNode *help = menu_node_new_item("Help");
    menu_node_add_child(help, mk_item("Search", NULL));
    menu_node_add_child(help, menu_node_new_separator());
    menu_node_add_child(help, mk_item("CopyCatOS Help", NULL));
    return help;
}

static MenuNode *build_shell_menu(void)
{
    MenuNode *shell = menu_node_new_item("Shell");
    menu_node_add_child(shell, mk_item("New Window",  GLYPH_CMD "N"));
    menu_node_add_child(shell, mk_item("New Tab",     GLYPH_CMD "T"));
    menu_node_add_child(shell, menu_node_new_separator());
    menu_node_add_child(shell, mk_item("Close Window", GLYPH_CMD "W"));
    menu_node_add_child(shell, mk_item("Close Tab",    GLYPH_OPT GLYPH_CMD "W"));
    return shell;
}

static MenuNode *build_history_menu(void)
{
    MenuNode *h = menu_node_new_item("History");
    menu_node_add_child(h, mk_item("Show All History", NULL));
    menu_node_add_child(h, menu_node_new_separator());
    menu_node_add_child(h, mk_item("Recently Closed", NULL));
    return h;
}

static MenuNode *build_bookmarks_menu(void)
{
    MenuNode *b = menu_node_new_item("Bookmarks");
    menu_node_add_child(b, mk_item("Show All Bookmarks",    NULL));
    menu_node_add_child(b, mk_item("Bookmark This Page...", NULL));
    menu_node_add_child(b, menu_node_new_separator());
    menu_node_add_child(b, mk_item("Bookmarks Bar", NULL));
    return b;
}

// Assemble a root whose children are a given sequence of top-level
// menus. The root itself carries no label and no action — it's the
// synthetic parent whose children are the bar's menu titles.
static MenuNode *build_root(MenuNode **menus, int n)
{
    MenuNode *root = menu_node_new_item(NULL);
    for (int i = 0; i < n; i++) menu_node_add_child(root, menus[i]);
    return root;
}

static void build_all_builtin_trees(void)
{
    // Finder
    {
        MenuNode *menus[] = {
            build_finder_file_menu(),
            build_edit_menu(),
            build_finder_view_menu(),
            build_go_menu(),
            build_window_menu(),
            build_help_menu(),
        };
        builtin_finder_root = build_root(menus, 6);
    }

    // Terminal
    {
        MenuNode *menus[] = {
            build_shell_menu(),
            build_edit_menu(),
            build_generic_view_menu(),
            build_window_menu(),
            build_help_menu(),
        };
        builtin_terminal_root = build_root(menus, 5);
    }

    // Browser
    {
        MenuNode *menus[] = {
            build_generic_file_menu(),
            build_edit_menu(),
            build_generic_view_menu(),
            build_history_menu(),
            build_bookmarks_menu(),
            build_window_menu(),
            build_help_menu(),
        };
        builtin_browser_root = build_root(menus, 7);
    }

    // System Preferences
    {
        MenuNode *menus[] = {
            build_generic_file_menu(),
            build_edit_menu(),
            build_generic_view_menu(),
            build_window_menu(),
            build_help_menu(),
        };
        builtin_sysprefs_root = build_root(menus, 5);
    }

    // Default (unknown apps)
    {
        MenuNode *menus[] = {
            build_generic_file_menu(),
            build_edit_menu(),
            build_generic_view_menu(),
            build_window_menu(),
            build_help_menu(),
        };
        builtin_default_root = build_root(menus, 5);
    }
}

static const MenuNode *builtin_root_for_class(const char *app_class)
{
    if (!app_class) return builtin_default_root;

    if (strcasecmp(app_class, "dolphin") == 0 ||
        strcasecmp(app_class, "desktop") == 0) {
        return builtin_finder_root;
    }
    if (strcasecmp(app_class, "konsole") == 0) {
        return builtin_terminal_root;
    }
    if (strcasecmp(app_class, "brave-browser") == 0 ||
        strcasecmp(app_class, "firefox") == 0) {
        return builtin_browser_root;
    }
    if (strcasecmp(app_class, "systemcontrol") == 0 ||
        strcasecmp(app_class, "systemsettings") == 0) {
        return builtin_sysprefs_root;
    }
    return builtin_default_root;
}

// ── Legacy lifecycle ────────────────────────────────────────────────
// When the active window is registered on the AppMenu.Registrar bus,
// spin up a DbusMenuClient pointed at that endpoint. Its imported
// MenuNode tree replaces the built-in fallback for the active app.

// Last tree pointer we handed out. Used in the change callback to
// detect a full layout refetch (pointer differs) vs an in-place prop
// patch (pointer unchanged) — only the former invalidates open
// dropdown MenuNode pointers, so only the former forces a dismiss.
static const MenuNode *last_seen_legacy_root;

static void dismiss_all_dropdowns(MenuBar *mb);

static void on_legacy_client_changed(DbusMenuClient *client, void *user_data)
{
    MenuBar *mb = user_data;
    mb->legacy_is_loading = false;

    const MenuNode *root = dbusmenu_client_root(client);
    bool tree_replaced = (root != last_seen_legacy_root);
    last_seen_legacy_root = root;

    if (tree_replaced) {
        // A LayoutUpdated fully replaced the tree. Any open dropdown
        // is now holding pointers into freed MenuNodes. Dismiss the
        // whole stack; user can reopen against the fresh tree.
        dismiss_all_dropdowns(mb);
    }
    menubar_paint(mb);
}

static void update_legacy_client(MenuBar *mb, Window active)
{
    const char *service = NULL;
    const char *path    = NULL;
    bool has_registration =
        (active != None) &&
        appmenu_bridge_lookup((uint32_t)active, &service, &path);

    // Same window as last time — nothing to rebuild. (Bridge lookup
    // returning the same (service, path) is implied — the registry is
    // keyed by wid, so wid equality is sufficient.)
    if (has_registration && active == mb->legacy_wid && mb->legacy_client) {
        return;
    }

    // Tear down whatever we had.
    if (mb->legacy_client) {
        dbusmenu_client_free(mb->legacy_client);
        mb->legacy_client       = NULL;
        mb->legacy_wid          = None;
        mb->legacy_is_loading   = false;
        last_seen_legacy_root   = NULL;
        dismiss_all_dropdowns(mb);
    }

    if (has_registration) {
        mb->legacy_client = dbusmenu_client_new(
            service, path, on_legacy_client_changed, mb);
        mb->legacy_wid          = active;
        mb->legacy_is_loading   = (mb->legacy_client != NULL);
        last_seen_legacy_root   = NULL;
    }
}

// ── Menu source resolution ──────────────────────────────────────────

const MenuNode *appmenu_root_for(MenuBar *mb)
{
    if (mb->legacy_client) {
        const MenuNode *root = dbusmenu_client_root(mb->legacy_client);
        // First-paint gap: a DbusMenuClient was spun up but GetLayout
        // hasn't replied yet. Show the app name with no menu titles —
        // rather than flashing a stale built-in set that's about to be
        // replaced.
        if (!root || root->n_children == 0) return NULL;
        return root;
    }
    return builtin_root_for_class(mb->active_class);
}

// ── Accelerator glyph conversion ────────────────────────────────────
// Legacy shortcut strings arrive as "Ctrl+S" / "Shift+Ctrl+Z" / etc.
// Map modifiers to their Aqua glyphs, dropping anything whose chord
// includes a Super / Meta key (doesn't exist in the Mac conceptual
// model; better blank than wrong). Ctrl gets mapped to ⌘ because
// Linux keybindings' Ctrl is the conceptual equivalent of Mac Cmd —
// displaying it as ⌃ would mislead the user.
//
// Built-in shortcut strings are already stored as glyph literals;
// they skip this path.

static bool shortcut_is_preformatted(const char *s)
{
    // Built-in shortcuts start with a UTF-8 glyph byte (high bit set).
    // DBusMenu shortcuts are plain ASCII ("Ctrl", "S", etc.).
    return s && ((unsigned char)s[0] & 0x80);
}

static const char *shortcut_to_aqua(const char *raw, char *buf, size_t buflen)
{
    if (!raw || !*raw) return NULL;
    if (shortcut_is_preformatted(raw)) return raw;

    // Tokenize on '+'. Collect modifier glyphs in SL display order
    // (⌃⌥⇧⌘), then append the key token.
    char scratch[128];
    snprintf(scratch, sizeof(scratch), "%s", raw);

    bool has_cmd = false, has_shift = false, has_opt = false;
    char key[64] = "";

    char *save = NULL;
    for (char *tok = strtok_r(scratch, "+", &save);
         tok;
         tok = strtok_r(NULL, "+", &save)) {
        if (strcasecmp(tok, "Control") == 0 || strcasecmp(tok, "Ctrl") == 0 ||
            strcasecmp(tok, "Primary") == 0) {
            // Control → ⌘ (Mac-Linux convention). "Primary" is GTK's
            // platform-neutral alias — dbusmenu-gtk3 forwards it
            // verbatim on some distros.
            has_cmd = true;
        } else if (strcasecmp(tok, "Shift") == 0) {
            has_shift = true;
        } else if (strcasecmp(tok, "Alt") == 0 || strcasecmp(tok, "Option") == 0) {
            has_opt = true;
        } else if (strcasecmp(tok, "Super") == 0 ||
                   strcasecmp(tok, "Meta")  == 0 ||
                   strcasecmp(tok, "Hyper") == 0) {
            // Super/Meta/Hyper have no Aqua glyph and confuse the user
            // more than they help. Drop the whole shortcut label.
            return NULL;
        } else {
            // Whatever's left is the key. Uppercase single letters;
            // leave "Return", "Tab", etc. as-is (the dbusmenu spec
            // names them mostly by XKB keysym).
            if (strlen(tok) == 1) {
                snprintf(key, sizeof(key), "%c", toupper((unsigned char)tok[0]));
            } else {
                snprintf(key, sizeof(key), "%s", tok);
            }
        }
    }

    size_t pos = 0;
    if (has_opt)   pos += (size_t)snprintf(buf + pos, buflen - pos, "%s", GLYPH_OPT);
    if (has_shift) pos += (size_t)snprintf(buf + pos, buflen - pos, "%s", GLYPH_SHIFT);
    if (has_cmd)   pos += (size_t)snprintf(buf + pos, buflen - pos, "%s", GLYPH_CMD);
    snprintf(buf + pos, buflen - pos, "%s", key);
    return buf[0] ? buf : NULL;
}

// ── Dropdown stack ──────────────────────────────────────────────────
// A dropdown is a popup window showing the children of some parent
// MenuNode. The stack lets submenus drill in — Qt AppMenu builds
// File → Recent → <file list>, which is two levels deep, so MAX=4
// is comfortable headroom.

#define DROPDOWN_MAX_DEPTH 4

typedef struct {
    Window           win;
    const MenuNode  *parent;          // parent->children[0..n_children-1] are items
    int              hover;            // hovered item index, -1 = none
    int              spawned_child_at; // item whose submenu is open, -1 if none
    int              w, h;             // window geometry (physical pixels)
    int              x, y;             // screen position
} DropdownLevel;

static DropdownLevel stack[DROPDOWN_MAX_DEPTH];
static int           depth;             // number of levels currently open

// Event mask for every dropdown window.
static const long dropdown_events = ExposureMask | ButtonPressMask
                                  | PointerMotionMask | LeaveWindowMask
                                  | KeyPressMask;

// Row heights (points). Separators are shorter than item rows.
#define ROW_H_ITEM 22
#define ROW_H_SEP   7

static int row_height_of(const MenuNode *n)
{
    return (n && n->type == MENU_ITEM_SEPARATOR) ? S(ROW_H_SEP) : S(ROW_H_ITEM);
}

// Y offset within a dropdown for item `idx` (0-based), starting from
// the popup's top edge. Accounts for top padding and mixed row heights.
static int row_y_offset(const MenuNode *parent, int idx)
{
    int y = S(4); // top padding
    for (int i = 0; i < idx && i < parent->n_children; i++) {
        y += row_height_of(parent->children[i]);
    }
    return y;
}

// Convert a popup-local y coordinate to the item index it falls on.
// Returns -1 on padding or separator (separators aren't hoverable).
static int hit_test_row(const MenuNode *parent, int local_y)
{
    int y = S(4);
    for (int i = 0; i < parent->n_children; i++) {
        const MenuNode *n = parent->children[i];
        int rh = row_height_of(n);
        if (local_y >= y && local_y < y + rh) {
            return (n->type == MENU_ITEM_SEPARATOR) ? -1 : i;
        }
        y += rh;
    }
    return -1;
}

// Helper: rounded rectangle path (local copy so we don't depend on render.c's).
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

// Measure a popup's width. Accounts for labels, the optional shortcut
// glyph column, the submenu arrow, and scaled paddings. Minimum floor
// keeps tiny submenus from looking cramped.
static int measure_popup_width(const MenuNode *parent)
{
    int popup_w = S(200); // floor
    char scbuf[128];

    for (int i = 0; i < parent->n_children; i++) {
        const MenuNode *n = parent->children[i];
        if (n->type == MENU_ITEM_SEPARATOR) continue;
        if (!n->label) continue;

        double w = render_measure_text(n->label, false);
        int extra = 0;

        if (n->shortcut) {
            const char *aqua = shortcut_to_aqua(n->shortcut, scbuf, sizeof(scbuf));
            if (aqua) {
                extra += (int)render_measure_text(aqua, false) + S(30);
            }
        }
        if (n->is_submenu) {
            // Submenu arrow column on the right.
            extra += S(18);
        }

        int needed = (int)w + S(40) + extra;
        if (needed > popup_w) popup_w = needed;
    }
    return popup_w;
}

static int measure_popup_height(const MenuNode *parent)
{
    int popup_h = S(8); // S(4) top + S(4) bottom padding
    for (int i = 0; i < parent->n_children; i++) {
        popup_h += row_height_of(parent->children[i]);
    }
    return popup_h;
}

static void paint_level(MenuBar *mb, int level)
{
    if (level < 0 || level >= depth) return;
    DropdownLevel *L = &stack[level];
    if (!L->parent) return;

    cairo_surface_t *surface = cairo_xlib_surface_create(
        mb->dpy, L->win,
        DefaultVisual(mb->dpy, mb->screen),
        L->w, L->h);
    cairo_t *cr = cairo_create(surface);

    // Background — Snow Leopard menu fill is 245/255 translucent white.
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 245.0 / 255.0);
    dropdown_rounded_rect(cr, 0, 0, L->w, L->h, SF(5.0));
    cairo_fill(cr);

    // Border
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 60.0 / 255.0);
    cairo_set_line_width(cr, 1.0);
    dropdown_rounded_rect(cr, 0.5, 0.5, L->w - 1, L->h - 1, SF(5.0));
    cairo_stroke(cr);

    char scbuf[128];
    int y = S(4);

    for (int i = 0; i < L->parent->n_children; i++) {
        const MenuNode *n = L->parent->children[i];

        if (n->type == MENU_ITEM_SEPARATOR) {
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 40.0 / 255.0);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, S(10), y + SF(3.5));
            cairo_line_to(cr, L->w - S(10), y + SF(3.5));
            cairo_stroke(cr);
            y += S(ROW_H_SEP);
            continue;
        }

        // A row is "selected" (blue) when the user is hovering it OR
        // when the user's child submenu is spawned from this item —
        // parent stays highlighted while the child is open.
        bool selected = (i == L->hover) || (i == L->spawned_child_at);
        bool enabled  = n->enabled;

        if (selected && enabled) {
            cairo_set_source_rgb(cr, 56.0/255.0, 117.0/255.0, 215.0/255.0);
            dropdown_rounded_rect(cr, S(4), y, L->w - S(8), S(ROW_H_ITEM), SF(3.0));
            cairo_fill(cr);
        }

        // Label (left)
        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, n->label ? n->label : "", -1);
        PangoFontDescription *desc = pango_font_description_from_string(
            appmenu_scaled_font("Lucida Grande", 13));
        pango_layout_set_font_description(layout, desc);
        pango_font_description_free(desc);

        // Text colour: white on selected-enabled, gray on disabled,
        // near-black on enabled-normal.
        if (selected && enabled) {
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        } else if (!enabled) {
            cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
        } else {
            cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        }
        cairo_move_to(cr, S(18), y + S(2));
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);

        // Toggle indicator (left margin). Checkmark when the item has
        // toggle-type=checkmark and state=1; radio dot when type=radio
        // and state=1. Drawn as a text glyph so it picks up the row's
        // text colour without a second colour switch.
        if (n->toggle != MENU_TOGGLE_NONE && n->toggle_state == 1) {
            const char *glyph = (n->toggle == MENU_TOGGLE_CHECKMARK)
                                    ? "\xe2\x9c\x93"   // ✓ U+2713
                                    : "\xe2\x80\xa2";  // • U+2022
            PangoLayout *tl = pango_cairo_create_layout(cr);
            pango_layout_set_text(tl, glyph, -1);
            PangoFontDescription *td = pango_font_description_from_string(
                appmenu_scaled_font("Lucida Grande", 13));
            pango_layout_set_font_description(tl, td);
            pango_font_description_free(td);
            cairo_move_to(cr, S(6), y + S(2));
            pango_cairo_show_layout(cr, tl);
            g_object_unref(tl);
        }

        // Submenu arrow (right-most), before the shortcut column.
        int right_margin = S(14);
        if (n->is_submenu) {
            PangoLayout *al = pango_cairo_create_layout(cr);
            pango_layout_set_text(al, "\xe2\x96\xb8", -1);  // ▸ U+25B8
            PangoFontDescription *ad = pango_font_description_from_string(
                appmenu_scaled_font("Lucida Grande", 11));
            pango_layout_set_font_description(al, ad);
            pango_font_description_free(ad);
            int arrow_w, arrow_h;
            pango_layout_get_pixel_size(al, &arrow_w, &arrow_h);
            cairo_move_to(cr, L->w - arrow_w - right_margin, y + S(3));
            pango_cairo_show_layout(cr, al);
            g_object_unref(al);
            right_margin += arrow_w + S(4);
        }

        // Shortcut glyphs (right-aligned, before submenu arrow).
        if (n->shortcut) {
            const char *aqua = shortcut_to_aqua(n->shortcut, scbuf, sizeof(scbuf));
            if (aqua) {
                PangoLayout *sc = pango_cairo_create_layout(cr);
                pango_layout_set_text(sc, aqua, -1);
                PangoFontDescription *sd = pango_font_description_from_string(
                    appmenu_scaled_font("Lucida Grande", 12));
                pango_layout_set_font_description(sc, sd);
                pango_font_description_free(sd);

                int sw, sh;
                pango_layout_get_pixel_size(sc, &sw, &sh);

                if (selected && enabled) {
                    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
                } else if (!enabled) {
                    cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
                } else {
                    cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
                }
                cairo_move_to(cr, L->w - sw - right_margin, y + S(2));
                pango_cairo_show_layout(cr, sc);
                g_object_unref(sc);
            }
        }

        y += S(ROW_H_ITEM);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    XFlush(mb->dpy);
}

// Destroy everything >= `level`, leaving depth = level. Used by
// dismiss, by Escape-from-submenu (level -> level-1 pop), and by
// level-N hovering a non-submenu item (drops any child at N+1).
static void dismiss_from(MenuBar *mb, int level)
{
    if (level < 0) level = 0;
    if (level >= depth) return;

    for (int i = depth - 1; i >= level; i--) {
        if (stack[i].win != None) {
            XDestroyWindow(mb->dpy, stack[i].win);
        }
        stack[i].win              = None;
        stack[i].parent           = NULL;
        stack[i].hover            = -1;
        stack[i].spawned_child_at = -1;
    }
    depth = level;
    if (depth > 0) {
        // Parent level no longer has a child spawned.
        stack[depth - 1].spawned_child_at = -1;
        paint_level(mb, depth - 1);
    }
    XFlush(mb->dpy);
}

static void dismiss_all_dropdowns(MenuBar *mb)
{
    dismiss_from(mb, 0);
}

// Create the popup window for a new level and push it onto the stack.
// Caller has filled in parent/x/y; we compute w/h from the MenuNode,
// clamp to the screen, and map the window.
static void push_level(MenuBar *mb, const MenuNode *parent, int x, int y)
{
    if (depth >= DROPDOWN_MAX_DEPTH || !parent) return;

    int popup_w = measure_popup_width(parent);
    int popup_h = measure_popup_height(parent);

    // Clamp: never let a popup disappear below the screen. Flip to
    // left of the spawning item if that keeps it on-screen.
    if (x + popup_w > mb->screen_w) {
        // Try flipping left relative to the parent level if there is one.
        if (depth > 0) {
            DropdownLevel *P = &stack[depth - 1];
            int flipped = P->x - popup_w + S(2);
            if (flipped >= 0) x = flipped;
            else              x = mb->screen_w - popup_w;
        } else {
            x = mb->screen_w - popup_w;
        }
        if (x < 0) x = 0;
    }
    if (y + popup_h > mb->screen_h) {
        y = mb->screen_h - popup_h;
        if (y < 0) y = 0;
    }

    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.event_mask        = dropdown_events;
    attrs.background_pixel  = WhitePixel(mb->dpy, mb->screen);

    Window w = XCreateWindow(
        mb->dpy, mb->root,
        x, y,
        (unsigned int)popup_w, (unsigned int)popup_h,
        0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWEventMask | CWBackPixel,
        &attrs);
    XMapRaised(mb->dpy, w);

    DropdownLevel *L = &stack[depth];
    L->win              = w;
    L->parent           = parent;
    L->hover            = -1;
    L->spawned_child_at = -1;
    L->w = popup_w; L->h = popup_h;
    L->x = x;       L->y = y;
    depth++;

    paint_level(mb, depth - 1);
}

// Spawn the submenu that belongs to level `parent_level`'s hovered
// item `item_idx`. Closes any pre-existing child first, then positions
// the new child to the right of the parent row with clip-aware left-
// flip if it would overflow.
static void spawn_submenu(MenuBar *mb, int parent_level, int item_idx)
{
    if (parent_level < 0 || parent_level >= depth) return;
    DropdownLevel *P = &stack[parent_level];
    if (item_idx < 0 || item_idx >= P->parent->n_children) return;
    const MenuNode *item = P->parent->children[item_idx];
    if (!item->is_submenu || item->n_children == 0) return;

    // Ask the server whether its cache needs a refresh before the
    // submenu opens. Qt AppMenu lazy-populates on this call; if we
    // skip it, large submenus (File → Recent) come up empty on first
    // open and populate on the next.
    if (mb->legacy_client && item->action_kind == MENU_ACTION_LEGACY) {
        dbusmenu_client_about_to_show(mb->legacy_client, item->legacy_id);
    }

    // Close any child level above parent_level.
    dismiss_from(mb, parent_level + 1);

    int row_y = P->y + row_y_offset(P->parent, item_idx);
    // S(2) horizontal overlap so the popup abuts the parent row cleanly.
    int row_x = P->x + P->w - S(2);

    P->spawned_child_at = item_idx;
    paint_level(mb, parent_level);  // parent repaints with this row highlighted

    push_level(mb, item, row_x, row_y);
}

// ── Dispatch ────────────────────────────────────────────────────────

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
        &nitems, &bytes_after, &data);

    if (status != Success || nitems == 0 || !data) {
        if (data) XFree(data);
        return None;
    }
    Window w = *(Window *)data;
    XFree(data);
    return w;
}

// Built-in items dispatch by label: the handful of items whose Snow
// Leopard semantics map onto a standard ICCCM/EWMH ClientMessage to
// the WM. Everything else is still informational until native apps
// wire into the MoonBase IPC.
static void dispatch_native_by_label(MenuBar *mb, const char *label)
{
    if (!label) return;

    Window active = get_active_window(mb);

    if (strcmp(label, "Minimize") == 0) {
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

    } else if (strcmp(label, "Close Window") == 0 ||
               strcmp(label, "Close Tab") == 0) {
        if (active == None) return;
        XEvent ev = {0};
        ev.type                 = ClientMessage;
        ev.xclient.window       = active;
        ev.xclient.message_type = mb->atom_net_close_window;
        ev.xclient.format       = 32;
        ev.xclient.data.l[0]    = 0;
        ev.xclient.data.l[1]    = 0;
        XSendEvent(mb->dpy, mb->root, False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        XFlush(mb->dpy);

    } else if (strcmp(label, "Zoom") == 0) {
        if (active == None) return;
        XEvent ev = {0};
        ev.type                 = ClientMessage;
        ev.xclient.window       = active;
        ev.xclient.message_type = mb->atom_net_active_window;
        ev.xclient.format       = 32;
        ev.xclient.data.l[0]    = 2;
        XSendEvent(mb->dpy, mb->root, False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        XFlush(mb->dpy);

    } else if (strcmp(label, "Bring All to Front") == 0) {
        if (active == None) return;
        XRaiseWindow(mb->dpy, active);
        XFlush(mb->dpy);
    }
    // Other labels (File items, Go, Help, etc.) are informational —
    // native menus are a stub until the MoonBase IPC layer is wired.
}

static void dispatch_menu_item(MenuBar *mb, const MenuNode *node)
{
    if (!node || node->type == MENU_ITEM_SEPARATOR) return;
    if (!node->enabled) return;

    // Two branches: legacy items fire dbusmenu Event; everything else
    // falls through to the built-in label-based dispatch.
    if (node->action_kind == MENU_ACTION_LEGACY && mb->legacy_client) {
        dbusmenu_client_activate(mb->legacy_client, node->legacy_id);
        return;
    }
    dispatch_native_by_label(mb, node->label);
}

// ── Active window tracking ──────────────────────────────────────────

void appmenu_update_active(MenuBar *mb)
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
        &nitems, &bytes_after, &data);

    if (status != Success || nitems == 0 || !data) {
        if (data) XFree(data);
        strncpy(mb->active_app, "Finder", sizeof(mb->active_app) - 1);
        strncpy(mb->active_class, "dolphin", sizeof(mb->active_class) - 1);
        update_legacy_client(mb, None);
        return;
    }

    Window active = *(Window *)data;
    XFree(data);

    if (active == None || active == 0) {
        strncpy(mb->active_app, "Finder", sizeof(mb->active_app) - 1);
        strncpy(mb->active_class, "dolphin", sizeof(mb->active_class) - 1);
        update_legacy_client(mb, None);
        return;
    }

    // Read WM_CLASS: two NUL-terminated strings, "instance\0class\0".
    data = NULL;
    status = XGetWindowProperty(
        mb->dpy, active,
        mb->atom_wm_class,
        0, 256, False, XA_STRING,
        &actual_type, &actual_format,
        &nitems, &bytes_after, &data);

    if (status != Success || nitems == 0 || !data) {
        if (data) XFree(data);
        strncpy(mb->active_app, "Finder", sizeof(mb->active_app) - 1);
        strncpy(mb->active_class, "dolphin", sizeof(mb->active_class) - 1);
        update_legacy_client(mb, active);
        return;
    }

    const char *instance = (const char *)data;
    const char *classname = instance;
    size_t len = strlen(instance);
    if (len + 1 < nitems) {
        classname = instance + len + 1;
    }

    char lower_class[128];
    strncpy(lower_class, classname, sizeof(lower_class) - 1);
    lower_class[sizeof(lower_class) - 1] = '\0';
    for (int i = 0; lower_class[i]; i++) {
        lower_class[i] = (char)tolower((unsigned char)lower_class[i]);
    }
    strncpy(mb->active_class, lower_class, sizeof(mb->active_class) - 1);

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
        strncpy(mb->active_app, lower_class, sizeof(mb->active_app) - 1);
        if (mb->active_app[0]) {
            mb->active_app[0] = (char)toupper((unsigned char)mb->active_app[0]);
        }
    }

    XFree(data);

    // Reconcile the DbusMenuClient with the new active window. This is
    // the seam between 18-A's registrar and 18-B's client — we cross
    // it once per focus change.
    update_legacy_client(mb, active);
}

// ── Public API: lifecycle / access ─────────────────────────────────

void appmenu_init(MenuBar *mb)
{
    (void)mb;
    build_all_builtin_trees();
    for (int i = 0; i < DROPDOWN_MAX_DEPTH; i++) {
        stack[i].win              = None;
        stack[i].parent           = NULL;
        stack[i].hover            = -1;
        stack[i].spawned_child_at = -1;
    }
    depth = 0;
    last_seen_legacy_root = NULL;
}

Window appmenu_get_dropdown_win(void)
{
    return (depth > 0) ? stack[depth - 1].win : None;
}

bool appmenu_pop_submenu_level(MenuBar *mb)
{
    if (depth < 2) return false;
    dismiss_from(mb, depth - 1);
    return true;
}

bool appmenu_find_dropdown_at(MenuBar *mb, int mx, int my,
                              Window *out_win, int *out_lx, int *out_ly)
{
    (void)mb;
    // Innermost popup wins — search top of stack first.
    for (int i = depth - 1; i >= 0; i--) {
        DropdownLevel *L = &stack[i];
        if (mx >= L->x && mx < L->x + L->w &&
            my >= L->y && my < L->y + L->h) {
            if (out_win) *out_win = L->win;
            if (out_lx)  *out_lx  = mx - L->x;
            if (out_ly)  *out_ly  = my - L->y;
            return true;
        }
    }
    return false;
}

void appmenu_show_dropdown(MenuBar *mb, int menu_index, int x)
{
    dismiss_all_dropdowns(mb);

    const MenuNode *root = appmenu_root_for(mb);
    if (!root || menu_index < 0 || menu_index >= root->n_children) return;

    const MenuNode *menu = root->children[menu_index];
    if (!menu || menu->n_children == 0) return;

    // Ask the server for a pre-show refresh — same reason as
    // submenu spawn.
    if (mb->legacy_client && menu->action_kind == MENU_ACTION_LEGACY) {
        dbusmenu_client_about_to_show(mb->legacy_client, menu->legacy_id);
    }

    push_level(mb, menu, x, MENUBAR_HEIGHT);
}

void appmenu_dismiss(MenuBar *mb)
{
    dismiss_all_dropdowns(mb);
}

// ── Event handling ─────────────────────────────────────────────────

static int find_level_for_window(Window w)
{
    for (int i = 0; i < depth; i++) {
        if (stack[i].win == w) return i;
    }
    return -1;
}

bool appmenu_handle_dropdown_event(MenuBar *mb, XEvent *ev, bool *should_dismiss)
{
    *should_dismiss = false;
    if (depth == 0) return false;

    Window ev_win = None;
    switch (ev->type) {
        case Expose:       ev_win = ev->xexpose.window;   break;
        case ButtonPress:  ev_win = ev->xbutton.window;   break;
        case MotionNotify: ev_win = ev->xmotion.window;   break;
        case LeaveNotify:  ev_win = ev->xcrossing.window; break;
        case KeyPress:     ev_win = ev->xkey.window;      break;
        default: return false;
    }

    int level = find_level_for_window(ev_win);
    if (level < 0) return false;
    DropdownLevel *L = &stack[level];

    switch (ev->type) {
        case Expose:
            if (ev->xexpose.count == 0) paint_level(mb, level);
            return true;

        case MotionNotify: {
            int new_hover = hit_test_row(L->parent, ev->xmotion.y);
            if (new_hover != L->hover) {
                L->hover = new_hover;
                // Hover moved to a different row at this level: kill
                // any child popup from a different spawning row.
                if (L->spawned_child_at != -1 && L->spawned_child_at != new_hover) {
                    dismiss_from(mb, level + 1);
                }
                paint_level(mb, level);
            }
            // Hover landed on a submenu item that isn't currently
            // spawned — open it.
            if (new_hover >= 0) {
                const MenuNode *n = L->parent->children[new_hover];
                if (n->is_submenu && n->n_children > 0 &&
                    L->spawned_child_at != new_hover) {
                    spawn_submenu(mb, level, new_hover);
                }
            }
            return true;
        }

        case LeaveNotify:
            // Don't clear hover when leaving in favour of a child —
            // keep the parent row highlighted for continuity. We only
            // clear when leaving the whole dropdown (handled when the
            // menubar loop routes motion that's outside every popup).
            return true;

        case ButtonPress: {
            int idx = L->hover;
            if (idx < 0 || idx >= L->parent->n_children) {
                // Click on padding / separator → dismiss all.
                *should_dismiss = true;
                return true;
            }
            const MenuNode *n = L->parent->children[idx];
            if (n->type == MENU_ITEM_SEPARATOR) {
                *should_dismiss = true;
                return true;
            }
            if (n->is_submenu && n->n_children > 0) {
                // Click on submenu parent: ensure submenu is open and
                // stay in the tree. (Hover already opened it.)
                if (L->spawned_child_at != idx) spawn_submenu(mb, level, idx);
                return true;
            }
            // Leaf: dispatch and dismiss the whole stack.
            dispatch_menu_item(mb, n);
            *should_dismiss = true;
            return true;
        }

        case KeyPress: {
            KeySym sym = XLookupKeysym(&ev->xkey, 0);
            if (sym == XK_Escape) {
                if (depth > 1) {
                    // Pop one submenu level — don't dismiss the whole
                    // stack. Only the top-level Escape tears everything
                    // down, and the menubar loop handles that.
                    dismiss_from(mb, depth - 1);
                    return true;
                }
                *should_dismiss = true;
                return true;
            }
            return true;
        }

        default:
            return false;
    }
}

// ── Shutdown ───────────────────────────────────────────────────────

void appmenu_cleanup(MenuBar *mb)
{
    dismiss_all_dropdowns(mb);

    if (mb && mb->legacy_client) {
        dbusmenu_client_free(mb->legacy_client);
        mb->legacy_client       = NULL;
        mb->legacy_wid          = None;
        mb->legacy_is_loading   = false;
    }

    menu_node_free(builtin_default_root);   builtin_default_root   = NULL;
    menu_node_free(builtin_finder_root);    builtin_finder_root    = NULL;
    menu_node_free(builtin_terminal_root);  builtin_terminal_root  = NULL;
    menu_node_free(builtin_browser_root);   builtin_browser_root   = NULL;
    menu_node_free(builtin_sysprefs_root);  builtin_sysprefs_root  = NULL;
    last_seen_legacy_root = NULL;
}
