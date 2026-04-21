// CopyCatOS — by Kyle Blizzard at Blizzard.show

// bundle — on-disk .app / .appdev loader. Implements the pipeline from
// bundle-spec.md §8 except quarantine, which lives in its own slice.
//
// Transitional quad-suffix: during the rename from .appc/.appcd to the
// classic Snow Leopard .app (single-file) and .appdev (developer directory)
// scheme, we accept all four as directory bundles. Single-file .app
// (ELF stub + squashfs tail) lands in its own slice; until then .app is
// also treated as a directory. After the ABI freeze, .app becomes
// single-file only and .appdev is the one dev directory form. Legacy
// .appc/.appcd acceptance gets dropped at that point.

#include "bundle.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "moonbase.h"   // for MOONBASE_API_VERSION

// -------------------------------------------------------------------
// helpers
// -------------------------------------------------------------------

static void set_err(char *buf, size_t cap, const char *fmt, ...) {
    if (!buf || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
}

static int ends_with(const char *s, const char *suffix) {
    size_t ns = strlen(s), nsu = strlen(suffix);
    if (ns < nsu) return 0;
    return strcmp(s + ns - nsu, suffix) == 0;
}

// Parse "major.minor" into the MOONBASE_API_VERSION numeric form
// (MAJOR*10000 + MINOR*100). Returns -1 on parse failure.
static long parse_minimum_moonbase(const char *s) {
    if (!s) return -1;
    char *end = NULL;
    long major = strtol(s, &end, 10);
    if (end == s || *end != '.' || major < 0) return -1;
    const char *p = end + 1;
    char *end2 = NULL;
    long minor = strtol(p, &end2, 10);
    if (end2 == p || minor < 0) return -1;
    // Allow an optional patch component for forward-compatibility.
    if (*end2 == '.') {
        p = end2 + 1;
        char *end3 = NULL;
        long patch = strtol(p, &end3, 10);
        if (end3 == p || patch < 0) return -1;
        if (*end3 != '\0') return -1;
        return major * 10000 + minor * 100 + patch;
    }
    if (*end2 != '\0') return -1;
    return major * 10000 + minor * 100;
}

// Concatenate bundle_root + "/" + rel into dst. Returns 0 on success,
// -1 if the result would overflow PATH_MAX.
static int join_path(char *dst, size_t cap, const char *root, const char *rel) {
    int n = snprintf(dst, cap, "%s/%s", root, rel);
    if (n < 0 || (size_t)n >= cap) return -1;
    return 0;
}

// -------------------------------------------------------------------
// scan for absolute symlinks + setuid/setgid files (nftw walker)
// -------------------------------------------------------------------

// We use globals because nftw's callback signature has no user-data
// slot. The scan completes synchronously inside mb_bundle_load, so
// this doesn't become a threading hazard.
static _Thread_local int scan_failed;
// 2*PATH_MAX + slack so GCC's -Wformat-truncation can prove the two
// embedded paths fit no matter what the kernel gives us.
static _Thread_local char scan_err_msg[8320];

static int scan_walker(const char *path, const struct stat *st,
                       int tflag, struct FTW *ftwbuf) {
    (void)ftwbuf;
    if (tflag == FTW_SL || tflag == FTW_SLN) {
        char tgt[PATH_MAX];
        ssize_t n = readlink(path, tgt, sizeof(tgt) - 1);
        if (n < 0) {
            scan_failed = MB_BUNDLE_ERR_IO;
            snprintf(scan_err_msg, sizeof(scan_err_msg),
                     "readlink(%s): %s", path, strerror(errno));
            return 1;
        }
        tgt[n] = '\0';
        if (tgt[0] == '/') {
            scan_failed = MB_BUNDLE_ERR_ABSOLUTE_SYMLINK;
            snprintf(scan_err_msg, sizeof(scan_err_msg),
                     "absolute symlink inside bundle: %s -> %s", path, tgt);
            return 1;
        }
    } else if (tflag == FTW_F) {
        // setuid/setgid disallowed per bundle-spec §1.3.
        if (st->st_mode & (S_ISUID | S_ISGID)) {
            scan_failed = MB_BUNDLE_ERR_EXEC_MODE;
            snprintf(scan_err_msg, sizeof(scan_err_msg),
                     "setuid/setgid bit set on %s", path);
            return 1;
        }
    }
    return 0;
}

// -------------------------------------------------------------------
// main loader
// -------------------------------------------------------------------

mb_bundle_err_t mb_bundle_load(const char *path, mb_bundle_t *out,
                               char *err, size_t err_cap) {
    if (!out) return MB_BUNDLE_ERR_NO_MEM;
    memset(out, 0, sizeof(*out));

    if (!path || !*path) {
        set_err(err, err_cap, "bundle path is empty");
        return MB_BUNDLE_ERR_NOT_DIR;
    }
    if (!ends_with(path, ".app")  && !ends_with(path, ".appdev") &&
        !ends_with(path, ".appc") && !ends_with(path, ".appcd")) {
        set_err(err, err_cap,
                "bundle path does not end in .app, .appdev, .appc, or .appcd: %s",
                path);
        return MB_BUNDLE_ERR_BAD_SUFFIX;
    }

    // Canonicalize.
    char abs[PATH_MAX];
    if (!realpath(path, abs)) {
        set_err(err, err_cap, "realpath(%s): %s", path, strerror(errno));
        return MB_BUNDLE_ERR_IO;
    }
    struct stat bst;
    if (stat(abs, &bst) != 0) {
        set_err(err, err_cap, "stat(%s): %s", abs, strerror(errno));
        return MB_BUNDLE_ERR_IO;
    }
    if (!S_ISDIR(bst.st_mode)) {
        set_err(err, err_cap, "%s is not a directory", abs);
        return MB_BUNDLE_ERR_NOT_DIR;
    }

    // Enforce §1.3: no files outside Contents/ at the bundle root.
    DIR *d = opendir(abs);
    if (!d) {
        set_err(err, err_cap, "opendir(%s): %s", abs, strerror(errno));
        return MB_BUNDLE_ERR_IO;
    }
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *n = de->d_name;
        if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0) continue;
        if (strcmp(n, "Contents") != 0) {
            closedir(d);
            set_err(err, err_cap,
                    "unexpected entry at bundle root: %s/%s (only Contents/ allowed)",
                    abs, n);
            return MB_BUNDLE_ERR_OUTSIDE_CONTENTS;
        }
    }
    closedir(d);

    // Read Contents/Info.appc.
    char info_path[PATH_MAX];
    if (join_path(info_path, sizeof(info_path), abs, "Contents/Info.appc") != 0) {
        set_err(err, err_cap, "path too long: %s/Contents/Info.appc", abs);
        return MB_BUNDLE_ERR_IO;
    }
    if (access(info_path, R_OK) != 0) {
        set_err(err, err_cap, "missing or unreadable Contents/Info.appc: %s", strerror(errno));
        return MB_BUNDLE_ERR_NO_INFO;
    }
    char infoerr[256] = {0};
    mb_info_appc_err_t ie = mb_info_appc_parse_file(info_path, &out->info,
                                                    infoerr, sizeof(infoerr));
    if (ie != MB_INFO_APPC_OK) {
        set_err(err, err_cap, "Info.appc: %s (%s)", infoerr,
                mb_info_appc_err_string(ie));
        return MB_BUNDLE_ERR_INFO_APPC;
    }

    // minimum-moonbase gate.
    long needed = parse_minimum_moonbase(out->info.minimum_moonbase);
    if (needed < 0) {
        mb_info_appc_free(&out->info);
        set_err(err, err_cap, "[bundle].minimum-moonbase = \"%s\" is not major.minor",
                out->info.minimum_moonbase);
        return MB_BUNDLE_ERR_API_VERSION;
    }
    if (needed > MOONBASE_API_VERSION) {
        mb_info_appc_free(&out->info);
        set_err(err, err_cap,
                "[bundle].minimum-moonbase = \"%s\" (%ld) > library %d",
                out->info.minimum_moonbase, needed, MOONBASE_API_VERSION);
        return MB_BUNDLE_ERR_API_VERSION;
    }

    // Executable path — must resolve inside bundle and under
    // Contents/CopyCatOS/ (canonical), Contents/MacOS/ (legacy soft-transition),
    // or Contents/Resources/.
    char exe_full[PATH_MAX];
    if (join_path(exe_full, sizeof(exe_full), abs, out->info.exec_path) != 0) {
        mb_info_appc_free(&out->info);
        set_err(err, err_cap, "[executable].path too long: %s", out->info.exec_path);
        return MB_BUNDLE_ERR_IO;
    }
    char exe_abs[PATH_MAX];
    if (!realpath(exe_full, exe_abs)) {
        mb_info_appc_free(&out->info);
        set_err(err, err_cap, "[executable].path does not resolve: %s (%s)",
                exe_full, strerror(errno));
        return MB_BUNDLE_ERR_EXEC_MISSING;
    }
    // Escape check — exe_abs must be a prefix-path of abs + '/'.
    size_t abs_len = strlen(abs);
    if (strncmp(exe_abs, abs, abs_len) != 0 || exe_abs[abs_len] != '/') {
        mb_info_appc_free(&out->info);
        set_err(err, err_cap,
                "[executable].path escapes bundle: %s resolves to %s (outside %s)",
                out->info.exec_path, exe_abs, abs);
        return MB_BUNDLE_ERR_EXEC_ESCAPE;
    }
    // Location check — must be under Contents/CopyCatOS/ (canonical),
    // Contents/MacOS/ (legacy soft-transition), or Contents/Resources/.
    const char *rel = exe_abs + abs_len + 1;
    if (strncmp(rel, "Contents/CopyCatOS/", 19) != 0
        && strncmp(rel, "Contents/MacOS/", 15) != 0
        && strncmp(rel, "Contents/Resources/", 19) != 0) {
        mb_info_appc_free(&out->info);
        set_err(err, err_cap,
                "[executable].path must live under Contents/CopyCatOS/ or Contents/Resources/, not %s",
                rel);
        return MB_BUNDLE_ERR_EXEC_LOCATION;
    }
    // Mode check.
    struct stat est;
    if (stat(exe_abs, &est) != 0) {
        mb_info_appc_free(&out->info);
        set_err(err, err_cap, "stat(%s): %s", exe_abs, strerror(errno));
        return MB_BUNDLE_ERR_IO;
    }
    if (!S_ISREG(est.st_mode)) {
        mb_info_appc_free(&out->info);
        set_err(err, err_cap, "executable %s is not a regular file", exe_abs);
        return MB_BUNDLE_ERR_EXEC_MODE;
    }
    if ((est.st_mode & 0111) == 0) {
        mb_info_appc_free(&out->info);
        set_err(err, err_cap, "executable %s has no execute bit", exe_abs);
        return MB_BUNDLE_ERR_EXEC_MODE;
    }
    if (est.st_mode & (S_ISUID | S_ISGID)) {
        mb_info_appc_free(&out->info);
        set_err(err, err_cap, "executable %s has setuid/setgid bit set", exe_abs);
        return MB_BUNDLE_ERR_EXEC_MODE;
    }

    // Scan for absolute symlinks and setuid/setgid files across the tree.
    scan_failed = 0;
    scan_err_msg[0] = '\0';
    int nrc = nftw(abs, scan_walker, 32, FTW_PHYS);
    if (nrc < 0) {
        mb_info_appc_free(&out->info);
        set_err(err, err_cap, "nftw(%s): %s", abs, strerror(errno));
        return MB_BUNDLE_ERR_IO;
    }
    if (scan_failed) {
        mb_bundle_err_t e = (mb_bundle_err_t)scan_failed;
        mb_info_appc_free(&out->info);
        set_err(err, err_cap, "%s", scan_err_msg);
        return e;
    }

    // Web-lang sanity — main-url must be https. The parser already
    // required [web].main-url to be a string; this is the §8.8 check.
    if (out->info.lang == MB_INFO_APPC_LANG_WEB) {
        const char *u = out->info.web_main_url;
        if (!u || strncmp(u, "https://", 8) != 0) {
            mb_info_appc_free(&out->info);
            set_err(err, err_cap,
                    "[web].main-url must be an https:// URL, got %s", u ? u : "(null)");
            return MB_BUNDLE_ERR_INFO_APPC;
        }
    }

    // All checks passed.
    out->bundle_path = strdup(abs);
    out->exe_abs_path = strdup(exe_abs);
    if (!out->bundle_path || !out->exe_abs_path) {
        mb_bundle_free(out);
        set_err(err, err_cap, "out of memory");
        return MB_BUNDLE_ERR_NO_MEM;
    }
    return MB_BUNDLE_OK;
}

void mb_bundle_free(mb_bundle_t *b) {
    if (!b) return;
    free(b->bundle_path);
    free(b->exe_abs_path);
    mb_info_appc_free(&b->info);
    memset(b, 0, sizeof(*b));
}

const char *mb_bundle_err_string(mb_bundle_err_t e) {
    switch (e) {
        case MB_BUNDLE_OK:                  return "ok";
        case MB_BUNDLE_ERR_NOT_DIR:         return "not-dir";
        case MB_BUNDLE_ERR_BAD_SUFFIX:      return "bad-suffix";
        case MB_BUNDLE_ERR_NO_INFO:         return "no-info";
        case MB_BUNDLE_ERR_INFO_APPC:       return "info-appc";
        case MB_BUNDLE_ERR_API_VERSION:     return "api-version";
        case MB_BUNDLE_ERR_EXEC_MISSING:    return "exec-missing";
        case MB_BUNDLE_ERR_EXEC_ESCAPE:     return "exec-escape";
        case MB_BUNDLE_ERR_EXEC_LOCATION:   return "exec-location";
        case MB_BUNDLE_ERR_EXEC_MODE:       return "exec-mode";
        case MB_BUNDLE_ERR_ABSOLUTE_SYMLINK:return "absolute-symlink";
        case MB_BUNDLE_ERR_OUTSIDE_CONTENTS:return "outside-contents";
        case MB_BUNDLE_ERR_IO:              return "io";
        case MB_BUNDLE_ERR_NO_MEM:          return "no-mem";
    }
    return "unknown";
}
