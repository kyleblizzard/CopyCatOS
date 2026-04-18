// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ─── search.c ───
// Desktop-file parser and search engine.
//
// This module has two jobs:
//
//   A) At startup, scan the standard XDG application directories
//      for .desktop files, parse each one, and build an in-memory
//      "catalogue" of installed applications.
//
//   B) When the user types a query, filter the catalogue by
//      substring match on a pre-built lowercase blob, then sort
//      results by relevance (name-prefix first, then name-contains,
//      then keyword-only matches).
//
// The .desktop file format is part of the freedesktop.org
// specification.  It's basically an INI file with a mandatory
// [Desktop Entry] section that contains keys like Name, Exec,
// Icon, etc.

#define _GNU_SOURCE  // For strcasecmp under strict C11

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/types.h>

#include "search.h"
#include "icons.h"

// ──────────────────────────────────────────────
// Internal catalogue storage
// ──────────────────────────────────────────────

// Hard upper limit on the number of desktop entries we store.
// Most Linux desktops have a few hundred at most.
#define MAX_ENTRIES 1024

// The master list of parsed application entries.
static SearchEntry catalogue[MAX_ENTRIES];

// How many entries are currently in the catalogue.
static int catalogue_count = 0;

// ──────────────────────────────────────────────
// String helpers
// ──────────────────────────────────────────────

// Convert a string to lowercase in-place.
static void str_lower(char *s) {
    for (; *s; s++)
        *s = (char)tolower((unsigned char)*s);
}

// Strip leading and trailing whitespace from a string in-place
// by shifting the content forward and null-terminating early.
static void str_trim(char *s) {
    // Skip leading whitespace.
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;

    // If the whole string is whitespace, just empty it.
    if (*start == '\0') {
        s[0] = '\0';
        return;
    }

    // Find the last non-whitespace character.
    char *end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;

    // Move trimmed content to the front of the buffer.
    size_t len = (size_t)(end - start + 1);
    if (start != s) memmove(s, start, len);
    s[len] = '\0';
}

// Case-insensitive substring search.  Returns true if
// `needle` appears anywhere inside `haystack`.
static int str_contains_ci(const char *haystack, const char *needle) {
    // Both haystack and needle should already be lowercase
    // in our usage, but we handle them as-is for safety.
    return strstr(haystack, needle) != NULL;
}

// Check if string `s` starts with prefix `p` (case-insensitive).
// Both are expected to already be lowercase.
static int str_starts_with_ci(const char *s, const char *p) {
    return strncmp(s, p, strlen(p)) == 0;
}

// ──────────────────────────────────────────────
// .desktop file parser
// ──────────────────────────────────────────────

// Parse a single .desktop file and, if it represents a visible
// application, append it to the catalogue.
static void parse_desktop_file(const char *filepath) {
    if (catalogue_count >= MAX_ENTRIES) return;

    FILE *fp = fopen(filepath, "r");
    if (!fp) return;

    // Temporary storage for the fields we care about.
    char name[256]         = {0};
    char generic_name[256] = {0};
    char exec[512]         = {0};
    char icon_name[256]    = {0};
    char keywords[512]     = {0};
    char type[64]          = {0};
    int  no_display        = 0;
    int  hidden            = 0;

    // We only care about keys inside the [Desktop Entry] section.
    // This flag tracks whether we've entered that section.
    int in_desktop_entry = 0;

    char line[2048];
    while (fgets(line, sizeof(line), fp)) {
        // Strip the trailing newline.
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
        if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';

        // Skip empty lines and comments.
        if (line[0] == '\0' || line[0] == '#') continue;

        // Detect section headers like [Desktop Entry] or [Desktop Action ...].
        if (line[0] == '[') {
            in_desktop_entry = (strcmp(line, "[Desktop Entry]") == 0);
            // If we were inside [Desktop Entry] and just hit a new
            // section, stop — we've parsed everything we need.
            if (!in_desktop_entry && name[0] != '\0') break;
            continue;
        }

        // Only parse key=value pairs inside [Desktop Entry].
        if (!in_desktop_entry) continue;

        // Split at the first '=' character.
        char *eq = strchr(line, '=');
        if (!eq) continue;

        // Null-terminate the key, advance to the value.
        *eq = '\0';
        char *key   = line;
        char *value = eq + 1;

        str_trim(key);
        str_trim(value);

        // Ignore localised keys like "Name[fr]" — we only want
        // the bare "Name" key (the default / English value).
        if (strchr(key, '[')) continue;

        // Store the values we care about.
        if      (strcmp(key, "Name")        == 0) strncpy(name,         value, sizeof(name) - 1);
        else if (strcmp(key, "GenericName") == 0) strncpy(generic_name, value, sizeof(generic_name) - 1);
        else if (strcmp(key, "Exec")        == 0) strncpy(exec,         value, sizeof(exec) - 1);
        else if (strcmp(key, "Icon")        == 0) strncpy(icon_name,    value, sizeof(icon_name) - 1);
        else if (strcmp(key, "Keywords")    == 0) strncpy(keywords,     value, sizeof(keywords) - 1);
        else if (strcmp(key, "Type")        == 0) strncpy(type,         value, sizeof(type) - 1);
        else if (strcmp(key, "NoDisplay")   == 0) no_display = (strcasecmp(value, "true") == 0);
        else if (strcmp(key, "Hidden")      == 0) hidden     = (strcasecmp(value, "true") == 0);
    }

    fclose(fp);

    // ── Filter out entries we don't want ──

    // Must have a name and an exec line.
    if (name[0] == '\0' || exec[0] == '\0') return;

    // Must be of type "Application" (or have no Type, which we treat
    // as Application for compatibility).
    if (type[0] != '\0' && strcmp(type, "Application") != 0) return;

    // Skip entries the desktop file explicitly hides.
    if (no_display || hidden) return;

    // ── Deduplicate by name ──
    // If we already have an entry with the same name, skip this one.
    for (int i = 0; i < catalogue_count; i++) {
        if (strcmp(catalogue[i].name, name) == 0) return;
    }

    // ── Build the catalogue entry ──
    SearchEntry *e = &catalogue[catalogue_count];
    memset(e, 0, sizeof(*e));

    strncpy(e->name,         name,         sizeof(e->name) - 1);
    strncpy(e->generic_name, generic_name, sizeof(e->generic_name) - 1);
    strncpy(e->exec,         exec,         sizeof(e->exec) - 1);
    strncpy(e->icon_name,    icon_name,    sizeof(e->icon_name) - 1);
    strncpy(e->keywords,     keywords,     sizeof(e->keywords) - 1);

    // Build the search blob: "name generic_name keywords" all in
    // lowercase.  This lets us do a single strstr() per entry when
    // the user types a query.
    snprintf(e->search_blob, sizeof(e->search_blob),
             "%s %s %s", name, generic_name, keywords);
    str_lower(e->search_blob);

    // Icon will be loaded lazily (on first render).
    e->icon = NULL;

    catalogue_count++;
}

// ──────────────────────────────────────────────
// Directory scanner
// ──────────────────────────────────────────────

// Scan a single directory for .desktop files and parse each one.
static void scan_directory(const char *dirpath) {
    DIR *dir = opendir(dirpath);
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        // Only process files ending in ".desktop".
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len < 9) continue; // ".desktop" is 8 chars + at least 1 char name
        if (strcmp(name + len - 8, ".desktop") != 0) continue;

        // Build the full path.
        char path[2048];
        snprintf(path, sizeof(path), "%s/%s", dirpath, name);

        parse_desktop_file(path);
    }

    closedir(dir);
}

// ──────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────

void search_init(void) {
    catalogue_count = 0;

    // Standard directories where .desktop files live on a
    // freedesktop.org-compliant system.
    scan_directory("/usr/share/applications");
    scan_directory("/usr/local/share/applications");

    // User-specific applications.
    const char *home = getenv("HOME");
    if (home) {
        char buf[1024];

        snprintf(buf, sizeof(buf), "%s/.local/share/applications", home);
        scan_directory(buf);

        // Flatpak per-user exports.
        snprintf(buf, sizeof(buf),
                 "%s/.local/share/flatpak/exports/share/applications", home);
        scan_directory(buf);
    }

    // System-wide Flatpak exports.
    scan_directory("/var/lib/flatpak/exports/share/applications");

    printf("searchsystem: loaded %d applications\n", catalogue_count);
}

void search_cleanup(void) {
    for (int i = 0; i < catalogue_count; i++) {
        // Icons are owned by the icon cache, so we don't free them here.
        catalogue[i].icon = NULL;
    }
    catalogue_count = 0;
}

int search_entry_count(void) {
    return catalogue_count;
}

// ──────────────────────────────────────────────
// Sorting helper for qsort
// ──────────────────────────────────────────────

// Each result is tagged with a priority tier before sorting:
//   0 = name starts with query (best match)
//   1 = name contains query
//   2 = keyword / generic-name match only
//
// Within the same tier we sort alphabetically by name.

// We use a global (file-scope) variable to pass the current
// query into the comparison function because qsort's comparator
// doesn't accept user data.
static char sort_query[1024];

static int compare_results(const void *a, const void *b) {
    const SearchEntry *ea = *(const SearchEntry *const *)a;
    const SearchEntry *eb = *(const SearchEntry *const *)b;

    // Compute lowercase copies of names for comparison.
    char na[256], nb[256];
    strncpy(na, ea->name, sizeof(na) - 1); na[sizeof(na)-1] = '\0';
    strncpy(nb, eb->name, sizeof(nb) - 1); nb[sizeof(nb)-1] = '\0';
    str_lower(na);
    str_lower(nb);

    // Determine the priority tier for each entry.
    int pa = 2, pb = 2;

    if (str_starts_with_ci(na, sort_query)) pa = 0;
    else if (str_contains_ci(na, sort_query)) pa = 1;

    if (str_starts_with_ci(nb, sort_query)) pb = 0;
    else if (str_contains_ci(nb, sort_query)) pb = 1;

    // Sort by tier first, then alphabetically.
    if (pa != pb) return pa - pb;
    return strcmp(na, nb);
}

int search_query(const char *query, SearchEntry **out_results, int max_results) {
    // If the query is empty, show nothing.
    if (!query || query[0] == '\0') return 0;

    // Build a lowercase version of the query for matching.
    char lquery[1024];
    strncpy(lquery, query, sizeof(lquery) - 1);
    lquery[sizeof(lquery) - 1] = '\0';
    str_lower(lquery);

    // First pass: collect all entries whose search_blob contains
    // the query as a substring.
    int count = 0;
    for (int i = 0; i < catalogue_count && count < max_results; i++) {
        if (str_contains_ci(catalogue[i].search_blob, lquery)) {
            out_results[count++] = &catalogue[i];
        }
    }

    // Second pass: sort by relevance.
    strncpy(sort_query, lquery, sizeof(sort_query) - 1);
    sort_query[sizeof(sort_query) - 1] = '\0';

    qsort(out_results, (size_t)count, sizeof(SearchEntry *), compare_results);

    // Lazily resolve icons for visible results.
    int visible = count < 8 ? count : 8; // MAX_VISIBLE_RESULTS
    for (int i = 0; i < visible; i++) {
        if (!out_results[i]->icon && out_results[i]->icon_name[0] != '\0') {
            out_results[i]->icon = icon_lookup(out_results[i]->icon_name);
        }
    }

    return count;
}
