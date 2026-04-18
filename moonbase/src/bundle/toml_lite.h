// CopyCatOS — by Kyle Blizzard at Blizzard.show

// toml_lite — narrow TOML subset parser for Info.appc.
//
// Purpose-built for the exact shape of moonbase/docs/info_appc_schema.md:
// flat top-level tables, bare keys, double-quoted or single-quoted strings,
// arrays of strings, and `#` comments. No numbers, booleans, datetimes,
// inline tables, dotted keys, array-of-tables, or multiline strings.
// Anything outside that subset is a parse error — the schema the launcher
// is validating against never needs them, and rejecting early keeps
// surprises from sneaking into bundle metadata.
//
// Parse result is an in-memory tree owned by a single tl_doc. Callers
// borrow pointers for the document's lifetime; tl_doc_free frees
// everything.

#ifndef MOONBASE_BUNDLE_TOML_LITE_H
#define MOONBASE_BUNDLE_TOML_LITE_H

#include <stddef.h>

typedef struct tl_doc tl_doc;
typedef struct tl_table tl_table;

typedef enum {
    TL_OK = 0,
    TL_ERR_PARSE,
    TL_ERR_NO_MEM,
} tl_error;

// Parse TOML from a memory buffer. src does NOT need to stay alive after
// this call — strings are deep-copied into the document.
// On success: *out owns the parse tree. Caller frees via tl_doc_free.
// On failure: err_buf receives a human-readable message (line number
// included when applicable), *out is unset.
tl_error tl_parse(const char *src, size_t len,
                  tl_doc **out,
                  char *err_buf, size_t err_cap);

void tl_doc_free(tl_doc *doc);

// ---- top-level table access ---------------------------------------

// NULL if table absent.
const tl_table *tl_doc_get_table(const tl_doc *doc, const char *name);

// Enumerate every top-level table name, for unknown-table warning loop.
size_t tl_doc_table_count(const tl_doc *doc);
const char *tl_doc_table_name(const tl_doc *doc, size_t i);

// ---- value access -------------------------------------------------

// Returns borrowed string pointer, or NULL if key absent or not a string.
const char *tl_table_string(const tl_table *t, const char *key);

// Array of strings. Returns count; sets *items to a borrowed array of
// borrowed string pointers (do not free). On absence or wrong type,
// returns 0 and sets *items = NULL.
size_t tl_table_string_array(const tl_table *t, const char *key,
                             const char *const **items);

// Enumerate every key inside a table, for unknown-key warning loop.
size_t tl_table_key_count(const tl_table *t);
const char *tl_table_key_name(const tl_table *t, size_t i);

#endif
