// CopyCatOS — by Kyle Blizzard at Blizzard.show

// consents.c — reader for consents.toml.
//
// Resolves the per-bundle path, slurps the file (64 KiB cap), and
// scans for `[<group>.<value>]` followed by a `decision = "allow"` /
// `"deny"` line. Anything the scanner doesn't understand — comments,
// unknown keys, odd whitespace — is skipped instead of rejected, so
// future schema additions (scope, granted, per-field updates) don't
// trip this parser on older installs.
//
// No cache: a pending IPC consent grant may have just rewritten the
// file, and a stale cached "missing" would regress the caller.

#include "consents.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Hard cap so a pathological file can't pin the process on read.
// 64 KiB is two orders of magnitude above any realistic consents file
// (dozens of capabilities, each a handful of short fields).
#define MB_CONSENTS_MAX_BYTES (64 * 1024)

// ---------------------------------------------------------------------
// Path resolution
// ---------------------------------------------------------------------

// Join components into a freshly malloc'd absolute path. Returns NULL
// on allocation failure. Callers free with plain free().
static char *join3(const char *a, const char *b, const char *c,
                   const char *d, const char *e) {
    size_t len = strlen(a) + strlen(b) + strlen(c) + strlen(d) + strlen(e) + 1;
    char *out = malloc(len);
    if (!out) return NULL;
    snprintf(out, len, "%s%s%s%s%s", a, b, c, d, e);
    return out;
}

// Build the consents.toml path for the caller's bundle:
//   $XDG_DATA_HOME/moonbase/<id>/Preferences/consents.toml
//   or $HOME/.local/share/moonbase/<id>/Preferences/consents.toml
// Returns NULL if the bundle id can't be resolved — no path, no query,
// caller gets MB_CONSENT_MISSING.
static char *consents_path_for_self(void) {
    const char *bid = getenv("MOONBASE_BUNDLE_ID");
    if (!bid || !bid[0]) return NULL;

    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) {
        return join3(xdg, "/moonbase/", bid, "/Preferences/", "consents.toml");
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) return NULL;
    return join3(home, "/.local/share/moonbase/", bid,
                 "/Preferences/", "consents.toml");
}

// ---------------------------------------------------------------------
// File slurp
// ---------------------------------------------------------------------

// Read the whole file into a freshly malloc'd NUL-terminated buffer.
// Returns NULL if the file doesn't exist, is too large, or any I/O
// error fires — a missing or unreadable file is just "no consent yet",
// not an error the caller surfaces.
static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    struct stat st;
    if (fstat(fileno(f), &st) != 0) { fclose(f); return NULL; }
    if (!S_ISREG(st.st_mode)) { fclose(f); return NULL; }
    if (st.st_size < 0 || (size_t)st.st_size > MB_CONSENTS_MAX_BYTES) {
        fclose(f);
        return NULL;
    }

    size_t n = (size_t)st.st_size;
    char *buf = malloc(n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    if (got != n) { free(buf); return NULL; }
    buf[n] = '\0';
    return buf;
}

// ---------------------------------------------------------------------
// Scanner
// ---------------------------------------------------------------------

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

// Copy a line into `dst` (capacity `cap`, NUL-terminated) and return
// the pointer to the next line. Truncates silently if the line is
// longer than `dst` — the fields we care about are all short.
static const char *copy_line(const char *p, char *dst, size_t cap) {
    size_t i = 0;
    while (*p && *p != '\n' && *p != '\r') {
        if (i + 1 < cap) dst[i++] = *p;
        p++;
    }
    if (cap > 0) dst[i] = '\0';
    if (*p == '\r') p++;
    if (*p == '\n') p++;
    return p;
}

// Strip leading / trailing whitespace in-place; return dst.
static char *trim(char *s) {
    char *a = s;
    while (*a == ' ' || *a == '\t') a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = '\0';
    return s;
}

// Is `line` exactly `[name]` (after trimming)?
static bool line_is_section(const char *line, const char *name) {
    const char *p = skip_ws(line);
    if (*p != '[') return false;
    p++;
    size_t nlen = strlen(name);
    if (strncmp(p, name, nlen) != 0) return false;
    p += nlen;
    if (*p != ']') return false;
    p++;
    p = skip_ws(p);
    return *p == '\0' || *p == '#';
}

// Does `line` look like a section header (`[...]`)? Any section, not
// a specific name — used to know when the current section ends.
static bool line_is_any_section(const char *line) {
    const char *p = skip_ws(line);
    return *p == '[';
}

// Parse `decision = "allow"` / `"deny"`; return MB_CONSENT_* or
// MB_CONSENT_MISSING if the line isn't a recognized decision.
static mb_consent_status_t parse_decision_line(const char *line) {
    const char *p = skip_ws(line);
    if (strncmp(p, "decision", 8) != 0) return MB_CONSENT_MISSING;
    p += 8;
    p = skip_ws(p);
    if (*p != '=') return MB_CONSENT_MISSING;
    p++;
    p = skip_ws(p);
    if (*p != '"') return MB_CONSENT_MISSING;
    p++;
    // Copy up to the closing quote.
    char val[32] = {0};
    size_t i = 0;
    while (*p && *p != '"' && *p != '\n' && i + 1 < sizeof(val)) {
        val[i++] = *p++;
    }
    val[i] = '\0';
    if (strcmp(val, "allow") == 0) return MB_CONSENT_ALLOW;
    if (strcmp(val, "deny")  == 0) return MB_CONSENT_DENY;
    return MB_CONSENT_MISSING;
}

// Search `buf` for `[section]`; on hit, scan forward for a decision
// line until the next section header or EOF. Comments (`#…`) and
// unknown keys are skipped.
static mb_consent_status_t scan_for_decision(const char *buf,
                                             const char *section) {
    const char *p = buf;
    bool in_section = false;
    while (*p) {
        char line[256];
        const char *next = copy_line(p, line, sizeof(line));
        p = next;

        char *t = trim(line);
        if (t[0] == '\0' || t[0] == '#') continue;

        if (line_is_any_section(t)) {
            in_section = line_is_section(t, section);
            continue;
        }
        if (!in_section) continue;

        mb_consent_status_t s = parse_decision_line(t);
        if (s != MB_CONSENT_MISSING) return s;
        // Unknown key inside the target section — keep scanning, the
        // decision line may come later.
    }
    return MB_CONSENT_MISSING;
}

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------

mb_consent_status_t mb_consent_query(const char *group, const char *value) {
    if (!group || !group[0] || !value || !value[0]) return MB_CONSENT_MISSING;

    char *path = consents_path_for_self();
    if (!path) return MB_CONSENT_MISSING;

    char *buf = slurp(path);
    free(path);
    if (!buf) return MB_CONSENT_MISSING;

    // Section name is "<group>.<value>".
    size_t sn = strlen(group) + 1 + strlen(value) + 1;
    char *section = malloc(sn);
    if (!section) { free(buf); return MB_CONSENT_MISSING; }
    snprintf(section, sn, "%s.%s", group, value);

    mb_consent_status_t s = scan_for_decision(buf, section);
    free(section);
    free(buf);
    return s;
}

// One-time warning so CI logs make the grant-by-default state
// explicit. Uses a simple guard under a mutex — this path fires at
// most once per process; contention is a non-concern.
static pthread_mutex_t warn_mu = PTHREAD_MUTEX_INITIALIZER;
static bool warned = false;

bool mb_consent_gate_allows(const char *group, const char *value) {
    mb_consent_status_t s = mb_consent_query(group, value);
    if (s == MB_CONSENT_ALLOW) return true;
    if (s == MB_CONSENT_DENY)  return false;

    // MISSING: grant-by-default until MB_IPC_CONSENT_REQUEST lands.
    pthread_mutex_lock(&warn_mu);
    bool first = !warned;
    warned = true;
    pthread_mutex_unlock(&warn_mu);
    if (first) {
        fprintf(stderr,
            "moonbase: consent missing for %s:%s — granting by default "
            "(IPC consent wire pending)\n",
            group, value);
    }
    return true;
}
