# CopyCatOS — by Kyle Blizzard at Blizzard.show

# inputsession — CopyCatOS session bridge
#
# Connects to inputd via Unix socket and provides X11 context
# (active window tracking, desktop action dispatch).
#
# Build:
#   meson setup build
#   meson compile -C build

project('inputsession', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2'],
)

# _GNU_SOURCE enables some POSIX extensions we use (e.g., strnlen)
add_project_arguments('-D_GNU_SOURCE', language: 'c')

# X11 — needed for display connection, window properties, atoms, events
x11_dep = dependency('x11')

# Math library — linked for completeness (some IPC helpers may need it)
cc = meson.get_compiler('c')
math_dep = cc.find_library('m', required: true)

sources = [
  'src/main.c',
  'src/session.c',
  'src/window_track.c',
  'src/actions.c',
]

executable('inputsession', sources,
  dependencies: [x11_dep, math_dep],
  install: true,
  install_dir: get_option('bindir'),
)
