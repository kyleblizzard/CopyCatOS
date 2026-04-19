// CopyCatOS — by Kyle Blizzard at Blizzard.show

// keychain-access — reference .appc exercising moonbase_keychain.h.
//
// Third of the four pre-freeze reference apps. The goal is coverage of
// every public symbol in moonbase_keychain.h end-to-end through a real
// .appc launched via moonbase-launch:
//
//   moonbase_keychain_list / _count / _service / _label / _account / _free
//   moonbase_keychain_store
//   moonbase_keychain_fetch
//   moonbase_keychain_delete
//   moonbase_keychain_generate_password
//   moonbase_keychain_secret_free
//
// v0.1 scope stays tight: no text-input form for label / account, no
// editing of existing items, no copy-to-clipboard (clipboard IPC isn't
// in the framework yet). The New shortcut auto-generates a 20-char
// password and a sequential label — that's enough to drive every path
// on the keychain adapter without pulling in the cursor / modal-dialog
// plumbing that TextEdit already validates. Later revisions grow into
// a full add / edit form the moment the framework grows the fields.
//
// Shortcuts (Cmd is Apple-style by default; toggle lives in
// SysPrefs → Keyboard once that pane ships):
//
//   Up / Down     — move selection in the list
//   Cmd-N         — generate + store a new sample entry
//   Cmd-R         — reveal / hide the secret for the selected entry
//   Cmd-Delete    — delete the selected entry
//   Cmd-Q         — quit
//
// Layout is two-column: 240pt list on the left, detail on the right,
// a thin status strip across the bottom. Everything measures in
// points — MoonBase scales the Cairo surface for us.

#include <moonbase.h>
#include <moonbase_keychain.h>

#include <cairo/cairo.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------
// X11 keysyms we care about.
// ---------------------------------------------------------------------
#define KS_BACKSPACE 0xFF08
#define KS_DELETE    0xFFFF
#define KS_UP        0xFF52
#define KS_DOWN      0xFF54

// ---------------------------------------------------------------------
// Visual constants (points).
// ---------------------------------------------------------------------
#define WIN_W           720
#define WIN_H           480
#define LIST_W          240
#define ROW_H           22
#define MARGIN_X        12
#define MARGIN_Y        10
#define STATUS_H        22
#define FONT_SIZE_BODY  12
#define FONT_SIZE_HEAD  14

// ---------------------------------------------------------------------
// App state.
//
// `items` is the live snapshot from moonbase_keychain_list. We refresh
// it every time a mutation happens (store / delete) so the UI stays
// consistent with the keyring. `selected` is a row index into that
// snapshot, or -1 when the list is empty. `revealed` holds the secret
// string for the selected row when the user pressed Cmd-R on it; NULL
// otherwise. The owned buffer is zeroed-and-freed via
// moonbase_keychain_secret_free the moment selection changes or the
// user hides it.
//
// `status` holds a short human-readable message drawn along the bottom
// of the window — useful for "New entry created", "Item deleted",
// "Fetch failed: ...". Always a static string pointer, never dynamic.
// ---------------------------------------------------------------------
static mb_keychain_list_t *g_items    = NULL;
static int                 g_selected = -1;
static char               *g_revealed = NULL;
static const char         *g_status   = "";
static int                 g_new_counter = 0;

static void clear_revealed(void) {
    if (g_revealed) {
        moonbase_keychain_secret_free(g_revealed);
        g_revealed = NULL;
    }
}

// Pull a fresh snapshot. Callers clamp / update g_selected afterwards
// so the index still makes sense against the new count.
static void refresh_items(void) {
    if (g_items) {
        moonbase_keychain_list_free(g_items);
        g_items = NULL;
    }
    mb_error_t e = moonbase_keychain_list(&g_items);
    if (e != MB_EOK) {
        // Leave g_items NULL — the UI renders "(empty)" and the
        // status line surfaces the failure.
        g_status = moonbase_error_string(e);
    }
}

// Produce a 20-char password drawn from every class. The generator
// lives in libmoonbase, so v0.1 exercises that path too.
static char *generate_sample_password(void) {
    char *pw = NULL;
    mb_error_t e = moonbase_keychain_generate_password(
        20, MB_KEYCHAIN_PW_ALL, &pw);
    if (e != MB_EOK) return NULL;
    return pw;
}

// Store a new sample item. Label and account are synthesized so v0.1
// doesn't need a text-input form. Label looks like "Sample-001"; the
// counter lives across the session so repeated Cmd-N presses stay
// distinct.
static void action_new(void) {
    char *pw = generate_sample_password();
    if (!pw) {
        g_status = "Password generation failed";
        return;
    }

    char label[32];
    g_new_counter++;
    snprintf(label, sizeof(label), "Sample-%03d", g_new_counter);

    // NULL service => the app's own bundle-id, exactly what a sandboxed
    // third-party app would get.
    mb_error_t e = moonbase_keychain_store(NULL, label,
                                           "user@example.com", pw);
    moonbase_keychain_secret_free(pw);

    if (e != MB_EOK) {
        g_status = moonbase_error_string(e);
        return;
    }

    g_status = "Created sample entry";
    clear_revealed();
    refresh_items();
    // Select the newly-created entry if we can find it by label.
    // secret-service doesn't guarantee ordering, so a linear scan is
    // the honest way to locate it.
    size_t n = moonbase_keychain_list_count(g_items);
    for (size_t i = 0; i < n; i++) {
        const char *lab = moonbase_keychain_list_label(g_items, i);
        if (lab && strcmp(lab, label) == 0) {
            g_selected = (int)i;
            return;
        }
    }
    // Fallback: keep the selection valid.
    if (n > 0 && (g_selected < 0 || (size_t)g_selected >= n)) {
        g_selected = 0;
    }
}

static void action_delete(void) {
    if (g_selected < 0 || !g_items) return;
    size_t i = (size_t)g_selected;
    if (i >= moonbase_keychain_list_count(g_items)) return;

    const char *svc = moonbase_keychain_list_service(g_items, i);
    const char *lab = moonbase_keychain_list_label  (g_items, i);
    const char *acc = moonbase_keychain_list_account(g_items, i);
    if (!svc || !lab || !acc) {
        g_status = "Entry missing required fields";
        return;
    }

    mb_error_t e = moonbase_keychain_delete(svc, lab, acc);
    if (e != MB_EOK) {
        g_status = moonbase_error_string(e);
        return;
    }

    g_status = "Deleted";
    clear_revealed();
    refresh_items();

    size_t n = moonbase_keychain_list_count(g_items);
    if (n == 0) {
        g_selected = -1;
    } else if ((size_t)g_selected >= n) {
        g_selected = (int)n - 1;
    }
}

static void action_toggle_reveal(void) {
    if (g_selected < 0 || !g_items) return;
    if (g_revealed) {
        clear_revealed();
        g_status = "Secret hidden";
        return;
    }

    size_t i = (size_t)g_selected;
    const char *svc = moonbase_keychain_list_service(g_items, i);
    const char *lab = moonbase_keychain_list_label  (g_items, i);
    const char *acc = moonbase_keychain_list_account(g_items, i);
    if (!svc || !lab || !acc) {
        g_status = "Entry missing required fields";
        return;
    }

    mb_error_t e = moonbase_keychain_fetch(svc, lab, acc, &g_revealed);
    if (e != MB_EOK) {
        g_revealed = NULL;
        g_status = moonbase_error_string(e);
        return;
    }
    g_status = "Secret revealed";
}

// ---------------------------------------------------------------------
// Paint.
// ---------------------------------------------------------------------

static void paint_list(cairo_t *cr, int height) {
    // Sidebar background — Aqua source-list grey.
    cairo_set_source_rgb(cr,
                         0xD8 / 255.0, 0xDE / 255.0, 0xE8 / 255.0);
    cairo_rectangle(cr, 0, 0, LIST_W, height);
    cairo_fill(cr);

    // Divider.
    cairo_set_source_rgb(cr,
                         0xA0 / 255.0, 0xA0 / 255.0, 0xA0 / 255.0);
    cairo_rectangle(cr, LIST_W - 1, 0, 1, height);
    cairo_fill(cr);

    cairo_select_font_face(cr, "Lucida Grande",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, FONT_SIZE_BODY);

    size_t n = moonbase_keychain_list_count(g_items);
    if (n == 0) {
        cairo_set_source_rgb(cr,
                             0x73 / 255.0, 0x73 / 255.0, 0x73 / 255.0);
        cairo_move_to(cr, MARGIN_X, MARGIN_Y + FONT_SIZE_BODY);
        cairo_show_text(cr, "(no entries — press Cmd-N to create one)");
        return;
    }

    double y = MARGIN_Y + FONT_SIZE_BODY;
    for (size_t i = 0; i < n; i++) {
        if ((int)i == g_selected) {
            // Selection pill — Aqua selection blue.
            cairo_set_source_rgb(cr,
                                 0x38 / 255.0, 0x75 / 255.0, 0xD7 / 255.0);
            cairo_rectangle(cr,
                            MARGIN_X - 4,
                            y - FONT_SIZE_BODY,
                            LIST_W - 2 * (MARGIN_X - 4),
                            ROW_H - 2);
            cairo_fill(cr);
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        } else {
            cairo_set_source_rgb(cr,
                                 0x1A / 255.0,
                                 0x1A / 255.0,
                                 0x1A / 255.0);
        }

        const char *lab = moonbase_keychain_list_label(g_items, i);
        cairo_move_to(cr, MARGIN_X, y);
        cairo_show_text(cr, lab ? lab : "(unnamed)");

        y += ROW_H;
    }
}

static void paint_detail(cairo_t *cr, int width, int height) {
    (void)height;
    double x = LIST_W + MARGIN_X;
    double y = MARGIN_Y + FONT_SIZE_HEAD;

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_rectangle(cr, LIST_W, 0, width - LIST_W, height);
    cairo_fill(cr);

    cairo_select_font_face(cr, "Lucida Grande",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, FONT_SIZE_HEAD);
    cairo_set_source_rgb(cr,
                         0x1A / 255.0, 0x1A / 255.0, 0x1A / 255.0);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, "Keychain Access");
    y += ROW_H + 4;

    cairo_select_font_face(cr, "Lucida Grande",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, FONT_SIZE_BODY);

    if (g_selected < 0 || !g_items) {
        cairo_set_source_rgb(cr,
                             0x73 / 255.0, 0x73 / 255.0, 0x73 / 255.0);
        cairo_move_to(cr, x, y);
        cairo_show_text(cr,
                        "Select an entry on the left, or Cmd-N to create one.");
        return;
    }

    size_t i = (size_t)g_selected;
    const char *svc = moonbase_keychain_list_service(g_items, i);
    const char *lab = moonbase_keychain_list_label  (g_items, i);
    const char *acc = moonbase_keychain_list_account(g_items, i);

    struct { const char *label; const char *value; } rows[] = {
        { "Name",    lab ? lab : "" },
        { "Account", acc ? acc : "" },
        { "Service", svc ? svc : "" },
    };

    cairo_set_source_rgb(cr,
                         0x73 / 255.0, 0x73 / 255.0, 0x73 / 255.0);
    for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
        cairo_move_to(cr, x, y);
        cairo_show_text(cr, rows[k].label);
        cairo_move_to(cr, x + 80, y);
        cairo_set_source_rgb(cr,
                             0x1A / 255.0, 0x1A / 255.0, 0x1A / 255.0);
        cairo_show_text(cr, rows[k].value);
        y += ROW_H;
        cairo_set_source_rgb(cr,
                             0x73 / 255.0, 0x73 / 255.0, 0x73 / 255.0);
    }

    y += 8;
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, "Secret");
    cairo_move_to(cr, x + 80, y);
    cairo_set_source_rgb(cr,
                         0x1A / 255.0, 0x1A / 255.0, 0x1A / 255.0);
    cairo_show_text(cr, g_revealed ? g_revealed : "••••••••  (Cmd-R to reveal)");
}

static void paint_status(cairo_t *cr, int width, int height) {
    double top = height - STATUS_H;
    cairo_set_source_rgb(cr,
                         0xEC / 255.0, 0xEC / 255.0, 0xEC / 255.0);
    cairo_rectangle(cr, 0, top, width, STATUS_H);
    cairo_fill(cr);

    cairo_set_source_rgb(cr,
                         0xA0 / 255.0, 0xA0 / 255.0, 0xA0 / 255.0);
    cairo_rectangle(cr, 0, top, width, 1);
    cairo_fill(cr);

    cairo_select_font_face(cr, "Lucida Grande",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    cairo_set_source_rgb(cr,
                         0x73 / 255.0, 0x73 / 255.0, 0x73 / 255.0);
    cairo_move_to(cr, MARGIN_X, top + 15);
    cairo_show_text(cr, g_status);

    const char *shortcuts = "Cmd-N New · Cmd-R Reveal · Cmd-Delete Remove · Cmd-Q Quit";
    cairo_text_extents_t ext;
    cairo_text_extents(cr, shortcuts, &ext);
    cairo_move_to(cr, width - ext.x_advance - MARGIN_X, top + 15);
    cairo_show_text(cr, shortcuts);
}

static void paint(mb_window_t *w) {
    cairo_t *cr = (cairo_t *)moonbase_window_cairo(w);
    if (!cr) return;

    int width = 0, height = 0;
    moonbase_window_size(w, &width, &height);

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    paint_list  (cr, height - STATUS_H);
    paint_detail(cr, width, height - STATUS_H);
    paint_status(cr, width, height);

    moonbase_window_commit(w);
}

// ---------------------------------------------------------------------
// main.
// ---------------------------------------------------------------------

int main(int argc, char **argv) {
    int rc = moonbase_init(argc, argv);
    if (rc != MB_EOK) {
        fprintf(stderr,
                "keychain-access: moonbase_init failed: %s\n",
                moonbase_error_string((mb_error_t)rc));
        return 1;
    }

    mb_window_desc_t desc = {
        .version       = MOONBASE_WINDOW_DESC_VERSION,
        .title         = "Keychain Access",
        .width_points  = WIN_W,
        .height_points = WIN_H,
        .render_mode   = MOONBASE_RENDER_CAIRO,
        .flags         = MB_WINDOW_FLAG_CENTER,
    };

    mb_window_t *w = moonbase_window_create(&desc);
    if (!w) {
        fprintf(stderr,
                "keychain-access: moonbase_window_create failed: %s\n",
                moonbase_error_string(moonbase_last_error()));
        return 1;
    }

    refresh_items();
    if (moonbase_keychain_list_count(g_items) > 0) {
        g_selected = 0;
        g_status = "Ready";
    } else {
        g_status = "No entries yet — press Cmd-N to add one";
    }

    paint(w);

    int running   = 1;
    int exit_code = 0;

    while (running) {
        mb_event_t ev;
        int wr = moonbase_wait_event(&ev, -1);
        if (wr < 0) {
            fprintf(stderr,
                    "keychain-access: wait_event error: %s\n",
                    moonbase_error_string((mb_error_t)wr));
            exit_code = 1;
            break;
        }
        if (wr == 0) continue;

        bool dirty = false;

        switch (ev.kind) {
        case MB_EV_WINDOW_REDRAW:
            dirty = true;
            break;

        case MB_EV_WINDOW_CLOSED:
        case MB_EV_APP_WILL_QUIT:
            running = 0;
            break;

        case MB_EV_KEY_DOWN: {
            bool cmd = (ev.key.modifiers & MB_MOD_COMMAND) != 0;

            if (cmd && (ev.key.keycode == 'q' || ev.key.keycode == 'Q')) {
                running = 0;
                break;
            }
            if (cmd && (ev.key.keycode == 'n' || ev.key.keycode == 'N')) {
                action_new();
                dirty = true;
                break;
            }
            if (cmd && (ev.key.keycode == 'r' || ev.key.keycode == 'R')) {
                action_toggle_reveal();
                dirty = true;
                break;
            }
            if (cmd && (ev.key.keycode == KS_BACKSPACE ||
                        ev.key.keycode == KS_DELETE)) {
                action_delete();
                dirty = true;
                break;
            }

            // Un-modified navigation keys.
            if (!cmd) {
                size_t n = moonbase_keychain_list_count(g_items);
                if (ev.key.keycode == KS_UP && n > 0) {
                    if (g_selected > 0) {
                        g_selected--;
                        clear_revealed();
                        dirty = true;
                    }
                } else if (ev.key.keycode == KS_DOWN && n > 0) {
                    if ((size_t)g_selected + 1 < n) {
                        g_selected++;
                        clear_revealed();
                        dirty = true;
                    }
                }
            }
            break;
        }

        default:
            break;
        }

        if (dirty) paint(w);
    }

    clear_revealed();
    if (g_items) moonbase_keychain_list_free(g_items);
    moonbase_window_close(w);
    return exit_code;
}
