// CopyCatOS — by Kyle Blizzard at Blizzard.show

// hello — minimum MoonBase reference app.
//
// Opens one Cairo-rendered window, paints an Aqua-grey background with
// a centered "Hello from MoonBase" label, and quits cleanly when the
// window is closed or the user hits Cmd-Q. Proves the end-to-end path:
// libmoonbase.so.1 loads inside a bwrap sandbox, the IPC handshake with
// MoonRock succeeds, a Cairo window round-trips a commit, and events
// reach an installed .appc launched via moonbase-launch.
//
// Everything here is deliberately boring. The value isn't in the
// rendering — it's in proving the plumbing works the same way a real
// app would use it.

#include <moonbase.h>

#include <cairo/cairo.h>

#include <stdio.h>

static void paint(mb_window_t *w)
{
    cairo_t *cr = (cairo_t *)moonbase_window_cairo(w);
    if (!cr) return;

    int width = 0, height = 0;
    moonbase_window_size(w, &width, &height);

    // Window body — Aqua neutral #ECECEC.
    cairo_set_source_rgb(cr, 0xEC / 255.0, 0xEC / 255.0, 0xEC / 255.0);
    cairo_paint(cr);

    // Centered label. Lucida Grande is the only UI font on this OS;
    // Cairo's toy-font-face API falls back gracefully when it's
    // missing (e.g. running outside an installed CopyCatOS session).
    cairo_select_font_face(cr, "Lucida Grande",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 24);
    cairo_set_source_rgb(cr, 0x1A / 255.0, 0x1A / 255.0, 0x1A / 255.0);

    const char *msg = "Hello from MoonBase";
    cairo_text_extents_t ext;
    cairo_text_extents(cr, msg, &ext);
    cairo_move_to(cr,
                  width / 2.0 - ext.width / 2.0 - ext.x_bearing,
                  height / 2.0 - ext.height / 2.0 - ext.y_bearing);
    cairo_show_text(cr, msg);

    moonbase_window_commit(w);
}

static void on_event(const mb_event_t *ev, void *userdata)
{
    mb_window_t *w = (mb_window_t *)userdata;

    switch (ev->kind) {
    case MB_EV_WINDOW_REDRAW:
        paint(w);
        break;

    case MB_EV_WINDOW_CLOSED:
        moonbase_quit(0);
        break;

    case MB_EV_KEY_DOWN:
        // Cmd-Q quits. Plain Q is ignored — this app has no commands.
        if ((ev->key.modifiers & MB_MOD_COMMAND) &&
            (ev->key.keycode == 'q' || ev->key.keycode == 'Q')) {
            moonbase_quit(0);
        }
        break;

    default:
        break;
    }
}

int main(int argc, char **argv)
{
    int rc = moonbase_init(argc, argv);
    if (rc != MB_EOK) {
        fprintf(stderr,
                "hello: moonbase_init failed: %s\n",
                moonbase_error_string((mb_error_t)rc));
        return 1;
    }

    mb_window_desc_t desc = {
        .version       = MOONBASE_WINDOW_DESC_VERSION,
        .title         = "Hello",
        .width_points  = 480,
        .height_points = 320,
        .render_mode   = MOONBASE_RENDER_CAIRO,
        .flags         = MB_WINDOW_FLAG_CENTER,
    };

    mb_window_t *w = moonbase_window_create(&desc);
    if (!w) {
        fprintf(stderr,
                "hello: moonbase_window_create failed: %s\n",
                moonbase_error_string(moonbase_last_error()));
        return 1;
    }

    moonbase_set_event_handler(on_event, w);
    moonbase_window_show(w);

    return moonbase_run();
}
