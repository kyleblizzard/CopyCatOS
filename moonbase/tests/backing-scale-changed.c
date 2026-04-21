// CopyCatOS — by Kyle Blizzard at Blizzard.show

// backing-scale-changed.c — Phase C slice 3c.4 event pump test.
//
// Parent stands up a real mb_server_t, replies to the child's
// MB_IPC_WINDOW_CREATE with a canned reply at scale=1.0 / output_id=1,
// then pushes an MB_IPC_BACKING_SCALE_CHANGED frame advertising
// new_scale=1.5 / new_output_id=2. Child calls moonbase_wait_event and
// asserts:
//   * the next event is MB_EV_BACKING_SCALE_CHANGED with the right
//     old_scale / new_scale payload
//   * ev.window points at the handle returned from
//     moonbase_window_create
//   * moonbase_window_backing_scale(w) now reports 1.5
//   * moonbase_window_backing_pixel_size scales up accordingly
//
// Pins the client side of the scale-migration contract. The
// moonrock-side emitter (per-frame scan of surface rects vs output
// rects) is exercised only in integration on the Legion Go S.

#include "CopyCatAppKit.h"
#include "../src/server/server.h"
#include "../src/ipc/cbor.h"
#include "moonbase/ipc/kinds.h"

#include <errno.h>
#include <math.h>
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

#define EXPECTED_WINDOW_ID   0xBEEFu
#define OLD_OUTPUT_ID        0x0001u
#define NEW_OUTPUT_ID        0x0002u
#define OLD_SCALE            1.0
#define NEW_SCALE            1.5
#define REQUESTED_W          640
#define REQUESTED_H          400

static int g_connected          = 0;
static int g_disconnected       = 0;
static int g_window_create_seen = 0;
static int g_scale_sent         = 0;

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

// Compose a BACKING_SCALE_CHANGED body:
//   { 1: window_id, 2: float old_scale, 3: float new_scale,
//     4: uint output_id }
static uint8_t *build_scale_changed(uint32_t window_id,
                                    double old_scale, double new_scale,
                                    uint32_t output_id, size_t *out_len) {
    mb_cbor_w_t wr;
    mb_cbor_w_init_grow(&wr, 32);
    mb_cbor_w_map_begin(&wr, 4);
    mb_cbor_w_key(&wr, 1); mb_cbor_w_uint (&wr, window_id);
    mb_cbor_w_key(&wr, 2); mb_cbor_w_float(&wr, old_scale);
    mb_cbor_w_key(&wr, 3); mb_cbor_w_float(&wr, new_scale);
    mb_cbor_w_key(&wr, 4); mb_cbor_w_uint (&wr, output_id);
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
            if (ev->frame_kind == MB_IPC_WINDOW_CREATE
             && g_window_create_seen == 0) {
                g_window_create_seen++;

                size_t body_len = 0;
                uint8_t *body = build_reply(EXPECTED_WINDOW_ID,
                                            OLD_OUTPUT_ID,
                                            OLD_SCALE,
                                            (uint32_t)REQUESTED_W,
                                            (uint32_t)REQUESTED_H,
                                            &body_len);
                if (!body) FAIL("build_reply: OOM");
                int rc = mb_server_send(s, ev->client,
                                        MB_IPC_WINDOW_CREATE_REPLY,
                                        body, body_len, NULL, 0);
                free(body);
                if (rc != 0) FAIL("mb_server_send reply rc=%d", rc);

                // Immediately push a BACKING_SCALE_CHANGED. The child is
                // still inside mb_conn_request when both frames land;
                // the reply matches the request, the scale-change gets
                // parked on the pending-frame queue, and wait_event
                // picks it up on the next pump.
                size_t sc_len = 0;
                uint8_t *sc_body = build_scale_changed(
                    EXPECTED_WINDOW_ID, OLD_SCALE, NEW_SCALE,
                    NEW_OUTPUT_ID, &sc_len);
                if (!sc_body) FAIL("build_scale_changed: OOM");
                rc = mb_server_send(s, ev->client,
                                    MB_IPC_BACKING_SCALE_CHANGED,
                                    sc_body, sc_len, NULL, 0);
                free(sc_body);
                if (rc != 0) FAIL("mb_server_send scale rc=%d", rc);
                g_scale_sent = 1;
            }
            break;
        case MB_SERVER_EV_DISCONNECTED:
            g_disconnected++;
            break;
    }
}

static int approx_eq(float a, float b) {
    return fabsf(a - b) < 0.0001f;
}

static int run_client(const char *sock_dir) {
    if (setenv("XDG_RUNTIME_DIR", sock_dir, 1) != 0) return 10;
    if (setenv("MOONBASE_BUNDLE_ID", "show.blizzard.scaletest", 1) != 0) return 11;
    if (setenv("MOONBASE_BUNDLE_VERSION", "0.1.0", 1) != 0) return 12;

    int rc = moonbase_init(0, NULL);
    if (rc != 0) { fprintf(stderr, "child: init rc=%d\n", rc); return 20; }

    mb_window_desc_t desc = {
        .version       = MOONBASE_WINDOW_DESC_VERSION,
        .title         = "scale-test",
        .width_points  = REQUESTED_W,
        .height_points = REQUESTED_H,
        .render_mode   = MOONBASE_RENDER_CAIRO,
        .flags         = 0,
    };
    mb_window_t *win = moonbase_window_create(&desc);
    if (!win) {
        fprintf(stderr, "child: window_create failed last_err=%d\n",
                moonbase_last_error());
        moonbase_quit(0);
        return 21;
    }

    // Sanity: backing scale should start at OLD_SCALE since the parent
    // replied with it. If this already drifted the test can't prove
    // anything about the update path.
    float initial = moonbase_window_backing_scale(win);
    if (!approx_eq(initial, (float)OLD_SCALE)) {
        fprintf(stderr, "child: wrong initial scale %.3f (want %.3f)\n",
                (double)initial, OLD_SCALE);
        moonbase_window_close(win);
        moonbase_quit(0);
        return 22;
    }

    mb_event_t ev = {0};
    int wr = moonbase_wait_event(&ev, 2000);
    if (wr != 1) {
        fprintf(stderr, "child: wait_event returned %d last_err=%d\n",
                wr, moonbase_last_error());
        moonbase_window_close(win);
        moonbase_quit(0);
        return 23;
    }
    if (ev.kind != MB_EV_BACKING_SCALE_CHANGED) {
        fprintf(stderr, "child: wrong event kind %d (want BACKING_SCALE_CHANGED)\n",
                (int)ev.kind);
        moonbase_window_close(win);
        moonbase_quit(0);
        return 24;
    }
    if (ev.window != win) {
        fprintf(stderr, "child: ev.window mismatch %p vs %p\n",
                (void *)ev.window, (void *)win);
        moonbase_window_close(win);
        moonbase_quit(0);
        return 25;
    }
    if (!approx_eq(ev.backing_scale.old_scale, (float)OLD_SCALE)) {
        fprintf(stderr, "child: wrong old_scale %.3f (want %.3f)\n",
                (double)ev.backing_scale.old_scale, OLD_SCALE);
        moonbase_window_close(win);
        moonbase_quit(0);
        return 26;
    }
    if (!approx_eq(ev.backing_scale.new_scale, (float)NEW_SCALE)) {
        fprintf(stderr, "child: wrong new_scale %.3f (want %.3f)\n",
                (double)ev.backing_scale.new_scale, NEW_SCALE);
        moonbase_window_close(win);
        moonbase_quit(0);
        return 27;
    }

    float updated = moonbase_window_backing_scale(win);
    if (!approx_eq(updated, (float)NEW_SCALE)) {
        fprintf(stderr, "child: backing_scale not updated — %.3f (want %.3f)\n",
                (double)updated, NEW_SCALE);
        moonbase_window_close(win);
        moonbase_quit(0);
        return 28;
    }

    // Pixel size must follow. 640x400 pt × 1.5 = 960x600 px (±1 rounding).
    int pw = 0, ph = 0;
    moonbase_window_backing_pixel_size(win, &pw, &ph);
    int want_w = (int)(REQUESTED_W * NEW_SCALE + 0.5);
    int want_h = (int)(REQUESTED_H * NEW_SCALE + 0.5);
    if (pw != want_w || ph != want_h) {
        fprintf(stderr,
                "child: backing_pixel_size %dx%d (want %dx%d)\n",
                pw, ph, want_w, want_h);
        moonbase_window_close(win);
        moonbase_quit(0);
        return 29;
    }

    moonbase_window_close(win);
    moonbase_quit(0);
    return 0;
}

int main(void) {
    char dir_tmpl[] = "/tmp/mb-scale.XXXXXX";
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
        || !g_scale_sent     || g_disconnected < 1) {
        struct pollfd fds[16];
        size_t n = mb_server_get_pollfds(srv, fds, 16);
        int pr = poll(fds, (nfds_t)n, 50);
        if (pr < 0 && errno != EINTR) FAIL("poll: %s", strerror(errno));
        mb_server_tick(srv, fds, n);

        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000
                     + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed > timeout_ms) {
            FAIL("timeout: connected=%d create=%d scale_sent=%d disc=%d",
                 g_connected, g_window_create_seen,
                 g_scale_sent, g_disconnected);
        }
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) FAIL("waitpid: %s", strerror(errno));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        FAIL("child exited with status %d (exit=%d)",
             status, WEXITSTATUS(status));
    }

    mb_server_close(srv);
    rmdir(dir);

    printf("ok: BACKING_SCALE_CHANGED → MB_EV_BACKING_SCALE_CHANGED "
           "(%.1f→%.1f, output %u→%u)\n",
           OLD_SCALE, NEW_SCALE, OLD_OUTPUT_ID, NEW_OUTPUT_ID);
    return 0;
}
