# CopyCatOS — by Kyle Blizzard at Blizzard.show

# moonbase — CopyCatOS application framework library
#
# Phase B skeleton: builds libmoonbase.so.1 with every public symbol
# from the Phase A headers defined. Three symbols are real
# (moonbase_last_error, moonbase_error_string, moonbase_release);
# the other 41 are stubs that return MB_ENOSYS or set last-error
# and return a null-sentinel. Phase C replaces the stub bodies with
# real IPC to MoonRock — the ABI surface does not change.
#
# Targets:
#   libmoonbase.so.1.0.0 / libmoonbase.so.1 / libmoonbase.so
#   moonbase-launch      -- bwrap-based bundle launcher scaffold
#   mb-hello             -- tests/hello.c, exercises init+quit symbols
#
# Generated:
#   moonbase.pc          -- pkg-config file for third-party consumers
#
# Build:
#   meson setup build
#   meson compile -C build
#
# Install:
#   meson install -C build

project('moonbase', 'c',
  version: '1.0.0',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    'werror=true',
  ],
)

# _GNU_SOURCE unlocks realpath(3) and the other POSIX extensions
# Phase C+ will lean on (memfd_create, pidfd_open, getentropy, etc.).
add_project_arguments('-D_GNU_SOURCE', language: 'c')

# ---------------------------------------------------------------------
# Public headers
# ---------------------------------------------------------------------
#
# Phase A froze this set. Every app linking libmoonbase.so.1 includes
# through <moonbase.h> + one or more companions. All companion
# headers ship even in Phase B so downstream compile-gates against
# their MOONBASE_<DOMAIN>_API_VERSION macros resolve today.

public_headers = files(
  'include/moonbase.h',
  'include/moonbase_keychain.h',
  'include/moonbase_a11y.h',
  'include/moonbase_notifications.h',
  'include/moonbase_power.h',
  'include/moonbase_controller.h',
  'include/moonbase_clipboard.h',
  'include/moonbase_display.h',
  'include/moonbase_services.h',
  'include/moonbase_share.h',
  'include/moonbase_print.h',
  'include/moonbase_quicklook.h',
  'include/moonbase_spotlight.h',
)

ipc_headers = files(
  'include/moonbase/ipc/kinds.h',
)

install_headers(public_headers)
install_headers(ipc_headers, subdir: 'moonbase/ipc')

# ---------------------------------------------------------------------
# libmoonbase.so.1
# ---------------------------------------------------------------------

moonbase_lib_sources = files(
  'src/error.c',
  'src/eventloop.c',
  'src/init.c',
  'src/stubs.c',
  'src/window.c',
  'src/ipc/cbor.c',
  'src/ipc/debug.c',
  'src/ipc/frame.c',
  'src/ipc/transport.c',
  'src/server/server.c',
)

# Phase B library links no external deps on purpose — the stubs
# never call into Cairo, EGL, libsecret, AT-SPI, etc. Threads is
# listed anyway because the TLS last-error slot assumes libpthread
# is present at load time on glibc.
threads_dep = dependency('threads')

libmoonbase = shared_library(
  'moonbase',
  moonbase_lib_sources,
  include_directories: include_directories('include', 'src'),
  version: '1.0.0',
  soversion: '1',
  dependencies: [threads_dep],
  install: true,
)

# pkg-config so third-party code can `pkg-config --cflags --libs moonbase`.
# subdirs: the generated .pc adds -I${includedir} so top-level
# headers resolve as <moonbase.h>; the `moonbase/ipc` subdir makes
# <moonbase/ipc/kinds.h> resolve the same way.
pkg = import('pkgconfig')
pkg.generate(
  libmoonbase,
  name: 'moonbase',
  description: 'CopyCatOS application framework (libmoonbase.so.1)',
  version: meson.project_version(),
  subdirs: ['.'],
)

# ---------------------------------------------------------------------
# moonbase-launch — bwrap launcher scaffold
# ---------------------------------------------------------------------
#
# No library link — the launcher is a bwrap wrapper, it does not
# embed libmoonbase. Phase D grows this into the real Info.appc
# reader + quarantine-aware launcher.

executable(
  'moonbase-launch',
  'launcher/moonbase-launch.c',
  install: true,
)

# ---------------------------------------------------------------------
# Sandbox profiles
# ---------------------------------------------------------------------
#
# Bash arg-string generators for bwrap. Installed under
# ${datadir}/moonbase/sandbox/ so moonbase-launch can find them by
# well-known path. install_mode keeps them executable after install.

install_data(
  'sandbox/native.profile',
  'sandbox/webview.profile',
  install_dir: join_paths(get_option('datadir'), 'moonbase', 'sandbox'),
  install_mode: 'rwxr-xr-x',
)

# ---------------------------------------------------------------------
# mb-hello — minimum link test
# ---------------------------------------------------------------------
#
# Links libmoonbase and calls moonbase_init / moonbase_quit. Passing
# build == proof that every symbol from moonbase.h resolves, that
# the stub convention doesn't crash, and that the build system
# produces the right soname. Not installed.

executable(
  'mb-hello',
  'tests/hello.c',
  include_directories: include_directories('include'),
  link_with: libmoonbase,
  install: false,
)

# ---------------------------------------------------------------------
# mb-hello-handshake — Phase C slice 1 end-to-end test
# ---------------------------------------------------------------------
#
# Forks a mock compositor, runs moonbase_init against it, asserts the
# full HELLO / WELCOME / BYE exchange. Links libmoonbase plus reaches
# into src/ipc/ directly so it can drive the compositor side without
# duplicating the frame / CBOR code.

mb_hello_handshake = executable(
  'mb-hello-handshake',
  'tests/hello-handshake.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('hello-handshake', mb_hello_handshake, timeout: 10)

# ---------------------------------------------------------------------
# mb-server-smoke — Phase C slice 2 server-side test
# ---------------------------------------------------------------------
#
# Stands up an mb_server_t in the parent and forks two client children.
# Verifies that both clients handshake and bye cleanly, and that the
# server delivers connected + disconnected events for each.

mb_server_smoke = executable(
  'mb-server-smoke',
  'tests/server-smoke.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('server-smoke', mb_server_smoke, timeout: 10)

# ---------------------------------------------------------------------
# mb-server-fds — server-side SCM_RIGHTS round-trip
# ---------------------------------------------------------------------
#
# Child sends a frame with a pipe read-end attached; parent verifies
# the server surfaces the fd on the matching MB_SERVER_EV_FRAME and
# that reading from it yields the magic byte the child wrote. Pins
# the client→server fd-passing contract and the "server owns and
# closes received fds" ownership rule.

mb_server_fds = executable(
  'mb-server-fds',
  'tests/server-fds.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('server-fds', mb_server_fds, timeout: 10)

# ---------------------------------------------------------------------
# mb-window-create — Phase C slice 3a round-trip test
# ---------------------------------------------------------------------
#
# Parent runs mb_server_t and replies to MB_IPC_WINDOW_CREATE with a
# canned WINDOW_CREATE_REPLY. Child calls moonbase_window_create and
# asserts the returned handle carries the advertised size and scale.
# Proves mb_conn_request + client-side window.c work end-to-end.

mb_window_create = executable(
  'mb-window-create',
  'tests/window-create.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('window-create', mb_window_create, timeout: 10)

# ---------------------------------------------------------------------
# mb-event-window-closed — Phase C slice 4 event-pump test
# ---------------------------------------------------------------------
#
# Parent replies to WINDOW_CREATE with a canned reply, then immediately
# pushes a WINDOW_CLOSED frame at the client. Child asserts
# moonbase_wait_event delivers it as MB_EV_WINDOW_CLOSED with ev.window
# pointing at the handle returned from moonbase_window_create.

mb_event_window_closed = executable(
  'mb-event-window-closed',
  'tests/event-window-closed.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('event-window-closed', mb_event_window_closed, timeout: 10)
