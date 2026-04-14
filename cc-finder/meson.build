# CopiCatOS Finder — Build configuration
#
# CCFinder is the Snow Leopard-style file manager for CopiCatOS.
# It creates a normal window (framed by cc-wm) with a toolbar,
# sidebar source list, and icon/list content view.
#
# Dependencies: Xlib (windowing), Cairo (2D rendering), Pango (text layout),
#               libpng (icon loading from AquaKDE theme)

project('cc-finder', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2'],
)

# X11 for window creation and event handling
x11_dep = dependency('x11')

# Cairo for 2D rendering (toolbar, sidebar, icons, content)
cairo_dep = dependency('cairo')

# Cairo-Xlib bridge — lets Cairo draw directly onto X windows
cairo_xlib_dep = dependency('cairo-xlib')

# Pango + Cairo integration for text rendering (labels, breadcrumbs, search)
pangocairo_dep = dependency('pangocairo')

# libpng for loading PNG icons from the AquaKDE icon theme
libpng_dep = dependency('libpng')

# Math library (for M_PI and gradient calculations)
cc = meson.get_compiler('c')
math_dep = cc.find_library('m', required: true)

sources = [
  'src/main.c',
  'src/finder.c',
  'src/sidebar.c',
  'src/content.c',
  'src/toolbar.c',
]

executable('cc-finder',
  sources,
  dependencies: [x11_dep, cairo_dep, cairo_xlib_dep, pangocairo_dep, libpng_dep, math_dep],
  install: true,
  install_dir: get_option('bindir'),
)
