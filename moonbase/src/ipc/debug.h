// CopyCatOS — by Kyle Blizzard at Blizzard.show

// debug.h — MOONBASE_DEBUG_JSON frame dumper.
//
// IPC.md §8 says JSON never rides the wire. It has exactly one job:
// when `MOONBASE_DEBUG_JSON=1` is set in the environment, every frame
// sent or received by libmoonbase is dumped to stderr as one JSON
// object per line. That's a debugging convenience only — the live
// transport is always binary CBOR/TLV (IPC.md §2).
//
// When MOONBASE_DEBUG_JSON is unset or 0, these functions are
// zero-cost: they read the env variable once on first call, cache the
// result, and return immediately on every subsequent call.

#ifndef MOONBASE_IPC_DEBUG_H
#define MOONBASE_IPC_DEBUG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Dump a single frame to stderr when debug mode is on. `outgoing`
// true == "we sent this", false == "we received this". The body must
// be a CBOR-encoded message body (or NULL / 0 for empty bodies).
// Never copies the buffer; safe to call from hot paths.
void mb_debug_dump_frame(bool outgoing,
                         uint16_t kind,
                         const uint8_t *body, size_t body_len);

#ifdef __cplusplus
}
#endif

#endif // MOONBASE_IPC_DEBUG_H
