# CopyCatOS — by Kyle Blizzard at Blizzard.show

# ============================================================================
# meson.build — Build configuration for dock
#
# Meson is a build system (like CMake or Make) that generates ninja build files.
# This file tells Meson what our project needs: which source files to compile,
# what libraries to link against, and how to install the final binary.
# ============================================================================

project('dock', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2'],
)

# --- External library dependencies ---
# Each dependency() call uses pkg-config to find the library's headers and
# linker flags on the system. If any of these are missing, the build will
# fail with a clear error message telling you what to install.

x11_dep = dependency('x11')              # Xlib — low-level X11 window management
cairo_dep = dependency('cairo')          # Cairo — 2D vector graphics library
cairo_xlib_dep = dependency('cairo-xlib') # Cairo's Xlib backend for drawing on X windows
pangocairo_dep = dependency('pangocairo') # Pango + Cairo — text rendering with font shaping
libpng_dep = dependency('libpng')        # libpng — for loading PNG icon files

# The C compiler object lets us search for system libraries that don't have
# pkg-config files. The math library (libm) provides sin(), cos(), fabs(), etc.
cc = meson.get_compiler('c')
math_dep = cc.find_library('m', required: true)

# --- Source files ---
# Every .c file that contains code for the dock. Headers (.h) are found
# automatically through #include directives.
sources = [
  'src/main.c',
  'src/dock.c',
  'src/config.c',
  'src/shelf.c',
  'src/magnify.c',
  'src/bounce.c',
  'src/reflect.c',
  'src/indicator.c',
  'src/launch.c',
  'src/menu.c',
  'src/dnd.c',
  'src/poof.c',
  'src/tooltip.c',
  'src/stacks.c',
]

# --- Build the executable ---
# This compiles all source files and links them with our dependencies into
# a single binary called 'dock'. install: true means `meson install`
# will copy it to the system bin directory (usually /usr/local/bin).
executable('dock', sources,
  dependencies: [x11_dep, cairo_dep, cairo_xlib_dep, pangocairo_dep, libpng_dep, math_dep],
  install: true,
  install_dir: get_option('bindir'),
)
