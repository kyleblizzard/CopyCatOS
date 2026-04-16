project('cc-wm', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2'],
)

# _GNU_SOURCE needed for M_PI and other POSIX extensions in C11 mode.
# MR_EMBEDDED_IN_WM tells MoonRock's wm_compat.h to use CopyCatOS's real
# CCWM/Client structs from wm.h instead of its own minimal definitions.
add_project_arguments('-D_GNU_SOURCE', '-DMR_EMBEDDED_IN_WM', language: 'c')

# ── Core X11 dependencies ──────────────────────────────────────────────
x11_dep        = dependency('x11')
xcomposite_dep = dependency('xcomposite')
xdamage_dep    = dependency('xdamage')
xfixes_dep     = dependency('xfixes')
xext_dep       = dependency('xext')
xrender_dep    = dependency('xrender')
xrandr_dep     = dependency('xrandr')

# ── Rendering ──────────────────────────────────────────────────────────
cairo_dep      = dependency('cairo')
cairo_xlib_dep = dependency('cairo-xlib')
pangocairo_dep = dependency('pangocairo')
libpng_dep     = dependency('libpng')

# ── OpenGL ─────────────────────────────────────────────────────────────
gl_dep  = dependency('gl')
egl_dep = dependency('egl', required: false)

# ── System libraries ───────────────────────────────────────────────────
cc = meson.get_compiler('c')
math_dep = cc.find_library('m', required: true)
dl_dep   = cc.find_library('dl', required: true)

# ── XInput2, XTest, XCursor ───────────────────────────────────────────
xi_dep      = dependency('xi')
xtst_dep    = dependency('xtst')
xcursor_dep = dependency('xcursor', required: false)

# ── MoonRock Compositor ─────────────────────────────────────────────────
#
# MoonRock is CopyCatOS's custom OpenGL X11 compositor. It exists as a
# standalone project at /Volumes/Development/MoonRock/ and can be built
# independently as libmoonrock.so.
#
# For CopyCatOS, we compile MoonRock's sources directly into the WM binary.
# The moonrock_*.c files in src/ are kept in sync with MoonRock's src/.
# MR_EMBEDDED_IN_WM is defined so MoonRock uses CopyCatOS's real
# CCWM/Client types instead of its compatibility shim.
#
# To use MoonRock as a pre-built library instead, install it and use:
#   moonrock_dep = dependency('moonrock')

# MoonRock sources are developed here in CopyCatOS/cc-wm/src/.
# This is the authoritative copy. To publish changes to the standalone
# MoonRock repo, run: scripts/sync-moonrock.sh
# That script copies all moonrock_*.c/h files to /Volumes/Development/MoonRock/src/.
moonrock_sources = [
  'src/moonrock_shaders.c',
  'src/moonrock_anim.c',
  'src/moonrock_mission_control.c',
  'src/moonrock_touch.c',
  'src/moonrock_display.c',
  'src/moonrock_color.c',
  'src/moonrock_plugin.c',
  'src/moonrock_robust.c',
]

# ── CopyCatOS Window Manager sources ─────────────────────────────────────
cc_wm_sources = [
  'src/main.c',
  'src/wm.c',
  'src/events.c',
  'src/client.c',
  'src/frame.c',
  'src/decor.c',
  'src/assets.c',
  'src/ewmh.c',
  'src/input.c',
  'src/moonrock.c',
  'src/struts.c',
  'src/resize.c',
]

executable('cc-wm',
  cc_wm_sources + moonrock_sources,
  dependencies: [
    x11_dep, xcomposite_dep, xdamage_dep, xfixes_dep,
    xext_dep, xrender_dep, xrandr_dep,
    cairo_dep, cairo_xlib_dep, pangocairo_dep, libpng_dep,
    gl_dep, math_dep, dl_dep, xcursor_dep,
    xi_dep, xtst_dep,
  ],
  install: true,
  install_dir: get_option('bindir'),
)
