// CopyCatOS — by Kyle Blizzard at Blizzard.show

// hello-handshake.c — Phase C slice 1 end-to-end test.
//
// Spins up a minimum MoonRock mock compositor in a child process,
// then runs moonbase_init / moonbase_quit in the parent against
// that mock. Asserts the full HELLO / WELCOME / BYE cycle on the
// wire: the child parses HELLO, replies with WELCOME, and expects
// BYE on shutdown.
//
// This is a client-side test only — once moonrock grows its real
// server-side handshake in a later slice, this test is replaced
// by a moonrock-linked runner and the mock compositor here is
// retired. The mock does not attempt to be a faithful protocol
// implementation; it validates just enough to prove the client
// side is correctly framed.

#include "CopyCatAppKit.h"
#include "moonbase/ipc/kinds.h"
#include "../src/ipc/cbor.h"
#include "../src/ipc/frame.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// Printf-style test-fail macro. Written with a mandatory format string
// so the ", ##__VA_ARGS__" GNU extension isn't needed — pedantic C11
// rejects a literal "fmt" with zero trailing arguments otherwise.
#define FAIL(...) do { \
    fprintf(stderr, "FAIL: " __VA_ARGS__); \
    fputc('\n', stderr); \
    exit(1); \
} while (0)

// ---------------------------------------------------------------------
// Server side — the child process
// ---------------------------------------------------------------------

static int mock_server(const char *sock_path) {
    int listen_fd = mb_ipc_frame_listen(sock_path);
    if (listen_fd < 0) {
        fprintf(stderr, "mock: listen failed (%d)\n", listen_fd);
        return 1;
    }

    int cfd = mb_ipc_frame_accept(listen_fd);
    close(listen_fd);
    if (cfd < 0) {
        fprintf(stderr, "mock: accept failed (%d)\n", cfd);
        return 1;
    }

    // Expect HELLO.
    uint16_t  kind = 0;
    uint8_t  *body = NULL;
    size_t    body_len = 0;
    size_t    nfds = 0;
    int rc = mb_ipc_frame_recv(cfd, &kind, &body, &body_len, NULL, &nfds);
    if (rc != 1) {
        fprintf(stderr, "mock: hello recv rc=%d\n", rc);
        return 1;
    }
    if (kind != MB_IPC_HELLO) {
        fprintf(stderr, "mock: expected HELLO kind, got 0x%04x\n", kind);
        return 1;
    }

    // Quick sanity-check: the body is a CBOR map with at least key 1
    // present. We don't enforce every field — we just assert that a
    // well-formed CBOR encoding reaches us.
    mb_cbor_r_t r;
    mb_cbor_r_init(&r, body, body_len);
    uint64_t pairs = 0;
    if (!mb_cbor_r_map_begin(&r, &pairs) || pairs == 0) {
        fprintf(stderr, "mock: hello body not a map\n");
        return 1;
    }
    free(body);

    // Send WELCOME.
    mb_cbor_w_t w;
    mb_cbor_w_init_grow(&w, 64);
    mb_cbor_w_map_begin(&w, 4);
    mb_cbor_w_key(&w, 1); mb_cbor_w_uint(&w, MOONBASE_API_VERSION);
    mb_cbor_w_key(&w, 2); mb_cbor_w_uint(&w, 1u << 20);
    mb_cbor_w_key(&w, 3); mb_cbor_w_array_begin(&w, 2);
                          mb_cbor_w_tstr(&w, "cairo");
                          mb_cbor_w_tstr(&w, "gl");
    mb_cbor_w_key(&w, 4); mb_cbor_w_uint(&w, 0);
    size_t   wl = 0;
    uint8_t *wb = mb_cbor_w_finish(&w, &wl);
    if (!wb) {
        fprintf(stderr, "mock: welcome encode failed\n");
        return 1;
    }
    rc = mb_ipc_frame_send(cfd, MB_IPC_WELCOME, wb, wl, NULL, 0);
    free(wb);
    if (rc < 0) {
        fprintf(stderr, "mock: welcome send rc=%d\n", rc);
        return 1;
    }

    // Expect BYE.
    kind = 0; body = NULL; body_len = 0; nfds = 0;
    rc = mb_ipc_frame_recv(cfd, &kind, &body, &body_len, NULL, &nfds);
    if (rc != 1) {
        fprintf(stderr, "mock: bye recv rc=%d\n", rc);
        return 1;
    }
    free(body);
    if (kind != MB_IPC_BYE) {
        fprintf(stderr, "mock: expected BYE, got 0x%04x\n", kind);
        return 1;
    }

    close(cfd);
    return 0;
}

// ---------------------------------------------------------------------
// Client side — the parent process
// ---------------------------------------------------------------------

int main(void) {
    // Private per-run socket path under /tmp. XDG_RUNTIME_DIR would
    // normally point at /run/user/<uid>, but `meson test` sandboxes
    // that out — /tmp is the one directory both sides can reach.
    char dir_template[] = "/tmp/mb-hs.XXXXXX";
    char *dir = mkdtemp(dir_template);
    if (!dir) FAIL("mkdtemp: %s", strerror(errno));

    char sock_path[256];
    snprintf(sock_path, sizeof(sock_path), "%s/moonbase.sock", dir);

    // Point moonbase at our private dir so mb_conn_open picks the
    // right socket without ever touching the real XDG path.
    if (setenv("XDG_RUNTIME_DIR", dir, 1) != 0) {
        FAIL("setenv XDG_RUNTIME_DIR: %s", strerror(errno));
    }

    pid_t pid = fork();
    if (pid < 0) FAIL("fork: %s", strerror(errno));

    if (pid == 0) {
        int rc = mock_server(sock_path);
        _exit(rc);
    }

    // Parent: give the child a moment to bind. A 50ms poll loop is
    // enough — the child hits listen() within microseconds.
    for (int i = 0; i < 50; i++) {
        if (access(sock_path, F_OK) == 0) break;
        usleep(1000);
    }

    int rc = moonbase_init(0, NULL);
    if (rc != 0) {
        FAIL("moonbase_init rc=%d (%s)",
             rc, moonbase_error_string(moonbase_last_error()));
    }

    moonbase_quit(0);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) FAIL("waitpid: %s", strerror(errno));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        FAIL("mock child exit status = %d", status);
    }

    unlink(sock_path);
    rmdir(dir);

    printf("ok: HELLO/WELCOME/BYE\n");
    return 0;
}
