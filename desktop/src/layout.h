// CopyCatOS — by Kyle Blizzard at Blizzard.show

// layout.h — Desktop spatial workspace: free-form icon placement
//
// Snow Leopard's identity feature: icons stay exactly where you put them.
// Drag a folder to the middle of the desktop, log out, log back in — the
// folder is still in the middle of the desktop, at the same pixel.
//
// Storage is per-file xattr `user.moonbase.position`, holding two ASCII
// integers in points: "x y". Points (not pixels) so the position survives
// HiDPI scale changes — every paint multiplies through S() to land in
// physical pixels for the current output scale.
//
// Why xattr (not a central index file): positions move with the file.
// Rename ~/Desktop/Notes.txt → ~/Desktop/Old Notes.txt and the position
// follows. Move it to /tmp and back, the position follows. No central
// registry to fall out of sync, no orphaned entries when files vanish.
//
// Filesystems without xattr support (FAT32, exFAT, some NFS) fall through
// to auto-place every login. A sidecar fallback is a future slice; ext4
// and btrfs are the daily path on the dev target and any modern Linux box.
//
// Usage:
//   1. layout_apply(icons, count, sw, sh)  — after scan_desktop()
//   2. layout_save_position(icon)          — after a single drag end
//   3. layout_clear_position(icon)         — Clean Up reverts to auto-place

#ifndef CC_LAYOUT_H
#define CC_LAYOUT_H

#include "icons.h"

// Read xattr-stored positions for any icons that have one, then auto-place
// the rest in canonical Snow Leopard order (top-right → down → left).
// Icons with a stored xattr keep their exact pixel-XY (translated from
// stored points through the current desktop scale). Icons without one
// fill the next free grid cell — the cell containing each xattr-positioned
// icon's center is reserved so a fresh file doesn't auto-place under it.
void layout_apply(DesktopIcon *icons, int count, int screen_w, int screen_h);

// Persist one icon's current pixel position as `user.moonbase.position`
// on its file. Stored as ASCII "x y" in points so it stays correct after
// docking to a different-scale display. Called from icons_drag_end after
// a successful drag.
void layout_save_position(const DesktopIcon *icon);

// Remove the position xattr for one icon — reverts that icon to the
// auto-place grid the next time layout_apply runs. Called from Clean Up.
void layout_clear_position(const DesktopIcon *icon);

#endif // CC_LAYOUT_H
