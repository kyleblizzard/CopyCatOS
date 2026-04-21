// CopyCatOS — by Kyle Blizzard at Blizzard.show

// input-route.c — Phase C slice 3c.3 focus + key-event pump test.
//
// The parent stands up a real mb_server_t, replies to the child's
// MB_IPC_WINDOW_CREATE, then sends three canned frames in order:
//   1. MB_IPC_WINDOW_FOCUSED  has_focus=true
//   2. MB_IPC_KEY_DOWN        keycode=0x41 modifiers=MB_MOD_SHIFT
//                             is_repeat=false
//   3. MB_IPC_KEY_UP          keycode=0x41 modifiers=0 is_repeat=false
//
// The child's moonbase_wait_event loop asserts they arrive in the same
// order as MB_EV_WINDOW_FOCUSED, MB_EV_KEY_DOWN, MB_EV_KEY_UP, with the
// correct window pointer, keycodes, modifiers, and is_repeat flag.
//
// Invariants exercised:
//   * MB_IPC_WINDOW_FOCUSED → MB_EV_WINDOW_FOCUSED with ev.focus.has_focus
//   * MB_IPC_KEY_DOWN/UP → MB_EV_KEY_DOWN/UP with ev.key.{keycode,
//     modifiers, is_repeat}
//   * Event ordering matches send ordering (FIFO ring behavior)

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

#define EXPECTED_WINDOW_ID   0x1234u
#define EXPECTED_OUTPUT_ID   0x0001u
#define EXPECTED_SCALE       1.0
#define REQUESTED_W          800
#define REQUESTED_H          600
#define TEST_KEYCODE         0x0041u
#define TEST_MOD_DOWN        MB_MOD_SHIFT
#define TEST_MOD_UP          0u

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

static uint8_t *build_focus(uint32_t window_id, bool focused,
                            size_t *out_len) {
    mb_cbor_w_t wr;
    mb_cbor_w_init_grow(&wr, 16);
    mb_cbor_w_map_begin(&wr, 2);
    mb_cbor_w_key(&wr, 1); mb_cbor_w_uint(&wr, window_id);
    mb_cbor_w_key(&wr, 2); mb_cbor_w_bool(&wr, focused);
    if (!mb_cbor_w_ok(&wr)) { mb_cbor_w_drop(&wr); return NULL; }
    return mb_cbor_w_finish(&wr, out_len);
}

static uint8_t *build_key(uint32_t window_id, uint32_t keycode,
                          uint32_t modifiers, bool is_repeat,
                          size_t *out_len) {
    mb_cbor_w_t wr;
    mb_cbor_w_init_grow(&wr, 32);
    mb_cbor_w_map_begin(&wr, 5);
    mb_cbor_w_key(&wr, 1); mb_cbor_w_uint(&wr, window_id);
    mb_cbor_w_key(&wr, 2); mb_cbor_w_uint(&wr, keycode);
    mb_cbor_w_key(&wr, 3); mb_cbor_w_uint(&wr, modifiers);
    mb_cbor_w_key(&wr, 4); mb_cbor_w_bool(&wr, is_repeat);
    mb_cbor_w_key(&wr, 5); mb_cbor_w_uint(&wr, 0);
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

            body = build_focus(EXPECTED_WINDOW_ID, true, &len);
            send_or_fail(s, ev->client, MB_IPC_WINDOW_FOCUSED,
                         body, len, "focus");

            body = build_key(EXPECTED_WINDOW_ID, TEST_KEYCODE,
                             TEST_MOD_DOWN, false, &len);
            send_or_fail(s, ev->client, MB_IPC_KEY_DOWN,
                         body, len, "key_down");

            body = build_key(EXPECTED_WINDOW_ID, TEST_KEYCODE,
                             TEST_MOD_UP, false, &len);
            send_or_fail(s, ev->client, MB_IPC_KEY_UP,
                         body, len, "key_up");

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
    if (setenv("MOONBASE_BUNDLE_ID", "show.blizzard.inputtest", 1) != 0)
        return 11;
    if (setenv("MOONBASE_BUNDLE_VERSION", "0.1.0", 1) != 0) return 12;

    int rc = moonbase_init(0, NULL);
    if (rc != 0) return 20;

    mb_window_desc_t desc = {
        .version       = MOONBASE_WINDOW_DESC_VERSION,
        .title         = "input-test",
        .width_points  = REQUESTED_W,
        .height_points = REQUESTED_H,
        .render_mode   = MOONBASE_RENDER_CAIRO,
        .flags         = 0,
    };
    mb_window_t *win = moonbase_window_create(&desc);
    if (!win) return 21;

    mb_event_t ev = {0};
    if (!expect(MB_EV_WINDOW_FOCUSED, win, &ev)) return 22;
    if (!ev.focus.has_focus) {
        fprintf(stderr, "child: focus event but has_focus=false\n");
        return 23;
    }

    if (!expect(MB_EV_KEY_DOWN, win, &ev)) return 24;
    if (ev.key.keycode != TEST_KEYCODE
            || ev.key.modifiers != TEST_MOD_DOWN
            || ev.key.is_repeat) {
        fprintf(stderr,
                "child: key_down fields wrong code=0x%x mods=0x%x rep=%d\n",
                ev.key.keycode, ev.key.modifiers, ev.key.is_repeat);
        return 25;
    }

    if (!expect(MB_EV_KEY_UP, win, &ev)) return 26;
    if (ev.key.keycode != TEST_KEYCODE
            || ev.key.modifiers != TEST_MOD_UP
            || ev.key.is_repeat) {
        fprintf(stderr,
                "child: key_up fields wrong code=0x%x mods=0x%x rep=%d\n",
                ev.key.keycode, ev.key.modifiers, ev.key.is_repeat);
        return 27;
    }

    moonbase_window_close(win);
    moonbase_quit(0);
    return 0;
}

int main(void) {
    char dir_tmpl[] = "/tmp/mb-input.XXXXXX";
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

    printf("ok: FOCUSED + KEY_DOWN + KEY_UP → events delivered\n");
    return 0;
}
