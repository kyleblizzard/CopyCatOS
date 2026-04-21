// CopyCatOS — by Kyle Blizzard at Blizzard.show

// gl-hello.app — minimum end-to-end proof of the MOONBASE_RENDER_GL
// ABI. Creates a 480×320-point GL window, clears it to a steady Aqua
// blue, and draws a single white-to-gold triangle. Re-renders on
// every MB_EV_WINDOW_REDRAW. Closes on Cmd-Q or the red traffic
// light.
//
// The app never allocates its own FBO — it renders into the default
// framebuffer of the framework-provided EGL pbuffer. After draw it
// calls moonbase_window_gl_swap_buffers() once per frame.
//
// Shader pipeline is intentionally trivial: one vertex attribute
// (vec2 position), one per-vertex colour attribute (vec3), and a
// fragment shader that passes the colour straight through. Keeps the
// file small enough to read top-to-bottom and still exercises the
// full vertex → fragment → swap chain through MoonBase's GL path.

#include <moonbase.h>

#include <GLES3/gl3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Aqua blue to match Snow Leopard's default Desktop.
static const float kClearR = 0.141f;
static const float kClearG = 0.353f;
static const float kClearB = 0.643f;
static const float kClearA = 1.000f;

// Compile a single shader stage and emit the driver's log if it
// fails. Real apps ship pre-compiled binaries, but for a reference
// app readable source is worth the couple of milliseconds at launch.
static GLuint compile_shader(GLenum kind, const char *src) {
    GLuint sh = glCreateShader(kind);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "gl-hello: shader compile failed:\n%s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint link_program(const char *vsrc, const char *fsrc) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   vsrc);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fsrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aColor");
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "gl-hello: program link failed:\n%s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// Lazy-initialised GL resources. Allocated on first redraw so the EGL
// context the framework sets up is definitely current.
static GLuint g_prog = 0;
static GLuint g_vbo  = 0;
static GLuint g_vao  = 0;

static void ensure_gl_resources(void) {
    if (g_prog) return;
    static const char *vs_src =
        "#version 300 es\n"
        "in vec2 aPos;\n"
        "in vec3 aColor;\n"
        "out vec3 vColor;\n"
        "void main() {\n"
        "    vColor = aColor;\n"
        "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "}\n";
    static const char *fs_src =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in  vec3 vColor;\n"
        "out vec4 fragColor;\n"
        "void main() { fragColor = vec4(vColor, 1.0); }\n";
    g_prog = link_program(vs_src, fs_src);
    if (!g_prog) return;

    // Pos (x,y) + Colour (r,g,b) interleaved. Triangle centred in
    // clip space with top vertex white and base vertices gold — the
    // exact palette isn't the point, the point is that vertex-
    // interpolated colour survives the whole pipeline.
    static const float verts[] = {
    //   x      y      r     g     b
         0.0f,  0.6f,  1.0f, 1.0f, 1.0f,
        -0.6f, -0.5f,  1.0f, 0.82f, 0.32f,
         0.6f, -0.5f,  1.0f, 0.82f, 0.32f,
    };
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float),
                          (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float),
                          (void *)(2*sizeof(float)));
}

// Redraw handler. MB_EV_WINDOW_REDRAW is the signal that MoonRock
// wants a fresh frame; we make the GL context current, draw, and
// commit. The framework handles the glReadPixels → SHM transport.
static void on_redraw(mb_window_t *w) {
    if (moonbase_window_gl_make_current(w) != MB_EOK) {
        fprintf(stderr, "gl-hello: gl_make_current failed: %s\n",
                moonbase_error_string(moonbase_last_error()));
        return;
    }
    ensure_gl_resources();

    int px_w = 0, px_h = 0;
    moonbase_window_backing_pixel_size(w, &px_w, &px_h);
    glViewport(0, 0, px_w, px_h);

    glClearColor(kClearR, kClearG, kClearB, kClearA);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (g_prog) {
        glUseProgram(g_prog);
        glBindVertexArray(g_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    if (moonbase_window_gl_swap_buffers(w) != 0) {
        fprintf(stderr, "gl-hello: gl_swap_buffers failed: %s\n",
                moonbase_error_string(moonbase_last_error()));
    }
}

int main(int argc, char **argv) {
    if (moonbase_init(argc, argv) != MB_EOK) {
        fprintf(stderr, "gl-hello: moonbase_init failed\n");
        return 1;
    }

    mb_window_desc_t desc = {
        .version        = MOONBASE_WINDOW_DESC_VERSION,
        .title          = "GL Hello",
        .width_points   = 480,
        .height_points  = 320,
        .render_mode    = MOONBASE_RENDER_GL,
        .flags          = MB_WINDOW_FLAG_CENTER,
    };
    mb_window_t *win = moonbase_window_create(&desc);
    if (!win) {
        fprintf(stderr, "gl-hello: window_create failed: %s\n",
                moonbase_error_string(moonbase_last_error()));
        return 1;
    }
    moonbase_window_show(win);
    // Kick the first frame — apps may get their first REDRAW event
    // before the loop even spins up, but requesting it explicitly is
    // the right "I'm ready" signal.
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
            // Framework dropped the old pbuffer; next redraw will
            // allocate a new one at the right size. Just ask for
            // one.
            moonbase_window_request_redraw(win, 0, 0, 0, 0);
            break;
        default:
            break;
        }
    }

    if (g_vbo)  glDeleteBuffers(1, &g_vbo);
    if (g_vao)  glDeleteVertexArrays(1, &g_vao);
    if (g_prog) glDeleteProgram(g_prog);
    moonbase_window_close(win);
    return 0;
}
