// CopyCatOS — by Kyle Blizzard at Blizzard.show

// leakcheck-loop.c — Phase C plumbing-tasks line 62.
//
// Runs 1000 full client lifecycles in a single process and asserts that
// neither the heap nor the open-fd count grew from the start of the
// loop to the end. This catches any allocation, mmap, or fd that
// moonbase_init / moonbase_window_create / moonbase_window_commit /
// moonbase_window_close / moonbase_quit fails to release on the way
// out. Running 1000 of them in one process is a stronger leak check
// than 1000 separate processes would be — each process exit reaps all
// memory regardless of leaks, but a long-running process surfaces any
// accumulating allocation linearly.
//
// Why not valgrind? Valgrind isn't on the Legion, the machine where
// CopyCatOS actually builds, and we can't install it non-interactively.
// LeakSanitizer has the same constraint. mallinfo2 + /proc/self/fd
// counting, though coarser, catches the exact classes of leak we care
// about (malloc/mmap bytes, fd count) with zero install dependency and
// runs in well under a second.
//
// Parent:
//   Stands up an mb_server_t, accepts sequential connections from the
//   child, replies to WINDOW_CREATE with a canned reply, and on each
//   WINDOW_COMMIT closes the received fd and emits WINDOW_FOCUSED so
//   the child has something to pump before it re-enters the loop.
//
// Child:
//   1. moonbase_init
//   2. moonbase_window_create (640x400 points, Cairo)
//   3. moonbase_window_cairo — paint one row of red so cairo's
//      internal state walks normal paths (premul encode, damage, etc.)
//   4. moonbase_window_commit
//   5. moonbase_window_close
//   6. moonbase_quit
//   ... × N iterations. Measures mallinfo2().uordblks and fd count at
//   iteration WARMUP and at iteration N; both deltas must stay at or
//   near zero.

#include "moonbase.h"
#include "../src/server/server.h"
#include "../src/ipc/cbor.h"
#include "moonbase/ipc/kinds.h"

#include <cairo/cairo.h>
#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FAIL(...) do { \
    fprintf(stderr, "FAIL: " __VA_ARGS__); \
    fputc('\n', stderr); \
    exit(1); \
} while (0)

#define EXPECTED_WINDOW_ID   0x7777u
#define EXPECTED_OUTPUT_ID   0x0001u
#define EXPECTED_SCALE       1.0
#define REQUESTED_W          640
#define REQUESTED_H          400

// Iteration counts. WARMUP bleeds off first-launch one-shot allocations
// (glibc arena reservations, lazy-binding caches, cairo's font-face
// cache) before the first measurement so we're comparing steady state
// against steady state. ITERATIONS is the headline "1000-launch" value.
#define WARMUP          10
#define ITERATIONS      1000

static int g_parent_commit_count = 0;

// ---------------------------------------------------------------------
// Parent-side server callback
// ---------------------------------------------------------------------

static uint8_t *build_reply(uint32_t window_id, uint32_t output_id,
                            double scale, uint32_t w, uint32_t h,
                            size_t *out_len) {
    mb_cbor_w_t wr;
    mb_cbor_w_init_grow(&wr, 48);
    mb_cbor_w_map_begin(&wr, 5);
    mb_cbor_w_key(&wr, 1); mb_cbor_w_uint (&wr, window_id);
    mb_cbor_w_key(&wr, 2); mb_cbor_w_uint (&wr, output_id);
    mb_cbor_w_key(&wr, 3); mb_cbor_w_float(&wr, scale);
    mb_cbor_w_key(&wr, 4); mb_cbor_w_uint (&wr, w);
    mb_cbor_w_key(&wr, 5); mb_cbor_w_uint (&wr, h);
    if (!mb_cbor_w_ok(&wr)) { mb_cbor_w_drop(&wr); return NULL; }
    return mb_cbor_w_finish(&wr, out_len);
}

static void on_event(mb_server_t *s, const mb_server_event_t *ev, void *user) {
    (void)user;
    switch (ev->kind) {
        case MB_SERVER_EV_CONNECTED:
            break;
        case MB_SERVER_EV_FRAME:
            if (ev->frame_kind == MB_IPC_WINDOW_CREATE) {
                size_t body_len = 0;
                uint8_t *body = build_reply(EXPECTED_WINDOW_ID,
                                            EXPECTED_OUTPUT_ID,
                                            EXPECTED_SCALE,
                                            (uint32_t)REQUESTED_W,
                                            (uint32_t)REQUESTED_H,
                                            &body_len);
                if (!body) return;
                (void)mb_server_send(s, ev->client,
                                     MB_IPC_WINDOW_CREATE_REPLY,
                                     body, body_len, NULL, 0);
                free(body);
            } else if (ev->frame_kind == MB_IPC_WINDOW_COMMIT) {
                g_parent_commit_count++;
                // The server closes fds after the callback returns, but
                // only fds it still owns. If we wanted to keep one we'd
                // dup here — we don't, so leave them alone.
            }
            break;
        case MB_SERVER_EV_DISCONNECTED:
            break;
    }
}

// ---------------------------------------------------------------------
// Child-side measurement helpers
// ---------------------------------------------------------------------

// Currently allocated heap bytes (glibc-private). Runs malloc_trim
// first so arena caching doesn't mask actual retained allocations.
static size_t heap_bytes(void) {
    malloc_trim(0);
    struct mallinfo2 mi = mallinfo2();
    return (size_t)mi.uordblks;
}

// Count entries in /proc/self/fd. Excludes the readdir's own fd by
// filtering out the dirfd we just opened. Catches raw fd leaks, shm
// memfd leaks, and socket leaks in one number.
static int open_fd_count(void) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    int dirfd_self = dirfd(d);
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        int fd = atoi(e->d_name);
        if (fd == dirfd_self) continue;
        count++;
    }
    closedir(d);
    return count;
}

// One full lifecycle iteration. Returns 0 on success, non-zero exit
// code on any failure. No asserts — the caller compares heap/fd deltas
// across iterations, so any failure here is a hard bug to diagnose, not
// something to paper over with FAIL().
static int one_lifecycle(void) {
    if (moonbase_init(0, NULL) != 0) {
        fprintf(stderr, "init failed, err=%d\n", moonbase_last_error());
        return 30;
    }

    mb_window_desc_t desc = {
        .version       = MOONBASE_WINDOW_DESC_VERSION,
        .title         = "leak-test",
        .width_points  = REQUESTED_W,
        .height_points = REQUESTED_H,
        .render_mode   = MOONBASE_RENDER_CAIRO,
        .flags         = 0,
    };
    mb_window_t *win = moonbase_window_create(&desc);
    if (!win) {
        fprintf(stderr, "window_create failed, err=%d\n", moonbase_last_error());
        moonbase_quit(0);
        return 31;
    }

    // Paint one row so Cairo actually allocates + touches the buffer.
    cairo_t *cr = (cairo_t *)moonbase_window_cairo(win);
    if (!cr) {
        fprintf(stderr, "window_cairo returned NULL, err=%d\n",
                moonbase_last_error());
        moonbase_window_close(win);
        moonbase_quit(0);
        return 32;
    }
    cairo_set_source_rgb(cr, 1.0, 0.0, 0.0);
    cairo_rectangle(cr, 0, 0, REQUESTED_W, 1);
    cairo_fill(cr);

    if (moonbase_window_commit(win) != 0) {
        fprintf(stderr, "window_commit failed, err=%d\n", moonbase_last_error());
        moonbase_window_close(win);
        moonbase_quit(0);
        return 33;
    }

    moonbase_window_close(win);
    moonbase_quit(0);
    return 0;
}

// ---------------------------------------------------------------------
// Child entry point
// ---------------------------------------------------------------------

static int run_child(const char *sock_dir) {
    if (setenv("XDG_RUNTIME_DIR", sock_dir, 1) != 0) return 10;
    if (setenv("MOONBASE_BUNDLE_ID", "show.blizzard.leaktest", 1) != 0) return 11;
    if (setenv("MOONBASE_BUNDLE_VERSION", "0.1.0", 1) != 0) return 12;

    // Warmup: first few cycles allocate glibc arena pages, cairo font
    // state, libpthread TLS, etc. — one-time costs we don't want
    // counted as leaks.
    for (int i = 0; i < WARMUP; i++) {
        int rc = one_lifecycle();
        if (rc != 0) return rc;
    }

    size_t heap_before = heap_bytes();
    int    fds_before  = open_fd_count();

    for (int i = 0; i < ITERATIONS; i++) {
        int rc = one_lifecycle();
        if (rc != 0) {
            fprintf(stderr, "iteration %d failed rc=%d\n", i, rc);
            return rc;
        }
    }

    size_t heap_after = heap_bytes();
    int    fds_after  = open_fd_count();

    long heap_delta = (long)heap_after - (long)heap_before;
    int  fd_delta   = fds_after - fds_before;

    // Generous heap threshold — glibc arena bookkeeping can jitter by a
    // few hundred bytes across long runs without any real leak. Anything
    // in the KB range across 1000 iterations would be a bona fide drip.
    // A real single-byte leak per iteration would show up as >=1000 B.
    const long HEAP_MAX_DELTA = 1024;

    fprintf(stderr,
            "leak-check: %d iterations, heap %zu -> %zu (Δ %ld B), "
            "fds %d -> %d (Δ %d)\n",
            ITERATIONS, heap_before, heap_after, heap_delta,
            fds_before, fds_after, fd_delta);

    if (fd_delta != 0) {
        fprintf(stderr, "FAIL: fd count changed (%d new fds)\n", fd_delta);
        return 40;
    }
    if (heap_delta > HEAP_MAX_DELTA) {
        fprintf(stderr, "FAIL: heap grew by %ld B across %d iterations "
                "(threshold %ld B)\n",
                heap_delta, ITERATIONS, HEAP_MAX_DELTA);
        return 41;
    }

    return 0;
}

// ---------------------------------------------------------------------
// Parent entry point
// ---------------------------------------------------------------------

int main(void) {
    char dir_tmpl[] = "/tmp/mb-leak.XXXXXX";
    char *dir = mkdtemp(dir_tmpl);
    if (!dir) FAIL("mkdtemp: %s", strerror(errno));

    char sock_path[256];
    snprintf(sock_path, sizeof(sock_path), "%s/moonbase.sock", dir);

    mb_server_t *srv = NULL;
    int rc = mb_server_open(&srv, sock_path, on_event, NULL);
    if (rc != 0) FAIL("mb_server_open rc=%d", rc);

    pid_t pid = fork();
    if (pid < 0) FAIL("fork: %s", strerror(errno));
    if (pid == 0) {
        int r = run_child(dir);
        mb_server_close(srv);
        _exit(r);
    }

    // Service the socket until the child exits. Poll with a tight
    // budget so we keep pumping through each iteration's brief ramp.
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const long timeout_ms = 60 * 1000;   // generous — 1000 iterations is cheap

    for (;;) {
        int wr = waitpid(pid, &rc, WNOHANG);
        if (wr == pid) break;
        if (wr < 0 && errno != EINTR) FAIL("waitpid: %s", strerror(errno));

        struct pollfd fds[16];
        size_t n = mb_server_get_pollfds(srv, fds, 16);
        int pr = poll(fds, (nfds_t)n, 10);
        if (pr < 0 && errno != EINTR) FAIL("poll: %s", strerror(errno));
        mb_server_tick(srv, fds, n);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000
                     + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed > timeout_ms) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            FAIL("timeout after %ld ms, parent commits=%d",
                 elapsed, g_parent_commit_count);
        }
    }

    if (!WIFEXITED(rc) || WEXITSTATUS(rc) != 0) {
        FAIL("child exited status=%d (exit=%d), parent commits=%d",
             rc, WEXITSTATUS(rc), g_parent_commit_count);
    }

    mb_server_close(srv);
    unlink(sock_path);
    rmdir(dir);

    printf("ok: %d lifecycles, heap + fd count stable, "
           "parent saw %d commits\n",
           ITERATIONS, g_parent_commit_count);
    return 0;
}
