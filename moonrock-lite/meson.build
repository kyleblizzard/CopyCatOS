# CopyCatOS — by Kyle Blizzard at Blizzard.show

# moonrock-lite — sidecar Aqua chrome process for foreign-distro
# .app launches.
#
# Inside a full CopyCatOS session moonrock owns chrome in-process via
# its OpenGL compositor. On a foreign distro (KDE Plasma 6, GNOME, XFCE)
# there is no compositor we control, so a small companion process
# decorates the bundle window from the outside: opens its own X
# connection, finds the bundle's top-level by WM_CLASS, raises an
# override-redirect chrome window above it, paints the title and menu
# bars via the shared menubar_render primitives, and forwards traffic-
# light clicks back to the bundle as ICCCM/EWMH ClientMessages.
#
# Spawned by moonbase-launcher when XDG_SESSION_DESKTOP is not
# CopyCatOS:
#   fork() in the launcher; child execs `moonrock-lite`; parent
#   execvp's into moonbase-launch. Lifetime is tied to the bundle
#   process via PR_SET_PDEATHSIG=SIGTERM, which the launcher arms in
#   the pre-exec child and moonrock-lite re-arms at startup.
#
# Pre-19.H this code lived inline as `run_chrome_stub()` inside
# moonbase/runtime/moonbase-launcher.c. Splitting it out gives chrome
# its own translation unit, its own restart story (moonbase-launcher
# survives a chrome crash; chrome can be restarted without re-exec'ing
# the bundle), and a clean seam for 19.H.1.b — migrating paint from
# menubar_render onto the shared host_chrome painter.

project('moonrock-lite', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2'],
)

# _GNU_SOURCE unlocks the same POSIX extensions moonrock relies on
# (strdup, M_PI, etc.) and matches the launcher's flag bar so a TU
# moved from one to the other compiles unchanged.
add_project_arguments('-D_GNU_SOURCE', language: 'c')

# ── X11 + rendering ────────────────────────────────────────────────────
x11_dep        = dependency('x11')
xrandr_dep     = dependency('xrandr')
cairo_dep      = dependency('cairo')
cairo_xlib_dep = dependency('cairo-xlib')
pangocairo_dep = dependency('pangocairo')
libpng_dep     = dependency('libpng')

# GLib/GIO for the menubar daemon's DBusMenu helpers — RegisterWindow
# arrives over the session bus; appmenu_bridge claims the registrar
# name and resolves wid → (service, path), and dbusmenu_client pulls
# the menu tree as a MenuNode root the chrome stub paints.
gio_dep        = dependency('gio-2.0')

# ── Shared sources ─────────────────────────────────────────────────────
#
# moonrock-lite reuses three categories of code:
#
#   1. menubar/render/menubar_render.c — Snow Leopard chrome paint
#      primitives. Same TU the menubar daemon links and the launcher
#      used to compile inline. One source of truth keeps daemon-side
#      menus and the chrome stub bit-identical at every scale.
#
#   2. menubar/src/menu_model.c, dbusmenu_client.c, appmenu_bridge.c —
#      the daemon's Legacy-Mode menu importers. Standalone-friendly
#      (no menubar globals); pull a Qt/GTK appmenu tree across DBus
#      and present it as MenuNode children for paint.
#
# All three are compiled into moonrock-lite directly rather than via a
# shared library — same model as moonbase-launcher used pre-extraction.
# When 19.H.1.b lands and the daemon also moves onto the shared host
# painter, this list collapses further.

moonrock_lite_sources = [
  'src/moonrock_lite.c',
  '../menubar/render/menubar_render.c',
  '../menubar/src/menu_model.c',
  '../menubar/src/dbusmenu_client.c',
  '../menubar/src/appmenu_bridge.c',
  # 19.H.1.b — shared host_chrome painter so the title strip is
  # bit-identical to moonrock's in-session window decoration.
  '../moonbase/src/host/host_chrome.c',
]

executable('moonrock-lite',
  moonrock_lite_sources,
  include_directories: include_directories(
    '../menubar/render',
    '../menubar/src',
    '../moonbase/src/host',
  ),
  dependencies: [
    x11_dep, xrandr_dep,
    cairo_dep, cairo_xlib_dep, pangocairo_dep, libpng_dep,
    gio_dep,
  ],
  install: true,
  install_dir: get_option('bindir'),
)
