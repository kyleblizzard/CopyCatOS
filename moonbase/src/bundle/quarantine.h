// CopyCatOS — by Kyle Blizzard at Blizzard.show

// quarantine — first-launch trust check for .appc bundles.
//
// Policy (bundle-spec.md §7):
//   A downloaded .appc carries a quarantine xattr on its bundle
//   directory. moonbase-launch reads that xattr before it assembles
//   the sandbox argv:
//
//       user.moonbase.quarantine missing / ENODATA   -> APPROVED
//         (bundle came from a package manager or a local build —
//         whoever put it on disk didn't mark it, we trust them)
//       user.moonbase.quarantine == "approved"       -> APPROVED
//       user.moonbase.quarantine == "pending"        -> PENDING
//         (downloaded, never launched — hand off to moonbase-consent)
//       user.moonbase.quarantine == "rejected"       -> REJECTED
//         (user previously declined — refuse to launch without
//         re-opening the consent flow via another tool)
//
//   Once consent approves, the caller rewrites the xattr to
//   "approved" via mb_quarantine_approve.
//
// Fallback trust database (flat TSV, for filesystems that report
// ENOTSUP on getxattr) is out of scope for this slice — returned as
// ERR_NO_XATTR and the caller decides. When it lands it'll live at
// $HOME/.local/share/moonbase/trust.db, keyed by (bundle-id, hash,
// absolute bundle path).

#ifndef MOONBASE_BUNDLE_QUARANTINE_H
#define MOONBASE_BUNDLE_QUARANTINE_H

typedef enum {
    MB_QUARANTINE_APPROVED = 0,
    MB_QUARANTINE_PENDING,
    MB_QUARANTINE_REJECTED,
    // Unknown xattr value (not one of the three above). Treated as
    // PENDING by the launcher — malformed xattr can never gate a
    // launch through, only gate one out.
    MB_QUARANTINE_MALFORMED,
    // Filesystem doesn't support xattrs. Caller should fall back to
    // the trust-db path (slice D.4b), or, for now, treat as APPROVED
    // on first launch — we don't want to brick exFAT-mounted builds
    // before the fallback exists.
    MB_QUARANTINE_NO_XATTR,
    // I/O error: bundle path missing, permission denied, etc.
    MB_QUARANTINE_ERR_IO,
} mb_quarantine_status_t;

// Read the quarantine status of a bundle directory. bundle_path must
// be the directory that contains Contents/Info.appc; the xattr is
// queried on that directory (not on Info.appc itself) so a
// tamper-aware repair scan can reach every file inside later.
mb_quarantine_status_t mb_quarantine_check(const char *bundle_path);

// Mark a bundle as approved by writing user.moonbase.quarantine
// = "approved". Returns 0 on success, -1 on I/O failure.
int mb_quarantine_approve(const char *bundle_path);

// Mark a bundle as rejected. Same contract as approve.
int mb_quarantine_reject(const char *bundle_path);

// Short human-readable label for a status — for error messages and
// logging.
const char *mb_quarantine_status_string(mb_quarantine_status_t s);

#endif
