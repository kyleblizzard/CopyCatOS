// CopyCatOS — by Kyle Blizzard at Blizzard.show

// mb-icon-bucket — covers the two pure helpers in
// runtime/icon_bucket.h that pick a hicolor size bucket from the
// dimensions in a PNG IHDR. The launcher's xdg_register uses these
// to land an AppIcon.png in the correct hicolor/<N>x<N>/apps
// directory instead of the historical always-128 fallback.
//
// Tests:
//   1. PNG signature mismatch is rejected.
//   2. "IHDR" chunk-type mismatch is rejected.
//   3. A well-formed 512x512 IHDR yields width=height=512.
//   4. Big-endian width/height parsing handles values that span
//      every byte (e.g. 0x12345678).
//   5. Bucket selection rounds down to the largest standard bucket.
//   6. Bucket selection floors at 16 for tiny icons.
//   7. Bucket selection saturates at 512 for oversized icons.

#include "../runtime/icon_bucket.h"

#include <stdio.h>
#include <string.h>

#define FAIL(msg, ...) do {                       \
    fprintf(stderr, "FAIL: " msg "\n", ##__VA_ARGS__); \
    return 1;                                     \
} while (0)

static void make_ihdr(uint8_t buf[24], uint32_t w, uint32_t h) {
    static const uint8_t sig[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
    };
    memcpy(buf, sig, 8);
    // length field — 13 — not inspected by the parser; fill with the
    // canonical PNG value so the buffer is byte-accurate to a real file.
    buf[8]  = 0x00; buf[9]  = 0x00; buf[10] = 0x00; buf[11] = 0x0D;
    memcpy(buf + 12, "IHDR", 4);
    buf[16] = (uint8_t)(w >> 24); buf[17] = (uint8_t)(w >> 16);
    buf[18] = (uint8_t)(w >>  8); buf[19] = (uint8_t)(w);
    buf[20] = (uint8_t)(h >> 24); buf[21] = (uint8_t)(h >> 16);
    buf[22] = (uint8_t)(h >>  8); buf[23] = (uint8_t)(h);
}

int main(void) {
    uint8_t buf[24];
    uint32_t w = 0, h = 0;

    // 1. signature mismatch
    memset(buf, 0, sizeof buf);
    if (mb_png_dims_from_buf(buf, &w, &h) == 0) {
        FAIL("zero buffer accepted as PNG");
    }

    // 2. chunk-type mismatch (correct sig, wrong IHDR tag)
    make_ihdr(buf, 128, 128);
    memcpy(buf + 12, "IDAT", 4);
    if (mb_png_dims_from_buf(buf, &w, &h) == 0) {
        FAIL("non-IHDR first chunk accepted");
    }

    // 3. canonical 512x512
    make_ihdr(buf, 512, 512);
    if (mb_png_dims_from_buf(buf, &w, &h) != 0) {
        FAIL("well-formed IHDR rejected");
    }
    if (w != 512 || h != 512) FAIL("expected 512x512, got %ux%u", w, h);

    // 4. big-endian round-trip on a value that uses every byte
    make_ihdr(buf, 0x12345678u, 0x9ABCDEF0u);
    if (mb_png_dims_from_buf(buf, &w, &h) != 0) {
        FAIL("big-endian IHDR rejected");
    }
    if (w != 0x12345678u || h != 0x9ABCDEF0u) {
        FAIL("big-endian decode wrong: w=%08x h=%08x", w, h);
    }

    // 5. bucket selection rounds down to the largest standard size
    struct { uint32_t in; int expect; } cases[] = {
        {  512, 512 }, // exact
        { 1024, 512 }, // saturates at 512
        {  511, 256 }, // 511 < 512 → 256
        {  256, 256 },
        {  255, 192 },
        {  192, 192 },
        {  191, 128 },
        {  128, 128 },
        {  127,  96 },
        {  100,  96 }, // 100 → 96, not 128
        {   96,  96 },
        {   65,  64 },
        {   64,  64 },
        {   63,  48 },
        {   48,  48 },
        {   33,  32 },
        {   32,  32 },
        {   24,  24 },
        {   16,  16 },
        {   15,  16 }, // floors at 16
        {    1,  16 },
        {    0,  16 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int got = mb_hicolor_bucket_for(cases[i].in);
        if (got != cases[i].expect) {
            FAIL("bucket(%u) = %d, expected %d",
                 cases[i].in, got, cases[i].expect);
        }
    }

    return 0;
}
