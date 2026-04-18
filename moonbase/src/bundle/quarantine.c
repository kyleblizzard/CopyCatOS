// CopyCatOS — by Kyle Blizzard at Blizzard.show

// quarantine — xattr-based first-launch trust for .appc bundles.
// See quarantine.h for the state machine.

#include "quarantine.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

static const char XATTR_NAME[] = "user.moonbase.quarantine";

mb_quarantine_status_t mb_quarantine_check(const char *bundle_path) {
    if (!bundle_path) return MB_QUARANTINE_ERR_IO;

    struct stat st;
    if (stat(bundle_path, &st) != 0) return MB_QUARANTINE_ERR_IO;
    if (!S_ISDIR(st.st_mode)) return MB_QUARANTINE_ERR_IO;

    // Sized generously — real values are "approved" / "pending" /
    // "rejected". Anything larger is malformed.
    char buf[32] = {0};
    ssize_t n = getxattr(bundle_path, XATTR_NAME, buf, sizeof(buf) - 1);
    if (n < 0) {
        // ENOTSUP       — filesystem can't store xattrs (FAT32, exFAT)
        // EOPNOTSUPP    — glibc spells ENOTSUP this way on some kernels
        // ENODATA       — attr never set on this path → local / packaged
        //                 build → trusted
        if (errno == ENOTSUP
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
            || errno == EOPNOTSUPP
#endif
        ) {
            return MB_QUARANTINE_NO_XATTR;
        }
        if (errno == ENODATA) return MB_QUARANTINE_APPROVED;
        return MB_QUARANTINE_ERR_IO;
    }
    // Trim trailing newlines/whitespace in case a setfattr -v command
    // line left one behind.
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                     buf[n - 1] == ' '  || buf[n - 1] == '\t')) {
        buf[--n] = '\0';
    }

    if (strcmp(buf, "approved") == 0) return MB_QUARANTINE_APPROVED;
    if (strcmp(buf, "pending")  == 0) return MB_QUARANTINE_PENDING;
    if (strcmp(buf, "rejected") == 0) return MB_QUARANTINE_REJECTED;
    return MB_QUARANTINE_MALFORMED;
}

static int write_status(const char *bundle_path, const char *value) {
    if (!bundle_path || !value) { errno = EINVAL; return -1; }
    size_t len = strlen(value);
    // XATTR_REPLACE isn't right either — the attr may not exist yet.
    // 0 = create or replace, which is exactly the semantic we want.
    if (setxattr(bundle_path, XATTR_NAME, value, len, 0) != 0) return -1;
    return 0;
}

int mb_quarantine_approve(const char *bundle_path) {
    return write_status(bundle_path, "approved");
}

int mb_quarantine_reject(const char *bundle_path) {
    return write_status(bundle_path, "rejected");
}

const char *mb_quarantine_status_string(mb_quarantine_status_t s) {
    switch (s) {
    case MB_QUARANTINE_APPROVED:  return "approved";
    case MB_QUARANTINE_PENDING:   return "pending";
    case MB_QUARANTINE_REJECTED:  return "rejected";
    case MB_QUARANTINE_MALFORMED: return "malformed-xattr";
    case MB_QUARANTINE_NO_XATTR:  return "filesystem-no-xattr";
    case MB_QUARANTINE_ERR_IO:    return "io-error";
    }
    return "?";
}
