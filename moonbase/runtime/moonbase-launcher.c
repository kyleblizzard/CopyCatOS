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

#include "bundle/bundle.h"

#include <errno.h>
#include <fcntl.h>
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
// Bundle manifest — minimal launcher view over mb_bundle_load
// ----------------------------------------------------------------------------

// Read the manifest fields the launcher needs (bundle id, display name,
// icon path) by running the canonical bundle-spec.md §8 validator from
// libmoonbase. moonbase-launch will run the same validator a second
// time when we exec it — that's deliberate. The launcher must not exec
// against a malformed bundle (we'd register a broken .desktop entry,
// then fail past the point where we could surface the error nicely).
//
// .appdev directories load directly. Single-file .app images need a
// squashfuse mount before Info.appc is reachable; that mount lives
// inside moonbase-launch, and teaching the launcher to peek through the
// .app trailer is slice 19.E. Until then mb_bundle_load returns
// MB_BUNDLE_ERR_NOT_DIR for single-file .app and we surface a clear
// "use the .appdev directory for now" hint.
static bool bundle_manifest_load(const char *bundle_root,
                                 LauncherManifest *out) {
    mb_bundle_t b = {0};
    char err[256] = {0};
    mb_bundle_err_t rc = mb_bundle_load(bundle_root, &b, err, sizeof err);
    if (rc != MB_BUNDLE_OK) {
        fprintf(stderr,
                "[moonbase-launcher] %s: %s\n",
                mb_bundle_err_string(rc), err);
        if (rc == MB_BUNDLE_ERR_NOT_DIR) {
            fprintf(stderr,
                    "[moonbase-launcher] hint: single-file .app on a "
                    "foreign distro is not yet supported (slice 19.E). "
                    "Use the .appdev directory for now.\n");
        }
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

static bool xdg_already_registered(const char *bundle_id) {
    (void)bundle_id;
    // TODO(19.E): stat ~/.local/share/applications/copycatos-<bundle_id>.desktop.
    return false;
}

static bool xdg_register(const char *bundle_path, const LauncherManifest *m) {
    (void)bundle_path; (void)m;
    // TODO(19.E):
    //   1. Write ~/.local/share/applications/copycatos-<bundle_id>.desktop
    //      with Exec=moonbase-launcher <absolute bundle_path>, Name=display_name,
    //      Icon=copycatos-<bundle_id>, Categories suitable per manifest.
    //   2. Copy <bundle_path>/Contents/Resources/<icon_relpath> into
    //      ~/.local/share/icons/hicolor/<size>/apps/copycatos-<bundle_id>.<ext>.
    //   3. Best-effort run update-desktop-database / gtk-update-icon-cache so
    //      the entry shows up immediately in the host DE menu.
    return false;
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

    // Foreign-distro path only from here down.
    LauncherManifest m   = {0};
    LauncherConfig   cfg = {0};
    // TODO(19.F): launcher_config_load must also create the config dir if missing.
    launcher_config_load(&cfg);
    // bundle_manifest_load already printed a specific diagnostic
    // (and a single-file-.app hint when applicable) on failure.
    if (!bundle_manifest_load(bundle_path, &m)) return 1;

    // Lazy first-launch XDG registration so the bundle becomes a real
    // app on the host DE (Activities, application menu, taskbar) on
    // the very first double-click. Subsequent launches no-op.
    if (!xdg_already_registered(m.bundle_id)) xdg_register(bundle_path, &m);

    bool host_theme_on = resolve_host_theme(&cfg, &m, m.bundle_id);
    menubar_render_theme_t theme = resolve_theme(host_theme_on);

    // Fork the chrome stub. The parent execs into moonbase-launch
    // (which execvps bwrap which execvps the bundle binary at
    // Contents/CopyCatOS/<name>), so the stub's parent-death signal
    // fires when the bundle exits — clean teardown, no zombie chrome.
    pid_t stub = fork();
    if (stub < 0) { perror("[moonbase-launcher] fork chrome stub"); return 1; }
    if (stub == 0) {
        _exit(run_chrome_stub(bundle_path, &m, theme));
    }

    return exec_moonbase_launch(bundle_path, extra_argc, extra_argv);
}
