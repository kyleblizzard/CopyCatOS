# CopyCatOS — by Kyle Blizzard at Blizzard.show

# moonrock-host — active X11 frontend for *native* libmoonbase apps on
# foreign distros.
#
# Sibling to moonrock-lite. They split duty by app toolkit:
#
#   - moonrock-lite: Qt/GTK/AppImage bundles. The bundle process opens
#     its own X window (Qt/GTK does this); moonrock-lite waits for it
#     in _NET_CLIENT_LIST and decorates it externally.
#
#   - moonrock-host (this binary): native libmoonbase bundles. The
#     bundle process never opens an X window — it talks IPC to us and
#     hands over a backing buffer. moonrock-host is the X frontend:
#     it creates the window from MB_IPC_WINDOW_CREATE, allocates the
#     Cairo ARGB32 backing surface at the output's effective scale,
#     paints Aqua chrome via the shared host_chrome painter, and
#     replies with the window-id + scale.
#
# One process per .app. Single client. Per-bundle Unix socket at
#     $XDG_RUNTIME_DIR/moonbase/moonbase-<bundle-id>.sock
# so two .apps don't fight over the flat moonbase.sock that
# moonrock-lite uses for foreign-toolkit apps.
#
# moonbase-launch picks moonrock-lite vs moonrock-host based on the
# bundle's Info.appc toolkit hint (qt/gtk/appimage → lite; native or
# unset → host). That dispatch lives in moonbase-launcher.c.

project('moonrock-host', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2'],
)

# _GNU_SOURCE matches the rest of the moonrock/moonbase flag bar so a
# TU can move between them without recompiling.
add_project_arguments('-D_GNU_SOURCE', language: 'c')

# ── X11 + rendering ────────────────────────────────────────────────────
x11_dep        = dependency('x11')
xrandr_dep     = dependency('xrandr')
cairo_dep      = dependency('cairo')
cairo_xlib_dep = dependency('cairo-xlib')
pangocairo_dep = dependency('pangocairo')
libpng_dep     = dependency('libpng')

# ── Shared sources ─────────────────────────────────────────────────────
#
# Pulled-in source files (no separate static lib, mirrors the pattern in
# moonrock-lite/meson.build and moonrock/meson.build):
#
#   1. moonbase/src/host/ — the compositor-agnostic helpers. We need
#      host_chrome (title-strip painter), host_util (default socket
#      path helper, geometry math, ts), and host_protocol (CBOR
#      parsers + reply encoders for WINDOW_CREATE/REPLY).
#
#   2. moonbase/src/ipc/ + src/server/ — TLV framing, CBOR codec, and
#      the non-blocking server state machine. Same source-pull pattern
#      moonrock-lite uses for HELLO/WELCOME; we extend it here with
#      the WINDOW_* dispatch.

moonrock_host_sources = [
  'src/moonrock_host.c',
  '../moonbase/src/host/host_chrome.c',
  '../moonbase/src/host/host_util.c',
  '../moonbase/src/host/host_protocol.c',
  '../moonbase/src/ipc/cbor.c',
  '../moonbase/src/ipc/frame.c',
  '../moonbase/src/server/server.c',
]

executable('moonrock-host',
  moonrock_host_sources,
  include_directories: include_directories(
    '../moonbase/include',
    '../moonbase/src',
    '../moonbase/src/host',
    '../moonbase/src/ipc',
    '../moonbase/src/server',
  ),
  dependencies: [
    x11_dep, xrandr_dep,
    cairo_dep, cairo_xlib_dep, pangocairo_dep, libpng_dep,
  ],
  install: true,
  install_dir: get_option('bindir'),
)
