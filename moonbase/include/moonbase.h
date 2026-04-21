// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase.h — soft-transition shim for the core public API.
//
// The header was renamed to CopyCatAppKit.h on 2026-04-21 to align with
// the CopyCatAppKit.framework bundle layout. Existing consumers that
// include <moonbase.h> keep compiling unchanged — this shim forwards
// to the real header. The shim is removed after ABI freeze.

#ifndef MOONBASE_H_SHIM
#define MOONBASE_H_SHIM

#include "CopyCatAppKit.h"

#endif // MOONBASE_H_SHIM
