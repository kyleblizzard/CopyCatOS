// CopyCatOS — by Kyle Blizzard at Blizzard.show

// mb-quarantine — Phase D slice 4 xattr gate unit test.
//
// Builds a dummy bundle directory in /tmp, manipulates the xattr
// directly, and checks that mb_quarantine_check returns the expected
// status for every value plus the no-xattr, missing-xattr, and
// malformed cases. Also round-trips mb_quarantine_approve /
// mb_quarantine_reject: write, re-read, assert the stored string.
//
// Skips (exit 77) if xattrs aren't supported on /tmp — on typical
// Linux this never happens (/tmp is tmpfs, tmpfs has xattr support),
// but the check keeps the test honest on exotic builds.

#include "bundle/quarantine.h"

#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

static int fail_count = 0;

#define EXPECT(cond, ...) do {                                     \
    if (!(cond)) {                                                 \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);       \
        fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");       \
        fail_count++;                                              \
    }                                                              \
} while (0)

static int rm_rf_cb(const char *p, const struct stat *s, int t, struct FTW *f) {
    (void)s; (void)t; (void)f; (void)remove(p); return 0;
}
static void rm_rf(const char *p) { nftw(p, rm_rf_cb, 16, FTW_DEPTH | FTW_PHYS); }

int main(void) {
    char tmpl[] = "/tmp/mb-quarantine-XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) { perror("mkdtemp"); return 1; }

    // Baseline: no xattr set → APPROVED.
    EXPECT(mb_quarantine_check(dir) == MB_QUARANTINE_APPROVED,
           "no-xattr treated as APPROVED");

    // If the filesystem refuses xattrs outright the very first set
    // call below will fail with ENOTSUP — skip the test in that case.
    if (setxattr(dir, "user.moonbase.quarantine", "approved", 8, 0) != 0) {
        if (errno == ENOTSUP
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
            || errno == EOPNOTSUPP
#endif
        ) {
            fprintf(stderr, "SKIP: /tmp does not support user xattrs\n");
            rm_rf(dir);
            return 77;
        }
        fprintf(stderr, "setxattr: %s\n", strerror(errno));
        rm_rf(dir);
        return 1;
    }
    EXPECT(mb_quarantine_check(dir) == MB_QUARANTINE_APPROVED,
           "xattr=approved → APPROVED");

    EXPECT(setxattr(dir, "user.moonbase.quarantine", "pending", 7, 0) == 0,
           "set pending");
    EXPECT(mb_quarantine_check(dir) == MB_QUARANTINE_PENDING,
           "xattr=pending → PENDING");

    EXPECT(setxattr(dir, "user.moonbase.quarantine", "rejected", 8, 0) == 0,
           "set rejected");
    EXPECT(mb_quarantine_check(dir) == MB_QUARANTINE_REJECTED,
           "xattr=rejected → REJECTED");

    EXPECT(setxattr(dir, "user.moonbase.quarantine", "bogus", 5, 0) == 0,
           "set bogus");
    EXPECT(mb_quarantine_check(dir) == MB_QUARANTINE_MALFORMED,
           "unknown xattr → MALFORMED");

    // Round-trip approve().
    EXPECT(mb_quarantine_approve(dir) == 0, "approve() ok");
    EXPECT(mb_quarantine_check(dir) == MB_QUARANTINE_APPROVED,
           "after approve(): APPROVED");
    char buf[32] = {0};
    ssize_t n = getxattr(dir, "user.moonbase.quarantine", buf, sizeof(buf) - 1);
    EXPECT(n == 8 && strcmp(buf, "approved") == 0,
           "xattr literally 'approved' (got n=%zd, buf=%s)", n, buf);

    // Round-trip reject().
    EXPECT(mb_quarantine_reject(dir) == 0, "reject() ok");
    EXPECT(mb_quarantine_check(dir) == MB_QUARANTINE_REJECTED,
           "after reject(): REJECTED");

    // Bad path.
    EXPECT(mb_quarantine_check("/does/not/exist") == MB_QUARANTINE_ERR_IO,
           "missing path → ERR_IO");
    EXPECT(mb_quarantine_check(NULL) == MB_QUARANTINE_ERR_IO,
           "NULL path → ERR_IO");

    rm_rf(dir);

    if (fail_count) {
        fprintf(stderr, "FAILED: %d assertion(s)\n", fail_count);
        return 1;
    }
    fprintf(stderr, "OK\n");
    return 0;
}
