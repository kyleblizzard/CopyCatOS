// CopyCatOS — by Kyle Blizzard at Blizzard.show

// window-commit-burst.c — regression test for MB_EPROTO on the second
// commit.
//
// The bug: the client sends each WINDOW_COMMIT as two writes — a
// sendmsg carrying [6-byte header + SCM_RIGHTS] and a plain write
// carrying the body. When two commits are fired back-to-back, the
// Linux AF_UNIX stream socket can coalesce frame1's body-skb (no scm)
// with frame2's header-skb (with scm) into a single recvmsg on the
// server. If the server tags the fd batch with PRE-APPEND rx_received,
// the tag lands mid-way through frame1's body — and drain_rx either
// attaches frame2's fds to frame1 (silent corruption) or returns
// MB_EPROTO because the offset is < rx_consumed. Either way the
// second commit blows up.
//
// The fix: server tags with POST-APPEND rx_received, which equals
// end-of-scm-skb by Linux AF_UNIX invariant (the kernel breaks the
// recv after consuming an scm-bearing skb). drain_rx then matches
// via a range check against the current frame's byte span.
//
// This test fires two commits in rapid succession from the child and
// asserts both arrive, both carry exactly one fd, and the connection
// never disconnects with an error mid-stream.

#include "moonbase.h"
#include "../src/server/server.h"
#include "../src/ipc/cbor.h"
#include "moonbase/ipc/kinds.h"

#include <cairo/cairo.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FAIL(...) do { \
    fprintf(stderr, "FAIL: " __VA_ARGS__); \
    fputc('\n', stderr); \
    exit(1); \
} while (0)

#define EXPECTED_WINDOW_ID 0x4321u
#define EXPECTED_OUTPUT_ID 0x8765u
#define EXPECTED_SCALE     1.0
#define REQUESTED_W        320
#define REQUESTED_H        200
#define EXPECTED_COMMITS   2

static int g_connected       = 0;
static int g_disconnected    = 0;
static int g_create_seen     = 0;
static int g_close_seen      = 0;
static int g_commit_seen     = 0;
static int g_commit_missing_fd = 0;

static uint8_t *build_create_reply(size_t *out_len) {
    mb_cbor_w_t wr;
    mb_cbor_w_init_grow(&wr, 48);
    mb_cbor_w_map_begin(&wr, 5);
    mb_cbor_w_key(&wr, 1); mb_cbor_w_uint (&wr, EXPECTED_WINDOW_ID);
    mb_cbor_w_key(&wr, 2); mb_cbor_w_uint (&wr, EXPECTED_OUTPUT_ID);
    mb_cbor_w_key(&wr, 3); mb_cbor_w_float(&wr, EXPECTED_SCALE);
    mb_cbor_w_key(&wr, 4); mb_cbor_w_uint (&wr, (uint32_t)REQUESTED_W);
    mb_cbor_w_key(&wr, 5); mb_cbor_w_uint (&wr, (uint32_t)REQUESTED_H);
    if (!mb_cbor_w_ok(&wr)) { mb_cbor_w_drop(&wr); return NULL; }
    return mb_cbor_w_finish(&wr, out_len);
}

static void on_event(mb_server_t *s, const mb_server_event_t *ev, void *user) {
    (void)user;
    switch (ev->kind) {
        case MB_SERVER_EV_CONNECTED:
            g_connected++;
            break;
        case MB_SERVER_EV_FRAME:
            if (ev->frame_kind == MB_IPC_WINDOW_CREATE) {
                g_create_seen++;
                size_t body_len = 0;
                uint8_t *body = build_create_reply(&body_len);
                if (!body) FAIL("build_create_reply OOM");
                int rc = mb_server_send(s, ev->client,
                                        MB_IPC_WINDOW_CREATE_REPLY,
                                        body, body_len, NULL, 0);
                free(body);
                if (rc != 0) FAIL("mb_server_send rc=%d", rc);
            } else if (ev->frame_kind == MB_IPC_WINDOW_COMMIT) {
                g_commit_seen++;
                // Every commit must carry exactly one shm fd. Under
                // the pre-append bug, the second commit either got
                // zero fds (stolen by frame1) or triggered EPROTO.
                if (ev->frame_fd_count != 1) {
                    g_commit_missing_fd++;
                    fprintf(stderr,
                            "[burst] commit #%d: expected 1 fd, got %zu\n",
                            g_commit_seen, ev->frame_fd_count);
                }
                fprintf(stderr,
                        "[burst] commit #%d ok (fds=%zu)\n",
                        g_commit_seen, ev->frame_fd_count);
            } else if (ev->frame_kind == MB_IPC_WINDOW_CLOSE) {
                g_close_seen++;
            }
            break;
        case MB_SERVER_EV_DISCONNECTED:
            g_disconnected++;
            // Any disconnect with a non-zero reason before we've seen
            // both commits is a failure of the regression being tested.
            if (g_commit_seen < EXPECTED_COMMITS && ev->disconnect_reason != 0) {
                fprintf(stderr,
                        "[burst] early disconnect reason=%d after %d commits\n",
                        ev->disconnect_reason, g_commit_seen);
            }
            break;
    }
}

static int run_client(const char *sock_dir) {
    if (setenv("XDG_RUNTIME_DIR", sock_dir, 1) != 0) return 10;
    if (setenv("MOONBASE_BUNDLE_ID", "show.blizzard.commit-burst-test", 1) != 0) return 11;
    if (setenv("MOONBASE_BUNDLE_VERSION", "0.1.0", 1) != 0) return 12;

    int rc = moonbase_init(0, NULL);
    if (rc != 0) return 20;

    mb_window_desc_t desc = {
        .version       = MOONBASE_WINDOW_DESC_VERSION,
        .title         = "commit-burst",
        .width_points  = REQUESTED_W,
        .height_points = REQUESTED_H,
        .render_mode   = MOONBASE_RENDER_CAIRO,
        .flags         = 0,
    };
    mb_window_t *win = moonbase_window_create(&desc);
    if (!win) return 30;

    cairo_t *cr = (cairo_t *)moonbase_window_cairo(win);
    if (!cr) return 31;

    // Fire two commits back-to-back with no intervening event pump.
    // This is the pattern that forces frame1's body-skb to coalesce
    // with frame2's header+scm-skb on the server's next recvmsg.
    //
    // moonbase_window_commit releases the current shm frame, so
    // between commits we re-acquire a fresh Cairo surface. This is
    // exactly what a real app does on successive redraws.
    cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, 1.0);
    cairo_paint(cr);
    rc = moonbase_window_commit(win);
    if (rc != 0) return 32;

    cr = (cairo_t *)moonbase_window_cairo(win);
    if (!cr) return 33;
    cairo_set_source_rgba(cr, 0.0, 1.0, 0.0, 1.0);
    cairo_paint(cr);
    rc = moonbase_window_commit(win);
    if (rc != 0) return 34;

    moonbase_window_close(win);
    moonbase_quit(0);
    return 0;
}

int main(void) {
    char dir_tmpl[] = "/tmp/mb-commit-burst.XXXXXX";
    char *dir = mkdtemp(dir_tmpl);
    if (!dir) FAIL("mkdtemp: %s", strerror(errno));

    char sock_path[256];
    snprintf(sock_path, sizeof(sock_path), "%s/moonbase.sock", dir);

    mb_server_t *srv = NULL;
    int rc = mb_server_open(&srv, sock_path, on_event, NULL);
    if (rc != 0) FAIL("mb_server_open rc=%d", rc);

    pid_t pid = fork();
    if (pid < 0) FAIL("fork: %s", strerror(errno));
    if (pid == 0) _exit(run_client(dir));

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const long timeout_ms = 3000;

    while (g_commit_seen < EXPECTED_COMMITS
        || g_close_seen < 1
        || g_disconnected < 1) {
        struct pollfd fds[16];
        size_t n = mb_server_get_pollfds(srv, fds, 16);
        int pr = poll(fds, (nfds_t)n, 50);
        if (pr < 0 && errno != EINTR) FAIL("poll: %s", strerror(errno));
        mb_server_tick(srv, fds, n);

        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000
                     + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed > timeout_ms) {
            FAIL("timeout: conn=%d create=%d commit=%d missfd=%d close=%d disc=%d",
                 g_connected, g_create_seen,
                 g_commit_seen, g_commit_missing_fd,
                 g_close_seen, g_disconnected);
        }
    }

    if (g_commit_missing_fd != 0) {
        FAIL("%d commit(s) arrived without the expected shm fd — "
             "scm batch was lost or mis-paired", g_commit_missing_fd);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) FAIL("waitpid: %s", strerror(errno));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        FAIL("child status %d exit=%d", status, WEXITSTATUS(status));
    }

    mb_server_close(srv);
    rmdir(dir);

    printf("ok: two back-to-back commits both dispatched with fds\n");
    return 0;
}
