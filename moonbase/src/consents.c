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
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// Hard cap so a pathological file can't pin the process on read.
// 64 KiB is two orders of magnitude above any realistic consents file
// (dozens of capabilities, each a handful of short fields).
#define MB_CONSENTS_MAX_BYTES (64 * 1024)

// Plenty for any sane $XDG_DATA_HOME/moonbase/<bundle-id>/Preferences
// path plus a ".tmp-<pid>" suffix. Stays off PATH_MAX so Linux's
// 4096-byte PATH_MAX doesn't drag a 4 KiB buffer into every function
// that touches a consent path.
#define PATH_MAX_SAFE 1024

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

// First-time warning per (group, value) so CI logs make the
// grant-by-default state explicit without spamming on repeated calls.
// One global bool would silence warnings for any app that touches
// several missing capabilities; a tiny linked list keyed by
// "group:value" keeps each capability visible exactly once per
// process. This path is cold (fires at most once per capability per
// process) — a linked list is plenty.
typedef struct warn_node {
    struct warn_node *next;
    char key[64];  // "group:value" — schema keeps both short
} warn_node_t;

static pthread_mutex_t warn_mu = PTHREAD_MUTEX_INITIALIZER;
static warn_node_t *warn_head = NULL;

// Returns true if this is the first time we've seen (group, value)
// in this process. Caller holds no locks.
static bool warn_first_time(const char *group, const char *value) {
    char key[64];
    snprintf(key, sizeof(key), "%s:%s", group, value);

    pthread_mutex_lock(&warn_mu);
    for (warn_node_t *n = warn_head; n; n = n->next) {
        if (strcmp(n->key, key) == 0) {
            pthread_mutex_unlock(&warn_mu);
            return false;
        }
    }
    warn_node_t *node = malloc(sizeof(*node));
    if (!node) {
        // Allocation failure: treat as "already warned" so we don't
        // spam on every call — the gate still grants, the caller
        // just misses the log line.
        pthread_mutex_unlock(&warn_mu);
        return false;
    }
    snprintf(node->key, sizeof(node->key), "%s", key);
    node->next = warn_head;
    warn_head = node;
    pthread_mutex_unlock(&warn_mu);
    return true;
}

bool mb_consent_gate_allows(const char *group, const char *value) {
    mb_consent_status_t s = mb_consent_query(group, value);
    if (s == MB_CONSENT_ALLOW) return true;
    if (s == MB_CONSENT_DENY)  return false;

    // MISSING: grant-by-default until MB_IPC_CONSENT_REQUEST lands.
    if (group && value && warn_first_time(group, value)) {
        fprintf(stderr,
            "moonbase: consent missing for %s:%s — granting by default "
            "(IPC consent wire pending)\n",
            group, value);
    }
    return true;
}

// ---------------------------------------------------------------------
// Writer — mb_consent_record
// ---------------------------------------------------------------------
//
// Symmetric to the reader: writes a `[<group>.<value>]` section to
// the same file the reader reads. Parses the existing file (if any)
// into a small in-memory model — preamble (everything before the
// first header) plus an ordered list of sections — mutates the
// target section in place or appends it if missing, then serializes
// back out. Avoids text splicing so a pathological section (odd
// whitespace, comments, trailing blanks) can't shift unrelated
// sections on rewrite.
//
// Write is atomic: content lands in a sibling tmp file, is fsynced,
// then rename()d over consents.toml. A crash between open and
// rename leaves the original untouched.

typedef struct {
    char *name;   // "system.keychain" — owned, no brackets
    char *body;   // lines between this header (exclusive) and next header
                  // or EOF — owned, may be empty
} cs_section_t;

typedef struct {
    char         *preamble;      // owned, content before the first section
    cs_section_t *sections;
    size_t        n;
    size_t        cap;
} cs_doc_t;

static void cs_doc_free(cs_doc_t *doc) {
    if (!doc) return;
    free(doc->preamble);
    for (size_t i = 0; i < doc->n; i++) {
        free(doc->sections[i].name);
        free(doc->sections[i].body);
    }
    free(doc->sections);
    doc->preamble = NULL;
    doc->sections = NULL;
    doc->n = doc->cap = 0;
}

// Append `src` (length `len`) to `*dst` (grown with realloc), keeping
// `*dst` NUL-terminated. Returns 0 on success, -1 on alloc failure.
static int str_append(char **dst, size_t *dst_len, const char *src, size_t len) {
    size_t cur = *dst_len;
    char *grown = realloc(*dst, cur + len + 1);
    if (!grown) return -1;
    memcpy(grown + cur, src, len);
    grown[cur + len] = '\0';
    *dst = grown;
    *dst_len = cur + len;
    return 0;
}

// If `line` is a section header `[name]` (after trimming trailing ws
// and comments), return a freshly malloc'd copy of `name`; else NULL.
static char *header_name_dup(const char *line) {
    const char *p = skip_ws(line);
    if (*p != '[') return NULL;
    p++;
    const char *start = p;
    while (*p && *p != ']' && *p != '\n' && *p != '\r') p++;
    if (*p != ']') return NULL;
    size_t len = (size_t)(p - start);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, start, len);
    out[len] = '\0';
    // Trim trailing whitespace inside the brackets just in case.
    while (len > 0 && (out[len-1] == ' ' || out[len-1] == '\t')) {
        out[--len] = '\0';
    }
    return out;
}

// Parse `buf` (NUL-terminated, possibly NULL or empty) into `doc`.
// Returns 0 on success, negative mb_error_t on failure.
static int cs_parse(const char *buf, cs_doc_t *doc) {
    memset(doc, 0, sizeof(*doc));

    if (!buf) {
        doc->preamble = calloc(1, 1);
        return doc->preamble ? 0 : MB_ENOMEM;
    }

    size_t pre_len = 0;
    doc->preamble = calloc(1, 1);
    if (!doc->preamble) return MB_ENOMEM;

    const char *p = buf;
    cs_section_t *cur = NULL;
    size_t cur_body_len = 0;

    while (*p) {
        // Find the end of this line (including trailing newline).
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        size_t line_len = (size_t)(p - line_start);

        // Peek at the line (stripped of newline) to decide role.
        char peek[256];
        size_t peek_len = line_len;
        if (peek_len >= sizeof(peek)) peek_len = sizeof(peek) - 1;
        // Strip trailing \r\n for header detection only; we still
        // write the original bytes verbatim.
        memcpy(peek, line_start, peek_len);
        peek[peek_len] = '\0';
        while (peek_len > 0 &&
               (peek[peek_len-1] == '\n' || peek[peek_len-1] == '\r')) {
            peek[--peek_len] = '\0';
        }

        char *hname = header_name_dup(peek);
        if (hname) {
            // Start a new section.
            if (doc->n == doc->cap) {
                size_t ncap = doc->cap ? doc->cap * 2 : 4;
                cs_section_t *ns = realloc(doc->sections,
                                           ncap * sizeof(*ns));
                if (!ns) { free(hname); return MB_ENOMEM; }
                doc->sections = ns;
                doc->cap = ncap;
            }
            cur = &doc->sections[doc->n++];
            cur->name = hname;
            cur->body = calloc(1, 1);
            if (!cur->body) return MB_ENOMEM;
            cur_body_len = 0;
            continue;
        }

        if (!cur) {
            if (str_append(&doc->preamble, &pre_len,
                           line_start, line_len) != 0) return MB_ENOMEM;
        } else {
            if (str_append(&cur->body, &cur_body_len,
                           line_start, line_len) != 0) return MB_ENOMEM;
        }
    }
    return 0;
}

// Format the body of a target section: `decision = "..."\ngranted = ...\n`.
// Writes into `out` (cap bytes) and returns the number of bytes
// written (excluding the terminating NUL), or -1 on format error.
static int format_section_body(char *out, size_t cap,
                               mb_consent_status_t decision) {
    const char *word = (decision == MB_CONSENT_ALLOW) ? "allow" : "deny";

    // RFC 3339 UTC timestamp. tm fields are zero-based month, so +1.
    char ts[32];
    time_t now = time(NULL);
    struct tm tm_utc;
    if (!gmtime_r(&now, &tm_utc)) {
        snprintf(ts, sizeof(ts), "1970-01-01T00:00:00Z");
    } else {
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    }

    int w = snprintf(out, cap,
                     "decision = \"%s\"\n"
                     "granted  = %s\n",
                     word, ts);
    if (w < 0 || (size_t)w >= cap) return -1;
    return w;
}

// Atomic write: write to <path>.tmp-<pid>, fsync, rename over path.
// Returns 0 on success, MB_EIPC on any I/O failure.
static int atomic_write(const char *path, const char *data, size_t len) {
    char tmp[PATH_MAX_SAFE];
    int w = snprintf(tmp, sizeof(tmp), "%s.tmp-%d", path, (int)getpid());
    if (w < 0 || (size_t)w >= sizeof(tmp)) return MB_EIPC;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return MB_EIPC;

    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp);
            return MB_EIPC;
        }
        off += (size_t)n;
    }
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp);
        return MB_EIPC;
    }
    if (close(fd) != 0) {
        unlink(tmp);
        return MB_EIPC;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return MB_EIPC;
    }
    return 0;
}

// Ensure every directory on `path`'s parent chain exists with mode
// 0700. Returns 0 on success, MB_EIPC on failure. Tolerates EEXIST.
static int mkdir_p_0700(const char *path) {
    char buf[PATH_MAX_SAFE];
    int w = snprintf(buf, sizeof(buf), "%s", path);
    if (w < 0 || (size_t)w >= sizeof(buf)) return MB_EIPC;

    // Walk up to but not including the final component (the file).
    char *slash = strrchr(buf, '/');
    if (!slash) return 0;  // no directory component
    *slash = '\0';

    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0700) != 0 && errno != EEXIST) return MB_EIPC;
            *p = '/';
        }
    }
    if (mkdir(buf, 0700) != 0 && errno != EEXIST) return MB_EIPC;
    // Tighten the final Preferences/ dir's mode in case it already
    // existed with something looser.
    (void)chmod(buf, 0700);
    return 0;
}

mb_error_t mb_consent_record(const char *group,
                             const char *value,
                             mb_consent_status_t decision) {
    if (!group || !group[0] || !value || !value[0]) return MB_EINVAL;
    if (decision != MB_CONSENT_ALLOW && decision != MB_CONSENT_DENY) {
        return MB_EINVAL;
    }

    char *path = consents_path_for_self();
    if (!path) return MB_EPERM;

    mb_error_t rc = (mb_error_t)mkdir_p_0700(path);
    if (rc != MB_EOK) { free(path); return rc; }

    // Read the existing file (if any) and parse into a doc.
    char *buf = slurp(path);
    cs_doc_t doc = {0};
    int prc = cs_parse(buf, &doc);
    free(buf);
    if (prc != 0) {
        cs_doc_free(&doc);
        free(path);
        return (mb_error_t)prc;
    }

    // Build the target section name.
    size_t nl = strlen(group) + 1 + strlen(value) + 1;
    char *name = malloc(nl);
    if (!name) {
        cs_doc_free(&doc);
        free(path);
        return MB_ENOMEM;
    }
    snprintf(name, nl, "%s.%s", group, value);

    // Build the new body.
    char body[128];
    int bw = format_section_body(body, sizeof(body), decision);
    if (bw < 0) {
        free(name);
        cs_doc_free(&doc);
        free(path);
        return MB_EIPC;
    }

    // Replace in place, or append.
    bool replaced = false;
    for (size_t i = 0; i < doc.n; i++) {
        if (strcmp(doc.sections[i].name, name) == 0) {
            free(doc.sections[i].body);
            doc.sections[i].body = strdup(body);
            if (!doc.sections[i].body) {
                free(name);
                cs_doc_free(&doc);
                free(path);
                return MB_ENOMEM;
            }
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        if (doc.n == doc.cap) {
            size_t ncap = doc.cap ? doc.cap * 2 : 4;
            cs_section_t *ns = realloc(doc.sections, ncap * sizeof(*ns));
            if (!ns) {
                free(name);
                cs_doc_free(&doc);
                free(path);
                return MB_ENOMEM;
            }
            doc.sections = ns;
            doc.cap = ncap;
        }
        doc.sections[doc.n].name = name;
        doc.sections[doc.n].body = strdup(body);
        if (!doc.sections[doc.n].body) {
            cs_doc_free(&doc);
            free(path);
            return MB_ENOMEM;
        }
        doc.n++;
    } else {
        free(name);
    }

    // Serialize: preamble + for each section "[name]\n" + body.
    // If the previous byte isn't a newline, append one between
    // sections so a pathologically trimmed input doesn't fuse
    // headers with bodies.
    char *out = calloc(1, 1);
    if (!out) { cs_doc_free(&doc); free(path); return MB_ENOMEM; }
    size_t out_len = 0;

    if (doc.preamble) {
        size_t pl = strlen(doc.preamble);
        if (str_append(&out, &out_len, doc.preamble, pl) != 0) {
            free(out); cs_doc_free(&doc); free(path); return MB_ENOMEM;
        }
        if (pl > 0 && out[out_len - 1] != '\n') {
            if (str_append(&out, &out_len, "\n", 1) != 0) {
                free(out); cs_doc_free(&doc); free(path); return MB_ENOMEM;
            }
        }
    }
    for (size_t i = 0; i < doc.n; i++) {
        char header[256];
        int hw = snprintf(header, sizeof(header),
                          "[%s]\n", doc.sections[i].name);
        if (hw < 0 || (size_t)hw >= sizeof(header)) {
            free(out); cs_doc_free(&doc); free(path); return MB_EIPC;
        }
        if (str_append(&out, &out_len, header, (size_t)hw) != 0) {
            free(out); cs_doc_free(&doc); free(path); return MB_ENOMEM;
        }
        const char *b = doc.sections[i].body ? doc.sections[i].body : "";
        size_t bl = strlen(b);
        if (str_append(&out, &out_len, b, bl) != 0) {
            free(out); cs_doc_free(&doc); free(path); return MB_ENOMEM;
        }
        if (bl > 0 && b[bl - 1] != '\n') {
            if (str_append(&out, &out_len, "\n", 1) != 0) {
                free(out); cs_doc_free(&doc); free(path); return MB_ENOMEM;
            }
        }
    }

    rc = (mb_error_t)atomic_write(path, out, out_len);

    free(out);
    cs_doc_free(&doc);
    free(path);
    return rc;
}
