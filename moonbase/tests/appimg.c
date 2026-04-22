// CopyCatOS — by Kyle Blizzard at Blizzard.show

// mb-appimg — unit test for appimg.c. Builds synthetic single-file
// `.app` trailers in tmpfiles and drives every branch of
// mb_appimg_read_trailer / mb_appimg_is_single_file:
//
//   * happy path: real values, correct magic, bundle-id round-trip
//   * short file (footer pread short-circuits)
//   * wrong magic_end (not a single-file .app)
//   * wrong magic_start (bad magic)
//   * version = 999 (bad version)
//   * oversize trailer_size (bad layout)
//   * squashfs span not matching file_size - trailer_size (bad layout)
//   * empty bundle_id (bad bundle-id)
//   * oversize bundle_id (bad bundle-id)
//   * NUL embedded in bundle_id (bad bundle-id)
//   * is_single_file on a directory + missing path + appimg file
//
// The writer here lives in the test because the packer tool that
// will own the real writer lives in a later slice (17-A). Trailer
// layout must stay in lock-step with appimg.h — that's the whole
// reason both sides of the parser/writer split cite the same header.

#include "bundle/appimg.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int fail_count = 0;

#define EXPECT(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); \
        fail_count++; \
    } \
} while (0)

// -----------------------------------------------------------------
// writer helpers — mirror the on-disk layout declared in appimg.h.
// These mutate freely for negative tests; the happy path wires them
// up clean. Field packs use htole32/htole64 exactly as the real
// packer will.
// -----------------------------------------------------------------

typedef struct {
    // Body bytes placed before the trailer — stub + squashfs
    // payload. For these tests we write N bytes of 0xAB as "stub"
    // and S bytes of 0xCD as "squashfs image"; the parser doesn't
    // inspect either region, only its declared offset and length.
    size_t stub_len;
    size_t squashfs_len;
    // Trailer fields.
    uint8_t  magic_start[8];
    uint32_t version;
    uint64_t squashfs_offset;   // normally stub_len
    uint64_t squashfs_size;     // normally squashfs_len
    uint32_t bundle_id_len;
    const uint8_t *bundle_id;   // bundle_id_len bytes
    uint32_t trailer_size;      // normally 44 + bundle_id_len
    uint8_t  magic_end[8];
} appimg_fixture_t;

static void fixture_defaults(appimg_fixture_t *f,
                             const char *bundle_id) {
    memset(f, 0, sizeof(*f));
    f->stub_len = 64;
    f->squashfs_len = 128;
    memcpy(f->magic_start, MB_APPIMG_MAGIC, MB_APPIMG_MAGIC_LEN);
    memcpy(f->magic_end,   MB_APPIMG_MAGIC, MB_APPIMG_MAGIC_LEN);
    f->version = MB_APPIMG_TRAILER_VERSION;
    f->squashfs_offset = (uint64_t)f->stub_len;
    f->squashfs_size   = (uint64_t)f->squashfs_len;
    f->bundle_id_len   = (uint32_t)strlen(bundle_id);
    f->bundle_id       = (const uint8_t *)bundle_id;
    f->trailer_size    = 44 + f->bundle_id_len;
}

// Write the fixture to a freshly-created tmpfile under /tmp. Returns
// a heap-allocated absolute path on success; caller unlinks + frees.
// NULL on any I/O failure.
static char *fixture_write(const appimg_fixture_t *f) {
    char tmpl[] = "/tmp/mb-appimg-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return NULL;

    uint8_t *stub_bytes = malloc(f->stub_len);
    uint8_t *squash_bytes = malloc(f->squashfs_len);
    if (!stub_bytes || !squash_bytes) goto fail;
    memset(stub_bytes, 0xAB, f->stub_len);
    memset(squash_bytes, 0xCD, f->squashfs_len);

    if (write(fd, stub_bytes, f->stub_len) != (ssize_t)f->stub_len) goto fail;
    if (write(fd, squash_bytes, f->squashfs_len) != (ssize_t)f->squashfs_len) goto fail;

    if (write(fd, f->magic_start, MB_APPIMG_MAGIC_LEN) != MB_APPIMG_MAGIC_LEN) goto fail;
    uint32_t v_le = htole32(f->version);
    if (write(fd, &v_le, 4) != 4) goto fail;
    uint64_t off_le = htole64(f->squashfs_offset);
    if (write(fd, &off_le, 8) != 8) goto fail;
    uint64_t sz_le = htole64(f->squashfs_size);
    if (write(fd, &sz_le, 8) != 8) goto fail;
    uint32_t len_le = htole32(f->bundle_id_len);
    if (write(fd, &len_le, 4) != 4) goto fail;
    if (f->bundle_id_len &&
        write(fd, f->bundle_id, f->bundle_id_len) != (ssize_t)f->bundle_id_len) goto fail;
    uint32_t ts_le = htole32(f->trailer_size);
    if (write(fd, &ts_le, 4) != 4) goto fail;
    if (write(fd, f->magic_end, MB_APPIMG_MAGIC_LEN) != MB_APPIMG_MAGIC_LEN) goto fail;

    free(stub_bytes);
    free(squash_bytes);
    close(fd);
    return strdup(tmpl);

fail:
    free(stub_bytes);
    free(squash_bytes);
    close(fd);
    unlink(tmpl);
    return NULL;
}

// -----------------------------------------------------------------
// tests
// -----------------------------------------------------------------

static void test_happy_path(void) {
    appimg_fixture_t f;
    fixture_defaults(&f, "show.blizzard.textedit");
    char *path = fixture_write(&f);
    EXPECT(path != NULL, "fixture_write failed");
    if (!path) return;

    mb_appimg_trailer_t t;
    char err[256];
    mb_appimg_err_t e = mb_appimg_read_trailer_path(path, &t, err, sizeof(err));
    EXPECT(e == MB_APPIMG_OK, "happy: err=%d (%s): %s", e,
           mb_appimg_err_string(e), err);
    EXPECT(t.version == MB_APPIMG_TRAILER_VERSION, "version mismatch");
    EXPECT(t.squashfs_offset == 64, "offset mismatch");
    EXPECT(t.squashfs_size == 128, "size mismatch");
    EXPECT(t.bundle_id != NULL, "bundle_id null");
    EXPECT(t.bundle_id_len == strlen("show.blizzard.textedit"), "bid len");
    EXPECT(strcmp(t.bundle_id, "show.blizzard.textedit") == 0,
           "bid contents: %s", t.bundle_id ? t.bundle_id : "(null)");

    bool detected = false;
    mb_appimg_err_t de = mb_appimg_is_single_file(path, &detected);
    EXPECT(de == MB_APPIMG_OK && detected, "is_single_file true-path");

    mb_appimg_trailer_free(&t);
    EXPECT(t.bundle_id == NULL, "free should NULL out bundle_id");
    unlink(path);
    free(path);
}

static void test_short_file(void) {
    // Write fewer than 12 bytes — footer pread can't even land.
    char tmpl[] = "/tmp/mb-appimg-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) {
        uint8_t data[4] = {1, 2, 3, 4};
        (void)!write(fd, data, 4);
        close(fd);
    }
    mb_appimg_trailer_t t;
    char err[128];
    mb_appimg_err_t e = mb_appimg_read_trailer_path(tmpl, &t, err, sizeof(err));
    EXPECT(e == MB_APPIMG_ERR_TOO_SMALL, "short-file err=%d (%s)",
           e, mb_appimg_err_string(e));
    mb_appimg_trailer_free(&t);
    unlink(tmpl);
}

static void test_wrong_magic_end(void) {
    appimg_fixture_t f;
    fixture_defaults(&f, "show.blizzard.textedit");
    memcpy(f.magic_end, "XXXXXXXX", 8);
    char *path = fixture_write(&f);
    if (!path) { EXPECT(0, "fixture_write failed"); return; }

    mb_appimg_trailer_t t;
    char err[128];
    mb_appimg_err_t e = mb_appimg_read_trailer_path(path, &t, err, sizeof(err));
    EXPECT(e == MB_APPIMG_ERR_NOT_SINGLE_FILE,
           "wrong magic_end -> not-single-file, got %d (%s)",
           e, mb_appimg_err_string(e));

    bool detected = true;
    mb_appimg_err_t de = mb_appimg_is_single_file(path, &detected);
    EXPECT(de == MB_APPIMG_OK && !detected,
           "is_single_file false-path on bad magic_end");

    mb_appimg_trailer_free(&t);
    unlink(path);
    free(path);
}

static void test_wrong_magic_start(void) {
    appimg_fixture_t f;
    fixture_defaults(&f, "show.blizzard.textedit");
    memcpy(f.magic_start, "YYYYYYYY", 8);
    char *path = fixture_write(&f);
    if (!path) { EXPECT(0, "fixture_write failed"); return; }

    mb_appimg_trailer_t t;
    char err[128];
    mb_appimg_err_t e = mb_appimg_read_trailer_path(path, &t, err, sizeof(err));
    EXPECT(e == MB_APPIMG_ERR_BAD_MAGIC,
           "wrong magic_start -> bad-magic, got %d (%s)",
           e, mb_appimg_err_string(e));

    mb_appimg_trailer_free(&t);
    unlink(path);
    free(path);
}

static void test_bad_version(void) {
    appimg_fixture_t f;
    fixture_defaults(&f, "show.blizzard.textedit");
    f.version = 999;
    char *path = fixture_write(&f);
    if (!path) { EXPECT(0, "fixture_write failed"); return; }

    mb_appimg_trailer_t t;
    char err[128];
    mb_appimg_err_t e = mb_appimg_read_trailer_path(path, &t, err, sizeof(err));
    EXPECT(e == MB_APPIMG_ERR_BAD_VERSION,
           "version=999 -> bad-version, got %d (%s)",
           e, mb_appimg_err_string(e));

    mb_appimg_trailer_free(&t);
    unlink(path);
    free(path);
}

static void test_trailer_size_too_big(void) {
    appimg_fixture_t f;
    fixture_defaults(&f, "show.blizzard.textedit");
    // Claim the trailer is enormous; writer still lays the real bytes.
    // Outer footer's trailer_size field is what the reader trusts
    // first — override that to exceed MB_APPIMG_TRAILER_MAX.
    f.trailer_size = MB_APPIMG_TRAILER_MAX + 1;
    char *path = fixture_write(&f);
    if (!path) { EXPECT(0, "fixture_write failed"); return; }

    mb_appimg_trailer_t t;
    char err[128];
    mb_appimg_err_t e = mb_appimg_read_trailer_path(path, &t, err, sizeof(err));
    EXPECT(e == MB_APPIMG_ERR_BAD_LAYOUT,
           "oversize trailer -> bad-layout, got %d (%s)",
           e, mb_appimg_err_string(e));

    mb_appimg_trailer_free(&t);
    unlink(path);
    free(path);
}

static void test_squashfs_span_mismatch(void) {
    appimg_fixture_t f;
    fixture_defaults(&f, "show.blizzard.textedit");
    // Declare offset+size that don't add up to file_size - trailer.
    // Shift the declared offset up by 32 — the actual payload is
    // still at offset 64; the reader catches the mismatch.
    f.squashfs_offset = 96;
    char *path = fixture_write(&f);
    if (!path) { EXPECT(0, "fixture_write failed"); return; }

    mb_appimg_trailer_t t;
    char err[128];
    mb_appimg_err_t e = mb_appimg_read_trailer_path(path, &t, err, sizeof(err));
    EXPECT(e == MB_APPIMG_ERR_BAD_LAYOUT,
           "span mismatch -> bad-layout, got %d (%s)",
           e, mb_appimg_err_string(e));

    mb_appimg_trailer_free(&t);
    unlink(path);
    free(path);
}

static void test_empty_bundle_id(void) {
    appimg_fixture_t f;
    fixture_defaults(&f, "x");
    f.bundle_id_len = 0;
    f.trailer_size  = 44;   // 44 + 0
    char *path = fixture_write(&f);
    if (!path) { EXPECT(0, "fixture_write failed"); return; }

    mb_appimg_trailer_t t;
    char err[128];
    mb_appimg_err_t e = mb_appimg_read_trailer_path(path, &t, err, sizeof(err));
    // trailer_size of 44 < the minimum 45 the reader accepts, so
    // it lands as BAD_LAYOUT before hitting the bundle-id check —
    // either bucket is acceptable as "this isn't mountable".
    EXPECT(e == MB_APPIMG_ERR_BAD_BUNDLE_ID ||
           e == MB_APPIMG_ERR_BAD_LAYOUT,
           "empty bundle_id -> bad-bundle-id/layout, got %d (%s)",
           e, mb_appimg_err_string(e));

    mb_appimg_trailer_free(&t);
    unlink(path);
    free(path);
}

static void test_oversize_bundle_id(void) {
    appimg_fixture_t f;
    // 257 chars — one over the cap. Use a repeating letter so any
    // byte is valid printable.
    char big[258];
    memset(big, 'a', 257);
    big[257] = '\0';
    fixture_defaults(&f, big);   // sets bundle_id_len to 257
    char *path = fixture_write(&f);
    if (!path) { EXPECT(0, "fixture_write failed"); return; }

    mb_appimg_trailer_t t;
    char err[128];
    mb_appimg_err_t e = mb_appimg_read_trailer_path(path, &t, err, sizeof(err));
    EXPECT(e == MB_APPIMG_ERR_BAD_BUNDLE_ID,
           "oversize bundle_id -> bad-bundle-id, got %d (%s)",
           e, mb_appimg_err_string(e));

    mb_appimg_trailer_free(&t);
    unlink(path);
    free(path);
}

static void test_nul_in_bundle_id(void) {
    appimg_fixture_t f;
    fixture_defaults(&f, "show.blizzard.textedit");
    // Overwrite the bundle_id with something containing an embedded NUL.
    static const uint8_t bid[] = {'s','h','o','w','\0','b','a','d'};
    f.bundle_id = bid;
    f.bundle_id_len = 8;
    f.trailer_size  = 44 + 8;
    char *path = fixture_write(&f);
    if (!path) { EXPECT(0, "fixture_write failed"); return; }

    mb_appimg_trailer_t t;
    char err[128];
    mb_appimg_err_t e = mb_appimg_read_trailer_path(path, &t, err, sizeof(err));
    EXPECT(e == MB_APPIMG_ERR_BAD_BUNDLE_ID,
           "embedded NUL -> bad-bundle-id, got %d (%s)",
           e, mb_appimg_err_string(e));

    mb_appimg_trailer_free(&t);
    unlink(path);
    free(path);
}

static void test_is_single_file_edge_cases(void) {
    // Missing path: returns OK + false without error.
    bool detected = true;
    mb_appimg_err_t e = mb_appimg_is_single_file(
        "/tmp/definitely-not-a-path-12345xyz", &detected);
    EXPECT(e == MB_APPIMG_OK && !detected,
           "is_single_file on ENOENT -> OK+false, got %d det=%d",
           e, detected);

    // A directory: regular-file check short-circuits to OK+false.
    detected = true;
    e = mb_appimg_is_single_file("/tmp", &detected);
    EXPECT(e == MB_APPIMG_OK && !detected,
           "is_single_file on directory -> OK+false, got %d det=%d",
           e, detected);
}

int main(void) {
    test_happy_path();
    test_short_file();
    test_wrong_magic_end();
    test_wrong_magic_start();
    test_bad_version();
    test_trailer_size_too_big();
    test_squashfs_span_mismatch();
    test_empty_bundle_id();
    test_oversize_bundle_id();
    test_nul_in_bundle_id();
    test_is_single_file_edge_cases();

    if (fail_count) {
        fprintf(stderr, "%d assertion(s) failed\n", fail_count);
        return 1;
    }
    printf("all appimg tests passed\n");
    return 0;
}
