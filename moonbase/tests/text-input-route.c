// CopyCatOS — by Kyle Blizzard at Blizzard.show

// text-input-route.c — TEXT_INPUT translator test.
//
// Companion to input-route.c: pins the MB_IPC_TEXT_INPUT →
// MB_EV_TEXT_INPUT path. Parent stands up a real mb_server_t, replies
// to the child's MB_IPC_WINDOW_CREATE, then sends four canned frames
// in order:
//   1. MB_IPC_WINDOW_FOCUSED  has_focus=true
//   2. MB_IPC_TEXT_INPUT      text="a"
//   3. MB_IPC_TEXT_INPUT      text="Hello"
//   4. MB_IPC_TEXT_INPUT      text=""           (empty — must be dropped)
//
// The child's moonbase_wait_event loop asserts they arrive in the same
// order as MB_EV_WINDOW_FOCUSED, MB_EV_TEXT_INPUT("a"),
// MB_EV_TEXT_INPUT("Hello") — each with the correct window pointer and
// a non-NULL, NUL-terminated text pointer that still reads back the
// right bytes. The empty-text frame must NOT produce an event; a short
// wait after "Hello" times out cleanly.
//
// Invariants exercised:
//   * MB_IPC_TEXT_INPUT → MB_EV_TEXT_INPUT with ev.text.text pointing
//     at a heap-owned UTF-8 string for the documented lifetime rule
//     (valid until next pump call).
//   * Multiple back-to-back TEXT_INPUT events — the second one makes
//     the first's pointer unsafe, confirming the "until next pump"
//     contract and that the library frees the prior buffer without
//     leaking or double-freeing.
//   * Empty-text frames are dropped silently by the translator (no
//     spurious event ever surfaces).
//
// Server-side XLookupString emission is covered separately by the
// Xvfb end-to-end run, not by this unit test.

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

#define EXPECTED_WINDOW_ID   0x2345u
#define EXPECTED_OUTPUT_ID   0x0001u
#define EXPECTED_SCALE       1.0
#define REQUESTED_W          640
#define REQUESTED_H          480

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

static uint8_t *build_text_input(uint32_t window_id, const char *text,
                                 size_t *out_len) {
    mb_cbor_w_t wr;
    mb_cbor_w_init_grow(&wr, 32);
    mb_cbor_w_map_begin(&wr, 3);
    mb_cbor_w_key(&wr, 1); mb_cbor_w_uint(&wr, window_id);
    mb_cbor_w_key(&wr, 2); mb_cbor_w_tstr(&wr, text);
    mb_cbor_w_key(&wr, 3); mb_cbor_w_uint(&wr, 0);
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

            body = build_text_input(EXPECTED_WINDOW_ID, "a", &len);
            send_or_fail(s, ev->client, MB_IPC_TEXT_INPUT,
                         body, len, "text_input_a");

            body = build_text_input(EXPECTED_WINDOW_ID, "Hello", &len);
            send_or_fail(s, ev->client, MB_IPC_TEXT_INPUT,
                         body, len, "text_input_hello");

            body = build_text_input(EXPECTED_WINDOW_ID, "", &len);
            send_or_fail(s, ev->client, MB_IPC_TEXT_INPUT,
                         body, len, "text_input_empty");

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
    if (setenv("MOONBASE_BUNDLE_ID", "show.blizzard.textinputtest", 1) != 0)
        return 11;
    if (setenv("MOONBASE_BUNDLE_VERSION", "0.1.0", 1) != 0) return 12;

    int rc = moonbase_init(0, NULL);
    if (rc != 0) return 20;

    mb_window_desc_t desc = {
        .version       = MOONBASE_WINDOW_DESC_VERSION,
        .title         = "text-input-test",
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

    if (!expect(MB_EV_TEXT_INPUT, win, &ev)) return 24;
    if (!ev.text.text) {
        fprintf(stderr, "child: text_input NULL text\n");
        return 25;
    }
    if (strcmp(ev.text.text, "a") != 0) {
        fprintf(stderr, "child: text_input[0] got '%s' want 'a'\n",
                ev.text.text);
        return 26;
    }

    // Pull the next event. This pump call is exactly what the docs
    // promise invalidates the previous ev.text.text pointer. We must
    // not reach back into the old text — the ASAN build will catch it
    // if we do.
    if (!expect(MB_EV_TEXT_INPUT, win, &ev)) return 27;
    if (!ev.text.text) {
        fprintf(stderr, "child: text_input[1] NULL text\n");
        return 28;
    }
    if (strcmp(ev.text.text, "Hello") != 0) {
        fprintf(stderr, "child: text_input[1] got '%s' want 'Hello'\n",
                ev.text.text);
        return 29;
    }

    // The empty-text frame sent by the parent must be dropped by the
    // translator. A short wait should time out cleanly with no extra
    // event. If anything arrives here the translator is wrong.
    mb_event_t extra = {0};
    int extra_rc = moonbase_wait_event(&extra, 200);
    if (extra_rc != 0) {
        fprintf(stderr,
                "child: expected timeout after empty-text drop, got rc=%d kind=%d\n",
                extra_rc, (int)extra.kind);
        return 30;
    }

    moonbase_window_close(win);
    moonbase_quit(0);
    return 0;
}

int main(void) {
    char dir_tmpl[] = "/tmp/mb-textinput.XXXXXX";
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

    printf("ok: FOCUSED + two TEXT_INPUT frames → events delivered\n");
    return 0;
}
