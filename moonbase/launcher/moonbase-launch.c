// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase-launch — bundle-aware, sandbox-aware launcher.
//
// Given a bundle path (.app shipping or .appdev developer directory):
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

#include "bundle/appimg.h"
#include "bundle/bundle.h"
#include "bundle/info_appc.h"
#include "bundle/quarantine.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
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
        "  %s <bundle.app|.appdev> [-- <app-args>...]\n"
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
// single-file .app mount lifecycle
// -------------------------------------------------------------------
//
// Single-file `.app` bundles are one ELF file: static stub, appended
// squashfs image, trailer. moonbase-launch handles them by reading
// the trailer, mounting the squashfs read-only with squashfuse at
// $XDG_RUNTIME_DIR/moonbase/mounts/<bundle-id>-<pid>/, and treating
// the mount as a normal directory-form bundle for the rest of the
// pipeline. On exit we fusermount -u and rmdir the mount point.
//
// squashfuse's own mount helper is the friendliest API — it
// daemonizes after the FS is ready, so fork + execvp + waitpid
// against the child returning 0 is the "mount is live" signal.
// Reusing the stock binary is also why we don't link libsquashfuse:
// a direct `execvp` keeps our LGPL surface area at zero.
//
// No kernel-facing privilege required anywhere — squashfuse is pure
// userspace FUSE, so this path runs without suid, without
// CAP_SYS_ADMIN, without any kernel mount() syscall.

// Forward decls — atexit_cleanup calls unmount_single_file_app, which
// is defined further down so the mount/unmount pair reads top-down
// in the order they run at launch.
static void unmount_single_file_app(const char *mount_path);

// Path of the currently-held FUSE mount, if any. Written once by main
// before fork/exec, consulted by the SIGTERM/SIGINT/SIGHUP handler
// during waitpid and by the post-child cleanup block. "" when no
// mount is held.
static char g_mount_path[PATH_MAX] = {0};

// PID of the bwrap child once main has forked it. The signal handler
// forwards termination signals to this PID so the child shuts down
// gracefully and the parent's waitpid unblocks cleanly, at which
// point we run the unmount. 0 while no child is live.
static volatile sig_atomic_t g_child_pid = 0;

// Async-signal-safe signal forwarder. Writes kill() to the child if
// one is running; otherwise the signal's default disposition would
// have terminated us before we got here, so the bare return is only
// reached for replay / spurious delivery. No locale-dependent libc,
// no stdio, no malloc — pure POSIX primitives.
static void forward_signal(int sig) {
    if (g_child_pid != 0) {
        (void)kill((pid_t)g_child_pid, sig);
    }
}

// atexit-hooked cleanup. Fires for any normal `return N` out of main
// (or any path that calls exit()). Does not fire on _exit, on signal
// death, or on a process crash — those leave the mount live, which
// the user clears with `fusermount -u` manually.
static void atexit_cleanup(void) {
    unmount_single_file_app(g_mount_path);
    g_mount_path[0] = '\0';
}

static void atexit_cleanup_register(void) {
    // atexit can be called multiple times; we only want the cleanup
    // once per launcher invocation. A static one-shot flag keeps it
    // idempotent even if the callsite grows more entry points.
    static bool registered = false;
    if (!registered) {
        (void)atexit(atexit_cleanup);
        registered = true;
    }
}

// Install SIGTERM/SIGINT/SIGHUP handlers that forward to the bwrap
// child. SA_RESTART keeps waitpid from returning EINTR when the
// handler fires between syscalls. Called once, right before the
// child fork, so earlier failures take the default disposition
// (terminate) — acceptable because nothing irreversible has been
// written outside the mount dir yet.
static void install_child_signal_forwarding(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = forward_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT,  &sa, NULL);
    (void)sigaction(SIGHUP,  &sa, NULL);
}

// Resolve the squashfuse binary. Override via $MOONBASE_SQUASHFUSE_BIN
// for tests; otherwise fall through to $PATH via execvp. Returns a
// pointer into the env string or a literal — never allocates.
static const char *squashfuse_bin(void) {
    const char *env = getenv("MOONBASE_SQUASHFUSE_BIN");
    if (env && *env) return env;
    return "squashfuse";
}

// Same shape for fusermount — tests drop in a stub via env.
static const char *fusermount_bin(void) {
    const char *env = getenv("MOONBASE_FUSERMOUNT_BIN");
    if (env && *env) return env;
    return "fusermount";
}

// Compute mount_path for a given bundle-id and emit it into out.
// Shape:  $XDG_RUNTIME_DIR/moonbase/mounts/<bundle-id>-<pid>.app/
// Two launches of the same bundle get distinct mount dirs thanks to
// the pid suffix; no reference-counted / shared mount. Bundle-id
// came from the trailer (not Info.appc), so this works before the
// squashfs is visible. The `.app` suffix is load-bearing:
// mb_bundle_load's §8 validator rejects any bundle path whose
// basename does not end in `.app` or `.appdev`, and the mount dir
// IS the bundle dir the launcher then loads.
static int build_mount_path(const char *bundle_id, char *out, size_t cap) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (!xdg || !*xdg) {
        // Fall back to /tmp so smoke tests without a user session
        // still work. Plain user-process permissions cover us.
        xdg = "/tmp";
    }
    int n = snprintf(out, cap, "%s/moonbase/mounts/%s-%ld.app",
                     xdg, bundle_id, (long)getpid());
    if (n < 0 || (size_t)n >= cap) return -1;
    return 0;
}

// Mount a single-file .app with squashfuse. On success, *out_mount
// holds the mount dir path (owned by the caller's buffer — never
// freed, always sized PATH_MAX). On failure, returns non-zero and
// any partial mount-dir is rmdir'd.
//
// Strategy:
//   1. Parse the trailer to get bundle-id + squashfs_offset.
//   2. mkdir -p the mount path under XDG_RUNTIME_DIR.
//   3. fork + execvp "squashfuse -o offset=<N>,ro <file> <mount>".
//      squashfuse backgrounds itself once the FS is serving, so
//      waitpid returning 0 means the mount is live.
//   4. rmdir mount dir on any early failure so we never leave
//      empty mount stubs around the runtime dir.
static int mount_single_file_app(const char *path,
                                 char *out_mount, size_t mount_cap,
                                 char *err, size_t err_cap) {
    mb_appimg_trailer_t t;
    char aerr[256];
    mb_appimg_err_t ar = mb_appimg_read_trailer_path(path, &t, aerr, sizeof(aerr));
    if (ar != MB_APPIMG_OK) {
        snprintf(err, err_cap, "appimg trailer: %s (%s)",
                 aerr, mb_appimg_err_string(ar));
        return -1;
    }

    if (build_mount_path(t.bundle_id, out_mount, mount_cap) != 0) {
        snprintf(err, err_cap, "mount path too long");
        mb_appimg_trailer_free(&t);
        return -1;
    }
    if (mkdir_p(out_mount, 0700) != 0) {
        snprintf(err, err_cap, "mkdir %s: %s", out_mount, strerror(errno));
        mb_appimg_trailer_free(&t);
        return -1;
    }

    char offset_arg[64];
    snprintf(offset_arg, sizeof(offset_arg),
             "offset=%llu,ro", (unsigned long long)t.squashfs_offset);

    const char *sqfs_bin = squashfuse_bin();
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, err_cap, "fork(squashfuse): %s", strerror(errno));
        (void)rmdir(out_mount);
        mb_appimg_trailer_free(&t);
        return -1;
    }
    if (pid == 0) {
        char *const child_argv[] = {
            (char *)sqfs_bin,
            "-o", offset_arg,
            (char *)path,
            out_mount,
            NULL,
        };
        execvp(sqfs_bin, child_argv);
        // Child-only branch; parent already forked, stdio is still
        // wired to the controlling tty so a one-line message is OK.
        fprintf(stderr, "moonbase-launch: execvp(%s): %s\n",
                sqfs_bin, strerror(errno));
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        snprintf(err, err_cap, "waitpid(squashfuse): %s", strerror(errno));
        (void)rmdir(out_mount);
        mb_appimg_trailer_free(&t);
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        snprintf(err, err_cap, "squashfuse failed (status %d)", status);
        (void)rmdir(out_mount);
        mb_appimg_trailer_free(&t);
        return -1;
    }

    mb_appimg_trailer_free(&t);
    return 0;
}

// Tear the mount down. Never fails the process: on error we log and
// move on — the worst-case is a stale mount the user clears with
// `fusermount -u` themselves. Safe to call with an empty path.
static void unmount_single_file_app(const char *mount_path) {
    if (!mount_path || !*mount_path) return;

    const char *fu_bin = fusermount_bin();
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "moonbase-launch: fork(fusermount): %s\n",
                strerror(errno));
        return;
    }
    if (pid == 0) {
        char *const child_argv[] = {
            (char *)fu_bin, "-u", (char *)mount_path, NULL,
        };
        execvp(fu_bin, child_argv);
        fprintf(stderr, "moonbase-launch: execvp(%s): %s\n",
                fu_bin, strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "moonbase-launch: waitpid(fusermount): %s\n",
                strerror(errno));
        return;
    }
    // rmdir the empty mount point. Fails silently if fusermount left
    // the mount live for any reason — better than masking the real
    // error with a misleading follow-up.
    (void)rmdir(mount_path);
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
    // env_dir may be NULL when running from an installed tree; bounded
    // iteration ensures a NULL candidate is skipped instead of ending
    // the loop, so the install-path fallbacks are always reachable.
    const char *candidates[] = {
        env_dir,                                        // 1
        "/usr/local/share/moonbase/sandbox",            // 2a
        "/usr/share/moonbase/sandbox",                  // 2b
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
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
// entitlement env builder
// -------------------------------------------------------------------

// Compose MOONBASE_ENTITLEMENTS from all five perm_* arrays. The format
// is "group1=v1,v2;group2=v3" — missing groups don't appear. libmoonbase
// reads this string once and gates privileged APIs (keychain, …) at
// their top edge with MB_EPERM when a declaration is missing.
//
// Returns a malloc'd NUL-terminated buffer on success. Returns NULL
// only on allocation failure; an empty entitlement set returns an
// empty string ("") which is still valid output.
static char *build_entitlements_env(const mb_info_appc_t *info) {
    size_t cap = 256;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t len = 0;
    buf[0] = '\0';

    struct group { const char *name; char **values; size_t count; };
    struct group groups[] = {
        {"filesystem", info->perm_filesystem, info->perm_filesystem_count},
        {"network",    info->perm_network,    info->perm_network_count},
        {"hardware",   info->perm_hardware,   info->perm_hardware_count},
        {"system",     info->perm_system,     info->perm_system_count},
        {"ipc",        info->perm_ipc,        info->perm_ipc_count},
    };

    for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); g++) {
        if (groups[g].count == 0) continue;

        // Pre-compute the bytes this group needs so one realloc covers
        // the ';', the "group=", every ',' value, and the NUL.
        size_t need = len + 1 /* ';' */
                          + strlen(groups[g].name) + 1 /* '=' */
                          + 1 /* trailing NUL */;
        for (size_t v = 0; v < groups[g].count; v++) {
            need += strlen(groups[g].values[v]) + 1; /* ',' or join */
        }
        if (need >= cap) {
            while (cap < need + 1) cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }

        if (len > 0) buf[len++] = ';';
        size_t nl = strlen(groups[g].name);
        memcpy(buf + len, groups[g].name, nl); len += nl;
        buf[len++] = '=';
        for (size_t v = 0; v < groups[g].count; v++) {
            if (v > 0) buf[len++] = ',';
            size_t vl = strlen(groups[g].values[v]);
            memcpy(buf + len, groups[g].values[v], vl); len += vl;
        }
    }
    buf[len] = '\0';
    return buf;
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

    // 0. Single-file `.app` detection. If the argument is a regular
    //    file carrying the appimg trailer, mount its squashfs tail
    //    and redirect the rest of the pipeline at the mount point.
    //    Directory-form `.app` / `.appdev` inputs skip this block
    //    entirely; for them bundle_dir == bundle_arg and no mount
    //    is held.
    char mount_buf[PATH_MAX] = {0};
    const char *bundle_dir = bundle_arg;
    bool detected = false;
    char probe_err[128] = {0};
    mb_appimg_err_t probe = mb_appimg_is_single_file(bundle_arg, &detected);
    if (probe != MB_APPIMG_OK) {
        fprintf(stderr, "moonbase-launch: probe %s: %s\n",
                bundle_arg, mb_appimg_err_string(probe));
        return 2;
    }
    if (detected) {
        if (mount_single_file_app(bundle_arg, mount_buf, sizeof(mount_buf),
                                  probe_err, sizeof(probe_err)) != 0) {
            fprintf(stderr, "moonbase-launch: %s: %s\n",
                    bundle_arg, probe_err);
            return 2;
        }
        // Record globally so the signal handler and the final
        // cleanup block can find the mount without threading a
        // context struct through every return path.
        memcpy(g_mount_path, mount_buf, sizeof(g_mount_path));
        bundle_dir = g_mount_path;

        // Register cleanup for every normal-exit path below. atexit
        // handlers run after `return` from main or `exit()`, which
        // covers every early-fail `return 2/3` without having to
        // pepper the rest of the function with unmount calls. On
        // signal death we rely on fork+waitpid + the signal
        // forwarder further down to let main exit normally; a
        // SIGKILL or launcher crash leaves the squashfuse daemon
        // alive and the user clears it with fusermount -u, matching
        // Slice 17-B's design note.
        atexit_cleanup_register();
    }

    // 1. Load bundle.
    mb_bundle_t bundle;
    char err[512] = {0};
    mb_bundle_err_t rc = mb_bundle_load(bundle_dir, &bundle, err, sizeof(err));
    if (rc != MB_BUNDLE_OK) {
        fprintf(stderr, "moonbase-launch: %s: %s (%s)\n",
                bundle_dir, err, mb_bundle_err_string(rc));
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

    // MOONBASE_ENTITLEMENTS: declared entitlement set from Info.appc.
    // libmoonbase reads this once and gates privileged APIs with
    // MB_EPERM when the matching permission wasn't declared. --clearenv
    // in the profile wipes the host env, so this --setenv is the only
    // way the string reaches the sandbox.
    {
        char *ent_str = build_entitlements_env(&bundle.info);
        if (!ent_str) goto oom;
        int push_rc = 0;
        push_rc |= argv_push(&bw, "--setenv");
        push_rc |= argv_push(&bw, "MOONBASE_ENTITLEMENTS");
        push_rc |= argv_push(&bw, ent_str);
        free(ent_str);
        if (push_rc < 0) goto oom;
    }

    // Legacy Mode menu-export bootstrap: hand the inner toolkit the env
    // vars that make it export its menu tree on the session bus through
    // com.canonical.AppMenu.Registrar. The chrome stub (or, in a full
    // CopyCatOS session, the menubar daemon) owns the registrar and
    // imports the menus over DBusMenu. Without these, Qt/GTK draw their
    // own menu inside the client window — which is exactly the duplicate
    // chrome the global menu bar exists to avoid.
    //
    // Qt5 uses the standalone `appmenu-qt5` platformtheme plugin
    // (package: appmenu-qt5). Qt6 dropped support for that plugin; the
    // KDE Plasma 6 platform theme (`kde`, package: plasma-integration /
    // plasma6-integration) inherited DBusMenu export and is the path Qt6
    // apps take. Both speak the Canonical com.canonical.AppMenu.Registrar
    // protocol our bridge owns. The GTK_MODULES value matches the
    // appmenu-gtk-module shipped on Nobara via vala-panel-appmenu-gtk-module
    // (and the older Canonical appmenu-gtk-module package on Ubuntu);
    // the Unity name (unity-gtk-module) is intentionally not used so the
    // bridge contract is one stable module name across distros.
    //
    // Native bundles get neither — they speak MoonBase IPC for menus,
    // not DBusMenu, so loading appmenu modules would just add noise.
    if (bundle.info.wrap_toolkit == MB_INFO_APPC_WRAP_QT5) {
        if (argv_push(&bw, "--setenv") < 0) goto oom;
        if (argv_push(&bw, "QT_QPA_PLATFORMTHEME") < 0) goto oom;
        if (argv_push(&bw, "appmenu-qt5") < 0) goto oom;
    } else if (bundle.info.wrap_toolkit == MB_INFO_APPC_WRAP_QT6) {
        if (argv_push(&bw, "--setenv") < 0) goto oom;
        if (argv_push(&bw, "QT_QPA_PLATFORMTHEME") < 0) goto oom;
        if (argv_push(&bw, "kde") < 0) goto oom;
    } else if (bundle.info.wrap_toolkit == MB_INFO_APPC_WRAP_GTK3 ||
               bundle.info.wrap_toolkit == MB_INFO_APPC_WRAP_GTK4) {
        if (argv_push(&bw, "--setenv") < 0) goto oom;
        if (argv_push(&bw, "GTK_MODULES") < 0) goto oom;
        if (argv_push(&bw, "appmenu-gtk-module") < 0) goto oom;
    }

    // Legacy Mode X11 passthrough. native.profile uses --clearenv and a
    // /tmp tmpfs, which strips DISPLAY/XAUTHORITY and hides the X server
    // socket dir from the sandbox. A toolkit-wrapped (Qt/GTK) bundle has
    // no choice but to talk X11 — it has no MoonBase IPC path — so we
    // re-bind the socket dir and re-export the two env vars here. Native
    // MoonBase bundles deliberately do not get DISPLAY: they render
    // through libmoonbase and the moonbase.sock IPC, and granting raw
    // X11 access would weaken sandbox privacy with no upside.
    //
    // XAUTHORITY may live anywhere — usually $XDG_RUNTIME_DIR/xauth_xxx
    // (already covered by the runtime-dir bind above) but on some distros
    // it's ~/.Xauthority, which native.profile's --tmpfs $HOME blanks.
    // Bind unconditionally on whatever path the env var points to so the
    // location doesn't matter. The chrome stub itself runs OUTSIDE the
    // sandbox, so this block has no effect on it.
    if (bundle.info.wrap_toolkit != MB_INFO_APPC_WRAP_NATIVE) {
        struct stat x11_st;
        if (stat("/tmp/.X11-unix", &x11_st) == 0 && S_ISDIR(x11_st.st_mode)) {
            if (argv_push(&bw, "--ro-bind") < 0) goto oom;
            if (argv_push(&bw, "/tmp/.X11-unix") < 0) goto oom;
            if (argv_push(&bw, "/tmp/.X11-unix") < 0) goto oom;
        }
        const char *display = getenv("DISPLAY");
        if (display && *display) {
            if (argv_push(&bw, "--setenv") < 0) goto oom;
            if (argv_push(&bw, "DISPLAY") < 0) goto oom;
            if (argv_push(&bw, display) < 0) goto oom;
        }
        const char *xauth = getenv("XAUTHORITY");
        if (xauth && *xauth) {
            struct stat xa_st;
            if (stat(xauth, &xa_st) == 0 && S_ISREG(xa_st.st_mode)) {
                if (argv_push(&bw, "--ro-bind") < 0) goto oom;
                if (argv_push(&bw, xauth) < 0) goto oom;
                if (argv_push(&bw, xauth) < 0) goto oom;
                if (argv_push(&bw, "--setenv") < 0) goto oom;
                if (argv_push(&bw, "XAUTHORITY") < 0) goto oom;
                if (argv_push(&bw, xauth) < 0) goto oom;
            }
        }
    }

    // 7. Inner exec + user args.
    //
    // Legacy Mode toolkit hint ([wrap].toolkit in Info.appc):
    //
    //   * non-native — bwrap is told `--argv0 <bundle.id>`, which sets
    //     argv[0] inside the sandbox to the reverse-DNS bundle id. GTK
    //     reads argv[0] basename for g_get_prgname() → WM_CLASS.res_name,
    //     so this alone is enough for GTK3/4 wrappers.
    //   * qt5 / qt6 — Qt picks res_name from QGuiApplication's own argv
    //     parser, not from argv[0]. Pass `-name <bundle.id>` after the
    //     inner exec so QApplication::setApplicationName() / WM_CLASS
    //     instance both land on the bundle id.
    //
    // The chrome stub finds the bundle's top-level window by matching
    // XClassHint.res_name against the bundle id (see chrome_stub_*).
    if (bundle.info.wrap_toolkit != MB_INFO_APPC_WRAP_NATIVE) {
        if (argv_push(&bw, "--argv0") < 0) goto oom;
        if (argv_push(&bw, bundle.info.id) < 0) goto oom;
    }
    if (argv_push(&bw, bundle.exe_abs_path) < 0) goto oom;
    if (bundle.info.wrap_toolkit == MB_INFO_APPC_WRAP_QT5 ||
        bundle.info.wrap_toolkit == MB_INFO_APPC_WRAP_QT6) {
        if (argv_push(&bw, "-name") < 0) goto oom;
        if (argv_push(&bw, bundle.info.id) < 0) goto oom;
    }
    if (inner_user_argv) {
        for (int i = 0; inner_user_argv[i]; i++) {
            if (argv_push(&bw, inner_user_argv[i]) < 0) goto oom;
        }
    }
    if (argv_push(&bw, NULL) < 0) goto oom;

    // Free the bundle before exec — the arg vector carries copies of
    // everything it needed.
    mb_bundle_free(&bundle);

    // If we didn't mount anything, keep the legacy execvp path so
    // the launcher PID is replaced by bwrap exactly like before.
    // Callers that don't know about the single-file case (SDDM
    // session scripts, a menubar quicklauncher) don't get a new
    // supervising process just because they asked for a .appdev.
    if (g_mount_path[0] == '\0') {
        execvp("bwrap", bw.data);
        fprintf(stderr, "moonbase-launch: execvp(bwrap): %s\n", strerror(errno));
        argv_free(&bw);
        return 127;
    }

    // Single-file `.app` path: the launcher has to outlive bwrap so
    // it can fusermount -u / rmdir. Fork the child, forward
    // termination signals (SIGINT/SIGTERM/SIGHUP) to it, waitpid,
    // then let atexit run the unmount before main returns.
    install_child_signal_forwarding();

    pid_t bwpid = fork();
    if (bwpid < 0) {
        fprintf(stderr, "moonbase-launch: fork(bwrap): %s\n", strerror(errno));
        argv_free(&bw);
        return 127;
    }
    if (bwpid == 0) {
        execvp("bwrap", bw.data);
        fprintf(stderr, "moonbase-launch: execvp(bwrap): %s\n", strerror(errno));
        _exit(127);
    }
    g_child_pid = bwpid;

    int wstatus = 0;
    for (;;) {
        pid_t wr = waitpid(bwpid, &wstatus, 0);
        if (wr >= 0) break;
        if (errno == EINTR) continue;
        fprintf(stderr, "moonbase-launch: waitpid(bwrap): %s\n", strerror(errno));
        argv_free(&bw);
        return 127;
    }
    g_child_pid = 0;
    argv_free(&bw);

    // Map bwrap's exit the way a shell would see it if we had kept
    // execvp: normal exit -> WEXITSTATUS, signal death -> 128 + sig.
    if (WIFEXITED(wstatus)) {
        return WEXITSTATUS(wstatus);
    }
    if (WIFSIGNALED(wstatus)) {
        return 128 + WTERMSIG(wstatus);
    }
    return 127;

oom:
    fprintf(stderr, "moonbase-launch: out of memory\n");
    argv_free(&bw);
    mb_bundle_free(&bundle);
    return 2;
}
