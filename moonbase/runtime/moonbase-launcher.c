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
//      Fork a minimal chrome stub that draws the title bar + menu bar
//      *inside the app's own window* using shared menubar_render code,
//      register an XDG .desktop entry on first launch so the bundle
//      shows up like any native app, then execvp moonbase-launch.
//
// Bundle parsing, sandbox tier selection, entitlement bindings, network
// namespace decisions, and the final execvp into Contents/CopyCatOS/<bin>
// all live in moonbase-launch (../launcher/moonbase-launch.c). Two
// binaries, one each for portability and for sandboxing — neither
// duplicates the other.

#include "menubar_render.h"

#include "bundle/appimg.h"
#include "bundle/bundle.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

    mb_bundle_free(&b);
    return true;
}

// ----------------------------------------------------------------------------
// Launcher config
// ----------------------------------------------------------------------------

static void launcher_config_load(LauncherConfig *out) {
    out->host_theme_enabled = false;     // pure Snow Leopard Aqua — hard default
    // TODO(19.F): open ~/.config/copycatos/moonbase.conf, read
    // [theme] host_theme_enabled = true|false. Missing file = defaults.
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
static bool resolve_host_theme(const LauncherConfig *cfg,
                               const LauncherManifest *m,
                               const char *bundle_id) {
    (void)bundle_id;
    // TODO(19.F): read ~/.local/share/moonbase/<bundle_id>/Preferences/
    //             host_theme_enabled (set by the View menu item in the
    //             running app). If present, return that value.
    if (m->host_theme_override_present) return m->host_theme_override_value;
    return cfg->host_theme_enabled;
}

// When host_theme_enabled is on, query the host DE (KDE Breeze, GNOME
// Adwaita, Xfce) via XSettings (Net/ThemeName) and the freedesktop
// portal Settings interface, map to a menubar_render_theme_t, and
// return it. Pure Aqua otherwise.
static menubar_render_theme_t resolve_theme(bool host_theme_on) {
    if (!host_theme_on) return MENUBAR_THEME_AQUA;
    // TODO(19.F): XSettings + portal Settings probe → Breeze / Adwaita.
    return MENUBAR_THEME_AQUA;
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
//   2. ~/.local/share/icons/hicolor/128x128/apps/copycatos-<bundle_id>.png
//      Source is <bundle_root>/Contents/Resources/<icon_relpath>. For a
//      single-file .app `bundle_root` is the squashfuse mount path the
//      caller arranges; for a .appdev it's the bundle dir directly.
//      v1 always installs at 128x128 — a future slice can read PNG IHDR
//      to land in the correct hicolor size dir. Missing icon is not an
//      error; the host DE falls back to its generic application glyph.
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

    // Icon copy — best-effort. v1 lands at 128x128/apps regardless of
    // the source PNG's actual dimensions; a future slice can read IHDR
    // and pick the correct hicolor bucket.
    char icon_src[PATH_MAX];
    n = snprintf(icon_src, sizeof icon_src,
                 "%s/Contents/Resources/%s",
                 bundle_root, m->icon_relpath);
    if (n > 0 && (size_t)n < sizeof icon_src) {
        struct stat ist;
        if (stat(icon_src, &ist) == 0 && S_ISREG(ist.st_mode)) {
            char icon_dir[PATH_MAX];
            int dn = snprintf(icon_dir, sizeof icon_dir,
                              "%s/icons/hicolor/128x128/apps", data_home);
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
// Foreign-distro chrome stub
// ----------------------------------------------------------------------------

// Spawn a minimal MoonRock-equivalent that owns chrome for this bundle's
// window only. We are *not* the WM — the host WM still owns placement,
// focus, virtual desktops, and the rest of the screen. We just draw the
// title bar + menu bar at the top of the app's own X window.
//
// Geometry contract (in points; multiply by host output's effective
// scale to get pixels at paint time):
//
//   title_bar_h    = menubar_render_title_bar_height_pts()  // 22
//   menu_bar_h     = menubar_render_menu_bar_height_pts()   // 22
//   chrome_h       = title_bar_h + menu_bar_h               // 44
//   content_origin = (0, chrome_h)
//
//   Title bar lays out left-to-right: 3 traffic lights at the canonical
//   12 / 32 / 52 SL positions, then a small gap, then the menu items
//   via menubar_render_layout_menus(items, n, origin_x = 76, scale).
//   The window title centers in any remaining whitespace to the right.
//   The menu bar row sits directly below, full-width, with the same
//   menu items — Snow Leopard's two-row chrome.
//
// Teardown: the stub installs PR_SET_PDEATHSIG so it gets SIGTERM the
// instant its parent (which becomes moonbase-launch → bwrap → bundle
// binary after the parent's execvp) exits. No orphaned chrome windows.
static int run_chrome_stub(const char *bundle_path,
                           const LauncherManifest *m,
                           menubar_render_theme_t theme) {
    (void)bundle_path; (void)m; (void)theme;
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    // TODO(19.D):
    //   1. XOpenDisplay; create a top-level chrome window above the
    //      app's content surface (or, more likely, a full window the
    //      app draws into starting at content_origin — exact handoff
    //      to be decided when 19.D lands).
    //   2. Cairo XlibSurface + main event loop (Expose, ConfigureNotify,
    //      ButtonPress / Motion for menu hover and click).
    //   3. Paint via menubar_render_paint_title_bar +
    //      menubar_render_paint_menu_bar with the resolved theme.
    //   4. Route pointer/keyboard events to the bundle process via the
    //      MoonBase IPC contract that the full daemon already speaks.
    //   5. Set up two-tier bwrap sandbox? — NO. Sandbox setup lives in
    //      moonbase-launch. The stub is unsandboxed because it owns X
    //      chrome on the host. Anything privileged stays in the bundle
    //      child, which moonbase-launch wraps in bwrap.
    return 0;
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
        "moonbase-launch. On foreign distros (KDE, GNOME, XFCE) it also\n"
        "draws the title bar and menu bar inside the app's window using\n"
        "the shared chrome render code, and registers an XDG .desktop\n"
        "entry on first launch so the bundle integrates with the host DE.\n",
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
    // TODO(19.F): launcher_config_load must also create the config dir if missing.
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

    // Fork the chrome stub. The stub copies what it needs out of
    // LauncherManifest before we unmount/exec, and PR_SET_PDEATHSIG
    // (set inside the stub) tears it down when the bundle exits.
    pid_t stub = fork();
    if (stub < 0) {
        perror("[moonbase-launcher] fork chrome stub");
        if (single_file) {
            unmount_single_file_app(mount_path);
            mb_appimg_trailer_free(&trailer);
        }
        return 1;
    }
    if (stub == 0) {
        _exit(run_chrome_stub(bundle_path, &m, theme));
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
