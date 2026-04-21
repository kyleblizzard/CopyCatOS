// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase_keychain.h — secure secret storage.
//
// Aqua-feeling facade over libsecret (Secret Service D-Bus API). Apps
// store, fetch, list, and delete generic passwords; generate strong
// passwords; and free returned secrets via a helper that zeroes the
// buffer before releasing it.
//
// Items are keyed by a flat tuple:
//   (service, label, account)
// where `service` is normally the bundle-id of the app that wrote the
// item (pass NULL to use the caller's bundle-id), `label` is the human
// name shown in Keychain Access, and `account` is the user-visible
// identifier (email, handle, etc.). All three together uniquely
// identify an item; passing NULL for any of them on fetch/delete/list
// matches every value for that field.
//
// Entitlement: `system:"keychain"` in Info.appc. Enforced at the
// libmoonbase API boundary — every call below returns MB_EPERM
// unless the bundle declared it. The D-Bus session socket itself
// is still reachable from every native-tier sandbox because the
// launcher binds $XDG_RUNTIME_DIR in; a sandboxed xdg-dbus-proxy
// filter is the runtime-fence follow-up.
//
// API stability: MOONBASE_KEYCHAIN_API_VERSION is 0 (unstable) until
// all four reference apps ship end-to-end. Signatures may change
// during that window. Once libmoonbase.so.1 is tagged public-SDK the
// surface graduates to 10000 and becomes add-only within the major.

#ifndef MOONBASE_KEYCHAIN_H
#define MOONBASE_KEYCHAIN_H

#include <moonbase.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// MOONBASE_KEYCHAIN_API_VERSION follows the same MAJOR*10000 + MINOR*100
// + PATCH encoding as MOONBASE_API_VERSION. It evolves on its own axis
// so a keychain feature bump does not force a core-ABI bump.
//
// 0 = unstable pre-graduation surface. Do not assume ABI stability.
#define MOONBASE_KEYCHAIN_API_VERSION 0

// ---------------------------------------------------------------------
// Listing
// ---------------------------------------------------------------------
//
// A `mb_keychain_list_t` is an opaque, owned snapshot of every item
// visible to the caller at the moment the list was taken. The caller
// releases it with moonbase_keychain_list_free. Secrets are never
// included in a list — use moonbase_keychain_fetch for the value.

typedef struct mb_keychain_list mb_keychain_list_t;

// Take a snapshot of every generic-password item the caller can see.
// On success *out receives an owned list; free with
// moonbase_keychain_list_free. Returns MB_EOK or a negative mb_error_t.
mb_error_t moonbase_keychain_list(mb_keychain_list_t **out);

// Number of entries in a list. Safe to call on NULL (returns 0).
size_t moonbase_keychain_list_count(const mb_keychain_list_t *list);

// Read-only accessors. The returned pointers are owned by the list
// and valid until the list is freed. Indexing out of range returns
// NULL. Values may be NULL for items that did not record that field.
const char *moonbase_keychain_list_service(const mb_keychain_list_t *list, size_t i);
const char *moonbase_keychain_list_label  (const mb_keychain_list_t *list, size_t i);
const char *moonbase_keychain_list_account(const mb_keychain_list_t *list, size_t i);

// Release a list and all strings it owns. Safe to call on NULL.
void moonbase_keychain_list_free(mb_keychain_list_t *list);

// ---------------------------------------------------------------------
// CRUD
// ---------------------------------------------------------------------

// Store or replace a generic password.
//
// Passing NULL for `service` uses the caller's bundle-id, which is the
// normal case — apps silo their own items under their own id. Passing
// an explicit service lets system tools (Keychain Access itself) edit
// items that belong to other apps.
//
// `label` and `account` must be non-NULL, non-empty UTF-8 strings.
// `secret` must be non-NULL; the byte "\0" terminates it.
//
// Storing the same (service, label, account) tuple twice overwrites
// the previous secret in place.
mb_error_t moonbase_keychain_store(const char *service,
                                   const char *label,
                                   const char *account,
                                   const char *secret);

// Fetch the secret for a specific (service, label, account) tuple.
//
// On success *out_secret receives a heap-allocated NUL-terminated
// string; free it with moonbase_keychain_secret_free so the bytes get
// zeroed before release. On MB_ENOTFOUND *out_secret is set to NULL.
//
// NULL for `service` means the caller's bundle-id. `label` and
// `account` must both be non-NULL.
mb_error_t moonbase_keychain_fetch(const char *service,
                                   const char *label,
                                   const char *account,
                                   char **out_secret);

// Delete a specific (service, label, account) tuple. Returns MB_EOK
// on success, MB_ENOTFOUND if nothing matched. NULL for `service`
// means the caller's bundle-id.
mb_error_t moonbase_keychain_delete(const char *service,
                                    const char *label,
                                    const char *account);

// ---------------------------------------------------------------------
// Password generation
// ---------------------------------------------------------------------

// Character-class flags for moonbase_keychain_generate_password.
// Combine with bitwise OR. Passing 0 is an error; generated passwords
// must draw from at least one class.
typedef enum mb_keychain_pw_flags {
    MB_KEYCHAIN_PW_LOWER   = 1 << 0,  // a-z
    MB_KEYCHAIN_PW_UPPER   = 1 << 1,  // A-Z
    MB_KEYCHAIN_PW_DIGITS  = 1 << 2,  // 0-9
    MB_KEYCHAIN_PW_SYMBOLS = 1 << 3,  // !@#$%^&*()-_=+[]{};:,.<>/?
} mb_keychain_pw_flags_t;

// A sensible default: every class, the way Keychain Access does it.
#define MB_KEYCHAIN_PW_ALL \
    (MB_KEYCHAIN_PW_LOWER  | MB_KEYCHAIN_PW_UPPER | \
     MB_KEYCHAIN_PW_DIGITS | MB_KEYCHAIN_PW_SYMBOLS)

// Generate a cryptographically strong password of exactly `length`
// characters drawing only from the classes selected by `flags`.
// Minimum length is 1; reasonable lengths are 12–64.
//
// On success *out receives a heap-allocated NUL-terminated string;
// release it with moonbase_keychain_secret_free.
mb_error_t moonbase_keychain_generate_password(int length,
                                               int flags,
                                               char **out);

// ---------------------------------------------------------------------
// Secret lifetime
// ---------------------------------------------------------------------

// Zero the buffer, then free it. The right call for any string that
// moonbase_keychain_fetch or moonbase_keychain_generate_password
// handed back. Safe to call on NULL.
void moonbase_keychain_secret_free(char *secret);

#ifdef __cplusplus
}
#endif

#endif // MOONBASE_KEYCHAIN_H
