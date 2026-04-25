// CopyCatOS — by Kyle Blizzard at Blizzard.show

// info_appc — schema-aware loader for Info.appc.
//
// Reads and validates the TOML schema documented in
// moonbase/docs/info_appc_schema.md. Rejects anything ambiguous enough
// that a real launcher would have to guess. Enforces the 64 KiB cap
// from the schema doc.
//
// Scope for this header is the *schema* — the parsed, validated,
// structurally-sound view of Info.appc. Disk-resolution of the
// executable (Contents/CopyCatOS/ vs Contents/Resources/) lives in the
// bundle loader in a later slice. Keeping the two separate lets unit
// tests parse Info.appc from memory buffers without ever touching the
// filesystem.

#ifndef MOONBASE_BUNDLE_INFO_APPC_H
#define MOONBASE_BUNDLE_INFO_APPC_H

#include <stddef.h>

// 64 KiB cap from info_appc_schema.md.
#define MB_INFO_APPC_MAX_BYTES (64u * 1024u)

typedef enum {
    MB_INFO_APPC_LANG_C,
    MB_INFO_APPC_LANG_WEB,
    MB_INFO_APPC_LANG_PYTHON,
    MB_INFO_APPC_LANG_RUST,
    MB_INFO_APPC_LANG_SWIFT,
} mb_info_appc_lang_t;

typedef enum {
    MB_INFO_APPC_RENDER_DEFAULT,  // follow language default
    MB_INFO_APPC_RENDER_CAIRO,
    MB_INFO_APPC_RENDER_GL,
} mb_info_appc_render_t;

// [wrap].toolkit — hint that lets moonbase-launch stamp the bundle id onto
// the resulting top-level X window's WM_CLASS instance. Default NATIVE means
// the bundle binary is a real MoonBase app and the chrome stub will find
// it via IPC handshake; the other values cover Legacy Mode bundles that
// wrap an unmodified Qt or GTK binary.
typedef enum {
    MB_INFO_APPC_WRAP_NATIVE = 0,
    MB_INFO_APPC_WRAP_QT5,
    MB_INFO_APPC_WRAP_QT6,
    MB_INFO_APPC_WRAP_GTK3,
    MB_INFO_APPC_WRAP_GTK4,
} mb_info_appc_wrap_toolkit_t;

typedef enum {
    MB_INFO_APPC_OK = 0,
    MB_INFO_APPC_ERR_TOO_LARGE,         // > MB_INFO_APPC_MAX_BYTES
    MB_INFO_APPC_ERR_PARSE,             // TOML syntax error
    MB_INFO_APPC_ERR_MISSING_REQUIRED,  // missing required table or key
    MB_INFO_APPC_ERR_BAD_VALUE,         // wrong type, bad id regex, etc.
    MB_INFO_APPC_ERR_UNKNOWN_VALUE,     // enum key with value outside allowlist
    MB_INFO_APPC_ERR_NO_MEM,
} mb_info_appc_err_t;

typedef struct {
    // [bundle]
    char *id;                            // required
    char *name;                          // required
    char *version;                       // required
    char *minimum_moonbase;              // required, "major.minor"
    char *copyright;                     // optional
    char *category;                      // optional, allowlist-checked

    // [executable]
    char *exec_path;                     // required, relative
    mb_info_appc_lang_t lang;            // required
    mb_info_appc_render_t render_default; // default follows lang

    // [permissions] — each sub-array is NULL-terminated for convenience;
    // count fields carry the length too.
    char **perm_filesystem; size_t perm_filesystem_count;
    char **perm_network;    size_t perm_network_count;
    char **perm_hardware;   size_t perm_hardware_count;
    char **perm_system;     size_t perm_system_count;
    char **perm_ipc;        size_t perm_ipc_count;

    // [web] — required iff lang == WEB
    char *web_main_url;
    char *web_manifest_url;
    char **web_allowed_origins; size_t web_allowed_origins_count;

    // [localization]
    char *base_locale;
    char **supported_locales; size_t supported_locales_count;

    // [update]
    char *update_url;
    char *update_channel;                // stable | beta

    // [wrap] — Legacy Mode toolkit hint. NATIVE when omitted.
    mb_info_appc_wrap_toolkit_t wrap_toolkit;
} mb_info_appc_t;

// Parse an Info.appc buffer. Result on success is a fully populated,
// validated mb_info_appc_t. err_buf gets a human-readable message on
// any failure.
mb_info_appc_err_t mb_info_appc_parse_buffer(const char *src, size_t len,
                                             mb_info_appc_t *out,
                                             char *err_buf, size_t err_cap);

// Read Info.appc from disk, enforcing the size cap before parsing.
mb_info_appc_err_t mb_info_appc_parse_file(const char *path,
                                           mb_info_appc_t *out,
                                           char *err_buf, size_t err_cap);

// Release all allocations owned by a successfully-parsed info struct.
// Safe on a zeroed struct (no-op).
void mb_info_appc_free(mb_info_appc_t *info);

// Returns a short human-readable name for a given error code, or
// "unknown" if the code is unrecognised. Never NULL.
const char *mb_info_appc_err_string(mb_info_appc_err_t e);

#endif
