// CopyCatOS — by Kyle Blizzard at Blizzard.show

// appimg — reader for the single-file `.app` trailer format declared
// in appimg.h. Shared between moonbase-launch (detect + mount) and
// the later moonbase-pack tool (validate what it just wrote).
//
// Read path uses two preads:
//   1. Last 12 bytes → trailer_size (4) + magic_end (8). Cheap reject
//      path for anything that isn't a single-file `.app`.
//   2. `trailer_size` bytes ending at EOF - 8 → the whole trailer.
//      Parsed field-by-field with memcpy into native uint32/uint64
//      locals followed by le32toh / le64toh. The buffer is never
//      reinterpret-cast to a struct pointer — packed-struct pitfalls
//      and platform-endianness bugs both stay out of the code.
//
// Every validation failure maps to a specific MB_APPIMG_ERR_* code so
// the launcher can produce a human-readable reason without second-
// guessing the parser.

#include "appimg.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// -------------------------------------------------------------------
// helpers
// -------------------------------------------------------------------

static void set_err(char *buf, size_t cap, const char *fmt, ...) {
    if (!buf || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
}

// Full pread — loops until `len` bytes land at `buf` or a short read
// / error stops us. Returns 0 on success, -1 on I/O error (errno
// preserved), -2 on short read (EOF before `len`).
static int pread_full(int fd, void *buf, size_t len, off_t off) {
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = pread(fd, p + got, len - got, off + (off_t)got);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -2;
        got += (size_t)n;
    }
    return 0;
}

// -------------------------------------------------------------------
// mb_appimg_read_trailer — core parser
// -------------------------------------------------------------------

mb_appimg_err_t mb_appimg_read_trailer(int fd,
                                       mb_appimg_trailer_t *out,
                                       char *err_buf, size_t err_cap) {
    if (out) memset(out, 0, sizeof(*out));
    if (fd < 0 || !out) {
        set_err(err_buf, err_cap, "invalid arguments");
        return MB_APPIMG_ERR_IO;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        set_err(err_buf, err_cap, "fstat failed: %s", strerror(errno));
        return MB_APPIMG_ERR_IO;
    }
    if (!S_ISREG(st.st_mode)) {
        set_err(err_buf, err_cap, "not a regular file");
        return MB_APPIMG_ERR_NOT_SINGLE_FILE;
    }

    off_t file_size = st.st_size;

    // Minimum possible trailer: magic_start(8) + version(4) +
    // squashfs_offset(8) + squashfs_size(8) + bundle_id_len(4) +
    // bundle_id[1] + trailer_size(4) + magic_end(8) = 45 bytes. Plus
    // the squashfs image and stub above it — so anything smaller than
    // the last-12 footer is simply not an appimg.
    if (file_size < (off_t)(4 + MB_APPIMG_MAGIC_LEN)) {
        set_err(err_buf, err_cap, "file too small (%lld bytes)",
                (long long)file_size);
        return MB_APPIMG_ERR_TOO_SMALL;
    }

    // Step 1: pread the last 12 bytes.
    uint8_t footer[12];
    int r = pread_full(fd, footer, sizeof(footer),
                       file_size - (off_t)sizeof(footer));
    if (r == -1) {
        set_err(err_buf, err_cap, "pread footer: %s", strerror(errno));
        return MB_APPIMG_ERR_IO;
    }
    if (r == -2) {
        set_err(err_buf, err_cap, "short read on footer");
        return MB_APPIMG_ERR_TOO_SMALL;
    }

    if (memcmp(footer + 4, MB_APPIMG_MAGIC, MB_APPIMG_MAGIC_LEN) != 0) {
        set_err(err_buf, err_cap, "magic_end mismatch");
        return MB_APPIMG_ERR_NOT_SINGLE_FILE;
    }

    uint32_t trailer_size_le;
    memcpy(&trailer_size_le, footer, 4);
    uint32_t trailer_size = le32toh(trailer_size_le);

    // Reject out-of-range trailer_size before we trust it for pread.
    // Minimum layout with bundle_id_len == 1 is 45 bytes.
    if (trailer_size < 45 || trailer_size > MB_APPIMG_TRAILER_MAX) {
        set_err(err_buf, err_cap,
                "trailer_size %u out of range", trailer_size);
        return MB_APPIMG_ERR_BAD_LAYOUT;
    }
    if ((off_t)trailer_size > file_size) {
        set_err(err_buf, err_cap,
                "trailer_size %u exceeds file size %lld",
                trailer_size, (long long)file_size);
        return MB_APPIMG_ERR_BAD_LAYOUT;
    }

    // Step 2: pread the whole trailer.
    uint8_t *buf = malloc(trailer_size);
    if (!buf) {
        set_err(err_buf, err_cap, "malloc(%u) failed", trailer_size);
        return MB_APPIMG_ERR_NO_MEM;
    }

    off_t trailer_start = file_size - (off_t)trailer_size;
    r = pread_full(fd, buf, trailer_size, trailer_start);
    if (r == -1) {
        set_err(err_buf, err_cap, "pread trailer: %s", strerror(errno));
        free(buf);
        return MB_APPIMG_ERR_IO;
    }
    if (r == -2) {
        set_err(err_buf, err_cap, "short read on trailer");
        free(buf);
        return MB_APPIMG_ERR_TOO_SMALL;
    }

    // Validate magic_start.
    if (memcmp(buf, MB_APPIMG_MAGIC, MB_APPIMG_MAGIC_LEN) != 0) {
        set_err(err_buf, err_cap, "magic_start mismatch");
        free(buf);
        return MB_APPIMG_ERR_BAD_MAGIC;
    }

    // Field extraction — never cast buf to a struct pointer; packed
    // struct layouts and cross-platform endianness are both foot-
    // guns. memcpy into native, then le*toh.
    size_t off = MB_APPIMG_MAGIC_LEN;

    uint32_t version_le;
    memcpy(&version_le, buf + off, 4);
    off += 4;
    uint32_t version = le32toh(version_le);

    if (version != MB_APPIMG_TRAILER_VERSION) {
        set_err(err_buf, err_cap,
                "trailer version %u unsupported (want %u)",
                version, (unsigned)MB_APPIMG_TRAILER_VERSION);
        free(buf);
        return MB_APPIMG_ERR_BAD_VERSION;
    }

    uint64_t squashfs_offset_le;
    memcpy(&squashfs_offset_le, buf + off, 8);
    off += 8;
    uint64_t squashfs_offset = le64toh(squashfs_offset_le);

    uint64_t squashfs_size_le;
    memcpy(&squashfs_size_le, buf + off, 8);
    off += 8;
    uint64_t squashfs_size = le64toh(squashfs_size_le);

    uint32_t bundle_id_len_le;
    memcpy(&bundle_id_len_le, buf + off, 4);
    off += 4;
    uint32_t bundle_id_len = le32toh(bundle_id_len_le);

    if (bundle_id_len == 0 || bundle_id_len > MB_APPIMG_BUNDLE_ID_MAX) {
        set_err(err_buf, err_cap,
                "bundle_id_len %u out of range (1..%u)",
                bundle_id_len, (unsigned)MB_APPIMG_BUNDLE_ID_MAX);
        free(buf);
        return MB_APPIMG_ERR_BAD_BUNDLE_ID;
    }

    // trailer_size must cover: magic_start + version + offset + size
    // + len + bundle_id + trailer_size + magic_end.
    uint32_t expected_trailer = MB_APPIMG_MAGIC_LEN + 4 + 8 + 8 + 4
                              + bundle_id_len + 4 + MB_APPIMG_MAGIC_LEN;
    if (trailer_size != expected_trailer) {
        set_err(err_buf, err_cap,
                "trailer_size %u != expected %u for bundle_id_len %u",
                trailer_size, expected_trailer, bundle_id_len);
        free(buf);
        return MB_APPIMG_ERR_BAD_LAYOUT;
    }

    // Layout invariant: squashfs_offset + squashfs_size ==
    // file_size - trailer_size. A mismatch means the trailer was
    // mutated or the file was truncated; either way, don't mount.
    if (squashfs_offset > (uint64_t)file_size ||
        squashfs_size > (uint64_t)file_size ||
        squashfs_offset + squashfs_size !=
            (uint64_t)(file_size - (off_t)trailer_size)) {
        set_err(err_buf, err_cap,
                "squashfs span %llu+%llu != %lld - trailer %u",
                (unsigned long long)squashfs_offset,
                (unsigned long long)squashfs_size,
                (long long)file_size, trailer_size);
        free(buf);
        return MB_APPIMG_ERR_BAD_LAYOUT;
    }

    // Copy bundle_id out with a NUL terminator for callers. On-disk
    // form has no NUL; in-memory form always does.
    char *bundle_id = malloc((size_t)bundle_id_len + 1);
    if (!bundle_id) {
        set_err(err_buf, err_cap, "malloc bundle_id failed");
        free(buf);
        return MB_APPIMG_ERR_NO_MEM;
    }
    memcpy(bundle_id, buf + off, bundle_id_len);
    bundle_id[bundle_id_len] = '\0';
    off += bundle_id_len;

    // Reject embedded NULs — bundle_id is a printable reverse-DNS
    // string per bundle-spec; a NUL here is a malformed trailer.
    if (memchr(bundle_id, '\0', bundle_id_len) != NULL) {
        set_err(err_buf, err_cap, "bundle_id contains embedded NUL");
        free(bundle_id);
        free(buf);
        return MB_APPIMG_ERR_BAD_BUNDLE_ID;
    }

    // Sanity-check the self-describing trailer_size field against
    // what we just pread (belt-and-braces; already validated shape).
    uint32_t trailer_size_field_le;
    memcpy(&trailer_size_field_le, buf + off, 4);
    off += 4;
    uint32_t trailer_size_field = le32toh(trailer_size_field_le);
    if (trailer_size_field != trailer_size) {
        set_err(err_buf, err_cap,
                "trailer_size field %u != outer footer %u",
                trailer_size_field, trailer_size);
        free(bundle_id);
        free(buf);
        return MB_APPIMG_ERR_BAD_LAYOUT;
    }

    // magic_end inside the full trailer must match what we already
    // saw in the footer. Checking again here keeps the per-field
    // walk honest.
    if (memcmp(buf + off, MB_APPIMG_MAGIC, MB_APPIMG_MAGIC_LEN) != 0) {
        set_err(err_buf, err_cap, "magic_end mismatch (full-trailer)");
        free(bundle_id);
        free(buf);
        return MB_APPIMG_ERR_BAD_MAGIC;
    }

    out->version         = version;
    out->squashfs_offset = squashfs_offset;
    out->squashfs_size   = squashfs_size;
    out->bundle_id       = bundle_id;
    out->bundle_id_len   = bundle_id_len;

    free(buf);
    return MB_APPIMG_OK;
}

// -------------------------------------------------------------------
// mb_appimg_read_trailer_path — open + parse + close
// -------------------------------------------------------------------

mb_appimg_err_t mb_appimg_read_trailer_path(const char *path,
                                            mb_appimg_trailer_t *out,
                                            char *err_buf, size_t err_cap) {
    if (out) memset(out, 0, sizeof(*out));
    if (!path || !out) {
        set_err(err_buf, err_cap, "invalid arguments");
        return MB_APPIMG_ERR_IO;
    }

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        set_err(err_buf, err_cap, "open %s: %s", path, strerror(errno));
        return MB_APPIMG_ERR_IO;
    }

    mb_appimg_err_t e = mb_appimg_read_trailer(fd, out, err_buf, err_cap);
    close(fd);
    return e;
}

// -------------------------------------------------------------------
// mb_appimg_is_single_file — cheap detector
// -------------------------------------------------------------------

mb_appimg_err_t mb_appimg_is_single_file(const char *path, bool *out) {
    if (out) *out = false;
    if (!path || !out) return MB_APPIMG_ERR_IO;

    struct stat st;
    if (stat(path, &st) != 0) {
        // Missing path is not an I/O error from our caller's view —
        // fileviewer asks "is this a single-file .app?" on paths it
        // already resolved. Surface ENOENT through errno but treat
        // the answer as "no".
        if (errno == ENOENT) return MB_APPIMG_OK;
        return MB_APPIMG_ERR_IO;
    }
    if (!S_ISREG(st.st_mode)) return MB_APPIMG_OK;
    if (st.st_size < (off_t)(4 + MB_APPIMG_MAGIC_LEN)) return MB_APPIMG_OK;

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return MB_APPIMG_ERR_IO;

    uint8_t footer[12];
    int r = pread_full(fd, footer, sizeof(footer),
                       st.st_size - (off_t)sizeof(footer));
    close(fd);
    if (r == -1) return MB_APPIMG_ERR_IO;
    if (r == -2) return MB_APPIMG_OK;   // short file — not an appimg

    if (memcmp(footer + 4, MB_APPIMG_MAGIC, MB_APPIMG_MAGIC_LEN) == 0) {
        *out = true;
    }
    return MB_APPIMG_OK;
}

// -------------------------------------------------------------------
// mb_appimg_trailer_free
// -------------------------------------------------------------------

void mb_appimg_trailer_free(mb_appimg_trailer_t *t) {
    if (!t) return;
    free(t->bundle_id);
    memset(t, 0, sizeof(*t));
}

// -------------------------------------------------------------------
// mb_appimg_err_string
// -------------------------------------------------------------------

const char *mb_appimg_err_string(mb_appimg_err_t e) {
    switch (e) {
    case MB_APPIMG_OK:                   return "ok";
    case MB_APPIMG_ERR_NOT_SINGLE_FILE:  return "not a single-file .app";
    case MB_APPIMG_ERR_IO:               return "io error";
    case MB_APPIMG_ERR_TOO_SMALL:        return "file too small";
    case MB_APPIMG_ERR_BAD_MAGIC:        return "bad magic";
    case MB_APPIMG_ERR_BAD_VERSION:      return "unsupported trailer version";
    case MB_APPIMG_ERR_BAD_LAYOUT:       return "bad trailer layout";
    case MB_APPIMG_ERR_BAD_BUNDLE_ID:    return "bad bundle-id";
    case MB_APPIMG_ERR_NO_MEM:           return "out of memory";
    }
    return "unknown";
}
