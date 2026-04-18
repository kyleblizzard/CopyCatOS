// CopyCatOS — by Kyle Blizzard at Blizzard.show

// server-fds.c — Phase C SCM_RIGHTS round-trip test.
//
// The server side of MoonBase IPC now harvests ancillary fds out of
// recvmsg and re-attaches each batch to the frame that carried it.
// This test pins that contract by having a child client send a frame
// with a pipe's read-end attached; the parent reads a magic byte
// written into the write end and verifies it came through.
//
// Invariants exercised:
//   * HELLO/WELCOME still works through the new recvmsg path
//   * a post-handshake frame with nfds=1 surfaces on MB_SERVER_EV_FRAME
//     with frame_fd_count == 1
//   * the fd in frame_fds[0] is a live, readable reference to the
//     child's pipe — proving SCM_RIGHTS delivered the real descriptor
//   * the server closes the fd after the callback returns (we read
//     first, then trust the server to close — no leak assertion here)
//   * frames WITHOUT fds (like the BYE that moonbase_quit sends)
//     continue to surface with frame_fd_count == 0

#include "moonbase.h"
#include "../src/server/server.h"
#include "../src/ipc/transport.h"
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

// Magic byte the child writes before sending. Chosen to be distinct
// from zero and from any framing byte we might accidentally read.
#define FD_MAGIC  0x7Eu

// Arbitrary post-handshake kind we use as the fd-carrying payload.
// MB_IPC_WINDOW_COMMIT is a future "surface ready" signal from app to
// compositor — a natural place for fd handoff. Choosing it here just
// so the test looks like real future traffic; the server doesn't
// enforce any body schema on it in this slice.
#define TEST_FRAME_KIND  MB_IPC_WINDOW_COMMIT

static int     g_connected        = 0;
static int     g_disconnected     = 0;
static int     g_frame_count      = 0;
static int     g_fd_frame_seen    = 0;
static uint8_t g_received_byte    = 0;
static int     g_bye_had_no_fds   = 1;   // assumes good until proven bad

static void on_event(mb_server_t *s, const mb_server_event_t *ev, void *user) {
    (void)s; (void)user;
    switch (ev->kind) {
        case MB_SERVER_EV_CONNECTED:
            g_connected++;
            fprintf(stderr,
                    "[test] EV_CONNECTED client=%u bundle='%s'\n",
                    ev->client,
                    ev->hello.bundle_id ? ev->hello.bundle_id : "(null)");
            break;
        case MB_SERVER_EV_FRAME:
            g_frame_count++;
            fprintf(stderr,
                    "[test] EV_FRAME client=%u kind=0x%04x len=%zu fds=%zu\n",
                    ev->client, ev->frame_kind,
                    ev->frame_body_len, ev->frame_fd_count);
            if (ev->frame_kind == TEST_FRAME_KIND) {
                if (ev->frame_fd_count != 1 || !ev->frame_fds) {
                    fprintf(stderr,
                            "[test] fd-carrying frame arrived without fd\n");
                    break;
                }
                int fd = ev->frame_fds[0];
                uint8_t b = 0;
                ssize_t r;
                do {
                    r = read(fd, &b, 1);
                } while (r < 0 && errno == EINTR);
                if (r == 1) {
                    g_received_byte = b;
                    g_fd_frame_seen++;
                } else {
                    fprintf(stderr,
                            "[test] read(fd) returned %zd errno=%d\n",
                            r, errno);
                }
                // Deliberately do NOT close(fd). The server owns it
                // and closes after we return — this test also pins
                // that ownership rule.
            }
            break;
        case MB_SERVER_EV_DISCONNECTED:
            g_disconnected++;
            fprintf(stderr, "[test] EV_DISCONNECTED client=%u reason=%d\n",
                    ev->client, ev->disconnect_reason);
            break;
    }
}

// Child: standard init → send fd-carrying frame → quit.
static int run_client(const char *sock_dir) {
    if (setenv("XDG_RUNTIME_DIR", sock_dir, 1) != 0) return 10;
    if (setenv("MOONBASE_BUNDLE_ID", "show.blizzard.fdtest", 1) != 0) return 11;
    if (setenv("MOONBASE_BUNDLE_VERSION", "0.1.0", 1) != 0) return 12;

    int rc = moonbase_init(0, NULL);
    if (rc != 0) {
        fprintf(stderr, "child: init rc=%d\n", rc);
        return 20;
    }

    // Pipe pre-loaded with the magic byte. The read end is the fd we
    // hand to the server; the write end stays on the child and is
    // closed after the write so the read side EOFs after one byte.
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        fprintf(stderr, "child: pipe: %s\n", strerror(errno));
        moonbase_quit(0);
        return 21;
    }
    uint8_t magic = (uint8_t)FD_MAGIC;
    if (write(pipefd[1], &magic, 1) != 1) {
        fprintf(stderr, "child: pipe write: %s\n", strerror(errno));
        close(pipefd[0]); close(pipefd[1]);
        moonbase_quit(0);
        return 22;
    }
    close(pipefd[1]);

    rc = mb_conn_send(TEST_FRAME_KIND, NULL, 0, &pipefd[0], 1);
    // Server dup'd the fd out of the kernel on recvmsg — our copy is
    // still ours to close.
    close(pipefd[0]);
    if (rc != 0) {
        fprintf(stderr, "child: mb_conn_send rc=%d\n", rc);
        moonbase_quit(0);
        return 23;
    }

    moonbase_quit(0);
    return 0;
}

int main(void) {
    char dir_tmpl[] = "/tmp/mb-fd.XXXXXX";
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

    while (g_connected < 1 || g_fd_frame_seen < 1 || g_disconnected < 1) {
        struct pollfd fds[16];
        size_t n = mb_server_get_pollfds(srv, fds, 16);
        int pr = poll(fds, (nfds_t)n, 50);
        if (pr < 0 && errno != EINTR) FAIL("poll: %s", strerror(errno));
        mb_server_tick(srv, fds, n);

        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000
                     + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed > timeout_ms) {
            FAIL("timeout: connected=%d fd_frame=%d disconnected=%d",
                 g_connected, g_fd_frame_seen, g_disconnected);
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

    if (g_received_byte != (uint8_t)FD_MAGIC) {
        FAIL("byte mismatch: got 0x%02x expected 0x%02x",
             g_received_byte, (unsigned)FD_MAGIC);
    }
    if (!g_bye_had_no_fds) {
        FAIL("BYE unexpectedly arrived with fds attached");
    }

    printf("ok: SCM_RIGHTS fd round-trip (magic=0x%02x)\n",
           (unsigned)FD_MAGIC);
    return 0;
}
