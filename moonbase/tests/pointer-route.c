// CopyCatOS — by Kyle Blizzard at Blizzard.show

// pointer-route.c — Phase C slice 19.H.2.e pointer-event pump test.
//
// Companion to input-route.c. The parent stands up a real mb_server_t,
// replies to the child's MB_IPC_WINDOW_CREATE, then sends three canned
// frames in order:
//   1. MB_IPC_POINTER_DOWN  x=120 y=80  button=MB_BUTTON_LEFT
//                           modifiers=MB_MOD_SHIFT
//   2. MB_IPC_POINTER_MOVE  x=125 y=82  button=0
//                           modifiers=MB_MOD_SHIFT
//   3. MB_IPC_POINTER_UP    x=130 y=85  button=MB_BUTTON_LEFT
//                           modifiers=0
//
// The child's moonbase_wait_event loop asserts they arrive in the same
// order as MB_EV_POINTER_DOWN, MB_EV_POINTER_MOVE, MB_EV_POINTER_UP,
// with the correct window pointer, x/y, button, and modifier fields.

#include "moonbase.h"
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

#define EXPECTED_WINDOW_ID   0x4321u
#define EXPECTED_OUTPUT_ID   0x0001u
#define EXPECTED_SCALE       1.0
#define REQUESTED_W          800
#define REQUESTED_H          600

static int g_connected          = 0;
static int g_disconnected       = 0;
static int g_window_create_seen = 0;
static int g_all_sent           = 0;

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

static uint8_t *build_pointer(uint32_t window_id, int x, int y,
                              uint32_t button, uint32_t modifiers,
                              size_t *out_len) {
    mb_cbor_w_t wr;
    mb_cbor_w_init_grow(&wr, 32);
    mb_cbor_w_map_begin(&wr, 6);
    mb_cbor_w_key(&wr, 1); mb_cbor_w_uint(&wr, window_id);
    mb_cbor_w_key(&wr, 2); mb_cbor_w_int (&wr, (int64_t)x);
    mb_cbor_w_key(&wr, 3); mb_cbor_w_int (&wr, (int64_t)y);
    mb_cbor_w_key(&wr, 4); mb_cbor_w_uint(&wr, button);
    mb_cbor_w_key(&wr, 5); mb_cbor_w_uint(&wr, modifiers);
    mb_cbor_w_key(&wr, 6); mb_cbor_w_uint(&wr, 0);
    if (!mb_cbor_w_ok(&wr)) { mb_cbor_w_drop(&wr); return NULL; }
    return mb_cbor_w_finish(&wr, out_len);
}

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
            uint8_t *body = build_reply(EXPECTED_WINDOW_ID,
                                        EXPECTED_OUTPUT_ID,
                                        EXPECTED_SCALE,
                                        (uint32_t)REQUESTED_W,
                                        (uint32_t)REQUESTED_H, &len);
            send_or_fail(s, ev->client, MB_IPC_WINDOW_CREATE_REPLY,
                         body, len, "reply");

            body = build_pointer(EXPECTED_WINDOW_ID, 120, 80,
                                 MB_BUTTON_LEFT, MB_MOD_SHIFT, &len);
            send_or_fail(s, ev->client, MB_IPC_POINTER_DOWN,
                         body, len, "pointer_down");

            body = build_pointer(EXPECTED_WINDOW_ID, 125, 82,
                                 0, MB_MOD_SHIFT, &len);
            send_or_fail(s, ev->client, MB_IPC_POINTER_MOVE,
                         body, len, "pointer_move");

            body = build_pointer(EXPECTED_WINDOW_ID, 130, 85,
                                 MB_BUTTON_LEFT, 0, &len);
            send_or_fail(s, ev->client, MB_IPC_POINTER_UP,
                         body, len, "pointer_up");

            g_all_sent = 1;
            break;
        }
        case MB_SERVER_EV_DISCONNECTED:
            g_disconnected++;
            break;
    }
}

static int expect(mb_event_kind_t want, mb_window_t *want_win, mb_event_t *ev) {
    int rc = moonbase_wait_event(ev, 2000);
    if (rc != 1) {
        fprintf(stderr, "child: wait_event rc=%d last_err=%d\n",
                rc, moonbase_last_error());
        return 0;
    }
    if (ev->kind != want) {
        fprintf(stderr, "child: wrong kind %d (want %d)\n",
                (int)ev->kind, (int)want);
        return 0;
    }
    if (ev->window != want_win) {
        fprintf(stderr, "child: ev.window mismatch\n");
        return 0;
    }
    return 1;
}

static int run_client(const char *sock_dir) {
    if (setenv("XDG_RUNTIME_DIR", sock_dir, 1) != 0) return 10;
    if (setenv("MOONBASE_BUNDLE_ID", "show.blizzard.pointertest", 1) != 0)
        return 11;
    if (setenv("MOONBASE_BUNDLE_VERSION", "0.1.0", 1) != 0) return 12;

    int rc = moonbase_init(0, NULL);
    if (rc != 0) return 20;

    mb_window_desc_t desc = {
        .version       = MOONBASE_WINDOW_DESC_VERSION,
        .title         = "pointer-test",
        .width_points  = REQUESTED_W,
        .height_points = REQUESTED_H,
        .render_mode   = MOONBASE_RENDER_CAIRO,
        .flags         = 0,
    };
    mb_window_t *win = moonbase_window_create(&desc);
    if (!win) return 21;

    mb_event_t ev = {0};
    if (!expect(MB_EV_POINTER_DOWN, win, &ev)) return 22;
    if (ev.pointer.x != 120 || ev.pointer.y != 80
            || ev.pointer.button != MB_BUTTON_LEFT
            || ev.pointer.modifiers != MB_MOD_SHIFT) {
        fprintf(stderr,
                "child: pointer_down fields wrong "
                "x=%d y=%d btn=%u mods=0x%x\n",
                ev.pointer.x, ev.pointer.y,
                ev.pointer.button, ev.pointer.modifiers);
        return 23;
    }

    if (!expect(MB_EV_POINTER_MOVE, win, &ev)) return 24;
    if (ev.pointer.x != 125 || ev.pointer.y != 82
            || ev.pointer.button != 0
            || ev.pointer.modifiers != MB_MOD_SHIFT) {
        fprintf(stderr,
                "child: pointer_move fields wrong "
                "x=%d y=%d btn=%u mods=0x%x\n",
                ev.pointer.x, ev.pointer.y,
                ev.pointer.button, ev.pointer.modifiers);
        return 25;
    }

    if (!expect(MB_EV_POINTER_UP, win, &ev)) return 26;
    if (ev.pointer.x != 130 || ev.pointer.y != 85
            || ev.pointer.button != MB_BUTTON_LEFT
            || ev.pointer.modifiers != 0) {
        fprintf(stderr,
                "child: pointer_up fields wrong "
                "x=%d y=%d btn=%u mods=0x%x\n",
                ev.pointer.x, ev.pointer.y,
                ev.pointer.button, ev.pointer.modifiers);
        return 27;
    }

    moonbase_window_close(win);
    moonbase_quit(0);
    return 0;
}

int main(void) {
    char dir_tmpl[] = "/tmp/mb-pointer.XXXXXX";
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

    printf("ok: POINTER_DOWN + POINTER_MOVE + POINTER_UP → events delivered\n");
    return 0;
}
