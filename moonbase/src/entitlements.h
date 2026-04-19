// CopyCatOS — by Kyle Blizzard at Blizzard.show

// entitlements — API-boundary entitlement gate.
//
// Reads `MOONBASE_ENTITLEMENTS` (set by moonbase-launch from the
// bundle's Info.appc [permissions] tables) once per process and
// answers `(group, value)` membership queries. Every privileged
// public API in libmoonbase — keychain for v1, more as companion
// headers land — calls mb_has_entitlement at its top edge and
// returns MB_EPERM when the matching permission wasn't declared.
//
// Parse is deferred until the first call so unit tests that never
// reach any gated API pay nothing, and the result is cached for
// the life of the process. Thread-safe under pthread_once.
//
// Missing env var == empty set == every gated API returns MB_EPERM.
// That's the right default for dev runs — the launcher fills the
// var for real bundle launches.

#ifndef MOONBASE_ENTITLEMENTS_H
#define MOONBASE_ENTITLEMENTS_H

#include <stdbool.h>

// Does the caller's static entitlement set carry (group, value)?
// `group` is one of "filesystem", "network", "hardware", "system",
// "ipc"; `value` is the full allowlist string from info_appc_schema.md
// ("keychain", "outbound:https", "process-list", …). Returns false
// on NULL inputs.
bool mb_has_entitlement(const char *group, const char *value);

#endif // MOONBASE_ENTITLEMENTS_H
