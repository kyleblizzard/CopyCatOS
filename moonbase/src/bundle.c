// CopyCatOS — by Kyle Blizzard at Blizzard.show

// bundle.c — .appc bundle manifest reader
//
// See bundle.h for the bundle layout. This file is the v0.1 scaffold
// implementation: it resolves the bundle path, reads Info.appc, and
// pulls out the four fields we currently care about. Anything missing
// is an error. Anything extra is ignored silently.

#include "bundle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>

// Duplicate a string with strdup semantics, but return NULL on empty input.
// Helper used when parsing manifest fields so we never store empty strings.
static char *dup_nonempty(const char *s) {
    if (!s || !*s) return NULL;
    return strdup(s);
}

// Trim leading and trailing whitespace in-place. Returns the start of the
// trimmed region (which may be inside the buffer). The input must be NUL
// terminated. Used on every line read from Info.appc.
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }
    *end = '\0';
    return s;
}

// Parse a single key=value line. Fills *key and *value with pointers into
// the provided line buffer (which is modified in place). Returns 0 on
// success, -1 if the line has no '=' separator.
static int split_kv(char *line, char **key, char **value) {
    char *eq = strchr(line, '=');
    if (!eq) return -1;
    *eq = '\0';
    *key = trim(line);
    *value = trim(eq + 1);
    return 0;
}

// Build the absolute path to Info.appc and return a newly allocated string.
// Caller frees. Returns NULL on allocation failure.
static char *info_path_for(const char *bundle_path) {
    size_t n = strlen(bundle_path) + strlen("/Contents/Info.appc") + 1;
    char *buf = malloc(n);
    if (!buf) return NULL;
    snprintf(buf, n, "%s/Contents/Info.appc", bundle_path);
    return buf;
}

struct mb_bundle *bundle_open(const char *bundle_path) {
    if (!bundle_path) return NULL;

    // Verify the bundle directory actually exists. Doing this up front
    // gives a cleaner error than failing later on the Info.appc open.
    struct stat st;
    if (stat(bundle_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "[moonbase] bundle path is not a directory: %s\n",
                bundle_path);
        return NULL;
    }

    char *info = info_path_for(bundle_path);
    if (!info) return NULL;

    FILE *f = fopen(info, "r");
    if (!f) {
        fprintf(stderr, "[moonbase] Info.appc not found at %s\n", info);
        free(info);
        return NULL;
    }
    free(info);

    // Allocate the result up front; populate fields as we parse. Any
    // missing required field causes bundle_close() and a NULL return.
    struct mb_bundle *b = calloc(1, sizeof(*b));
    if (!b) { fclose(f); return NULL; }
    b->bundle_path = strdup(bundle_path);

    char line[1024];
    char *exec_relative = NULL;
    while (fgets(line, sizeof(line), f)) {
        // Skip blank lines and comments (# prefix). Everything else is
        // expected to be key=value.
        char *trimmed = trim(line);
        if (!*trimmed || *trimmed == '#') continue;

        char *key, *value;
        if (split_kv(trimmed, &key, &value) != 0) continue;

        if (strcmp(key, "CFBundleName") == 0) {
            b->display_name = dup_nonempty(value);
        } else if (strcmp(key, "Host") == 0) {
            b->host_kind = dup_nonempty(value);
        } else if (strcmp(key, "Executable") == 0) {
            exec_relative = dup_nonempty(value);
        }
        // Other fields (CFBundleVersion, etc.) are ignored in v0.1.
    }
    fclose(f);

    // Validate required fields. Any missing value is a fatal manifest
    // error — better to fail now than to ship a half-broken bundle to
    // the language host.
    if (!b->display_name || !b->host_kind || !exec_relative) {
        fprintf(stderr,
                "[moonbase] Info.appc is missing required fields "
                "(CFBundleName, Host, Executable)\n");
        free(exec_relative);
        bundle_close(b);
        return NULL;
    }

    // Resolve Executable relative to the bundle directory. We store the
    // absolute path so the language host does not need to re-resolve.
    size_t n = strlen(bundle_path) + 1 + strlen(exec_relative) + 1;
    b->executable_path = malloc(n);
    if (!b->executable_path) {
        free(exec_relative);
        bundle_close(b);
        return NULL;
    }
    snprintf(b->executable_path, n, "%s/%s", bundle_path, exec_relative);
    free(exec_relative);

    return b;
}

void bundle_close(struct mb_bundle *bundle) {
    if (!bundle) return;
    free(bundle->bundle_path);
    free(bundle->display_name);
    free(bundle->host_kind);
    free(bundle->executable_path);
    free(bundle);
}
