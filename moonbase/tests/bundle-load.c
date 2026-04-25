// CopyCatOS — by Kyle Blizzard at Blizzard.show

// mb-bundle-load — Phase D slice 2 test.
//
// Builds real .app directory trees under a tmpdir, writes a full
// Info.appc, places a chmod-0755 stub executable, then exercises
// mb_bundle_load against each failure mode and the happy path.
// Every tmp tree is torn down after the test regardless of outcome.

#include "bundle/bundle.h"

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
#include <unistd.h>

static int fail_count = 0;

#define EXPECT(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); \
        fail_count++; \
    } \
} while (0)

// Bounded path join — returns dst on success, aborts on truncation so a
// test can never silently take the wrong path. Using a helper (instead
// of letting every call site grow a ternary) also makes GCC's
// -Wformat-truncation stop chasing the snprintfs.
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

// ---- tmp tree helpers ----------------------------------------------

static int rm_rf_cb(const char *path, const struct stat *st, int typeflag,
                    struct FTW *ftwbuf) {
    (void)st; (void)typeflag; (void)ftwbuf;
    // Use remove() to handle both files and directories. Best-effort
    // cleanup — a tmp leak isn't a test failure.
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

// Create tmp/.../MyApp.app/Contents/{Info.appc,CopyCatOS/exe} with caller-
// supplied Info.appc body and a trivial executable. dst_root must be
// caller-provided scratch; returns 0 and fills *out_bundle with the
// bundle path on success.
static int make_bundle(const char *scratch, const char *bundle_name,
                       const char *info_toml, const char *rel_exec_path,
                       mode_t exec_mode,
                       char *out_bundle, size_t out_cap) {
    char root[PATH_MAX];
    path_snprintf(root, sizeof(root), "%s/%s", scratch, bundle_name);
    if (mkdir(scratch, 0700) != 0 && errno != EEXIST) return -1;
    rm_rf(root);
    if (mkdir(root, 0755) != 0) return -1;
    char sub[PATH_MAX];
    path_snprintf(sub, sizeof(sub), "%s/Contents", root);
    if (mkdir(sub, 0755) != 0) return -1;
    path_snprintf(sub, sizeof(sub), "%s/Contents/CopyCatOS", root);
    if (mkdir(sub, 0755) != 0) return -1;
    path_snprintf(sub, sizeof(sub), "%s/Contents/Resources", root);
    if (mkdir(sub, 0755) != 0) return -1;
    path_snprintf(sub, sizeof(sub), "%s/Contents/Info.appc", root);
    if (write_file(sub, info_toml, 0644) != 0) return -1;
    if (rel_exec_path) {
        char exe[PATH_MAX];
        path_snprintf(exe, sizeof(exe), "%s/%s", root, rel_exec_path);
        if (write_file(exe, "#!/bin/sh\nexit 0\n", exec_mode) != 0) return -1;
    }
    snprintf(out_bundle, out_cap, "%s", root);
    return 0;
}

// ---- fixtures ------------------------------------------------------

static const char *HAPPY_INFO =
"[bundle]\n"
"id = \"show.blizzard.hello\"\n"
"name = \"Hello\"\n"
"version = \"1.0.0\"\n"
"minimum-moonbase = \"1.0\"\n"
"\n"
"[executable]\n"
"path = \"Contents/CopyCatOS/hello\"\n"
"language = \"c\"\n"
"\n"
"[permissions]\n";

// ---- tests ---------------------------------------------------------

static void test_happy(const char *scratch) {
    char path[PATH_MAX];
    EXPECT(make_bundle(scratch, "Happy.app", HAPPY_INFO,
                       "Contents/CopyCatOS/hello", 0755, path, sizeof(path)) == 0,
           "make happy bundle");

    mb_bundle_t b;
    char err[256] = {0};
    mb_bundle_err_t rc = mb_bundle_load(path, &b, err, sizeof(err));
    EXPECT(rc == MB_BUNDLE_OK, "happy rc=%s (%s)", mb_bundle_err_string(rc), err);
    EXPECT(b.bundle_path && strstr(b.bundle_path, "/Happy.app") != NULL, "bundle_path");
    EXPECT(b.exe_abs_path && strstr(b.exe_abs_path, "/Contents/CopyCatOS/hello") != NULL,
           "exe_abs_path: %s", b.exe_abs_path ? b.exe_abs_path : "(null)");
    EXPECT(strcmp(b.info.id, "show.blizzard.hello") == 0, "info.id");
    mb_bundle_free(&b);
    rm_rf(path);
}

static void test_bad_suffix(const char *scratch) {
    char path[PATH_MAX];
    path_snprintf(path, sizeof(path), "%s/NoSuffix", scratch);
    rm_rf(path);
    mkdir(path, 0755);
    mb_bundle_t b;
    char err[256] = {0};
    mb_bundle_err_t rc = mb_bundle_load(path, &b, err, sizeof(err));
    EXPECT(rc == MB_BUNDLE_ERR_BAD_SUFFIX, "bad-suffix rc=%s", mb_bundle_err_string(rc));
    mb_bundle_free(&b);
    rm_rf(path);

    // Retired legacy suffix: .appc and .appcd must both be rejected
    // now that the loader only accepts .app and .appdev. The name is
    // Info.appc for metadata — the bundle directory suffix is not.
    path_snprintf(path, sizeof(path), "%s/Legacy.appc", scratch);
    rm_rf(path);
    mkdir(path, 0755);
    rc = mb_bundle_load(path, &b, err, sizeof(err));
    EXPECT(rc == MB_BUNDLE_ERR_BAD_SUFFIX,
           "legacy .appc rejected rc=%s", mb_bundle_err_string(rc));
    mb_bundle_free(&b);
    rm_rf(path);

    path_snprintf(path, sizeof(path), "%s/Legacy.appcd", scratch);
    rm_rf(path);
    mkdir(path, 0755);
    rc = mb_bundle_load(path, &b, err, sizeof(err));
    EXPECT(rc == MB_BUNDLE_ERR_BAD_SUFFIX,
           "legacy .appcd rejected rc=%s", mb_bundle_err_string(rc));
    mb_bundle_free(&b);
    rm_rf(path);
}

static void test_not_dir(const char *scratch) {
    char path[PATH_MAX];
    path_snprintf(path, sizeof(path), "%s/notdir.app", scratch);
    rm_rf(path);
    EXPECT(write_file(path, "hi", 0644) == 0, "write dummy file");
    mb_bundle_t b;
    char err[256] = {0};
    mb_bundle_err_t rc = mb_bundle_load(path, &b, err, sizeof(err));
    EXPECT(rc == MB_BUNDLE_ERR_NOT_DIR, "not-dir rc=%s", mb_bundle_err_string(rc));
    mb_bundle_free(&b);
    unlink(path);
}

static void test_no_info(const char *scratch) {
    char path[PATH_MAX];
    path_snprintf(path, sizeof(path), "%s/NoInfo.app", scratch);
    rm_rf(path);
    mkdir(path, 0755);
    char sub[PATH_MAX];
    path_snprintf(sub, sizeof(sub), "%s/Contents", path);
    mkdir(sub, 0755);
    mb_bundle_t b;
    char err[256] = {0};
    mb_bundle_err_t rc = mb_bundle_load(path, &b, err, sizeof(err));
    EXPECT(rc == MB_BUNDLE_ERR_NO_INFO, "no-info rc=%s", mb_bundle_err_string(rc));
    mb_bundle_free(&b);
    rm_rf(path);
}

static void test_exec_missing(const char *scratch) {
    char path[PATH_MAX];
    EXPECT(make_bundle(scratch, "NoExe.app", HAPPY_INFO,
                       NULL, 0, path, sizeof(path)) == 0, "make NoExe bundle");
    mb_bundle_t b;
    char err[256] = {0};
    mb_bundle_err_t rc = mb_bundle_load(path, &b, err, sizeof(err));
    EXPECT(rc == MB_BUNDLE_ERR_EXEC_MISSING, "exec-missing rc=%s (%s)",
           mb_bundle_err_string(rc), err);
    mb_bundle_free(&b);
    rm_rf(path);
}

static void test_exec_not_executable(const char *scratch) {
    char path[PATH_MAX];
    EXPECT(make_bundle(scratch, "NotExec.app", HAPPY_INFO,
                       "Contents/CopyCatOS/hello", 0644, path, sizeof(path)) == 0, "make NotExec");
    mb_bundle_t b;
    char err[256] = {0};
    mb_bundle_err_t rc = mb_bundle_load(path, &b, err, sizeof(err));
    EXPECT(rc == MB_BUNDLE_ERR_EXEC_MODE, "exec-mode rc=%s (%s)",
           mb_bundle_err_string(rc), err);
    mb_bundle_free(&b);
    rm_rf(path);
}

static void test_exec_location(const char *scratch) {
    // Info.appc points at Contents/Info-wrong/hello which is not in
    // CopyCatOS/ or Resources/.
    const char *bad_info =
        "[bundle]\n"
        "id = \"show.blizzard.hello\"\n"
        "name = \"Hello\"\n"
        "version = \"1.0.0\"\n"
        "minimum-moonbase = \"1.0\"\n"
        "\n"
        "[executable]\n"
        "path = \"Contents/BadDir/hello\"\n"
        "language = \"c\"\n"
        "\n"
        "[permissions]\n";
    char path[PATH_MAX];
    path_snprintf(path, sizeof(path), "%s/BadLoc.app", scratch);
    rm_rf(path);
    mkdir(path, 0755);
    char sub[PATH_MAX];
    path_snprintf(sub, sizeof(sub), "%s/Contents", path);         mkdir(sub, 0755);
    path_snprintf(sub, sizeof(sub), "%s/Contents/BadDir", path);  mkdir(sub, 0755);
    path_snprintf(sub, sizeof(sub), "%s/Contents/Info.appc", path);
    EXPECT(write_file(sub, bad_info, 0644) == 0, "write bad_info");
    path_snprintf(sub, sizeof(sub), "%s/Contents/BadDir/hello", path);
    EXPECT(write_file(sub, "#!/bin/sh\nexit 0\n", 0755) == 0, "write exe");

    mb_bundle_t b;
    char err[256] = {0};
    mb_bundle_err_t rc = mb_bundle_load(path, &b, err, sizeof(err));
    EXPECT(rc == MB_BUNDLE_ERR_EXEC_LOCATION, "exec-location rc=%s (%s)",
           mb_bundle_err_string(rc), err);
    mb_bundle_free(&b);
    rm_rf(path);
}

static void test_exec_escape_via_symlink(const char *scratch) {
    // Bundle has a relative symlink Contents/CopyCatOS/hello -> ../../../../bin/sh,
    // which resolves outside the bundle. After realpath, the executable
    // path shouldn't lie inside abs bundle.
    char path[PATH_MAX];
    EXPECT(make_bundle(scratch, "Escape.app", HAPPY_INFO,
                       NULL, 0, path, sizeof(path)) == 0, "make Escape bundle");
    char link[PATH_MAX];
    path_snprintf(link, sizeof(link), "%s/Contents/CopyCatOS/hello", path);
    unlink(link);
    // Make a symlink to /bin/sh using a relative path so the bundle itself
    // carries the relative link (absolute links are rejected first).
    EXPECT(symlink("../../../../../../../bin/sh", link) == 0, "make relative escape link");
    mb_bundle_t b;
    char err[256] = {0};
    mb_bundle_err_t rc = mb_bundle_load(path, &b, err, sizeof(err));
    EXPECT(rc == MB_BUNDLE_ERR_EXEC_ESCAPE, "exec-escape rc=%s (%s)",
           mb_bundle_err_string(rc), err);
    mb_bundle_free(&b);
    rm_rf(path);
}

static void test_absolute_symlink(const char *scratch) {
    char path[PATH_MAX];
    EXPECT(make_bundle(scratch, "AbsLink.app", HAPPY_INFO,
                       "Contents/CopyCatOS/hello", 0755, path, sizeof(path)) == 0,
           "make AbsLink bundle");
    char link[PATH_MAX];
    path_snprintf(link, sizeof(link), "%s/Contents/Resources/abs-link", path);
    EXPECT(symlink("/etc/passwd", link) == 0, "make abs link");
    mb_bundle_t b;
    char err[256] = {0};
    mb_bundle_err_t rc = mb_bundle_load(path, &b, err, sizeof(err));
    EXPECT(rc == MB_BUNDLE_ERR_ABSOLUTE_SYMLINK, "abs-symlink rc=%s (%s)",
           mb_bundle_err_string(rc), err);
    mb_bundle_free(&b);
    rm_rf(path);
}

static void test_outside_contents(const char *scratch) {
    char path[PATH_MAX];
    EXPECT(make_bundle(scratch, "Stray.app", HAPPY_INFO,
                       "Contents/CopyCatOS/hello", 0755, path, sizeof(path)) == 0,
           "make Stray bundle");
    char stray[PATH_MAX];
    path_snprintf(stray, sizeof(stray), "%s/README", path);
    EXPECT(write_file(stray, "outside contents\n", 0644) == 0, "write stray");
    mb_bundle_t b;
    char err[256] = {0};
    mb_bundle_err_t rc = mb_bundle_load(path, &b, err, sizeof(err));
    EXPECT(rc == MB_BUNDLE_ERR_OUTSIDE_CONTENTS, "outside-contents rc=%s (%s)",
           mb_bundle_err_string(rc), err);
    mb_bundle_free(&b);
    rm_rf(path);
}

static void test_api_version_too_new(const char *scratch) {
    const char *info =
        "[bundle]\n"
        "id = \"show.blizzard.hello\"\n"
        "name = \"Hello\"\n"
        "version = \"1.0.0\"\n"
        "minimum-moonbase = \"99.99\"\n"
        "\n"
        "[executable]\n"
        "path = \"Contents/CopyCatOS/hello\"\n"
        "language = \"c\"\n"
        "\n"
        "[permissions]\n";
    char path[PATH_MAX];
    EXPECT(make_bundle(scratch, "Future.app", info,
                       "Contents/CopyCatOS/hello", 0755, path, sizeof(path)) == 0,
           "make Future bundle");
    mb_bundle_t b;
    char err[256] = {0};
    mb_bundle_err_t rc = mb_bundle_load(path, &b, err, sizeof(err));
    EXPECT(rc == MB_BUNDLE_ERR_API_VERSION, "api-version rc=%s (%s)",
           mb_bundle_err_string(rc), err);
    mb_bundle_free(&b);
    rm_rf(path);
}

// ---- main ---------------------------------------------------------

int main(void) {
    char scratch[128];
    path_snprintf(scratch, sizeof(scratch), "/tmp/mb-bundle-load.%d", (int)getpid());
    rm_rf(scratch);
    mkdir(scratch, 0700);

    test_happy(scratch);
    test_bad_suffix(scratch);
    test_not_dir(scratch);
    test_no_info(scratch);
    test_exec_missing(scratch);
    test_exec_not_executable(scratch);
    test_exec_location(scratch);
    test_exec_escape_via_symlink(scratch);
    test_absolute_symlink(scratch);
    test_outside_contents(scratch);
    test_api_version_too_new(scratch);

    rm_rf(scratch);

    if (fail_count) {
        fprintf(stderr, "FAIL: %d assertion(s) failed\n", fail_count);
        return 1;
    }
    printf("ok: bundle-load\n");
    return 0;
}
