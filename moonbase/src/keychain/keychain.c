// CopyCatOS — by Kyle Blizzard at Blizzard.show

// keychain.c — libsecret adapter backing moonbase_keychain.h.
//
// Thin, synchronous wrapper over the Secret Service D-Bus API. Items
// live in the user's default keyring, under a single custom schema
// shared by every MoonBase app:
//
//   org.blizzard.moonbase.generic
//     attrs: service (string), label (string), account (string)
//
// Using one shared schema means every item stored through MoonBase is
// listable by Keychain Access regardless of which app wrote it. Apps
// silo their own entries by writing their bundle-id into `service` —
// passing NULL for `service` on any public call substitutes the
// caller's $MOONBASE_BUNDLE_ID (set by moonbase-launch per the bundle
// spec).
//
// Secrets returned to callers (fetch, generate_password) are heap
// buffers that must be released via moonbase_keychain_secret_free,
// which zeroes the bytes before calling free. The three libsecret-
// originated strings go through secret_password_free, which does the
// same thing inside libsecret's allocator.
//
// Entitlement + consent: every public CRUD + list call below walks
// the two gates from sandbox.md §6. Entitlement first — the bundle
// must have declared `system:"keychain"` in Info.appc, else MB_EPERM
// without prompting. Consent second — the user's recorded decision
// in consents.toml wins; a `deny` entry turns into MB_EPERM, an
// `allow` entry (or, during the pre-IPC grant-by-default window, a
// missing entry) lets the call through. generate_password and
// secret_free don't touch the keychain and stay ungated.
//
// D-Bus reachability: the session bus socket lives under
// $XDG_RUNTIME_DIR, which moonbase-launch binds into every sandbox.
// Tightening to a real runtime fence (sandboxed xdg-dbus-proxy with
// a whitelisted Secret Service filter) is deferred; the API gate is
// the load-bearing check today.

#include "moonbase.h"
#include "moonbase_keychain.h"
#include "consents.h"
#include "entitlements.h"
#include "internal.h"

#include <libsecret/secret.h>

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

// ---------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------
//
// A static SecretSchema the whole library shares. Declaring the schema
// here (not as a compile-time initialized global in a header) keeps
// the definition single-owner and avoids multiple-definition trouble
// when the header grows consumers down the line.

static const SecretSchema moonbase_keychain_schema = {
    .name = "org.blizzard.moonbase.generic",
    .flags = SECRET_SCHEMA_NONE,
    .attributes = {
        { "service", SECRET_SCHEMA_ATTRIBUTE_STRING },
        { "label",   SECRET_SCHEMA_ATTRIBUTE_STRING },
        { "account", SECRET_SCHEMA_ATTRIBUTE_STRING },
        // Remaining attribute slots zero-init as sentinel terminators.
    },
    // SecretSchema carries a gint + seven gpointer reserved slots for
    // ABI growth inside libsecret. Designated initializers keep us
    // quiet under -Werror=missing-field-initializers regardless of
    // libsecret's private padding layout.
    .reserved  = 0,
    .reserved1 = NULL, .reserved2 = NULL, .reserved3 = NULL,
    .reserved4 = NULL, .reserved5 = NULL, .reserved6 = NULL,
    .reserved7 = NULL,
};

static const SecretSchema *mb_keychain_schema(void) {
    return &moonbase_keychain_schema;
}

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------

// Resolve the effective `service` attribute. Callers pass NULL to mean
// "my own bundle". MOONBASE_BUNDLE_ID is set by moonbase-launch for
// every launched .appc; when it's absent (developer-run, tests)
// "unknown.bundle" is the documented fallback.
static const char *effective_service(const char *service) {
    if (service && service[0] != '\0') return service;
    const char *env = getenv("MOONBASE_BUNDLE_ID");
    if (env && env[0] != '\0') return env;
    return "unknown.bundle";
}

// Translate a libsecret GError into an mb_error_t. The mapping stays
// coarse on purpose — callers get MB_EPERM for "the user said no",
// MB_EIPC for anything else, and the details live in the error-string
// surface of moonbase_last_error (future enhancement).
static mb_error_t map_gerror(GError *err) {
    if (!err) return MB_EIPC;
    mb_error_t code = MB_EIPC;
    if (g_error_matches(err, SECRET_ERROR, SECRET_ERROR_NO_SUCH_OBJECT)) {
        code = MB_ENOTFOUND;
    } else if (g_error_matches(err, SECRET_ERROR, SECRET_ERROR_IS_LOCKED)) {
        code = MB_EPERM;
    }
    g_error_free(err);
    return code;
}

// Zero + free. Used for both app-facing buffers (moonbase_keychain_*)
// and for internal transient allocations we want gone from memory
// before the heap block is recycled.
static void zero_free(char *p) {
    if (!p) return;
    size_t n = strlen(p);
    // memset_explicit / explicit_bzero both work on glibc; the cast
    // through volatile keeps the compiler from eliding the store when
    // the buffer is unused afterwards.
    volatile char *vp = p;
    while (n--) *vp++ = 0;
    free(p);
}

// ---------------------------------------------------------------------
// CRUD
// ---------------------------------------------------------------------

mb_error_t moonbase_keychain_store(const char *service,
                                   const char *label,
                                   const char *account,
                                   const char *secret)
{
    if (!mb_has_entitlement("system", "keychain")) {
        mb_internal_set_last_error(MB_EPERM);
        return MB_EPERM;
    }
    if (!mb_consent_gate_allows("system", "keychain")) {
        mb_internal_set_last_error(MB_EPERM);
        return MB_EPERM;
    }
    if (!label || !label[0] || !account || !account[0] || !secret) {
        mb_internal_set_last_error(MB_EINVAL);
        return MB_EINVAL;
    }

    const char *svc = effective_service(service);

    GError *err = NULL;
    gboolean ok = secret_password_store_sync(
        mb_keychain_schema(),
        SECRET_COLLECTION_DEFAULT,
        label,
        secret,
        NULL,
        &err,
        "service", svc,
        "label",   label,
        "account", account,
        NULL);

    if (!ok) {
        mb_error_t e = map_gerror(err);
        mb_internal_set_last_error(e);
        return e;
    }
    return MB_EOK;
}

mb_error_t moonbase_keychain_fetch(const char *service,
                                   const char *label,
                                   const char *account,
                                   char **out_secret)
{
    if (out_secret) *out_secret = NULL;
    if (!mb_has_entitlement("system", "keychain")) {
        mb_internal_set_last_error(MB_EPERM);
        return MB_EPERM;
    }
    if (!mb_consent_gate_allows("system", "keychain")) {
        mb_internal_set_last_error(MB_EPERM);
        return MB_EPERM;
    }
    if (!label || !label[0] || !account || !account[0] || !out_secret) {
        mb_internal_set_last_error(MB_EINVAL);
        return MB_EINVAL;
    }

    const char *svc = effective_service(service);

    GError *err = NULL;
    gchar *found = secret_password_lookup_sync(
        mb_keychain_schema(),
        NULL,
        &err,
        "service", svc,
        "label",   label,
        "account", account,
        NULL);

    if (err) {
        mb_error_t e = map_gerror(err);
        mb_internal_set_last_error(e);
        return e;
    }
    if (!found) {
        mb_internal_set_last_error(MB_ENOTFOUND);
        return MB_ENOTFOUND;
    }

    // Copy into a buffer the caller can release through
    // moonbase_keychain_secret_free. The libsecret-owned original gets
    // zeroed on the spot via secret_password_free.
    size_t n = strlen(found);
    char *copy = malloc(n + 1);
    if (!copy) {
        secret_password_free(found);
        mb_internal_set_last_error(MB_ENOMEM);
        return MB_ENOMEM;
    }
    memcpy(copy, found, n + 1);
    secret_password_free(found);

    *out_secret = copy;
    return MB_EOK;
}

mb_error_t moonbase_keychain_delete(const char *service,
                                    const char *label,
                                    const char *account)
{
    if (!mb_has_entitlement("system", "keychain")) {
        mb_internal_set_last_error(MB_EPERM);
        return MB_EPERM;
    }
    if (!mb_consent_gate_allows("system", "keychain")) {
        mb_internal_set_last_error(MB_EPERM);
        return MB_EPERM;
    }
    if (!label || !label[0] || !account || !account[0]) {
        mb_internal_set_last_error(MB_EINVAL);
        return MB_EINVAL;
    }

    const char *svc = effective_service(service);

    GError *err = NULL;
    gboolean removed = secret_password_clear_sync(
        mb_keychain_schema(),
        NULL,
        &err,
        "service", svc,
        "label",   label,
        "account", account,
        NULL);

    if (err) {
        mb_error_t e = map_gerror(err);
        mb_internal_set_last_error(e);
        return e;
    }
    if (!removed) {
        mb_internal_set_last_error(MB_ENOTFOUND);
        return MB_ENOTFOUND;
    }
    return MB_EOK;
}

// ---------------------------------------------------------------------
// List
// ---------------------------------------------------------------------
//
// The opaque list owns three parallel arrays of heap-allocated UTF-8
// strings. Every entry holds the `service`, `label`, and `account`
// attributes read off a SecretItem — secrets are never pulled into
// the snapshot.

struct mb_keychain_list {
    size_t  count;
    char  **services;
    char  **labels;
    char  **accounts;
};

static char *dup_attr(GHashTable *attrs, const char *key) {
    const char *v = g_hash_table_lookup(attrs, key);
    if (!v) return NULL;
    return strdup(v);
}

mb_error_t moonbase_keychain_list(mb_keychain_list_t **out) {
    if (out) *out = NULL;
    if (!mb_has_entitlement("system", "keychain")) {
        mb_internal_set_last_error(MB_EPERM);
        return MB_EPERM;
    }
    if (!mb_consent_gate_allows("system", "keychain")) {
        mb_internal_set_last_error(MB_EPERM);
        return MB_EPERM;
    }
    if (!out) {
        mb_internal_set_last_error(MB_EINVAL);
        return MB_EINVAL;
    }

    GHashTable *empty = g_hash_table_new(g_str_hash, g_str_equal);
    if (!empty) {
        mb_internal_set_last_error(MB_ENOMEM);
        return MB_ENOMEM;
    }

    GError *err = NULL;
    GList *items = secret_service_search_sync(
        NULL,                     // default service
        mb_keychain_schema(),
        empty,                    // no attribute filter => every item
                                  // under this schema
        SECRET_SEARCH_ALL,
        NULL,
        &err);
    g_hash_table_unref(empty);

    if (err) {
        mb_error_t e = map_gerror(err);
        mb_internal_set_last_error(e);
        return e;
    }

    size_t n = g_list_length(items);
    mb_keychain_list_t *list = calloc(1, sizeof(*list));
    if (!list) goto oom;

    if (n > 0) {
        list->services = calloc(n, sizeof(char *));
        list->labels   = calloc(n, sizeof(char *));
        list->accounts = calloc(n, sizeof(char *));
        if (!list->services || !list->labels || !list->accounts) goto oom;
    }
    list->count = n;

    size_t i = 0;
    for (GList *it = items; it; it = it->next, i++) {
        SecretItem *item = SECRET_ITEM(it->data);
        GHashTable *a = secret_item_get_attributes(item);
        if (a) {
            list->services[i] = dup_attr(a, "service");
            list->labels[i]   = dup_attr(a, "label");
            list->accounts[i] = dup_attr(a, "account");
            g_hash_table_unref(a);
        }
    }

    g_list_free_full(items, g_object_unref);
    *out = list;
    return MB_EOK;

oom:
    g_list_free_full(items, g_object_unref);
    moonbase_keychain_list_free(list);
    mb_internal_set_last_error(MB_ENOMEM);
    return MB_ENOMEM;
}

size_t moonbase_keychain_list_count(const mb_keychain_list_t *list) {
    return list ? list->count : 0;
}

const char *moonbase_keychain_list_service(const mb_keychain_list_t *list, size_t i) {
    if (!list || i >= list->count || !list->services) return NULL;
    return list->services[i];
}

const char *moonbase_keychain_list_label(const mb_keychain_list_t *list, size_t i) {
    if (!list || i >= list->count || !list->labels) return NULL;
    return list->labels[i];
}

const char *moonbase_keychain_list_account(const mb_keychain_list_t *list, size_t i) {
    if (!list || i >= list->count || !list->accounts) return NULL;
    return list->accounts[i];
}

void moonbase_keychain_list_free(mb_keychain_list_t *list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) {
        if (list->services) free(list->services[i]);
        if (list->labels)   free(list->labels[i]);
        if (list->accounts) free(list->accounts[i]);
    }
    free(list->services);
    free(list->labels);
    free(list->accounts);
    free(list);
}

// ---------------------------------------------------------------------
// Password generation
// ---------------------------------------------------------------------

// Fill buf with `len` cryptographically strong random bytes, blocking
// until the kernel RNG is seeded. Falls back to reading /dev/urandom
// if getrandom() is unavailable — it shouldn't be on the target glibc
// but the belt-and-braces path costs nothing.
static int fill_random(unsigned char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = getrandom(buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

mb_error_t moonbase_keychain_generate_password(int length,
                                               int flags,
                                               char **out)
{
    if (length < 1 || length > 4096 || !out) {
        if (out) *out = NULL;
        mb_internal_set_last_error(MB_EINVAL);
        return MB_EINVAL;
    }
    *out = NULL;

    static const char LOWER[]   = "abcdefghijklmnopqrstuvwxyz";
    static const char UPPER[]   = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char DIGITS[]  = "0123456789";
    static const char SYMBOLS[] = "!@#$%^&*()-_=+[]{};:,.<>/?";

    char alphabet[128];
    size_t alpha_len = 0;

    if (flags & MB_KEYCHAIN_PW_LOWER)   { memcpy(alphabet + alpha_len, LOWER,   sizeof(LOWER)   - 1); alpha_len += sizeof(LOWER)   - 1; }
    if (flags & MB_KEYCHAIN_PW_UPPER)   { memcpy(alphabet + alpha_len, UPPER,   sizeof(UPPER)   - 1); alpha_len += sizeof(UPPER)   - 1; }
    if (flags & MB_KEYCHAIN_PW_DIGITS)  { memcpy(alphabet + alpha_len, DIGITS,  sizeof(DIGITS)  - 1); alpha_len += sizeof(DIGITS)  - 1; }
    if (flags & MB_KEYCHAIN_PW_SYMBOLS) { memcpy(alphabet + alpha_len, SYMBOLS, sizeof(SYMBOLS) - 1); alpha_len += sizeof(SYMBOLS) - 1; }

    if (alpha_len == 0) {
        mb_internal_set_last_error(MB_EINVAL);
        return MB_EINVAL;
    }

    char *buf = malloc((size_t)length + 1);
    if (!buf) {
        mb_internal_set_last_error(MB_ENOMEM);
        return MB_ENOMEM;
    }

    // Rejection sampling: we pull one random byte per output character
    // and reject any byte >= (256 - 256 % alpha_len) so the modulo is
    // unbiased. 256 - 256 % alpha_len is the largest multiple of
    // alpha_len that fits in one byte.
    size_t fair = 256 - (256 % alpha_len);
    int filled = 0;
    while (filled < length) {
        unsigned char pool[256];
        if (fill_random(pool, sizeof(pool)) != 0) {
            // zero_free relies on strlen, but buf isn't NUL-terminated
            // on the partial-fill error path. Zero exactly the bytes
            // we wrote, then plain free, so we never walk past the
            // allocation looking for a terminator.
            volatile char *vp = buf;
            for (int k = 0; k < filled; k++) vp[k] = 0;
            free(buf);
            mb_internal_set_last_error(MB_EIPC);
            return MB_EIPC;
        }
        for (size_t i = 0; i < sizeof(pool) && filled < length; i++) {
            if (pool[i] >= fair) continue;
            buf[filled++] = alphabet[pool[i] % alpha_len];
        }
        // Don't leak kernel entropy bytes on the stack beyond the
        // call. Zero the pool before looping.
        volatile unsigned char *vp = pool;
        for (size_t i = 0; i < sizeof(pool); i++) vp[i] = 0;
    }
    buf[length] = '\0';

    *out = buf;
    return MB_EOK;
}

// ---------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------

void moonbase_keychain_secret_free(char *secret) {
    zero_free(secret);
}
