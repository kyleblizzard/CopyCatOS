// CopyCatOS — by Kyle Blizzard at Blizzard.show

// apple.c — Apple logo button and Apple menu dropdown
//
// The Apple logo is the leftmost element of the menu bar. It's loaded
// from a PNG file and scaled to 22x15 pixels (measured from real Snow Leopard). Clicking it opens the
// Apple menu — a dropdown with system-level actions like Sleep, Restart,
// Shut Down, and Log Out.
//
// If the PNG files aren't found, we fall back to drawing a small filled
// circle as a placeholder. The actual Apple logo PNGs need to be placed
// at $HOME/.local/share/aqua-widgets/menubar/apple_logo.png (and the
// _selected variant).

#define _GNU_SOURCE  // For M_PI in math.h under strict C11

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  // strcasecmp
#include <math.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>  // XClassHint

#include "apple.h"
#include "render.h"

// ── Font scaling helper (mirrors the one in render.c) ──────────────
static char *apple_scaled_font(const char *base_name, int base_size)
{
    static char buf[128];
    int scaled_size = (int)(base_size * menubar_scale + 0.5);
    if (scaled_size < base_size) scaled_size = base_size;
    snprintf(buf, sizeof(buf), "%s %d", base_name, scaled_size);
    return buf;
}

// ── Module state ────────────────────────────────────────────────────

// The Apple logo surfaces, scaled to 22x15 pixels (measured from real Snow Leopard).
// NULL if the PNG files couldn't be loaded.
static cairo_surface_t *logo_normal   = NULL;
static cairo_surface_t *logo_selected = NULL;

// The Apple menu dropdown popup window. None if not open.
static Window apple_popup = None;

// Currently hovered item index in the dropdown (-1 = none).
// While the Open Applications submenu is open and the pointer is inside
// it, this stays pinned to the parent row so the parent stays highlighted
// — same behavior as Snow Leopard's hierarchical menus.
static int apple_hover = -1;

// ── Open Applications submenu (cascading popup) ────────────────────
//
// The "Open Applications" row in the main popup carries a ▶ arrow.
// When the pointer hovers it, this secondary popup opens to the right
// and lists every running app, one per row. Click an entry → activate
// that window via _NET_ACTIVE_WINDOW. Same auto-open / auto-close UX
// as Recent Items in real Snow Leopard.

static Window apple_submenu = None;
static int    apple_submenu_hover = -1;
static int    apple_submenu_w = 0;
static int    apple_submenu_h = 0;

// ── Apple menu item model ──────────────────────────────────────────
//
// The menu is a single flat list of AppleItem entries. Each rebuild
// (on apple_show_menu) prepends a single "Open Applications ▶" item
// (whose submenu lists every running app), then appends the static
// system actions (System Preferences, Sleep, Log Out, etc.). The
// running apps live in a second parallel array drawn into the submenu
// popup. Rendering, hit-testing, and dispatch walk these two arrays
// — no special-case branching beyond submenu open/close.

typedef struct {
    char    text[128];          // label drawn in the menu
    char    shortcut[16];       // empty string = no shortcut shown
    Window  target;             // window to raise on click (running apps)
    bool    is_separator;       // draw a thin horizontal line, not a row
    bool    is_header;          // disabled label styled as a section title
    bool    is_running_app;     // click sends _NET_ACTIVE_WINDOW
    bool    disabled;           // greyed out, no hover, no dispatch
    bool    has_submenu_arrow;  // ▶ glyph (Dock / Recent Items)
} AppleItem;

#define APPLE_MAX_ITEMS 48
static AppleItem apple_items[APPLE_MAX_ITEMS];
static int       apple_item_count = 0;

// Open Applications submenu — parallel array drawn into apple_submenu.
// apple_submenu_parent_idx is the index of the "Open Applications" row
// in apple_items[], or -1 if no apps are running (in which case the
// parent row is omitted from the main popup entirely).
static AppleItem apple_submenu_items[APPLE_MAX_ITEMS];
static int       apple_submenu_count = 0;
static int       apple_submenu_parent_idx = -1;

// Dynamic "Log Out <user>..." text — populated in apple_init from $USER
// and patched into the static menu definition by rebuild_apple_items.
static char logout_label[128] = "Log Out...";

// ── Static menu definition ─────────────────────────────────────────
// The fixed portion of the menu, appended after the running-apps section.
// A NULL `text` is the placeholder for the dynamic logout label so the
// real username can be substituted at rebuild time.

typedef struct {
    const char *text;
    const char *shortcut;       // NULL = no shortcut
    bool        disabled;
    bool        has_submenu_arrow;
} AppleStaticItem;

static const AppleStaticItem apple_static_items[] = {
    { "About CopyCatOS",        NULL,      false, false },
    { "---",                    NULL,      false, false },
    { "Software Update...",     NULL,      true,  false },
    { "---",                    NULL,      false, false },
    { "System Preferences...",  NULL,      false, false },
    { "Controller Settings...", NULL,      false, false },
    { "Dock",                   NULL,      true,  true  },
    { "Recent Items",           NULL,      true,  true  },
    { "---",                    NULL,      false, false },
    { "Force Quit...",          "⌥⌘Esc",   false, false },
    { "---",                    NULL,      false, false },
    { "Sleep",                  NULL,      false, false },
    { "Restart...",             NULL,      false, false },
    { "Shut Down...",           NULL,      false, false },
    { "---",                    NULL,      false, false },
    { NULL,                     "⇧⌘Q",     false, false },  // logout placeholder
};
#define APPLE_STATIC_COUNT (sizeof(apple_static_items)/sizeof(apple_static_items[0]))

// ── Internal: load a PNG, scale it, and extract an alpha mask ───────

// Loads a PNG file, scales it to the given width/height, and extracts
// just the alpha (opacity) channel as a grayscale mask surface.
//
// Why a mask? The Apple logo PNG is a colorful image (blue gradient),
// but macOS Snow Leopard renders the menu bar Apple logo as a solid
// dark silhouette. We use the logo's shape (alpha channel) as a stencil
// and paint it in solid black/white via cairo_mask_surface() in the
// paint function. This matches real Snow Leopard behavior.
//
// Returns an A8 (alpha-only) surface, or NULL if the file can't be loaded.
static cairo_surface_t *load_and_scale_png(const char *path, int target_w, int target_h)
{
    // Load the original PNG
    cairo_surface_t *original = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(original) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(original);
        return NULL;
    }

    // Get the original dimensions so we can compute the scale factor
    int orig_w = cairo_image_surface_get_width(original);
    int orig_h = cairo_image_surface_get_height(original);

    // Step 1: Scale the original PNG to the target size (keeps color+alpha)
    cairo_surface_t *scaled = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, target_w, target_h
    );
    cairo_t *cr = cairo_create(scaled);

    // Scale the drawing context so the original image maps to the target size
    double sx = (double)target_w / (double)orig_w;
    double sy = (double)target_h / (double)orig_h;
    cairo_scale(cr, sx, sy);

    // Use BEST filter for smooth downscaling (76x88 -> 22x15 is aggressive)
    cairo_pattern_t *pattern;
    cairo_set_source_surface(cr, original, 0, 0);
    pattern = cairo_get_source(cr);
    cairo_pattern_set_filter(pattern, CAIRO_FILTER_BEST);
    cairo_paint(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(original);

    // Step 2: Extract the alpha channel into an A8 mask surface.
    // A8 format stores only opacity — one byte per pixel, no color info.
    // We walk the scaled ARGB32 pixels and copy each pixel's alpha byte
    // into the A8 surface. This gives us the logo's silhouette shape.
    cairo_surface_flush(scaled);
    unsigned char *argb_data = cairo_image_surface_get_data(scaled);
    int argb_stride = cairo_image_surface_get_stride(scaled);

    cairo_surface_t *mask = cairo_image_surface_create(
        CAIRO_FORMAT_A8, target_w, target_h
    );
    cairo_surface_flush(mask);
    unsigned char *a8_data = cairo_image_surface_get_data(mask);
    int a8_stride = cairo_image_surface_get_stride(mask);

    for (int y = 0; y < target_h; y++) {
        for (int x = 0; x < target_w; x++) {
            // ARGB32 on little-endian: bytes are [B, G, R, A] per pixel
            unsigned char alpha = argb_data[y * argb_stride + x * 4 + 3];
            a8_data[y * a8_stride + x] = alpha;
        }
    }

    // Tell Cairo we modified the pixel data directly
    cairo_surface_mark_dirty(mask);
    cairo_surface_destroy(scaled);

    fprintf(stderr, "[apple] Loaded logo mask from %s (%dx%d -> %dx%d)\n",
            path, orig_w, orig_h, target_w, target_h);

    return mask;
}

// ── Public API ──────────────────────────────────────────────────────

void apple_init(MenuBar *mb)
{
    (void)mb;

    // Build the "Log Out <username>..." label from the actual system user.
    // Real Snow Leopard shows the short username (e.g., "Log Out Kyle...").
    // The dynamic label is plumbed in via rebuild_apple_items() — the
    // static_items table carries a NULL placeholder that gets substituted.
    const char *user = getenv("USER");
    if (!user) user = "User";
    snprintf(logout_label, sizeof(logout_label), "Log Out %s...", user);

    // Build the paths to the Apple logo PNGs.
    // We use $HOME to avoid hardcoding a username.
    const char *home = getenv("HOME");
    if (!home) return;

    char path_normal[512];
    char path_selected[512];
    snprintf(path_normal, sizeof(path_normal),
             "%s/.local/share/aqua-widgets/menubar/apple_logo.png", home);
    snprintf(path_selected, sizeof(path_selected),
             "%s/.local/share/aqua-widgets/menubar/apple_logo_selected.png", home);

    // Load and scale to proportionally-sized mask. Measured from real
    // Snow Leopard menu.png: Apple ink x=20..34 (w=15), y=1..16 (h~16) —
    // a portrait aspect ~0.82. Source HiResAppleMenu.png is 76×88, same
    // aspect within 2%. Target 15×18 keeps the logo upright; the earlier
    // 22×15 was a landscape aspect that squashed the logo at 1.75×.
    logo_normal   = load_and_scale_png(path_normal, S(15), S(18));
    logo_selected = load_and_scale_png(path_selected, S(15), S(18));

    if (!logo_normal) {
        fprintf(stderr, "menubar: Apple logo not found at %s (using fallback)\n",
                path_normal);
    }
}

void apple_reload(MenuBar *mb, MenuBarPane *pane)
{
    (void)mb;
    (void)pane;

    // Free old logo surfaces
    if (logo_normal)   { cairo_surface_destroy(logo_normal);   logo_normal = NULL; }
    if (logo_selected) { cairo_surface_destroy(logo_selected); logo_selected = NULL; }

    // Reload at the new scale
    const char *home = getenv("HOME");
    if (!home) return;

    char path_normal[512], path_selected[512];
    snprintf(path_normal, sizeof(path_normal),
             "%s/.local/share/aqua-widgets/menubar/apple_logo.png", home);
    snprintf(path_selected, sizeof(path_selected),
             "%s/.local/share/aqua-widgets/menubar/apple_logo_selected.png", home);

    logo_normal   = load_and_scale_png(path_normal, S(15), S(18));
    logo_selected = load_and_scale_png(path_selected, S(15), S(18));

    fprintf(stderr, "[apple] Reloaded logos at scale %.1f (%dx%d)\n",
            menubar_scale, S(15), S(18));
}

void apple_paint(MenuBar *mb, MenuBarPane *pane, cairo_t *cr)
{
    // Choose which logo variant to draw based on current state.
    // The "selected" variant shows when the pointer is hovering THIS
    // pane's Apple logo, or when THIS pane is the one hosting the open
    // Apple menu. Other panes stay on the normal variant even if the
    // menu is open somewhere — matches Snow Leopard's "only the active
    // output's bar shows the open state" feel.
    int pane_idx = (int)(pane - mb->panes);
    bool active =
        pane->hover_index == 0 ||
        (mb->active_pane == pane_idx && mb->open_menu == 0);
    cairo_surface_t *logo = (active && logo_selected) ? logo_selected : logo_normal;

    // Position: logo ink left-edge x=20, vertically centered in the bar.
    // Measured from real Snow Leopard menu.png: ink bbox x=20..34, y=1..16.
    double x = SF(20.0);
    double y = (MENUBAR_HEIGHT - S(18)) / 2.0;

    if (logo) {
        // Draw the logo as a solid-color silhouette using the alpha mask.
        // cairo_mask_surface() uses the mask's alpha values to control
        // where the current source color is painted. Where the mask is
        // opaque, the color shows through; where transparent, nothing
        // is drawn. This gives us a crisp black (or white) Apple logo
        // matching real Snow Leopard behavior.
        if (active) {
            // Selected state: white logo on the blue highlight
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        } else {
            // Normal state: pure black logo on the gray gradient bar
            // Use pure black for maximum contrast — real SL Apple logo
            // is a crisp, solid dark shape
            cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        }
        cairo_mask_surface(cr, logo, x, y);
    } else {
        // Fallback: draw a simple filled circle as a placeholder.
        // This is a dark gray circle roughly matching the Apple logo position.
        cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
        cairo_arc(cr, x + SF(11.0), MENUBAR_HEIGHT / 2.0, SF(7.0), 0, 2 * M_PI);
        cairo_fill(cr);
    }
}

// ── Running-app enumeration ─────────────────────────────────────────

// Read _NET_CLIENT_LIST from the root window. Caller must XFree(*out_list)
// when the returned count > 0. Duplicates the helper in appmenu.c so apple.c
// can stay self-contained — both are tiny and keeping them inline avoids a
// new shared header just for two utility functions.
static int apple_read_client_list(MenuBar *mb, Window **out_list)
{
    *out_list = NULL;
    Atom atom_client_list = XInternAtom(mb->dpy, "_NET_CLIENT_LIST", False);

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        mb->dpy, mb->root, atom_client_list,
        0, 1024, False, XA_WINDOW,
        &actual_type, &actual_format,
        &nitems, &bytes_after, &data);

    if (status != Success || nitems == 0 || !data) {
        if (data) XFree(data);
        return 0;
    }
    *out_list = (Window *)data;
    return (int)nitems;
}

// Read WM_CLASS for `w` and return a friendly app name in `out` (size `n`).
// We prefer res_class (e.g. "TextEdit", "Brave-browser") over res_name
// (e.g. the binary basename) — res_class is closer to a human-facing app
// name. Returns true on success.
static bool apple_get_class(MenuBar *mb, Window w, char *out, size_t n)
{
    if (!out || n == 0) return false;
    XClassHint ch = {0};
    if (!XGetClassHint(mb->dpy, w, &ch)) return false;

    const char *src = (ch.res_class && ch.res_class[0])
                    ? ch.res_class
                    : (ch.res_name && ch.res_name[0] ? ch.res_name : NULL);
    bool ok = false;
    if (src) {
        strncpy(out, src, n - 1);
        out[n - 1] = '\0';
        ok = true;
    }
    if (ch.res_class) XFree(ch.res_class);
    if (ch.res_name)  XFree(ch.res_name);
    return ok;
}

// Filter shell components out of the running-apps list. The menubar
// itself, dock, desktop wallpaper surface, searchsystem overlay, and the
// inputmap utility are infrastructure — they're always running and
// listing them in the "Open Applications" section would be noise.
static bool apple_is_shell_component(const char *cls)
{
    if (!cls) return true;
    static const char *shell_classes[] = {
        "menubar", "dock", "desktop", "searchsystem",
        "inputmap", "moonrock", "moonrock-lite",
        NULL,
    };
    for (int i = 0; shell_classes[i]; i++) {
        if (strcasecmp(cls, shell_classes[i]) == 0) return true;
    }
    return false;
}

// Read _NET_WM_WINDOW_TYPE for `w` and return true if the window is a
// type that should appear in the application list. Excludes DOCK,
// DESKTOP, MENU, TOOLTIP, NOTIFICATION, SPLASH, etc. — these are
// transient or shell chrome and shouldn't be picked as the activate
// target for an app entry. Missing _NET_WM_WINDOW_TYPE defaults to
// "normal app" (true) so apps that never set the property still list.
static bool apple_window_is_app(MenuBar *mb, Window w)
{
    Atom atom_type   = XInternAtom(mb->dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        mb->dpy, w, atom_type,
        0, 32, False, XA_ATOM,
        &actual_type, &actual_format,
        &nitems, &bytes_after, &data);

    if (status != Success || !data) {
        if (data) XFree(data);
        return true;  // unknown → treat as a normal app
    }

    static const char *exclude[] = {
        "_NET_WM_WINDOW_TYPE_DOCK",
        "_NET_WM_WINDOW_TYPE_DESKTOP",
        "_NET_WM_WINDOW_TYPE_MENU",
        "_NET_WM_WINDOW_TYPE_TOOLTIP",
        "_NET_WM_WINDOW_TYPE_NOTIFICATION",
        "_NET_WM_WINDOW_TYPE_SPLASH",
        "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU",
        "_NET_WM_WINDOW_TYPE_POPUP_MENU",
        "_NET_WM_WINDOW_TYPE_COMBO",
        NULL,
    };

    Atom *types = (Atom *)data;
    bool ok = true;
    for (unsigned long i = 0; i < nitems && ok; i++) {
        char *name = XGetAtomName(mb->dpy, types[i]);
        if (!name) continue;
        for (int e = 0; exclude[e]; e++) {
            if (strcmp(name, exclude[e]) == 0) { ok = false; break; }
        }
        XFree(name);
    }
    XFree(data);
    return ok;
}

// Capitalize the first character if it's a lowercase ASCII letter.
// "fileviewer" → "Fileviewer", "TextEdit" → "TextEdit". Quick visual
// polish so res_class lowercase names don't look out of place next to
// proper-case ones in the menu.
static void apple_capitalize_first(char *s)
{
    if (s && s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 'a' + 'A');
}

// ── Submenu dedupe helper ───────────────────────────────────────────
// Same job as apple_find_running_entry but for the submenu's parallel
// array. Keeps one row per WM_CLASS so a second window of the same app
// updates the existing entry's target instead of duplicating the row.
static int apple_find_running_entry_submenu(const char *cls)
{
    for (int i = 0; i < apple_submenu_count; i++) {
        if (strcasecmp(apple_submenu_items[i].text, cls) == 0) return i;
    }
    return -1;
}

// ── Menu rebuild ────────────────────────────────────────────────────
// Repopulate apple_items[] and apple_submenu_items[] from scratch.
// Called at the start of every apple_show_menu() so both reflect the
// current set of running apps.
//
// Main popup layout:
//   [Open Applications ▶] [---]          ← only if apps exist
//   [About CopyCatOS] ... [Log Out ...]  ← static items
//
// Submenu layout (only built when at least one app is found):
//   [App A] [App B] [App C] ...
//
// If no real running apps are found, the parent row + its separator
// are dropped entirely — the menu looks exactly like the original
// (About → ...) instead of carrying an empty cascade.

static void apple_rebuild(MenuBar *mb)
{
    apple_item_count = 0;
    apple_submenu_count = 0;
    apple_submenu_parent_idx = -1;

    // ── Collect running apps into the submenu's array ───────────
    Window *list = NULL;
    int n = apple_read_client_list(mb, &list);
    for (int i = 0; i < n; i++) {
        if (apple_submenu_count >= APPLE_MAX_ITEMS) break;

        char cls[128];
        if (!apple_get_class(mb, list[i], cls, sizeof(cls))) continue;
        if (apple_is_shell_component(cls)) continue;
        if (!apple_window_is_app(mb, list[i])) continue;

        // Dedupe by class — one row per app, target = most recently
        // seen window so a click activates the latest mapped instance.
        int existing = apple_find_running_entry_submenu(cls);
        if (existing >= 0) {
            apple_submenu_items[existing].target = list[i];
            continue;
        }

        AppleItem *it = &apple_submenu_items[apple_submenu_count++];
        memset(it, 0, sizeof(*it));
        strncpy(it->text, cls, sizeof(it->text) - 1);
        apple_capitalize_first(it->text);
        it->target         = list[i];
        it->is_running_app = true;
    }
    if (list) XFree(list);

    // ── Parent row in the main popup ────────────────────────────
    // Only emitted when at least one app populated the submenu. The
    // row carries the ▶ arrow and stays enabled so it can be hovered
    // (hovering opens the submenu — see apple_handle_motion).
    if (apple_submenu_count > 0) {
        apple_submenu_parent_idx = apple_item_count;
        AppleItem *parent = &apple_items[apple_item_count++];
        memset(parent, 0, sizeof(*parent));
        strncpy(parent->text, "Open Applications",
                sizeof(parent->text) - 1);
        parent->has_submenu_arrow = true;

        AppleItem *sep = &apple_items[apple_item_count++];
        memset(sep, 0, sizeof(*sep));
        sep->is_separator = true;
    }

    // ── Static fixed items ──────────────────────────────────────
    for (size_t i = 0; i < APPLE_STATIC_COUNT; i++) {
        if (apple_item_count >= APPLE_MAX_ITEMS) break;
        const AppleStaticItem *s = &apple_static_items[i];
        AppleItem *it = &apple_items[apple_item_count++];
        memset(it, 0, sizeof(*it));

        // NULL text is the logout placeholder — substitute the live
        // dynamic label built in apple_init() from $USER.
        const char *text = s->text ? s->text : logout_label;
        if (text && strcmp(text, "---") == 0) {
            it->is_separator = true;
            continue;
        }
        if (text) strncpy(it->text, text, sizeof(it->text) - 1);
        if (s->shortcut) strncpy(it->shortcut, s->shortcut,
                                 sizeof(it->shortcut) - 1);
        it->disabled          = s->disabled;
        it->has_submenu_arrow = s->has_submenu_arrow;
    }
}

// ── Dropdown helpers ────────────────────────────────────────────────

// Helper: draw a rounded rectangle path (for the dropdown background).
static void apple_rounded_rect(cairo_t *cr, double x, double y,
                               double w, double h, double radius)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - radius, y + radius,     radius, -M_PI / 2, 0);
    cairo_arc(cr, x + w - radius, y + h - radius, radius, 0,          M_PI / 2);
    cairo_arc(cr, x + radius,     y + h - radius, radius, M_PI / 2,   M_PI);
    cairo_arc(cr, x + radius,     y + radius,     radius, M_PI,       3 * M_PI / 2);
    cairo_close_path(cr);
}

// Compute the row height for an item — separators are short, every
// other row is full-height. Centralized so paint, hit-test, and popup
// height all agree.
static int apple_row_height(const AppleItem *it)
{
    return it->is_separator ? S(7) : S(22);
}

// Paint the Apple menu dropdown contents.
static void paint_apple_dropdown(MenuBar *mb, int popup_w, int popup_h)
{
    cairo_surface_t *surface = cairo_xlib_surface_create(
        mb->dpy, apple_popup,
        DefaultVisual(mb->dpy, mb->screen),
        popup_w, popup_h
    );
    cairo_t *cr = cairo_create(surface);

    // Background: slightly transparent white with scaled corner radius
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 245.0 / 255.0);
    apple_rounded_rect(cr, 0, 0, popup_w, popup_h, SF(5.0));
    cairo_fill(cr);

    // Border: subtle dark outline
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 60.0 / 255.0);
    cairo_set_line_width(cr, 1.0);
    apple_rounded_rect(cr, 0.5, 0.5, popup_w - 1, popup_h - 1, SF(5.0));
    cairo_stroke(cr);

    // Draw each menu item — all dimensions scale proportionally
    int y = S(4); // Top padding

    for (int i = 0; i < apple_item_count; i++) {
        AppleItem *it = &apple_items[i];

        if (it->is_separator) {
            // Separator line
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 40.0 / 255.0);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, S(10), y + SF(3.5));
            cairo_line_to(cr, popup_w - S(10), y + SF(3.5));
            cairo_stroke(cr);
            y += S(7);
            continue;
        }

        // Hover highlight — only on enabled, non-separator items.
        bool hovered = (i == apple_hover && !it->disabled);
        if (hovered) {
            // Blue selection highlight (rounded rect) — Snow Leopard #3875D7
            cairo_set_source_rgba(cr, 56/255.0, 117/255.0, 215/255.0, 0.9);
            double rx = S(4), ry = y, rw = popup_w - S(8), rh = S(22), rr = SF(3.0);
            cairo_new_sub_path(cr);
            cairo_arc(cr, rx + rw - rr, ry + rr, rr, -M_PI/2, 0);
            cairo_arc(cr, rx + rw - rr, ry + rh - rr, rr, 0, M_PI/2);
            cairo_arc(cr, rx + rr, ry + rh - rr, rr, M_PI/2, M_PI);
            cairo_arc(cr, rx + rr, ry + rr, rr, M_PI, 3*M_PI/2);
            cairo_close_path(cr);
            cairo_fill(cr);
        }

        // Text color: white on blue highlight, gray for disabled, dark otherwise
        if (hovered) {
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        } else if (it->disabled) {
            cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
        } else {
            cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        }

        // Headers render in a slightly smaller font to read as section
        // titles rather than disabled menu rows. Running-app rows get a
        // small left indent to visually separate them from the section
        // header above.
        int label_x = S(18);
        const char *font_base = "Lucida Grande";
        int font_size = 13;
        if (it->is_header) {
            font_size = 11;
        } else if (it->is_running_app) {
            label_x = S(24);
        }

        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, it->text, -1);
        PangoFontDescription *desc = pango_font_description_from_string(
            apple_scaled_font(font_base, font_size)
        );
        pango_layout_set_font_description(layout, desc);
        pango_font_description_free(desc);

        cairo_move_to(cr, label_x, y + S(2));
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);

        // Submenu indicator arrow (Dock / Recent Items)
        if (it->has_submenu_arrow) {
            double ax = popup_w - S(16);
            double ay = y + SF(10.0);
            cairo_move_to(cr, ax, ay - SF(4.0));
            cairo_line_to(cr, ax + SF(5.0), ay);
            cairo_line_to(cr, ax, ay + SF(4.0));
            cairo_close_path(cr);
            cairo_fill(cr);
        }

        // Right-aligned keyboard shortcut
        if (it->shortcut[0]) {
            PangoLayout *sc_layout = pango_cairo_create_layout(cr);
            pango_layout_set_text(sc_layout, it->shortcut, -1);
            PangoFontDescription *sc_desc = pango_font_description_from_string(
                apple_scaled_font("Lucida Grande", 12)
            );
            pango_layout_set_font_description(sc_layout, sc_desc);
            pango_font_description_free(sc_desc);

            int sc_w, sc_h;
            pango_layout_get_pixel_size(sc_layout, &sc_w, &sc_h);

            cairo_move_to(cr, popup_w - sc_w - S(12), y + S(2));
            pango_cairo_show_layout(cr, sc_layout);
            g_object_unref(sc_layout);
        }

        y += S(22);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    XFlush(mb->dpy);
}

// ── Submenu paint / open / close ───────────────────────────────────

// Paint the Open Applications submenu contents. Same visual language
// as paint_apple_dropdown (rounded white panel, blue hover highlight,
// Lucida Grande 13pt at scale) but without separators or shortcuts.
static void paint_apple_submenu(MenuBar *mb, int popup_w, int popup_h)
{
    cairo_surface_t *surface = cairo_xlib_surface_create(
        mb->dpy, apple_submenu,
        DefaultVisual(mb->dpy, mb->screen),
        popup_w, popup_h
    );
    cairo_t *cr = cairo_create(surface);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 245.0 / 255.0);
    apple_rounded_rect(cr, 0, 0, popup_w, popup_h, SF(5.0));
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 60.0 / 255.0);
    cairo_set_line_width(cr, 1.0);
    apple_rounded_rect(cr, 0.5, 0.5, popup_w - 1, popup_h - 1, SF(5.0));
    cairo_stroke(cr);

    int y = S(4);
    for (int i = 0; i < apple_submenu_count; i++) {
        AppleItem *it = &apple_submenu_items[i];

        bool hovered = (i == apple_submenu_hover && !it->disabled);
        if (hovered) {
            cairo_set_source_rgba(cr, 56/255.0, 117/255.0, 215/255.0, 0.9);
            double rx = S(4), ry = y, rw = popup_w - S(8), rh = S(22), rr = SF(3.0);
            cairo_new_sub_path(cr);
            cairo_arc(cr, rx + rw - rr, ry + rr, rr, -M_PI/2, 0);
            cairo_arc(cr, rx + rw - rr, ry + rh - rr, rr, 0, M_PI/2);
            cairo_arc(cr, rx + rr, ry + rh - rr, rr, M_PI/2, M_PI);
            cairo_arc(cr, rx + rr, ry + rr, rr, M_PI, 3*M_PI/2);
            cairo_close_path(cr);
            cairo_fill(cr);
        }

        if (hovered) {
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        } else {
            cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        }

        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, it->text, -1);
        PangoFontDescription *desc = pango_font_description_from_string(
            apple_scaled_font("Lucida Grande", 13)
        );
        pango_layout_set_font_description(layout, desc);
        pango_font_description_free(desc);

        cairo_move_to(cr, S(18), y + S(2));
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);

        y += S(22);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    XFlush(mb->dpy);
}

// Compute the Y offset (within the main popup) where item `idx` begins.
// Walks the apple_items array summing per-row heights — same accumulator
// as paint_apple_dropdown so submenu placement matches the visible row.
static int apple_row_top_y(int idx)
{
    int y = S(4);
    for (int i = 0; i < idx && i < apple_item_count; i++) {
        y += apple_row_height(&apple_items[i]);
    }
    return y;
}

// Open the Open Applications submenu next to the main popup. No-op if
// there's no parent row (no apps running) or if it's already open.
static void apple_open_submenu(MenuBar *mb)
{
    if (apple_submenu != None) return;
    if (apple_submenu_parent_idx < 0) return;
    if (apple_popup == None) return;
    if (apple_submenu_count <= 0) return;

    XWindowAttributes mwa;
    if (!XGetWindowAttributes(mb->dpy, apple_popup, &mwa)) return;

    apple_submenu_w = S(220);
    apple_submenu_h = S(8) + apple_submenu_count * S(22);

    int sub_x = mwa.x + mwa.width;
    int sub_y = mwa.y + apple_row_top_y(apple_submenu_parent_idx) - S(4);
    if (sub_y < 0) sub_y = 0;

    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.event_mask = ExposureMask | ButtonPressMask
                     | PointerMotionMask | LeaveWindowMask;
    attrs.background_pixel = WhitePixel(mb->dpy, mb->screen);

    apple_submenu = XCreateWindow(
        mb->dpy, mb->root,
        sub_x, sub_y,
        (unsigned int)apple_submenu_w,
        (unsigned int)apple_submenu_h,
        0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWEventMask | CWBackPixel,
        &attrs
    );

    apple_submenu_hover = -1;
    XMapRaised(mb->dpy, apple_submenu);
    paint_apple_submenu(mb, apple_submenu_w, apple_submenu_h);
}

// Close the submenu without dismissing the main Apple menu.
static void apple_close_submenu(MenuBar *mb)
{
    if (apple_submenu == None) return;
    XDestroyWindow(mb->dpy, apple_submenu);
    apple_submenu = None;
    apple_submenu_hover = -1;
    XFlush(mb->dpy);
}

void apple_show_menu(MenuBar *mb)
{
    // Dismiss any existing Apple menu first
    apple_dismiss(mb);

    // Rebuild the items array from current state — this captures the
    // current set of running apps for the "Open Applications" section.
    apple_rebuild(mb);

    // ── Calculate popup size (all dimensions scale proportionally) ──
    int popup_w = S(220); // Fixed width matching Snow Leopard Apple menu

    // Height: sum the per-item heights from the rebuilt list, plus padding
    int popup_h = S(8);
    for (int i = 0; i < apple_item_count; i++) {
        popup_h += apple_row_height(&apple_items[i]);
    }

    // ── Anchor in root-absolute coordinates ─────────────────────
    // Override-redirect popup windows live in the virtual-root space,
    // so we translate the pane's apple_x/logo origin into root coords
    // using the active pane's screen_{x,y}. In Modern mode the Apple
    // menu anchors under the Apple logo of whichever pane spawned it
    // (mb->active_pane). In Classic mode pane 0 is the only pane, so
    // active_pane == 0 and the math collapses to the historical case.
    MenuBarPane *host =
        (mb->active_pane >= 0 && mb->active_pane < mb->pane_count)
            ? &mb->panes[mb->active_pane]
            : mb_primary_pane(mb);
    int popup_x = host ? host->screen_x + host->apple_x : 0;
    int popup_y = host ? host->screen_y + MENUBAR_HEIGHT : MENUBAR_HEIGHT;

    // ── Create the popup window ─────────────────────────────────
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.event_mask = ExposureMask | ButtonPressMask
                     | PointerMotionMask | LeaveWindowMask;
    attrs.background_pixel = WhitePixel(mb->dpy, mb->screen);

    apple_popup = XCreateWindow(
        mb->dpy, mb->root,
        popup_x, popup_y,
        (unsigned int)popup_w,
        (unsigned int)popup_h,
        0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWEventMask | CWBackPixel,
        &attrs
    );

    XMapRaised(mb->dpy, apple_popup);

    // Paint the dropdown contents
    paint_apple_dropdown(mb, popup_w, popup_h);
}

void apple_dismiss(MenuBar *mb)
{
    // Close the submenu first so its X window goes away before the
    // parent — avoids a single-frame flash of the cascade with no parent.
    apple_close_submenu(mb);
    if (apple_popup != None) {
        XDestroyWindow(mb->dpy, apple_popup);
        apple_popup = None;
        apple_hover = -1;
        XFlush(mb->dpy);
    }
}

Window apple_get_popup(void)
{
    return apple_popup;
}

Window apple_get_submenu(void)
{
    return apple_submenu;
}

// ── Item hit testing ───────────────────────────────────────────────
// Convert a Y coordinate inside the popup to the menu item index.
// Returns -1 for separators, padding, or out-of-bounds.
static int apple_y_to_item(int y)
{
    int row_y = S(4); // Top padding (scaled)
    for (int i = 0; i < apple_item_count; i++) {
        AppleItem *it = &apple_items[i];
        int h = apple_row_height(it);
        if (it->is_separator) {
            row_y += h;
            continue;
        }
        if (y >= row_y && y < row_y + h) return i;
        row_y += h;
    }
    return -1;
}

// ── Action execution ───────────────────────────────────────────────
// Executes the system action for the clicked Apple menu item.
// This is where menu items become functional — each item maps to
// a real system command matching macOS Snow Leopard behavior.
static void apple_execute(MenuBar *mb, int index)
{
    if (index < 0 || index >= apple_item_count) return;
    AppleItem *it = &apple_items[index];
    if (it->disabled || it->is_separator) return;

    const char *label = it->text;
    fprintf(stderr, "[apple] Execute: %s\n", label);

    // ── Running-app activation ──────────────────────────────────
    // The Open Applications section dispatches by sending the standard
    // EWMH _NET_ACTIVE_WINDOW ClientMessage to the root for the entry's
    // target window. moonrock interprets this as "raise + focus" and
    // pulls the app to the front, identical to the dock click path.
    if (it->is_running_app) {
        Window target = it->target;
        if (target == None) return;
        XEvent ev = {0};
        ev.type                 = ClientMessage;
        ev.xclient.window       = target;
        ev.xclient.message_type = mb->atom_net_active_window;
        ev.xclient.format       = 32;
        ev.xclient.data.l[0]    = 2;  // source = pager/taskbar
        XSendEvent(mb->dpy, mb->root, False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        XFlush(mb->dpy);
        return;
    }

    if (strcmp(label, "About CopyCatOS") == 0) {
        // Placeholder About surface — pulls the live libmoonbase.so runtime
        // version via the `moonbase-version` CLI and fires it through
        // notify-send as an ephemeral toast. This is NOT the final UX.
        // Snow Leopard's "About This Mac" is a bespoke Aqua panel; the
        // real version lands once MoonRock exposes a shell-owned dialog
        // primitive we can draw the panel inside. Until then, a toast
        // with the correct version beats a disabled menu item.
        if (fork() == 0) {
            setsid();
            execlp("sh", "sh", "-c",
                "v=$(moonbase-version 2>/dev/null); "
                "notify-send -a 'CopyCatOS' 'About CopyCatOS' "
                "\"MoonBase runtime v${v:-unknown}\"",
                NULL);
            _exit(1);
        }
    } else if (strcmp(label, "System Preferences...") == 0) {
        // Launch the CopyCatOS System Preferences app
        if (fork() == 0) {
            setsid();
            execlp("systemcontrol", "systemcontrol", NULL);
            _exit(1);
        }
    } else if (strcmp(label, "Controller Settings...") == 0) {
        // Launch System Preferences directly to the Controller pane
        if (fork() == 0) {
            setsid();
            execlp("systemcontrol", "systemcontrol", "--pane", "controller", NULL);
            _exit(1);
        }
    } else if (strcmp(label, "Force Quit...") == 0) {
        // Show xkill or a force-quit dialog
        // For now, launch xkill which lets user click a window to kill it
        if (fork() == 0) {
            setsid();
            execlp("xkill", "xkill", NULL);
            _exit(1);
        }
    } else if (strcmp(label, "Sleep") == 0) {
        // Suspend the system (same as power button short press)
        system("systemctl suspend");
    } else if (strncmp(label, "Restart", 7) == 0) {
        // Reboot the system
        // TODO: Show confirmation dialog first
        system("systemctl reboot");
    } else if (strncmp(label, "Shut Down", 9) == 0) {
        // Power off the system
        // TODO: Show confirmation dialog first
        system("systemctl poweroff");
    } else if (strncmp(label, "Log Out", 7) == 0) {
        // Log out by killing the window manager, which triggers
        // moonrock-session.sh's cleanup (kills all shell components).
        // This is how Snow Leopard does it — the WM exit triggers
        // the loginwindow to reappear.
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        fprintf(stderr, "[logout-trace] apple Log Out clicked at %ld.%09ld\n",
                (long)ts.tv_sec, ts.tv_nsec);
        // Mirror to persistent trace file (xsession-errors is wiped on relogin)
        const char *xdg_state = getenv("XDG_STATE_HOME");
        const char *home = getenv("HOME");
        char trace_path[512];
        if (xdg_state && *xdg_state) {
            snprintf(trace_path, sizeof(trace_path),
                     "%s/copycatos/logout-trace.log", xdg_state);
        } else if (home) {
            snprintf(trace_path, sizeof(trace_path),
                     "%s/.local/state/copycatos/logout-trace.log", home);
        } else {
            trace_path[0] = '\0';
        }
        if (trace_path[0]) {
            FILE *tf = fopen(trace_path, "a");
            if (tf) {
                fprintf(tf, "[logout-trace] apple Log Out clicked at %ld.%09ld\n",
                        (long)ts.tv_sec, ts.tv_nsec);
                fclose(tf);
            }
        }
        pid_t wm_pid = 0;
        FILE *proc = popen("pgrep -x moonrock", "r");
        if (proc) {
            char buf[32];
            if (fgets(buf, sizeof(buf), proc)) {
                wm_pid = (pid_t)atoi(buf);
            }
            pclose(proc);
        }
        if (wm_pid > 0) {
            kill(wm_pid, SIGTERM);
        }
    }
}

// ── Event handling ─────────────────────────────────────────────────
// Handles hover highlighting and click-to-execute inside the Apple
// menu popup. Called from the menubar event loop when events are
// routed to the Apple popup window.

void apple_handle_motion(MenuBar *mb, int motion_y)
{
    int item = apple_y_to_item(motion_y);
    // Don't highlight disabled items or separators
    if (item >= 0 && apple_items[item].disabled) item = -1;

    if (item != apple_hover) {
        apple_hover = item;
        // Repaint the dropdown with updated highlight
        if (apple_popup != None) {
            XWindowAttributes wa;
            XGetWindowAttributes(mb->dpy, apple_popup, &wa);
            paint_apple_dropdown(mb, wa.width, wa.height);
        }
    }

    // Cascade rule: hovering the parent row auto-opens the submenu;
    // hovering any other row auto-closes it. This is the same UX as
    // Snow Leopard's Recent Items / Dock — no click needed to expand.
    // motion_y == -999 (pointer left both popups) also closes it via
    // the "item != parent_idx" branch.
    if (apple_submenu_parent_idx >= 0 && item == apple_submenu_parent_idx) {
        if (apple_submenu == None) apple_open_submenu(mb);
    } else if (apple_submenu != None) {
        apple_close_submenu(mb);
    }
}

void apple_handle_submenu_motion(MenuBar *mb, int motion_y)
{
    if (apple_submenu == None) return;

    // Map Y inside the submenu popup to a row index. Same row geometry
    // as paint_apple_submenu: top padding S(4), then S(22) per row.
    int item = -1;
    int row_y = S(4);
    for (int i = 0; i < apple_submenu_count; i++) {
        if (motion_y >= row_y && motion_y < row_y + S(22)) {
            item = i;
            break;
        }
        row_y += S(22);
    }

    bool changed = (item != apple_submenu_hover);
    apple_submenu_hover = item;

    // While the pointer is inside the submenu, pin the parent row
    // highlighted in the main popup (Snow Leopard pattern). If the
    // main hover was on a different row, repaint main too.
    bool main_changed = false;
    if (apple_hover != apple_submenu_parent_idx) {
        apple_hover = apple_submenu_parent_idx;
        main_changed = true;
    }

    if (changed) {
        paint_apple_submenu(mb, apple_submenu_w, apple_submenu_h);
    }
    if (main_changed && apple_popup != None) {
        XWindowAttributes wa;
        XGetWindowAttributes(mb->dpy, apple_popup, &wa);
        paint_apple_dropdown(mb, wa.width, wa.height);
    }
}

bool apple_handle_click(MenuBar *mb, int click_x, int click_y)
{
    (void)click_x;
    int item = apple_y_to_item(click_y);

    if (item >= 0 && !apple_items[item].disabled) {
        // Parent row click is a no-op on the action path: the submenu
        // already opens on hover, so a click there just keeps the
        // cascade open instead of dismissing the menu.
        if (item == apple_submenu_parent_idx) {
            if (apple_submenu == None) apple_open_submenu(mb);
            return false;
        }
        // Execute the action, then dismiss
        apple_execute(mb, item);
        return true; // Should dismiss
    }
    return false; // Clicked on separator, disabled, or padding
}

bool apple_handle_submenu_click(MenuBar *mb, int click_x, int click_y)
{
    (void)click_x;
    if (apple_submenu == None) return false;

    int item = -1;
    int row_y = S(4);
    for (int i = 0; i < apple_submenu_count; i++) {
        if (click_y >= row_y && click_y < row_y + S(22)) {
            item = i;
            break;
        }
        row_y += S(22);
    }
    if (item < 0) return false;

    AppleItem *it = &apple_submenu_items[item];
    if (!it->is_running_app || it->target == None) return false;

    // Same EWMH activation as the dock: send _NET_ACTIVE_WINDOW to root,
    // moonrock raises + focuses the target window.
    XEvent ev = {0};
    ev.type                 = ClientMessage;
    ev.xclient.window       = it->target;
    ev.xclient.message_type = mb->atom_net_active_window;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = 2;  // source = pager/taskbar
    XSendEvent(mb->dpy, mb->root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(mb->dpy);
    return true; // dismiss the whole Apple menu
}

bool apple_handle_event(MenuBar *mb, XEvent *ev, bool *should_dismiss)
{
    *should_dismiss = false;

    if (apple_popup == None) return false;

    // Submenu Expose comes first — its window has its own X id, but the
    // grab covers root so motion/button events are delivered through the
    // root-coord path in menubar.c (apple_handle_submenu_motion / click).
    if (apple_submenu != None && ev->xany.window == apple_submenu) {
        if (ev->type == Expose && ev->xexpose.count == 0) {
            paint_apple_submenu(mb, apple_submenu_w, apple_submenu_h);
        }
        return true;
    }

    if (ev->xany.window != apple_popup) return false;

    switch (ev->type) {
    case Expose:
        if (ev->xexpose.count == 0) {
            XWindowAttributes wa;
            XGetWindowAttributes(mb->dpy, apple_popup, &wa);
            paint_apple_dropdown(mb, wa.width, wa.height);
        }
        return true;

    case MotionNotify:
        apple_handle_motion(mb, ev->xmotion.y);
        return true;

    case LeaveNotify:
        // Don't clear the highlight if the pointer just crossed into
        // the submenu — the submenu motion handler keeps the parent row
        // pinned. Only clear when truly leaving every Apple-menu popup.
        if (apple_submenu != None) return true;
        if (apple_hover != -1) {
            apple_hover = -1;
            XWindowAttributes wa;
            XGetWindowAttributes(mb->dpy, apple_popup, &wa);
            paint_apple_dropdown(mb, wa.width, wa.height);
        }
        return true;

    case ButtonPress:
        *should_dismiss = apple_handle_click(mb, ev->xbutton.x, ev->xbutton.y);
        return true;
    }

    return false;
}

void apple_cleanup(void)
{
    // Free the loaded PNG surfaces
    if (logo_normal) {
        cairo_surface_destroy(logo_normal);
        logo_normal = NULL;
    }
    if (logo_selected) {
        cairo_surface_destroy(logo_selected);
        logo_selected = NULL;
    }

    apple_popup = None;
    apple_submenu = None;
    apple_submenu_hover = -1;
    apple_submenu_count = 0;
    apple_submenu_parent_idx = -1;
}
