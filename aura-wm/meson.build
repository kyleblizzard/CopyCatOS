project('aura-wm', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2'],
)

# Core X11
x11_dep = dependency('x11')
xcomposite_dep = dependency('xcomposite')
xdamage_dep = dependency('xdamage')
xfixes_dep = dependency('xfixes')
xext_dep = dependency('xext')
xrender_dep = dependency('xrender')
xrandr_dep = dependency('xrandr')

# Rendering
cairo_dep = dependency('cairo')
cairo_xlib_dep = dependency('cairo-xlib')
pangocairo_dep = dependency('pangocairo')
libpng_dep = dependency('libpng')

# OpenGL
gl_dep = dependency('gl')
egl_dep = dependency('egl', required: false)

# IPC
# dbus_dep = dependency('dbus-1')  # Enable when shell IPC is needed

cc = meson.get_compiler('c')
math_dep = cc.find_library('m', required: true)

aura_wm_sources = [
  'src/main.c',
  'src/wm.c',
  'src/events.c',
  'src/client.c',
  'src/frame.c',
  'src/decor.c',
  'src/assets.c',
  'src/ewmh.c',
  'src/input.c',
  'src/crystal.c',
  'src/struts.c',
  'src/resize.c',
]

xcursor_dep = dependency('xcursor', required: false)

executable('aura-wm',
  aura_wm_sources,
  dependencies: [
    x11_dep, xcomposite_dep, xdamage_dep, xfixes_dep,
    xext_dep, xrender_dep, xrandr_dep,
    cairo_dep, cairo_xlib_dep, pangocairo_dep, libpng_dep,
    gl_dep, math_dep, xcursor_dep,
  ],
  install: true,
  install_dir: get_option('bindir'),
)
