// CopyCatOS — by Kyle Blizzard at Blizzard.show

// mb-launch-smoke — Phase D slice 3 end-to-end launcher test.
//
// Builds a real .app bundle in a tmpdir whose executable is a small
// shell script, invokes moonbase-launch pointed at it via an overridden
// $HOME and $MOONBASE_SANDBOX_DIR, and verifies:
//
//   * moonbase-launch exits with the script's exit code (42)
//   * the per-app data dir was created with the Apple-style subdirs
//   * the script ran inside the bwrap sandbox and saw HOME pointing at
//     that data dir (proves the native.profile was sourced and the
//     --setenv HOME arg landed)
//   * the bundle-id was plumbed through via MOONBASE_BUNDLE_ID
//
// Arguments passed in from meson:
//   argv[1] — absolute path to the built moonbase-launch binary
//   argv[2] — absolute path to the source sandbox/ directory (has the
//             native.profile + webview.profile shell scripts)

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdarg.h>
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
static char *path_snprintf(char *dst, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) {
        fprintf(stderr, "path_snprintf overflow (fmt=%s, cap=%zu)\n", fmt, cap);
        abort();
    }
    return dst;
}

// Recursive rm — same best-effort pattern as mb-bundle-load.
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

// Info.appc for the fixture. No permissions — strictest sandbox, no
// network, no device access. The script only has to write a marker
// inside its own HOME, which is the data dir bound rw by native.profile.
static const char *FIXTURE_INFO =
"[bundle]\n"
"id = \"show.blizzard.launchsmoke\"\n"
"name = \"LaunchSmoke\"\n"
"version = \"1.0.0\"\n"
"minimum-moonbase = \"1.0\"\n"
"\n"
"[executable]\n"
"path = \"Contents/MacOS/run\"\n"
"language = \"c\"\n"
"\n"
"[permissions]\n";

// Shell script executable. Prints HOME + MOONBASE_BUNDLE_ID to stdout
// (the test doesn't capture stdout — those are for a human looking at
// meson test -v), writes a marker file inside HOME, and exits 42 so the
// test can prove the exit-code plumbs through bwrap and the launcher.
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
            "usage: mb-launch-smoke <moonbase-launch> <sandbox-dir>\n");
        return 2;
    }
    const char *launch_bin  = argv[1];
    const char *sandbox_dir = argv[2];

    if (access("/usr/bin/bwrap", X_OK) != 0 && access("/bin/bwrap", X_OK) != 0) {
        fprintf(stderr, "SKIP: bwrap not on PATH\n");
        return 77;  // meson's "skipped" exit code
    }

    // Tmpdir layout:
    //   <tmp>/root/           = fake $HOME
    //   <tmp>/LaunchSmoke.app  = bundle
    char tmp_template[] = "/tmp/mb-launch-smoke-XXXXXX";
    char *tmp = mkdtemp(tmp_template);
    if (!tmp) { perror("mkdtemp"); return 1; }

    char fake_home[PATH_MAX];
    path_snprintf(fake_home, sizeof(fake_home), "%s/root", tmp);
    if (mkdir(fake_home, 0755) != 0) { perror("mkdir home"); rm_rf(tmp); return 1; }

    char bundle[PATH_MAX];
    path_snprintf(bundle, sizeof(bundle), "%s/LaunchSmoke.app", tmp);
    EXPECT(mkdir(bundle, 0755) == 0, "mkdir bundle");

    char sub[PATH_MAX];
    path_snprintf(sub, sizeof(sub), "%s/Contents", bundle);         mkdir(sub, 0755);
    path_snprintf(sub, sizeof(sub), "%s/Contents/MacOS", bundle);   mkdir(sub, 0755);
    path_snprintf(sub, sizeof(sub), "%s/Contents/Resources", bundle); mkdir(sub, 0755);

    char info_path[PATH_MAX];
    path_snprintf(info_path, sizeof(info_path), "%s/Contents/Info.appc", bundle);
    EXPECT(write_file(info_path, FIXTURE_INFO, 0644) == 0, "write Info.appc");

    char exe_path[PATH_MAX];
    path_snprintf(exe_path, sizeof(exe_path), "%s/Contents/MacOS/run", bundle);
    EXPECT(write_file(exe_path, FIXTURE_SCRIPT, 0755) == 0, "write exe");

    // Tighten HOME/bundle modes to show bwrap binds work with anything
    // walkable by the invoking user.
    chmod(fake_home, 0700);

    // Fork, run moonbase-launch. Child wires up the overridden env
    // before exec so we don't stomp the test's own environment (which
    // meson populates and which bwrap itself needs).
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); rm_rf(tmp); return 1; }
    if (pid == 0) {
        setenv("HOME", fake_home, 1);
        setenv("MOONBASE_SANDBOX_DIR", sandbox_dir, 1);
        // Ensure XDG_RUNTIME_DIR points somewhere the bwrap bind can
        // resolve even if meson didn't inherit one. A plain tmp dir is
        // fine for this smoke — the launcher only needs the path to
        // exist for --bind to succeed.
        char xdg[PATH_MAX];
        path_snprintf(xdg, sizeof(xdg), "%s/xdg", tmp);
        mkdir_p(xdg, 0700);
        setenv("XDG_RUNTIME_DIR", xdg, 1);

        char *child_argv[] = { (char *)launch_bin, (char *)bundle, NULL };
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

    // Data dir should be populated with Apple subdirs.
    char data_dir[PATH_MAX];
    path_snprintf(data_dir, sizeof(data_dir),
                  "%s/.local/share/moonbase/show.blizzard.launchsmoke",
                  fake_home);
    struct stat st;
    EXPECT(stat(data_dir, &st) == 0 && S_ISDIR(st.st_mode),
           "data dir exists: %s", data_dir);

    const char *apple_subdirs[] = {
        "Application Support", "Preferences", "Caches", "webview", NULL,
    };
    for (size_t i = 0; apple_subdirs[i]; i++) {
        char p[PATH_MAX];
        path_snprintf(p, sizeof(p), "%s/%s", data_dir, apple_subdirs[i]);
        EXPECT(stat(p, &st) == 0 && S_ISDIR(st.st_mode),
               "subdir exists: %s", p);
    }

    // Marker file proves the script ran with HOME=data_dir — because
    // that's where we find it. Body is HOME as seen inside the sandbox.
    char marker[PATH_MAX];
    path_snprintf(marker, sizeof(marker), "%s/marker", data_dir);
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

    // Bundle id should have been piped through MOONBASE_BUNDLE_ID.
    char bid_path[PATH_MAX];
    path_snprintf(bid_path, sizeof(bid_path), "%s/bundle-id", data_dir);
    fd = open(bid_path, O_RDONLY);
    EXPECT(fd >= 0, "bundle-id marker present: %s", strerror(errno));
    if (fd >= 0) {
        char buf[128] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
        EXPECT(strcmp(buf, "show.blizzard.launchsmoke") == 0,
               "MOONBASE_BUNDLE_ID set: got %s", buf);
        close(fd);
    }

    rm_rf(tmp);

    if (fail_count) {
        fprintf(stderr, "FAILED: %d assertion(s)\n", fail_count);
        return 1;
    }
    fprintf(stderr, "OK\n");
    return 0;
}
