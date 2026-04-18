// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase_keychain.h — secure secret storage (Phase A scaffold).
//
// This companion header will expose the Aqua-feeling Keychain API:
// generic passwords, internet passwords, identity/cert items, password
// generation, and autofill affordances, wrapping libsecret underneath
// so existing desktop agents keep working.
//
// Phase A status: **reserved**. No public symbols ship in v1. Only the
// API-version macro is defined so dependents can compile-gate against
// future growth without pulling the whole surface in early. Declaring
// anything here before the Keychain Access reference app is built is
// an ABI-freeze foot-gun — symbols land when the reference app needs
// them, one domain at a time.

#ifndef MOONBASE_KEYCHAIN_H
#define MOONBASE_KEYCHAIN_H

#include <moonbase.h>

#ifdef __cplusplus
extern "C" {
#endif

// MOONBASE_KEYCHAIN_API_VERSION follows the same MAJOR*10000 + MINOR*100
// + PATCH encoding as MOONBASE_API_VERSION. It evolves on its own axis
// so a keychain feature bump does not force a core-ABI bump.
#define MOONBASE_KEYCHAIN_API_VERSION 0

// (No symbols declared in v1. Reserved for:
//   mb_keychain_item_t, mb_keychain_item_kind_t,
//   moonbase_keychain_find, moonbase_keychain_store,
//   moonbase_keychain_delete, moonbase_keychain_generate_password,
//   and the browser-autofill bridge.)

#ifdef __cplusplus
}
#endif

#endif // MOONBASE_KEYCHAIN_H
