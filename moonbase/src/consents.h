// CopyCatOS — by Kyle Blizzard at Blizzard.show

// consents — per-capability first-use consent reader.
//
// Complements entitlements.h. Entitlements answer "did the bundle
// declare this capability in Info.appc?" — a static property of the
// bundle. Consent answers "did the user approve its use?" — a dynamic
// property written by moonbase-consent at first use.
//
// Sandbox doctrine (`moonbase/sandbox.md` §6) walks every privileged
// call through two gates in order:
//
//   1. entitlement — MB_EPERM immediately if missing, no prompt.
//   2. consent     — honor a recorded allow/deny; if nothing recorded,
//                    dispatch MB_IPC_CONSENT_REQUEST → moonbase-consent
//                    → write consents.toml → return the decision.
//
// This header ships gate 2's reader + gate helper. With the IPC wire
// live (moonrock's consent responder forwards MISSING lookups to
// moonbase-consent, which writes consents.toml and replies
// GRANT/DENY), mb_consent_gate_allows honors that decision in
// process. If the compositor connection is down or the IPC fails,
// the gate denies — an unprovoked privileged call reaching a headless
// tool has no user to approve it.
//
// On-disk store, per sandbox.md §6:
//
//   $XDG_DATA_HOME/moonbase/<bundle-id>/Preferences/consents.toml
//   (fallback: $HOME/.local/share/moonbase/<bundle-id>/Preferences/…)
//
// Schema (one section per capability, named "<group>.<value>"):
//
//   [system.keychain]
//   decision = "allow"      # "allow" | "deny"
//   scope    = "always"     # "always" | "session" — reader ignores
//                           #   for now; enforcement lands with the
//                           #   session boundary IPC.
//   granted  = 2026-04-18T12:00:00Z
//
// No cache. The file is re-read on every query because a pending IPC
// grant may have just written it — a stale cached "missing" would
// regress the refapp the moment the writer ships.

#ifndef MOONBASE_CONSENTS_H
#define MOONBASE_CONSENTS_H

#include <stdbool.h>

#include "CopyCatAppKit.h"  // mb_error_t

// What the reader found for (group, value) in consents.toml.
typedef enum {
    MB_CONSENT_MISSING = 0,  // no section, no file, or decision absent
    MB_CONSENT_ALLOW   = 1,  // decision = "allow"
    MB_CONSENT_DENY    = 2,  // decision = "deny"
} mb_consent_status_t;

// Raw query. Returns MB_CONSENT_MISSING on NULL inputs, absent bundle
// id, unreadable file, or missing section. Safe to call from any
// thread.
mb_consent_status_t mb_consent_query(const char *group, const char *value);

// Gate helper implementing sandbox §6: returns true on ALLOW, false
// on DENY. On MISSING, dispatches MB_IPC_CONSENT_REQUEST to moonrock
// and returns the responder's decision. If the connection isn't
// handshaken (no compositor) or the IPC fails, returns false and
// emits a first-time stderr diagnostic per (group, value) pair so
// the cause is obvious in logs without spamming on repeat calls.
bool mb_consent_gate_allows(const char *group, const char *value);

// Record a decision for (group, value) to the caller's consents.toml.
// `decision` must be MB_CONSENT_ALLOW or MB_CONSENT_DENY —
// MB_CONSENT_MISSING is rejected with MB_EINVAL (there is no "unset"
// wire representation, and the reader already treats absent sections
// as MISSING). Writes land atomically (tmp file → fsync → rename) so
// a crash mid-write can never leave consents.toml truncated or
// half-updated. If a section named [<group>.<value>] already exists,
// it is replaced in place; other sections and ordering are preserved.
//
// Returns MB_EOK on success; MB_EINVAL on bad args; MB_EPERM when no
// bundle id is resolvable (the writer has no idea where to put the
// file); MB_ENOMEM on allocation failure; MB_EIPC on filesystem I/O
// failure. The file is mode 0600 and its Preferences/ parent is
// mode 0700 — the caller's home directory already enforces
// per-user privacy, but belt-and-suspenders costs nothing.
//
// Dead code in this commit: no caller in libmoonbase yet. The IPC
// responder (`moonbase-consent` growing MB_IPC_CONSENT_REQUEST
// handling) wires this up in a follow-up.
mb_error_t mb_consent_record(const char *group,
                             const char *value,
                             mb_consent_status_t decision);

#endif // MOONBASE_CONSENTS_H
