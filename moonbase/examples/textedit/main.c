// CopyCatOS — by Kyle Blizzard at Blizzard.show

// textedit — the smallest useful Cairo app on MoonBase.
//
// One window, one in-memory UTF-8 byte buffer with a cursor. Printable
// text arrives as MB_EV_TEXT_INPUT and gets inserted at the cursor;
// BackSpace / Delete / Left / Right / Up / Down / Home / End / Return
// arrive as MB_EV_KEY_DOWN and move the cursor or mutate the buffer.
// Cmd-Q quits. The window's close button quits.
//
// Scope is deliberately narrow so this app pressure-tests the framework,
// not the editor experience:
//   * no file open / save (the file picker isn't in the framework yet)
//   * no menu bar, no toolbar (menu IPC is a later framework slice)
//   * no selection, no clipboard, no undo
//   * no word wrap (lines split on literal '\n'; long lines draw past
//     the right edge without reflow)
//   * no cursor blink (static bar; visible only when the window has
//     keyboard focus)
//   * no font metrics beyond what Cairo's toy text API gives us
//
// Everything above exists solely so the Tier 1 TEXT_INPUT + KEY_DOWN +
// focus + commit paths round-trip end-to-end through a real .appc
// launched via moonbase-launch. When real menus / files / selection
// land in the framework, TextEdit grows into them — the skeleton here
// doesn't need to be thrown away for any of that.

#include <moonbase.h>

#include <cairo/cairo.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------
// X11 keysyms we care about.
//
// MoonBase exposes ev.key.keycode as the raw X11 keysym (the public API
// says so). We only need a handful here, so they live as literals
// rather than dragging <X11/keysym.h> into a MoonBase app. Any app that
// needs the full set is free to include keysym.h itself — the values
// are identical.
// ---------------------------------------------------------------------
#define KS_BACKSPACE 0xFF08
#define KS_RETURN    0xFF0D
#define KS_DELETE    0xFFFF
#define KS_HOME      0xFF50
#define KS_LEFT      0xFF51
#define KS_UP        0xFF52
#define KS_RIGHT     0xFF53
#define KS_DOWN      0xFF54
#define KS_END       0xFF57

// ---------------------------------------------------------------------
// Visual constants. Points, not pixels — MoonBase scales the Cairo
// surface for us, so these numbers stay the same on a 1.0x external
// monitor and a 1.5x Legion Go S panel.
// ---------------------------------------------------------------------
#define MARGIN_X        12   // content inset from the window's left
#define MARGIN_Y        10   // content inset from the window's top
#define LINE_HEIGHT     16   // baseline-to-baseline spacing
#define FONT_SIZE       13
#define CURSOR_WIDTH    1

// ---------------------------------------------------------------------
// Buffer. Fixed capacity is fine for a reference app; real text editors
// use a gap buffer or piece table, but v0.1 only needs to prove the IPC
// path works, and 4 KiB is several screens of prose.
// ---------------------------------------------------------------------
#define BUFFER_CAP 4096

typedef struct {
    char   bytes[BUFFER_CAP];
    size_t len;
    size_t cursor;           // byte offset in [0, len]
} buffer_t;

static buffer_t g_buf;
static bool     g_has_focus = false;

static void buffer_insert(buffer_t *b, const char *src, size_t n) {
    if (n == 0) return;
    if (b->len + n > BUFFER_CAP) {
        // Drop silently. A real editor would beep; v0.1 just refuses
        // to overflow the fixed buffer.
        return;
    }
    // Shift the tail right to make room, then memcpy the new bytes in.
    memmove(b->bytes + b->cursor + n,
            b->bytes + b->cursor,
            b->len - b->cursor);
    memcpy(b->bytes + b->cursor, src, n);
    b->len    += n;
    b->cursor += n;
}

static void buffer_delete_before(buffer_t *b) {
    if (b->cursor == 0) return;
    memmove(b->bytes + b->cursor - 1,
            b->bytes + b->cursor,
            b->len - b->cursor);
    b->cursor--;
    b->len--;
}

static void buffer_delete_after(buffer_t *b) {
    if (b->cursor >= b->len) return;
    memmove(b->bytes + b->cursor,
            b->bytes + b->cursor + 1,
            b->len - b->cursor - 1);
    b->len--;
}

// Find the byte offset of the start of the line containing `off`.
static size_t line_start(const buffer_t *b, size_t off) {
    while (off > 0 && b->bytes[off - 1] != '\n') off--;
    return off;
}

// Find the byte offset of the '\n' (or end-of-buffer) that ends the
// line containing `off`.
static size_t line_end(const buffer_t *b, size_t off) {
    while (off < b->len && b->bytes[off] != '\n') off++;
    return off;
}

// Move the cursor up or down one line, trying to keep the same column
// when the target line is long enough. A shorter line clamps to its
// own end.
static void cursor_vertical(buffer_t *b, int dir) {
    size_t cur_line_start = line_start(b, b->cursor);
    size_t column          = b->cursor - cur_line_start;

    if (dir < 0) {
        if (cur_line_start == 0) return;         // already on top line
        size_t prev_end     = cur_line_start - 1; // the '\n' itself
        size_t prev_start   = line_start(b, prev_end);
        size_t prev_length  = prev_end - prev_start;
        size_t new_column   = column < prev_length ? column : prev_length;
        b->cursor = prev_start + new_column;
    } else {
        size_t cur_end = line_end(b, b->cursor);
        if (cur_end >= b->len) return;           // already on bottom line
        size_t next_start   = cur_end + 1;       // past the '\n'
        size_t next_end     = line_end(b, next_start);
        size_t next_length  = next_end - next_start;
        size_t new_column   = column < next_length ? column : next_length;
        b->cursor = next_start + new_column;
    }
}

// ---------------------------------------------------------------------
// Paint. White page, grey border, text in Lucida Grande. The cursor is
// a 1-point vertical bar drawn immediately after the glyph run that
// precedes it on the cursor's line. We measure that run with Cairo's
// cairo_text_extents so the bar lands in the right spot on any scale.
// ---------------------------------------------------------------------
static void paint(mb_window_t *w)
{
    cairo_t *cr = (cairo_t *)moonbase_window_cairo(w);
    if (!cr) return;

    int width = 0, height = 0;
    moonbase_window_size(w, &width, &height);

    // Page background. TextEdit's document body is white on Snow
    // Leopard; the surrounding window chrome (title bar, border) is
    // drawn by MoonRock, not us.
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    cairo_select_font_face(cr, "Lucida Grande",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, FONT_SIZE);
    cairo_set_source_rgb(cr,
                         0x1A / 255.0, 0x1A / 255.0, 0x1A / 255.0);

    // Walk the buffer line-by-line. A line is the run of bytes between
    // the previous '\n' (or buffer start) and the next '\n' (or buffer
    // end). We also find which line and what column the cursor sits on
    // so we can draw its bar in the same pass without a second scan.
    double  y = MARGIN_Y + FONT_SIZE;
    size_t  i = 0;
    int     line_index       = 0;
    int     cursor_line      = 0;
    double  cursor_x         = MARGIN_X;
    double  cursor_y_top     = MARGIN_Y;
    bool    cursor_placed    = false;

    while (i <= g_buf.len) {
        size_t line_from = i;
        size_t j = line_from;
        while (j < g_buf.len && g_buf.bytes[j] != '\n') j++;
        size_t line_bytes = j - line_from;

        // Draw the line. cairo_show_text needs a C string, so copy the
        // slice into a stack buffer and NUL-terminate.
        char tmp[BUFFER_CAP + 1];
        if (line_bytes > 0) {
            memcpy(tmp, g_buf.bytes + line_from, line_bytes);
            tmp[line_bytes] = '\0';
            cairo_move_to(cr, MARGIN_X, y);
            cairo_show_text(cr, tmp);
        }

        // If the cursor is on this line, measure the substring up to
        // the cursor to place the bar. We don't draw the bar yet —
        // batching the cairo_rectangle + fill below keeps the font
        // state from shifting mid-line.
        if (!cursor_placed
                && g_buf.cursor >= line_from
                && g_buf.cursor <= line_from + line_bytes) {
            size_t col_bytes = g_buf.cursor - line_from;
            if (col_bytes > 0) {
                memcpy(tmp, g_buf.bytes + line_from, col_bytes);
                tmp[col_bytes] = '\0';
                cairo_text_extents_t ext;
                cairo_text_extents(cr, tmp, &ext);
                cursor_x = MARGIN_X + ext.x_advance;
            } else {
                cursor_x = MARGIN_X;
            }
            cursor_line    = line_index;
            cursor_y_top   = y - FONT_SIZE + 1; // slight top bias
            cursor_placed  = true;
        }

        y += LINE_HEIGHT;
        line_index++;
        if (j >= g_buf.len) break;
        i = j + 1;
    }

    if (!cursor_placed) {
        // Empty buffer — cursor at the start of the first line.
        cursor_x      = MARGIN_X;
        cursor_y_top  = MARGIN_Y + 1;
        cursor_line   = 0;
    }

    if (g_has_focus) {
        cairo_set_source_rgb(cr, 0x1A / 255.0, 0x1A / 255.0, 0x1A / 255.0);
        cairo_rectangle(cr,
                        cursor_x,
                        cursor_y_top,
                        CURSOR_WIDTH,
                        FONT_SIZE);
        cairo_fill(cr);
    }

    // Silence -Wunused warnings if width/height aren't consumed (we
    // intentionally don't clip yet — v0.1 scope, no scrolling).
    (void)width;
    (void)height;
    (void)cursor_line;

    moonbase_window_commit(w);
}

// ---------------------------------------------------------------------
// main — standard app-owns-loop pattern. moonbase_run() is still
// ENOSYS in this framework slice (same as hello.appc); we drive the
// loop ourselves and call paint() after any mutation so the surface
// reflects the buffer immediately, not on the next REDRAW event.
// ---------------------------------------------------------------------
int main(int argc, char **argv)
{
    int rc = moonbase_init(argc, argv);
    if (rc != MB_EOK) {
        fprintf(stderr,
                "textedit: moonbase_init failed: %s\n",
                moonbase_error_string((mb_error_t)rc));
        return 1;
    }

    mb_window_desc_t desc = {
        .version       = MOONBASE_WINDOW_DESC_VERSION,
        .title         = "Untitled",
        .width_points  = 560,
        .height_points = 400,
        .render_mode   = MOONBASE_RENDER_CAIRO,
        .flags         = MB_WINDOW_FLAG_CENTER,
    };

    mb_window_t *w = moonbase_window_create(&desc);
    if (!w) {
        fprintf(stderr,
                "textedit: moonbase_window_create failed: %s\n",
                moonbase_error_string(moonbase_last_error()));
        return 1;
    }

    // Initial paint. Subsequent paints come from either REDRAW events
    // or any mutation in our key / text-input handling below.
    paint(w);

    int running   = 1;
    int exit_code = 0;

    while (running) {
        mb_event_t ev;
        int wr = moonbase_wait_event(&ev, -1);
        if (wr < 0) {
            fprintf(stderr,
                    "textedit: wait_event error: %s\n",
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

        case MB_EV_WINDOW_FOCUSED:
            g_has_focus = ev.focus.has_focus;
            dirty = true;
            break;

        case MB_EV_WINDOW_CLOSED:
        case MB_EV_APP_WILL_QUIT:
            running = 0;
            break;

        case MB_EV_TEXT_INPUT:
            if (ev.text.text && ev.text.text[0] != '\0') {
                buffer_insert(&g_buf, ev.text.text, strlen(ev.text.text));
                dirty = true;
            }
            break;

        case MB_EV_KEY_DOWN:
            // Cmd-Q takes priority over everything else.
            if ((ev.key.modifiers & MB_MOD_COMMAND) &&
                (ev.key.keycode == 'q' || ev.key.keycode == 'Q')) {
                running = 0;
                break;
            }
            // Ignore any Command-held keystroke — they're menu
            // shortcuts, and we don't own a menu yet.
            if (ev.key.modifiers & MB_MOD_COMMAND) break;

            switch (ev.key.keycode) {
            case KS_BACKSPACE:
                buffer_delete_before(&g_buf);
                dirty = true;
                break;
            case KS_DELETE:
                buffer_delete_after(&g_buf);
                dirty = true;
                break;
            case KS_LEFT:
                if (g_buf.cursor > 0) { g_buf.cursor--; dirty = true; }
                break;
            case KS_RIGHT:
                if (g_buf.cursor < g_buf.len) {
                    g_buf.cursor++;
                    dirty = true;
                }
                break;
            case KS_UP:
                cursor_vertical(&g_buf, -1);
                dirty = true;
                break;
            case KS_DOWN:
                cursor_vertical(&g_buf, +1);
                dirty = true;
                break;
            case KS_HOME:
                g_buf.cursor = line_start(&g_buf, g_buf.cursor);
                dirty = true;
                break;
            case KS_END:
                g_buf.cursor = line_end(&g_buf, g_buf.cursor);
                dirty = true;
                break;
            case KS_RETURN: {
                char nl = '\n';
                buffer_insert(&g_buf, &nl, 1);
                dirty = true;
                break;
            }
            default:
                // Non-printable keys we don't consume fall through with
                // no mutation. TEXT_INPUT handles printable ASCII.
                break;
            }
            break;

        default:
            break;
        }

        if (dirty) paint(w);
    }

    moonbase_window_close(w);
    return exit_code;
}
