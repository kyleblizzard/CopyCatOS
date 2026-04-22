// CopyCatOS — by Kyle Blizzard at Blizzard.show

// appimg — single-file `.app` trailer format (shared between
// moonbase-launch and moonbase-pack).
//
// A shipping `.app` is one executable file: a static ELF stub, an
// appended squashfs image, and a fixed-shape trailer at EOF. The
// stub's only job is `execvp("moonbase-launch", "/proc/self/exe",
// argv[1..])`; moonbase-launch reads the trailer, mounts the
// squashfs at `$XDG_RUNTIME_DIR/moonbase/mounts/<bundle-id>-<pid>/`
// via squashfuse, and hands that path to the existing bundle loader.
//
// The trailer carries bundle-id so the mount path is computable
// without mounting the squashfs first (chicken-and-egg: bundle-id
// lives inside Info.appc inside the squashfs). Everything else in
// the trailer exists so a corrupted file is rejected before we
// touch the mount subsystem.
//
// On-disk layout, little-endian, packed, no padding between fields:
//
//   [0 .. N)                static ELF stub bytes
//   [N .. N + S)            squashfs image bytes
//   [N + S .. N + S + T)    trailer (this struct, serialized)
//
// Trailer serialization (all multi-byte fields little-endian):
//
//   +0       char     magic_start[8]    = "MBAPP\0\0\1"
//   +8       uint32   version           = MB_APPIMG_TRAILER_VERSION
//   +12      uint64   squashfs_offset   = N (absolute, start of file)
//   +20      uint64   squashfs_size     = S
//   +28      uint32   bundle_id_len     = L (bytes, no NUL on disk)
//   +32      char     bundle_id[L]
//   +32+L    uint32   trailer_size      = T (this trailer's own
//                                            length; self-describing
//                                            so the reader can pread
//                                            from EOF)
//   +36+L    char     magic_end[8]      = "MBAPP\0\0\1"
//
// Read path:
//   1. pread the last 12 bytes of the file — trailer_size (4) +
//      magic_end (8).
//   2. Validate magic_end; pread `trailer_size` bytes ending at
//      (EOF - 8) to capture the full trailer.
//   3. Validate magic_start and version; extract fields with
//      le32toh / le64toh.
//
// Invariants the reader must enforce (any violation -> BAD_LAYOUT):
//   - trailer_size >= 32 + bundle_id_len + 4 + 8
//   - squashfs_offset + squashfs_size == file_size - trailer_size
//   - bundle_id_len in (0, MB_APPIMG_BUNDLE_ID_MAX]
//
// Endianness: serialize + deserialize with htole32/htole64 on the
// writer side and le32toh/le64toh on the reader side (both from
// <endian.h>). Never cast the buffer to a struct pointer — pull one
// field at a time with memcpy into a native integer first.
//
// Version bump rule: any layout change (new field, reordering, field
// widening) bumps MB_APPIMG_TRAILER_VERSION. Readers reject unknown
// versions loudly — we do not silently ignore future fields.
//
// bundle_id_offset deliberately omitted: it's always
// (file_size - trailer_size) + 32 by construction, so storing it
// would only create a way for writer/reader to disagree.

#ifndef MOONBASE_BUNDLE_APPIMG_H
#define MOONBASE_BUNDLE_APPIMG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The 8-byte magic sentinel that appears at both the trailer start
// and the very last 8 bytes of the file. The last byte (0x01) is a
// format-family marker — not the trailer version, which lives in
// its own uint32 field.
#define MB_APPIMG_MAGIC "MBAPP\0\0\1"
#define MB_APPIMG_MAGIC_LEN 8

// Current trailer layout revision. Bump on any on-disk shape change.
#define MB_APPIMG_TRAILER_VERSION 1

// Hard caps to keep malformed files from triggering giant allocations
// before validation fails. bundle-id per bundle-spec is reverse-DNS;
// 256 bytes is generous. Trailer itself is tiny.
#define MB_APPIMG_BUNDLE_ID_MAX  256
#define MB_APPIMG_TRAILER_MAX   1024

typedef enum {
    MB_APPIMG_OK = 0,
    MB_APPIMG_ERR_NOT_SINGLE_FILE,   // path exists but has no trailer magic
    MB_APPIMG_ERR_IO,                // open/read/pread failed; see errno
    MB_APPIMG_ERR_TOO_SMALL,         // file shorter than trailer footer
    MB_APPIMG_ERR_BAD_MAGIC,         // magic_start or magic_end mismatch
    MB_APPIMG_ERR_BAD_VERSION,       // version != MB_APPIMG_TRAILER_VERSION
    MB_APPIMG_ERR_BAD_LAYOUT,        // offsets/sizes inconsistent w/ file size
    MB_APPIMG_ERR_BAD_BUNDLE_ID,     // bundle_id empty or > _MAX
    MB_APPIMG_ERR_NO_MEM,
} mb_appimg_err_t;

// In-memory view of a successfully parsed trailer. bundle_id is a
// heap-allocated NUL-terminated copy (the on-disk form has no NUL).
typedef struct {
    uint32_t version;
    uint64_t squashfs_offset;
    uint64_t squashfs_size;
    char    *bundle_id;    // owned; mb_appimg_trailer_free releases
    size_t   bundle_id_len;
} mb_appimg_trailer_t;

// Quick probe: does `path` look like a single-file `.app`? Returns
// MB_APPIMG_OK with *out set either way; returns a non-OK code only
// on I/O error. A regular `.appdev` directory or any non-appimg file
// yields MB_APPIMG_OK with *out == false.
mb_appimg_err_t mb_appimg_is_single_file(const char *path, bool *out);

// Parse the trailer from an already-open fd (caller keeps ownership).
// On success, *out is populated and the caller releases the
// bundle_id copy via mb_appimg_trailer_free. On failure, *out is
// zeroed and err_buf (may be NULL) gets a short diagnostic.
mb_appimg_err_t mb_appimg_read_trailer(int fd,
                                       mb_appimg_trailer_t *out,
                                       char *err_buf, size_t err_cap);

// Convenience — open, parse, close.
mb_appimg_err_t mb_appimg_read_trailer_path(const char *path,
                                            mb_appimg_trailer_t *out,
                                            char *err_buf, size_t err_cap);

// Free heap fields inside *t. Safe to call on a zeroed struct.
void mb_appimg_trailer_free(mb_appimg_trailer_t *t);

// Short human-readable name for an error code.
const char *mb_appimg_err_string(mb_appimg_err_t e);

// Serialise a trailer to `fd` at its current file offset. Caller is
// responsible for having already written the stub bytes at [0, off)
// and the squashfs image at [off, off + size) before calling — the
// trailer itself is the last write. On success returns MB_APPIMG_OK;
// on failure, err_buf (may be NULL) gets a short diagnostic.
//
// Layout written matches mb_appimg_read_trailer's parser exactly, so
// a writer + reader round-trip on the same inputs is lossless.
mb_appimg_err_t mb_appimg_write_trailer(int fd,
                                        uint64_t squashfs_offset,
                                        uint64_t squashfs_size,
                                        const char *bundle_id,
                                        char *err_buf, size_t err_cap);

#endif
