// CopyCatOS — by Kyle Blizzard at Blizzard.show

// event-window-closed.c — Phase C slice 4 event pump test.
//
// The parent stands up a real mb_server_t, replies to the child's
// MB_IPC_WINDOW_CREATE with a canned reply, then pushes an
// MB_IPC_WINDOW_CLOSED frame naming that same window_id. The child
// calls moonbase_wait_event and asserts the next event is
// MB_EV_WINDOW_CLOSED with ev.window pointing to the handle the
// framework returned from moonbase_window_create.
//
// Invariants exercised:
//   * mb_conn_request's unrelated-frame handling routes into the
//     event-loop queue instead of being discarded
//   * compositor-initiated WINDOW_CLOSED frames surface through
//     moonbase_wait_event as MB_EV_WINDOW_CLOSED
//   * ev.window is filled in via the window registry (so apps can
//     compare against their own handle, not raw ids)
//   * moonbase_quit posts MB_EV_APP_WILL_QUIT before tearing down

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

// Canned reply + close values. Chosen distinct from the window-create
// test's ids so a cross-test mixup would be visible.
#define EXPECTED_WINDOW_ID   0xABCDu
#define EXPECTED_OUTPUT_ID   0x0001u
#define EXPECTED_SCALE       1.0
#define REQUESTED_W          320
#define REQUESTED_H          200

static int g_connected         = 0;
static int g_disconnected      = 0;
static int g_window_create_seen = 0;
static int g_closed_sent       = 0;

// Compose a WINDOW_CREATE_REPLY body. Same layout as window-create.c.
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

// Compose a WINDOW_CLOSED body: { 1: uint window_id }.
static uint8_t *build_window_closed(uint32_t window_id, size_t *out_len) {
    mb_cbor_w_t wr;
    mb_cbor_w_init_grow(&wr, 16);
    mb_cbor_w_map_begin(&wr, 1);
    mb_cbor_w_key(&wr, 1); mb_cbor_w_uint(&wr, window_id);
    if (!mb_cbor_w_ok(&wr)) { mb_cbor_w_drop(&wr); return NULL; }
    return mb_cbor_w_finish(&wr, out_len);
}

static void on_event(mb_server_t *s, const mb_server_event_t *ev, void *user) {
    (void)user;
    switch (ev->kind) {
        case MB_SERVER_EV_CONNECTED:
            g_connected++;
            fprintf(stderr, "[test] EV_CONNECTED client=%u bundle='%s'\n",
                    ev->client,
                    ev->hello.bundle_id ? ev->hello.bundle_id : "(null)");
            break;
        case MB_SERVER_EV_FRAME:
            if (ev->frame_kind == MB_IPC_WINDOW_CREATE
             && g_window_create_seen == 0) {
                g_window_create_seen++;
                // Reply to create ...
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
                if (rc != 0) FAIL("mb_server_send reply rc=%d", rc);

                // ... then immediately push a WINDOW_CLOSED at the
                // child. The child is still inside mb_conn_request when
                // these two frames land in close succession; the reply
                // matches the request, the CLOSED gets parked on the
                // pending-frame queue, and wait_event picks it up.
                size_t close_len = 0;
                uint8_t *close_body = build_window_closed(
                    EXPECTED_WINDOW_ID, &close_len);
                if (!close_body) FAIL("build_window_closed: OOM");
                rc = mb_server_send(s, ev->client,
                                    MB_IPC_WINDOW_CLOSED,
                                    close_body, close_len, NULL, 0);
                free(close_body);
                if (rc != 0) FAIL("mb_server_send closed rc=%d", rc);
                g_closed_sent = 1;
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

// Child: init → create → wait_event → assert CLOSED → quit.
static int run_client(const char *sock_dir) {
    if (setenv("XDG_RUNTIME_DIR", sock_dir, 1) != 0) return 10;
    if (setenv("MOONBASE_BUNDLE_ID", "show.blizzard.evtest", 1) != 0) return 11;
    if (setenv("MOONBASE_BUNDLE_VERSION", "0.1.0", 1) != 0) return 12;

    int rc = moonbase_init(0, NULL);
    if (rc != 0) { fprintf(stderr, "child: init rc=%d\n", rc); return 20; }

    mb_window_desc_t desc = {
        .version       = MOONBASE_WINDOW_DESC_VERSION,
        .title         = "ev-test",
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

    // Wait for the WINDOW_CLOSED event. The server sends it right
    // after the WINDOW_CREATE_REPLY; with a 2 s budget we have ample
    // margin for the socket round-trip on a loaded build host.
    mb_event_t ev = {0};
    int wr = moonbase_wait_event(&ev, 2000);
    if (wr != 1) {
        fprintf(stderr, "child: wait_event returned %d last_err=%d\n",
                wr, moonbase_last_error());
        moonbase_window_close(win);
        moonbase_quit(0);
        return 22;
    }
    if (ev.kind != MB_EV_WINDOW_CLOSED) {
        fprintf(stderr, "child: wrong event kind %d (want MB_EV_WINDOW_CLOSED)\n",
                (int)ev.kind);
        moonbase_window_close(win);
        moonbase_quit(0);
        return 23;
    }
    if (ev.window != win) {
        fprintf(stderr, "child: ev.window mismatch %p vs %p\n",
                (void *)ev.window, (void *)win);
        moonbase_window_close(win);
        moonbase_quit(0);
        return 24;
    }

    // Now call quit and assert APP_WILL_QUIT is queued. moonbase_quit
    // closes the connection, so poll_event after it operates on the
    // ring only — no socket involved.
    moonbase_window_close(win);
    moonbase_quit(0);

    mb_event_t qev = {0};
    int pr = moonbase_poll_event(&qev);
    if (pr != 1 || qev.kind != MB_EV_APP_WILL_QUIT) {
        fprintf(stderr, "child: expected APP_WILL_QUIT got rc=%d kind=%d\n",
                pr, (int)qev.kind);
        return 25;
    }
    return 0;
}

int main(void) {
    char dir_tmpl[] = "/tmp/mb-ev.XXXXXX";
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
        || !g_closed_sent    || g_disconnected < 1) {
        struct pollfd fds[16];
        size_t n = mb_server_get_pollfds(srv, fds, 16);
        int pr = poll(fds, (nfds_t)n, 50);
        if (pr < 0 && errno != EINTR) FAIL("poll: %s", strerror(errno));
        mb_server_tick(srv, fds, n);

        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000
                     + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed > timeout_ms) {
            FAIL("timeout: connected=%d create=%d closed_sent=%d disc=%d",
                 g_connected, g_window_create_seen,
                 g_closed_sent, g_disconnected);
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

    printf("ok: WINDOW_CLOSED → MB_EV_WINDOW_CLOSED (window_id=%u)\n",
           EXPECTED_WINDOW_ID);
    return 0;
}
