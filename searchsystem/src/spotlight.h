// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ─── spotlight.h ───
// Public interface for the core Spotlight overlay.
//
// This header defines the constants that control the overlay's size and
// appearance, and declares the functions that main.c calls to set up the
// overlay window, register the Ctrl+Space hotkey, and run the event loop.

#ifndef SPOTLIGHT_H
#define SPOTLIGHT_H

#include <X11/Xlib.h>
#include <stdbool.h>

// ──────────────────────────────────────────────
// Layout constants — tweak these to adjust the
// look and feel of the overlay.
// ──────────────────────────────────────────────

// Total width of the overlay window in pixels.
#define SPOTLIGHT_WIDTH       680

// How far from the top of the screen the overlay sits,
// expressed as a fraction of the screen height (0.22 = 22%).
#define SPOTLIGHT_TOP_RATIO   0.22

// Height of the search text-field area at the top of the overlay.
#define SEARCH_HEIGHT         48

// Height of each result row below the search field.
#define RESULT_HEIGHT         44

// Maximum number of result rows visible at one time.
// If more results match they are still stored, but only
// this many are rendered.
#define MAX_VISIBLE_RESULTS   8

// Hard cap on the total number of search results kept
// after filtering (even if more entries match the query).
#define MAX_SEARCH_RESULTS    50

// Radius (in pixels) for the rounded corners on the
// main overlay background.
#define CORNER_RADIUS         12

// ──────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────

// Initialise the overlay: create the ARGB window, grab
// the Ctrl+Space hotkey on the root window, and prepare
// the internal state.  Returns 0 on success, -1 on error.
int  spotlight_init(Display *dpy);

// Enter the main event loop.  This blocks forever (or
// until a signal sets the quit flag).  It listens for
// hotkey presses, keyboard input, and Expose events.
void spotlight_run(Display *dpy);

// Clean up all resources (window, surfaces, search data).
void spotlight_cleanup(Display *dpy);

// Called from signal handlers to tell the event loop to
// exit cleanly on the next iteration.
void spotlight_request_quit(void);

#endif // SPOTLIGHT_H
