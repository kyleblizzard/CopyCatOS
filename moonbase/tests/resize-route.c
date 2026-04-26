// CopyCatOS — by Kyle Blizzard at Blizzard.show

// resize-route.c — slice 19.H.2.j-α/β resize-event pump test.
//
// Companion to pointer-route.c. Parent stands up an mb_server_t,
// replies to the child's MB_IPC_WINDOW_CREATE at 800×600, then sends
// one MB_IPC_WINDOW_RESIZED frame advertising 640×480 in points.
//
// The child's moonbase_wait_event loop asserts that:
//   * the event arrives as MB_EV_WINDOW_RESIZED on the same window,
//   * ev.resize.old_width / old_height match the originally-requested
//     800 / 600 (the framework reads them from mb_window_t before the
//     event is delivered),
//   * ev.resize.new_width / new_height match the wire payload 640 / 480,
//   * post-event, moonbase_window_size() reports 640×480 — the
//     framework-auto-realloc surface contract from slice 19.H.2.j-β.
//
// Slice 19.H.2.j-β cr-drop coverage (task #118): also exercises the
// underlying surface-realloc path. Before wait_event, child allocates a
// Cairo frame against the 800×600 window and confirms the surface is
// sized 800 px wide (scale 1.0). After the resize event delivers, the
// cached cairo_t and surface have been retired by mb_internal_window_
// apply_resize → window_release_frame, so the next moonbase_window_cairo
// allocates a fresh surface — this test asserts that surface reports
// 640 px wide.

#include "moonbase.h"
#include "../src/server/server.h"
#include "../src/host/host_protocol.h"
#include "moonbase/ipc/kinds.h"

#include <cairo/cairo.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FAIL(...) do { \
    fprintf(stderr, "FAIL: " __VA_ARGS__); \
    fputc('\n', stderr); \
    exit(1); \
} while (0)

#define EXPECTED_WINDOW_ID   0x4321u
#define EXPECTED_OUTPUT_ID   0x0001u
#define EXPECTED_SCALE       1.0
#define REQUESTED_W          800
#define REQUESTED_H          600
#define RESIZED_W            640
#define RESIZED_H            480

static int g_connected          = 0;
static int g_disconnected       = 0;
static int g_window_create_seen = 0;
static int g_all_sent           = 0;

static void send_or_fail(mb_server_t *s, mb_client_id_t c, uint16_t kind,
                         uint8_t *body, size_t len, const char *what) {
    if (!body) FAIL("%s: build body failed", what);
    int rc = mb_server_send(s, c, kind, body, len, NULL, 0);
    free(body);
    if (rc != 0) FAIL("%s: mb_server_send rc=%d", what, rc);
}

static void on_event(mb_server_t *s, const mb_server_event_t *ev, void *user) {
    (void)user;
    switch (ev->kind) {
        case MB_SERVER_EV_CONNECTED:
            g_connected++;
            break;
        case MB_SERVER_EV_FRAME: {
            if (ev->frame_kind != MB_IPC_WINDOW_CREATE
                    || g_window_create_seen > 0) {
                fprintf(stderr,
                        "[test] EV_FRAME unexpected kind=0x%04x\n",
                        ev->frame_kind);
                break;
            }
            g_window_create_seen++;

            size_t len = 0;
            uint8_t *body = mb_host_build_window_create_reply(
                EXPECTED_WINDOW_ID, EXPECTED_OUTPUT_ID, EXPECTED_SCALE,
                (uint32_t)REQUESTED_W, (uint32_t)REQUESTED_H, &len);
            send_or_fail(s, ev->client, MB_IPC_WINDOW_CREATE_REPLY,
                         body, len, "reply");

            body = mb_host_build_window_resized(EXPECTED_WINDOW_ID,
                                                (uint32_t)RESIZED_W,
                                                (uint32_t)RESIZED_H,
                                                &len);
            send_or_fail(s, ev->client, MB_IPC_WINDOW_RESIZED,
                         body, len, "resized");

            g_all_sent = 1;
            break;
        }
        case MB_SERVER_EV_DISCONNECTED:
            g_disconnected++;
            break;
    }
}

static int run_client(const char *sock_dir) {
    if (setenv("XDG_RUNTIME_DIR", sock_dir, 1) != 0) return 10;
    if (setenv("MOONBASE_BUNDLE_ID", "show.blizzard.resizetest", 1) != 0)
        return 11;
    if (setenv("MOONBASE_BUNDLE_VERSION", "0.1.0", 1) != 0) return 12;

    int rc = moonbase_init(0, NULL);
    if (rc != 0) return 20;

    mb_window_desc_t desc = {
        .version       = MOONBASE_WINDOW_DESC_VERSION,
        .title         = "resize-test",
        .width_points  = REQUESTED_W,
        .height_points = REQUESTED_H,
        .render_mode   = MOONBASE_RENDER_CAIRO,
        .flags         = 0,
    };
    mb_window_t *win = moonbase_window_create(&desc);
    if (!win) return 21;

    // Force a Cairo frame allocation at the original 800×600 dims. The
    // resize event has already been queued by the parent (sent in the
    // same handler as the WINDOW_CREATE_REPLY) but has not yet been
    // pumped, so the framework's cached width is still REQUESTED_W.
    cairo_t *cr_before = (cairo_t *)moonbase_window_cairo(win);
    if (!cr_before) return 28;
    int w_before_px =
        cairo_image_surface_get_width(cairo_get_target(cr_before));
    if (w_before_px != REQUESTED_W) {
        fprintf(stderr,
                "child: pre-resize cairo surface width %d (want %d)\n",
                w_before_px, REQUESTED_W);
        return 29;
    }

    mb_event_t ev = {0};
    rc = moonbase_wait_event(&ev, 2000);
    if (rc != 1) {
        fprintf(stderr, "child: wait_event rc=%d last_err=%d\n",
                rc, moonbase_last_error());
        return 22;
    }
    if (ev.kind != MB_EV_WINDOW_RESIZED) {
        fprintf(stderr, "child: wrong kind %d (want %d)\n",
                (int)ev.kind, (int)MB_EV_WINDOW_RESIZED);
        return 23;
    }
    if (ev.window != win) {
        fprintf(stderr, "child: ev.window mismatch\n");
        return 24;
    }
    if (ev.resize.old_width != REQUESTED_W
            || ev.resize.old_height != REQUESTED_H) {
        fprintf(stderr,
                "child: old dims wrong got %dx%d want %dx%d\n",
                ev.resize.old_width, ev.resize.old_height,
                REQUESTED_W, REQUESTED_H);
        return 25;
    }
    if (ev.resize.new_width != RESIZED_W
            || ev.resize.new_height != RESIZED_H) {
        fprintf(stderr,
                "child: new dims wrong got %dx%d want %dx%d\n",
                ev.resize.new_width, ev.resize.new_height,
                RESIZED_W, RESIZED_H);
        return 26;
    }

    // Slice 19.H.2.j-β surface contract: framework auto-applies the new
    // dims before delivering the event, so the public size accessor
    // must already report the new size.
    int now_w = 0, now_h = 0;
    moonbase_window_size(win, &now_w, &now_h);
    if (now_w != RESIZED_W || now_h != RESIZED_H) {
        fprintf(stderr,
                "child: moonbase_window_size post-event got %dx%d want %dx%d\n",
                now_w, now_h, RESIZED_W, RESIZED_H);
        return 27;
    }

    // Slice 19.H.2.j-β cr-drop assertion: the previously-cached cairo_t
    // was sized to 800×600 backing pixels. The resize delivery retired
    // it, so the next moonbase_window_cairo() must allocate a fresh
    // surface at 640×480 — the size the app will draw against next.
    cairo_t *cr_after = (cairo_t *)moonbase_window_cairo(win);
    if (!cr_after) return 30;
    int w_after_px =
        cairo_image_surface_get_width(cairo_get_target(cr_after));
    if (w_after_px != RESIZED_W) {
        fprintf(stderr,
                "child: post-resize cairo surface width %d (want %d)\n",
                w_after_px, RESIZED_W);
        return 31;
    }

    moonbase_window_close(win);
    moonbase_quit(0);
    return 0;
}

int main(void) {
    char dir_tmpl[] = "/tmp/mb-resize.XXXXXX";
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
        _exit(run_client(dir));
    }

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const long timeout_ms = 3000;

    while (g_connected < 1 || g_window_create_seen < 1
        || !g_all_sent       || g_disconnected < 1) {
        struct pollfd fds[16];
        size_t n = mb_server_get_pollfds(srv, fds, 16);
        int pr = poll(fds, (nfds_t)n, 50);
        if (pr < 0 && errno != EINTR) FAIL("poll: %s", strerror(errno));
        mb_server_tick(srv, fds, n);

        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000
                     + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed > timeout_ms) {
            FAIL("timeout: connected=%d create=%d sent=%d disc=%d",
                 g_connected, g_window_create_seen,
                 g_all_sent, g_disconnected);
        }
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) FAIL("waitpid: %s", strerror(errno));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        FAIL("child exited status %d (exit=%d)",
             status, WEXITSTATUS(status));
    }

    mb_server_close(srv);
    rmdir(dir);

    printf("ok: WINDOW_RESIZED → MB_EV_WINDOW_RESIZED delivered with "
           "old %dx%d → new %dx%d\n",
           REQUESTED_W, REQUESTED_H, RESIZED_W, RESIZED_H);
    return 0;
}
