// CopyCatOS — by Kyle Blizzard at Blizzard.show

// menubar_render.h — Shared Aqua chrome rendering primitives.
//
// One source of truth for menubar pixels. Used by:
//   - the full menubar daemon (menubar/src/) — paints the global bar
//     across one or more output panes, plus its dropdowns.
//   - moonbase-launcher's foreign-distro chrome stub
//     (moonbase/runtime/) — paints a unified title + menu bar inside
//     an .app's own window when no MoonRock session is running.
//
// Two layers of API:
//
//   * Low-level primitives — text, measure, center-y, hover, background.
//     The daemon's render.c forwards each render_* call to the matching
//     menubar_render_* with `menubar_scale` plugged in. That's how 19.B
//     stays zero-behavior-change for the daemon.
//
//   * High-level "paint a whole bar" — layout_menus, paint_title_bar,
//     paint_menu_bar. The foreign-distro chrome stub calls these. The
//     daemon does not — it has its own paint flow with apple logo,
//     dropdowns, systray, etc. that's far past one function.
//
// All sizes are in points unless suffixed _px. Callers that hold an
// effective scale (daemon: menubar_scale; launcher: per-output scale)
// pass it explicitly to every function — no globals leak across the
// shared-library boundary.

#ifndef CC_MENUBAR_RENDER_H
#define CC_MENUBAR_RENDER_H

#include <cairo/cairo.h>
#include <stdbool.h>
#include <stddef.h>

// Snow Leopard baseline — 22 points each. Combined chrome height in
// foreign-distro mode is title + menu = 44 points before scale.
#define MENUBAR_RENDER_TITLE_BAR_POINTS 22
#define MENUBAR_RENDER_MENU_BAR_POINTS  22

// Chrome theme variant. Default is pixel-perfect Snow Leopard Aqua.
// Host variants exist for the foreign-distro chrome stub when the user
// opts in *per-app* via the app's own View → Use Host Desktop Theme
// menu item (a standard framework-injected checkbox item). Persistence
// is per-app inside MoonBase Preferences
// (~/.local/share/moonbase/<bundle-id>/Preferences/), not global.
// ~/.config/copycatos/moonbase.conf only supplies the default for new
// apps that have no per-app preference yet.
typedef enum {
    MENUBAR_THEME_AQUA = 0,           // Snow Leopard pixel-perfect (hard default)
    MENUBAR_THEME_HOST_BREEZE_LIGHT,  // KDE Breeze tint via XSettings/portal
    MENUBAR_THEME_HOST_ADWAITA_LIGHT, // GNOME Adwaita tint via XSettings/portal
} menubar_render_theme_t;

// Toggle indicator drawn at a row's left margin. Mirrors appmenu.c's
// MENU_TOGGLE_NONE / CHECKMARK / RADIO + toggle_state pair: the caller
// passes the *displayed* state, not the underlying type+state — i.e.
// an unchecked checkbox is `MENUBAR_TOGGLE_NONE`, not "CHECKMARK off".
// Keeps the descriptor a pure rendering contract.
typedef enum {
    MENUBAR_TOGGLE_NONE = 0,    // no glyph
    MENUBAR_TOGGLE_CHECKMARK,   // ✓ U+2713 — checkbox toggled on
    MENUBAR_TOGGLE_RADIO,       // • U+2022 — radio item selected
} menubar_render_toggle_t;

// Snow Leopard dropdown menu item — used by both the daemon's appmenu
// dropdown render (after migration) and moonbase-launcher's foreign
// distro chrome stub. The View → Use Host Desktop Theme item is the
// concrete reason this descriptor exists: the toggle's checkmark must
// look identical whether the daemon paints it (CopyCatOS session) or
// the chrome stub paints it (foreign distro). Same primitive, same
// pixels, no divergence.
//
// Pixel formula matches menubar/src/appmenu.c's existing row paint
// loop (S(4)/S(6)/S(18)/S(14) margins, ROW_H_ITEM height, SF(3.0)
// selection-pill radius, #3875D7 selection blue). When the daemon
// migrates its dropdown render onto this primitive, output is
// bit-identical to today's pixels — including radio toggles.
//
// Fields the caller doesn't need can be left zero / NULL: the chrome
// stub's View checkbox passes `is_submenu = false` and `shortcut = NULL`;
// a separator row isn't drawn through this struct at all (separators
// are a sibling primitive, not a menu item).
typedef struct {
    const char              *label;       // UTF-8 label (must not be NULL — pass "")
    menubar_render_toggle_t  toggle;      // glyph at left margin (NONE = nothing)
    bool                     enabled;     // grayed out (#A6A6A6) when false
    bool                     is_submenu;  // ▸ glyph at right margin when true
    const char              *shortcut;    // UTF-8 right-aligned shortcut text, or NULL
} menubar_render_menu_item_t;

typedef struct {
    const char *title;            // UTF-8, owned by caller
    int         x;                // assigned by menubar_render_layout_menus
    int         width;            // assigned by menubar_render_layout_menus
    bool        is_app_name_bold; // first item is the bold app name
} menubar_render_item_t;

// Logical heights in points. Multiply by effective scale to get pixels.
int menubar_render_title_bar_height_pts(void);
int menubar_render_menu_bar_height_pts(void);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Load the menubar background texture (~/.local/share/aqua-widgets/
// menubar/menubar_bg.png) once per process. Idempotent. If the asset
// is missing, the background path falls back to the canonical 4-stop
// gradient defined in CLAUDE.md's "Menu bar gradient" line.
void menubar_render_init(void);

// Free the background texture. Safe to call before init.
void menubar_render_cleanup(void);

// ---------------------------------------------------------------------------
// Low-level primitives — daemon and stub both call these
// ---------------------------------------------------------------------------

// Paint the menubar background across (width_px, height_px). Tiles the
// real Snow Leopard asset when present; falls back to the canonical
// 4-stop gradient + 1px bottom border. theme picks AQUA or a host tint.
void menubar_render_background(cairo_t *cr, int width_px, int height_px,
                               menubar_render_theme_t theme);

// Draw text at (x, y) using Lucida Grande (or Bold), color (r, g, b),
// font size scaled by `scale`. Returns the rendered pixel width so the
// caller can advance its layout cursor.
double menubar_render_text(cairo_t *cr, const char *text,
                           double x, double y, bool bold,
                           double r, double g, double b, double scale);

// Measure a text run's pixel width without drawing it (used during
// layout). Same Pango font + scale formula as menubar_render_text.
double menubar_render_measure_text(const char *text, bool bold, double scale);

// Y coordinate that vertically centers `text` inside a bar of
// bar_height_px pixels at the given scale. Uses Pango's actual layout
// height so the baseline stays on the bar's midline at fractional
// scales (1.25×, 1.5×, 1.75×) where a hardcoded font-size constant
// would drift and push text toward the top edge.
int menubar_render_text_center_y(const char *text, bool bold,
                                 int bar_height_px, double scale);

// Subtle dark rounded-rect overlay — the hover pill behind a menu
// title, apple logo, or systray glyph. Corner radius scales with `scale`.
void menubar_render_hover_highlight(cairo_t *cr, int x, int y, int w, int h,
                                    double scale);

// ---------------------------------------------------------------------------
// High-level paint — foreign-distro chrome stub
// ---------------------------------------------------------------------------

// Lay out menu items in a horizontal line starting at origin_x. Each
// item's x and width are written based on Pango text metrics at the
// given scale. Caller already reserved space for traffic lights and
// any preceding gap; origin_x is the first pixel after them.
//
// items[i].x is the left edge of the slot; the title itself is drawn
// inset by half the per-item padding so paint_menu_bar doesn't have
// to re-measure. items[i].width is the full slot width including pad.
void menubar_render_layout_menus(menubar_render_item_t *items, size_t n,
                                 int origin_x, double scale);

// Paint a row of menu items already laid out by layout_menus. The
// slot at hover_index gets the rounded pill; pass -1 to disable hover.
void menubar_render_paint_menu_bar(cairo_t *cr, int width_px, int height_px,
                                   const menubar_render_item_t *items, size_t n,
                                   int hover_index,
                                   menubar_render_theme_t theme,
                                   double scale);

// ---------------------------------------------------------------------------
// Dropdown row paint — used by daemon dropdowns AND foreign chrome stub
// ---------------------------------------------------------------------------

// Paint one dropdown row inside the rectangle (x, y, w, h). The caller
// owns the surrounding panel — this primitive draws only the row itself
// (selection pill + label + optional checkmark + optional submenu arrow
// + optional shortcut). When `hovered` is true and the item is enabled,
// the SL selection pill is painted at S(4) inset with SF(3.0) corner
// radius in #3875D7 and text flips to white.
//
// `h` is the row's full pixel height — pass S(ROW_H_ITEM) (= 22 × scale,
// rounded) to match appmenu.c's row geometry exactly. The selection
// pill spans the full h; label/glyph baselines are insets from y.
//
// Pixel formula matches menubar/src/appmenu.c lines 730-820 verbatim.
// When the daemon migrates its dropdown render onto this primitive, the
// rendered pixels are bit-identical to today's output.
void menubar_render_paint_menu_item(cairo_t *cr,
                                    int x, int y, int w, int h,
                                    const menubar_render_menu_item_t *item,
                                    bool hovered,
                                    menubar_render_theme_t theme,
                                    double scale);

#endif // CC_MENUBAR_RENDER_H
