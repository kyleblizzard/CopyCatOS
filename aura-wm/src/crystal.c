// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.
//
// AuraOS — Crystal Compositor (Phase 1)
//
// Crystal is AuraOS's built-in OpenGL compositor, replacing picom. Instead of
// relying on an external program to composite windows, Crystal handles it
// directly inside the window manager using OpenGL for GPU-accelerated rendering.
//
// How compositing works at a high level:
//   Every window on your screen is normally drawn directly to the display by
//   the X server. A compositor intercepts this — it tells X to draw each window
//   to an off-screen image (called a "pixmap") instead. The compositor then
//   takes all those images, layers them on top of each other (back-to-front,
//   like stacking transparencies), adds effects like shadows, and draws the
//   final combined image to the screen.
//
// Crystal's approach:
//   1. XComposite redirects all windows to off-screen pixmaps (Manual mode —
//      we handle ALL rendering, unlike the old compositor's Automatic mode).
//   2. Each window's pixmap is bound as an OpenGL texture using the
//      GLX_EXT_texture_from_pixmap extension (or a CPU fallback if unavailable).
//   3. Every frame, we draw textured quads (rectangles) for each window at its
//      screen position, back-to-front, with alpha blending for transparency.
//   4. Shadows are drawn as concentric rectangles behind each window (Phase 1).
//   5. Double-buffered via GLX — we draw to a back buffer, then swap it to the
//      screen, preventing flicker.
//   6. VSync keeps us locked to the monitor's refresh rate (typically 60 Hz),
//      preventing screen tearing.

#define _GNU_SOURCE  // Needed for M_PI from <math.h>
#include "crystal.h"
#include "decor.h"

// Forward declaration removed — compositor.c is no longer linked.
// crystal_create_argb_visual() now handles fallback internally.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// OpenGL and GLX headers — these give us access to GPU rendering on X11.
// GL/gl.h:      Core OpenGL functions (drawing, textures, blending)
// GL/glx.h:     GLX — the bridge between OpenGL and X11 (contexts, surfaces)
// GL/glxext.h:  GLX extensions (texture_from_pixmap, swap control, etc.)
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>

// X11 extension headers for compositing, damage tracking, and input shapes
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>

// ────────────────────────────────────────────────────────────────────────
// SECTION: GLX extension function pointers
// ────────────────────────────────────────────────────────────────────────
//
// OpenGL extensions aren't part of the base API — their functions must be
// loaded at runtime using glXGetProcAddress(). We store pointers to these
// functions so we can call them later. If the extension isn't available,
// the pointer stays NULL and we skip that feature.
//
// "PFNGLX...PROC" is the naming convention for OpenGL function pointer types:
//   PFN = "Pointer to FunctioN"
//   GLX = the GLX subsystem
//   ...EXT = it's an extension function

// glXBindTexImageEXT: Binds an X pixmap to an OpenGL texture. This is the
// key function that lets us use window contents as GPU textures without
// copying pixel data through the CPU.
static PFNGLXBINDTEXIMAGEEXTPROC glXBindTexImageEXT_func = NULL;

// glXReleaseTexImageEXT: Releases a pixmap that was bound as a texture.
// Must be called before re-binding or destroying the pixmap.
static PFNGLXRELEASETEXIMAGEEXTPROC glXReleaseTexImageEXT_func = NULL;

// glXSwapIntervalEXT: Controls VSync (vertical synchronization).
// Setting the interval to 1 means "wait for one monitor refresh before
// swapping buffers," which prevents screen tearing.
static PFNGLXSWAPINTERVALEXTPROC glXSwapIntervalEXT_func = NULL;

// ────────────────────────────────────────────────────────────────────────
// SECTION: Per-window texture tracking
// ────────────────────────────────────────────────────────────────────────
//
// Each visible window on screen needs a corresponding OpenGL texture so we
// can draw it. This struct tracks the relationship between an X window and
// its GPU texture, plus metadata we need for rendering.

struct WindowTexture {
    Window xwin;            // The X11 window ID (the frame window, not client)
    Pixmap pixmap;          // The off-screen pixmap from XComposite
    GLXPixmap glx_pixmap;   // GLX wrapper around the pixmap (for texture binding)
    GLuint texture;         // OpenGL texture ID (the GPU-side handle)
    Damage damage;          // XDamage handle — notifies us when contents change
    int x, y, w, h;        // Window position and size on screen
    bool dirty;             // True if the texture needs refreshing (contents changed)
    bool has_alpha;         // True if the window uses a 32-bit ARGB visual (transparency)
    bool bound;             // True if the texture is currently bound to a pixmap
};

// ────────────────────────────────────────────────────────────────────────
// SECTION: Module state (file-scope static)
// ────────────────────────────────────────────────────────────────────────
//
// All Crystal state lives in this single struct. Using a static struct at
// file scope means only crystal.c can access it, keeping the compositor's
// internals private from the rest of the window manager.

static struct {
    bool active;                    // Is Crystal initialized and running?

    // ── GLX / OpenGL context ──
    // A "GLX context" is the bridge between X11 and OpenGL. It holds all
    // OpenGL state (current texture, blend mode, etc.) and must be "made
    // current" before any GL calls will work.
    GLXContext gl_context;

    // FBConfig = "Framebuffer Configuration" — describes the pixel format
    // of the rendering surface (color depth, double buffering, alpha, etc.).
    // We need one that supports texture_from_pixmap for binding X pixmaps.
    GLXFBConfig fb_config;

    // The GLX drawable we render to — this is essentially the root window
    // wrapped in a GLX surface so OpenGL can draw to it.
    GLXWindow gl_window;

    // Screen dimensions — needed for the orthographic projection matrix
    // that maps pixel coordinates to OpenGL's coordinate system.
    int root_w, root_h;

    // ── XDamage event tracking ──
    // XDamage events don't have a fixed event type number. Instead, the X
    // server assigns a base number at runtime. A DamageNotify event has type
    // (damage_event_base + XDamageNotify). We store the base here.
    int damage_event_base;
    int damage_error_base;

    // ── Extension support flags ──
    // Not all GPU drivers support GLX_EXT_texture_from_pixmap. If they don't,
    // we fall back to reading pixmap data through the CPU (slower but universal).
    bool has_texture_from_pixmap;

    // ── Per-window texture array ──
    // We track up to MAX_CLIENTS windows (matching the WM's client limit).
    // Each entry maps an X window to its OpenGL texture.
    struct WindowTexture windows[MAX_CLIENTS];
    int window_count;

    // ── ARGB visual for transparent frame windows ──
    // Frame windows need a 32-bit visual (with alpha channel) so the shadow
    // regions can be semi-transparent. We find this visual during init and
    // share it with the frame creation code.
    Visual *argb_visual;
    Colormap argb_colormap;

    // ── FBConfig for pixmap binding ──
    // A separate FBConfig used specifically for creating GLX pixmaps from
    // X pixmaps. This may differ from the rendering FBConfig because pixmap
    // binding has its own requirements (GLX_BIND_TO_TEXTURE_RGBA_EXT, etc.).
    GLXFBConfig pixmap_fb_config;
    GLXFBConfig pixmap_fb_config_rgb;  // For non-alpha (24-bit) windows

    // ── Desktop background color ──
    // Cleared to this color each frame. Dark blue-gray by default.
    float bg_r, bg_g, bg_b;
} crystal;

// ────────────────────────────────────────────────────────────────────────
// SECTION: Forward declarations (private helper functions)
// ────────────────────────────────────────────────────────────────────────

static void draw_window_shadow(struct WindowTexture *wt, bool focused);
static void refresh_window_texture(AuraWM *wm, struct WindowTexture *wt);
static void refresh_window_texture_fallback(AuraWM *wm, struct WindowTexture *wt);
static void release_window_texture(AuraWM *wm, struct WindowTexture *wt);
static struct WindowTexture *find_window_texture(Window xwin);
static bool choose_fb_configs(Display *dpy, int screen);
static bool load_glx_extensions(Display *dpy, int screen);

// ────────────────────────────────────────────────────────────────────────
// SECTION: FBConfig selection
// ────────────────────────────────────────────────────────────────────────
//
// An FBConfig (Framebuffer Configuration) describes how pixels are stored
// in a rendering surface: how many bits for red, green, blue, alpha,
// whether it's double-buffered, etc.
//
// We need TWO kinds of FBConfig:
//   1. A "rendering" FBConfig for the GLX context (what we draw to)
//   2. A "pixmap" FBConfig for binding X pixmaps as textures
//
// These often differ because the pixmap config needs GLX_BIND_TO_TEXTURE_*
// support, which not all configs provide.

static bool choose_fb_configs(Display *dpy, int screen)
{
    // ── Rendering FBConfig ──
    // This is for the main drawing surface. We want:
    //   - RGBA color (8 bits per channel)
    //   - Double buffering (draw to back buffer, then swap — no flicker)
    //   - Window drawable type (we're rendering to a window, not a pixmap)
    int render_attrs[] = {
        GLX_RENDER_TYPE,    GLX_RGBA_BIT,      // Color channels: R, G, B, A
        GLX_DRAWABLE_TYPE,  GLX_WINDOW_BIT,     // We'll render to a window
        GLX_DOUBLEBUFFER,   True,               // Double buffer to avoid flicker
        GLX_RED_SIZE,       8,                  // 8 bits per color channel
        GLX_GREEN_SIZE,     8,
        GLX_BLUE_SIZE,      8,
        GLX_ALPHA_SIZE,     8,                  // Alpha channel for transparency
        None                                    // Null terminator for the list
    };

    int num_configs = 0;
    GLXFBConfig *configs = glXChooseFBConfig(dpy, screen, render_attrs, &num_configs);
    if (!configs || num_configs == 0) {
        fprintf(stderr, "[crystal] ERROR: No suitable FBConfig for rendering\n");
        if (configs) XFree(configs);
        return false;
    }

    // Take the first matching config — the X server returns them sorted by
    // preference (fewest extra features first, best match at index 0).
    crystal.fb_config = configs[0];
    XFree(configs);

    // ── Pixmap FBConfig (RGBA — for 32-bit windows with alpha) ──
    // For binding X pixmaps as OpenGL textures, we need configs that declare
    // support for GLX_BIND_TO_TEXTURE_RGBA_EXT. This is a separate query
    // because not all rendering configs support pixmap binding.
    int pixmap_attrs_rgba[] = {
        GLX_RENDER_TYPE,                GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE,              GLX_PIXMAP_BIT,     // Must support pixmap drawables
        GLX_BIND_TO_TEXTURE_RGBA_EXT,   True,               // Can bind RGBA pixmaps as textures
        GLX_RED_SIZE,                   8,
        GLX_GREEN_SIZE,                 8,
        GLX_BLUE_SIZE,                  8,
        GLX_ALPHA_SIZE,                 8,
        None
    };

    configs = glXChooseFBConfig(dpy, screen, pixmap_attrs_rgba, &num_configs);
    if (configs && num_configs > 0) {
        crystal.pixmap_fb_config = configs[0];
        XFree(configs);
    } else {
        fprintf(stderr, "[crystal] WARNING: No FBConfig for RGBA pixmap binding\n");
        if (configs) XFree(configs);
        // Not fatal — we'll fall back to CPU texture upload
    }

    // ── Pixmap FBConfig (RGB — for 24-bit windows without alpha) ──
    int pixmap_attrs_rgb[] = {
        GLX_RENDER_TYPE,                GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE,              GLX_PIXMAP_BIT,
        GLX_BIND_TO_TEXTURE_RGB_EXT,    True,               // Can bind RGB pixmaps as textures
        GLX_RED_SIZE,                   8,
        GLX_GREEN_SIZE,                 8,
        GLX_BLUE_SIZE,                  8,
        None
    };

    configs = glXChooseFBConfig(dpy, screen, pixmap_attrs_rgb, &num_configs);
    if (configs && num_configs > 0) {
        crystal.pixmap_fb_config_rgb = configs[0];
        XFree(configs);
    } else {
        fprintf(stderr, "[crystal] WARNING: No FBConfig for RGB pixmap binding\n");
        if (configs) XFree(configs);
    }

    return true;
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: GLX extension loading
// ────────────────────────────────────────────────────────────────────────
//
// GLX extensions add capabilities beyond the base GLX spec. We need to:
//   1. Check which extensions are available (by searching a string list)
//   2. Load the function pointers for those extensions
//
// This is standard practice in OpenGL programming — extensions are always
// loaded at runtime because different drivers support different extensions.

static bool load_glx_extensions(Display *dpy, int screen)
{
    // glXQueryExtensionsString returns a space-separated list of all GLX
    // extensions supported by this driver. We search it for the ones we need.
    const char *exts = glXQueryExtensionsString(dpy, screen);
    if (!exts) {
        fprintf(stderr, "[crystal] WARNING: Cannot query GLX extensions\n");
        return false;
    }

    // ── GLX_EXT_texture_from_pixmap ──
    // This is the most important extension for a compositor. It lets us bind
    // an X pixmap (off-screen window contents) directly as an OpenGL texture,
    // avoiding a slow CPU copy. Without it, we must use XGetImage() to read
    // the pixmap into CPU memory, then upload it to the GPU with glTexImage2D.
    if (strstr(exts, "GLX_EXT_texture_from_pixmap")) {
        // Load the function pointers using glXGetProcAddress.
        // The (const GLubyte*) cast is required by the API signature.
        glXBindTexImageEXT_func = (PFNGLXBINDTEXIMAGEEXTPROC)
            glXGetProcAddress((const GLubyte *)"glXBindTexImageEXT");
        glXReleaseTexImageEXT_func = (PFNGLXRELEASETEXIMAGEEXTPROC)
            glXGetProcAddress((const GLubyte *)"glXReleaseTexImageEXT");

        // Both functions must be present for the extension to work
        if (glXBindTexImageEXT_func && glXReleaseTexImageEXT_func) {
            crystal.has_texture_from_pixmap = true;
            fprintf(stderr, "[crystal] GLX_EXT_texture_from_pixmap available "
                    "(fast path)\n");
        } else {
            fprintf(stderr, "[crystal] WARNING: texture_from_pixmap functions "
                    "not loadable\n");
        }
    } else {
        fprintf(stderr, "[crystal] GLX_EXT_texture_from_pixmap NOT available "
                "(using CPU fallback)\n");
    }

    // ── GLX_EXT_swap_control ──
    // Controls VSync. When enabled (interval=1), the buffer swap waits for
    // the monitor's vertical blank period. This prevents "screen tearing" —
    // a visual artifact where the top half of the screen shows one frame and
    // the bottom half shows the next.
    if (strstr(exts, "GLX_EXT_swap_control")) {
        glXSwapIntervalEXT_func = (PFNGLXSWAPINTERVALEXTPROC)
            glXGetProcAddress((const GLubyte *)"glXSwapIntervalEXT");
        if (glXSwapIntervalEXT_func) {
            fprintf(stderr, "[crystal] GLX_EXT_swap_control available (VSync)\n");
        }
    }

    return true;
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: ARGB visual discovery
// ────────────────────────────────────────────────────────────────────────
//
// This is the same logic as compositor_create_argb_visual() from the old
// compositor, but stored in Crystal's state. We need a 32-bit visual with
// an alpha channel so frame windows can have semi-transparent shadow regions.

static bool find_argb_visual(AuraWM *wm)
{
    // Ask X for all 32-bit TrueColor visuals on this screen.
    // "TrueColor" means direct RGB values (not palette-indexed).
    // "32-bit" means 8 bits each for R, G, B, and A (alpha).
    XVisualInfo templ;
    templ.screen = wm->screen;
    templ.depth = 32;
    templ.class = TrueColor;

    int num_visuals = 0;
    XVisualInfo *visuals = XGetVisualInfo(wm->dpy,
                                          VisualScreenMask | VisualDepthMask |
                                          VisualClassMask,
                                          &templ, &num_visuals);

    if (!visuals || num_visuals == 0) {
        if (visuals) XFree(visuals);
        return false;
    }

    // Use the first match (usually there's exactly one 32-bit TrueColor visual)
    crystal.argb_visual = visuals[0].visual;

    // Every visual needs a colormap (X11 requirement). AllocNone means
    // "don't reserve any color cells" — it's just a formality for TrueColor.
    crystal.argb_colormap = XCreateColormap(wm->dpy, wm->root,
                                             crystal.argb_visual, AllocNone);

    XFree(visuals);
    return true;
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Initialization
// ────────────────────────────────────────────────────────────────────────

bool crystal_init(AuraWM *wm)
{
    if (!wm || !wm->dpy) return false;

    // Zero out all state so we start clean
    memset(&crystal, 0, sizeof(crystal));

    // Default background color — a dark neutral gray. This is what you see
    // behind all windows if there's no wallpaper.
    crystal.bg_r = 0.15f;
    crystal.bg_g = 0.15f;
    crystal.bg_b = 0.18f;

    fprintf(stderr, "[crystal] Initializing Crystal Compositor...\n");

    // ── Step 1: Check for GLX extension ──
    // GLX is the glue between X11 and OpenGL. Without it, we can't do any
    // GPU-accelerated rendering at all.
    int glx_error_base, glx_event_base;
    if (!glXQueryExtension(wm->dpy, &glx_error_base, &glx_event_base)) {
        fprintf(stderr, "[crystal] ERROR: GLX extension not available. "
                "Cannot initialize OpenGL compositor.\n");
        return false;
    }

    // Check GLX version — we need at least 1.3 for FBConfig support
    int glx_major = 0, glx_minor = 0;
    if (!glXQueryVersion(wm->dpy, &glx_major, &glx_minor)) {
        fprintf(stderr, "[crystal] ERROR: Cannot query GLX version\n");
        return false;
    }
    fprintf(stderr, "[crystal] GLX version: %d.%d\n", glx_major, glx_minor);

    if (glx_major < 1 || (glx_major == 1 && glx_minor < 3)) {
        fprintf(stderr, "[crystal] ERROR: GLX 1.3+ required, got %d.%d\n",
                glx_major, glx_minor);
        return false;
    }

    // ── Step 2: Check for XComposite ──
    // XComposite lets us redirect window rendering to off-screen pixmaps.
    // We need version 0.2+ for XCompositeNameWindowPixmap().
    int composite_major = 0, composite_minor = 0;
    if (!XCompositeQueryVersion(wm->dpy, &composite_major, &composite_minor)) {
        fprintf(stderr, "[crystal] ERROR: XComposite not available\n");
        return false;
    }
    if (composite_major == 0 && composite_minor < 2) {
        fprintf(stderr, "[crystal] ERROR: XComposite 0.2+ required, got %d.%d\n",
                composite_major, composite_minor);
        return false;
    }
    fprintf(stderr, "[crystal] XComposite %d.%d\n", composite_major, composite_minor);

    // ── Step 3: Check for XDamage ──
    // XDamage tells us when a window's contents have changed, so we only
    // refresh textures that actually need updating (not every window every frame).
    int damage_major = 0, damage_minor = 0;
    if (!XDamageQueryVersion(wm->dpy, &damage_major, &damage_minor)) {
        fprintf(stderr, "[crystal] ERROR: XDamage not available\n");
        return false;
    }
    // Store the event base — we need it to identify DamageNotify events
    XDamageQueryExtension(wm->dpy, &crystal.damage_event_base,
                          &crystal.damage_error_base);
    fprintf(stderr, "[crystal] XDamage %d.%d (event base: %d)\n",
            damage_major, damage_minor, crystal.damage_event_base);

    // ── Step 4: Check for XFixes ──
    // XFixes provides input shape manipulation — we use it to make shadow
    // regions click-through (clicks pass to windows behind them).
    int fixes_major = 0, fixes_minor = 0;
    if (!XFixesQueryVersion(wm->dpy, &fixes_major, &fixes_minor)) {
        fprintf(stderr, "[crystal] ERROR: XFixes not available\n");
        return false;
    }
    fprintf(stderr, "[crystal] XFixes %d.%d\n", fixes_major, fixes_minor);

    // ── Step 5: Choose FBConfigs ──
    // FBConfigs describe pixel formats. We need configs for both rendering
    // (the output surface) and pixmap binding (input textures from windows).
    if (!choose_fb_configs(wm->dpy, wm->screen)) {
        fprintf(stderr, "[crystal] ERROR: Cannot find suitable FBConfigs\n");
        return false;
    }
    fprintf(stderr, "[crystal] FBConfigs selected\n");

    // ── Step 6: Create GLX context ──
    // A GLX context holds all OpenGL state. We create it from our rendering
    // FBConfig, then "make it current" so GL calls go to this context.
    // NULL = no shared context (we only have one).
    // True = direct rendering (bypass X server for GL calls — much faster).
    crystal.gl_context = glXCreateNewContext(wm->dpy, crystal.fb_config,
                                             GLX_RGBA_TYPE, NULL, True);
    if (!crystal.gl_context) {
        fprintf(stderr, "[crystal] ERROR: Cannot create GLX context\n");
        return false;
    }
    fprintf(stderr, "[crystal] GLX context created (direct: %s)\n",
            glXIsDirect(wm->dpy, crystal.gl_context) ? "yes" : "no");

    // ── Step 7: Create a GLX window from the root window ──
    // We can't render directly to an X window — we need a GLX wrapper.
    // This creates a GLX drawable backed by the root window, which is where
    // our composited output will appear.
    crystal.gl_window = glXCreateWindow(wm->dpy, crystal.fb_config,
                                         wm->root, NULL);
    if (!crystal.gl_window) {
        fprintf(stderr, "[crystal] ERROR: Cannot create GLX window\n");
        glXDestroyContext(wm->dpy, crystal.gl_context);
        crystal.gl_context = NULL;
        return false;
    }

    // ── Step 8: Make the context current ──
    // "Making current" binds the GLX context to the current thread and the
    // GLX window. All subsequent OpenGL calls will render to this window
    // through this context. Think of it as "activating" the context.
    if (!glXMakeContextCurrent(wm->dpy, crystal.gl_window, crystal.gl_window,
                                crystal.gl_context)) {
        fprintf(stderr, "[crystal] ERROR: Cannot make GLX context current\n");
        glXDestroyWindow(wm->dpy, crystal.gl_window);
        glXDestroyContext(wm->dpy, crystal.gl_context);
        crystal.gl_window = 0;
        crystal.gl_context = NULL;
        return false;
    }

    // ── Step 9: Load GLX extension functions ──
    load_glx_extensions(wm->dpy, wm->screen);

    // ── Step 10: Enable VSync ──
    // Without VSync, the GPU renders as fast as possible and tears appear on
    // screen where one frame ends and the next begins. VSync synchronizes
    // buffer swaps with the monitor's refresh rate.
    if (glXSwapIntervalEXT_func) {
        glXSwapIntervalEXT_func(wm->dpy, crystal.gl_window, 1);
        fprintf(stderr, "[crystal] VSync enabled (swap interval = 1)\n");
    } else {
        fprintf(stderr, "[crystal] WARNING: VSync not available "
                "(may see tearing)\n");
    }

    // ── Step 11: Set up XComposite redirection ──
    // Using Automatic mode during development — picom handles the actual
    // compositing for now while Crystal's rendering pipeline is completed.
    // When Crystal Phase 1 is fully working, this switches to Manual.
    XCompositeRedirectSubwindows(wm->dpy, wm->root, CompositeRedirectAutomatic);
    fprintf(stderr, "[crystal] XComposite redirect set (automatic mode — picom fallback)\n");

    // ── Step 12: Find ARGB visual ──
    // Needed for frame windows that want semi-transparent shadow regions.
    if (find_argb_visual(wm)) {
        fprintf(stderr, "[crystal] Found 32-bit ARGB visual\n");
    } else {
        fprintf(stderr, "[crystal] WARNING: No 32-bit ARGB visual "
                "(shadows may not render correctly)\n");
    }

    // ── Step 13: Store screen dimensions ──
    crystal.root_w = wm->root_w;
    crystal.root_h = wm->root_h;

    // ── Step 14: Initialize OpenGL state ──
    //
    // OpenGL is a state machine — you set modes (like blending, texturing)
    // and they stay active until you change them. Here we set up the initial
    // state that Crystal needs.

    // Set the "clear color" — the background that shows through when no
    // windows cover a region. This fills the screen before we draw anything.
    glClearColor(crystal.bg_r, crystal.bg_g, crystal.bg_b, 1.0f);

    // Enable 2D texturing — required for drawing window contents as textures
    // on quads. When disabled, quads are drawn as solid colors.
    glEnable(GL_TEXTURE_2D);

    // Enable alpha blending — this is how transparency works in OpenGL.
    // When blending is enabled, each pixel's final color is computed by
    // combining the source (new pixel) with the destination (existing pixel)
    // using a blend function.
    glEnable(GL_BLEND);

    // Set the blend function for premultiplied alpha.
    // "Premultiplied alpha" means the RGB values have already been multiplied
    // by the alpha value. For example, a 50% transparent red pixel would be
    // stored as (0.5, 0.0, 0.0, 0.5) instead of (1.0, 0.0, 0.0, 0.5).
    //
    // GL_ONE: use the source color as-is (it's already premultiplied)
    // GL_ONE_MINUS_SRC_ALPHA: scale the destination by (1 - source alpha)
    //
    // Final color = src_color * 1 + dst_color * (1 - src_alpha)
    // This correctly blends transparent windows over whatever is behind them.
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // Set up an orthographic projection matrix.
    //
    // OpenGL normally uses a 3D coordinate system with perspective (objects
    // farther away appear smaller). For a 2D compositor, we want a flat,
    // non-perspective view where pixel coordinates map directly to screen
    // positions.
    //
    // glOrtho(left, right, bottom, top, near, far) defines the visible volume:
    //   left=0, right=root_w:    x-axis matches screen pixels
    //   top=0, bottom=root_h:    y-axis goes DOWN (screen convention)
    //   near=-1, far=1:          z-axis range (we don't use depth)
    //
    // GL_PROJECTION is the "lens" of our virtual camera.
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, crystal.root_w, crystal.root_h, 0, -1, 1);

    // GL_MODELVIEW positions objects in the scene. We start with identity
    // (no transformation) since our coordinates are already in screen space.
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Disable depth testing — we don't need it for 2D compositing. We
    // handle z-order ourselves by drawing back-to-front.
    glDisable(GL_DEPTH_TEST);

    // Mark Crystal as active
    crystal.active = true;

    // Also set the global compositor_active flag so the rest of the WM
    // (frame.c, decor.c) knows compositing is available. This flag controls
    // whether frame windows get ARGB visuals and shadow padding.
    compositor_active = true;

    fprintf(stderr, "[crystal] Crystal Compositor initialized successfully\n");
    fprintf(stderr, "[crystal] OpenGL renderer: %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "[crystal] OpenGL version: %s\n", glGetString(GL_VERSION));
    return true;
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Window texture management
// ────────────────────────────────────────────────────────────────────────
//
// When a window is mapped (becomes visible), we create a texture for it.
// When it's unmapped (hidden/closed), we destroy the texture. When its
// contents change (damaged), we refresh the texture data.

// Find a window's texture entry in our tracking array.
// Returns NULL if the window isn't tracked (e.g., hasn't been mapped yet).
static struct WindowTexture *find_window_texture(Window xwin)
{
    for (int i = 0; i < crystal.window_count; i++) {
        if (crystal.windows[i].xwin == xwin) {
            return &crystal.windows[i];
        }
    }
    return NULL;
}

// Refresh a window's texture using the GLX_EXT_texture_from_pixmap extension.
// This is the "fast path" — the GPU reads the pixmap directly from video
// memory without going through the CPU.
//
// The texture_from_pixmap extension works by creating a GLX pixmap wrapper
// around the X pixmap, then binding that as a texture source. The GPU can
// then sample from it directly when drawing.
static void refresh_window_texture(AuraWM *wm, struct WindowTexture *wt)
{
    if (!crystal.has_texture_from_pixmap) {
        // Fall back to CPU-based texture upload
        refresh_window_texture_fallback(wm, wt);
        return;
    }

    // If we already have a bound texture, release it first.
    // You can't re-bind without releasing — it's like unlocking a file before
    // another program can read it.
    if (wt->bound && wt->glx_pixmap) {
        glXReleaseTexImageEXT_func(wm->dpy, wt->glx_pixmap, GLX_FRONT_EXT);
        wt->bound = false;
    }

    // Destroy the old GLX pixmap wrapper if it exists.
    // We need a new one because the underlying X pixmap may have changed
    // (e.g., the window was resized).
    if (wt->glx_pixmap) {
        glXDestroyPixmap(wm->dpy, wt->glx_pixmap);
        wt->glx_pixmap = 0;
    }

    // Destroy the old X pixmap
    if (wt->pixmap) {
        XFreePixmap(wm->dpy, wt->pixmap);
        wt->pixmap = 0;
    }

    // Get a fresh pixmap from XComposite.
    // XCompositeNameWindowPixmap returns a handle to the off-screen buffer
    // where the X server is rendering this window's contents.
    wt->pixmap = XCompositeNameWindowPixmap(wm->dpy, wt->xwin);
    if (!wt->pixmap) {
        fprintf(stderr, "[crystal] WARNING: Cannot get pixmap for window 0x%lx\n",
                wt->xwin);
        return;
    }

    // Choose the right FBConfig based on whether the window has alpha.
    // ARGB windows need the RGBA config; regular windows use RGB.
    GLXFBConfig fb = wt->has_alpha ? crystal.pixmap_fb_config
                                   : crystal.pixmap_fb_config_rgb;

    // Create a GLX pixmap — this wraps the X pixmap so GLX can use it.
    // The attributes tell GLX how to interpret the pixmap data:
    //   GLX_TEXTURE_TARGET_EXT: treat it as a 2D texture
    //   GLX_TEXTURE_FORMAT_EXT: RGBA or RGB depending on alpha support
    int pixmap_attrs[] = {
        GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
        GLX_TEXTURE_FORMAT_EXT, wt->has_alpha
            ? GLX_TEXTURE_FORMAT_RGBA_EXT
            : GLX_TEXTURE_FORMAT_RGB_EXT,
        None
    };

    wt->glx_pixmap = glXCreatePixmap(wm->dpy, fb, wt->pixmap, pixmap_attrs);
    if (!wt->glx_pixmap) {
        fprintf(stderr, "[crystal] WARNING: Cannot create GLX pixmap for 0x%lx\n",
                wt->xwin);
        return;
    }

    // Bind the GLX pixmap as the source data for our OpenGL texture.
    // After this call, the texture contains the window's current contents.
    // GLX_FRONT_EXT means "read from the front buffer" of the pixmap.
    glBindTexture(GL_TEXTURE_2D, wt->texture);
    glXBindTexImageEXT_func(wm->dpy, wt->glx_pixmap, GLX_FRONT_EXT, NULL);
    wt->bound = true;

    // Set texture filtering — how OpenGL samples the texture when it's
    // scaled up or down.
    // GL_LINEAR = bilinear interpolation (smooth, slightly blurry at edges)
    // GL_NEAREST would be pixel-perfect but looks jagged when scaled.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // GL_CLAMP_TO_EDGE prevents texture coordinates outside [0,1] from
    // wrapping around. Without this, you might see repeating artifacts at
    // window edges.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// CPU fallback for texture refresh.
// Used when GLX_EXT_texture_from_pixmap is not available. This is slower
// because we must:
//   1. Read the pixmap into CPU memory using XGetImage()
//   2. Upload the pixel data to the GPU using glTexImage2D()
//
// On a modern system, this copies data over the PCIe bus twice (GPU→CPU→GPU),
// which is much slower than the zero-copy texture_from_pixmap approach.
static void refresh_window_texture_fallback(AuraWM *wm, struct WindowTexture *wt)
{
    // Destroy and recreate the X pixmap to get fresh contents
    if (wt->pixmap) {
        XFreePixmap(wm->dpy, wt->pixmap);
        wt->pixmap = 0;
    }

    wt->pixmap = XCompositeNameWindowPixmap(wm->dpy, wt->xwin);
    if (!wt->pixmap) return;

    // XGetImage reads pixel data from a drawable (pixmap) into CPU memory.
    // ZPixmap format returns the image as a packed pixel array (as opposed to
    // XYPixmap which separates color planes).
    // AllPlanes means "read all color channels."
    XImage *img = XGetImage(wm->dpy, wt->pixmap, 0, 0,
                            wt->w, wt->h, AllPlanes, ZPixmap);
    if (!img) {
        fprintf(stderr, "[crystal] WARNING: XGetImage failed for 0x%lx\n",
                wt->xwin);
        return;
    }

    // Upload the pixel data to our OpenGL texture.
    // GL_BGRA: X11 stores pixels as Blue-Green-Red-Alpha (on little-endian),
    //          which matches GL_BGRA byte order.
    // GL_UNSIGNED_BYTE: each color channel is one byte (0-255).
    // glTexImage2D replaces the entire texture contents.
    glBindTexture(GL_TEXTURE_2D, wt->texture);
    glTexImage2D(GL_TEXTURE_2D,     // Target: 2D texture
                 0,                  // Mipmap level 0 (base, full resolution)
                 wt->has_alpha ? GL_RGBA : GL_RGB,  // Internal format on GPU
                 wt->w, wt->h,      // Dimensions
                 0,                  // Border (must be 0)
                 GL_BGRA,            // Format of the input data
                 GL_UNSIGNED_BYTE,   // Data type per channel
                 img->data);         // Pointer to the pixel data

    // Set texture parameters (same as the fast path)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Free the XImage — we've uploaded the data to the GPU, no need to keep
    // a CPU copy.
    XDestroyImage(img);
}

// Release all GPU and X resources associated with a window's texture.
// Called when a window is unmapped or when shutting down.
static void release_window_texture(AuraWM *wm, struct WindowTexture *wt)
{
    // Release the texture binding (must happen before destroying the pixmap)
    if (wt->bound && wt->glx_pixmap && glXReleaseTexImageEXT_func) {
        glXReleaseTexImageEXT_func(wm->dpy, wt->glx_pixmap, GLX_FRONT_EXT);
        wt->bound = false;
    }

    // Destroy the OpenGL texture on the GPU
    if (wt->texture) {
        glDeleteTextures(1, &wt->texture);
        wt->texture = 0;
    }

    // Destroy the GLX pixmap wrapper
    if (wt->glx_pixmap) {
        glXDestroyPixmap(wm->dpy, wt->glx_pixmap);
        wt->glx_pixmap = 0;
    }

    // Free the X pixmap (the off-screen buffer)
    if (wt->pixmap) {
        XFreePixmap(wm->dpy, wt->pixmap);
        wt->pixmap = 0;
    }

    // Destroy the XDamage tracker for this window
    if (wt->damage) {
        XDamageDestroy(wm->dpy, wt->damage);
        wt->damage = 0;
    }
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Window lifecycle (map/unmap/damage)
// ────────────────────────────────────────────────────────────────────────

void crystal_window_mapped(AuraWM *wm, Client *c)
{
    if (!crystal.active || !wm || !c || !c->frame) return;

    // Don't add duplicates — check if we're already tracking this window
    if (find_window_texture(c->frame)) return;

    // Make sure we haven't hit the window limit
    if (crystal.window_count >= MAX_CLIENTS) {
        fprintf(stderr, "[crystal] WARNING: Maximum window count reached, "
                "cannot track window 0x%lx\n", c->frame);
        return;
    }

    // Get the window's visual to determine if it has an alpha channel.
    // Windows created with our ARGB visual will have depth 32.
    XWindowAttributes wa;
    if (!XGetWindowAttributes(wm->dpy, c->frame, &wa)) {
        fprintf(stderr, "[crystal] WARNING: Cannot get attributes for 0x%lx\n",
                c->frame);
        return;
    }

    // Create a new tracking entry for this window
    struct WindowTexture *wt = &crystal.windows[crystal.window_count];
    memset(wt, 0, sizeof(*wt));

    wt->xwin = c->frame;
    wt->has_alpha = (wa.depth == 32);

    // Calculate the window's on-screen geometry.
    // The frame position (c->x, c->y) is in root window coordinates. If the
    // compositor is active, the frame includes shadow padding, so the actual
    // screen position needs to account for that offset.
    if (compositor_active) {
        wt->x = c->x - SHADOW_LEFT;
        wt->y = c->y - SHADOW_TOP;
        wt->w = wa.width;
        wt->h = wa.height;
    } else {
        wt->x = c->x;
        wt->y = c->y;
        wt->w = wa.width;
        wt->h = wa.height;
    }

    // Generate an OpenGL texture ID for this window.
    // glGenTextures creates "names" (integer IDs) for textures on the GPU.
    // The texture doesn't have any data yet — it's just a handle.
    glGenTextures(1, &wt->texture);

    // Mark the texture as dirty so it gets filled with actual window content
    // on the next composite pass.
    wt->dirty = true;

    // Register an XDamage monitor on the frame window.
    // XDamageReportNonEmpty means "notify me whenever the damage region becomes
    // non-empty" — i.e., as soon as any pixel changes, we get notified.
    // We then acknowledge the damage and refresh the texture.
    wt->damage = XDamageCreate(wm->dpy, c->frame, XDamageReportNonEmpty);

    crystal.window_count++;

    if (getenv("AURA_DEBUG")) {
        fprintf(stderr, "[crystal] Mapped window 0x%lx (%dx%d at %d,%d, "
                "alpha=%d)\n", c->frame, wt->w, wt->h, wt->x, wt->y,
                wt->has_alpha);
    }
}

void crystal_window_unmapped(AuraWM *wm, Client *c)
{
    if (!crystal.active || !wm || !c) return;

    // Find the window in our tracking array
    for (int i = 0; i < crystal.window_count; i++) {
        if (crystal.windows[i].xwin == c->frame) {
            // Release all GPU and X resources for this window
            release_window_texture(wm, &crystal.windows[i]);

            // Remove from the array by shifting everything after it left.
            // This maintains the z-order (array order = stacking order).
            int remaining = crystal.window_count - i - 1;
            if (remaining > 0) {
                memmove(&crystal.windows[i], &crystal.windows[i + 1],
                        remaining * sizeof(struct WindowTexture));
            }
            crystal.window_count--;

            if (getenv("AURA_DEBUG")) {
                fprintf(stderr, "[crystal] Unmapped window 0x%lx\n", c->frame);
            }
            return;
        }
    }
}

void crystal_window_damaged(AuraWM *wm, Client *c)
{
    if (!crystal.active || !wm || !c) return;

    // Try the frame window first (that's what we track), then the client window
    struct WindowTexture *wt = find_window_texture(c->frame);
    if (!wt) return;

    // Mark the texture as dirty so it gets refreshed on the next frame
    wt->dirty = true;

    // Acknowledge the damage — tell the X server "I've seen this change."
    // Without this, the server keeps re-sending the same damage notification.
    // Passing None for both regions means "acknowledge all damage."
    if (wt->damage) {
        XDamageSubtract(wm->dpy, wt->damage, None, None);
    }
}

// Called when a window is resized — update our tracking info and refresh
// the texture since the old pixmap is now invalid.
void crystal_window_resized(AuraWM *wm, Client *c)
{
    if (!crystal.active || !wm || !c) return;

    struct WindowTexture *wt = find_window_texture(c->frame);
    if (!wt) return;

    // Get the updated geometry
    XWindowAttributes wa;
    if (!XGetWindowAttributes(wm->dpy, c->frame, &wa)) return;

    // Update position (accounting for shadow padding)
    if (compositor_active) {
        wt->x = c->x - SHADOW_LEFT;
        wt->y = c->y - SHADOW_TOP;
    } else {
        wt->x = c->x;
        wt->y = c->y;
    }

    // If the size changed, we need to refresh the texture because the old
    // pixmap was for the previous size.
    bool size_changed = (wt->w != wa.width || wt->h != wa.height);
    wt->w = wa.width;
    wt->h = wa.height;

    if (size_changed) {
        wt->dirty = true;
    }
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Shadow rendering (Phase 1 — concentric rectangles)
// ────────────────────────────────────────────────────────────────────────
//
// Phase 1 uses the same shadow technique as the old Cairo-based compositor:
// concentric rectangles with decreasing alpha, creating a soft gradient
// that approximates a Gaussian blur.
//
// In Phase 2, this will be replaced with a real GPU-accelerated Gaussian
// blur for much better visual quality and performance.

static void draw_window_shadow(struct WindowTexture *wt, bool focused)
{
    // Shadow parameters — these match the old compositor's values.
    // SHADOW_RADIUS (22) layers create the full falloff gradient.
    int layers = SHADOW_RADIUS;
    double peak_alpha = focused ? SHADOW_ALPHA_ACTIVE : SHADOW_ALPHA_INACTIVE;
    int y_offset = SHADOW_Y_OFFSET;

    // The shadow is drawn behind the window (the chrome area within the frame).
    // When the frame has shadow padding, the chrome starts at (SHADOW_LEFT, SHADOW_TOP)
    // within the frame. But our wt->x/y already account for this offset, so
    // the chrome position is at (wt->x + SHADOW_LEFT, wt->y + SHADOW_TOP).
    int chrome_x = wt->x + SHADOW_LEFT;
    int chrome_y = wt->y + SHADOW_TOP;
    int chrome_w = wt->w - SHADOW_LEFT - SHADOW_RIGHT;
    int chrome_h = wt->h - SHADOW_TOP - SHADOW_BOTTOM;

    // Don't draw shadows for windows that are too small
    if (chrome_w <= 0 || chrome_h <= 0) return;

    // Disable texturing — shadows are solid colored rectangles, not textured
    glDisable(GL_TEXTURE_2D);

    // Draw layers from outermost (most transparent) to innermost (most opaque).
    // Each layer is a filled rectangle that's slightly larger than the chrome.
    for (int i = layers; i >= 1; i--) {
        // t goes from 1.0 (outermost) to ~0.0 (innermost)
        double t = (double)i / layers;

        // Cubic falloff: (1-t)^3 creates a soft edge that's dark near the
        // window and fades out smoothly. This closely matches the diffuse
        // shadow appearance of real Snow Leopard windows.
        double alpha = peak_alpha * (1.0 - t) * (1.0 - t) * (1.0 - t);

        // Each layer expands outward by 2 pixels more than the previous
        int expand = i * 2;

        // Shadow rectangle position and size
        float sx = (float)(chrome_x - expand);
        float sy = (float)(chrome_y - expand + y_offset);
        float sw = (float)(chrome_w + expand * 2);
        float sh = (float)(chrome_h + expand * 2);

        // Set the shadow color: pure black with varying transparency.
        // glColor4f sets the current drawing color as (R, G, B, A) in [0, 1].
        glColor4f(0.0f, 0.0f, 0.0f, (float)alpha);

        // Draw a filled rectangle using GL_QUADS.
        // glBegin(GL_QUADS) starts a group of quadrilaterals (4-sided shapes).
        // Each set of 4 glVertex2f calls defines one quad. The vertices must
        // be specified in order (clockwise or counter-clockwise).
        glBegin(GL_QUADS);
        glVertex2f(sx, sy);             // Top-left
        glVertex2f(sx + sw, sy);        // Top-right
        glVertex2f(sx + sw, sy + sh);   // Bottom-right
        glVertex2f(sx, sy + sh);        // Bottom-left
        glEnd();
    }

    // Reset color to fully opaque white so subsequent textured drawing
    // isn't tinted by the shadow color. When texturing is enabled, the
    // texture color is multiplied by the current glColor — white (1,1,1,1)
    // means the texture appears unmodified.
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Main compositing pass
// ────────────────────────────────────────────────────────────────────────
//
// This is the heart of Crystal — called every frame from the event loop.
// It draws all visible windows to the screen using OpenGL.
//
// The rendering process is:
//   1. Clear the screen to the background color
//   2. Set up a 2D projection matching pixel coordinates
//   3. For each window (back-to-front):
//      a. Draw its shadow behind it
//      b. Bind its texture (the window's contents)
//      c. Draw a textured quad at the window's screen position
//   4. Swap buffers to show the frame

void crystal_composite(AuraWM *wm)
{
    if (!crystal.active || !wm) return;

    // Make our GL context current. This is technically redundant if we're
    // the only GL user, but it's good practice — other code might have
    // changed the current context.
    glXMakeContextCurrent(wm->dpy, crystal.gl_window, crystal.gl_window,
                          crystal.gl_context);

    // ── Step 1: Clear the screen ──
    // GL_COLOR_BUFFER_BIT tells OpenGL to fill the entire framebuffer with
    // the clear color (set in crystal_init). This erases the previous frame.
    glClear(GL_COLOR_BUFFER_BIT);

    // ── Step 2: Set up 2D projection ──
    // Reset the projection matrix each frame (in case the screen was resized).
    // glOrtho maps (0,0) to the top-left corner and (root_w, root_h) to the
    // bottom-right, matching X11's coordinate convention.
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, crystal.root_w, crystal.root_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // ── Step 3: Enable blending for transparent windows ──
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // ── Step 4: Draw all windows back-to-front ──
    //
    // We use a 3-pass approach to respect the natural z-ordering of different
    // window types:
    //   Pass 0: Desktop-type windows (_NET_WM_WINDOW_TYPE_DESKTOP)
    //   Pass 1: Normal application windows
    //   Pass 2: Dock/panel windows (_NET_WM_WINDOW_TYPE_DOCK)
    //
    // This ensures the desktop is always at the bottom, panels are always on
    // top, and normal windows are sandwiched between them.

    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < crystal.window_count; i++) {
            struct WindowTexture *wt = &crystal.windows[i];

            // Determine which pass this window belongs to.
            // For now, all windows are drawn in pass 1 (normal).
            // TODO: Check _NET_WM_WINDOW_TYPE to sort into passes 0/2.
            int window_pass = 1;

            // Skip windows that don't belong to this pass
            if (window_pass != pass) continue;

            // Skip zero-size windows (shouldn't happen, but be defensive)
            if (wt->w <= 0 || wt->h <= 0) continue;

            // ── Refresh dirty textures ──
            // If the window's contents have changed since the last frame,
            // update the OpenGL texture with the new pixel data.
            if (wt->dirty) {
                refresh_window_texture(wm, wt);
                wt->dirty = false;
            }

            // ── Draw the shadow ──
            // Shadows are drawn BEHIND the window. We need to find the
            // corresponding Client to know if the window is focused (focused
            // windows get stronger shadows).
            Client *c = wm_find_client_by_frame(wm, wt->xwin);
            bool focused = c ? c->focused : false;
            draw_window_shadow(wt, focused);

            // ── Draw the window contents ──
            // Bind the window's texture and draw a quad that covers the
            // window's screen rectangle.
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, wt->texture);

            // Reset color to white so the texture colors aren't modified.
            // When GL_TEXTURE_2D is enabled, the final pixel color is:
            //   texture_color * glColor
            // With glColor = (1,1,1,1), the texture appears as-is.
            glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

            // Draw a textured quad.
            // glTexCoord2f sets the texture coordinate for the next vertex.
            // Texture coordinates go from (0,0) at the top-left of the image
            // to (1,1) at the bottom-right. By mapping these to the window's
            // screen rectangle, we stretch the texture to fill the quad.
            //
            // Vertices are specified in clockwise order:
            //   (0,0)──────(1,0)
            //     │  texture  │
            //   (0,1)──────(1,1)
            glBegin(GL_QUADS);
                glTexCoord2f(0.0f, 0.0f);
                glVertex2f((float)wt->x, (float)wt->y);                 // Top-left

                glTexCoord2f(1.0f, 0.0f);
                glVertex2f((float)(wt->x + wt->w), (float)wt->y);      // Top-right

                glTexCoord2f(1.0f, 1.0f);
                glVertex2f((float)(wt->x + wt->w), (float)(wt->y + wt->h));  // Bottom-right

                glTexCoord2f(0.0f, 1.0f);
                glVertex2f((float)wt->x, (float)(wt->y + wt->h));       // Bottom-left
            glEnd();

            glDisable(GL_TEXTURE_2D);
        }
    }

    // ── Step 5: Swap buffers ──
    // We've been drawing to the "back buffer" (an invisible off-screen surface).
    // glXSwapBuffers swaps the back buffer with the front buffer (what's on
    // screen), making our new frame visible instantaneously. This is called
    // "double buffering" and prevents flicker — the user never sees a
    // half-drawn frame.
    //
    // If VSync is enabled, this call blocks until the next vertical blank
    // period, limiting us to the monitor's refresh rate (typically 60 FPS).
    glXSwapBuffers(wm->dpy, crystal.gl_window);
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Event handling
// ────────────────────────────────────────────────────────────────────────

bool crystal_handle_event(AuraWM *wm, XEvent *e)
{
    if (!crystal.active || !wm || !e) return false;

    // Check if this is an XDamage event.
    // XDamage events have type == (damage_event_base + XDamageNotify).
    // XDamageNotify is 0, so the type is just damage_event_base.
    if (e->type == crystal.damage_event_base + XDamageNotify) {
        // Cast the generic XEvent to the damage-specific struct
        XDamageNotifyEvent *dev = (XDamageNotifyEvent *)e;

        // Find which window was damaged
        struct WindowTexture *wt = find_window_texture(dev->drawable);
        if (wt) {
            wt->dirty = true;

            // Acknowledge the damage so X stops re-sending this notification
            XDamageSubtract(wm->dpy, dev->damage, None, None);
        } else {
            // The damaged window might be a client window (not a frame).
            // Try to find the client and damage its frame instead.
            Client *c = wm_find_client(wm, dev->drawable);
            if (c && c->frame) {
                wt = find_window_texture(c->frame);
                if (wt) {
                    wt->dirty = true;
                }
            }
            // Always acknowledge damage, even for unknown windows
            XDamageSubtract(wm->dpy, dev->damage, None, None);
        }

        return true;  // Event was handled
    }

    return false;  // Not a Crystal event
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Input shape passthrough
// ────────────────────────────────────────────────────────────────────────
//
// Shadow regions should not intercept mouse clicks. We use XFixes input
// shapes to define which parts of the frame window are "clickable."
// Only the chrome area (title bar + borders) responds to clicks; the
// shadow padding lets clicks fall through to windows behind.
//
// This is the same logic as the old compositor_set_input_shape(), kept
// here for consistency since Crystal owns compositing now.

void crystal_set_input_shape(AuraWM *wm, Client *c)
{
    if (!wm || !c || !c->frame) return;

    // Calculate the chrome (clickable) area within the frame.
    // The chrome starts at (SHADOW_LEFT, SHADOW_TOP) and covers the
    // title bar, borders, and client content area.
    int chrome_w = c->w + 2 * BORDER_WIDTH;
    int chrome_h = c->h + TITLEBAR_HEIGHT + BORDER_WIDTH;

    // Create an XFixes region that covers just the chrome area
    XRectangle rect;
    rect.x = SHADOW_LEFT;
    rect.y = SHADOW_TOP;
    rect.width = chrome_w;
    rect.height = chrome_h;

    XserverRegion region = XFixesCreateRegion(wm->dpy, &rect, 1);

    // ShapeInput controls which parts of the window receive mouse events.
    // ShapeSet replaces the entire input shape with our rectangle.
    XFixesSetWindowShapeRegion(wm->dpy, c->frame, ShapeInput, 0, 0, region);

    // Clean up — the X server made its own copy of the region
    XFixesDestroyRegion(wm->dpy, region);

    if (getenv("AURA_DEBUG")) {
        fprintf(stderr, "[crystal] Set input shape for '%s': "
                "clickable at (%d,%d) %dx%d\n",
                c->title, rect.x, rect.y, rect.width, rect.height);
    }
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: ARGB visual access
// ────────────────────────────────────────────────────────────────────────
//
// These functions expose Crystal's ARGB visual and colormap to the rest of
// the window manager (especially frame.c, which needs them to create frame
// windows with alpha support).
//
// They maintain compatibility with the old compositor's
// compositor_create_argb_visual() interface.

bool crystal_create_argb_visual(AuraWM *wm, Visual **out_visual,
                                Colormap *out_colormap)
{
    if (!out_visual || !out_colormap) return false;

    // If Crystal found an ARGB visual during init, return it
    if (crystal.argb_visual && crystal.argb_colormap) {
        *out_visual = crystal.argb_visual;
        *out_colormap = crystal.argb_colormap;
        return true;
    }

    // Fall back to searching for a 32-bit ARGB visual manually.
    // This is the same logic that was in the old compositor.c — look for
    // a TrueColor visual with 32-bit depth on the default screen.
    if (!wm || !wm->dpy) return false;

    XVisualInfo templ;
    templ.screen = wm->screen;
    templ.depth = 32;
    templ.class = TrueColor;

    int num_visuals = 0;
    XVisualInfo *visuals = XGetVisualInfo(wm->dpy,
                                          VisualScreenMask | VisualDepthMask |
                                          VisualClassMask,
                                          &templ, &num_visuals);

    if (!visuals || num_visuals == 0) {
        if (visuals) XFree(visuals);
        return false;
    }

    // Use the first matching 32-bit TrueColor visual
    *out_visual = visuals[0].visual;

    // X11 requires a colormap for every visual. AllocNone means we don't
    // need to allocate color cells (TrueColor uses direct RGB encoding).
    *out_colormap = XCreateColormap(wm->dpy, wm->root,
                                    *out_visual, AllocNone);

    XFree(visuals);
    return true;
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Status queries
// ────────────────────────────────────────────────────────────────────────

bool crystal_is_active(void)
{
    return crystal.active;
}

int crystal_get_damage_event_base(void)
{
    return crystal.damage_event_base;
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Screen resize handling
// ────────────────────────────────────────────────────────────────────────

static void crystal_screen_resized(AuraWM *wm)
{
    if (!crystal.active || !wm) return;

    // Update our stored screen dimensions
    crystal.root_w = wm->root_w;
    crystal.root_h = wm->root_h;

    // Update the GL viewport to match the new screen size.
    // The viewport maps OpenGL's normalized coordinates to pixel coordinates.
    // (0, 0) is the bottom-left corner; (root_w, root_h) is the top-right.
    glViewport(0, 0, crystal.root_w, crystal.root_h);

    fprintf(stderr, "[crystal] Screen resized to %dx%d\n",
            crystal.root_w, crystal.root_h);
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Shutdown
// ────────────────────────────────────────────────────────────────────────

void crystal_shutdown(AuraWM *wm)
{
    if (!wm || !wm->dpy) return;

    fprintf(stderr, "[crystal] Shutting down Crystal Compositor...\n");

    // Release all tracked window textures
    for (int i = 0; i < crystal.window_count; i++) {
        release_window_texture(wm, &crystal.windows[i]);
    }
    crystal.window_count = 0;

    // Undo XComposite redirection — let X go back to drawing windows directly.
    // This is critical for a clean shutdown. If we don't do this and the WM
    // crashes, the screen goes blank because windows are still redirected to
    // off-screen pixmaps that nobody is compositing.
    if (crystal.active) {
        XCompositeUnredirectSubwindows(wm->dpy, wm->root,
                                       CompositeRedirectManual);
        fprintf(stderr, "[crystal] Unredirected subwindows\n");
    }

    // Destroy the GLX context and window.
    // Order matters: release the context first, then destroy the window.
    if (crystal.gl_context) {
        // Make sure nothing is current before destroying
        glXMakeContextCurrent(wm->dpy, None, None, NULL);

        if (crystal.gl_window) {
            glXDestroyWindow(wm->dpy, crystal.gl_window);
            crystal.gl_window = 0;
        }

        glXDestroyContext(wm->dpy, crystal.gl_context);
        crystal.gl_context = NULL;
    }

    // Free the ARGB colormap (the visual is owned by X, not us)
    if (crystal.argb_colormap) {
        XFreeColormap(wm->dpy, crystal.argb_colormap);
        crystal.argb_colormap = 0;
    }
    crystal.argb_visual = NULL;

    // Clear flags
    crystal.active = false;
    compositor_active = false;

    fprintf(stderr, "[crystal] Crystal Compositor shut down\n");
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Animation stubs (Phase 4+)
// ────────────────────────────────────────────────────────────────────────
//
// These are placeholder functions for future animation support. The genie
// minimize effect will warp the window's texture using a mesh distortion
// in the GL pipeline. For now, these are no-ops so the rest of the codebase
// can call them without conditional compilation.

void crystal_animate_minimize(AuraWM *wm, Client *c,
                              int dock_icon_x, int dock_icon_y)
{
    // Phase 4: Genie minimize animation.
    // Will distort the window texture over multiple frames to create the
    // classic macOS "sucking into the dock" effect.
    (void)wm;
    (void)c;
    (void)dock_icon_x;
    (void)dock_icon_y;
}

void crystal_animate_restore(AuraWM *wm, Client *c,
                             int dock_icon_x, int dock_icon_y)
{
    // Phase 4: Genie restore animation (reverse of minimize).
    // The window texture expands from the dock icon back to full size.
    (void)wm;
    (void)c;
    (void)dock_icon_x;
    (void)dock_icon_y;
}

bool crystal_animation_active(AuraWM *wm)
{
    // Phase 4: Returns true if any animation is in progress.
    // When active, the main loop should keep calling crystal_composite()
    // at the refresh rate to advance the animation smoothly.
    (void)wm;
    return false;
}
