// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// shellconf.c — Trivial key=value parser/writer for shell.conf
// ============================================================================
//
// Intentionally small: no multi-section INI, no escaping, no comments-with-
// quotes. Just "key = value" per line; whitespace around the equals is
// trimmed. Unknown keys are ignored. A missing file is not an error — we
// fall back to the defaults the caller seeded.
// ============================================================================

#include "shellconf.h"

#include <X11/Xatom.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ============================================================================
//  File path helpers
// ============================================================================

// Compose the full path to shell.conf. Returns 0 on success, -1 on error.
// The caller supplies a buffer large enough for the expanded path (PATH_MAX
// is generous — we stay well under).
static int shellconf_path(char *out, size_t out_size)
{
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;

    int n = snprintf(out, out_size,
                     "%s/.config/copycatos/shell.conf", home);
    return (n > 0 && (size_t)n < out_size) ? 0 : -1;
}

// Ensure ~/.config/copycatos/ exists. We don't bother creating ~/.config/
// — every modern desktop login session gets that from systemd or the
// distro defaults.
static void ensure_config_dir(void)
{
    const char *home = getenv("HOME");
    if (!home) return;

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config", home);
    mkdir(dir, 0755);   // ignore EEXIST
    snprintf(dir, sizeof(dir), "%s/.config/copycatos", home);
    mkdir(dir, 0755);
}

// ============================================================================
//  Tiny key=value parser
// ============================================================================

// Skip leading whitespace in-place; returns the advanced pointer.
static char *ltrim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

// Trim trailing whitespace + newlines in-place.
static void rtrim(char *s)
{
    size_t len = strlen(s);
    while (len > 0 &&
           (s[len - 1] == ' '  || s[len - 1] == '\t' ||
            s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

// Parse "true"/"false" (case-insensitive); returns *out unchanged on any
// other input so the caller keeps the default.
static void parse_bool(const char *val, bool *out)
{
    if (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0 ||
        strcasecmp(val, "yes")  == 0 || strcasecmp(val, "on") == 0) {
        *out = true;
    } else if (strcasecmp(val, "false") == 0 || strcmp(val, "0") == 0 ||
               strcasecmp(val, "no")    == 0 || strcasecmp(val, "off") == 0) {
        *out = false;
    }
}

// ============================================================================
//  Public API
// ============================================================================

void shellconf_load(ShellConf *conf)
{
    // Seed defaults — both toggles ON per task #69.
    conf->displays_separate_menu_bars = true;
    conf->displays_separate_spaces    = true;

    char path[512];
    if (shellconf_path(path, sizeof(path)) != 0) return;

    FILE *f = fopen(path, "r");
    if (!f) return;   // first-run is fine, defaults stand

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        // Strip comments + whitespace
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        rtrim(line);
        char *p = ltrim(line);
        if (*p == '\0') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = ltrim(eq + 1);
        rtrim(key);

        if (strcmp(key, "displays_separate_menu_bars") == 0) {
            parse_bool(val, &conf->displays_separate_menu_bars);
        } else if (strcmp(key, "displays_separate_spaces") == 0) {
            parse_bool(val, &conf->displays_separate_spaces);
        }
    }
    fclose(f);
}

void shellconf_save(const ShellConf *conf)
{
    ensure_config_dir();

    char path[512];
    if (shellconf_path(path, sizeof(path)) != 0) return;

    // Atomic write: temp file next to the target, rename over it.
    char tmp[528];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "[systemcontrol] shellconf_save: open %s: %s\n",
                tmp, strerror(errno));
        return;
    }
    fprintf(f,
        "# CopyCatOS shell configuration — managed by systemcontrol\n"
        "displays_separate_menu_bars = %s\n"
        "displays_separate_spaces = %s\n",
        conf->displays_separate_menu_bars ? "true" : "false",
        conf->displays_separate_spaces    ? "true" : "false");
    fclose(f);

    if (rename(tmp, path) != 0) {
        fprintf(stderr, "[systemcontrol] shellconf_save: rename %s → %s: %s\n",
                tmp, path, strerror(errno));
        unlink(tmp);
    }
}

// Write one XA_STRING format-8 atom with the exact byte payload (no NUL,
// no newline) so readers doing a byte-exact memcmp see the intended value.
static void write_string_atom(Display *dpy, Window root, const char *atom_name,
                              const char *payload, int nbytes)
{
    Atom atom = XInternAtom(dpy, atom_name, False);
    if (atom == None) return;
    XChangeProperty(dpy, root, atom, XA_STRING, 8, PropModeReplace,
                    (const unsigned char *)payload, nbytes);
}

void shellconf_publish_atoms(Display *dpy, Window root, const ShellConf *conf)
{
    // Menu-bar mode: "modern" (6 bytes) or "classic" (7 bytes).
    if (conf->displays_separate_menu_bars) {
        write_string_atom(dpy, root, COPYCATOS_MENUBAR_MODE_ATOM_NAME,
                          "modern", 6);
    } else {
        write_string_atom(dpy, root, COPYCATOS_MENUBAR_MODE_ATOM_NAME,
                          "classic", 7);
    }

    // Spaces mode: "per_display" (11 bytes) or "global" (6 bytes).
    if (conf->displays_separate_spaces) {
        write_string_atom(dpy, root, COPYCATOS_SPACES_MODE_ATOM_NAME,
                          "per_display", 11);
    } else {
        write_string_atom(dpy, root, COPYCATOS_SPACES_MODE_ATOM_NAME,
                          "global", 6);
    }

    XFlush(dpy);
}
