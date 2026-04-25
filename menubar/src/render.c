// CopyCatOS — by Kyle Blizzard at Blizzard.show

// render.c — Daemon-side wrappers for the shared menubar render library.
//
// Slice 19.B moved the actual Cairo/Pango bodies into
// menubar/render/menubar_render.c. This file's only job now is to plug
// the daemon's global `menubar_scale` and MENUBAR_HEIGHT into the
// explicit-scale signatures the shared library exposes.
//
// Every render_* call site in the daemon (paint_pane, appmenu, systray,
// dropdowns) keeps working unchanged — those modules still call
// render_text / render_background / render_hover_highlight with no
// scale argument, and these forwarders pass the global scale through.
//
// Behavior is identical to the pre-19.B render.c: same texture, same
// font formula, same gradient, same hover pill, same vertical-centering
// math. The only observable diff is a log-tag rename
// ("[menubar]" → "[menubar-render]") so messages from the shared
// library are tagged honestly when both the daemon and moonbase-launcher
// link it.

#include "menubar.h"
#include "render.h"

#include "../render/menubar_render.h"

void render_init(MenuBar *mb)
{
    (void)mb;
    menubar_render_init();
}

void render_background(MenuBar *mb, MenuBarPane *pane, cairo_t *cr)
{
    (void)mb;
    menubar_render_background(cr, pane->screen_w, MENUBAR_HEIGHT,
                              MENUBAR_THEME_AQUA);
}

double render_text(cairo_t *cr, const char *text, double x, double y,
                   bool bold, double r, double g, double b)
{
    return menubar_render_text(cr, text, x, y, bold, r, g, b, menubar_scale);
}

double render_measure_text(const char *text, bool bold)
{
    return menubar_render_measure_text(text, bold, menubar_scale);
}

int render_text_center_y(const char *text, bool bold)
{
    return menubar_render_text_center_y(text, bold, MENUBAR_HEIGHT, menubar_scale);
}

void render_hover_highlight(cairo_t *cr, int x, int y, int w, int h)
{
    menubar_render_hover_highlight(cr, x, y, w, h, menubar_scale);
}

void render_cleanup(void)
{
    menubar_render_cleanup();
}
