// CopyCatOS — by Kyle Blizzard at Blizzard.show

// mb-launch-quarantine — Phase D slice 4 launcher integration test.
//
// Pins the full quarantine gate in the launcher:
//   1. bundle has user.moonbase.quarantine=pending + consent approves
//      → launcher runs the script, xattr is rewritten to "approved".
//   2. bundle has user.moonbase.quarantine=pending + consent rejects
//      → launcher refuses (exit 3), xattr is rewritten to "rejected",
//      inner script did NOT run.
//   3. bundle has user.moonbase.quarantine=rejected → launcher refuses
//      without even exec'ing the consent helper.
//
// Arguments passed in from meson:
//   argv[1] — absolute path to the moonbase-launch binary
//   argv[2] — absolute path to the sandbox/ dir
//   argv[3] — absolute path to moonbase-consent (stub)

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
#include <sys/xattr.h>
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
    if (n < 0 || (size_t)n >= cap) { abort(); }
    return dst;
}

static int rm_rf_cb(const char *p, const struct stat *s, int t, struct FTW *f) {
    (void)s; (void)t; (void)f; (void)remove(p); return 0;
}
static void rm_rf(const char *p) { nftw(p, rm_rf_cb, 16, FTW_DEPTH | FTW_PHYS); }

static int write_file(const char *path, const char *content, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    size_t n = strlen(content), off = 0;
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
    if (len >= sizeof(buf)) return -1;
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

static const char *INFO =
"[bundle]\n"
"id = \"show.blizzard.launchq\"\n"
"name = \"LaunchQ\"\n"
"version = \"1.0.0\"\n"
"minimum-moonbase = \"1.0\"\n"
"\n"
"[executable]\n"
"path = \"Contents/CopyCatOS/run\"\n"
"language = \"c\"\n"
"\n"
"[permissions]\n";

// Shell script marks $HOME/ran so the test can assert the inner exec
// did or didn't happen. Exit code 42 makes "launcher returned 42"
// prove the script actually ran.
static const char *SCRIPT =
"#!/bin/sh\n"
"touch \"$HOME/ran\"\n"
"exit 42\n";

typedef struct {
    char *tmp;
    char *fake_home;
    char *xdg;
    char *bundle;
    char *data_dir;
    char *ran_marker;
} env_t;

static void env_build(env_t *e) {
    char tmpl[] = "/tmp/mb-launch-q-XXXXXX";
    char *t = mkdtemp(tmpl);
    if (!t) { perror("mkdtemp"); abort(); }
    e->tmp = strdup(t);

    e->fake_home = malloc(PATH_MAX);
    e->xdg       = malloc(PATH_MAX);
    e->bundle    = malloc(PATH_MAX);
    e->data_dir  = malloc(PATH_MAX);
    e->ran_marker= malloc(PATH_MAX);
    path_snprintf(e->fake_home, PATH_MAX, "%s/root",   e->tmp);
    path_snprintf(e->xdg,       PATH_MAX, "%s/xdg",    e->tmp);
    path_snprintf(e->bundle,    PATH_MAX, "%s/LaunchQ.app", e->tmp);

    mkdir(e->fake_home, 0700);
    mkdir(e->xdg, 0700);
    mkdir(e->bundle, 0755);
    char sub[PATH_MAX];
    path_snprintf(sub, sizeof(sub), "%s/Contents", e->bundle);         mkdir(sub, 0755);
    path_snprintf(sub, sizeof(sub), "%s/Contents/CopyCatOS", e->bundle);   mkdir(sub, 0755);
    path_snprintf(sub, sizeof(sub), "%s/Contents/Resources", e->bundle); mkdir(sub, 0755);

    char p[PATH_MAX];
    path_snprintf(p, sizeof(p), "%s/Contents/Info.appc", e->bundle);
    if (write_file(p, INFO, 0644) != 0) { perror("info"); abort(); }
    path_snprintf(p, sizeof(p), "%s/Contents/CopyCatOS/run", e->bundle);
    if (write_file(p, SCRIPT, 0755) != 0) { perror("script"); abort(); }

    path_snprintf(e->data_dir, PATH_MAX,
                  "%s/.local/share/moonbase/show.blizzard.launchq",
                  e->fake_home);
    path_snprintf(e->ran_marker, PATH_MAX, "%s/ran", e->data_dir);
}

static void env_free(env_t *e) {
    if (e->tmp) { rm_rf(e->tmp); free(e->tmp); }
    free(e->fake_home); free(e->xdg); free(e->bundle);
    free(e->data_dir);  free(e->ran_marker);
    memset(e, 0, sizeof(*e));
}

static int run_launch(const char *launch_bin, const char *sandbox_dir,
                      const char *consent_bin, const char *consent_auto,
                      const env_t *e) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        setenv("HOME", e->fake_home, 1);
        setenv("XDG_RUNTIME_DIR", e->xdg, 1);
        setenv("MOONBASE_SANDBOX_DIR", sandbox_dir, 1);
        setenv("MOONBASE_CONSENT_BIN", consent_bin, 1);
        if (consent_auto) setenv("MOONBASE_CONSENT_AUTO", consent_auto, 1);
        else              unsetenv("MOONBASE_CONSENT_AUTO");
        char *child_argv[] = { (char *)launch_bin, e->bundle, NULL };
        execv(launch_bin, child_argv);
        perror("execv moonbase-launch");
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return -1;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// Return the current xattr value as a freshly-malloced NUL-terminated
// string (caller frees), or NULL if unset / missing.
static char *read_xattr(const char *path) {
    char buf[64] = {0};
    ssize_t n = getxattr(path, "user.moonbase.quarantine", buf, sizeof(buf) - 1);
    if (n <= 0) return NULL;
    buf[n] = '\0';
    return strdup(buf);
}

static void test_pending_approve(const char *launch, const char *sandbox,
                                 const char *consent) {
    env_t e = {0};
    env_build(&e);
    EXPECT(setxattr(e.bundle, "user.moonbase.quarantine",
                    "pending", 7, 0) == 0,
           "mark pending: %s", strerror(errno));

    int rc = run_launch(launch, sandbox, consent, "approve", &e);
    EXPECT(rc == 42, "pending+approve: rc=%d want 42", rc);
    struct stat st;
    EXPECT(stat(e.ran_marker, &st) == 0, "script ran (marker present)");

    char *attr = read_xattr(e.bundle);
    EXPECT(attr && strcmp(attr, "approved") == 0,
           "xattr rewritten to approved (got %s)", attr ? attr : "(null)");
    free(attr);
    env_free(&e);
}

static void test_pending_reject(const char *launch, const char *sandbox,
                                const char *consent) {
    env_t e = {0};
    env_build(&e);
    EXPECT(setxattr(e.bundle, "user.moonbase.quarantine",
                    "pending", 7, 0) == 0, "mark pending");

    int rc = run_launch(launch, sandbox, consent, "reject", &e);
    EXPECT(rc == 3, "pending+reject: rc=%d want 3", rc);
    struct stat st;
    EXPECT(stat(e.ran_marker, &st) != 0, "script did NOT run");

    char *attr = read_xattr(e.bundle);
    EXPECT(attr && strcmp(attr, "rejected") == 0,
           "xattr persisted as rejected (got %s)", attr ? attr : "(null)");
    free(attr);
    env_free(&e);
}

static void test_pending_no_auto_headless(const char *launch,
                                          const char *sandbox,
                                          const char *consent) {
    // With no MOONBASE_CONSENT_AUTO and no TTY, moonbase-consent picks
    // the safe default (reject). The launcher must persist that and
    // the script must not run.
    env_t e = {0};
    env_build(&e);
    EXPECT(setxattr(e.bundle, "user.moonbase.quarantine",
                    "pending", 7, 0) == 0, "mark pending");

    int rc = run_launch(launch, sandbox, consent, NULL, &e);
    EXPECT(rc == 3, "pending+no-auto+headless: rc=%d want 3", rc);
    struct stat st;
    EXPECT(stat(e.ran_marker, &st) != 0,
           "script did NOT run (headless auto-reject)");
    char *attr = read_xattr(e.bundle);
    EXPECT(attr && strcmp(attr, "rejected") == 0,
           "xattr persisted as rejected (got %s)", attr ? attr : "(null)");
    free(attr);
    env_free(&e);
}

static void test_already_rejected(const char *launch, const char *sandbox,
                                  const char *consent) {
    env_t e = {0};
    env_build(&e);
    EXPECT(setxattr(e.bundle, "user.moonbase.quarantine",
                    "rejected", 8, 0) == 0, "mark rejected");

    // consent_auto=approve would ordinarily approve — but the launcher
    // shouldn't even reach the consent helper for a "rejected" bundle,
    // so the script must not run.
    int rc = run_launch(launch, sandbox, consent, "approve", &e);
    EXPECT(rc == 3, "already-rejected: rc=%d want 3", rc);
    struct stat st;
    EXPECT(stat(e.ran_marker, &st) != 0, "script did NOT run");
    env_free(&e);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr,
            "usage: %s <moonbase-launch> <sandbox-dir> <moonbase-consent>\n",
            argv[0]);
        return 2;
    }
    const char *launch  = argv[1];
    const char *sandbox = argv[2];
    const char *consent = argv[3];

    if (access("/usr/bin/bwrap", X_OK) != 0 && access("/bin/bwrap", X_OK) != 0) {
        fprintf(stderr, "SKIP: bwrap not present\n");
        return 77;
    }

    // Smoke the xattr support on /tmp — mkdtemp + setxattr + unlink.
    {
        char tmpl[] = "/tmp/mb-xattr-probe-XXXXXX";
        char *d = mkdtemp(tmpl);
        if (!d) { perror("mkdtemp probe"); return 1; }
        if (setxattr(d, "user.moonbase.quarantine", "pending", 7, 0) != 0) {
            int e = errno;
            rmdir(d);
            if (e == ENOTSUP
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
                || e == EOPNOTSUPP
#endif
            ) {
                fprintf(stderr, "SKIP: /tmp has no xattr support\n");
                return 77;
            }
            errno = e;
            perror("probe setxattr");
            return 1;
        }
        mkdir_p(d, 0700);
        rmdir(d);
    }

    test_pending_approve(launch, sandbox, consent);
    test_pending_reject(launch, sandbox, consent);
    test_pending_no_auto_headless(launch, sandbox, consent);
    test_already_rejected(launch, sandbox, consent);

    if (fail_count) {
        fprintf(stderr, "FAILED: %d assertion(s)\n", fail_count);
        return 1;
    }
    fprintf(stderr, "OK\n");
    return 0;
}
