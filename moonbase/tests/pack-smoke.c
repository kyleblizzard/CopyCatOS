// CopyCatOS — by Kyle Blizzard at Blizzard.show

// mb-pack-smoke — integration test for moonbase-pack (slice 17-A).
//
// The test:
//   1. Builds a minimal `.appdev` in a tmpdir (Info.appc + a shell
//      script executable under Contents/CopyCatOS/run).
//   2. Shells out to the just-built moonbase-pack binary with --stub
//      pointing at a dummy 4-byte placeholder stub — the packer only
//      needs the stub bytes to prepend; it doesn't exec them.
//   3. Verifies the output `.app` parses cleanly via
//      mb_appimg_read_trailer_path: bundle-id matches Info.appc,
//      squashfs_offset equals the stub length (4 bytes), trailer
//      version matches, and the sqfs bytes at the advertised offset
//      start with the squashfs magic "hsqs".
//   4. Confirms the output file is chmod 0755.
//
// Skips (exit 77) if mksquashfs isn't on the host — macOS dev boxes
// won't have squashfs-tools, the Legion does. End-to-end "launch a
// real packed .app" validation is covered by the follow-up Legion
// hand-test; pack-smoke focuses on the packer's on-disk output.

#include "bundle/appimg.h"

#include <dirent.h>
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

static int write_file(const char *path, const void *content, size_t n, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, (const char *)content + off, n - off);
        if (w < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        off += (size_t)w;
    }
    close(fd);
    return 0;
}

static int which(const char *bin) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", bin);
    return system(cmd) == 0;
}

static const char *FIXTURE_INFO =
"[bundle]\n"
"id = \"show.blizzard.packsmoke\"\n"
"name = \"PackSmoke\"\n"
"version = \"1.0.0\"\n"
"minimum-moonbase = \"1.0\"\n"
"\n"
"[executable]\n"
"path = \"Contents/CopyCatOS/run\"\n"
"language = \"c\"\n"
"\n"
"[permissions]\n";

static const char *FIXTURE_SCRIPT = "#!/bin/sh\nexit 0\n";

// 4-byte placeholder stub. The packer only prepends bytes; it doesn't
// execute them. A 4-byte marker is enough for an exact squashfs_offset
// check after mb_appimg_read_trailer runs.
static const uint8_t DUMMY_STUB[4] = { 0xCA, 0xFE, 0xBA, 0xBE };

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: mb-pack-smoke <moonbase-pack>\n");
        return 2;
    }
    const char *pack_bin = argv[1];

    if (!which("mksquashfs")) {
        fprintf(stderr, "SKIP: mksquashfs not on PATH\n");
        return 77;
    }

    char tmp_template[] = "/tmp/mb-pack-smoke-XXXXXX";
    char *tmp = mkdtemp(tmp_template);
    if (!tmp) { perror("mkdtemp"); return 1; }

    // Fixture layout. The .appdev suffix is mandatory — moonbase-pack
    // refuses anything else via bundle-spec.md §8 validation.
    char appdev[PATH_MAX];
    psn(appdev, sizeof(appdev), "%s/PackSmoke.appdev", tmp);
    char contents[PATH_MAX];
    psn(contents, sizeof(contents), "%s/Contents", appdev);
    char copycatos[PATH_MAX];
    psn(copycatos, sizeof(copycatos), "%s/Contents/CopyCatOS", appdev);
    char resources[PATH_MAX];
    psn(resources, sizeof(resources), "%s/Contents/Resources", appdev);

    EXPECT(mkdir(appdev,     0755) == 0, "mkdir appdev");
    EXPECT(mkdir(contents,   0755) == 0, "mkdir Contents");
    EXPECT(mkdir(copycatos,  0755) == 0, "mkdir CopyCatOS");
    EXPECT(mkdir(resources,  0755) == 0, "mkdir Resources");

    char info[PATH_MAX];
    psn(info, sizeof(info), "%s/Contents/Info.appc", appdev);
    EXPECT(write_file(info, FIXTURE_INFO, strlen(FIXTURE_INFO), 0644) == 0,
           "write Info.appc");

    char run[PATH_MAX];
    psn(run, sizeof(run), "%s/Contents/CopyCatOS/run", appdev);
    EXPECT(write_file(run, FIXTURE_SCRIPT, strlen(FIXTURE_SCRIPT), 0755) == 0,
           "write run");

    // Dummy stub file.
    char stub[PATH_MAX];
    psn(stub, sizeof(stub), "%s/dummy-stub", tmp);
    EXPECT(write_file(stub, DUMMY_STUB, sizeof(DUMMY_STUB), 0755) == 0,
           "write stub");

    // Destination path (moonbase-pack auto-derives when omitted; test
    // the explicit form so we know exactly where the output lands).
    char dst[PATH_MAX];
    psn(dst, sizeof(dst), "%s/PackSmoke.app", tmp);

    // Invoke moonbase-pack --stub <stub> <appdev> <dst>.
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); rm_rf(tmp); return 1; }
    if (pid == 0) {
        char *args[] = {
            (char *)pack_bin,
            (char *)"--stub", stub,
            appdev, dst,
            NULL,
        };
        execv(pack_bin, args);
        fprintf(stderr, "exec %s: %s\n", pack_bin, strerror(errno));
        _exit(127);
    }
    int status = 0;
    EXPECT(waitpid(pid, &status, 0) > 0, "waitpid");
    EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "pack exit: %d", status);

    // Output file exists, is chmod 0755, and has non-trivial size
    // (stub + sqfs + trailer).
    struct stat st;
    EXPECT(stat(dst, &st) == 0, "stat dst");
    EXPECT(S_ISREG(st.st_mode), "dst is regular file");
    EXPECT((st.st_mode & 0777) == 0755,
           "dst mode: 0%o (want 0755)", st.st_mode & 0777);
    EXPECT(st.st_size > (off_t)(sizeof(DUMMY_STUB) + 40),
           "dst size: %lld (too small)", (long long)st.st_size);

    // Read the trailer back and cross-check every field.
    mb_appimg_trailer_t t;
    char err[256];
    mb_appimg_err_t ae = mb_appimg_read_trailer_path(dst, &t, err, sizeof(err));
    EXPECT(ae == MB_APPIMG_OK,
           "read_trailer: %s (%s)", mb_appimg_err_string(ae), err);

    EXPECT(t.version == MB_APPIMG_TRAILER_VERSION,
           "trailer version: %u", t.version);
    EXPECT(t.squashfs_offset == (uint64_t)sizeof(DUMMY_STUB),
           "sqfs offset: %llu (want %zu)",
           (unsigned long long)t.squashfs_offset, sizeof(DUMMY_STUB));
    EXPECT(t.bundle_id != NULL &&
           strcmp(t.bundle_id, "show.blizzard.packsmoke") == 0,
           "bundle-id: %s", t.bundle_id ? t.bundle_id : "(null)");

    // Verify the bytes at squashfs_offset look like a squashfs image
    // — first 4 bytes are the magic "hsqs" (0x73717368 little-endian).
    int fd = open(dst, O_RDONLY);
    EXPECT(fd >= 0, "open dst");
    if (fd >= 0) {
        uint8_t stub_read[sizeof(DUMMY_STUB)];
        EXPECT(read(fd, stub_read, sizeof(stub_read)) ==
                   (ssize_t)sizeof(stub_read),
               "read stub");
        EXPECT(memcmp(stub_read, DUMMY_STUB, sizeof(DUMMY_STUB)) == 0,
               "stub bytes survived pack");

        uint8_t sqfs_magic[4];
        EXPECT(pread(fd, sqfs_magic, 4, (off_t)t.squashfs_offset) == 4,
               "pread sqfs magic");
        EXPECT(sqfs_magic[0] == 'h' && sqfs_magic[1] == 's' &&
               sqfs_magic[2] == 'q' && sqfs_magic[3] == 's',
               "sqfs magic: %02x%02x%02x%02x",
               sqfs_magic[0], sqfs_magic[1], sqfs_magic[2], sqfs_magic[3]);
        close(fd);
    }

    mb_appimg_trailer_free(&t);
    rm_rf(tmp);

    if (fail_count > 0) {
        fprintf(stderr, "%d failure(s)\n", fail_count);
        return 1;
    }
    return 0;
}
