// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ─── search.h ───
// Public interface for the desktop-file search engine.
//
// At startup we scan standard XDG application directories for .desktop
// files and build an in-memory catalogue.  When the user types a query
// we filter that catalogue using substring matching, with a priority
// sort so that name-prefix matches come first.

#ifndef SEARCH_H
#define SEARCH_H

#include <cairo/cairo.h>
#include <stdbool.h>

// ──────────────────────────────────────────────
// Data structures
// ──────────────────────────────────────────────

// One entry in the application catalogue, parsed from a .desktop file.
typedef struct {
    char name[256];            // Human-readable app name (Name=)
    char generic_name[256];    // Generic description   (GenericName=)
    char exec[512];            // Command line           (Exec=)
    char icon_name[256];       // Icon theme name        (Icon=)
    char keywords[512];        // Semicolon-separated    (Keywords=)

    // Precomputed lowercase blob used for fast substring matching.
    // Built as: lowercase(name + " " + generic_name + " " + keywords)
    char search_blob[1024];

    // Resolved icon surface (32x32 pixels).  Loaded lazily — starts
    // as NULL and gets filled in the first time this entry is rendered.
    cairo_surface_t *icon;
} SearchEntry;

// ──────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────

// Scan all standard .desktop directories and populate the
// internal catalogue.  Call once at startup.
void search_init(void);

// Free every catalogue entry and its icon surface.
void search_cleanup(void);

// Run a search against the catalogue.
//
// query        — the user's current input string (may be empty)
// out_results  — caller-provided array of SearchEntry pointers
// max_results  — size of that array (use MAX_SEARCH_RESULTS)
//
// Returns the number of results written into out_results (0..max_results).
int search_query(const char *query, SearchEntry **out_results, int max_results);

// Return the total number of entries in the catalogue (for diagnostics).
int search_entry_count(void);

#endif // SEARCH_H
