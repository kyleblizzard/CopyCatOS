// CopyCatOS — by Kyle Blizzard at Blizzard.show

// window-create.c — Phase C slice 3a test.
//
// Proves the client-side moonbase_window_create round-trips cleanly
// against the real mb_server_t. The parent stands up a server that
// replies to MB_IPC_WINDOW_CREATE with a canned MB_IPC_WINDOW_CREATE_REPLY.
// The forked child calls moonbase_init, then moonbase_window_create
// with a known descriptor, then moonbase_window_close + moonbase_quit.
//
// The test pins the invariants that slice 3a needs to hold before
// slice 3b adds per-output scale tracking:
//   * the client sends a well-formed HELLO + WINDOW_CREATE
//   * the compositor reply round-trips through mb_conn_request
//   * the returned mb_window_t handle reports the size + scale the
//     server advertised
//   * the WINDOW_CLOSE message is delivered post-create
//   * the child exits 0 and the server sees one clean EV_DISCONNECTED

#include "CopyCatAppKit.h"
#include "../src/server/server.h"
#include "../src/ipc/cbor.h"
#include "moonbase/ipc/kinds.h"

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

// Canned reply values. The child asserts these after the round-trip.
#define EXPECTED_WINDOW_ID   0x1234u
#define EXPECTED_OUTPUT_ID   0x5678u
#define EXPECTED_SCALE       1.5
#define REQUESTED_W          640
#define REQUESTED_H          400

// Counters shared with the callback.
static int g_connected = 0;
static int g_disconnected = 0;
static int g_window_create_seen = 0;
static int g_window_close_seen = 0;

// Build the reply. Keeps the test self-contained (no moonrock link).
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
            g_connected++;
            fprintf(stderr,
                    "[test] EV_CONNECTED client=%u bundle='%s'\n",
                    ev->client,
                    ev->hello.bundle_id ? ev->hello.bundle_id : "(null)");
            break;
        case MB_SERVER_EV_FRAME:
            if (ev->frame_kind == MB_IPC_WINDOW_CREATE) {
                g_window_create_seen++;
                fprintf(stderr,
                        "[test] EV_FRAME WINDOW_CREATE client=%u len=%zu\n",
                        ev->client, ev->frame_body_len);
                size_t body_len = 0;
                uint8_t *body = build_reply(EXPECTED_WINDOW_ID,
                                            EXPECTED_OUTPUT_ID,
                                            EXPECTED_SCALE,
                                            (uint32_t)REQUESTED_W,
                                            (uint32_t)REQUESTED_H,
                                            &body_len);
                if (!body) FAIL("build_reply: out of memory");
                int rc = mb_server_send(s, ev->client,
                                        MB_IPC_WINDOW_CREATE_REPLY,
                                        body, body_len, NULL, 0);
                free(body);
                if (rc != 0) FAIL("mb_server_send rc=%d", rc);
            } else if (ev->frame_kind == MB_IPC_WINDOW_CLOSE) {
                g_window_close_seen++;
                fprintf(stderr,
                        "[test] EV_FRAME WINDOW_CLOSE client=%u\n",
                        ev->client);
            } else {
                fprintf(stderr,
                        "[test] EV_FRAME unexpected kind=0x%04x len=%zu\n",
                        ev->frame_kind, ev->frame_body_len);
            }
            break;
        case MB_SERVER_EV_DISCONNECTED:
            g_disconnected++;
            fprintf(stderr, "[test] EV_DISCONNECTED client=%u reason=%d\n",
                    ev->client, ev->disconnect_reason);
            break;
    }
}

// Child process: drives the client API and exits 0 on success.
static int run_client(const char *sock_dir) {
    if (setenv("XDG_RUNTIME_DIR", sock_dir, 1) != 0) return 10;
    if (setenv("MOONBASE_BUNDLE_ID", "show.blizzard.wintest", 1) != 0) return 11;
    if (setenv("MOONBASE_BUNDLE_VERSION", "0.1.0", 1) != 0) return 12;

    int rc = moonbase_init(0, NULL);
    if (rc != 0) {
        fprintf(stderr, "child: init rc=%d\n", rc);
        return 20;
    }

    mb_window_desc_t desc = {
        .version          = MOONBASE_WINDOW_DESC_VERSION,
        .title            = "hello",
        .width_points     = REQUESTED_W,
        .height_points    = REQUESTED_H,
        .render_mode      = MOONBASE_RENDER_CAIRO,
        .flags            = 0,
    };
    mb_window_t *win = moonbase_window_create(&desc);
    if (!win) {
        fprintf(stderr, "child: window_create failed last_err=%d\n",
                moonbase_last_error());
        moonbase_quit(0);
        return 30;
    }

    int w = 0, h = 0;
    moonbase_window_size(win, &w, &h);
    if (w != REQUESTED_W || h != REQUESTED_H) {
        fprintf(stderr, "child: size mismatch got %dx%d expected %dx%d\n",
                w, h, REQUESTED_W, REQUESTED_H);
        moonbase_window_close(win);
        moonbase_quit(0);
        return 31;
    }

    float scale = moonbase_window_backing_scale(win);
    if (scale < 1.49f || scale > 1.51f) {
        fprintf(stderr, "child: scale mismatch got %f expected ~%f\n",
                (double)scale, EXPECTED_SCALE);
        moonbase_window_close(win);
        moonbase_quit(0);
        return 32;
    }

    int wpx = 0, hpx = 0;
    moonbase_window_backing_pixel_size(win, &wpx, &hpx);
    // 640 * 1.5 = 960, 400 * 1.5 = 600. Tolerate +/- 1 px for rounding.
    if (wpx < 959 || wpx > 961 || hpx < 599 || hpx > 601) {
        fprintf(stderr, "child: pixel size off got %dx%d\n", wpx, hpx);
        moonbase_window_close(win);
        moonbase_quit(0);
        return 33;
    }

    moonbase_window_close(win);
    moonbase_quit(0);
    return 0;
}

int main(void) {
    char dir_tmpl[] = "/tmp/mb-wc.XXXXXX";
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
        || g_window_close_seen < 1 || g_disconnected < 1) {
        struct pollfd fds[16];
        size_t n = mb_server_get_pollfds(srv, fds, 16);
        int pr = poll(fds, (nfds_t)n, 50);
        if (pr < 0 && errno != EINTR) FAIL("poll: %s", strerror(errno));
        mb_server_tick(srv, fds, n);

        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000
                     + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed > timeout_ms) {
            FAIL("timeout: connected=%d create=%d close=%d disconnected=%d",
                 g_connected, g_window_create_seen,
                 g_window_close_seen, g_disconnected);
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

    printf("ok: window_create round-trip (window_id=%u scale=%.2f)\n",
           EXPECTED_WINDOW_ID, EXPECTED_SCALE);
    return 0;
}
