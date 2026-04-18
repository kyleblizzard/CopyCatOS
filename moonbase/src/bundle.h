// CopyCatOS — by Kyle Blizzard at Blizzard.show

// bundle.h — .appc bundle manifest reader
//
// A CopyCatOS application bundle is a directory that ends in .appc and
// has this structure:
//
//   MyApp.appc/
//     Contents/
//       Info.appc              <- plaintext key=value manifest
//       MacOS/MyApp              <- main executable (for native C apps)
//       Resources/                 <- images, localized strings, etc.
//       Frameworks/                <- bundled shared libraries
//
// Info.appc minimal fields:
//
//   CFBundleName      = MyApp                (display name)
//   CFBundleVersion   = 1.0.0
//   Host              = native|swift|python|web
//   Executable        = Contents/MacOS/MyApp (relative path inside bundle)
//
// This module reads and validates Info.appc so main.c can dispatch to the
// right language host. It is deliberately tiny for the v0.1 scaffold —
// more validation and asset resolution get added later.

#ifndef MOONBASE_BUNDLE_H
#define MOONBASE_BUNDLE_H

// mb_bundle — Parsed representation of an .appc bundle on disk.
// Owned memory (strings) is freed by bundle_close().
struct mb_bundle {
    char *bundle_path;       // Absolute path to the .appc directory
    char *display_name;      // CFBundleName from Info.appc
    char *host_kind;         // Host field: "native", "swift", "python", "web"
    char *executable_path;   // Absolute path to the main executable
};

// bundle_open — Open a bundle on disk and parse its Info.appc.
// Returns NULL on any error (missing directory, missing Info.appc,
// missing required fields). Caller must bundle_close() the result.
struct mb_bundle *bundle_open(const char *bundle_path);

// bundle_close — Release all memory owned by a bundle struct.
// Safe to pass NULL.
void bundle_close(struct mb_bundle *bundle);

#endif
