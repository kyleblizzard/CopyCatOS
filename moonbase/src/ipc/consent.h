// CopyCatOS — by Kyle Blizzard at Blizzard.show

// consent.h — internal client wire for MB_IPC_CONSENT_REQUEST.
//
// Sandbox doctrine (moonbase/sandbox.md §6) walks every privileged
// call through two gates in order:
//
//   1. entitlement — MB_EPERM immediately if missing, no prompt.
//   2. consent     — honor a recorded allow/deny; if nothing recorded,
//                    dispatch MB_IPC_CONSENT_REQUEST → moonbase-consent
//                    → write consents.toml → return the decision.
//
// This header ships the client half of gate 2's IPC leg: the framework
// sends a request, the compositor-side consent responder (landing in a
// paired slice) asks the user, writes the decision into the caller's
// consents.toml, and replies with CONSENT_GRANT or CONSENT_DENY.
//
// This is deliberately **not** a public moonbase_*.h symbol. Callers
// live inside libmoonbase — the only clients are the per-capability
// gate shims in consents_gate.c (once the responder lands). App code
// never talks directly to this surface; they hit entitlement+consent
// transparently when they touch keychain, a11y, camera, etc.
//
// Wire schema (IPC.md §5.4):
//   REQUEST body: { 1: uint req_id, 2: tstr capability,
//                   3: tstr context, 4: uint window_id }
//   GRANT  body: { 1: uint req_id, 2: bool remember }
//   DENY   body: { 1: uint req_id, 2: bool remember }
//
// The `remember` flag is informational — the responder already writes
// the consents.toml row before replying, so a `remember=false` hint
// just tells the framework "treat this as session-only if/when we
// honor scope"; the reader currently ignores it.

#ifndef MOONBASE_IPC_CONSENT_H
#define MOONBASE_IPC_CONSENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Request a consent decision from the compositor-side responder.
//
//   capability  — dotted "group.value" (e.g. "system.keychain"). Must
//                 be non-NULL and non-empty.
//   context     — human-readable reason shown on the sheet. NULL →
//                 empty string on the wire.
//   window_id   — foreground window to parent the sheet to, or 0 for
//                 "no window" (prefs / background work).
//   out_allow   — receives true on GRANT, false on DENY. Required.
//   out_remember— receives the responder's remember flag. Optional
//                 (pass NULL to ignore).
//
// Returns 0 on success. Negative mb_error_t on failure: MB_EINVAL for
// bad args, MB_EIPC if the connection drops mid-request, MB_EPROTO on
// a malformed reply, MB_ENOMEM on allocation failure, or whatever
// code the responder echoed back via MB_IPC_ERROR.
//
// Frames of kinds other than CONSENT_GRANT / CONSENT_DENY that arrive
// during the wait are forwarded to the pending-frame queue (same as
// mb_conn_request). A GRANT/DENY whose req_id doesn't match this
// call is *also* queued — a reply to an older in-flight request (or,
// once we grow concurrent requests, a sibling request) is not ours
// to consume.
int mb_ipc_consent_request(const char *capability,
                           const char *context,
                           uint32_t    window_id,
                           bool       *out_allow,
                           bool       *out_remember);

#ifdef __cplusplus
}
#endif

#endif // MOONBASE_IPC_CONSENT_H
