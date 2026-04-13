# AuraOS Menu Bar — Build configuration
#
# This component creates the 22px global menu bar at the top of the screen.
# It displays the Apple logo, active app name, app-specific menus, and a
# system tray with clock, volume, and battery indicators.
#
# Runs as a standalone process, separate from the window manager.
#
# Dependencies: Xlib (windowing), Cairo (2D rendering), Pango (text layout),
#               libpng (PNG asset loading)

project('aura-menubar', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2'],
)

# X11 for window creation and event handling
x11_dep = dependency('x11')

# Cairo for 2D rendering (background gradient, icons, menus)
cairo_dep = dependency('cairo')

# Cairo-Xlib bridge — lets Cairo draw directly onto X windows
cairo_xlib_dep = dependency('cairo-xlib')

# Pango + Cairo integration for text rendering (menu items, clock, labels)
pangocairo_dep = dependency('pangocairo')

# libpng for loading PNG assets (Apple logo, background texture)
libpng_dep = dependency('libpng')

# Math library (for rounding, arc calculations in icon drawing)
cc = meson.get_compiler('c')
math_dep = cc.find_library('m', required: true)

sources = [
  'src/main.c',
  'src/menubar.c',
  'src/render.c',
  'src/appmenu.c',
  'src/systray.c',
  'src/apple.c',
]

executable('aura-menubar',
  sources,
  dependencies: [x11_dep, cairo_dep, cairo_xlib_dep, pangocairo_dep, libpng_dep, math_dep],
  install: true,
  install_dir: get_option('bindir'),
)
