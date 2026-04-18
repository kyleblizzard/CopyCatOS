// CopyCatOS — by Kyle Blizzard at Blizzard.show

// frame.c — TLV frame I/O over AF_UNIX, with SCM_RIGHTS fd passing.
//
// Sends and receives one whole frame per call. Blocking. Restarts
// on EINTR. Peer credential check is enforced in accept() —
// MoonRock rejects any client whose EUID doesn't match its own.
//
// Wire format recap (IPC.md §2):
//   u32 length | u16 kind | body
// length = sizeof(kind) + sizeof(body). Both length and kind are
// transmitted big-endian. The body is CBOR (see cbor.c).

#include "frame.h"
#include "moonbase.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

// darwin doesn't define SOCK_CLOEXEC; we fall back to post-open
// fcntl on that platform. Linux defines it in <sys/socket.h>.
#ifndef SOCK_CLOEXEC
#  define SOCK_CLOEXEC 0
#endif

// Linux has SO_PEERCRED returning `struct ucred`; darwin does not.
// We compile on darwin for local dev and use LOCAL_PEERCRED there
// (available but not needed for Phase C testing on darwin). The
// cheap cross-platform fallback: if neither is available, just
// accept the connection — MoonRock only runs on Linux in production.
#if defined(__linux__)
#  include <sys/socket.h>
#endif

// ---------------------------------------------------------------------
// Low-level I/O helpers
// ---------------------------------------------------------------------

// Keep writing until `n` bytes are sent or the connection fails.
// Returns 0 on success, negative mb_error_t on failure.
static int write_all(int fd, const uint8_t *buf, size_t n) {
    while (n > 0) {
        ssize_t got = send(fd, buf, n, MSG_NOSIGNAL);
        if (got < 0) {
            if (errno == EINTR) continue;
            return MB_EIPC;
        }
        if (got == 0) return MB_EIPC;
        buf += (size_t)got;
        n   -= (size_t)got;
    }
    return 0;
}

// Read exactly `n` bytes. Returns 0 on success, MB_ENOTFOUND on
// clean EOF (peer closed cleanly with zero bytes remaining),
// MB_EIPC on I/O failure, MB_EPROTO if EOF mid-frame.
static int read_exact(int fd, uint8_t *buf, size_t n, bool already_started) {
    while (n > 0) {
        ssize_t got = recv(fd, buf, n, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            return MB_EIPC;
        }
        if (got == 0) {
            return already_started ? MB_EPROTO : MB_ENOTFOUND;
        }
        buf += (size_t)got;
        n   -= (size_t)got;
        already_started = true;
    }
    return 0;
}

static inline void be_pack32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static inline void be_pack16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static inline uint32_t be_unpack32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16
         | (uint32_t)p[2] << 8  | (uint32_t)p[3];
}
static inline uint16_t be_unpack16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

// ---------------------------------------------------------------------
// Send
// ---------------------------------------------------------------------

int mb_ipc_frame_send(int fd,
                      uint16_t kind,
                      const uint8_t *body, size_t body_len,
                      const int *fds, size_t nfds) {
    if (body_len + 2 > MB_IPC_FRAME_MAX_LEN) return MB_EPROTO;
    if (nfds > MB_IPC_FRAME_MAX_FDS) return MB_EINVAL;

    uint8_t header[6];
    be_pack32(header, (uint32_t)(body_len + 2));   // length field
    be_pack16(header + 4, kind);

    // If we have fds, we must attach them to the first sendmsg of
    // the frame so they ride with the header. Subsequent body bytes
    // go through plain send(). We always send the header in one
    // sendmsg to make the ancillary path simpler.
    struct iovec iov_header = { .iov_base = header, .iov_len = sizeof(header) };

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov_header;
    msg.msg_iovlen = 1;

    // CMSG buffer sized for MB_IPC_FRAME_MAX_FDS fds. Empty when
    // nfds == 0. Using CMSG_SPACE ensures correct alignment.
    union {
        char raw[CMSG_SPACE(sizeof(int) * MB_IPC_FRAME_MAX_FDS)];
        struct cmsghdr align;
    } cmsg_buf;

    if (nfds > 0) {
        memset(&cmsg_buf, 0, sizeof(cmsg_buf));
        msg.msg_control    = cmsg_buf.raw;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type  = SCM_RIGHTS;
        c->cmsg_len   = CMSG_LEN(sizeof(int) * nfds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * nfds);
    }

    for (;;) {
        ssize_t got = sendmsg(fd, &msg, MSG_NOSIGNAL);
        if (got < 0) {
            if (errno == EINTR) continue;
            return MB_EIPC;
        }
        if (got < (ssize_t)sizeof(header)) {
            // Short send on the first sendmsg is a kernel bug for
            // AF_UNIX SOCK_STREAM of this size; bail.
            return MB_EIPC;
        }
        break;
    }

    if (body_len > 0) {
        int rc = write_all(fd, body, body_len);
        if (rc != 0) return rc;
    }
    return 0;
}

// ---------------------------------------------------------------------
// Recv
// ---------------------------------------------------------------------

int mb_ipc_frame_recv(int fd,
                      uint16_t *out_kind,
                      uint8_t **out_body, size_t *out_body_len,
                      int *fds, size_t *io_nfds) {
    uint8_t header[6];

    // The header rides along with the SCM_RIGHTS attachment (sender
    // puts the ancillary data on the first sendmsg), so we must use
    // recvmsg to get both. If the peer sent no fds, cmsg_len is 0
    // and we return *io_nfds = 0.
    struct iovec iov = { .iov_base = header, .iov_len = sizeof(header) };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    union {
        char raw[CMSG_SPACE(sizeof(int) * MB_IPC_FRAME_MAX_FDS)];
        struct cmsghdr align;
    } cmsg_buf;
    memset(&cmsg_buf, 0, sizeof(cmsg_buf));
    msg.msg_control    = cmsg_buf.raw;
    msg.msg_controllen = sizeof(cmsg_buf.raw);

    size_t fd_cap = io_nfds ? *io_nfds : 0;
    if (io_nfds) *io_nfds = 0;

    size_t header_got = 0;
    for (;;) {
        ssize_t got;
        do {
            got = recvmsg(fd, &msg, 0);
        } while (got < 0 && errno == EINTR);

        if (got < 0) return MB_EIPC;
        if (got == 0) {
            if (header_got == 0) return 0;   // clean EOF
            return MB_EPROTO;
        }
        header_got += (size_t)got;

        // Harvest any fds attached. On the first recvmsg only —
        // subsequent iovec refills do not carry ancillary data.
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                size_t n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                int *src = (int *)CMSG_DATA(c);
                size_t copy = n;
                if (copy > fd_cap) copy = fd_cap;
                for (size_t i = 0; i < copy; i++) fds[i] = src[i];
                for (size_t i = copy; i < n; i++) close(src[i]);   // peer overshot
                if (io_nfds) *io_nfds = copy;
                if (n > fd_cap) return MB_EPROTO;
            }
        }
        // Clear ancillary data for subsequent recvmsg calls — we
        // only care about the first batch.
        msg.msg_control    = NULL;
        msg.msg_controllen = 0;

        if (header_got >= sizeof(header)) break;

        // Continue reading the rest of the header.
        iov.iov_base = header + header_got;
        iov.iov_len  = sizeof(header) - header_got;
    }

    uint32_t length = be_unpack32(header);
    uint16_t kind   = be_unpack16(header + 4);

    if (length < 2 || length > MB_IPC_FRAME_MAX_LEN) return MB_EPROTO;
    size_t body_len = length - 2;

    uint8_t *body = NULL;
    if (body_len > 0) {
        body = (uint8_t *)malloc(body_len);
        if (!body) return MB_ENOMEM;
        int rc = read_exact(fd, body, body_len, true);
        if (rc != 0) { free(body); return rc; }
    }

    *out_kind = kind;
    *out_body = body;
    *out_body_len = body_len;
    return 1;   // 1 == a frame was read
}

// ---------------------------------------------------------------------
// Socket creation
// ---------------------------------------------------------------------

int mb_ipc_frame_listen(const char *path) {
    if (!path) return MB_EINVAL;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    size_t plen = strlen(path);
    if (plen >= sizeof(sa.sun_path)) return MB_EINVAL;
    memcpy(sa.sun_path, path, plen + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return MB_EIPC;
    if (SOCK_CLOEXEC == 0) {
        // No SOCK_CLOEXEC on this platform — set it via fcntl.
        int flags = fcntl(fd, F_GETFD);
        if (flags >= 0) (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }

    // Drop any stale socket file left behind by an earlier crash.
    // A live second MoonRock binding the same path is a user error
    // — the second bind() will fail with EADDRINUSE and we report.
    unlink(path);

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return MB_EIPC;
    }
    // 0600 per IPC.md §1. bind() creates with umask-defaulted mode;
    // explicitly chmod to lock it down.
    (void)chmod(path, 0600);

    if (listen(fd, 16) < 0) {
        close(fd);
        unlink(path);
        return MB_EIPC;
    }
    return fd;
}

int mb_ipc_frame_accept(int listen_fd) {
    int cfd;
    do {
        cfd = accept(listen_fd, NULL, NULL);
    } while (cfd < 0 && errno == EINTR);
    if (cfd < 0) return MB_EIPC;

#if defined(__linux__) && defined(SO_PEERCRED)
    struct ucred cred;
    socklen_t cl = sizeof(cred);
    if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &cl) == 0) {
        if (cred.uid != geteuid()) {
            close(cfd);
            return MB_EPERM;
        }
    }
#endif
    int flags = fcntl(cfd, F_GETFD);
    if (flags >= 0) (void)fcntl(cfd, F_SETFD, flags | FD_CLOEXEC);
    return cfd;
}

int mb_ipc_frame_connect(const char *path) {
    if (!path) return MB_EINVAL;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    size_t plen = strlen(path);
    if (plen >= sizeof(sa.sun_path)) return MB_EINVAL;
    memcpy(sa.sun_path, path, plen + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return MB_EIPC;
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);

    for (;;) {
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) return fd;
        if (errno == EINTR) continue;
        close(fd);
        return MB_ENOTFOUND;
    }
}
