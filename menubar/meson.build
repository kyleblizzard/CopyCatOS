# CopyCatOS Menu Bar — Build configuration
#
# This component creates the 22px global menu bar at the top of the screen.
# It displays the Apple logo, active app name, app-specific menus, and a
# system tray with clock, volume, and battery indicators.
#
# Runs as a standalone process, separate from the window manager.
#
# Dependencies: Xlib (windowing), Cairo (2D rendering), Pango (text layout),
#               libpng (PNG asset loading)

project('menubar', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2'],
)

# _GNU_SOURCE needed for sigaction/SA_RESTART, M_PI, and other POSIX extensions
add_project_arguments('-D_GNU_SOURCE', language: 'c')

# X11 for window creation and event handling
x11_dep = dependency('x11')

# XTest — synthesises the Ctrl+Space chord when the user clicks the
# spotlight glyph, so searchsystem's global hotkey grab fires the
# same way it would from a physical press.
xtst_dep = dependency('xtst')

# Cairo for 2D rendering (background gradient, icons, menus)
cairo_dep = dependency('cairo')

# Cairo-Xlib bridge — lets Cairo draw directly onto X windows
cairo_xlib_dep = dependency('cairo-xlib')

# Pango + Cairo integration for text rendering (menu items, clock, labels)
pangocairo_dep = dependency('pangocairo')

# libpng for loading PNG assets (Apple logo, background texture)
libpng_dep = dependency('libpng')

# GDBus (gio-2.0) for the DBusMenu / AppMenu.Registrar bridge — slice 18-A.
# Legacy Mode GTK3 / Qt5 / Qt6-appmenu apps publish their menus over
# com.canonical.AppMenu.Registrar; we own that name and drive it through
# the existing select() loop via g_main_context_query/check/dispatch.
gio_dep = dependency('gio-2.0')

# Math library (for rounding, arc calculations in icon drawing)
cc = meson.get_compiler('c')
math_dep = cc.find_library('m', required: true)

# MoonRock -> shell scale bridge. Client-side reader lives in
# ../moonrock/shell-api/ and is compiled directly into menubar. Every shell
# component (dock, desktop, systemcontrol) will eventually do the same —
# a standalone helper library isn't worth the packaging overhead for three
# C files that change as a set with moonrock's display module.
moonrock_shell_api_dir = '../moonrock/shell-api'

# menu_model + dbusmenu_client are split off the menubar executable so
# the mb-dbusmenu-dump debug tool (slice 18-B) can link the exact same
# translation units without pulling in menubar's X11/Cairo/Pango stack.
# No shared library — just source reuse. Handful of C files.
legacy_menu_sources = [
  'src/menu_model.c',
  'src/dbusmenu_client.c',
]

sources = [
  'src/main.c',
  'src/menubar.c',
  'src/render.c',
  'src/appmenu.c',
  'src/systray.c',
  'src/apple.c',
  'src/appmenu_bridge.c',
  moonrock_shell_api_dir / 'moonrock_scale.c',
] + legacy_menu_sources

executable('menubar',
  sources,
  include_directories: include_directories(moonrock_shell_api_dir),
  dependencies: [x11_dep, xtst_dep, cairo_dep, cairo_xlib_dep, pangocairo_dep, libpng_dep, gio_dep, math_dep],
  install: true,
  install_dir: get_option('bindir'),
)

# mb-dbusmenu-dump — Debug tool for slice 18-B. Connects to a
# com.canonical.dbusmenu endpoint (as reported by the appmenu
# registrar) and pretty-prints the imported tree. Not installed;
# available under meson's build dir for hand-testing GTK3 / Qt5 / Qt6
# Legacy Mode apps.
executable('mb-dbusmenu-dump',
  ['src/tools/mb-dbusmenu-dump.c'] + legacy_menu_sources,
  dependencies: [gio_dep],
  install: false,
)
