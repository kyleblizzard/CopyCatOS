// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ─── icons.h ───
// Public interface for icon resolution and caching.
//
// Given an icon *name* from a .desktop file (e.g. "firefox") this
// module searches the icon theme hierarchy, loads the image as a
// Cairo surface, scales it to 32x32, and caches the result so the
// same icon is never loaded twice.

#ifndef ICONS_H
#define ICONS_H

#include <cairo/cairo.h>

// Look up (and cache) an icon by its theme name.
// Returns a 32x32 Cairo image surface, or a generic fallback
// icon if the name cannot be resolved to a file on disk.
// The returned surface is owned by the cache — do NOT destroy it.
cairo_surface_t *icon_lookup(const char *icon_name);

// Free every cached surface.  Call once at shutdown.
void icon_cache_cleanup(void);

#endif // ICONS_H
