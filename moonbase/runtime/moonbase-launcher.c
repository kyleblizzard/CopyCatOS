// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase-launcher.c — Portable public entry point for CopyCatOS .app bundles.
//
// Two paths, one binary:
//
//   1. Full CopyCatOS session — XDG_SESSION_DESKTOP=CopyCatOS (set by
//      moonrock-session.sh; mirrored in XDG_CURRENT_DESKTOP). No XDG
//      .desktop work, no chrome stub. execvp moonbase-launch and let
//      the existing 927-line bundle / sandbox / exec path do its job.
//      This is the fast path. Zero observable change vs. invoking
//      moonbase-launch directly today.
//
//   2. Foreign distro / no MoonRock — KDE, GNOME, XFCE, anywhere.
//      Fork moonrock-lite as a sidecar (it draws the title bar + menu
//      bar above the bundle window using shared chrome render code),
//      register an XDG .desktop entry on first launch so the bundle
//      shows up like any native app, then execvp moonbase-launch.
//
// Bundle parsing, sandbox tier selection, entitlement bindings, network
// namespace decisions, and the final execvp into Contents/CopyCatOS/<bin>
// all live in moonbase-launch (../launcher/moonbase-launch.c). Chrome
// painting lives in moonrock-lite (../../moonrock-lite/). Three binaries,
// one each for portability, chrome, and sandboxing — none duplicates
// another. Pre-19.H.1.a the chrome stub was inline in this file; slice
// 19.H.1.a moved it out to give it its own restart story (a chrome
// crash no longer takes the bundle with it) and a clean seam for
// 19.H.1.b — migrating paint onto the shared host_chrome painter.

#include "bundle/appimg.h"
#include "bundle/bundle.h"
#include "icon_bucket.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// Slice 19.H.1.a — the chrome stub moved to its own binary
// (moonrock-lite). The launcher no longer paints anything itself; it
// forks, execvp's "moonrock-lite", and forwards the resolved theme as
// an integer on the command line. Mirroring the enum locally keeps the
// theme-resolver code below unchanged without dragging cairo + X11 +
// menubar_render.h into a process that no longer touches a pixel.
typedef enum {
    MENUBAR_THEME_AQUA               = 0,
    MENUBAR_THEME_HOST_BREEZE_LIGHT  = 1,
    MENUBAR_THEME_HOST_ADWAITA_LIGHT = 2,
} menubar_render_theme_t;

// ----------------------------------------------------------------------------
// Manifest + config — minimal launcher-tier shape
// ----------------------------------------------------------------------------

// Just the fields the launcher needs to decide portability behavior.
// Sandbox tier, entitlements, language, executable name — all that
// stays in moonbase-launch. The launcher never opens Info.appc itself.
typedef struct {
    char bundle_id[128];
    char display_name[128];
    char icon_relpath[256];          // Contents/Resources/<this>
    bool host_theme_override_present;
    bool host_theme_override_value;
    // 19.H.2.d — toolkit hint decides chrome sidecar dispatch on a foreign
    // distro: NATIVE → moonrock-host (active X frontend, owns the window
    // and paints chrome around a Cairo backing surface the bundle commits
    // over IPC), anything else → moonrock-lite (passive sidecar over a
    // toolkit-owned window).
    mb_info_appc_wrap_toolkit_t toolkit;
} LauncherManifest;

// ~/.config/copycatos/moonbase.conf — global launcher *defaults* only.
// Per-app persistence is owned by MoonBase Preferences (Apple-style:
// ~/.local/share/moonbase/<bundle-id>/Preferences/). The user toggles
// host theme per-app via the app's own View → Use Host Desktop Theme
// menu item (standard, framework-injected). This file's value is the
// default for any app that has no per-app preference recorded yet.
typedef struct {
    bool host_theme_enabled;         // [theme] host_theme_enabled = true|false (default false)
} LauncherConfig;

// ----------------------------------------------------------------------------
// Session detection
// ----------------------------------------------------------------------------

// True inside a full CopyCatOS session. moonrock-session.sh sets
// XDG_SESSION_DESKTOP=CopyCatOS (the canonical signal per CLAUDE.md)
// and mirrors it into XDG_CURRENT_DESKTOP for any consumer that reads
// the broader spec variant. Foreign distros set neither to "CopyCatOS",
// which is the entire portability fork.
static bool in_full_copycatos_session(void) {
    const char *desktop = getenv("XDG_SESSION_DESKTOP");
    if (desktop && strcmp(desktop, "CopyCatOS") == 0) return true;
    const char *current = getenv("XDG_CURRENT_DESKTOP");
    if (current && strcmp(current, "CopyCatOS") == 0) return true;
    return false;
}

// ----------------------------------------------------------------------------
// Filesystem helpers
// ----------------------------------------------------------------------------

// Recursive mkdir — POSIX has no -p flag for the syscall. Returns 0 on
// success or if every component already exists.
static int mkdir_p(const char *path, mode_t mode) {
    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof buf) { errno = ENAMETOOLONG; return -1; }
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

// Resolve XDG_DATA_HOME with the spec fallback to $HOME/.local/share.
// Returns 0 on success; -1 if neither variable is set (a process with
// no $HOME can't host an Aqua app anyway, so fail loudly downstream).
static int resolve_xdg_data_home(char *out, size_t cap) {
    const char *xh = getenv("XDG_DATA_HOME");
    if (xh && *xh) {
        int n = snprintf(out, cap, "%s", xh);
        return (n < 0 || (size_t)n >= cap) ? -1 : 0;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;
    int n = snprintf(out, cap, "%s/.local/share", home);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

// Mirror of the data-home resolver for XDG_CONFIG_HOME → $HOME/.config.
// Used by the slice 19.F theme resolver to find moonbase.conf.
static int resolve_xdg_config_home(char *out, size_t cap) {
    const char *xh = getenv("XDG_CONFIG_HOME");
    if (xh && *xh) {
        int n = snprintf(out, cap, "%s", xh);
        return (n < 0 || (size_t)n >= cap) ? -1 : 0;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;
    int n = snprintf(out, cap, "%s/.config", home);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

// Strip ASCII whitespace from both ends of `s` in place. INI values like
// `host_theme_enabled = true` arrive with leading + trailing space; we
// don't want to ship a regex dep just for one option.
static void strip_inplace(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' '  || s[len - 1] == '\t' ||
                       s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

// Liberal bool parser — accepts the four words a user might write in a
// config file plus 1/0. Anything else returns false via *out_valid so
// the caller can fall through to the next precedence tier rather than
// silently picking a wrong default.
static bool parse_bool_loose(const char *s, bool *out_valid) {
    if (!s) { *out_valid = false; return false; }
    if (strcasecmp(s, "true")  == 0 || strcasecmp(s, "yes") == 0 ||
        strcmp(s, "1") == 0)        { *out_valid = true;  return true;  }
    if (strcasecmp(s, "false") == 0 || strcasecmp(s, "no")  == 0 ||
        strcmp(s, "0") == 0)        { *out_valid = true;  return false; }
    *out_valid = false; return false;
}

// Atomic write — emit `contents` to `<target>.tmp.<pid>` then rename.
// rename(2) is atomic within a filesystem, so a power loss mid-write
// can never leave a half-formed .desktop file the host DE indexes.
static int write_atomic(const char *target_path, const char *contents) {
    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.%ld",
                     target_path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof tmp) { errno = ENAMETOOLONG; return -1; }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    size_t left = strlen(contents);
    while (left > 0) {
        ssize_t w = write(fd, contents, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            close(fd); unlink(tmp); errno = e;
            return -1;
        }
        contents += w; left -= (size_t)w;
    }
    if (close(fd) != 0) { int e = errno; unlink(tmp); errno = e; return -1; }
    if (rename(tmp, target_path) != 0) {
        int e = errno; unlink(tmp); errno = e; return -1;
    }
    return 0;
}

// Plain byte-copy from src to dst. Used to hoist AppIcon.png out of a
// (possibly mounted) bundle's Resources/ into the host's hicolor tree
// so the icon survives later unmounts.
static int copy_file(const char *src, const char *dst) {
    int sfd = open(src, O_RDONLY);
    if (sfd < 0) return -1;
    int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dfd < 0) { int e = errno; close(sfd); errno = e; return -1; }
    char buf[8192];
    for (;;) {
        ssize_t r = read(sfd, buf, sizeof buf);
        if (r == 0) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno; close(sfd); close(dfd); errno = e; return -1;
        }
        char *p = buf; size_t left = (size_t)r;
        while (left > 0) {
            ssize_t w = write(dfd, p, left);
            if (w < 0) {
                if (errno == EINTR) continue;
                int e = errno; close(sfd); close(dfd); errno = e; return -1;
            }
            p += w; left -= (size_t)w;
        }
    }
    close(sfd);
    if (close(dfd) != 0) return -1;
    return 0;
}

// Best-effort fork+exec of a single-arg helper. Used for the cache
// refreshers (`update-desktop-database`, `gtk-update-icon-cache`) that
// improve first-launch UX but aren't load-bearing — if they're missing,
// the .desktop entry still appears on the next session login. stdio is
// redirected to /dev/null so a missing binary doesn't pollute the
// launcher's terminal output.
static void run_best_effort(const char *prog, const char *arg) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        if (arg) execlp(prog, prog, arg, (char *)NULL);
        else     execlp(prog, prog, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    (void)waitpid(pid, &status, 0);
}

// ----------------------------------------------------------------------------
// Single-file .app mount lifecycle
// ----------------------------------------------------------------------------
//
// 19.E: this block mirrors moonbase-launch.c's mount/unmount lifecycle
// (../launcher/moonbase-launch.c, ~lines 230-370) — read the appimg
// trailer, mkdir a per-pid mount dir under $XDG_RUNTIME_DIR, fork+exec
// `squashfuse -o offset=N,ro <path> <mount>`, waitpid for daemonize.
//
// The duplication is deliberate. moonbase-launch is the authoritative
// mount owner — it sandboxes the bundle and lives for the bundle's
// lifetime, so its atexit unmount is the one that matters. The
// launcher mounts only long enough to read Info.appc + AppIcon.png,
// then unmounts before exec'ing into moonbase-launch (which mounts
// again). atexit hooks don't fire across execvp, so an atexit-based
// design here would leak a mount on every launch. The double-mount
// cost (one per launch) is the price of a correct cleanup story.
// Extract to libmoonbase if a third consumer ever appears.

static const char *squashfuse_bin(void) {
    const char *e = getenv("MOONBASE_SQUASHFUSE_BIN");
    return (e && *e) ? e : "squashfuse";
}

static const char *fusermount_bin(void) {
    const char *e = getenv("MOONBASE_FUSERMOUNT_BIN");
    return (e && *e) ? e : "fusermount";
}

// Mount path: $XDG_RUNTIME_DIR/moonbase/mounts/launcher-<bundle-id>-<pid>.app/
// The `launcher-` prefix keeps the launcher's transient mount distinct
// from the long-lived moonbase-launch mount under the same parent dir;
// double-mounting the same .app concurrently is the expected case.
// `.app` suffix is load-bearing — mb_bundle_load's §8 validator rejects
// any path whose basename does not end in .app or .appdev.
static int build_mount_path(const char *bundle_id, char *out, size_t cap) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (!xdg || !*xdg) xdg = "/tmp";
    int n = snprintf(out, cap,
                     "%s/moonbase/mounts/launcher-%s-%ld.app",
                     xdg, bundle_id, (long)getpid());
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

// Mount the squashfs payload. On success, *out_mount holds the absolute
// mount dir path and *out_trailer holds the parsed trailer (caller
// frees via mb_appimg_trailer_free). On failure, both outputs are
// already cleaned up and an error has been printed to stderr.
static int mount_single_file_app(const char *path,
                                 char *out_mount, size_t mount_cap,
                                 mb_appimg_trailer_t *out_trailer) {
    char aerr[256];
    mb_appimg_err_t ar = mb_appimg_read_trailer_path(path, out_trailer,
                                                     aerr, sizeof aerr);
    if (ar != MB_APPIMG_OK) {
        fprintf(stderr,
                "[moonbase-launcher] appimg trailer: %s (%s)\n",
                aerr, mb_appimg_err_string(ar));
        return -1;
    }
    if (build_mount_path(out_trailer->bundle_id, out_mount, mount_cap) != 0) {
        fprintf(stderr, "[moonbase-launcher] mount path too long\n");
        mb_appimg_trailer_free(out_trailer);
        return -1;
    }
    if (mkdir_p(out_mount, 0700) != 0) {
        fprintf(stderr, "[moonbase-launcher] mkdir %s: %s\n",
                out_mount, strerror(errno));
        mb_appimg_trailer_free(out_trailer);
        return -1;
    }

    char offset_arg[64];
    snprintf(offset_arg, sizeof offset_arg,
             "offset=%llu,ro",
             (unsigned long long)out_trailer->squashfs_offset);

    const char *sqfs = squashfuse_bin();
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[moonbase-launcher] fork(squashfuse): %s\n",
                strerror(errno));
        (void)rmdir(out_mount);
        mb_appimg_trailer_free(out_trailer);
        return -1;
    }
    if (pid == 0) {
        char *child_argv[] = {
            (char *)sqfs, "-o", offset_arg,
            (char *)path, out_mount, NULL,
        };
        execvp(sqfs, child_argv);
        fprintf(stderr, "[moonbase-launcher] execvp(%s): %s\n",
                sqfs, strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "[moonbase-launcher] waitpid(squashfuse): %s\n",
                strerror(errno));
        (void)rmdir(out_mount);
        mb_appimg_trailer_free(out_trailer);
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr,
                "[moonbase-launcher] squashfuse failed (status=%d)\n",
                status);
        (void)rmdir(out_mount);
        mb_appimg_trailer_free(out_trailer);
        return -1;
    }
    return 0;
}

// Tear the FUSE mount down. Never blocks the launch on failure — a
// stale mount the user clears with `fusermount -u` themselves is
// strictly better than masking the underlying error with a follow-up.
static void unmount_single_file_app(const char *mount_path) {
    if (!mount_path || !*mount_path) return;
    const char *fu = fusermount_bin();
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[moonbase-launcher] fork(fusermount): %s\n",
                strerror(errno));
        return;
    }
    if (pid == 0) {
        char *child_argv[] = {
            (char *)fu, "-u", (char *)mount_path, NULL,
        };
        execvp(fu, child_argv);
        fprintf(stderr, "[moonbase-launcher] execvp(%s): %s\n",
                fu, strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "[moonbase-launcher] waitpid(fusermount): %s\n",
                strerror(errno));
        return;
    }
    (void)rmdir(mount_path);
}

// ----------------------------------------------------------------------------
// Bundle manifest — minimal launcher view over mb_bundle_load
// ----------------------------------------------------------------------------

// Read the manifest fields the launcher needs (bundle id, display name,
// icon path) by running the canonical bundle-spec.md §8 validator from
// libmoonbase. moonbase-launch will run the same validator a second
// time when we exec it — that's deliberate. The launcher must not exec
// against a malformed bundle (we'd register a broken .desktop entry,
// then fail past the point where we could surface the error nicely).
//
// `bundle_root` is the directory the validator can stat: a .appdev
// directly, or — for a single-file .app — the squashfuse mount path
// produced by mount_single_file_app(). The original argv[1] (the .app
// file the user double-clicked) is what we pass to moonbase-launch
// later; bundle_root is purely the manifest read path.
static bool bundle_manifest_load(const char *bundle_root,
                                 LauncherManifest *out) {
    mb_bundle_t b = {0};
    char err[256] = {0};
    mb_bundle_err_t rc = mb_bundle_load(bundle_root, &b, err, sizeof err);
    if (rc != MB_BUNDLE_OK) {
        fprintf(stderr,
                "[moonbase-launcher] %s: %s\n",
                mb_bundle_err_string(rc), err);
        return false;
    }
    snprintf(out->bundle_id,    sizeof out->bundle_id,    "%s", b.info.id);
    snprintf(out->display_name, sizeof out->display_name, "%s", b.info.name);

    // Icon convention: Contents/Resources/AppIcon.png. Info.appc's
    // schema doesn't carry an icon field yet — slice 19.E adds one
    // alongside XDG hicolor export.
    snprintf(out->icon_relpath, sizeof out->icon_relpath, "%s", "AppIcon.png");

    // Per-bundle theme override field is reserved for slice 19.F.
    out->host_theme_override_present = false;
    out->host_theme_override_value   = false;

    out->toolkit = b.info.wrap_toolkit;

    mb_bundle_free(&b);
    return true;
}

// ----------------------------------------------------------------------------
// Launcher config
// ----------------------------------------------------------------------------

// Minimal INI reader — single section/key tracker. We don't ship a
// general-purpose parser because moonbase.conf is the only file the
// launcher itself reads, and its grammar is two lines:
//
//     [theme]
//     host_theme_enabled = true
//
// Comments start with # or ;; section headers are bracketed; keys are
// `name = value`. Anything else is silently skipped — a future option
// the launcher doesn't recognize must not break older launchers.
static void launcher_config_load(LauncherConfig *out) {
    out->host_theme_enabled = false;     // pure Snow Leopard Aqua — hard default

    char cfg_home[PATH_MAX];
    if (resolve_xdg_config_home(cfg_home, sizeof cfg_home) != 0) return;

    char path[PATH_MAX];
    int n = snprintf(path, sizeof path, "%s/copycatos/moonbase.conf", cfg_home);
    if (n < 0 || (size_t)n >= sizeof path) return;

    FILE *f = fopen(path, "r");
    if (!f) return;                       // missing file = defaults; not an error

    char line[512];
    char section[64] = "";
    while (fgets(line, sizeof line, f)) {
        strip_inplace(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (!end) continue;
            *end = '\0';
            snprintf(section, sizeof section, "%s", line + 1);
            strip_inplace(section);
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;
        strip_inplace(key); strip_inplace(val);

        if (strcmp(section, "theme") == 0 &&
            strcmp(key, "host_theme_enabled") == 0) {
            bool ok = false;
            bool b = parse_bool_loose(val, &ok);
            if (ok) out->host_theme_enabled = b;
        }
    }
    fclose(f);
}

// Resolve the host-theme decision for THIS bundle. Precedence (most
// specific wins):
//
//   1. Per-app MoonBase Preferences — user's runtime toggle from the
//      app's own View → Use Host Desktop Theme menu item, persisted
//      to ~/.local/share/moonbase/<bundle-id>/Preferences/.
//   2. Bundle manifest override — developer's bundle-level default
//      from Contents/moonbase-manifest.json (rare; mostly for webview
//      apps that should default to host-theme on a fresh install).
//   3. Global config default — ~/.config/copycatos/moonbase.conf
//      [theme] host_theme_enabled. Applies only to apps with no
//      per-app preference yet.
//   4. Hard default: false (pure Aqua).
//
// Same precedence Apple's Info.plist + NSUserDefaults follow when an
// app overrides a system preference.
// Tier 1 reader — opens
// $XDG_DATA_HOME/moonbase/<bundle_id>/Preferences/host_theme_enabled
// and parses its body as a loose bool. The file's *existence* doesn't
// commit a tier — only a parseable value does, so an empty or garbage
// file falls through to the next tier instead of pinning Aqua. The
// View → Use Host Desktop Theme menu item writes "true\n" / "false\n"
// here; nothing else writes to this path.
static bool read_per_app_host_theme(const char *bundle_id, bool *out_set) {
    *out_set = false;
    if (!bundle_id || !*bundle_id) return false;

    char data_home[PATH_MAX];
    if (resolve_xdg_data_home(data_home, sizeof data_home) != 0) return false;

    char path[PATH_MAX];
    int n = snprintf(path, sizeof path,
                     "%s/moonbase/%s/Preferences/host_theme_enabled",
                     data_home, bundle_id);
    if (n < 0 || (size_t)n >= sizeof path) return false;

    FILE *f = fopen(path, "r");
    if (!f) return false;

    char buf[32] = {0};
    size_t got = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (got == 0) return false;
    buf[got] = '\0';
    strip_inplace(buf);

    bool ok = false;
    bool v = parse_bool_loose(buf, &ok);
    if (!ok) return false;
    *out_set = true;
    return v;
}

static bool resolve_host_theme(const LauncherConfig *cfg,
                               const LauncherManifest *m,
                               const char *bundle_id) {
    bool per_app_set = false;
    bool per_app = read_per_app_host_theme(bundle_id, &per_app_set);
    if (per_app_set) return per_app;
    if (m->host_theme_override_present) return m->host_theme_override_value;
    return cfg->host_theme_enabled;
}

// Env-based host DE detection. XDG_CURRENT_DESKTOP is the spec variable
// (colon-separated; first matching token wins); XDG_SESSION_DESKTOP is
// the SDDM/login-tier fallback. KDE / Plasma → Breeze. Anything else →
// Adwaita as a reasonable default for the GTK-family majority (GNOME,
// XFCE, Cinnamon, MATE, Pantheon, unknown). When a user explicitly
// opts into host theming they expect *some* host tint, not a silent
// fall-back to Aqua — that's what host_theme_enabled = false is for.
//
// XSettings (Net/ThemeName) and the freedesktop portal Settings probe
// are a follow-up. Both can refine to the user's actual chosen theme
// (e.g. Breeze-Dark, Adwaita-Dark) once the chrome stub paints; the
// env heuristic is sufficient to pick the right *family*.
static menubar_render_theme_t detect_host_theme_env(void) {
    const char *vars[] = { "XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP" };
    for (size_t i = 0; i < sizeof vars / sizeof vars[0]; i++) {
        const char *v = getenv(vars[i]);
        if (!v || !*v) continue;
        char buf[128];
        snprintf(buf, sizeof buf, "%s", v);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ":", &save); tok;
             tok = strtok_r(NULL, ":", &save)) {
            if (strcasecmp(tok, "KDE")    == 0 ||
                strcasecmp(tok, "Plasma") == 0) {
                return MENUBAR_THEME_HOST_BREEZE_LIGHT;
            }
        }
    }
    return MENUBAR_THEME_HOST_ADWAITA_LIGHT;
}

static menubar_render_theme_t resolve_theme(bool host_theme_on) {
    if (!host_theme_on) return MENUBAR_THEME_AQUA;
    return detect_host_theme_env();
}

// ----------------------------------------------------------------------------
// XDG integration — first-launch .desktop + icon registration
// ----------------------------------------------------------------------------

// True if a `.desktop` entry already exists for this bundle. Stat is
// the cheapest possible probe — we don't need to validate contents,
// just decide whether first-launch registration runs.
static bool xdg_already_registered(const char *bundle_id) {
    char data_home[PATH_MAX];
    if (resolve_xdg_data_home(data_home, sizeof data_home) != 0) return false;
    char path[PATH_MAX];
    int n = snprintf(path, sizeof path,
                     "%s/applications/copycatos-%s.desktop",
                     data_home, bundle_id);
    if (n < 0 || (size_t)n >= sizeof path) return false;
    struct stat st;
    return stat(path, &st) == 0;
}

// Register the bundle with the host DE on first launch. Two writes:
//
//   1. ~/.local/share/applications/copycatos-<bundle_id>.desktop
//      Exec= points at the *absolute* .app path (resolved via realpath
//      once at first-launch time) so a relative argv survives the user
//      dragging the file. The mount path would be wrong here — it's
//      ephemeral (per-pid) and gone before the host DE indexes it.
//   2. ~/.local/share/icons/hicolor/<NxN>/apps/copycatos-<bundle_id>.png
//      Source is <bundle_root>/Contents/Resources/<icon_relpath>. For a
//      single-file .app `bundle_root` is the squashfuse mount path the
//      caller arranges; for a .appdev it's the bundle dir directly.
//      <NxN> comes from reading the PNG's IHDR and rounding the shorter
//      side down to the nearest standard hicolor bucket — so a 512×512
//      AppIcon.png lands in 512x512/apps, a 256 in 256x256/apps. If the
//      file isn't a parseable PNG we fall back to 128x128 (the historical
//      always-on bucket). Missing icon is not an error; the host DE
//      falls back to its generic application glyph.
//
// Best-effort cache refresh runs `update-desktop-database` and
// `gtk-update-icon-cache` so the entry appears immediately. Either can
// be missing; we ignore exit status either way (next session picks
// the entry up regardless via the file's mtime).
static bool xdg_register(const char *bundle_path,
                         const char *bundle_root,
                         const LauncherManifest *m) {
    char real_bundle[PATH_MAX];
    if (!realpath(bundle_path, real_bundle)) {
        fprintf(stderr,
                "[moonbase-launcher] realpath(%s): %s — XDG registration skipped\n",
                bundle_path, strerror(errno));
        return false;
    }

    char data_home[PATH_MAX];
    if (resolve_xdg_data_home(data_home, sizeof data_home) != 0) {
        fprintf(stderr,
                "[moonbase-launcher] $HOME / $XDG_DATA_HOME unset — "
                "XDG registration skipped\n");
        return false;
    }

    char apps_dir[PATH_MAX];
    int n = snprintf(apps_dir, sizeof apps_dir, "%s/applications", data_home);
    if (n < 0 || (size_t)n >= sizeof apps_dir) return false;
    if (mkdir_p(apps_dir, 0755) != 0) {
        fprintf(stderr, "[moonbase-launcher] mkdir %s: %s\n",
                apps_dir, strerror(errno));
        return false;
    }

    char desktop_path[PATH_MAX];
    n = snprintf(desktop_path, sizeof desktop_path,
                 "%s/copycatos-%s.desktop", apps_dir, m->bundle_id);
    if (n < 0 || (size_t)n >= sizeof desktop_path) return false;

    // .desktop body. StartupWMClass matches the bundle-id so KDE/GNOME
    // task bars group windows under the catalog entry instead of the
    // generic "moonbase-launcher" process name. X-CopyCatOS-BundleId is
    // a private hint AuraFarm Code's `moonbase doctor` reads to map a
    // .desktop entry back to the bundle that produced it.
    char body[4096];
    n = snprintf(body, sizeof body,
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Version=1.0\n"
        "Name=%s\n"
        "Exec=moonbase-launcher %s %%U\n"
        "Icon=copycatos-%s\n"
        "Terminal=false\n"
        "Categories=Utility;\n"
        "StartupNotify=true\n"
        "StartupWMClass=%s\n"
        "X-CopyCatOS-BundleId=%s\n",
        m->display_name,
        real_bundle,
        m->bundle_id,
        m->bundle_id,
        m->bundle_id);
    if (n < 0 || (size_t)n >= sizeof body) {
        fprintf(stderr,
                "[moonbase-launcher] .desktop body too large for bundle %s\n",
                m->bundle_id);
        return false;
    }

    if (write_atomic(desktop_path, body) != 0) {
        fprintf(stderr, "[moonbase-launcher] write %s: %s\n",
                desktop_path, strerror(errno));
        return false;
    }

    // Icon copy — best-effort. Read the PNG IHDR and pick the largest
    // standard hicolor bucket ≤ the icon's shorter side; fall back to
    // 128 if the file isn't a parseable PNG.
    char icon_src[PATH_MAX];
    n = snprintf(icon_src, sizeof icon_src,
                 "%s/Contents/Resources/%s",
                 bundle_root, m->icon_relpath);
    if (n > 0 && (size_t)n < sizeof icon_src) {
        struct stat ist;
        if (stat(icon_src, &ist) == 0 && S_ISREG(ist.st_mode)) {
            int bucket = 128;
            int hfd = open(icon_src, O_RDONLY);
            if (hfd >= 0) {
                uint8_t header[24];
                ssize_t hn = read(hfd, header, sizeof header);
                close(hfd);
                if (hn == (ssize_t)sizeof header) {
                    uint32_t iw = 0, ih = 0;
                    if (mb_png_dims_from_buf(header, &iw, &ih) == 0) {
                        uint32_t shorter = (iw < ih) ? iw : ih;
                        bucket = mb_hicolor_bucket_for(shorter);
                    }
                }
            }
            char icon_dir[PATH_MAX];
            int dn = snprintf(icon_dir, sizeof icon_dir,
                              "%s/icons/hicolor/%dx%d/apps",
                              data_home, bucket, bucket);
            if (dn > 0 && (size_t)dn < sizeof icon_dir
                && mkdir_p(icon_dir, 0755) == 0) {
                char icon_dst[PATH_MAX];
                int xn = snprintf(icon_dst, sizeof icon_dst,
                                  "%s/copycatos-%s.png",
                                  icon_dir, m->bundle_id);
                if (xn > 0 && (size_t)xn < sizeof icon_dst) {
                    if (copy_file(icon_src, icon_dst) != 0) {
                        fprintf(stderr,
                                "[moonbase-launcher] copy icon %s -> %s: %s\n",
                                icon_src, icon_dst, strerror(errno));
                    }
                }
            }
        }
        // Missing AppIcon.png is fine — many dev bundles ship without one.
    }

    // Cache refreshers — fire and forget. Pass the apps_dir to
    // update-desktop-database so a per-user run with no system perms
    // still works; gtk-update-icon-cache wants the theme root.
    run_best_effort("update-desktop-database", apps_dir);
    char hicolor_root[PATH_MAX];
    int hn = snprintf(hicolor_root, sizeof hicolor_root,
                      "%s/icons/hicolor", data_home);
    if (hn > 0 && (size_t)hn < sizeof hicolor_root) {
        run_best_effort("gtk-update-icon-cache", hicolor_root);
    }

    return true;
}

// ----------------------------------------------------------------------------
// Delegate to moonbase-launch
// ----------------------------------------------------------------------------

// Replace the current process with `moonbase-launch <bundle> [args...]`.
// Returns only on failure — on success execvp doesn't return.
//
// The forwarding shape: argv[0] is rewritten to "moonbase-launch" so
// the delegate sees its own canonical argv[0] (matters for usage
// strings and error messages); argv[1] is the bundle path; argv[2..]
// are forwarded user args verbatim.
static int exec_moonbase_launch(const char *bundle_path,
                                int extra_argc, char **extra_argv) {
    char **forward = calloc((size_t)extra_argc + 3, sizeof(char *));
    if (!forward) return 127;
    forward[0] = "moonbase-launch";
    forward[1] = (char *)bundle_path;
    for (int i = 0; i < extra_argc; i++) forward[2 + i] = extra_argv[i];
    forward[2 + extra_argc] = NULL;
    execvp("moonbase-launch", forward);
    perror("[moonbase-launcher] execvp moonbase-launch");
    free(forward);
    return 127;
}

// ----------------------------------------------------------------------------
// main
// ----------------------------------------------------------------------------

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s <bundle.app|bundle.appdev> [args...]\n"
        "\n"
        "Launches a CopyCatOS .app bundle on the current X session.\n"
        "Inside a full CopyCatOS session, this is a thin pass-through to\n"
        "moonbase-launch. On foreign distros (KDE, GNOME, XFCE) it forks\n"
        "moonrock-lite to draw the title bar + menu bar above the app's\n"
        "window, and registers an XDG .desktop entry on first launch so\n"
        "the bundle integrates with the host DE.\n",
        argv0);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 2; }

    const char *bundle_path = argv[1];
    int    extra_argc = argc - 2;
    char **extra_argv = argv + 2;

    // Fast path. Full CopyCatOS session — moonrock-session.sh sets
    // XDG_SESSION_DESKTOP=CopyCatOS, the daemon owns chrome already,
    // no XDG glue applies. Hand straight through.
    if (in_full_copycatos_session()) {
        return exec_moonbase_launch(bundle_path, extra_argc, extra_argv);
    }

    // Foreign-distro path from here down.
    LauncherManifest m   = {0};
    LauncherConfig   cfg = {0};
    launcher_config_load(&cfg);

    // Single-file .app vs .appdev directory. mb_appimg_is_single_file
    // returns OK with *out=false for non-trailer paths (a .appdev dir,
    // a stray text file, a missing path) so any non-OK return reduces
    // to "treat as directory" — mb_bundle_load downstream gives the
    // real error message.
    bool single_file = false;
    {
        bool sf = false;
        mb_appimg_err_t r = mb_appimg_is_single_file(bundle_path, &sf);
        if (r == MB_APPIMG_OK) single_file = sf;
    }

    char mount_path[PATH_MAX] = {0};
    mb_appimg_trailer_t trailer = {0};
    if (single_file
        && mount_single_file_app(bundle_path, mount_path,
                                 sizeof mount_path, &trailer) != 0) {
        return 1;
    }

    // bundle_root is the directory mb_bundle_load can stat: the
    // squashfuse mount for a single-file .app, the .appdev directly
    // otherwise. The original argv path stays in `bundle_path` because
    // that's what we hand to moonbase-launch (and what the .desktop
    // entry's Exec= line points at).
    const char *bundle_root = single_file ? mount_path : bundle_path;
    if (!bundle_manifest_load(bundle_root, &m)) {
        if (single_file) {
            unmount_single_file_app(mount_path);
            mb_appimg_trailer_free(&trailer);
        }
        return 1;
    }

    // Cross-check: the trailer's bundle-id must match Info.appc's id.
    // A mismatch means the packer (moonbase-pack, AuraFarm Code) wrote
    // a stale trailer over a different Info.appc — a producer bug, not
    // a user-recoverable state, so we refuse to launch.
    if (single_file && strcmp(trailer.bundle_id, m.bundle_id) != 0) {
        fprintf(stderr,
                "[moonbase-launcher] trailer bundle-id %s mismatches "
                "Info.appc id %s — refusing to launch\n",
                trailer.bundle_id, m.bundle_id);
        unmount_single_file_app(mount_path);
        mb_appimg_trailer_free(&trailer);
        return 1;
    }

    // Lazy first-launch XDG registration. Failure logs but doesn't
    // abort — the .app still launches; the user just won't get a
    // host-DE shortcut on first run (the entry can be added by hand or
    // by re-launching).
    if (!xdg_already_registered(m.bundle_id)) {
        (void)xdg_register(bundle_path, bundle_root, &m);
    }

    bool host_theme_on = resolve_host_theme(&cfg, &m, m.bundle_id);
    menubar_render_theme_t theme = resolve_theme(host_theme_on);

    // Fork the chrome sidecar. Two binaries dispatch by Info.appc toolkit
    // hint (slice 19.H.2.d):
    //
    //   - NATIVE  → moonrock-host. Active X frontend: it creates the
    //     bundle's top-level window itself and binds a per-bundle IPC
    //     socket the bundle's libmoonbase connects to. No --theme: chrome
    //     paints from the shared host_chrome painter directly.
    //
    //   - QT5/QT6/GTK3/GTK4 → moonrock-lite. Passive sidecar that finds
    //     the toolkit-owned X window by WM_CLASS and decorates it from
    //     above. --theme forwards the resolved Aqua/host_breeze/host_adwaita
    //     selection so chrome blends with the host DE if requested.
    //
    // Pre-exec PR_SET_PDEATHSIG covers the race between fork and execvp;
    // both children re-arm it on entry. The sidecar dies when the bundle
    // process (our PID after execvp moonbase-launch) exits.
    bool native = (m.toolkit == MB_INFO_APPC_WRAP_NATIVE);
    pid_t stub = fork();
    if (stub < 0) {
        perror(native ? "[moonbase-launcher] fork moonrock-host"
                      : "[moonbase-launcher] fork moonrock-lite");
        if (single_file) {
            unmount_single_file_app(mount_path);
            mb_appimg_trailer_free(&trailer);
        }
        return 1;
    }
    if (stub == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (native) {
            char *child_argv[] = {
                "moonrock-host",
                "--bundle-id",    (char *)m.bundle_id,
                "--display-name", (char *)m.display_name,
                NULL
            };
            execvp("moonrock-host", child_argv);
            perror("[moonbase-launcher] execvp moonrock-host");
            _exit(127);
        }
        char theme_buf[16];
        snprintf(theme_buf, sizeof theme_buf, "%d", (int)theme);
        char *child_argv[] = {
            "moonrock-lite",
            "--bundle-id",    (char *)m.bundle_id,
            "--display-name", (char *)m.display_name,
            "--theme",        theme_buf,
            NULL
        };
        execvp("moonrock-lite", child_argv);
        perror("[moonbase-launcher] execvp moonrock-lite");
        _exit(127);
    }

    // For NATIVE bundles, wait briefly for moonrock-host to bind the
    // per-bundle IPC socket before we let moonbase-launch exec the
    // bundle binary. mb_ipc_frame_connect fails fast (no retry) — if
    // libmoonbase calls connect() before host has bound, the bundle
    // exits at startup. host binds before XOpenDisplay, so the gap
    // is small in practice; cap the wait so a host crash on launch
    // doesn't hang the whole pipeline. We proceed past the timeout
    // either way: if the socket truly never appears, the bundle's
    // own clean exit-on-connect-failure surfaces the error.
    if (native) {
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        if (xdg && *xdg) {
            char sock_path[768];
            int n = snprintf(sock_path, sizeof sock_path,
                             "%s/moonbase/moonbase-%s.sock",
                             xdg, m.bundle_id);
            if (n > 0 && (size_t)n < sizeof sock_path) {
                // Poll ~10 ms × 100 = 1 s max. Stat is cheap; fewer is
                // better but the launcher is a one-shot dispatcher,
                // not a hot path.
                for (int i = 0; i < 100; i++) {
                    struct stat st;
                    if (stat(sock_path, &st) == 0 && S_ISSOCK(st.st_mode)) break;
                    usleep(10 * 1000);
                }
            }
        }
    }

    // Unmount BEFORE exec — atexit doesn't fire across execvp, so
    // anything the launcher held on the FUSE mount must be released
    // here or it leaks. moonbase-launch will mount the same .app
    // independently for the bundle's lifetime; the brief double-mount
    // is the cost of a correct cleanup story.
    if (single_file) {
        unmount_single_file_app(mount_path);
        mb_appimg_trailer_free(&trailer);
    }

    return exec_moonbase_launch(bundle_path, extra_argc, extra_argv);
}
