// CopyCatOS — by Kyle Blizzard at Blizzard.show

// icon_bucket.h — pure helpers for picking a hicolor size bucket from
// a PNG icon's actual dimensions. Used by moonbase-launcher's XDG
// registration path so a 512×512 AppIcon.png lands in
// `hicolor/512x512/apps/` instead of the previous always-128 fallback.
//
// Header-only static-inline so the launcher and the unit test can share
// the implementation without spinning up a separate TU. The functions
// are pure: no allocation, no I/O, no errno. The caller does the file
// read; these only parse the bytes and pick a bucket.

#ifndef MB_LAUNCHER_ICON_BUCKET_H
#define MB_LAUNCHER_ICON_BUCKET_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Parse a PNG IHDR out of the first 24 bytes of a file. Layout:
//   bytes 0..7   PNG signature (89 50 4E 47 0D 0A 1A 0A)
//   bytes 8..11  first chunk length (skipped — IHDR is always 13)
//   bytes 12..15 chunk type "IHDR"
//   bytes 16..19 width  (big-endian uint32)
//   bytes 20..23 height (big-endian uint32)
// Returns 0 on success and writes width/height; -1 if the bytes are
// not a PNG IHDR. v1 only handles PNG — Info.appc's icon convention
// is AppIcon.png.
static inline int mb_png_dims_from_buf(const uint8_t *buf24,
                                       uint32_t *width,
                                       uint32_t *height) {
    static const uint8_t sig[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
    };
    if (memcmp(buf24, sig, 8) != 0) return -1;
    if (memcmp(buf24 + 12, "IHDR", 4) != 0) return -1;
    *width  = ((uint32_t)buf24[16] << 24) | ((uint32_t)buf24[17] << 16)
            | ((uint32_t)buf24[18] <<  8) | ((uint32_t)buf24[19]);
    *height = ((uint32_t)buf24[20] << 24) | ((uint32_t)buf24[21] << 16)
            | ((uint32_t)buf24[22] <<  8) | ((uint32_t)buf24[23]);
    return 0;
}

// Pick the largest hicolor bucket that's ≤ `pixels` (the icon's shorter
// side). Rule: never claim more resolution than the source has. A 512
// icon goes to 512, a 256 to 256, an oddly-sized 100 rounds *down* to
// 96 — the DE will scale it up for any larger ask, same as it would
// have done from 128. Pixels < 16 fall into the 16 bucket since
// hicolor's smallest reliably-indexed size across GNOME/KDE/XFCE is 16.
//
// Buckets are the ten the freedesktop hicolor theme spec lists that
// every major DE indexes. 22 / 36 / 72 are KDE-historical and skipped
// to avoid landing in a directory the host's icon cache won't pick up.
static inline int mb_hicolor_bucket_for(uint32_t pixels) {
    static const int buckets[] = {
        512, 256, 192, 128, 96, 64, 48, 32, 24, 16
    };
    for (size_t i = 0; i < sizeof buckets / sizeof buckets[0]; i++) {
        if (pixels >= (uint32_t)buckets[i]) return buckets[i];
    }
    return 16;
}

#endif // MB_LAUNCHER_ICON_BUCKET_H
