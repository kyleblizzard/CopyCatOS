// CopyCatOS — by Kyle Blizzard at Blizzard.show

// hello — minimum MoonBase reference app.
//
// Opens one Cairo-rendered window, paints an Aqua-grey background with
// a centered "Hello from MoonBase" label, and quits cleanly when the
// window is closed or the user hits Cmd-Q. Proves the end-to-end path:
// libmoonbase.so.1 loads inside a bwrap sandbox, the IPC handshake with
// MoonRock succeeds, a Cairo window round-trips a commit, and events
// reach an installed .app launched via moonbase-launch.
//
// Uses the app-owns-loop path (moonbase_wait_event). moonbase_run() is
// the MoonBase-owns-loop convenience shape; it is still ENOSYS in the
// current framework slice, so this app drives the loop itself.

#include <moonbase.h>

#include <cairo/cairo.h>

#include <stdio.h>

static void paint(mb_window_t *w)
{
    cairo_t *cr = (cairo_t *)moonbase_window_cairo(w);
    if (!cr) return;

    int width = 0, height = 0;
    moonbase_window_size(w, &width, &height);

    cairo_set_source_rgb(cr, 0xEC / 255.0, 0xEC / 255.0, 0xEC / 255.0);
    cairo_paint(cr);

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

    // Paint once so the window has content the first time MoonRock
    // composites it. Subsequent paints come from MB_EV_WINDOW_REDRAW.
    paint(w);

    int running = 1;
    int exit_code = 0;
    while (running) {
        mb_event_t ev;
        int wr = moonbase_wait_event(&ev, -1);
        if (wr < 0) {
            fprintf(stderr,
                    "hello: wait_event error: %s\n",
                    moonbase_error_string((mb_error_t)wr));
            exit_code = 1;
            break;
        }
        if (wr == 0) continue;

        switch (ev.kind) {
        case MB_EV_WINDOW_REDRAW:
            paint(w);
            break;

        case MB_EV_WINDOW_CLOSED:
        case MB_EV_APP_WILL_QUIT:
            running = 0;
            break;

        case MB_EV_KEY_DOWN:
            if ((ev.key.modifiers & MB_MOD_COMMAND) &&
                (ev.key.keycode == 'q' || ev.key.keycode == 'Q')) {
                running = 0;
            }
            break;

        default:
            break;
        }
    }

    moonbase_window_close(w);
    return exit_code;
}
