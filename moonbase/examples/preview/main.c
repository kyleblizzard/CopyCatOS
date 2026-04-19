// CopyCatOS — by Kyle Blizzard at Blizzard.show

// Preview.appc — reference app 3 of 4 before the libmoonbase.so.1
// public-SDK freeze. Exists to pressure-test three distinct ABI
// surfaces in a single bundle:
//   • MOONBASE_RENDER_GL        — open an image, upload to a texture,
//                                 draw a textured quad preserving
//                                 aspect ratio
//   • MB_EV_DRAG_*              — drop a file from fileviewer / Nautilus
//                                 onto the window, parse the URI, load it
//   • moonbase_prefs_*          — remember window size and last-opened
//                                 path in Preferences/preview.toml so the
//                                 next launch with no argv picks up where
//                                 the user left off
//
// Decode uses libpng for PNG and libjpeg-turbo for JPEG. Both ship with
// every Linux desktop and both moonrock already links, so no new
// dependency lands on the build. Every other format (TIFF, GIF, WEBP,
// HEIC, PDF) is deliberately out of v0.1 scope — Preview grows into
// those as their need shows up.
//
// Window model is single-document for v0.1 — one image per window,
// one window per app instance. Multi-document window memory ("the
// window that showed photo.png should reopen at the same size next
// time") lives in per-path prefs keys, but the launcher today opens a
// fresh process per invocation so cross-instance memory is the unit
// test. Splitting into per-path windows waits on a proper app
// launcher with window-reuse semantics.

#include <moonbase.h>

#include <GLES3/gl3.h>
#include <png.h>
#include <jpeglib.h>

#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ─────────────────────────────────────────────────────────────────────
// Loaded image state
// ─────────────────────────────────────────────────────────────────────

typedef struct {
    uint8_t *rgba;          // tightly packed, top-down, 4 bytes/px
    int      width;
    int      height;
    bool     valid;
    char     source_path[1024];
} preview_image_t;

static preview_image_t g_img;

static void image_free(preview_image_t *img) {
    free(img->rgba);
    img->rgba = NULL;
    img->width = img->height = 0;
    img->valid = false;
    img->source_path[0] = '\0';
}

// ─────────────────────────────────────────────────────────────────────
// PNG decode (libpng)
// ─────────────────────────────────────────────────────────────────────
//
// libpng's callback-based API is verbose. We keep it linear: create
// read + info structs, hand libpng the FILE, ask for 8-bit RGBA with
// alpha filled in, read the whole image into one contiguous buffer.

static bool decode_png(FILE *fp, preview_image_t *out) {
    unsigned char sig[8];
    if (fread(sig, 1, 8, fp) != 8 || png_sig_cmp(sig, 0, 8) != 0) {
        return false;
    }
    rewind(fp);

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                              NULL, NULL, NULL);
    if (!png) return false;
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); return false; }

    // libpng's error handling jumps out on failure. The row-pointer
    // array is declared before setjmp so the longjmp cleanup can free
    // it safely.
    png_bytep *rows = NULL;
    uint8_t   *pixels = NULL;
    if (setjmp(png_jmpbuf(png))) {
        free(rows);
        free(pixels);
        png_destroy_read_struct(&png, &info, NULL);
        return false;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    png_uint_32 w = 0, h = 0;
    int depth = 0, color = 0;
    png_get_IHDR(png, info, &w, &h, &depth, &color, NULL, NULL, NULL);

    // Normalize to 8-bit RGBA, top-down.
    if (color == PNG_COLOR_TYPE_PALETTE)          png_set_palette_to_rgb(png);
    if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (depth == 16)                              png_set_strip_16(png);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    // Ensure alpha channel is present.
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(png, info);

    size_t stride = 4 * (size_t)w;
    pixels = malloc(stride * (size_t)h);
    rows   = malloc(sizeof(png_bytep) * (size_t)h);
    if (!pixels || !rows) {
        free(pixels); free(rows);
        png_destroy_read_struct(&png, &info, NULL);
        return false;
    }
    for (png_uint_32 y = 0; y < h; y++) {
        rows[y] = pixels + y * stride;
    }
    png_read_image(png, rows);
    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);

    free(rows);
    out->rgba   = pixels;
    out->width  = (int)w;
    out->height = (int)h;
    return true;
}

// ─────────────────────────────────────────────────────────────────────
// JPEG decode (libjpeg-turbo)
// ─────────────────────────────────────────────────────────────────────
//
// libjpeg is also longjmp-happy; we hook its error manager to a
// jump_buffer so decode failures roll back cleanly.

struct jpeg_error_ctx {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void jpeg_error_exit(j_common_ptr cinfo) {
    struct jpeg_error_ctx *err = (struct jpeg_error_ctx *)cinfo->err;
    longjmp(err->setjmp_buffer, 1);
}

static bool decode_jpeg(FILE *fp, preview_image_t *out) {
    // SOI check up front so we don't spin up the decoder for a non-JPEG.
    unsigned char sig[2];
    if (fread(sig, 1, 2, fp) != 2 || sig[0] != 0xFF || sig[1] != 0xD8) {
        return false;
    }
    rewind(fp);

    struct jpeg_decompress_struct cinfo = {0};
    struct jpeg_error_ctx err;
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = jpeg_error_exit;

    uint8_t *pixels = NULL;
    uint8_t *row_buf = NULL;
    if (setjmp(err.setjmp_buffer)) {
        free(pixels);
        free(row_buf);
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    // Force RGB output; we add the alpha byte ourselves. libjpeg-turbo
    // supports JCS_EXT_RGBA but we keep to the baseline API so this
    // works against vanilla libjpeg too.
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int w = (int)cinfo.output_width;
    int h = (int)cinfo.output_height;
    pixels  = malloc((size_t)w * (size_t)h * 4);
    row_buf = malloc((size_t)w * 3);
    if (!pixels || !row_buf) {
        free(pixels); free(row_buf);
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    while ((int)cinfo.output_scanline < h) {
        JSAMPROW rp = row_buf;
        jpeg_read_scanlines(&cinfo, &rp, 1);
        int y = (int)cinfo.output_scanline - 1;
        uint8_t *dst = pixels + (size_t)y * 4 * (size_t)w;
        for (int x = 0; x < w; x++) {
            dst[4*x+0] = row_buf[3*x+0];
            dst[4*x+1] = row_buf[3*x+1];
            dst[4*x+2] = row_buf[3*x+2];
            dst[4*x+3] = 0xFF;
        }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    free(row_buf);

    out->rgba   = pixels;
    out->width  = w;
    out->height = h;
    return true;
}

// Top-level loader: tries PNG first, then JPEG. Dispatching by file
// extension would lie — users rename images all the time. Signature
// sniffing is authoritative.
static bool load_image(const char *path, preview_image_t *out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "preview: cannot open %s: %s\n", path, strerror(errno));
        return false;
    }

    preview_image_t img = {0};
    bool ok = decode_png(fp, &img);
    if (!ok) {
        rewind(fp);
        ok = decode_jpeg(fp, &img);
    }
    fclose(fp);

    if (!ok) {
        fprintf(stderr, "preview: %s is not a supported format (PNG/JPEG only)\n", path);
        return false;
    }
    img.valid = true;
    snprintf(img.source_path, sizeof(img.source_path), "%s", path);

    image_free(out);
    *out = img;
    return true;
}

// ─────────────────────────────────────────────────────────────────────
// file:// URI → absolute path
// ─────────────────────────────────────────────────────────────────────
//
// XDND sources always hand us file:// URIs. We tolerate the three
// valid syntaxes (file:/path, file:///path, file://host/path) and
// percent-decode %XX escapes on the path portion. Returns heap
// allocation (caller frees) or NULL if the URI scheme isn't file.

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *uri_to_path(const char *uri) {
    if (strncmp(uri, "file:", 5) != 0) return NULL;
    const char *p = uri + 5;
    if (p[0] == '/' && p[1] == '/') {
        p += 2;
        // skip optional host component — file://host/path
        const char *slash = strchr(p, '/');
        if (!slash) return NULL;
        p = slash;
    }
    size_t len = strlen(p);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t oi = 0;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '%' && i + 2 < len) {
            int hi = hex_nibble(p[i+1]);
            int lo = hex_nibble(p[i+2]);
            if (hi >= 0 && lo >= 0) {
                out[oi++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out[oi++] = p[i];
    }
    out[oi] = '\0';
    return out;
}

// ─────────────────────────────────────────────────────────────────────
// GL resources
// ─────────────────────────────────────────────────────────────────────

static GLuint g_prog = 0;
static GLuint g_vbo  = 0;
static GLuint g_vao  = 0;
static GLuint g_tex  = 0;
static GLint  g_uTex_loc = -1;

static GLuint compile_shader(GLenum kind, const char *src) {
    GLuint sh = glCreateShader(kind);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "preview: shader compile failed:\n%s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint link_program(const char *vs_src, const char *fs_src) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aUV");
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "preview: program link failed:\n%s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// Fullscreen quad (two triangles). Position in clip space, UV in
// (0..1, 0..1) with V flipped so the top row of the RGBA buffer lands
// at the top of the window.
static const float kQuadVerts[] = {
//   x      y     u     v
    -1.f, -1.f,  0.f, 1.f,
     1.f, -1.f,  1.f, 1.f,
    -1.f,  1.f,  0.f, 0.f,
    -1.f,  1.f,  0.f, 0.f,
     1.f, -1.f,  1.f, 1.f,
     1.f,  1.f,  1.f, 0.f,
};

static void ensure_gl_resources(void) {
    if (g_prog) return;
    static const char *vs =
        "#version 300 es\n"
        "in vec2 aPos;\n"
        "in vec2 aUV;\n"
        "out vec2 vUV;\n"
        "uniform vec2 uScale;\n"
        "void main() {\n"
        "    vUV = aUV;\n"
        "    gl_Position = vec4(aPos * uScale, 0.0, 1.0);\n"
        "}\n";
    static const char *fs =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in  vec2 vUV;\n"
        "out vec4 fragColor;\n"
        "uniform sampler2D uTex;\n"
        "uniform float     uHasImage;\n"
        "void main() {\n"
        "    if (uHasImage < 0.5) {\n"
        // Empty state: neutral gray matching Aqua content backgrounds.
        "        fragColor = vec4(0.8, 0.8, 0.8, 1.0);\n"
        "    } else {\n"
        "        fragColor = texture(uTex, vUV);\n"
        "    }\n"
        "}\n";
    g_prog = link_program(vs, fs);
    if (!g_prog) return;
    g_uTex_loc = glGetUniformLocation(g_prog, "uTex");

    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVerts), kQuadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float),
                          (void*)(2*sizeof(float)));

    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// Re-uploading only happens on load — every redraw just binds the
// texture already on the GPU and draws the quad.
static bool g_needs_upload = false;

static void upload_texture(void) {
    if (!g_img.valid || !g_img.rgba) return;
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 g_img.width, g_img.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, g_img.rgba);
    g_needs_upload = false;
}

// ─────────────────────────────────────────────────────────────────────
// Redraw
// ─────────────────────────────────────────────────────────────────────

static void on_redraw(mb_window_t *w) {
    if (moonbase_window_gl_make_current(w) != MB_EOK) {
        fprintf(stderr, "preview: gl_make_current failed: %s\n",
                moonbase_error_string(moonbase_last_error()));
        return;
    }
    ensure_gl_resources();
    if (g_needs_upload) upload_texture();

    int px_w = 0, px_h = 0;
    moonbase_window_backing_pixel_size(w, &px_w, &px_h);
    glViewport(0, 0, px_w, px_h);

    glClearColor(0.15f, 0.15f, 0.15f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (g_prog) {
        glUseProgram(g_prog);
        glBindVertexArray(g_vao);

        // Preserve aspect ratio: letterbox in whichever axis the
        // window is bigger than the image. uScale shrinks the quad in
        // that axis so the leftover space shows the clear colour.
        float sx = 1.f, sy = 1.f;
        if (g_img.valid && g_img.width > 0 && g_img.height > 0 &&
            px_w > 0 && px_h > 0) {
            float win_aspect = (float)px_w / (float)px_h;
            float img_aspect = (float)g_img.width / (float)g_img.height;
            if (win_aspect > img_aspect) {
                sx = img_aspect / win_aspect;
            } else {
                sy = win_aspect / img_aspect;
            }
        }
        GLint uScale = glGetUniformLocation(g_prog, "uScale");
        GLint uHas   = glGetUniformLocation(g_prog, "uHasImage");
        glUniform2f(uScale, sx, sy);
        glUniform1f(uHas, g_img.valid ? 1.f : 0.f);
        if (g_img.valid) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_tex);
            glUniform1i(g_uTex_loc, 0);
        }
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    if (moonbase_window_gl_swap_buffers(w) != 0) {
        fprintf(stderr, "preview: gl_swap_buffers failed: %s\n",
                moonbase_error_string(moonbase_last_error()));
    }
}

// ─────────────────────────────────────────────────────────────────────
// Prefs helpers
// ─────────────────────────────────────────────────────────────────────
//
// Keys: last-path (string), win-width / win-height (int). Loaded at
// launch and written whenever the user resizes or loads a different
// image.

static void prefs_load_window_size(int *w_points, int *h_points) {
    int w = moonbase_prefs_get_int("win-width",  640);
    int h = moonbase_prefs_get_int("win-height", 480);
    if (w < 200)  w = 640;
    if (h < 200)  h = 480;
    if (w > 4096) w = 4096;
    if (h > 4096) h = 4096;
    *w_points = w;
    *h_points = h;
}

static void prefs_save_window_size(int w_points, int h_points) {
    moonbase_prefs_set_int("win-width",  w_points);
    moonbase_prefs_set_int("win-height", h_points);
}

static void prefs_save_last_path(const char *path) {
    if (!path || !*path) return;
    moonbase_prefs_set_string("last-path", path);
}

// ─────────────────────────────────────────────────────────────────────
// Image-open flow shared by argv, drag-drop, prefs-reopen
// ─────────────────────────────────────────────────────────────────────

static bool open_path(mb_window_t *w, const char *path) {
    if (!load_image(path, &g_img)) return false;
    g_needs_upload = true;

    char title[1100];
    const char *bn = strrchr(path, '/');
    snprintf(title, sizeof(title), "%s — Preview", bn ? bn + 1 : path);
    moonbase_window_set_title(w, title);

    prefs_save_last_path(path);
    moonbase_window_request_redraw(w, 0, 0, 0, 0);
    fprintf(stderr, "preview: loaded %s (%dx%d)\n",
            path, g_img.width, g_img.height);
    return true;
}

// ─────────────────────────────────────────────────────────────────────
// Entry
// ─────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    if (moonbase_init(argc, argv) != MB_EOK) {
        fprintf(stderr, "preview: moonbase_init failed\n");
        return 1;
    }

    int wp = 640, hp = 480;
    prefs_load_window_size(&wp, &hp);

    mb_window_desc_t desc = {
        .version       = MOONBASE_WINDOW_DESC_VERSION,
        .title         = "Preview",
        .width_points  = wp,
        .height_points = hp,
        .render_mode   = MOONBASE_RENDER_GL,
        .flags         = MB_WINDOW_FLAG_CENTER,
    };
    mb_window_t *win = moonbase_window_create(&desc);
    if (!win) {
        fprintf(stderr, "preview: window_create failed: %s\n",
                moonbase_error_string(moonbase_last_error()));
        return 1;
    }
    moonbase_window_show(win);

    // Open policy: explicit argv > last-opened (if still exists) > empty.
    const char *target = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        target = argv[1];
    } else {
        const char *last = moonbase_prefs_get_string("last-path", NULL);
        if (last && last[0]) {
            FILE *probe = fopen(last, "rb");
            if (probe) { fclose(probe); target = last; }
        }
    }
    if (target) {
        open_path(win, target);
    }
    moonbase_window_request_redraw(win, 0, 0, 0, 0);

    bool running = true;
    while (running) {
        mb_event_t ev;
        int r = moonbase_wait_event(&ev, -1);
        if (r < 0) break;
        if (r == 0) continue;
        switch (ev.kind) {
        case MB_EV_WINDOW_REDRAW:
            if (ev.window == win) on_redraw(win);
            break;
        case MB_EV_WINDOW_RESIZED:
            if (ev.window == win) {
                prefs_save_window_size(ev.resize.new_width,
                                       ev.resize.new_height);
                moonbase_window_request_redraw(win, 0, 0, 0, 0);
            }
            break;
        case MB_EV_WINDOW_CLOSED:
            running = false;
            break;
        case MB_EV_KEY_DOWN:
            if ((ev.key.modifiers & MB_MOD_COMMAND) &&
                ev.key.keycode == 'q') {
                running = false;
            }
            break;
        case MB_EV_BACKING_SCALE_CHANGED:
            moonbase_window_request_redraw(win, 0, 0, 0, 0);
            break;

        // Drag & drop: accept any file and try to decode. Failed decodes
        // just leave the current image in place and log — the drop
        // isn't destructive.
        case MB_EV_DRAG_ENTER:
        case MB_EV_DRAG_OVER:
            // No per-OVER highlight yet — v0.1 just accepts silently.
            break;
        case MB_EV_DRAG_LEAVE:
            break;
        case MB_EV_DRAG_DROP:
            if (ev.drag.uri_count > 0 && ev.drag.uris && ev.drag.uris[0]) {
                char *path = uri_to_path(ev.drag.uris[0]);
                if (path) {
                    open_path(win, path);
                    free(path);
                } else {
                    fprintf(stderr, "preview: drop URI not a file: %s\n",
                            ev.drag.uris[0]);
                }
            }
            break;

        default:
            break;
        }
    }

    // Durable across relaunch without explicit sync, but force here so
    // a crash right after this function returns doesn't lose a resize.
    moonbase_prefs_sync();

    if (g_tex)  glDeleteTextures(1, &g_tex);
    if (g_vbo)  glDeleteBuffers(1, &g_vbo);
    if (g_vao)  glDeleteVertexArrays(1, &g_vao);
    if (g_prog) glDeleteProgram(g_prog);
    image_free(&g_img);

    moonbase_window_close(win);
    return 0;
}
