// CopyCatOS — by Kyle Blizzard at Blizzard.show

// entitlements.c — reads MOONBASE_ENTITLEMENTS once, answers queries.
//
// Format of MOONBASE_ENTITLEMENTS (built by moonbase-launch):
//
//   group1=v1,v2;group2=v3
//
// Groups are semicolon-separated; values inside a group are comma-
// separated. Missing groups simply don't appear. Unknown groups are
// ignored by the consumer — the schema allowlist is enforced by the
// Info.appc parser, so by the time moonbase-launch emits this string
// every group/value pair is schema-valid.
//
// Allocation failure during parse silently drops the offending entry.
// The gate then treats that (group, value) as not declared — failing
// closed, which is the safe side for a security-adjacent default.

#include "entitlements.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *group;
    char *value;
} entry_t;

static entry_t *entries = NULL;
static size_t entries_count = 0;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void add_entry(const char *group, size_t glen,
                      const char *value, size_t vlen) {
    entry_t *ne = realloc(entries, (entries_count + 1) * sizeof(entry_t));
    if (!ne) return;
    entries = ne;
    entry_t *e = &entries[entries_count];
    e->group = malloc(glen + 1);
    e->value = malloc(vlen + 1);
    if (!e->group || !e->value) {
        free(e->group);
        free(e->value);
        return;
    }
    memcpy(e->group, group, glen); e->group[glen] = '\0';
    memcpy(e->value, value, vlen); e->value[vlen] = '\0';
    entries_count++;
}

static void parse_once(void) {
    const char *src = getenv("MOONBASE_ENTITLEMENTS");
    if (!src || !*src) return;

    const char *p = src;
    while (*p) {
        const char *group_start = p;
        while (*p && *p != '=' && *p != ';') p++;
        if (*p != '=') {
            // Malformed — no '=' before ';' or end. Skip this chunk.
            while (*p && *p != ';') p++;
            if (*p == ';') p++;
            continue;
        }
        size_t glen = (size_t)(p - group_start);
        p++;  // skip '='

        // Values until the next ';' or end.
        while (*p && *p != ';') {
            const char *v_start = p;
            while (*p && *p != ',' && *p != ';') p++;
            size_t vlen = (size_t)(p - v_start);
            if (vlen > 0) add_entry(group_start, glen, v_start, vlen);
            if (*p == ',') p++;
        }
        if (*p == ';') p++;
    }
}

bool mb_has_entitlement(const char *group, const char *value) {
    if (!group || !value) return false;
    pthread_once(&once, parse_once);
    for (size_t i = 0; i < entries_count; i++) {
        if (strcmp(entries[i].group, group) == 0 &&
            strcmp(entries[i].value, value) == 0) {
            return true;
        }
    }
    return false;
}
