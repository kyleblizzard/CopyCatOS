# Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
# This code is publicly visible for portfolio purposes only.
# Unauthorized copying, forking, or distribution of this file,
# via any medium, is strictly prohibited.

# ─── CopyCatOS Spotlight ───
# Meson build file for cc-spotlight — a Spotlight-style search overlay
# activated by Ctrl+Space. Built with Xlib, Cairo, and Pango.

project('cc-spotlight', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2'],
)

# ─── Dependencies ───
# x11:        Xlib for window creation, event handling, key grabs
# cairo:      2D vector graphics for rendering the overlay
# cairo-xlib: Cairo surface backed by an Xlib drawable
# pangocairo: High-quality text rendering with font shaping
# libpng:     Loading PNG icon files from disk
# m (math):   Math functions like round(), floor(), etc.

x11_dep       = dependency('x11')
cairo_dep     = dependency('cairo')
cairo_xlib_dep = dependency('cairo-xlib')
pangocairo_dep = dependency('pangocairo')
libpng_dep    = dependency('libpng')

cc       = meson.get_compiler('c')
math_dep = cc.find_library('m', required: true)

# ─── Sources ───
# main.c      — Entry point, signal handling, event loop kickoff
# spotlight.c — Core overlay window, hotkey grab, event dispatch
# search.c    — .desktop file parsing, search/filter logic
# render.c    — Cairo/Pango drawing of the overlay UI
# icons.c     — Icon resolution, loading, and caching

sources = [
  'src/main.c',
  'src/spotlight.c',
  'src/search.c',
  'src/render.c',
  'src/icons.c',
]

executable('cc-spotlight', sources,
  dependencies: [
    x11_dep,
    cairo_dep,
    cairo_xlib_dep,
    pangocairo_dep,
    libpng_dep,
    math_dep,
  ],
  install: true,
  install_dir: get_option('bindir'),
)
