// CopyCatOS — by Kyle Blizzard at Blizzard.show

// gl-path.c — unit test for src/gl_path.c.
//
// Does not need moonrock. Drives mb_gl_path straight:
//
//   1. Init EGL.
//   2. Create a 64×32-pixel pbuffer.
//   3. Make it current.
//   4. glClear to a known colour.
//   5. Read the framebuffer back through the same function the
//      public moonbase_window_gl_swap_buffers uses.
//   6. Assert the corners match ARGB32 premul with the expected
//      bytes in native-endian order.
//
// If this passes, the client side of the GL ABI is wired correctly
// without needing a compositor on the other end.

#include "gl_path.h"

#include <GLES3/gl3.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int argb32_stride_for(int w) { return ((w * 4) + 3) & ~3; }

int main(void) {
    mb_error_t e = mb_gl_path_init();
    if (e != MB_EOK) {
        // Software EGL (llvmpipe) is enough for this test on any
        // Mesa-capable host, but some CI environments strip it. Skip
        // cleanly rather than fail — meson treats exit 77 as SKIP.
        fprintf(stderr, "gl-path: EGL init failed (%d) — skipping\n", (int)e);
        return 77;
    }

    const int px_w = 64;
    const int px_h = 32;
    mb_gl_window_t *glw = NULL;
    assert(mb_gl_window_create(px_w, px_h, &glw) == MB_EOK);
    assert(glw != NULL);

    int out_w = 0, out_h = 0;
    mb_gl_window_pixel_size(glw, &out_w, &out_h);
    assert(out_w == px_w && out_h == px_h);

    assert(mb_gl_window_make_current(glw) == MB_EOK);

    // Clear to fully-opaque orange. With premultiplied output and
    // alpha = 255, the read-back channels should be exactly these.
    // glClearColor is in [0,1] float; precompute the expected 8-bit
    // ints so the assertion is unambiguous.
    const float r_f = 1.0f, g_f = 0.5f, b_f = 0.25f, a_f = 1.0f;
    glClearColor(r_f, g_f, b_f, a_f);
    glClear(GL_COLOR_BUFFER_BIT);

    int stride = argb32_stride_for(px_w);
    uint8_t *buf = calloc((size_t)stride * (size_t)px_h, 1);
    assert(buf != NULL);
    assert(mb_gl_window_read_framebuffer(glw, stride, buf) == MB_EOK);

    // Check the corners. Native-byte-order ARGB32 premul on little-
    // endian is [B, G, R, A] in memory. At α=255 the premultiplied
    // and straight values coincide.
    //
    // Rounding: Mesa's glClearColor → 8-bit conversion is round-to-
    // nearest with ties-to-even, so 0.5 → 128 and 0.25 → 64. Accept
    // ±1 to cover vendor differences.
    uint8_t *top_left     = buf + 0 * stride + 0;
    uint8_t *bottom_right = buf + (px_h - 1) * stride + (px_w - 1) * 4;
    for (uint8_t *p = top_left; p != NULL; p = (p == top_left ? bottom_right : NULL)) {
        int B = p[0], G = p[1], R = p[2], A = p[3];
        assert(A == 255);
        assert(abs(R - 255) <= 1);
        assert(abs(G - 128) <= 1);
        assert(abs(B -  64) <= 1);
    }
    printf("PASS: EGL pbuffer + glReadPixels + ARGB32 premul swizzle\n");

    free(buf);
    mb_gl_window_destroy(glw);
    return 0;
}
