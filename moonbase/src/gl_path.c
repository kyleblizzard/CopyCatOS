// CopyCatOS — by Kyle Blizzard at Blizzard.show

// gl_path.c — EGL pbuffer + glReadPixels client-side GL handoff.
//
// This is the v1 MOONBASE_RENDER_GL internal path. It trades zero-
// copy performance for architectural simplicity — MoonRock doesn't
// learn anything new, the existing ARGB32 SHM commit path carries GL
// frames the same way it carries Cairo frames. The cost is one
// glReadPixels per frame at swap time (GPU → CPU sync).
//
// Layout:
//
//   * One process-wide EGLDisplay + EGLConfig + EGLContext. The
//     context is GLES 3.0, alpha-capable (ARGB32 to match the Cairo
//     path), and is bound to EGL_PLATFORM_SURFACELESS_MESA. We avoid
//     the native-platform EGL plumbing (X11, Wayland) because every
//     window already lives under MoonRock's X11 compositor — all the
//     client side needs is an offscreen surface to render into.
//
//   * One per-window EGLSurface (pbuffer), sized to the window's
//     current backing-pixel dimensions. The app renders into its
//     default framebuffer.
//
// Thread model: v1 only supports one thread driving GL at a time —
// the main thread. eglMakeCurrent is a thread-local call, so a future
// multi-thread GL story can come later without changing the ABI.

#include "gl_path.h"
#include "internal.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------
// Process-wide EGL state
// ---------------------------------------------------------------------

static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;
static EGLDisplay     g_display   = EGL_NO_DISPLAY;
static EGLConfig      g_config    = (EGLConfig)0;
static EGLContext     g_context   = EGL_NO_CONTEXT;
static mb_error_t     g_init_err  = MB_EIPC;   // MB_EIPC until init succeeds

// EGL_PLATFORM_SURFACELESS_MESA is the right platform for "just give
// me a GL context, I'll render into FBOs and pbuffers." It avoids any
// X11/Wayland coupling on the client side and doesn't require
// /dev/dri for software fallback (Mesa's llvmpipe). Hardware GL
// still works when /dev/dri is bound into the sandbox.
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

static void gl_init_once_cb(void) {
    // Resolve eglGetPlatformDisplay at runtime; ancient EGL headers
    // don't have the prototype but every Mesa in our target range
    // exposes the symbol.
    PFNEGLGETPLATFORMDISPLAYEXTPROC egl_get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)
        eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = EGL_NO_DISPLAY;
    if (egl_get_platform_display) {
        dpy = egl_get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                                       EGL_DEFAULT_DISPLAY, NULL);
    }
    if (dpy == EGL_NO_DISPLAY) {
        // Fall back to eglGetDisplay on a platform we don't fully
        // know about. Software llvmpipe still ends up here on very
        // old Mesa.
        dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (dpy == EGL_NO_DISPLAY) {
        g_init_err = MB_EIPC;
        return;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        g_init_err = MB_EIPC;
        return;
    }

    // Bind the ES API — the context we create must match. GLES 3.0 is
    // a safe floor on anything from the last decade.
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        eglTerminate(dpy);
        g_init_err = MB_EIPC;
        return;
    }

    // Request an ARGB32-class config with depth + stencil so the app
    // can do the usual 3D-style setup without having to negotiate. No
    // multisample — the app that wants it can render into its own
    // FBO with GL_RENDERBUFFER_SAMPLES.
    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE,     EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE,  EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,         8,
        EGL_GREEN_SIZE,       8,
        EGL_BLUE_SIZE,        8,
        EGL_ALPHA_SIZE,       8,
        EGL_DEPTH_SIZE,       24,
        EGL_STENCIL_SIZE,     8,
        EGL_NONE,
    };
    EGLConfig cfg = (EGLConfig)0;
    EGLint    num_cfg = 0;
    if (!eglChooseConfig(dpy, config_attrs, &cfg, 1, &num_cfg) || num_cfg < 1) {
        eglTerminate(dpy);
        g_init_err = MB_EIPC;
        return;
    }

    const EGLint ctx_attrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE,
    };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT) {
        eglTerminate(dpy);
        g_init_err = MB_EIPC;
        return;
    }

    g_display = dpy;
    g_config  = cfg;
    g_context = ctx;
    g_init_err = MB_EOK;
}

mb_error_t mb_gl_path_init(void) {
    pthread_once(&g_init_once, gl_init_once_cb);
    return g_init_err;
}

// ---------------------------------------------------------------------
// Per-window state
// ---------------------------------------------------------------------
//
// We own the pbuffer but not the context — it's process-wide. On a
// backing-scale change window.c calls mb_gl_window_destroy() + creates
// a fresh block; we release the old pbuffer here.

struct mb_gl_window {
    EGLSurface surface;
    int        px_w;
    int        px_h;
};

mb_error_t mb_gl_window_create(int px_w, int px_h, mb_gl_window_t **out) {
    if (!out || px_w <= 0 || px_h <= 0) return MB_EINVAL;
    mb_error_t e = mb_gl_path_init();
    if (e != MB_EOK) return e;

    const EGLint surf_attrs[] = {
        EGL_WIDTH,  px_w,
        EGL_HEIGHT, px_h,
        EGL_NONE,
    };
    EGLSurface surf = eglCreatePbufferSurface(g_display, g_config, surf_attrs);
    if (surf == EGL_NO_SURFACE) return MB_EIPC;

    mb_gl_window_t *w = calloc(1, sizeof(*w));
    if (!w) {
        eglDestroySurface(g_display, surf);
        return MB_ENOMEM;
    }
    w->surface = surf;
    w->px_w    = px_w;
    w->px_h    = px_h;
    *out = w;
    return MB_EOK;
}

void mb_gl_window_destroy(mb_gl_window_t *win) {
    if (!win) return;
    if (g_display != EGL_NO_DISPLAY && win->surface != EGL_NO_SURFACE) {
        // If this surface is current we must release it before destroy,
        // otherwise EGL keeps it alive until the next MakeCurrent.
        if (eglGetCurrentSurface(EGL_DRAW) == win->surface) {
            eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                           EGL_NO_CONTEXT);
        }
        eglDestroySurface(g_display, win->surface);
    }
    free(win);
}

mb_error_t mb_gl_window_make_current(mb_gl_window_t *win) {
    if (!win) return MB_EINVAL;
    if (g_display == EGL_NO_DISPLAY || g_context == EGL_NO_CONTEXT) {
        return MB_EIPC;
    }
    if (!eglMakeCurrent(g_display, win->surface, win->surface, g_context)) {
        return MB_EIPC;
    }
    // Default viewport to the whole pbuffer. Apps that want a sub-rect
    // can call glViewport themselves after this returns.
    glViewport(0, 0, win->px_w, win->px_h);
    return MB_EOK;
}

// Byte-swap routine for the post-ReadPixels transpose. glReadPixels
// hands back bytes in GL_RGBA order; Cairo + MoonRock both expect
// native-byte-order ARGB32 premul. On little-endian x86 and ARM that
// means BGRA in memory. We do the shuffle row-by-row and also
// premultiply alpha — since GL doesn't premultiply by default, a
// GL_ONE/GL_ONE_MINUS_SRC_ALPHA compositor will get wrong results
// otherwise.
//
// The buffer dst has pitch `stride_bytes`, which may differ from
// 4*w_px because the Cairo-path stride calculation rounds up to a
// 4-byte boundary (always true for ARGB32 anyway, but we respect it).
static void rgba_to_bgra_premul(const uint8_t *src, int src_pitch,
                                uint8_t *dst, int dst_pitch,
                                int w_px, int h_px) {
    for (int y = 0; y < h_px; y++) {
        const uint8_t *sr = src + (size_t)y * (size_t)src_pitch;
        uint8_t *dr = dst + (size_t)y * (size_t)dst_pitch;
        for (int x = 0; x < w_px; x++) {
            uint8_t r = sr[4*x + 0];
            uint8_t g = sr[4*x + 1];
            uint8_t b = sr[4*x + 2];
            uint8_t a = sr[4*x + 3];
            // premultiply; round-to-nearest via +127.
            uint32_t pr = (uint32_t)r * a + 127;
            uint32_t pg = (uint32_t)g * a + 127;
            uint32_t pb = (uint32_t)b * a + 127;
            dr[4*x + 0] = (uint8_t)((pb + (pb >> 8)) >> 8); // B
            dr[4*x + 1] = (uint8_t)((pg + (pg >> 8)) >> 8); // G
            dr[4*x + 2] = (uint8_t)((pr + (pr >> 8)) >> 8); // R
            dr[4*x + 3] = a;                                 // A
        }
    }
}

mb_error_t mb_gl_window_read_framebuffer(mb_gl_window_t *win,
                                         int stride_bytes,
                                         void *dst) {
    if (!win || !dst || stride_bytes < win->px_w * 4) return MB_EINVAL;
    if (g_display == EGL_NO_DISPLAY) return MB_EIPC;

    // Make sure the context is current on this thread before issuing
    // GL calls — apps that roll their own main loop may have made
    // other surfaces current between render and swap.
    if (!eglMakeCurrent(g_display, win->surface, win->surface, g_context)) {
        return MB_EIPC;
    }

    // Read the default framebuffer. GL_READ_FRAMEBUFFER defaults to
    // the same binding as GL_FRAMEBUFFER, so if the app left an FBO
    // bound its contents get read (right behavior — that is their
    // frame). Apps that want to explicitly read the pbuffer should
    // bind 0 before swap.
    glFinish();

    // Align strictest to whatever stride we were given. ARGB32 is
    // always 4-byte aligned so GL_PACK_ALIGNMENT=4 is safe.
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, stride_bytes / 4);

    // Allocate a scratch buffer matching the pbuffer so we can do the
    // RGBA→BGRA transpose in one sweep. GL_BGRA is widely supported
    // via GL_EXT_read_format_bgra, but not universally — this path
    // stays portable.
    uint8_t *scratch = malloc((size_t)stride_bytes * (size_t)win->px_h);
    if (!scratch) {
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        return MB_ENOMEM;
    }
    glReadPixels(0, 0, win->px_w, win->px_h, GL_RGBA, GL_UNSIGNED_BYTE, scratch);

    // Rows arrive bottom-up from GL's coordinate system. Flip while
    // we swap channels and premultiply. We write into dst directly
    // with y_dst = (h - 1 - y_src).
    for (int y = 0; y < win->px_h; y++) {
        const uint8_t *sr = scratch + (size_t)y * (size_t)stride_bytes;
        uint8_t *dr = (uint8_t *)dst
                    + (size_t)(win->px_h - 1 - y) * (size_t)stride_bytes;
        rgba_to_bgra_premul(sr, stride_bytes, dr, stride_bytes,
                            win->px_w, 1);
    }
    free(scratch);

    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    return MB_EOK;
}

void mb_gl_window_pixel_size(const mb_gl_window_t *win,
                             int *px_w, int *px_h) {
    if (!win) {
        if (px_w) *px_w = 0;
        if (px_h) *px_h = 0;
        return;
    }
    if (px_w) *px_w = win->px_w;
    if (px_h) *px_h = win->px_h;
}
