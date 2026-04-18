# Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
# This code is publicly visible for portfolio purposes only.
# Unauthorized copying, forking, or distribution of this file,
# via any medium, is strictly prohibited.

project('cc-sysprefs', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2'],
)

# Dependencies — same stack as every CopyCatOS shell component
x11_dep        = dependency('x11')
cairo_dep      = dependency('cairo')
cairo_xlib_dep = dependency('cairo-xlib')
pangocairo_dep = dependency('pangocairo')
libpng_dep     = dependency('libpng')
udev_dep       = dependency('libudev')
cc             = meson.get_compiler('c')
math_dep       = cc.find_library('m', required: true)

# _GNU_SOURCE gives us M_PI from math.h under strict C11
add_project_arguments('-D_GNU_SOURCE', language: 'c')

# cc-inputmap shared sources — config_editor reads/writes input.conf,
# scanner provides device enumeration and code name lookups.
# These live in ../cc-inputmap/src/ and are compiled directly into cc-sysprefs
# (no shared library needed since only two programs use them).
inputmap_dir = '../cc-inputmap/src'

sources = [
  'src/main.c',
  'src/sysprefs.c',
  'src/toolbar.c',
  'src/icongrid.c',
  'src/registry.c',
  'src/paneview.c',
  'src/panes/stub.c',
  'src/panes/dock.c',
  'src/panes/controller.c',
  'src/panes/power.c',
  inputmap_dir / 'config_editor.c',
  inputmap_dir / 'scanner.c',
]

executable('cc-sysprefs', sources,
  dependencies: [x11_dep, cairo_dep, cairo_xlib_dep, pangocairo_dep, libpng_dep, udev_dep, math_dep],
  include_directories: include_directories(inputmap_dir),
  install: true,
  install_dir: get_option('bindir'),
)
