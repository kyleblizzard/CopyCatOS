// CopyCatOS — by Kyle Blizzard at Blizzard.show

// dnd.h — XDND (X Drag and Drop Protocol v5) for desktop
//
// XDND is the standard drag-and-drop protocol for X11 applications.
// It's a multi-step ClientMessage handshake between a "source" (the window
// the drag starts in) and a "target" (the window where the user drops).
//
// desktop participates as both roles:
//
//   SOURCE: when you drag a file FROM the desktop to another app
//     (e.g., drag a .pdf to a text editor or drop onto a terminal)
//
//   TARGET: when another app (a file manager, fileviewer) drops files
//     ONTO the desktop, which copies them to ~/Desktop
//
// Protocol overview (14 atoms, handshake in both directions):
//
//   Source → Target:
//     XdndEnter    — "I'm dragging over you, here are my MIME types"
//     XdndPosition — "cursor is at (x,y), I'd like to do <action>"
//     XdndLeave    — "I moved away, cancel any hover state"
//     XdndDrop     — "user released the button, please accept"
//
//   Target → Source:
//     XdndStatus   — "accept (or reject), I'll do <action>"
//     XdndFinished — "data transfer complete (or failed)"
//
//   Data transfer: XConvertSelection on XdndSelection atom, MIME "text/uri-list"

#ifndef CC_DND_H
#define CC_DND_H

#include <X11/Xlib.h>
#include <stdbool.h>

// XDND protocol version we advertise and require.
// Version 5 is the current standard; virtually all apps support it.
#define XDND_VERSION 5

// Drag threshold: don't start XDND until the mouse moves this many pixels
// from the click point. Prevents accidental drags on click.
#define DND_DRAG_THRESHOLD 4

// All 15 atoms used by XDND + our MIME types, interned once at startup.
typedef struct {
    Atom XdndAware;       // Property declaring XDND support (value = version)
    Atom XdndProxy;       // Property redirecting root-window drops to us
    Atom XdndEnter;       // Source → target: announce drag + offered types
    Atom XdndPosition;    // Source → target: cursor position + proposed action
    Atom XdndStatus;      // Target → source: accept/reject + chosen action
    Atom XdndLeave;       // Source → target: drag has left this window
    Atom XdndDrop;        // Source → target: user released — please accept
    Atom XdndFinished;    // Target → source: transfer complete (or failed)
    Atom XdndActionCopy;  // Copy the dragged data (file stays at source)
    Atom XdndActionMove;  // Move the dragged data (delete from source after)
    Atom XdndTypeList;    // Extended type list atom (for >3 MIME types)
    Atom XdndSelection;   // X selection used for the data transfer
    Atom text_uri_list;   // "text/uri-list" — one URI per line, \r\n separated
    Atom text_plain;      // "text/plain" — fallback for apps that need it
    Atom UTF8_STRING;     // UTF-8 string type
} DndAtoms;

// Initialize XDND:
//   - Intern all atoms
//   - Set XdndAware (version=5) on desktop_win
//   - Set XdndProxy on root_win pointing to desktop_win (so apps that look
//     at the root window's XdndProxy find our window)
//   - Set XdndProxy on desktop_win pointing to itself (confirms proxy is live)
void dnd_init(Display *dpy, Window desktop_win, Window root_win);

// Remove XdndProxy from the root window. Call from desktop_shutdown() so
// other apps don't try to proxy drops to our dead window after we exit.
void dnd_shutdown(Display *dpy, Window root_win);

// Access the interned atoms (desktop.c needs these to identify events).
const DndAtoms *dnd_atoms(void);

// ── Source role: dragging FROM the desktop ───────────────────────────

// Begin tracking an XDND drag. Call this once the drag threshold has been
// exceeded (mouse moved DND_DRAG_THRESHOLD px from the click point).
// Grabs the pointer to src_win so all subsequent mouse events come to us
// regardless of what window the cursor is over.
void dnd_source_begin(Display *dpy, Window src_win, Window root_win,
                      const char *file_path, int root_x, int root_y);

// Update drag position. Call on every MotionNotify event during a drag.
// Internally finds the XdndAware window under the cursor via XQueryPointer,
// and sends XdndEnter, XdndPosition, or XdndLeave as needed when the
// target window changes.
void dnd_source_motion(Display *dpy, int root_x, int root_y);

// Returns true if we're currently in an XDND handshake with an external window.
// False means the cursor is over the desktop — normal grid-snap applies.
bool dnd_source_active(void);

// Handle the button release during a drag.
// If targeting an external XDND window: sends XdndDrop and releases the grab.
// Returns true to tell the caller that XDND handled the drop.
// Returns false (no external target) so the caller can do its own grid-snap.
bool dnd_source_drop(Display *dpy);

// Cancel the drag (e.g., Escape key pressed).
// Sends XdndLeave to the current target (if any) and releases the grab.
void dnd_source_cancel(Display *dpy);

// ── Event handlers: call from the desktop event loop ─────────────────

// Handle a ClientMessage event. Dispatches:
//   When we're SOURCE: XdndStatus (target's response), XdndFinished
//   When we're TARGET: XdndEnter, XdndPosition, XdndLeave, XdndDrop
void dnd_handle_client_message(Display *dpy, XClientMessageEvent *ev);

// Respond to a SelectionRequest — when we're the source and the drop target
// calls XConvertSelection to fetch the file URI, we get this event and
// must write the URI into the requested property then send SelectionNotify.
void dnd_handle_selection_request(Display *dpy, XSelectionRequestEvent *ev);

// Handle a SelectionNotify — when we're the target and we called
// XConvertSelection, the source sends us this event with the data.
// Reads the file URI, parses it, and copies the file to ~/Desktop.
// Returns the destination path if a file arrived, or NULL on failure.
const char *dnd_handle_selection_notify(Display *dpy, XSelectionEvent *ev);

// Periodic idle tick. Call from the event loop's 500ms timeout path.
// Checks if a pending XdndDrop has waited >3 seconds without XdndFinished
// (some apps never send it). Returns true if the source state was reset
// (caller should snap the dragged icon back to its grid position).
bool dnd_tick(void);

#endif // CC_DND_H
