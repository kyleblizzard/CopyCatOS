// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase_clipboard.h — system clipboard API (reserved filename).
//
// The base "something changed" signal is already delivered via
// `MB_IPC_CLIPBOARD_CHANGED` (see IPC.md §5.5). This header will add the
// read/write API and the richer type-negotiation model (utf-8 string,
// RTF, PNG, custom UTIs). Scoped for v1 if it lands cleanly; reserved
// filename regardless so the namespace is not squatted. No symbols
// ship in v1 at this stage.

#ifndef MOONBASE_CLIPBOARD_H
#define MOONBASE_CLIPBOARD_H

#define MOONBASE_CLIPBOARD_API_VERSION 0

#endif // MOONBASE_CLIPBOARD_H
