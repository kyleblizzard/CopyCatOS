// CopyCatOS — by Kyle Blizzard at Blizzard.show

// toml_lite — narrow TOML subset parser. See toml_lite.h for the
// supported grammar; everything outside that grammar is a parse error.
//
// Implementation shape: single-pass recursive-descent over the source
// buffer. One tl_value type (string or string-array); one tl_key type
// (name + value); one tl_table (name + keys); one tl_doc (tables).
// Every allocation is tracked under the document so tl_doc_free is a
// single tree walk.

#include "toml_lite.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// -------------------------------------------------------------------
// internal types
// -------------------------------------------------------------------

typedef enum {
    TL_V_STRING,
    TL_V_ARRAY_STRING,
} tl_vkind;

typedef struct {
    tl_vkind kind;
    // string case
    char *s;
    // array case
    char **arr;
    size_t arr_count;
} tl_value;

typedef struct {
    char *name;
    tl_value val;
} tl_key;

struct tl_table {
    char *name;
    tl_key *keys;
    size_t key_count;
    size_t key_cap;
};

struct tl_doc {
    tl_table *tables;
    size_t table_count;
    size_t table_cap;
};

// -------------------------------------------------------------------
// parse state
// -------------------------------------------------------------------

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    int line;           // 1-based
    tl_doc *doc;
    char *err_buf;
    size_t err_cap;
    int failed;         // 1 once we've emitted an error
} pstate;

static void set_err(pstate *st, const char *fmt, ...) {
    if (st->failed) return;
    st->failed = 1;
    if (!st->err_buf || st->err_cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st->err_buf, st->err_cap, fmt, ap);
    va_end(ap);
}

// -------------------------------------------------------------------
// memory helpers — everything is tracked, tl_doc_free walks the tree
// -------------------------------------------------------------------

static char *dup_range(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static void free_value(tl_value *v) {
    if (v->kind == TL_V_STRING) {
        free(v->s);
    } else {
        for (size_t i = 0; i < v->arr_count; i++) free(v->arr[i]);
        free(v->arr);
    }
}

static void free_table(tl_table *t) {
    free(t->name);
    for (size_t i = 0; i < t->key_count; i++) {
        free(t->keys[i].name);
        free_value(&t->keys[i].val);
    }
    free(t->keys);
}

void tl_doc_free(tl_doc *doc) {
    if (!doc) return;
    for (size_t i = 0; i < doc->table_count; i++) free_table(&doc->tables[i]);
    free(doc->tables);
    free(doc);
}

// Grow a vector by 1 and return a pointer to the new slot, or NULL on OOM.
// Slot is zero-initialized.
static void *vec_push(void *base, size_t *count, size_t *cap, size_t elem_size) {
    if (*count == *cap) {
        size_t nc = *cap ? *cap * 2 : 4;
        void *np = realloc(*(void **)base, nc * elem_size);
        if (!np) return NULL;
        *(void **)base = np;
        *cap = nc;
    }
    char *p = *(char **)base + (*count) * elem_size;
    memset(p, 0, elem_size);
    (*count)++;
    return p;
}

// -------------------------------------------------------------------
// scanner primitives
// -------------------------------------------------------------------

static int at_end(pstate *st) { return st->pos >= st->len; }
static char peek(pstate *st) { return at_end(st) ? '\0' : st->src[st->pos]; }
static char advance(pstate *st) {
    if (at_end(st)) return '\0';
    char c = st->src[st->pos++];
    if (c == '\n') st->line++;
    return c;
}
static int match(pstate *st, char c) {
    if (peek(st) == c) { advance(st); return 1; }
    return 0;
}

// Skip spaces and tabs but not newlines.
static void skip_hspace(pstate *st) {
    while (!at_end(st)) {
        char c = peek(st);
        if (c == ' ' || c == '\t') advance(st);
        else break;
    }
}

// Skip spaces, tabs, and newlines (used inside arrays + between items).
static void skip_any_ws(pstate *st) {
    while (!at_end(st)) {
        char c = peek(st);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') advance(st);
        else if (c == '#') {
            while (!at_end(st) && peek(st) != '\n') advance(st);
        } else break;
    }
}

// Skip a comment if one starts here, up to but not including the newline.
static void skip_comment(pstate *st) {
    if (peek(st) == '#') {
        while (!at_end(st) && peek(st) != '\n') advance(st);
    }
}

// Consume a full line terminator (CR, LF, or CRLF). No-op if already at EOF.
static void skip_eol(pstate *st) {
    if (peek(st) == '\r') advance(st);
    if (peek(st) == '\n') advance(st);
}

// -------------------------------------------------------------------
// parsers
// -------------------------------------------------------------------

static int is_bare_key_char(char c) {
    return (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9')
        || c == '-' || c == '_';
}

// [table] header (whole bracketed part). Returns a heap-allocated name,
// or NULL on error. Expects `[` NOT yet consumed.
static char *parse_table_header(pstate *st) {
    if (!match(st, '[')) {
        set_err(st, "line %d: expected '['", st->line);
        return NULL;
    }
    skip_hspace(st);
    size_t start = st->pos;
    while (!at_end(st) && is_bare_key_char(peek(st))) advance(st);
    size_t end = st->pos;
    if (end == start) {
        set_err(st, "line %d: empty table name", st->line);
        return NULL;
    }
    skip_hspace(st);
    if (!match(st, ']')) {
        set_err(st, "line %d: expected ']' in table header", st->line);
        return NULL;
    }
    skip_hspace(st);
    skip_comment(st);
    if (!at_end(st) && peek(st) != '\n' && peek(st) != '\r') {
        set_err(st, "line %d: junk after ']' in table header", st->line);
        return NULL;
    }
    skip_eol(st);
    return dup_range(st->src + start, end - start);
}

// Bare key. Returns heap-allocated name, or NULL on error.
static char *parse_bare_key(pstate *st) {
    size_t start = st->pos;
    while (!at_end(st) && is_bare_key_char(peek(st))) advance(st);
    if (st->pos == start) {
        set_err(st, "line %d: expected key", st->line);
        return NULL;
    }
    return dup_range(st->src + start, st->pos - start);
}

// Decode a quoted TOML string. Supports "..." with escape sequences and
// '...' literal. Returns heap-allocated decoded bytes. Returns NULL on error.
static char *parse_quoted_string(pstate *st) {
    char q = peek(st);
    if (q != '"' && q != '\'') {
        set_err(st, "line %d: expected string", st->line);
        return NULL;
    }
    advance(st);  // consume opening quote

    // Work into a growing buffer — escapes may shorten or lengthen nothing
    // beyond 1 char per source byte, so len is a safe upper bound.
    size_t cap = 32;
    char *buf = malloc(cap);
    if (!buf) { set_err(st, "out of memory"); return NULL; }
    size_t blen = 0;

    while (!at_end(st)) {
        char c = peek(st);
        if (c == q) {
            advance(st);
            if (blen == cap) {
                char *nb = realloc(buf, cap + 1);
                if (!nb) { free(buf); set_err(st, "out of memory"); return NULL; }
                buf = nb;
            }
            buf[blen] = '\0';
            return buf;
        }
        if (c == '\n') {
            free(buf);
            set_err(st, "line %d: unterminated string", st->line);
            return NULL;
        }
        // Escape sequences: only in "..." strings per TOML spec.
        if (q == '"' && c == '\\') {
            advance(st);
            char esc = advance(st);
            char decoded;
            switch (esc) {
                case 'n':  decoded = '\n'; break;
                case 't':  decoded = '\t'; break;
                case 'r':  decoded = '\r'; break;
                case '"':  decoded = '"';  break;
                case '\\': decoded = '\\'; break;
                case '/':  decoded = '/';  break;
                case '0':  decoded = '\0'; break;
                default:
                    free(buf);
                    set_err(st, "line %d: bad escape \\%c", st->line, esc);
                    return NULL;
            }
            if (blen + 1 >= cap) {
                cap *= 2;
                char *nb = realloc(buf, cap);
                if (!nb) { free(buf); set_err(st, "out of memory"); return NULL; }
                buf = nb;
            }
            buf[blen++] = decoded;
            continue;
        }
        advance(st);
        if (blen + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); set_err(st, "out of memory"); return NULL; }
            buf = nb;
        }
        buf[blen++] = c;
    }
    free(buf);
    set_err(st, "line %d: unterminated string", st->line);
    return NULL;
}

// Parse an array of strings: `[ "a", "b", ... ]`. Newlines permitted between
// elements. Populates val; returns 0 on success, -1 on error.
static int parse_string_array(pstate *st, tl_value *val) {
    if (!match(st, '[')) {
        set_err(st, "line %d: expected '['", st->line);
        return -1;
    }
    val->kind = TL_V_ARRAY_STRING;
    val->arr = NULL;
    val->arr_count = 0;
    size_t cap = 0;

    for (;;) {
        skip_any_ws(st);
        if (peek(st) == ']') { advance(st); return 0; }
        char *s = parse_quoted_string(st);
        if (!s) return -1;
        if (val->arr_count == cap) {
            size_t nc = cap ? cap * 2 : 4;
            char **na = realloc(val->arr, nc * sizeof(char *));
            if (!na) { free(s); set_err(st, "out of memory"); return -1; }
            val->arr = na; cap = nc;
        }
        val->arr[val->arr_count++] = s;
        skip_any_ws(st);
        if (peek(st) == ',') { advance(st); continue; }
        if (peek(st) == ']') { advance(st); return 0; }
        set_err(st, "line %d: expected ',' or ']' in array", st->line);
        return -1;
    }
}

// Parse one `key = value` line. Value is either a string or a
// string-array. Consumes the terminating newline (if any).
static int parse_key_line(pstate *st, tl_table *tbl) {
    char *key = parse_bare_key(st);
    if (!key) return -1;
    skip_hspace(st);
    if (!match(st, '=')) {
        free(key);
        set_err(st, "line %d: expected '='", st->line);
        return -1;
    }
    skip_hspace(st);

    tl_value val = (tl_value){0};
    char c = peek(st);
    if (c == '"' || c == '\'') {
        char *s = parse_quoted_string(st);
        if (!s) { free(key); return -1; }
        val.kind = TL_V_STRING;
        val.s = s;
    } else if (c == '[') {
        if (parse_string_array(st, &val) != 0) { free(key); return -1; }
    } else {
        free(key);
        set_err(st, "line %d: unsupported value type (only strings and "
                    "string arrays are allowed)", st->line);
        return -1;
    }

    skip_hspace(st);
    skip_comment(st);
    if (!at_end(st) && peek(st) != '\n' && peek(st) != '\r') {
        free(key); free_value(&val);
        set_err(st, "line %d: junk after value", st->line);
        return -1;
    }
    skip_eol(st);

    // Duplicate key check inside this table.
    for (size_t i = 0; i < tbl->key_count; i++) {
        if (strcmp(tbl->keys[i].name, key) == 0) {
            free(key); free_value(&val);
            set_err(st, "line %d: duplicate key '%s' in table [%s]",
                    st->line, tbl->keys[i].name, tbl->name);
            return -1;
        }
    }
    tl_key *slot = vec_push(&tbl->keys, &tbl->key_count, &tbl->key_cap, sizeof(tl_key));
    if (!slot) {
        free(key); free_value(&val);
        set_err(st, "out of memory"); return -1;
    }
    slot->name = key;
    slot->val = val;
    return 0;
}

// -------------------------------------------------------------------
// public API
// -------------------------------------------------------------------

tl_error tl_parse(const char *src, size_t len, tl_doc **out,
                  char *err_buf, size_t err_cap) {
    if (!src || !out) return TL_ERR_PARSE;

    tl_doc *doc = calloc(1, sizeof(tl_doc));
    if (!doc) {
        if (err_buf && err_cap) snprintf(err_buf, err_cap, "out of memory");
        return TL_ERR_NO_MEM;
    }

    pstate st = {
        .src = src, .len = len, .pos = 0, .line = 1,
        .doc = doc, .err_buf = err_buf, .err_cap = err_cap, .failed = 0,
    };

    tl_table *cur = NULL;

    for (;;) {
        // Skip leading blanks, comments, and blank lines.
        for (;;) {
            skip_hspace(&st);
            if (at_end(&st)) break;
            char c = peek(&st);
            if (c == '#') { while (!at_end(&st) && peek(&st) != '\n') advance(&st); }
            if (peek(&st) == '\n' || peek(&st) == '\r') { skip_eol(&st); continue; }
            break;
        }
        if (at_end(&st)) break;

        char c = peek(&st);
        if (c == '[') {
            char *name = parse_table_header(&st);
            if (!name) goto fail;
            // Duplicate table check.
            for (size_t i = 0; i < doc->table_count; i++) {
                if (strcmp(doc->tables[i].name, name) == 0) {
                    free(name);
                    set_err(&st, "line %d: duplicate table [%s]", st.line, doc->tables[i].name);
                    goto fail;
                }
            }
            tl_table *slot = vec_push(&doc->tables, &doc->table_count, &doc->table_cap, sizeof(tl_table));
            if (!slot) {
                free(name);
                set_err(&st, "out of memory");
                goto fail;
            }
            slot->name = name;
            cur = slot;
        } else if (is_bare_key_char(c)) {
            if (!cur) {
                set_err(&st, "line %d: key outside any [table] — root-level "
                             "keys are not supported", st.line);
                goto fail;
            }
            if (parse_key_line(&st, cur) != 0) goto fail;
        } else {
            set_err(&st, "line %d: unexpected character '%c'", st.line, c);
            goto fail;
        }
    }

    *out = doc;
    return TL_OK;

fail:
    tl_doc_free(doc);
    return TL_ERR_PARSE;
}

const tl_table *tl_doc_get_table(const tl_doc *doc, const char *name) {
    if (!doc || !name) return NULL;
    for (size_t i = 0; i < doc->table_count; i++) {
        if (strcmp(doc->tables[i].name, name) == 0) return &doc->tables[i];
    }
    return NULL;
}

size_t tl_doc_table_count(const tl_doc *doc) {
    return doc ? doc->table_count : 0;
}

const char *tl_doc_table_name(const tl_doc *doc, size_t i) {
    if (!doc || i >= doc->table_count) return NULL;
    return doc->tables[i].name;
}

const char *tl_table_string(const tl_table *t, const char *key) {
    if (!t || !key) return NULL;
    for (size_t i = 0; i < t->key_count; i++) {
        if (strcmp(t->keys[i].name, key) == 0) {
            if (t->keys[i].val.kind != TL_V_STRING) return NULL;
            return t->keys[i].val.s;
        }
    }
    return NULL;
}

size_t tl_table_string_array(const tl_table *t, const char *key,
                             const char *const **items) {
    if (items) *items = NULL;
    if (!t || !key) return 0;
    for (size_t i = 0; i < t->key_count; i++) {
        if (strcmp(t->keys[i].name, key) == 0) {
            if (t->keys[i].val.kind != TL_V_ARRAY_STRING) return 0;
            if (items) *items = (const char *const *)t->keys[i].val.arr;
            return t->keys[i].val.arr_count;
        }
    }
    return 0;
}

size_t tl_table_key_count(const tl_table *t) {
    return t ? t->key_count : 0;
}

const char *tl_table_key_name(const tl_table *t, size_t i) {
    if (!t || i >= t->key_count) return NULL;
    return t->keys[i].name;
}
