// CopyCatOS — by Kyle Blizzard at Blizzard.show

// server-smoke.c — Phase C slice 2 server-side test.
//
// Stands up an mb_server_t in the parent process, forks two child
// processes that each run moonbase_init against it, and verifies:
//   * both HELLOs arrive and trigger MB_SERVER_EV_CONNECTED
//   * both children receive WELCOME and init() returns 0
//   * both BYEs land and trigger MB_SERVER_EV_DISCONNECTED
//
// This test exercises the non-blocking server end-to-end without
// involving moonrock or X11 — pure libc + the MoonBase IPC stack.

#include "moonbase.h"
#include "../src/server/server.h"

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

// Two children — counters track both at once.
static int g_connected = 0;
static int g_disconnected = 0;
static int g_welcomed_children_expected = 2;

static void on_event(mb_server_t *s, const mb_server_event_t *ev, void *user) {
    (void)s; (void)user;
    switch (ev->kind) {
        case MB_SERVER_EV_CONNECTED:
            g_connected++;
            fprintf(stderr,
                    "[test] EV_CONNECTED client=%u bundle='%s' ver='%s' "
                    "pid=%u lang=%u api=%u\n",
                    ev->client, ev->hello.bundle_id, ev->hello.bundle_version,
                    ev->hello.pid, ev->hello.language, ev->hello.api_version);
            break;
        case MB_SERVER_EV_FRAME:
            fprintf(stderr,
                    "[test] EV_FRAME client=%u kind=0x%04x len=%zu\n",
                    ev->client, ev->frame_kind, ev->frame_body_len);
            break;
        case MB_SERVER_EV_DISCONNECTED:
            g_disconnected++;
            fprintf(stderr, "[test] EV_DISCONNECTED client=%u reason=%d\n",
                    ev->client, ev->disconnect_reason);
            break;
    }
}

// Run one HELLO/WELCOME/BYE round as a client. Executed in a child.
static int run_client(const char *sock_dir, int child_index) {
    if (setenv("XDG_RUNTIME_DIR", sock_dir, 1) != 0) return 1;
    if (setenv("MOONBASE_BUNDLE_ID", "show.blizzard.smoke", 1) != 0) return 1;
    if (setenv("MOONBASE_BUNDLE_VERSION", "0.1.0", 1) != 0) return 1;
    int rc = moonbase_init(0, NULL);
    if (rc != 0) {
        fprintf(stderr, "child %d: init rc=%d\n", child_index, rc);
        return 1;
    }
    moonbase_quit(0);
    return 0;
}

int main(void) {
    char dir_tmpl[] = "/tmp/mb-srv.XXXXXX";
    char *dir = mkdtemp(dir_tmpl);
    if (!dir) FAIL("mkdtemp: %s", strerror(errno));

    char sock_path[256];
    snprintf(sock_path, sizeof(sock_path), "%s/moonbase.sock", dir);

    mb_server_t *srv = NULL;
    int rc = mb_server_open(&srv, sock_path, on_event, NULL);
    if (rc != 0) FAIL("mb_server_open rc=%d", rc);

    // Fork two clients.
    pid_t pids[2] = {0, 0};
    for (int i = 0; i < g_welcomed_children_expected; i++) {
        pid_t pid = fork();
        if (pid < 0) FAIL("fork: %s", strerror(errno));
        if (pid == 0) {
            // Inherit but don't touch the server — mb_server_close
            // unlinks the socket, which would race with the parent.
            // _exit() reaps every inherited fd.
            _exit(run_client(dir, i));
        }
        pids[i] = pid;
    }

    // Event loop: poll + tick until both children connect and
    // disconnect, or we time out. The server closes the socket
    // promptly on BYE so a reasonable timeout here is ~1 second.
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const long timeout_ms = 2000;

    while (g_connected < 2 || g_disconnected < 2) {
        struct pollfd fds[16];
        size_t n = mb_server_get_pollfds(srv, fds, 16);

        int pr = poll(fds, (nfds_t)n, 50);
        if (pr < 0 && errno != EINTR) FAIL("poll: %s", strerror(errno));

        mb_server_tick(srv, fds, n);

        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000
                     + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed > timeout_ms) {
            FAIL("timeout: connected=%d disconnected=%d",
                 g_connected, g_disconnected);
        }
    }

    // Reap children and verify they exited 0.
    for (int i = 0; i < 2; i++) {
        int status = 0;
        if (waitpid(pids[i], &status, 0) < 0) FAIL("waitpid: %s", strerror(errno));
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            FAIL("child %d exited with status %d", i, status);
        }
    }

    mb_server_close(srv);
    rmdir(dir);

    if (g_connected != 2 || g_disconnected != 2) {
        FAIL("final counters: connected=%d disconnected=%d",
             g_connected, g_disconnected);
    }

    printf("ok: 2 clients handshook + disconnected cleanly\n");
    return 0;
}
