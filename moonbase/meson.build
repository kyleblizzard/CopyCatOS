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
  'src/bundle/bundle.c',
  'src/bundle/info_appc.c',
  'src/bundle/quarantine.c',
  'src/bundle/toml_lite.c',
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

# Cairo is the CPU-side render backend for MOONBASE_RENDER_CAIRO windows.
# Apps use moonbase_window_cairo() to get a cairo_t on a shm-backed
# ARGB32 buffer, draw in points (the framework scales automatically),
# then moonbase_window_commit() hands the shm fd to MoonRock over
# SCM_RIGHTS. The GL path in later slices will NOT link cairo — it'll
# use EGL + GBM instead — but every cairo-rendering app expects it.
cairo_dep = dependency('cairo')

libmoonbase = shared_library(
  'moonbase',
  moonbase_lib_sources,
  include_directories: include_directories('include', 'src'),
  version: '1.0.0',
  soversion: '1',
  dependencies: [threads_dep, cairo_dep],
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
# moonbase-launch — bundle-aware, sandbox-aware launcher
# ---------------------------------------------------------------------
#
# Links libmoonbase so it can call mb_bundle_load (bundle-spec.md §8
# validation) on the target .appc before building the bwrap argv.

moonbase_launch = executable(
  'moonbase-launch',
  'launcher/moonbase-launch.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: true,
)

# ---------------------------------------------------------------------
# moonbase-consent — first-launch consent sheet (stub in this slice)
# ---------------------------------------------------------------------
#
# Phase D slice 4 ships a deterministic stub the launcher execs when a
# bundle's xattr says "pending". Slice 5 replaces this with a real
# Aqua consent sheet drawn through MoonBase. Installed to libexecdir so
# users can't invoke it directly (no good reason to; the helper assumes
# it's been forked by moonbase-launch).

moonbase_consent = executable(
  'moonbase-consent',
  'launcher/moonbase-consent.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: true,
  install_dir: get_option('libexecdir'),
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

# ---------------------------------------------------------------------
# mb-window-commit — Phase C slice 3c.1 shm-backed Cairo round-trip
# ---------------------------------------------------------------------
#
# Client paints a solid-red Cairo surface, calls moonbase_window_commit,
# and the parent asserts on the SCM_RIGHTS'd memfd: size matches the
# advertised stride*height, and the first ARGB32 pixel round-trips back
# as the red the client painted. Pins the whole client→compositor frame
# path in one go.

mb_window_commit = executable(
  'mb-window-commit',
  'tests/window-commit.c',
  include_directories: include_directories('include', 'src'),
  dependencies: [cairo_dep],
  link_with: libmoonbase,
  install: false,
)

test('window-commit', mb_window_commit, timeout: 10)

# ---------------------------------------------------------------------
# mb-input-route — Phase C slice 3c.3 focus + key-event pump test
# ---------------------------------------------------------------------
#
# Parent replies to WINDOW_CREATE then sends WINDOW_FOCUSED, KEY_DOWN,
# and KEY_UP frames in order. Child asserts moonbase_wait_event
# delivers them as MB_EV_WINDOW_FOCUSED, MB_EV_KEY_DOWN, MB_EV_KEY_UP
# with the right ev.focus.has_focus, ev.key.keycode, modifiers, and
# is_repeat fields. Pins the full input-routing translator.

mb_input_route = executable(
  'mb-input-route',
  'tests/input-route.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('input-route', mb_input_route, timeout: 10)

# ---------------------------------------------------------------------
# mb-backing-scale-changed — Phase C slice 3c.4 scale migration test
# ---------------------------------------------------------------------
#
# Parent replies to WINDOW_CREATE at scale=1.0 then immediately pushes
# MB_IPC_BACKING_SCALE_CHANGED advertising scale=1.5 on a different
# output_id. Child asserts moonbase_wait_event delivers
# MB_EV_BACKING_SCALE_CHANGED with the right old/new payload, that
# moonbase_window_backing_scale returns the new value, and that
# moonbase_window_backing_pixel_size scales up accordingly. Pins the
# full client-side translator + internal apply path.

mb_backing_scale = executable(
  'mb-backing-scale-changed',
  'tests/backing-scale-changed.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('backing-scale-changed', mb_backing_scale, timeout: 10)

# ---------------------------------------------------------------------
# mb-leakcheck-loop — Phase C plumbing-tasks line 62
# ---------------------------------------------------------------------
#
# In-process 1000-iteration loop of init/window_create/cairo/commit/
# close/quit. Asserts that heap bytes (mallinfo2 after malloc_trim)
# and /proc/self/fd count are stable across iterations. Stronger than
# 1000 separate processes because each process exit reaps everything
# regardless of leaks; a single long-running process surfaces any
# drip linearly.

mb_leakcheck_loop = executable(
  'mb-leakcheck-loop',
  'tests/leakcheck-loop.c',
  include_directories: include_directories('include', 'src'),
  dependencies: [cairo_dep],
  link_with: libmoonbase,
  install: false,
)

test('leakcheck-loop', mb_leakcheck_loop, timeout: 90)

# ---------------------------------------------------------------------
# mb-info-appc — Phase D slice 1 Info.appc schema loader test
# ---------------------------------------------------------------------
#
# Pure-unit test over the TOML loader + schema validator. Happy paths
# (C and web bundles, empty [permissions]) plus every error class the
# loader reports: missing [bundle], bad id regex, unknown category,
# unknown permission value, unknown language, web-lang without [web],
# ipc entry that isn't reverse-DNS, raw parse error, 64 KiB size cap,
# and unknown-table forward-compat.

mb_info_appc = executable(
  'mb-info-appc',
  'tests/info-appc.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('info-appc', mb_info_appc, timeout: 10)

# ---------------------------------------------------------------------
# mb-bundle-load — Phase D slice 2 on-disk loader test
# ---------------------------------------------------------------------
#
# Drives mb_bundle_load against real tmpdir .appc trees. Covers the
# happy path plus every failure in bundle-spec.md §8 except quarantine
# (its own slice): bad suffix, not-a-directory, missing Info.appc,
# executable missing, executable not-executable, executable outside
# MacOS/Resources, relative-symlink escape via realpath, absolute
# symlink anywhere in the tree, file at bundle root outside Contents/,
# and minimum-moonbase newer than library.

mb_bundle_load = executable(
  'mb-bundle-load',
  'tests/bundle-load.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('bundle-load', mb_bundle_load, timeout: 15)

# ---------------------------------------------------------------------
# mb-launch-smoke — Phase D slice 3 end-to-end launcher test
# ---------------------------------------------------------------------
#
# Builds a real fixture .appc in a tmpdir, forks moonbase-launch with
# an overridden $HOME + $MOONBASE_SANDBOX_DIR, then verifies the
# bundle's shell script ran inside the bwrap sandbox (by checking a
# marker file in the per-app data dir), that MOONBASE_BUNDLE_ID was
# set, that the Apple-style subdirs were created, and that the
# script's exit code propagates through bwrap back to the launcher.
# Skips (exit 77) if bwrap isn't on the machine — macOS CI will, Legion
# won't.

mb_launch_smoke = executable(
  'mb-launch-smoke',
  'tests/launch-smoke.c',
  include_directories: include_directories('include', 'src'),
  install: false,
)

test('launch-smoke', mb_launch_smoke,
  args: [moonbase_launch, meson.project_source_root() / 'sandbox'],
  depends: [moonbase_launch],
  timeout: 20,
)

# ---------------------------------------------------------------------
# mb-quarantine — Phase D slice 4 xattr gate unit test
# ---------------------------------------------------------------------

mb_quarantine = executable(
  'mb-quarantine',
  'tests/quarantine.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('quarantine', mb_quarantine, timeout: 10)

# ---------------------------------------------------------------------
# mb-launch-quarantine — Phase D slice 4 launcher integration test
# ---------------------------------------------------------------------
#
# Drives moonbase-launch against fixture bundles whose quarantine xattr
# is pre-set to pending / rejected. Verifies the consent helper is
# exec'd on pending, that the xattr is rewritten on both approve and
# reject outcomes, and that a rejected bundle short-circuits without
# running the consent helper at all. MOONBASE_CONSENT_BIN points the
# launcher at the stub moonbase-consent binary this test suite builds;
# MOONBASE_CONSENT_AUTO tells that stub how to decide.

mb_launch_quarantine = executable(
  'mb-launch-quarantine',
  'tests/launch-quarantine.c',
  include_directories: include_directories('include', 'src'),
  install: false,
)

test('launch-quarantine', mb_launch_quarantine,
  args: [moonbase_launch,
         meson.project_source_root() / 'sandbox',
         moonbase_consent],
  depends: [moonbase_launch, moonbase_consent],
  timeout: 30,
)

# ---------------------------------------------------------------------
# mb-data-dir-isolation — Phase D slice 6 sandbox privacy test
# ---------------------------------------------------------------------
#
# Builds two fixture bundles with different bundle-ids and launches
# each through moonbase-launch under a shared fake $HOME. App A writes
# a secret into its Preferences dir; App B's script tries to read it
# through the sibling path and verifies the tmpfs overlay hides it.
# Also asserts every Apple-style subdir exists at mode 0700.

mb_data_dir_isolation = executable(
  'mb-data-dir-isolation',
  'tests/data-dir-isolation.c',
  include_directories: include_directories('include', 'src'),
  install: false,
)

test('data-dir-isolation', mb_data_dir_isolation,
  args: [moonbase_launch,
         meson.project_source_root() / 'sandbox',
         moonbase_consent],
  depends: [moonbase_launch, moonbase_consent],
  timeout: 30,
)

# ---------------------------------------------------------------------
# examples/hello — reference MoonBase app + assembled .appc bundle
# ---------------------------------------------------------------------
#
# Produces build/hello.appc/Contents/{Info.appc, MacOS/hello}. Layout
# matches bundle-spec.md §1 and passes mb_bundle_load's validator. Not
# installed system-wide — the bundle is a build artifact the user runs
# from the build tree via `moonbase-launch build/hello.appc`.
#
# No Resources/ contents are shipped yet; §8 validation doesn't require
# them and a real AppIcon.icns slice lands once the icon loader uses it.

hello_exe = executable(
  'hello',
  'examples/hello/main.c',
  include_directories: include_directories('include'),
  dependencies: [cairo_dep],
  link_with: libmoonbase,
  install: false,
)

hello_appc = custom_target('hello-appc',
  output: 'hello.appc',
  input: [files('examples/hello/Info.appc'), hello_exe],
  command: [
    find_program('sh'), '-c',
    'set -eu; '
    + 'rm -rf "$0"; '
    + 'mkdir -p "$0/Contents/MacOS" "$0/Contents/Resources"; '
    + 'cp "$1" "$0/Contents/Info.appc"; '
    + 'cp "$2" "$0/Contents/MacOS/hello"; '
    + 'chmod 0755 "$0/Contents/MacOS/hello"',
    '@OUTPUT@', '@INPUT0@', '@INPUT1@',
  ],
  build_by_default: true,
)
