// CopyCatOS — by Kyle Blizzard at Blizzard.show

// window-commit.c — Phase C slice 3c.1 test.
//
// Exercises the client-side shm-backed Cairo path and the WINDOW_COMMIT
// fd round-trip. The parent stands up a real mb_server_t, replies to
// MB_IPC_WINDOW_CREATE with a canned reply at scale 1.0 (so pixel == point
// for the arithmetic below), then waits for the child's WINDOW_COMMIT.
//
// Child does:
//   1. moonbase_init
//   2. moonbase_window_create(640x400 points, Cairo)
//   3. moonbase_window_cairo -> cairo_t *
//   4. paint solid red over the whole surface
//   5. moonbase_window_commit
//   6. moonbase_window_close + moonbase_quit
//
// Parent asserts on WINDOW_COMMIT:
//   * frame_fd_count == 1
//   * CBOR body carries window_id/width/height/stride
//   * the shm is >= stride * height bytes
//   * mmap the fd and confirm the first ARGB32 pixel is our red
//
// At scale 1.0 the expected pixel size is exactly 640x400, stride is
// 640 * 4 = 2560 (already aligned), so this test pins the whole chain
// without any rounding slop.

#include "CopyCatAppKit.h"
#include "../src/server/server.h"
#include "../src/ipc/cbor.h"
#include "moonbase/ipc/kinds.h"

#include <cairo/cairo.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
#define REQUESTED_W        640
#define REQUESTED_H        400

// ARGB32 red, premultiplied: A=255, R=255, G=0, B=0 => little-endian
// memory layout 0x00 0x00 0xFF 0xFF on a typical x86_64 / aarch64 host.
// cairo_image_surface_create_for_data with CAIRO_FORMAT_ARGB32 stores
// as native-endian u32 per pixel, so we read back the u32.
#define RED_PIXEL 0xFFFF0000u

static int g_connected = 0;
static int g_disconnected = 0;
static int g_window_create_seen = 0;
static int g_window_close_seen = 0;
static int g_window_commit_seen = 0;
static int g_commit_ok = 0;   // set to 1 after all commit-side checks pass

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

// Parse the commit body into the fields we care about for the test.
static bool parse_commit(const uint8_t *body, size_t body_len,
                         uint32_t *wid, uint32_t *wpx, uint32_t *hpx,
                         uint32_t *stride) {
    mb_cbor_r_t r;
    mb_cbor_r_init(&r, body, body_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs)) return false;
    *wid = *wpx = *hpx = *stride = 0;
    for (uint64_t i = 0; i < pairs; i++) {
        uint64_t k = 0, v = 0;
        if (!mb_cbor_r_uint(&r, &k)) return false;
        switch (k) {
            case 1: if (!mb_cbor_r_uint(&r, &v)) return false;
                    *wid = (uint32_t)v; break;
            case 2: if (!mb_cbor_r_uint(&r, &v)) return false;
                    *wpx = (uint32_t)v; break;
            case 3: if (!mb_cbor_r_uint(&r, &v)) return false;
                    *hpx = (uint32_t)v; break;
            case 4: if (!mb_cbor_r_uint(&r, &v)) return false;
                    *stride = (uint32_t)v; break;
            default:
                if (!mb_cbor_r_skip(&r)) return false;
                break;
        }
    }
    return *wpx > 0 && *hpx > 0 && *stride > 0;
}

static void on_event(mb_server_t *s, const mb_server_event_t *ev, void *user) {
    (void)user;
    switch (ev->kind) {
        case MB_SERVER_EV_CONNECTED:
            g_connected++;
            break;
        case MB_SERVER_EV_FRAME:
            if (ev->frame_kind == MB_IPC_WINDOW_CREATE) {
                g_window_create_seen++;
                size_t body_len = 0;
                uint8_t *body = build_reply(EXPECTED_WINDOW_ID,
                                            EXPECTED_OUTPUT_ID,
                                            EXPECTED_SCALE,
                                            (uint32_t)REQUESTED_W,
                                            (uint32_t)REQUESTED_H,
                                            &body_len);
                if (!body) FAIL("build_reply OOM");
                int rc = mb_server_send(s, ev->client,
                                        MB_IPC_WINDOW_CREATE_REPLY,
                                        body, body_len, NULL, 0);
                free(body);
                if (rc != 0) FAIL("mb_server_send rc=%d", rc);
            } else if (ev->frame_kind == MB_IPC_WINDOW_COMMIT) {
                g_window_commit_seen++;

                if (ev->frame_fd_count != 1) {
                    FAIL("WINDOW_COMMIT: expected 1 fd, got %zu",
                         ev->frame_fd_count);
                }
                uint32_t wid = 0, wpx = 0, hpx = 0, stride = 0;
                if (!parse_commit(ev->frame_body, ev->frame_body_len,
                                  &wid, &wpx, &hpx, &stride)) {
                    FAIL("WINDOW_COMMIT: malformed body");
                }
                if (wid != EXPECTED_WINDOW_ID) {
                    FAIL("WINDOW_COMMIT: window_id %u != %u",
                         wid, EXPECTED_WINDOW_ID);
                }
                if (wpx != REQUESTED_W || hpx != REQUESTED_H) {
                    FAIL("WINDOW_COMMIT: size %ux%u != %dx%d",
                         wpx, hpx, REQUESTED_W, REQUESTED_H);
                }
                // Stride must hold at least width*4 and be 4-byte aligned.
                if (stride < wpx * 4 || (stride & 3) != 0) {
                    FAIL("WINDOW_COMMIT: bad stride %u for width %u",
                         stride, wpx);
                }

                int fd = ev->frame_fds[0];
                struct stat st;
                if (fstat(fd, &st) != 0) {
                    FAIL("WINDOW_COMMIT: fstat %s", strerror(errno));
                }
                size_t need = (size_t)stride * (size_t)hpx;
                if ((size_t)st.st_size < need) {
                    FAIL("WINDOW_COMMIT: shm size %zu < %zu",
                         (size_t)st.st_size, need);
                }
                void *p = mmap(NULL, need, PROT_READ, MAP_SHARED, fd, 0);
                if (p == MAP_FAILED) {
                    FAIL("WINDOW_COMMIT: mmap %s", strerror(errno));
                }
                uint32_t px = *(const uint32_t *)p;
                munmap(p, need);

                if (px != RED_PIXEL) {
                    FAIL("WINDOW_COMMIT: first pixel 0x%08x != expected 0x%08x",
                         px, RED_PIXEL);
                }
                g_commit_ok = 1;
                fprintf(stderr,
                        "[test] commit ok: %ux%u stride=%u first_px=0x%08x\n",
                        wpx, hpx, stride, px);
            } else if (ev->frame_kind == MB_IPC_WINDOW_CLOSE) {
                g_window_close_seen++;
            } else {
                fprintf(stderr,
                        "[test] EV_FRAME unexpected kind=0x%04x len=%zu\n",
                        ev->frame_kind, ev->frame_body_len);
            }
            break;
        case MB_SERVER_EV_DISCONNECTED:
            g_disconnected++;
            break;
    }
}

static int run_client(const char *sock_dir) {
    if (setenv("XDG_RUNTIME_DIR", sock_dir, 1) != 0) return 10;
    if (setenv("MOONBASE_BUNDLE_ID", "show.blizzard.commit-test", 1) != 0) return 11;
    if (setenv("MOONBASE_BUNDLE_VERSION", "0.1.0", 1) != 0) return 12;

    int rc = moonbase_init(0, NULL);
    if (rc != 0) { fprintf(stderr, "child: init rc=%d\n", rc); return 20; }

    mb_window_desc_t desc = {
        .version       = MOONBASE_WINDOW_DESC_VERSION,
        .title         = "commit-test",
        .width_points  = REQUESTED_W,
        .height_points = REQUESTED_H,
        .render_mode   = MOONBASE_RENDER_CAIRO,
        .flags         = 0,
    };
    mb_window_t *win = moonbase_window_create(&desc);
    if (!win) { fprintf(stderr, "child: window_create failed %d\n",
                        moonbase_last_error()); return 30; }

    cairo_t *cr = (cairo_t *)moonbase_window_cairo(win);
    if (!cr) { fprintf(stderr, "child: window_cairo failed %d\n",
                       moonbase_last_error()); return 31; }

    // Paint the whole surface solid opaque red.
    cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, 1.0);
    cairo_paint(cr);

    rc = moonbase_window_commit(win);
    if (rc != 0) { fprintf(stderr, "child: commit rc=%d last=%d\n",
                           rc, moonbase_last_error()); return 32; }

    moonbase_window_close(win);
    moonbase_quit(0);
    return 0;
}

int main(void) {
    char dir_tmpl[] = "/tmp/mb-commit.XXXXXX";
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

    while (g_connected < 1 || g_window_create_seen < 1
        || g_window_commit_seen < 1 || !g_commit_ok
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
            FAIL("timeout: conn=%d create=%d commit=%d ok=%d close=%d disc=%d",
                 g_connected, g_window_create_seen,
                 g_window_commit_seen, g_commit_ok,
                 g_window_close_seen, g_disconnected);
        }
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) FAIL("waitpid: %s", strerror(errno));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        FAIL("child status %d exit=%d", status, WEXITSTATUS(status));
    }

    mb_server_close(srv);
    rmdir(dir);

    printf("ok: window_commit round-trip (shm pixel matches)\n");
    return 0;
}
