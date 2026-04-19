// CopyCatOS — by Kyle Blizzard at Blizzard.show

// activity-monitor — reference .appc 4 of 4 for the libmoonbase.so.1
// public-SDK freeze. Exercises the three ABI surfaces CLAUDE.md calls
// out for this slot:
//
//   1. Long-lived periodic sampling. The loop uses moonbase_wait_event
//      with a 1000ms timeout and refreshes the process table on every
//      timeout tick. No worker thread, no moonbase_dispatch_main —
//      that shape gets exercised elsewhere. The single-threaded
//      timer-in-wait pattern is the one most apps will reach for.
//
//   2. List views. MoonBase does not ship a list-view widget in v1 —
//      and will not ship one in v1 — so the UI paints rows directly
//      with Cairo: a header row, zebra stripes, a blue selection pill,
//      a right-aligned column layout. Pointer-down hit-tests Y to
//      select a row; Up / Down walk the list. When a later moonbase-
//      list companion header does land, this app migrates to it; until
//      then, Cairo rows are a clean way to prove the ABI doesn't
//      require a widget library.
//
//   3. Privileged data access behind an entitlement. Info.appc
//      declares system = ["process-list"]. /proc on Linux is already
//      world-readable so v0.1 does not enforce anything at runtime —
//      the entitlement is the *declaration* that will feed the consent
//      sheet and SysPrefs → Security & Privacy once the enforcement
//      layer ships. Shipping the declaration now pins the schema so
//      the later plumbing finds it already in place.
//
// Scope of v0.1 — deliberately narrow:
//   • single window, one table, no tabs (no Energy / Disk / Network
//     tabs — each wants its own sampler and own column set)
//   • sort is fixed: CPU% descending. No column click-to-sort.
//   • memory column is RSS only (no swap, compressed, private). RSS
//     is what `top` defaults to and is the number the user can act on.
//   • no kill / inspect / signal flow. This v0.1 is read-only.
//   • refresh cadence is fixed at 1s. No preference.
//
// Keyboard:
//   Up / Down   — move selection (and scroll to keep it visible)
//   Cmd-Q       — quit
//
// Everything measures in points; MoonBase scales the Cairo surface.

#include <moonbase.h>

#include <cairo/cairo.h>

#include <dirent.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------------
// X11 keysyms.
// ---------------------------------------------------------------------
#define KS_UP    0xFF52
#define KS_DOWN  0xFF54

// ---------------------------------------------------------------------
// Visual constants (points).
// ---------------------------------------------------------------------
#define WIN_W           720
#define WIN_H           440
#define HEADER_H        24
#define ROW_H           18
#define STATUS_H        22
#define MARGIN_X        10
#define FONT_SIZE_BODY  11
#define FONT_SIZE_HEAD  11

// Column x-offsets (left edge) and widths. Layout is right-aligned
// numbers (PID / CPU / MEM) and left-aligned Name in the flex column.
#define COL_PID_X       10
#define COL_PID_W       60
#define COL_NAME_X      80
#define COL_CPU_W       60
#define COL_MEM_W       96

// Refresh cadence.
#define REFRESH_MS      1000

// Max processes we track per sample. Real systems on a Legion Go S
// rarely exceed ~400 during heavy workloads; 1024 covers kernel threads
// too.
#define MAX_PROCS       1024

// ---------------------------------------------------------------------
// Per-process sample.
// ---------------------------------------------------------------------
typedef struct {
    int          pid;
    char         name[32];      // /proc/<pid>/stat comm, parens stripped
    unsigned long total_ticks;  // utime + stime at sample time
    long         rss_kb;        // VmRSS from /proc/<pid>/status
    double       cpu_percent;   // computed from delta against previous
    bool         alive;         // cleared each sample, set on observation
} proc_row_t;

static proc_row_t  g_prev[MAX_PROCS];
static int         g_prev_count = 0;
static struct timespec g_prev_time;
static bool        g_have_prev = false;

static proc_row_t  g_rows[MAX_PROCS];
static int         g_row_count = 0;

// UI state.
static int  g_selected = -1;   // index into g_rows after sort, or -1
static int  g_scroll   = 0;    // row index at top of visible area
static long g_tick_hz  = 100;  // CLK_TCK, cached at init

// ---------------------------------------------------------------------
// /proc parsing helpers.
// ---------------------------------------------------------------------

// Pull "comm" (field 2) and utime+stime (fields 14+15) out of
// /proc/<pid>/stat. comm is wrapped in ()s and may contain spaces, so
// find the last ')' rather than splitting on whitespace.
static bool read_stat(int pid, proc_row_t *out) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return false;
    buf[n] = '\0';

    char *rp = strrchr(buf, ')');
    if (!rp) return false;
    char *lp = strchr(buf, '(');
    if (!lp || lp >= rp) return false;

    // Comm — strip parens, truncate to buffer.
    size_t comm_len = (size_t)(rp - lp - 1);
    if (comm_len >= sizeof out->name) comm_len = sizeof out->name - 1;
    memcpy(out->name, lp + 1, comm_len);
    out->name[comm_len] = '\0';

    // Tail starts one past ')'; fields 3..n are space-separated. utime
    // is field 14, stime is field 15 — i.e. indices 11, 12 after the
    // state field at index 0 in the tail. Skip the state char, then
    // pass 10 fields, then read utime + stime.
    char *p = rp + 2; // skip ") "
    if (*p == '\0') return false;

    unsigned long utime = 0, stime = 0;
    int matched = sscanf(p,
        "%*c "                // state
        "%*d %*d %*d %*d %*d "  // ppid, pgrp, session, tty_nr, tpgid
        "%*u %*u %*u %*u %*u "  // flags, minflt, cminflt, majflt, cmajflt
        "%lu %lu",              // utime, stime
        &utime, &stime);
    if (matched < 2) return false;

    out->pid = pid;
    out->total_ticks = utime + stime;
    return true;
}

// /proc/<pid>/status is line-oriented. Grab VmRSS — kilobytes.
static long read_rss_kb(int pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[256];
    long rss = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            rss = strtol(line + 6, NULL, 10);
            break;
        }
    }
    fclose(f);
    return rss;
}

static unsigned long long ts_to_ms(const struct timespec *t) {
    return (unsigned long long)t->tv_sec * 1000ULL
         + (unsigned long long)t->tv_nsec / 1000000ULL;
}

// Look up a pid in the previous sample to find its prior total_ticks.
// Returns NULL if the pid is new since last sample.
static const proc_row_t *find_prev(int pid) {
    for (int i = 0; i < g_prev_count; i++) {
        if (g_prev[i].pid == pid) return &g_prev[i];
    }
    return NULL;
}

// Sort g_rows[] by cpu_percent desc — stable isn't required but a
// simple qsort-with-tiebreak-on-pid keeps row ordering predictable
// when many processes sit at 0%.
static int cmp_cpu_desc(const void *a, const void *b) {
    const proc_row_t *ra = a, *rb = b;
    if (ra->cpu_percent > rb->cpu_percent) return -1;
    if (ra->cpu_percent < rb->cpu_percent) return  1;
    return ra->pid - rb->pid;
}

// Walk /proc/<numeric>/ and rebuild g_rows[]. Compute CPU delta against
// the prior sample. Retain the selection across refreshes by pid, not
// by row index — index shifts every time the sort order changes.
static void sample_procs(void) {
    int selected_pid = (g_selected >= 0 && g_selected < g_row_count)
                       ? g_rows[g_selected].pid : -1;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    DIR *d = opendir("/proc");
    if (!d) return;

    g_row_count = 0;
    struct dirent *e;
    while ((e = readdir(d)) && g_row_count < MAX_PROCS) {
        // Only numeric entries are pids.
        const char *p = e->d_name;
        if (!isdigit((unsigned char)*p)) continue;
        int pid = atoi(p);
        if (pid <= 0) continue;

        proc_row_t row;
        memset(&row, 0, sizeof row);
        if (!read_stat(pid, &row)) continue;
        row.rss_kb = read_rss_kb(pid);

        // CPU% = (delta_proc_ticks / delta_wall_ticks) * 100. Wall-clock
        // delta is derived from (ms * tick_hz) / 1000. Not normalized
        // to cores — we mirror top's default behavior, so a process
        // maxing N cores reads 100*N.
        row.cpu_percent = 0.0;
        if (g_have_prev) {
            const proc_row_t *prev = find_prev(pid);
            if (prev) {
                unsigned long delta_p = row.total_ticks - prev->total_ticks;
                unsigned long long dt_ms =
                    ts_to_ms(&now) - ts_to_ms(&g_prev_time);
                if (dt_ms > 0) {
                    double wall_ticks = (double)dt_ms
                                      * (double)g_tick_hz / 1000.0;
                    if (wall_ticks > 0) {
                        row.cpu_percent =
                            ((double)delta_p / wall_ticks) * 100.0;
                    }
                }
            }
        }

        row.alive = true;
        g_rows[g_row_count++] = row;
    }
    closedir(d);

    // Sort by CPU desc, then by pid.
    qsort(g_rows, (size_t)g_row_count, sizeof g_rows[0], cmp_cpu_desc);

    // Preserve selection across refresh by re-finding the prior pid.
    if (selected_pid > 0) {
        g_selected = -1;
        for (int i = 0; i < g_row_count; i++) {
            if (g_rows[i].pid == selected_pid) { g_selected = i; break; }
        }
    }

    // Copy current sample into prev snapshot for next delta.
    memcpy(g_prev, g_rows, sizeof g_rows[0] * (size_t)g_row_count);
    g_prev_count = g_row_count;
    g_prev_time  = now;
    g_have_prev  = true;
}

// ---------------------------------------------------------------------
// Row visibility + scroll clamping.
// ---------------------------------------------------------------------

static int visible_rows(int window_h) {
    int body = window_h - HEADER_H - STATUS_H;
    int n = body / ROW_H;
    return n < 0 ? 0 : n;
}

static void ensure_selected_visible(int window_h) {
    if (g_selected < 0) return;
    int vis = visible_rows(window_h);
    if (vis <= 0) return;
    if (g_selected < g_scroll) g_scroll = g_selected;
    else if (g_selected >= g_scroll + vis) g_scroll = g_selected - vis + 1;
    if (g_scroll < 0) g_scroll = 0;
    int max_scroll = g_row_count - vis;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
}

// ---------------------------------------------------------------------
// Cairo helpers — right-align a string inside a column.
// ---------------------------------------------------------------------

static void text_right(cairo_t *cr, double right_x, double y,
                       const char *s) {
    cairo_text_extents_t te;
    cairo_text_extents(cr, s, &te);
    cairo_move_to(cr, right_x - te.width - te.x_bearing, y);
    cairo_show_text(cr, s);
}

static void format_mem(long kb, char *out, size_t n) {
    if (kb >= 1024 * 1024)
        snprintf(out, n, "%.1f GB", (double)kb / (1024.0 * 1024.0));
    else if (kb >= 1024)
        snprintf(out, n, "%.1f MB", (double)kb / 1024.0);
    else
        snprintf(out, n, "%ld KB", kb);
}

// ---------------------------------------------------------------------
// Paint.
// ---------------------------------------------------------------------

static void paint_header(cairo_t *cr, int width) {
    // Aqua window background for the header strip.
    cairo_set_source_rgb(cr,
                         0xEC / 255.0, 0xEC / 255.0, 0xEC / 255.0);
    cairo_rectangle(cr, 0, 0, width, HEADER_H);
    cairo_fill(cr);

    // Bottom border.
    cairo_set_source_rgb(cr,
                         0xA8 / 255.0, 0xA8 / 255.0, 0xA8 / 255.0);
    cairo_rectangle(cr, 0, HEADER_H - 1, width, 1);
    cairo_fill(cr);

    cairo_select_font_face(cr, "Lucida Grande",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, FONT_SIZE_HEAD);
    cairo_set_source_rgb(cr,
                         0x1A / 255.0, 0x1A / 255.0, 0x1A / 255.0);

    double y = HEADER_H - 7;

    // PID — right-aligned inside its column.
    text_right(cr, COL_PID_X + COL_PID_W, y, "PID");

    // Process Name — left-aligned.
    cairo_move_to(cr, COL_NAME_X, y);
    cairo_show_text(cr, "Process Name");

    // %CPU — right-aligned.
    double cpu_right = width - MARGIN_X - COL_MEM_W - 8;
    text_right(cr, cpu_right, y, "% CPU");

    // Memory — right-aligned at the far right.
    text_right(cr, width - MARGIN_X, y, "Memory");
}

static void paint_rows(cairo_t *cr, int width, int height) {
    // Content background — white.
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_rectangle(cr, 0, HEADER_H, width, height - HEADER_H - STATUS_H);
    cairo_fill(cr);

    cairo_select_font_face(cr, "Lucida Grande",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, FONT_SIZE_BODY);

    int vis = visible_rows(height);
    int end = g_scroll + vis;
    if (end > g_row_count) end = g_row_count;

    for (int i = g_scroll; i < end; i++) {
        int ri = i - g_scroll;
        double row_y_top = HEADER_H + ri * ROW_H;
        double text_y    = row_y_top + ROW_H - 5;

        // Zebra stripe on alternating rows, then selection pill above.
        if (i == g_selected) {
            cairo_set_source_rgb(cr,
                                 0x38 / 255.0, 0x75 / 255.0, 0xD7 / 255.0);
            cairo_rectangle(cr, 0, row_y_top, width, ROW_H);
            cairo_fill(cr);
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        } else {
            if (ri & 1) {
                cairo_set_source_rgb(cr,
                                     0xF5 / 255.0, 0xF5 / 255.0, 0xF5 / 255.0);
                cairo_rectangle(cr, 0, row_y_top, width, ROW_H);
                cairo_fill(cr);
            }
            cairo_set_source_rgb(cr,
                                 0x1A / 255.0, 0x1A / 255.0, 0x1A / 255.0);
        }

        char buf[64];

        snprintf(buf, sizeof buf, "%d", g_rows[i].pid);
        text_right(cr, COL_PID_X + COL_PID_W, text_y, buf);

        cairo_move_to(cr, COL_NAME_X, text_y);
        cairo_show_text(cr, g_rows[i].name);

        snprintf(buf, sizeof buf, "%.1f", g_rows[i].cpu_percent);
        double cpu_right = width - MARGIN_X - COL_MEM_W - 8;
        text_right(cr, cpu_right, text_y, buf);

        format_mem(g_rows[i].rss_kb, buf, sizeof buf);
        text_right(cr, width - MARGIN_X, text_y, buf);
    }
}

static void paint_status(cairo_t *cr, int width, int height) {
    double y_top = height - STATUS_H;

    cairo_set_source_rgb(cr,
                         0xEC / 255.0, 0xEC / 255.0, 0xEC / 255.0);
    cairo_rectangle(cr, 0, y_top, width, STATUS_H);
    cairo_fill(cr);

    cairo_set_source_rgb(cr,
                         0xA8 / 255.0, 0xA8 / 255.0, 0xA8 / 255.0);
    cairo_rectangle(cr, 0, y_top, width, 1);
    cairo_fill(cr);

    cairo_select_font_face(cr, "Lucida Grande",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, FONT_SIZE_BODY);
    cairo_set_source_rgb(cr,
                         0x1A / 255.0, 0x1A / 255.0, 0x1A / 255.0);

    char buf[96];
    snprintf(buf, sizeof buf, "%d processes  ·  sampled every %.1fs",
             g_row_count, REFRESH_MS / 1000.0);
    cairo_move_to(cr, MARGIN_X, y_top + STATUS_H - 6);
    cairo_show_text(cr, buf);
}

static void paint(mb_window_t *w) {
    cairo_t *cr = (cairo_t *)moonbase_window_cairo(w);
    if (!cr) return;

    int width = 0, height = 0;
    moonbase_window_size(w, &width, &height);

    paint_header(cr, width);
    paint_rows  (cr, width, height);
    paint_status(cr, width, height);

    moonbase_window_commit(w);
}

// ---------------------------------------------------------------------
// Hit-testing — pointer Y inside the rows area → row index.
// ---------------------------------------------------------------------
static int hit_test_row(int y, int window_h) {
    if (y < HEADER_H) return -1;
    if (y >= window_h - STATUS_H) return -1;
    int ri = (y - HEADER_H) / ROW_H;
    int idx = g_scroll + ri;
    if (idx < 0 || idx >= g_row_count) return -1;
    return idx;
}

// ---------------------------------------------------------------------
// main.
// ---------------------------------------------------------------------

int main(int argc, char **argv) {
    int rc = moonbase_init(argc, argv);
    if (rc != MB_EOK) {
        fprintf(stderr,
                "activity-monitor: moonbase_init failed: %s\n",
                moonbase_error_string((mb_error_t)rc));
        return 1;
    }

    long hz = sysconf(_SC_CLK_TCK);
    if (hz > 0) g_tick_hz = hz;

    mb_window_desc_t desc = {
        .version       = MOONBASE_WINDOW_DESC_VERSION,
        .title         = "Activity Monitor",
        .width_points  = WIN_W,
        .height_points = WIN_H,
        .render_mode   = MOONBASE_RENDER_CAIRO,
        .flags         = MB_WINDOW_FLAG_CENTER,
    };

    mb_window_t *w = moonbase_window_create(&desc);
    if (!w) {
        fprintf(stderr,
                "activity-monitor: window_create failed: %s\n",
                moonbase_error_string(moonbase_last_error()));
        return 1;
    }

    // First sample. CPU% will read 0.0 everywhere until the second
    // sample gives us a delta — matches macOS Activity Monitor's
    // one-second warmup.
    sample_procs();
    if (g_row_count > 0) g_selected = 0;
    paint(w);

    int running = 1;
    int exit_code = 0;

    while (running) {
        mb_event_t ev;
        int wr = moonbase_wait_event(&ev, REFRESH_MS);
        if (wr < 0) {
            fprintf(stderr,
                    "activity-monitor: wait_event error: %s\n",
                    moonbase_error_string((mb_error_t)wr));
            exit_code = 1;
            break;
        }

        bool dirty = false;
        int win_w = 0, win_h = 0;
        moonbase_window_size(w, &win_w, &win_h);

        if (wr == 0) {
            // Timeout tick — refresh the table.
            sample_procs();
            ensure_selected_visible(win_h);
            dirty = true;
        } else {
            switch (ev.kind) {
            case MB_EV_WINDOW_REDRAW:
                dirty = true;
                break;

            case MB_EV_WINDOW_RESIZED:
                ensure_selected_visible(ev.resize.new_height);
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

                if (!cmd && ev.key.keycode == KS_UP && g_row_count > 0) {
                    if (g_selected > 0) g_selected--;
                    else if (g_selected < 0) g_selected = 0;
                    ensure_selected_visible(win_h);
                    dirty = true;
                } else if (!cmd && ev.key.keycode == KS_DOWN
                        && g_row_count > 0) {
                    if (g_selected < 0) g_selected = 0;
                    else if (g_selected + 1 < g_row_count) g_selected++;
                    ensure_selected_visible(win_h);
                    dirty = true;
                }
                break;
            }

            case MB_EV_POINTER_DOWN: {
                int idx = hit_test_row(ev.pointer.y, win_h);
                if (idx >= 0) {
                    g_selected = idx;
                    dirty = true;
                }
                break;
            }

            default:
                break;
            }
        }

        if (dirty) paint(w);
    }

    moonbase_window_close(w);
    return exit_code;
}
