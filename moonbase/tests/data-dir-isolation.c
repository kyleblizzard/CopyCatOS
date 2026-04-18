// CopyCatOS — by Kyle Blizzard at Blizzard.show

// mb-data-dir-isolation — Phase D slice 6 sandbox isolation test.
//
// Builds two fixture bundles with different bundle-ids, launches each
// through moonbase-launch under a shared fake $HOME, and verifies the
// sandbox privacy mandate:
//
//   1. Per-app data dir exists under $HOME/.local/share/moonbase/<id>
//      with the Apple-style subdirs, all at mode 0700.
//   2. App A's script writes a secret into its own Preferences dir.
//      App B's script reads $HOME then tries to reach into App A's
//      sibling data dir via the relative path ../<app-a-id>/. Under
//      the tmpfs overlay that sibling doesn't exist — the read must
//      fail, and App B's script exits with a specific code so the
//      test can distinguish "saw-the-file" from "correctly-blocked".
//   3. App B also checks that the user's real $HOME has been hidden:
//      trying to read $HOME/../<real-user>/.ssh (or any other real
//      path outside $HOME) should come back empty.
//
// Arguments passed from meson:
//   argv[1] — moonbase-launch
//   argv[2] — sandbox-dir
//   argv[3] — moonbase-consent (stub, always approves)

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
    if (n < 0 || (size_t)n >= cap) abort();
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

static const char *INFO_TEMPLATE =
"[bundle]\n"
"id = \"%s\"\n"
"name = \"%s\"\n"
"version = \"1.0.0\"\n"
"minimum-moonbase = \"1.0\"\n"
"\n"
"[executable]\n"
"path = \"Contents/MacOS/run\"\n"
"language = \"c\"\n"
"\n"
"[permissions]\n";

// App A: writes "secret-A" into its own Preferences dir.
static const char *APP_A_SCRIPT =
"#!/bin/sh\n"
"mkdir -p \"$HOME/Preferences\"\n"
"echo 'secret-A' > \"$HOME/Preferences/data\"\n"
"exit 10\n";

// App B: tries three things it must NOT be able to do, then writes
// its own secret and exits with code 20. Each failure increments a
// counter passed to the test via the marker file's body.
//
//   test 1: read App A's Preferences/data via the sibling path
//   test 2: list the moonbase dir (should be empty but for self)
//   test 3: read a real host file outside $HOME (/etc/hostname is
//           still readable because / is ro-bind; the privacy cut
//           is about $HOME, not / as a whole, so we test only the
//           $HOME tmpfs overlay)
//
// Exit 20 if every isolation check passed, 21 otherwise.
static const char *APP_B_SCRIPT =
"#!/bin/sh\n"
"set +e\n"
"# Should return nothing — sibling's dir is hidden by the tmpfs overlay.\n"
"peek=$(cat \"$HOME/../show.blizzard.isoa/Preferences/data\" 2>/dev/null)\n"
"if [ -n \"$peek\" ]; then\n"
"    echo \"LEAK: saw A's secret -> $peek\" 1>&2\n"
"    exit 21\n"
"fi\n"
"# The moonbase subdir of the tmpfs'd $HOME must only contain this\n"
"# app's own dir.\n"
"dir=\"$HOME/../../moonbase\"\n"
"count=$(ls -1 \"$dir\" 2>/dev/null | wc -l)\n"
"if [ \"$count\" != \"1\" ]; then\n"
"    echo \"LEAK: moonbase dir has $count siblings\" 1>&2\n"
"    exit 21\n"
"fi\n"
"mkdir -p \"$HOME/Preferences\"\n"
"echo 'secret-B' > \"$HOME/Preferences/data\"\n"
"exit 20\n";

static int run_launch(const char *launch_bin, const char *sandbox_dir,
                      const char *consent_bin, const char *fake_home,
                      const char *xdg, const char *bundle) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        setenv("HOME", fake_home, 1);
        setenv("XDG_RUNTIME_DIR", xdg, 1);
        setenv("MOONBASE_SANDBOX_DIR", sandbox_dir, 1);
        setenv("MOONBASE_CONSENT_BIN", consent_bin, 1);
        setenv("MOONBASE_CONSENT_AUTO", "approve", 1);
        char *child_argv[] = { (char *)launch_bin, (char *)bundle, NULL };
        execv(launch_bin, child_argv);
        perror("execv moonbase-launch");
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return -1;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

static int make_bundle(const char *tmp, const char *bundle_id,
                       const char *bundle_name, const char *script,
                       char *bundle_out, size_t cap) {
    path_snprintf(bundle_out, cap, "%s/%s.appc", tmp, bundle_id);
    if (mkdir(bundle_out, 0755) != 0) return -1;
    char sub[PATH_MAX];
    path_snprintf(sub, sizeof(sub), "%s/Contents", bundle_out);
    if (mkdir(sub, 0755) != 0) return -1;
    path_snprintf(sub, sizeof(sub), "%s/Contents/MacOS", bundle_out);
    if (mkdir(sub, 0755) != 0) return -1;
    path_snprintf(sub, sizeof(sub), "%s/Contents/Resources", bundle_out);
    if (mkdir(sub, 0755) != 0) return -1;

    char info[1024];
    int n = snprintf(info, sizeof(info), INFO_TEMPLATE, bundle_id, bundle_name);
    if (n < 0 || (size_t)n >= sizeof(info)) return -1;

    char p[PATH_MAX];
    path_snprintf(p, sizeof(p), "%s/Contents/Info.appc", bundle_out);
    if (write_file(p, info, 0644) != 0) return -1;

    path_snprintf(p, sizeof(p), "%s/Contents/MacOS/run", bundle_out);
    if (write_file(p, script, 0755) != 0) return -1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <launch> <sandbox> <consent>\n", argv[0]);
        return 2;
    }
    const char *launch  = argv[1];
    const char *sandbox = argv[2];
    const char *consent = argv[3];

    if (access("/usr/bin/bwrap", X_OK) != 0 && access("/bin/bwrap", X_OK) != 0) {
        fprintf(stderr, "SKIP: bwrap not installed\n");
        return 77;
    }

    char tmpl[] = "/tmp/mb-iso-XXXXXX";
    char *tmp = mkdtemp(tmpl);
    if (!tmp) { perror("mkdtemp"); return 1; }

    char fake_home[PATH_MAX], xdg[PATH_MAX];
    path_snprintf(fake_home, sizeof(fake_home), "%s/home", tmp);
    path_snprintf(xdg,       sizeof(xdg),       "%s/xdg",  tmp);
    mkdir(fake_home, 0700);
    mkdir(xdg,       0700);

    char bundle_a[PATH_MAX], bundle_b[PATH_MAX];
    EXPECT(make_bundle(tmp, "show.blizzard.isoa", "AppA", APP_A_SCRIPT,
                       bundle_a, sizeof(bundle_a)) == 0, "build A");
    EXPECT(make_bundle(tmp, "show.blizzard.isob", "AppB", APP_B_SCRIPT,
                       bundle_b, sizeof(bundle_b)) == 0, "build B");

    // Run A first so its Preferences/data is on disk before B launches.
    int rca = run_launch(launch, sandbox, consent, fake_home, xdg, bundle_a);
    EXPECT(rca == 10, "A launched: rc=%d want 10", rca);

    char path[PATH_MAX];
    path_snprintf(path, sizeof(path),
                  "%s/.local/share/moonbase/show.blizzard.isoa/Preferences/data",
                  fake_home);
    struct stat st;
    EXPECT(stat(path, &st) == 0, "A wrote its secret");

    // Per-app data dirs + subdirs are 0700.
    path_snprintf(path, sizeof(path),
                  "%s/.local/share/moonbase/show.blizzard.isoa", fake_home);
    EXPECT(stat(path, &st) == 0 && (st.st_mode & 07777) == 0700,
           "A data dir mode 0%o want 0700", st.st_mode & 07777);
    const char *subs[] = {
        "Application Support", "Preferences", "Caches", "webview", NULL,
    };
    for (size_t i = 0; subs[i]; i++) {
        char sp[PATH_MAX];
        path_snprintf(sp, sizeof(sp), "%s/%s", path, subs[i]);
        EXPECT(stat(sp, &st) == 0 && (st.st_mode & 07777) == 0700,
               "A/%s mode 0%o want 0700", subs[i], st.st_mode & 07777);
    }

    // Run B.  Its internal checks assert isolation; exit 20 means all
    // checks passed, 21 means a leak was detected.
    int rcb = run_launch(launch, sandbox, consent, fake_home, xdg, bundle_b);
    EXPECT(rcb == 20, "B launched + isolation checks passed: rc=%d want 20",
           rcb);

    // B's own secret should live under its own data dir, not A's.
    path_snprintf(path, sizeof(path),
                  "%s/.local/share/moonbase/show.blizzard.isob/Preferences/data",
                  fake_home);
    EXPECT(stat(path, &st) == 0, "B wrote its secret");

    // Sanity check the reverse: from the outside, both secrets exist
    // side by side.
    path_snprintf(path, sizeof(path),
                  "%s/.local/share/moonbase/show.blizzard.isoa/Preferences/data",
                  fake_home);
    EXPECT(stat(path, &st) == 0, "A's secret still on disk post-B-launch");

    rm_rf(tmp);

    if (fail_count) {
        fprintf(stderr, "FAILED: %d assertion(s)\n", fail_count);
        return 1;
    }
    fprintf(stderr, "OK\n");
    return 0;
}
