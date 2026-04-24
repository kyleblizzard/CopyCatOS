// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
//  MoonRock -> Shell scale bridge — public contract
// ============================================================================
//
// MoonRock is the single source of truth for per-output HiDPI scale. EDID
// defaults, user overrides, and persistence all live inside moonrock_display.c.
//
// Standalone shell components (menubar, dock, desktop, systemcontrol) are
// separate X11 processes and cannot call into MoonRock's C API. To let them
// observe per-output scale without introducing a second daemon or a new IPC
// socket, MoonRock publishes a line-oriented text property on the root window.
//
//     Atom:   _MOONROCK_OUTPUT_SCALES           (type: UTF8_STRING, format 8)
//     Format: one line per connected output, newline-terminated:
//
//         <output_name> <x> <y> <width> <height> <effective_scale> <primary> <rotation> <multiplier>
//
//     <effective_scale> is the *final* scale every shell component should
//     render at. It already folds in the user-chosen Interface Scale
//     multiplier, so subscribers never multiply again — they just read the
//     field. Formally:
//
//         effective_scale = backing_scale × interface_multiplier
//
//     where <backing_scale> is MoonRock's EDID-derived density choice and
//     <interface_multiplier> is the per-output Interface Scale slider value
//     (SysPrefs → Displays).
//
//     <primary> is an optional trailing integer: 1 if this is the XRandR
//     primary output, 0 otherwise. Parsers that pre-date this field treat
//     lines with 6 tokens as before (primary = false for every entry).
//
//     <rotation> is a further optional integer: 0, 90, 180, or 270 degrees
//     counter-clockwise from landscape. Parsers that pre-date this field
//     accept lines of 6 or 7 tokens and default rotation to 0.
//
//     <multiplier> is the further-optional user-chosen Interface Scale
//     multiplier, carried so UI (systemcontrol's Displays pane) can show
//     the slider position without a second round-trip. Subscribers that
//     only render never need this field — it's already folded into
//     <effective_scale>. Parsers that pre-date this field accept lines of
//     6, 7, or 8 tokens and default multiplier to 1.0.
//
//     Example:
//         eDP-1 0 0 1920 1200 3.000 1 0 2.000
//         HDMI-1 1920 0 1920 1080 1.000 0 0 1.000
//
// The property is rewritten whenever the connected-output set changes
// (hotplug) or whenever the user changes a scale through SysPrefs (which in
// turn calls display_set_scale_for_output()). Xlib converts a root-property
// write into a PropertyNotify event on every client that has selected
// PropertyChangeMask on the root window, so subscribers get live updates for
// free.
//
// Geometry is in physical pixels in the virtual-screen coordinate space —
// the same space XRandR uses. This is so clients can pick the output that
// contains a given root-space window origin without a second round-trip.
// The scale is the final effective factor (user override if any, otherwise
// the EDID-derived default).
// ============================================================================

#ifndef MOONROCK_SCALE_H
#define MOONROCK_SCALE_H

#include <X11/Xlib.h>
#include <stdbool.h>

// Atom name for the root-window property. The only shared string between
// publisher (moonrock) and subscribers (shell components) — every other
// constant below is client-side.
#define MOONROCK_SCALE_ATOM_NAME "_MOONROCK_OUTPUT_SCALES"

// Reverse-direction atom for writing a scale change request back to MoonRock.
// systemcontrol's Displays pane writes a single line:
//
//     <output_name> <scale>\n     e.g. "eDP-1 1.500\n"
//
// on this atom (UTF8_STRING, format 8). MoonRock's event loop sees the
// PropertyNewValue notification, parses the line, calls
// display_set_scale_for_output(), and then deletes the property so a
// second write of the same value still generates a PropertyNotify. A scale
// of 0 clears the user override and reverts to the EDID-derived default.
#define MOONROCK_SET_SCALE_ATOM_NAME "_MOONROCK_SET_OUTPUT_SCALE"

// Reverse-direction atom for designating an output as the XRandR primary.
// systemcontrol's Displays pane writes a single bare output name plus
// newline:
//
//     <output_name>\n             e.g. "HDMI-1\n"
//
// on this atom (UTF8_STRING, format 8). MoonRock parses the name, calls
// XRRSetOutputPrimary on the matching output, persists the chosen EDID hash
// to ~/.local/share/moonrock/display-config.conf so the choice survives
// logout/login and cable swaps, and deletes the property so a repeat write
// still generates a PropertyNotify. An empty payload clears the user
// override (XRandR picks automatically).
//
// Slightly different naming from SET_SCALE: here the subject of the verb is
// "the primary" not "an output's primary", so SET_PRIMARY_OUTPUT reads
// better than SET_OUTPUT_PRIMARY.
#define MOONROCK_SET_PRIMARY_ATOM_NAME "_MOONROCK_SET_PRIMARY_OUTPUT"

// Reverse-direction atom for rotating an output. The Displays pane writes
// one line:
//
//     <output_name> <degrees>\n    e.g. "eDP-1 90\n"
//
// on this atom (UTF8_STRING, format 8). <degrees> is 0, 90, 180, or 270
// (counter-clockwise). MoonRock maps it to the corresponding XRandR
// RR_Rotate_* constant, commits via XRRSetCrtcConfig (growing the virtual
// screen first if the rotated footprint needs more room), persists the
// choice per EDID hash, and deletes the property so a repeat write of
// the same value still generates a PropertyNotify. Any unknown value is
// rejected without side-effects.
#define MOONROCK_SET_ROTATION_ATOM_NAME "_MOONROCK_SET_OUTPUT_ROTATION"

// Reverse-direction atom for the per-output Interface Scale multiplier.
// systemcontrol's Displays pane writes one line:
//
//     <output_name> <multiplier>\n   e.g. "eDP-1 2.000\n"
//
// on this atom (UTF8_STRING, format 8). <multiplier> is a float in
// 0.5 – 4.0 (matches the HiDPI invariant). 0.0 clears the user override
// and reverts to the EDID-keyed default (1.0 on externals, an
// auto-chosen 1.5–2.0 on the Legion Go S built-in panel).
//
// MoonRock persists the chosen multiplier per EDID hash in
// ~/.local/share/moonrock/display-config.conf alongside the backing
// scale, primary, and rotation overrides. On receipt it recomputes
// effective_scale = backing × multiplier and rewrites
// _MOONROCK_OUTPUT_SCALES so every shell component re-lays out live
// via PropertyNotify. The set-atom is then deleted so a repeat write
// of the same value still generates a PropertyNotify.
#define MOONROCK_SET_INTERFACE_SCALE_ATOM_NAME "_MOONROCK_SET_OUTPUT_INTERFACE_SCALE"

// Companion atoms — per-output focus state, row-index-aligned with
// _MOONROCK_OUTPUT_SCALES. Published together with the scales table
// so subscribers can read all three with a single PropertyNotify
// round-trip.
//
// _MOONROCK_ACTIVE_OUTPUT
//     Type: CARDINAL, format 32, length 1.
//     Value: row index into _MOONROCK_OUTPUT_SCALES of the output
//     hosting the keyboard-focused top-level window (_NET_ACTIVE_WINDOW's
//     home output). 0xFFFFFFFF when no window is focused.
//
//     "Home output" is the output containing the focused window's
//     geometric midpoint. This matches macOS's semantic — the active
//     output is the focused window's host, not the pointer's host.
//     Menubar uses it to pick which pane draws at full opacity and
//     which pane opens dropdowns when clicked.
//
// _MOONROCK_FRONTMOST_PER_OUTPUT
//     Type: WINDOW (XA_WINDOW), format 32, length == row count of
//     _MOONROCK_OUTPUT_SCALES.
//     Value: parallel array, one WID per output in the same row
//     order as _MOONROCK_OUTPUT_SCALES. Each entry is the X window
//     ID of the topmost managed client whose home output is that
//     row (using X stacking order via XQueryTree), or None (0) if
//     no managed window lives on that output. Menubar uses this to
//     label each per-output pane with its own output's frontmost app.
//
// Publication cadence: rewritten whenever _MOONROCK_OUTPUT_SCALES is
// rewritten (so row order stays in sync) AND whenever focus or the
// managed client list changes. Dedup identical writes so subscribers
// aren't spammed when nothing changed.
#define MOONROCK_ACTIVE_OUTPUT_ATOM_NAME        "_MOONROCK_ACTIVE_OUTPUT"
#define MOONROCK_FRONTMOST_PER_OUTPUT_ATOM_NAME "_MOONROCK_FRONTMOST_PER_OUTPUT"

// Sentinel value for _MOONROCK_ACTIVE_OUTPUT when no window is focused.
// CARDINAL is unsigned 32-bit on the wire, so we use 0xFFFFFFFF as the
// "no active output" marker. Subscribers should treat values >= output
// count as "no active output."
#define MOONROCK_ACTIVE_OUTPUT_NONE 0xFFFFFFFFu


// Cap on how many outputs we parse into a single table. Matches
// MAX_OUTPUTS on the publisher side so we never drop a legitimate entry.
#define MOONROCK_SCALE_MAX_OUTPUTS 16

// Fixed-size buffer for each output's human-readable name ("eDP-1",
// "HDMI-1", "DP-2-1", …). XRandR output names rarely exceed 16 bytes so
// 64 is comfortable headroom.
#define MOONROCK_SCALE_NAME_MAX 64


// One parsed line from the property — a snapshot of one connected output.
typedef struct {
    char  name[MOONROCK_SCALE_NAME_MAX];
    int   x, y;                 // top-left in virtual-screen pixels
    int   width, height;        // pixel size
    float scale;                // effective scale = backing × multiplier
                                // (what every shell component renders at)
    bool  primary;              // true if this is the XRandR primary output
    int   rotation;             // 0, 90, 180, or 270 degrees counter-clockwise
    float multiplier;           // user-chosen Interface Scale (0.5 – 4.0);
                                // 1.0 when no override is set. Folded into
                                // `scale`; present so UI can show the slider.
} MoonRockOutputScale;

// Full parsed table. `count` may be zero if the property is missing (e.g.
// MoonRock hasn't started yet) or malformed; callers should treat that
// case as "scale is 1.0 for all points."
typedef struct {
    MoonRockOutputScale outputs[MOONROCK_SCALE_MAX_OUTPUTS];
    int                 count;
    bool                valid;
} MoonRockScaleTable;


// Intern the atom and enable PropertyChangeMask on the root window so the
// calling process starts receiving PropertyNotify events for the scale
// property. Existing event-mask bits on the root window are preserved —
// we're additive, never destructive. Safe to call multiple times; only
// the first call does work.
//
// Returns true on success, false if `dpy` is NULL.
bool moonrock_scale_init(Display *dpy);

// Return the interned atom for _MOONROCK_OUTPUT_SCALES. Calling this before
// moonrock_scale_init() will intern it on demand. Useful for comparing
// against ev.xproperty.atom in an event loop.
Atom moonrock_scale_atom(Display *dpy);

// Read the current property value from the root window and parse it into
// `out`. Call once during startup (after moonrock_scale_init) and again
// from every PropertyNotify where ev.xproperty.atom == moonrock_scale_atom.
//
// On failure (property missing or malformed) `out->valid` is set to false
// and `out->count` to 0. The function still returns false in that case so
// callers can distinguish "no data" from "got data."
bool moonrock_scale_refresh(Display *dpy, MoonRockScaleTable *out);

// Look up the scale for a point in virtual-screen coordinates. Returns 1.0
// if the table is invalid or the point is outside every output.
float moonrock_scale_for_point(const MoonRockScaleTable *table, int x, int y);

// Look up the scale for an output by its name (e.g. "eDP-1"). Returns 1.0
// if the table is invalid or no output matches.
float moonrock_scale_for_name(const MoonRockScaleTable *table, const char *name);

// Return a pointer to the primary-output entry in `table`, or NULL if
// the table is invalid, empty, or no entry is marked primary. The
// pointer is only valid until the next call to moonrock_scale_refresh
// on the same table. Used by shell components (menubar, dock, desktop)
// to anchor their layout to the primary output rather than the virtual
// root, which can span multiple displays after hotplug.
const MoonRockOutputScale *
moonrock_scale_primary(const MoonRockScaleTable *table);


// ── Requester — systemcontrol Displays pane → MoonRock ──────────────────
// Writes _MOONROCK_SET_OUTPUT_SCALE on the root window so MoonRock picks
// up a user-initiated scale change. MoonRock owns the EDID hash + config
// persistence — the pane only sends the requested pair.
//
//   output_name — the same name MoonRock publishes in the scale table
//                 (e.g. "eDP-1"). Case-sensitive exact match.
//   scale       — desired effective scale (0.5 – 4.0), or 0.0 to clear the
//                 user override and fall back to the EDID-derived default.
//
// Returns true on a successful X write (MoonRock may still reject an
// out-of-range value; check the published scale table on the next
// PropertyNotify to confirm).
bool moonrock_request_scale(Display *dpy, const char *output_name, float scale);


// ── Requester — systemcontrol Displays pane → MoonRock ──────────────────
// Writes _MOONROCK_SET_PRIMARY_OUTPUT on the root window so MoonRock picks
// up a user-initiated primary change. The new primary is persisted by the
// output's EDID hash, so the same physical monitor stays primary next
// login regardless of which port it's plugged into.
//
//   output_name — the same name MoonRock publishes in the scale table
//                 (e.g. "HDMI-1"). Case-sensitive exact match. Pass NULL
//                 or an empty string to clear the user override and let
//                 XRandR pick automatically.
//
// Returns true on a successful X write (MoonRock rejects unknown names on
// its side; watch the `primary` field in the next PropertyNotify scale
// table to confirm).
bool moonrock_request_primary(Display *dpy, const char *output_name);


// ── Requester — systemcontrol Displays pane → MoonRock ──────────────────
// Writes _MOONROCK_SET_OUTPUT_ROTATION on the root window so MoonRock
// applies a user-initiated rotation. Persisted per EDID hash so the same
// physical monitor stays rotated the same way next login.
//
//   output_name — the same name MoonRock publishes in the scale table
//                 (e.g. "eDP-1"). Case-sensitive exact match.
//   degrees     — 0, 90, 180, or 270 (counter-clockwise). Any other
//                 value is rejected client-side.
//
// Returns true on a successful X write. MoonRock may still reject the
// change (unknown output, XRandR refusal); check the next PropertyNotify
// scale table for the applied rotation to confirm.
bool moonrock_request_rotation(Display *dpy, const char *output_name,
                               int degrees);


// ── Requester — systemcontrol Displays pane → MoonRock ──────────────────
// Writes _MOONROCK_SET_OUTPUT_INTERFACE_SCALE on the root window so
// MoonRock applies the user-chosen Interface Scale multiplier for an
// output. MoonRock persists the choice per EDID hash (so the same
// physical monitor keeps its Interface Scale across logouts and cable
// swaps), recomputes the effective scale, and rewrites the scale table
// so every shell component re-lays out live.
//
//   output_name — the same name MoonRock publishes in the scale table
//                 (e.g. "eDP-1"). Case-sensitive exact match.
//   multiplier  — 0.5 – 4.0, matching the HiDPI invariant. Pass 0.0 to
//                 clear the user override and fall back to MoonRock's
//                 EDID-keyed default (1.0 on externals; an auto-chosen
//                 1.5 – 2.0 on the Legion Go S built-in panel).
//
// Returns true on a successful X write. MoonRock clamps out-of-range
// values on its side; check the next PropertyNotify scale table for
// the multiplier actually applied.
bool moonrock_request_interface_scale(Display *dpy,
                                      const char *output_name,
                                      float multiplier);


// ── Reader — _MOONROCK_ACTIVE_OUTPUT ───────────────────────────────────
// Returns the 0-based row index published by MoonRock, or -1 if the
// property is missing / malformed / marked as "no active output"
// (MOONROCK_ACTIVE_OUTPUT_NONE). Callers typically cross-reference the
// returned index against a recently-refreshed MoonRockScaleTable.
int moonrock_active_output_index(Display *dpy);

// Intern the atom so callers can compare against ev.xproperty.atom in
// their PropertyNotify handler without another round-trip.
Atom moonrock_active_output_atom(Display *dpy);


// ── Reader — _MOONROCK_FRONTMOST_PER_OUTPUT ────────────────────────────
// Reads the parallel WID array into `out`. On success `*count` receives
// the number of entries (matches the output row count of the scale
// table captured at the publisher's last write) and the function returns
// true. On missing / malformed property, `*count` is set to 0 and the
// function returns false.
//
// Callers should size `out` to MOONROCK_SCALE_MAX_OUTPUTS; any extra
// entries from the publisher are silently dropped.
bool moonrock_frontmost_per_output(Display *dpy,
                                   Window *out, int cap, int *count);

// Intern the atom for event-loop comparison.
Atom moonrock_frontmost_per_output_atom(Display *dpy);

#endif // MOONROCK_SCALE_H
