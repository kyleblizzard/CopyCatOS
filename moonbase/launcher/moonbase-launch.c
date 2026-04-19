// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase-launch — bundle-aware, sandbox-aware launcher.
//
// Given a .appc path:
//   1. Load + validate the bundle with mb_bundle_load (bundle-spec.md §8).
//   2. Compute the per-app data dir (~/.local/share/moonbase/<bundle-id>/)
//      and create it (along with the Apple-style subdirs) if missing.
//      Full subdir layout belongs to the per-app-data-dir slice (D.6);
//      the mkdir-p here is the minimum the sandbox bind needs.
//   3. Pick the sandbox tier:
//        [executable].language == "web"  -> webview.profile
//        everything else                 -> native.profile
//   4. Run the tier's .profile shell script to collect the base bwrap
//      argv (one arg per line on stdout).
//   5. Decide unshare_net from [permissions].network — any outbound:*
//      entry leaves the app on the host netns; otherwise the netns
//      stays unshared.
//   6. Add entitlement-specific bindings: XDG home subdirs for each
//      [permissions].filesystem entry, plus the MoonBase IPC socket
//      (XDG_RUNTIME_DIR/moonbase.sock) so the app can talk to MoonRock.
//   7. execvp("bwrap", <assembled argv> -- <bundle exec> <user args>).
//
// Quarantine (D.4) and the rest of the per-app-data-dir layout (D.6)
// plug in on top of this.

#include "bundle/bundle.h"
#include "bundle/info_appc.h"
#include "bundle/quarantine.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// -------------------------------------------------------------------
// growable argv vector
// -------------------------------------------------------------------

typedef struct {
    char **data;
    size_t count;
    size_t cap;
} argv_t;

static void argv_init(argv_t *a) { a->data = NULL; a->count = 0; a->cap = 0; }

static int argv_push(argv_t *a, const char *s) {
    if (a->count == a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 16;
        char **nd = realloc(a->data, nc * sizeof(char *));
        if (!nd) return -1;
        a->data = nd; a->cap = nc;
    }
    // argv for execvp is terminated by a NULL element — let callers
    // push that sentinel by passing s == NULL.
    if (s) {
        a->data[a->count] = strdup(s);
        if (!a->data[a->count]) return -1;
    } else {
        a->data[a->count] = NULL;
    }
    a->count++;
    return 0;
}

static void argv_free(argv_t *a) {
    if (!a) return;
    for (size_t i = 0; i < a->count; i++) free(a->data[i]);
    free(a->data);
    a->data = NULL; a->count = 0; a->cap = 0;
}

// -------------------------------------------------------------------
// usage / errors
// -------------------------------------------------------------------

static void usage(const char *argv0) {
    fprintf(stderr,
        "moonbase-launch\n"
        "\n"
        "Usage:\n"
        "  %s <bundle.appc> [-- <app-args>...]\n"
        "\n"
        "Options:\n"
        "  MOONBASE_SANDBOX_DIR   override sandbox/ profile search dir\n"
        "                         (default: <prefix>/share/moonbase/sandbox)\n"
        "\n"
        "Reads Info.appc, picks the sandbox tier from [executable].language,\n"
        "generates the bwrap arg vector from the profile script, applies\n"
        "[permissions] relaxations, and execs the bundle's executable.\n",
        argv0);
}

// -------------------------------------------------------------------
// directory helpers
// -------------------------------------------------------------------

// Recursive mkdir — POSIX has no -p flag for the syscall. Returns 0 on
// success or if all dirs already exist.
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

// -------------------------------------------------------------------
// profile lookup
// -------------------------------------------------------------------

// Resolve the absolute path to a tier's .profile script. Search order:
//   1. $MOONBASE_SANDBOX_DIR (uninstalled / dev builds)
//   2. <prefix>/share/moonbase/sandbox  (embedded at build time)
// The function never mutates its caller's args.
static int find_profile(const char *tier, char *out, size_t cap) {
    const char *env_dir = getenv("MOONBASE_SANDBOX_DIR");
    const char *candidates[] = {
        env_dir,                                        // 1
        "/usr/local/share/moonbase/sandbox",            // 2a
        "/usr/share/moonbase/sandbox",                  // 2b
        NULL,
    };
    for (size_t i = 0; candidates[i]; i++) {
        if (!candidates[i]) continue;
        int n = snprintf(out, cap, "%s/%s.profile", candidates[i], tier);
        if (n < 0 || (size_t)n >= cap) continue;
        if (access(out, R_OK | X_OK) == 0) return 0;
    }
    return -1;
}

// Collect one bwrap arg per line of the profile script's stdout. The
// profile takes four positional args: bundle path, data path,
// unshare_net flag, and the host $HOME to overlay with a tmpfs.
static int collect_profile_args(const char *profile_path,
                                const char *bundle_path,
                                const char *data_path,
                                const char *unshare_net,
                                const char *host_home,
                                argv_t *out) {
    char cmd[PATH_MAX * 2 + 256];
    int n = snprintf(cmd, sizeof(cmd), "'%s' '%s' '%s' '%s' '%s'",
                     profile_path, bundle_path, data_path, unshare_net,
                     host_home ? host_home : "");
    if (n < 0 || (size_t)n >= sizeof(cmd)) return -1;

    FILE *pipe = popen(cmd, "r");
    if (!pipe) return -1;

    char line[PATH_MAX + 128];
    while (fgets(line, sizeof(line), pipe)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;
        if (argv_push(out, line) != 0) { pclose(pipe); return -1; }
    }
    int rc = pclose(pipe);
    if (rc != 0) return -1;
    return 0;
}

// -------------------------------------------------------------------
// entitlements
// -------------------------------------------------------------------

// Map one filesystem entitlement entry to a bwrap bind. Unknown
// entries are already rejected by the parser, so this never hits
// user-supplied strings — only schema-allowlist ones.
static int add_filesystem_bind(argv_t *a, const char *entry,
                               const char *home) {
    const char *xdg = NULL;  // subdirectory under $HOME
    bool read_write = false;
    if      (strcmp(entry, "documents:read") == 0)       { xdg = "Documents";  read_write = false; }
    else if (strcmp(entry, "documents:read-write") == 0) { xdg = "Documents";  read_write = true;  }
    else if (strcmp(entry, "downloads:read") == 0)       { xdg = "Downloads";  read_write = false; }
    else if (strcmp(entry, "downloads:read-write") == 0) { xdg = "Downloads";  read_write = true;  }
    else if (strcmp(entry, "desktop:read") == 0)         { xdg = "Desktop";    read_write = false; }
    else if (strcmp(entry, "desktop:read-write") == 0)   { xdg = "Desktop";    read_write = true;  }
    else if (strcmp(entry, "music:read") == 0)           { xdg = "Music";      read_write = false; }
    else if (strcmp(entry, "pictures:read") == 0)        { xdg = "Pictures";   read_write = false; }
    else if (strcmp(entry, "movies:read") == 0)          { xdg = "Videos";     read_write = false; }
    else if (strcmp(entry, "user-chosen") == 0)          { return 0;  /* dynamic via file picker */ }
    else return 0;  // unknown — parser won't let one through, so this is belt-and-suspenders.

    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", home, xdg);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    // Skip silently if the directory doesn't exist — the user may not
    // have one, and --ro-bind of a missing path is a fatal bwrap error.
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;

    if (read_write) {
        if (argv_push(a, "--bind") < 0) return -1;
    } else {
        if (argv_push(a, "--ro-bind") < 0) return -1;
    }
    if (argv_push(a, path) < 0) return -1;
    if (argv_push(a, path) < 0) return -1;
    return 0;
}

// -------------------------------------------------------------------
// consent helper lookup + exec
// -------------------------------------------------------------------

// Resolve moonbase-consent. Search order:
//   1. $MOONBASE_CONSENT_BIN (uninstalled / dev builds and tests)
//   2. <prefix>/libexec/moonbase-consent
//   3. <prefix>/bin/moonbase-consent
static int find_consent(char *out, size_t cap) {
    const char *env = getenv("MOONBASE_CONSENT_BIN");
    if (env && *env) {
        if (access(env, X_OK) == 0) {
            int n = snprintf(out, cap, "%s", env);
            if (n > 0 && (size_t)n < cap) return 0;
        }
    }
    const char *paths[] = {
        "/usr/local/libexec/moonbase-consent",
        "/usr/libexec/moonbase-consent",
        "/usr/local/bin/moonbase-consent",
        "/usr/bin/moonbase-consent",
        NULL,
    };
    for (size_t i = 0; paths[i]; i++) {
        if (access(paths[i], X_OK) == 0) {
            int n = snprintf(out, cap, "%s", paths[i]);
            if (n > 0 && (size_t)n < cap) return 0;
        }
    }
    return -1;
}

// Fork + exec the consent helper against a bundle. Returns:
//   1  → user approved, caller should mark xattr approved and continue
//   0  → user rejected (or helper failed) — caller should stop
//  -1  → couldn't run the helper at all (missing, crashed)
static int run_consent(const char *bundle_path, const char *bundle_id) {
    char consent_bin[PATH_MAX];
    if (find_consent(consent_bin, sizeof(consent_bin)) != 0) {
        fprintf(stderr,
            "moonbase-launch: bundle %s is quarantined but "
            "moonbase-consent isn't installed\n", bundle_id);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) { perror("moonbase-launch: fork(consent)"); return -1; }
    if (pid == 0) {
        char *child_argv[] = {
            consent_bin,
            (char *)bundle_path,
            (char *)bundle_id,
            NULL,
        };
        execv(consent_bin, child_argv);
        perror("moonbase-launch: execv(consent)");
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status) == 0 ? 1 : 0;
}

// -------------------------------------------------------------------
// main
// -------------------------------------------------------------------

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 2; }
    const char *bundle_arg = argv[1];
    if (strcmp(bundle_arg, "-h") == 0 || strcmp(bundle_arg, "--help") == 0) {
        usage(argv[0]); return 0;
    }

    // Split user's inner args after a bare "--".
    char **inner_user_argv = NULL;
    int inner_user_argc = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            inner_user_argv = &argv[i + 1];
            inner_user_argc = argc - (i + 1);
            break;
        }
    }
    (void)inner_user_argc;  // retained for future --help-style invariants

    // 1. Load bundle.
    mb_bundle_t bundle;
    char err[512] = {0};
    mb_bundle_err_t rc = mb_bundle_load(bundle_arg, &bundle, err, sizeof(err));
    if (rc != MB_BUNDLE_OK) {
        fprintf(stderr, "moonbase-launch: %s: %s (%s)\n",
                bundle_arg, err, mb_bundle_err_string(rc));
        return 2;
    }

    // 1b. Quarantine gate. Blocks the launch if the bundle's xattr says
    //     "pending" and the user refuses at the consent sheet — or if
    //     the xattr says "rejected" outright.
    mb_quarantine_status_t q = mb_quarantine_check(bundle.bundle_path);
    switch (q) {
    case MB_QUARANTINE_APPROVED:
    case MB_QUARANTINE_NO_XATTR:
        // NO_XATTR today is trusted because the fallback trust-db isn't
        // wired yet (D.4b). The day that lands, this branch looks up
        // (bundle_id, hash, bundle_path) in trust.db and routes through
        // consent on miss.
        break;
    case MB_QUARANTINE_REJECTED:
        fprintf(stderr,
            "moonbase-launch: %s refuses to launch (user rejected it "
            "on first run; clear user.moonbase.quarantine to reset)\n",
            bundle.info.id);
        mb_bundle_free(&bundle);
        return 3;
    case MB_QUARANTINE_PENDING:
    case MB_QUARANTINE_MALFORMED: {
        int rcc = run_consent(bundle.bundle_path, bundle.info.id);
        if (rcc == 1) {
            if (mb_quarantine_approve(bundle.bundle_path) != 0) {
                fprintf(stderr,
                    "moonbase-launch: warning: couldn't record "
                    "approval on %s: %s\n",
                    bundle.bundle_path, strerror(errno));
                // Continue anyway — the user did approve this launch.
            }
        } else if (rcc == 0) {
            // Explicit rejection. Persist it so the next launch short-
            // circuits straight to MB_QUARANTINE_REJECTED.
            (void)mb_quarantine_reject(bundle.bundle_path);
            fprintf(stderr,
                "moonbase-launch: %s: user declined at consent sheet\n",
                bundle.info.id);
            mb_bundle_free(&bundle);
            return 3;
        } else {
            // Consent helper absent or crashed — don't persist anything.
            mb_bundle_free(&bundle);
            return 3;
        }
        break;
    }
    case MB_QUARANTINE_ERR_IO:
        fprintf(stderr, "moonbase-launch: quarantine I/O error on %s\n",
                bundle.bundle_path);
        mb_bundle_free(&bundle);
        return 2;
    }

    // 2. Per-app data dir.
    const char *home = getenv("HOME");
    if (!home || !*home) {
        fprintf(stderr, "moonbase-launch: $HOME is unset\n");
        mb_bundle_free(&bundle);
        return 2;
    }
    char data_dir[PATH_MAX];
    int n = snprintf(data_dir, sizeof(data_dir),
                     "%s/.local/share/moonbase/%s", home, bundle.info.id);
    if (n < 0 || (size_t)n >= sizeof(data_dir)) {
        fprintf(stderr, "moonbase-launch: data dir path too long\n");
        mb_bundle_free(&bundle);
        return 2;
    }
    if (mkdir_p(data_dir, 0700) != 0) {
        fprintf(stderr, "moonbase-launch: mkdir(%s): %s\n",
                data_dir, strerror(errno));
        mb_bundle_free(&bundle);
        return 2;
    }
    // Apple-style subdirs — D.6 will read/manage these; create here so
    // the sandbox bind surfaces them to the app.
    const char *subdirs[] = {
        "Application Support", "Preferences", "Caches", "webview", NULL,
    };
    for (size_t i = 0; subdirs[i]; i++) {
        char sub[PATH_MAX];
        int sn = snprintf(sub, sizeof(sub), "%s/%s", data_dir, subdirs[i]);
        if (sn > 0 && (size_t)sn < sizeof(sub)) {
            if (mkdir(sub, 0700) != 0 && errno != EEXIST) {
                fprintf(stderr, "moonbase-launch: mkdir(%s): %s\n",
                        sub, strerror(errno));
            }
        }
    }

    // 3. Pick tier.
    const char *tier = bundle.info.lang == MB_INFO_APPC_LANG_WEB
                     ? "webview" : "native";

    // 4. Find profile.
    char profile_path[PATH_MAX];
    if (find_profile(tier, profile_path, sizeof(profile_path)) != 0) {
        fprintf(stderr,
            "moonbase-launch: couldn't find %s.profile "
            "(set MOONBASE_SANDBOX_DIR=<dir> to point at an uninstalled tree)\n",
            tier);
        mb_bundle_free(&bundle);
        return 2;
    }

    // 5. Decide unshare_net.
    bool want_net = bundle.info.perm_network_count > 0;
    const char *unshare_net = want_net ? "0" : "1";

    // 5b. system:process-list keeps the host PID namespace so /proc lists
    //     every process — Activity Monitor can't do its job with only
    //     its own sandbox's PIDs. Signal the profile via env var so the
    //     positional contract stays unchanged.
    for (size_t i = 0; i < bundle.info.perm_system_count; i++) {
        if (strcmp(bundle.info.perm_system[i], "process-list") == 0) {
            setenv("MOONBASE_UNSHARE_PID", "0", 1);
            break;
        }
    }

    // 6. Assemble bwrap argv.
    argv_t bw;
    argv_init(&bw);
    if (argv_push(&bw, "bwrap") < 0) goto oom;

    if (collect_profile_args(profile_path, bundle.bundle_path, data_dir,
                             unshare_net, home, &bw) != 0) {
        fprintf(stderr, "moonbase-launch: failed to source %s\n", profile_path);
        argv_free(&bw);
        mb_bundle_free(&bundle);
        return 2;
    }

    // MoonBase IPC socket: bind the whole XDG_RUNTIME_DIR so
    // $XDG_RUNTIME_DIR/moonbase.sock resolves the same path inside. The
    // socket itself may not exist yet if MoonRock hasn't started — bind
    // the directory, not the socket, so the path is creatable later.
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) {
        if (argv_push(&bw, "--bind") < 0) goto oom;
        if (argv_push(&bw, xdg) < 0) goto oom;
        if (argv_push(&bw, xdg) < 0) goto oom;
        if (argv_push(&bw, "--setenv") < 0) goto oom;
        if (argv_push(&bw, "XDG_RUNTIME_DIR") < 0) goto oom;
        if (argv_push(&bw, xdg) < 0) goto oom;
    }

    // Filesystem entitlements.
    for (size_t i = 0; i < bundle.info.perm_filesystem_count; i++) {
        if (add_filesystem_bind(&bw, bundle.info.perm_filesystem[i], home) != 0) {
            goto oom;
        }
    }

    // GL-mode bundles need /dev/dri so Mesa can reach the GPU. The
    // native.profile tier mounts a fresh /dev tmpfs (no renderD128 in
    // it by default); bind the host /dev/dri back in read-write. Mesa
    // falls back to software llvmpipe when card nodes aren't readable,
    // but the hardware path is what the Legion Go S wants. webview-
    // tier bundles already get /dev/dri via the webview.profile.
    if (bundle.info.render_default == MB_INFO_APPC_RENDER_GL) {
        struct stat dri_st;
        if (stat("/dev/dri", &dri_st) == 0 && S_ISDIR(dri_st.st_mode)) {
            if (argv_push(&bw, "--dev-bind") < 0) goto oom;
            if (argv_push(&bw, "/dev/dri") < 0) goto oom;
            if (argv_push(&bw, "/dev/dri") < 0) goto oom;
        }
    }

    // Let the app know its bundle id; useful for MoonRock introspection
    // and for the app's own logging path derivation.
    if (argv_push(&bw, "--setenv") < 0) goto oom;
    if (argv_push(&bw, "MOONBASE_BUNDLE_ID") < 0) goto oom;
    if (argv_push(&bw, bundle.info.id) < 0) goto oom;

    // 7. Inner exec + user args.
    if (argv_push(&bw, bundle.exe_abs_path) < 0) goto oom;
    if (inner_user_argv) {
        for (int i = 0; inner_user_argv[i]; i++) {
            if (argv_push(&bw, inner_user_argv[i]) < 0) goto oom;
        }
    }
    if (argv_push(&bw, NULL) < 0) goto oom;

    // Free the bundle before exec — the arg vector carries copies of
    // everything it needed. argv_free is moot because execvp replaces
    // the address space.
    mb_bundle_free(&bundle);

    execvp("bwrap", bw.data);

    // execvp returns only on failure.
    fprintf(stderr, "moonbase-launch: execvp(bwrap): %s\n", strerror(errno));
    argv_free(&bw);
    return 127;

oom:
    fprintf(stderr, "moonbase-launch: out of memory\n");
    argv_free(&bw);
    mb_bundle_free(&bundle);
    return 2;
}
