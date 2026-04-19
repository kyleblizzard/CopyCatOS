// CopyCatOS — by Kyle Blizzard at Blizzard.show

// consent_responder.h — compositor-side MB_IPC_CONSENT_REQUEST responder.
//
// The server half of the lazy-consent IPC leg. Pairs with
// `mb_ipc_consent_request` (moonbase/src/ipc/consent.h): the client
// sends a REQUEST, this module forks `moonbase-consent ask ...`, maps
// the child's exit code to a reply, and sends GRANT / DENY / ERROR
// back over the same connection.
//
// The module lives in libmoonbase's server tree (not in moonrock) so
// the same code is exercised by the compositor and by the integration
// test without X11 / GL pulled into the test binary. Moonrock wires it
// in from its event loop by delegating the relevant callbacks.
//
// Exit-code contract (matches moonbase-consent's cmd_ask_consent):
//
//   0 → GRANT (remember=true)    — user / automation approved, recorded
//   1 → DENY  (remember=true)    — user / automation rejected, recorded
//   2 → ERROR                    — bad argc (usage), nothing recorded
//   3 → ERROR                    — writer failure, nothing recorded
//   signal / !WIFEXITED → ERROR  — child crashed
//
// `remember=true` is correct for GRANT/DENY because `mb_consent_record`
// has already written the caller's consents.toml before the child
// returns — a subsequent gate check will read it from disk.
//
// Bundle-id provenance — do not read from the REQUEST body. The server
// already knows each client's bundle_id from the HELLO handshake and
// stashes it via mb_consent_responder_note_connected. A client that
// tries to request consent for someone else's capability set simply
// can't — the server doesn't consult the frame for that field.
//
// Non-blocking. Uses pidfd_open(2) (Linux 5.3+) so the outer event
// loop can poll() on every in-flight child alongside the rest of its
// fds. No SIGCHLD handler, no blocking waitpid.

#ifndef MOONBASE_SERVER_CONSENT_RESPONDER_H
#define MOONBASE_SERVER_CONSENT_RESPONDER_H

#include "server.h"

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the responder. Resolves the moonbase-consent binary path
// as follows:
//   1. `consent_bin_override` if non-NULL
//   2. $MOONBASE_CONSENT_BIN if set and non-empty
//   3. MOONBASE_CONSENT_PATH compile-time default
// Returns 0 on success or a negative mb_error_t. Safe to call twice
// (the second call is a no-op and returns 0).
int mb_consent_responder_init(const char *consent_bin_override);

// Tear down: kill (SIGTERM) + reap every in-flight child, close every
// pidfd, free every per-client bundle_id. Safe to call if init was
// never called.
void mb_consent_responder_shutdown(void);

// Call from MB_SERVER_EV_CONNECTED. `bundle_id` may be NULL or empty;
// the responder stores it either way but REQUESTs from clients whose
// bundle_id is empty get an ERROR reply (no per-bundle store to write
// to). Copies the string — the HELLO event's pointer is borrowed.
void mb_consent_responder_note_connected(mb_client_id_t client,
                                         const char *bundle_id);

// Call from MB_SERVER_EV_DISCONNECTED. Kills + reaps any in-flight
// child for `client`, closes that pidfd, frees stored bundle_id.
void mb_consent_responder_note_disconnected(mb_client_id_t client);

// Dispatch an MB_IPC_CONSENT_REQUEST frame body. On a decode failure,
// unknown bundle, or fork/exec failure, an MB_IPC_ERROR reply is sent
// synchronously. On success the child is forked and an in-flight slot
// is reserved; the reply goes out when the child exits (see tick).
// The `body`/`body_len` pair is borrowed; caller keeps ownership.
void mb_consent_responder_handle_request(mb_server_t *s,
                                         mb_client_id_t client,
                                         const uint8_t *body,
                                         size_t body_len);

// Append every in-flight child pidfd to `out_fds` (POLLIN). Writes at
// most `max` entries; returns the number appended. Meant to be called
// after mb_server_get_pollfds so the outer poll covers both.
size_t mb_consent_responder_collect_pollfds(struct pollfd *out_fds,
                                            size_t max);

// Service exited children. For each pidfd in `fds` that signals
// POLLIN, waitpid the child, map its exit status to a GRANT / DENY /
// ERROR reply, send it via `mb_server_send`, close the pidfd, and
// free the slot. Safe with nfds == 0.
void mb_consent_responder_tick(mb_server_t *s,
                               const struct pollfd *fds, size_t nfds);

#ifdef __cplusplus
}
#endif

#endif // MOONBASE_SERVER_CONSENT_RESPONDER_H
