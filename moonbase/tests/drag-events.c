// CopyCatOS — by Kyle Blizzard at Blizzard.show

// drag-events.c — drag-into-window translator test.
//
// Pins MB_IPC_DRAG_ENTER / OVER / LEAVE / DROP → MB_EV_DRAG_* for every
// shape of payload the wire can carry. Parent stands up a real server,
// replies to the child's WINDOW_CREATE, then sends six canned frames:
//
//   1. DRAG_ENTER   x=10, y=20, mods=SHIFT,
//                   uris=["file:///foo.txt", "file:///bar.png"]
//   2. DRAG_OVER    x=30, y=40, mods=SHIFT
//   3. DRAG_DROP    x=30, y=40, mods=SHIFT,
//                   uris=["file:///foo.txt", "file:///bar.png"]
//   4. DRAG_ENTER   x=0,  y=0,  mods=0, uris=[]         (zero-length list)
//   5. DRAG_LEAVE
//   6. DRAG_OVER    x=5,  y=5,  mods=0                  (no uris; skip case)
//
// The child's wait_event loop asserts all six land as the right event
// kind, with the right window pointer, coordinates, modifiers, and —
// crucially — that ENTER / DROP deliver a non-NULL uris array of the
// advertised count whose strings still compare equal, while OVER and
// LEAVE deliver uri_count=0 and uris=NULL. Stresses the lifetime
// contract too: by the time a second ENTER or DROP arrives, the prior
// payload's pointer must already be reclaimed, but that's covered by
// the ASAN build finding no use-after-free if anything breaks.

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

#define EXPECTED_WINDOW_ID   0x4455u
#define EXPECTED_OUTPUT_ID   0x0001u
#define EXPECTED_SCALE       1.0
#define REQUESTED_W          800
#define REQUESTED_H          600

static int g_connected          = 0;
static int g_disconnected       = 0;
static int g_window_create_seen = 0;
static int g_all_sent           = 0;

static uint8_t *build_reply(size_t *out_len) {
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

// ENTER / DROP body: 1:wid 2:x 3:y 4:mods 5:array<tstr> 6:ts
static uint8_t *build_drag_with_uris(int x, int y, uint32_t modifiers,
                                     const char *const *uris,
                                     size_t count, size_t *out_len) {
    mb_cbor_w_t wr;
    mb_cbor_w_init_grow(&wr, 64);
    mb_cbor_w_map_begin(&wr, 6);
    mb_cbor_w_key(&wr, 1); mb_cbor_w_uint(&wr, EXPECTED_WINDOW_ID);
    mb_cbor_w_key(&wr, 2); mb_cbor_w_int (&wr, (int64_t)x);
    mb_cbor_w_key(&wr, 3); mb_cbor_w_int (&wr, (int64_t)y);
    mb_cbor_w_key(&wr, 4); mb_cbor_w_uint(&wr, modifiers);
    mb_cbor_w_key(&wr, 5);
    mb_cbor_w_array_begin(&wr, count);
    for (size_t i = 0; i < count; i++) mb_cbor_w_tstr(&wr, uris[i]);
    mb_cbor_w_key(&wr, 6); mb_cbor_w_uint(&wr, 0);
    if (!mb_cbor_w_ok(&wr)) { mb_cbor_w_drop(&wr); return NULL; }
    return mb_cbor_w_finish(&wr, out_len);
}

// OVER body: 1:wid 2:x 3:y 4:mods 6:ts  (no uris)
static uint8_t *build_drag_over(int x, int y, uint32_t modifiers,
                                size_t *out_len) {
    mb_cbor_w_t wr;
    mb_cbor_w_init_grow(&wr, 32);
    mb_cbor_w_map_begin(&wr, 5);
    mb_cbor_w_key(&wr, 1); mb_cbor_w_uint(&wr, EXPECTED_WINDOW_ID);
    mb_cbor_w_key(&wr, 2); mb_cbor_w_int (&wr, (int64_t)x);
    mb_cbor_w_key(&wr, 3); mb_cbor_w_int (&wr, (int64_t)y);
    mb_cbor_w_key(&wr, 4); mb_cbor_w_uint(&wr, modifiers);
    mb_cbor_w_key(&wr, 6); mb_cbor_w_uint(&wr, 0);
    if (!mb_cbor_w_ok(&wr)) { mb_cbor_w_drop(&wr); return NULL; }
    return mb_cbor_w_finish(&wr, out_len);
}

// LEAVE body: 1:wid 6:ts
static uint8_t *build_drag_leave(size_t *out_len) {
    mb_cbor_w_t wr;
    mb_cbor_w_init_grow(&wr, 16);
    mb_cbor_w_map_begin(&wr, 2);
    mb_cbor_w_key(&wr, 1); mb_cbor_w_uint(&wr, EXPECTED_WINDOW_ID);
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
                break;
            }
            g_window_create_seen++;

            size_t len = 0;
            uint8_t *body = build_reply(&len);
            send_or_fail(s, ev->client, MB_IPC_WINDOW_CREATE_REPLY,
                         body, len, "reply");

            const char *uris[2] = {
                "file:///foo.txt",
                "file:///bar.png",
            };

            body = build_drag_with_uris(10, 20, MB_MOD_SHIFT,
                                        uris, 2, &len);
            send_or_fail(s, ev->client, MB_IPC_DRAG_ENTER,
                         body, len, "enter");

            body = build_drag_over(30, 40, MB_MOD_SHIFT, &len);
            send_or_fail(s, ev->client, MB_IPC_DRAG_OVER,
                         body, len, "over");

            body = build_drag_with_uris(30, 40, MB_MOD_SHIFT,
                                        uris, 2, &len);
            send_or_fail(s, ev->client, MB_IPC_DRAG_DROP,
                         body, len, "drop");

            // Zero-length uri list — valid CBOR, translator must
            // deliver the event with uri_count=0 and uris pointing at
            // an empty array (or NULL — app treats both the same way).
            body = build_drag_with_uris(0, 0, 0, NULL, 0, &len);
            send_or_fail(s, ev->client, MB_IPC_DRAG_ENTER,
                         body, len, "enter-empty");

            body = build_drag_leave(&len);
            send_or_fail(s, ev->client, MB_IPC_DRAG_LEAVE,
                         body, len, "leave");

            body = build_drag_over(5, 5, 0, &len);
            send_or_fail(s, ev->client, MB_IPC_DRAG_OVER,
                         body, len, "over-tail");

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
    if (setenv("MOONBASE_BUNDLE_ID", "show.blizzard.dragtest", 1) != 0)
        return 11;
    if (setenv("MOONBASE_BUNDLE_VERSION", "0.1.0", 1) != 0) return 12;

    if (moonbase_init(0, NULL) != 0) return 20;

    mb_window_desc_t desc = {
        .version       = MOONBASE_WINDOW_DESC_VERSION,
        .title         = "drag-test",
        .width_points  = REQUESTED_W,
        .height_points = REQUESTED_H,
        .render_mode   = MOONBASE_RENDER_CAIRO,
        .flags         = 0,
    };
    mb_window_t *win = moonbase_window_create(&desc);
    if (!win) return 21;

    mb_event_t ev = {0};

    // 1. DRAG_ENTER with two uris
    if (!expect(MB_EV_DRAG_ENTER, win, &ev)) return 22;
    if (ev.drag.x != 10 || ev.drag.y != 20) {
        fprintf(stderr, "enter: xy=%d,%d\n", ev.drag.x, ev.drag.y); return 23;
    }
    if (ev.drag.modifiers != MB_MOD_SHIFT) {
        fprintf(stderr, "enter: mods=%u\n", ev.drag.modifiers); return 24;
    }
    if (ev.drag.uri_count != 2 || ev.drag.uris == NULL) {
        fprintf(stderr, "enter: count=%u uris=%p\n",
                ev.drag.uri_count, (const void *)ev.drag.uris);
        return 25;
    }
    if (strcmp(ev.drag.uris[0], "file:///foo.txt") != 0 ||
        strcmp(ev.drag.uris[1], "file:///bar.png") != 0) {
        fprintf(stderr, "enter: uri mismatch ['%s','%s']\n",
                ev.drag.uris[0], ev.drag.uris[1]);
        return 26;
    }

    // 2. DRAG_OVER — no uris
    if (!expect(MB_EV_DRAG_OVER, win, &ev)) return 27;
    if (ev.drag.x != 30 || ev.drag.y != 40) return 28;
    if (ev.drag.uri_count != 0 || ev.drag.uris != NULL) {
        fprintf(stderr, "over: spurious uri payload count=%u uris=%p\n",
                ev.drag.uri_count, (const void *)ev.drag.uris);
        return 29;
    }

    // 3. DRAG_DROP with two uris — a second pump has happened since
    // ENTER, so the ENTER's pointer is now dangling; we only read the
    // DROP's fresh pointer.
    if (!expect(MB_EV_DRAG_DROP, win, &ev)) return 30;
    if (ev.drag.x != 30 || ev.drag.y != 40) return 31;
    if (ev.drag.uri_count != 2 ||
        strcmp(ev.drag.uris[0], "file:///foo.txt") != 0 ||
        strcmp(ev.drag.uris[1], "file:///bar.png") != 0) {
        return 32;
    }

    // 4. DRAG_ENTER with zero-length uri list
    if (!expect(MB_EV_DRAG_ENTER, win, &ev)) return 33;
    if (ev.drag.uri_count != 0) {
        fprintf(stderr, "enter-empty: count=%u\n", ev.drag.uri_count);
        return 34;
    }

    // 5. DRAG_LEAVE — just window + zero payload
    if (!expect(MB_EV_DRAG_LEAVE, win, &ev)) return 35;
    if (ev.drag.uri_count != 0 || ev.drag.uris != NULL) return 36;

    // 6. DRAG_OVER tail
    if (!expect(MB_EV_DRAG_OVER, win, &ev)) return 37;
    if (ev.drag.x != 5 || ev.drag.y != 5) return 38;

    moonbase_window_close(win);
    moonbase_quit(0);
    return 0;
}

int main(void) {
    char dir_tmpl[] = "/tmp/mb-drag.XXXXXX";
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
    const long timeout_ms = 4000;

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
        FAIL("child exited status 0x%x (exit=%d)",
             status, WEXITSTATUS(status));
    }

    mb_server_close(srv);
    rmdir(dir);

    printf("ok: DRAG_ENTER/OVER/DROP/LEAVE frames → events delivered\n");
    return 0;
}
