// CopyCatOS — by Kyle Blizzard at Blizzard.show

// mb-launch-appimg — end-to-end test for slice 17-B: single-file
// `.app` mount, launch, unmount.
//
// Pipeline the test runs:
//   1. Build a fixture .appdev layout in a tmpdir (Info.appc +
//      Contents/CopyCatOS/run shell script).
//   2. mksquashfs Contents/ -> sqfs.img.
//   3. Concatenate: <placeholder stub> + sqfs.img + trailer -> `.app`.
//      The stub is 64 bytes of filler — moonbase-launch never exec's
//      the stub in this flow; the packager-written stub will come
//      online in slice 17-A.
//   4. Fork + execvp moonbase-launch against the `.app` with a
//      controlled $HOME / $XDG_RUNTIME_DIR.
//   5. After launch returns, assert:
//        * bwrap ran the inner script (marker file present)
//        * exit code propagated (42)
//        * bundle-id came through $MOONBASE_BUNDLE_ID
//        * the mount dir under XDG_RUNTIME_DIR/moonbase/mounts/ is
//          gone — proves fusermount -u + rmdir cleanup ran
//
// Skip (exit 77) when mksquashfs / squashfuse / fusermount / bwrap
// aren't on the host; the Legion has them, macOS CI doesn't.

#include "bundle/appimg.h"

#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail_count = 0;

#define EXPECT(cond, ...) do {                                      \
    if (!(cond)) {                                                  \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
        fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");        \
        fail_count++;                                               \
    }                                                               \
} while (0)

__attribute__((format(printf, 3, 4)))
static void psn(char *dst, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) { fprintf(stderr, "psn overflow\n"); abort(); }
}

static int rm_rf_cb(const char *path, const struct stat *st, int typeflag,
                    struct FTW *ftwbuf) {
    (void)st; (void)typeflag; (void)ftwbuf;
    (void)remove(path);
    return 0;
}
static void rm_rf(const char *path) {
    nftw(path, rm_rf_cb, 16, FTW_DEPTH | FTW_PHYS);
}

static int write_file(const char *path, const char *content, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    size_t n = strlen(content);
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, content + off, n - off);
        if (w < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        off += (size_t)w;
    }
    close(fd);
    return 0;
}

static int mkdir_p(const char *path, mode_t mode) {
    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) { errno = ENAMETOOLONG; return -1; }
    memcpy(buf, path, len + 1);
    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, mode) != 0 && errno != EEXIST) return -1;
            buf[i] = '/';
        }
    }
    if (mkdir(buf, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int which(const char *bin) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", bin);
    return system(cmd) == 0;
}

static off_t file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return st.st_size;
}

// Spawn mksquashfs on `src` (a directory), writing `dst`. Uses the
// same settings bundle-spec.md calls for: zstd compression, 128 KiB
// block size, -noappend. Returns 0 on success.
static int run_mksquashfs(const char *src, const char *dst) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // Dup /dev/null over stdout so mksquashfs' chatty progress
        // output doesn't fill meson test logs. Keep stderr so
        // actual failures still surface.
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDOUT_FILENO); close(dn); }
        char *args[] = {
            "mksquashfs", (char *)src, (char *)dst,
            "-comp", "zstd", "-b", "131072", "-noappend",
            NULL,
        };
        execvp("mksquashfs", args);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return 0;
}

// Append `len` bytes from `buf` to an open fd.
static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        off += (size_t)w;
    }
    return 0;
}

// Assemble stub + squashfs + trailer into `dst`. stub_len bytes of
// 0xAB precede a verbatim copy of `sqfs_path`, followed by a valid
// trailer for the given bundle-id. Returns 0 on success.
static int assemble_appimg(const char *sqfs_path,
                           const char *dst,
                           const char *bundle_id,
                           size_t stub_len) {
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0) return -1;

    // 1. stub
    uint8_t *stub = malloc(stub_len);
    if (!stub) { close(out); return -1; }
    memset(stub, 0xAB, stub_len);
    if (write_all(out, stub, stub_len) != 0) {
        free(stub); close(out); return -1;
    }
    free(stub);

    // 2. squashfs image
    int in = open(sqfs_path, O_RDONLY);
    if (in < 0) { close(out); return -1; }
    off_t sqfs_size = file_size(sqfs_path);
    if (sqfs_size < 0) { close(in); close(out); return -1; }
    uint8_t buf[64 * 1024];
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR) continue; close(in); close(out); return -1; }
        if (write_all(out, buf, (size_t)n) != 0) { close(in); close(out); return -1; }
    }
    close(in);

    // 3. trailer
    uint32_t bid_len = (uint32_t)strlen(bundle_id);
    uint32_t trailer_size = 44 + bid_len;

    if (write_all(out, MB_APPIMG_MAGIC, MB_APPIMG_MAGIC_LEN) != 0) goto fail;
    uint32_t ver_le = htole32(MB_APPIMG_TRAILER_VERSION);
    if (write_all(out, &ver_le, 4) != 0) goto fail;
    uint64_t off_le = htole64((uint64_t)stub_len);
    if (write_all(out, &off_le, 8) != 0) goto fail;
    uint64_t sz_le = htole64((uint64_t)sqfs_size);
    if (write_all(out, &sz_le, 8) != 0) goto fail;
    uint32_t bid_len_le = htole32(bid_len);
    if (write_all(out, &bid_len_le, 4) != 0) goto fail;
    if (write_all(out, bundle_id, bid_len) != 0) goto fail;
    uint32_t ts_le = htole32(trailer_size);
    if (write_all(out, &ts_le, 4) != 0) goto fail;
    if (write_all(out, MB_APPIMG_MAGIC, MB_APPIMG_MAGIC_LEN) != 0) goto fail;

    close(out);
    return 0;
fail:
    close(out);
    return -1;
}

// -----------------------------------------------------------------
// fixture content
// -----------------------------------------------------------------

static const char *FIXTURE_INFO =
"[bundle]\n"
"id = \"show.blizzard.appimgsmoke\"\n"
"name = \"AppImgSmoke\"\n"
"version = \"1.0.0\"\n"
"minimum-moonbase = \"1.0\"\n"
"\n"
"[executable]\n"
"path = \"Contents/CopyCatOS/run\"\n"
"language = \"c\"\n"
"\n"
"[permissions]\n";

static const char *FIXTURE_SCRIPT =
"#!/bin/sh\n"
"echo \"HOME=$HOME\"\n"
"echo \"MOONBASE_BUNDLE_ID=$MOONBASE_BUNDLE_ID\"\n"
"echo \"$HOME\" > \"$HOME/marker\"\n"
"echo \"$MOONBASE_BUNDLE_ID\" > \"$HOME/bundle-id\"\n"
"exit 42\n";

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr,
            "usage: mb-launch-appimg <moonbase-launch> <sandbox-dir>\n");
        return 2;
    }
    const char *launch_bin  = argv[1];
    const char *sandbox_dir = argv[2];

    if (!which("bwrap")) {
        fprintf(stderr, "SKIP: bwrap not on PATH\n");
        return 77;
    }
    if (!which("mksquashfs")) {
        fprintf(stderr, "SKIP: mksquashfs not on PATH\n");
        return 77;
    }
    if (!which("squashfuse")) {
        fprintf(stderr, "SKIP: squashfuse not on PATH\n");
        return 77;
    }
    if (!which("fusermount")) {
        fprintf(stderr, "SKIP: fusermount not on PATH\n");
        return 77;
    }

    char tmp_template[] = "/tmp/mb-launch-appimg-XXXXXX";
    char *tmp = mkdtemp(tmp_template);
    if (!tmp) { perror("mkdtemp"); return 1; }

    char fake_home[PATH_MAX];
    psn(fake_home, sizeof(fake_home), "%s/root", tmp);
    if (mkdir(fake_home, 0700) != 0) { perror("mkdir home"); rm_rf(tmp); return 1; }

    // Build the .appdev-shape source tree. The same Contents/ is what
    // we feed to mksquashfs.
    char src_dir[PATH_MAX];
    psn(src_dir, sizeof(src_dir), "%s/src", tmp);
    mkdir(src_dir, 0755);
    char contents[PATH_MAX];
    psn(contents, sizeof(contents), "%s/Contents", src_dir);              mkdir(contents, 0755);
    char sub[PATH_MAX];
    psn(sub, sizeof(sub), "%s/Contents/CopyCatOS", src_dir);              mkdir(sub, 0755);
    psn(sub, sizeof(sub), "%s/Contents/Resources", src_dir);              mkdir(sub, 0755);

    char info_path[PATH_MAX];
    psn(info_path, sizeof(info_path), "%s/Contents/Info.appc", src_dir);
    EXPECT(write_file(info_path, FIXTURE_INFO, 0644) == 0, "write Info.appc");

    char exe_path[PATH_MAX];
    psn(exe_path, sizeof(exe_path), "%s/Contents/CopyCatOS/run", src_dir);
    EXPECT(write_file(exe_path, FIXTURE_SCRIPT, 0755) == 0, "write exe");

    // mksquashfs takes a directory and creates a .sqfs blob. The
    // bundle's *parent* is packed so the squashfs root IS the bundle
    // root — after mount, <mount>/Contents/Info.appc resolves, which
    // is what mb_bundle_load expects. Packing Contents/ directly
    // would put Info.appc at the mount root and break the validator.
    char sqfs_path[PATH_MAX];
    psn(sqfs_path, sizeof(sqfs_path), "%s/Bundle.sqfs", tmp);
    EXPECT(run_mksquashfs(src_dir, sqfs_path) == 0, "mksquashfs");

    char app_path[PATH_MAX];
    psn(app_path, sizeof(app_path), "%s/AppImgSmoke.app", tmp);
    EXPECT(assemble_appimg(sqfs_path, app_path,
                           "show.blizzard.appimgsmoke", 64) == 0,
           "assemble_appimg");

    // Sanity-check the trailer we just wrote with our own reader.
    mb_appimg_trailer_t t;
    char aerr[128];
    mb_appimg_err_t ar = mb_appimg_read_trailer_path(app_path, &t, aerr, sizeof(aerr));
    EXPECT(ar == MB_APPIMG_OK, "self-read trailer: %s", aerr);
    if (ar == MB_APPIMG_OK) {
        EXPECT(strcmp(t.bundle_id, "show.blizzard.appimgsmoke") == 0,
               "trailer bundle_id round-trip");
        mb_appimg_trailer_free(&t);
    }

    // XDG_RUNTIME_DIR lives under our tmpdir so we can inspect it
    // after launch to prove cleanup happened.
    char xdg[PATH_MAX];
    psn(xdg, sizeof(xdg), "%s/xdg", tmp);
    mkdir_p(xdg, 0700);

    // Fork moonbase-launch.
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); rm_rf(tmp); return 1; }
    if (pid == 0) {
        setenv("HOME", fake_home, 1);
        setenv("MOONBASE_SANDBOX_DIR", sandbox_dir, 1);
        setenv("XDG_RUNTIME_DIR", xdg, 1);
        char *child_argv[] = { (char *)launch_bin, app_path, NULL };
        execv(launch_bin, child_argv);
        perror("execv moonbase-launch");
        _exit(127);
    }

    int status = 0;
    pid_t w = waitpid(pid, &status, 0);
    EXPECT(w == pid, "waitpid rc=%d", (int)w);
    EXPECT(WIFEXITED(status), "launch exited cleanly (status=0x%x)", status);
    if (WIFEXITED(status)) {
        EXPECT(WEXITSTATUS(status) == 42,
               "script exit propagated: got %d, want 42",
               WEXITSTATUS(status));
    }

    // Data dir should carry the marker, same as launch-smoke.
    char data_dir[PATH_MAX];
    psn(data_dir, sizeof(data_dir),
        "%s/.local/share/moonbase/show.blizzard.appimgsmoke", fake_home);
    struct stat st;
    EXPECT(stat(data_dir, &st) == 0 && S_ISDIR(st.st_mode),
           "data dir exists: %s", data_dir);

    char marker[PATH_MAX];
    psn(marker, sizeof(marker), "%s/marker", data_dir);
    int fd = open(marker, O_RDONLY);
    EXPECT(fd >= 0, "marker present at %s: %s", marker, strerror(errno));
    if (fd >= 0) {
        char buf[PATH_MAX + 1] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
        EXPECT(strcmp(buf, data_dir) == 0,
               "sandbox HOME matches data dir: got %s, want %s", buf, data_dir);
        close(fd);
    }

    char bid_path[PATH_MAX];
    psn(bid_path, sizeof(bid_path), "%s/bundle-id", data_dir);
    fd = open(bid_path, O_RDONLY);
    EXPECT(fd >= 0, "bundle-id marker present: %s", strerror(errno));
    if (fd >= 0) {
        char buf[128] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
        EXPECT(strcmp(buf, "show.blizzard.appimgsmoke") == 0,
               "MOONBASE_BUNDLE_ID set: got %s", buf);
        close(fd);
    }

    // Mount cleanup: after the launcher returns, there should be
    // zero entries under XDG_RUNTIME_DIR/moonbase/mounts/.
    char mounts_dir[PATH_MAX];
    psn(mounts_dir, sizeof(mounts_dir), "%s/moonbase/mounts", xdg);
    DIR *d = opendir(mounts_dir);
    if (d) {
        int entries = 0;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            entries++;
            fprintf(stderr, "stale mount entry: %s/%s\n",
                    mounts_dir, de->d_name);
        }
        closedir(d);
        EXPECT(entries == 0,
               "mount dir %s should be empty after launcher exit", mounts_dir);
    }
    // If opendir failed because mounts_dir didn't exist, nothing got
    // left behind either — that's also a pass.

    rm_rf(tmp);

    if (fail_count) {
        fprintf(stderr, "FAILED: %d assertion(s)\n", fail_count);
        return 1;
    }
    fprintf(stderr, "OK\n");
    return 0;
}
