// CopyCatOS — by Kyle Blizzard at Blizzard.show

// bundle — on-disk .app / .appdev loader.
//
// The parser in info_appc.[ch] validates the shape of Info.appc; this
// layer takes a bundle directory on disk and runs the rest of the
// validation pipeline from bundle-spec.md §8: extension and directory
// check, minimum-moonbase gate, executable resolution, executable
// mode + setuid/setgid rejection, absolute-symlink scan. On success,
// the caller gets a struct with the absolute bundle path, the
// absolute resolved executable path, and the fully populated
// Info.appc — everything moonbase-launch needs to apply a sandbox
// profile and exec.
//
// Quarantine (§7) is intentionally out of scope here — that's its
// own slice (D.4) on top of this loader.

#ifndef MOONBASE_BUNDLE_BUNDLE_H
#define MOONBASE_BUNDLE_BUNDLE_H

#include "info_appc.h"

typedef enum {
    MB_BUNDLE_OK = 0,
    MB_BUNDLE_ERR_NOT_DIR,              // path not a directory
    MB_BUNDLE_ERR_BAD_SUFFIX,           // path doesn't end in .app or .appdev
    MB_BUNDLE_ERR_NO_INFO,              // Contents/Info.appc missing
    MB_BUNDLE_ERR_INFO_APPC,            // Info.appc failed validation
    MB_BUNDLE_ERR_API_VERSION,          // minimum-moonbase > library version
    MB_BUNDLE_ERR_EXEC_MISSING,         // [executable].path doesn't exist
    MB_BUNDLE_ERR_EXEC_ESCAPE,          // [executable].path escapes bundle
    MB_BUNDLE_ERR_EXEC_LOCATION,        // not under Contents/CopyCatOS/ or Contents/Resources/
    MB_BUNDLE_ERR_EXEC_MODE,            // not executable, or setuid/setgid set
    MB_BUNDLE_ERR_ABSOLUTE_SYMLINK,     // absolute symlink inside bundle
    MB_BUNDLE_ERR_OUTSIDE_CONTENTS,     // file at bundle root other than Contents/
    MB_BUNDLE_ERR_IO,                   // filesystem call failed
    MB_BUNDLE_ERR_NO_MEM,
} mb_bundle_err_t;

typedef struct {
    // Absolute canonical path to the bundle directory (realpath-resolved).
    char *bundle_path;
    // Absolute canonical path to the executable after resolving the
    // [executable].path entry relative to bundle_path.
    char *exe_abs_path;
    // Parsed, validated Info.appc.
    mb_info_appc_t info;
} mb_bundle_t;

// Load and validate a bundle directory. On success, *out is populated;
// caller frees via mb_bundle_free. On failure, err_buf gets a
// human-readable diagnostic and *out is left zeroed.
mb_bundle_err_t mb_bundle_load(const char *path, mb_bundle_t *out,
                               char *err_buf, size_t err_cap);

void mb_bundle_free(mb_bundle_t *b);

// Returns a short human-readable name for a bundle error code.
const char *mb_bundle_err_string(mb_bundle_err_t e);

#endif
