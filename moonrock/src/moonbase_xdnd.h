// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase_xdnd.h — MoonRock's XDND bridge.
//
// Receives XdndEnter / XdndPosition / XdndLeave / XdndDrop ClientMessages
// and SelectionNotify replies from the X drag source (fileviewer,
// Nautilus, any XDND-speaking app) and forwards them to the owning
// MoonBase surface as MB_IPC_DRAG_{ENTER,OVER,LEAVE,DROP} frames.
//
// Scope is deliberately narrow: the module only cares about XDND
// ClientMessages whose `window` member is a MoonBase surface's
// InputOnly proxy — everything else gets declined (`return false`) and
// the caller's existing X-client dispatch runs unchanged.

#ifndef MOONROCK_MOONBASE_XDND_H
#define MOONROCK_MOONBASE_XDND_H

#include <stdbool.h>

#include <X11/Xlib.h>

#ifdef __cplusplus
extern "C" {
#endif

// Wake the XDND module and cache the atoms we'll need on every
// ClientMessage / SelectionNotify. Safe to call multiple times; second
// and later calls no-op. `dpy` must outlive every other call into this
// module.
void mb_xdnd_init(Display *dpy, Window root);

// Try to consume an X ClientMessage as an XDND event. Returns true iff
// the event targeted a MoonBase proxy AND was an XDND message — in
// which case the caller must NOT run its normal ClientMessage
// dispatch. Returns false for non-XDND messages, or for XDND messages
// whose target is not one of ours.
bool mb_xdnd_handle_client_message(const XClientMessageEvent *cm);

// Try to consume an X SelectionNotify as a reply to our
// XConvertSelection(text/uri-list). Returns true iff the notification
// targeted a MoonBase proxy AND the property is our XdndSelection.
bool mb_xdnd_handle_selection_notify(const XSelectionEvent *se);

// Drop any active drag session that was targeting `win` — called when
// a MoonBase surface (and its InputOnly proxy) goes away mid-drag. No-
// op when there's no active session for that window.
void mb_xdnd_forget_window(Window win);

#ifdef __cplusplus
}
#endif

#endif // MOONROCK_MOONBASE_XDND_H
