# CopyCatOS Desktop — Build configuration
#
# This component creates the wallpaper surface and desktop icon grid.
# It runs as a standalone process, separate from the window manager.
#
# Dependencies: Xlib (windowing), Cairo (2D rendering), Pango (text layout),
#               libjpeg (JPEG wallpaper loading)

project('desktop', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2'],
)

# X11 for window creation and event handling
x11_dep = dependency('x11')

# Cairo for 2D rendering (wallpaper, icons, menus)
cairo_dep = dependency('cairo')

# Cairo-Xlib bridge — lets Cairo draw directly onto X windows
cairo_xlib_dep = dependency('cairo-xlib')

# Pango + Cairo integration for text rendering (icon labels, menu items)
pangocairo_dep = dependency('pangocairo')

# libjpeg for loading JPEG wallpaper images
libjpeg_dep = dependency('libjpeg')

# Math library (for scaling calculations)
cc = meson.get_compiler('c')
math_dep = cc.find_library('m', required: true)

# MoonRock -> shell scale bridge. Same setup as dock and menubar — the
# client-side reader lives in ../moonrock/shell-api/ and is compiled directly
# into the desktop binary so per-output HiDPI scale tracks MoonRock live.
moonrock_shell_api_dir = '../moonrock/shell-api'

sources = [
  'src/main.c',
  'src/desktop.c',
  'src/wallpaper.c',
  'src/icons.c',
  'src/layout.c',
  'src/labels.c',
  'src/contextmenu.c',
  'src/dnd.c',
  moonrock_shell_api_dir / 'moonrock_scale.c',
]

executable('desktop',
  sources,
  include_directories: include_directories(moonrock_shell_api_dir),
  dependencies: [x11_dep, cairo_dep, cairo_xlib_dep, pangocairo_dep, libjpeg_dep, math_dep],
  install: true,
  install_dir: get_option('bindir'),
)
