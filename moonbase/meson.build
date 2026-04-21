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

# Compile-time default for the moonbase-consent binary path used by
# the consent responder. Tests override via $MOONBASE_CONSENT_BIN;
# production installs resolve through libexecdir.
moonbase_consent_default_path = get_option('prefix') / get_option('libexecdir') / 'moonbase-consent'
add_project_arguments(
  '-DMOONBASE_CONSENT_PATH="@0@"'.format(moonbase_consent_default_path),
  language: 'c',
)

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
  'src/consents.c',
  'src/entitlements.c',
  'src/error.c',
  'src/eventloop.c',
  'src/gl_path.c',
  'src/init.c',
  'src/stubs.c',
  'src/version.c',
  'src/window.c',
  'src/bundle/bundle.c',
  'src/bundle/info_appc.c',
  'src/bundle/quarantine.c',
  'src/bundle/toml_lite.c',
  'src/ipc/cbor.c',
  'src/ipc/debug.c',
  'src/ipc/frame.c',
  'src/ipc/transport.c',
  'src/keychain/keychain.c',
  'src/server/server.c',
  'src/server/consent_responder.c',
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

# libsecret is the Secret Service D-Bus client library the keychain
# adapter wraps. One shared libmoonbase link means every language
# binding (C, Python, Rust, Swift, Web) gets keychain for free. The
# glib2 pull-in is transitive via libsecret's pkg-config file.
libsecret_dep = dependency('libsecret-1')

# EGL + OpenGL ES drive MOONBASE_RENDER_GL windows. The v1 internal
# path is EGL pbuffer + glReadPixels back into the existing SHM
# commit pipeline — MoonRock doesn't need to learn dmabuf import for
# this slice. A later slice swaps the internals for zero-copy GBM +
# dmabuf without touching the public API.
egl_dep   = dependency('egl')
glesv2_dep = dependency('glesv2')

# Preview.appdev decodes PNG and JPEG via the distro-stock libraries.
# Pulled in only for the example executable, not for libmoonbase itself —
# the framework doesn't own image decode.
libpng_dep  = dependency('libpng')
libjpeg_dep = dependency('libjpeg')

libmoonbase = shared_library(
  'moonbase',
  moonbase_lib_sources,
  include_directories: include_directories('include', 'src'),
  version: '1.0.0',
  soversion: '1',
  dependencies: [threads_dep, cairo_dep, libsecret_dep, egl_dep, glesv2_dep],
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
# validation) on the target .app/.appdev before building the bwrap argv.

moonbase_launch = executable(
  'moonbase-launch',
  'launcher/moonbase-launch.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: true,
)

# ---------------------------------------------------------------------
# moonbase-consent — first-launch consent sheet + consent-recorder CLI
# ---------------------------------------------------------------------
#
# Two surfaces in one binary, dispatched by argv[1]:
#   * <bundle-path> <bundle-id>
#       first-launch sheet exec'd by moonbase-launch when a bundle's
#       user.moonbase.quarantine xattr is "pending". A follow-up slice
#       replaces the text-mode prompt with a MoonBase-drawn Aqua sheet.
#   * <allow|deny> <group> <value> [bundle-id]
#       per-capability consent recorder — the `defaults write` of the
#       consents.toml store. Direct invocation from a shell is expected
#       and tested (tests/consent-cli.c).
# Installed to libexecdir for now since the primary consumer is still
# moonbase-launch; promotion to bindir can follow once the admin
# surface stabilizes.

moonbase_consent = executable(
  'moonbase-consent',
  'launcher/moonbase-consent.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: true,
  install_dir: get_option('libexecdir'),
)

# ---------------------------------------------------------------------
# moonbase-version — print libmoonbase.so runtime version to stdout
# ---------------------------------------------------------------------
#
# One-line shell surface for the runtime API version. Prints
# `MAJOR.MINOR.PATCH\n` and exits 0 — designed for `$(moonbase-version)`
# in shell consumers (menubar's About item, future About sheet, build
# scripts that want to assert an installed runtime).
#
# Installed to bindir because it's a public, user-invocable tool, not
# a launcher helper like moonbase-consent. Distinct from a future
# copycatos-version CLI, which will report the OS release — these are
# two axes and must not collapse into one command.

moonbase_version = executable(
  'moonbase-version',
  'tools/moonbase-version.c',
  include_directories: include_directories('include'),
  link_with: libmoonbase,
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
# mb-runtime-version — moonbase_runtime_version() symbol test
# ---------------------------------------------------------------------
#
# Links libmoonbase and asserts the runtime version query echoes
# MOONBASE_API_VERSION and shares a major with it. Proves the symbol
# is exported, callable, and returns a sane value — the prerequisite
# for any eventual "About CopyCatOS" / "About MoonBase" surface that
# wants to show the live runtime version instead of the app's
# compile-time macro.

mb_runtime_version = executable(
  'mb-runtime-version',
  'tests/runtime-version.c',
  include_directories: include_directories('include'),
  link_with: libmoonbase,
  install: false,
)

test('runtime-version', mb_runtime_version, timeout: 5)

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
# mb-text-input-route — TEXT_INPUT translator pump test
# ---------------------------------------------------------------------
#
# Companion to input-route: parent replies to WINDOW_CREATE, sends
# WINDOW_FOCUSED then two back-to-back MB_IPC_TEXT_INPUT frames
# ("a", "Hello"). Child asserts moonbase_wait_event delivers them as
# MB_EV_TEXT_INPUT with ev.text.text pointing at the right UTF-8 bytes
# and that the second pump call correctly releases the first frame's
# owned string without crashing (the lifetime contract).

mb_text_input_route = executable(
  'mb-text-input-route',
  'tests/text-input-route.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('text-input-route', mb_text_input_route, timeout: 10)

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
# mb-drag-events — DRAG_ENTER / OVER / LEAVE / DROP translator test
# ---------------------------------------------------------------------
#
# Parent replies to WINDOW_CREATE, then pushes a six-frame drag script:
# ENTER with two URIs, OVER position-only, DROP with two URIs, a second
# ENTER with a zero-length URI list, LEAVE, then a tail OVER. Child
# asserts each event arrives with the right kind, window, coords,
# modifiers, uri_count and (where applicable) uris[] contents. Also
# exercises the pump-lifetime contract — a second drag payload must
# invalidate the previous pointer without leaking or crashing.

mb_drag_events = executable(
  'mb-drag-events',
  'tests/drag-events.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('drag-events', mb_drag_events, timeout: 10)

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
# mb-gl-path — MOONBASE_RENDER_GL internal path unit test
# ---------------------------------------------------------------------
#
# Drives src/gl_path.c directly — no moonrock required. Brings up an
# EGL pbuffer, glClears to a known colour, reads the framebuffer back
# through the same swizzle the public GL swap path uses, and asserts
# the corners land as ARGB32 premul in native byte order (±1 for
# vendor rounding). Exits 77 (SKIP) if EGL isn't available on the
# host — some CI images strip llvmpipe.

mb_gl_path = executable(
  'mb-gl-path',
  'tests/gl-path.c',
  include_directories: include_directories('include', 'src'),
  dependencies: [glesv2_dep],
  link_with: libmoonbase,
  install: false,
)

test('gl-path', mb_gl_path, timeout: 10)

# ---------------------------------------------------------------------
# mb-entitlement-gate — API-boundary entitlement gate unit test
# ---------------------------------------------------------------------
#
# Exercises the MB_EPERM gate at the top of every privileged
# moonbase_keychain_* function. Forks a child per phase so
# MOONBASE_ENTITLEMENTS can change between runs without the
# pthread_once cache leaking state. Positive phase points
# DBUS_SESSION_BUS_ADDRESS at a nonexistent socket so libsecret
# can't hang waiting for a bus on headless runners.

mb_entitlement_gate = executable(
  'mb-entitlement-gate',
  'tests/entitlement-gate.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('entitlement-gate', mb_entitlement_gate, timeout: 15)

# ---------------------------------------------------------------------
# mb-consent-reader — consents.toml reader unit test
# ---------------------------------------------------------------------
#
# Seeds a hand-written consents.toml under a per-test tmpdir and
# drives mb_consent_query + mb_consent_gate_allows against every
# schema shape we expect in production: missing file, allow, deny,
# different section, section without decision, comments + blanks,
# unknown decision value, NULL inputs, and missing bundle id. Does
# not fork — the reader has no cache so rewriting the file between
# phases is enough.

mb_consent_reader = executable(
  'mb-consent-reader',
  'tests/consent-reader.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('consent-reader', mb_consent_reader, timeout: 10)

# ---------------------------------------------------------------------
# mb-consent-cli — moonbase-consent allow/deny CLI integration test
# ---------------------------------------------------------------------
#
# Forks the real moonbase-consent binary and drives it through the
# record-consent subcommand: allow, deny-overwrites-allow, env-only
# bundle id, missing bundle id, bad argc, and an unknown verb that
# falls through to the first-launch arm. Each phase points
# XDG_DATA_HOME at a fresh tmpdir; the test reads back through
# mb_consent_query to confirm the writer landed the right decision.
# Pins the CLI surface end-to-end — same-process unit tests can't
# catch argv parsing or exec/link regressions.

mb_consent_cli = executable(
  'mb-consent-cli',
  'tests/consent-cli.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('consent-cli', mb_consent_cli,
  args: [moonbase_consent],
  depends: [moonbase_consent],
  timeout: 15,
)

# ---------------------------------------------------------------------
# mb-consent-wire — client-side MB_IPC_CONSENT_REQUEST round-trip
# ---------------------------------------------------------------------
#
# Parent runs mb_server_t and replies to each CONSENT_REQUEST according
# to the capability string: phase 1 gets a canned GRANT, phase 2 a
# DENY, phase 3 an unrelated POWER_CHANGED followed by a GRANT (the
# helper must queue the unrelated frame), phase 4 a GRANT with a
# wrong req_id followed by the correct one (the helper must queue
# the mismatched reply). Each phase after the call drains
# mb_conn_pop_queued and asserts the expected queued frame is there.
# Pins the client half of the lazy-consent IPC leg — the responder
# side lands in the paired slice that also flips
# mb_consent_gate_allows from grant-by-default to deny-on-missing.

mb_consent_wire = executable(
  'mb-consent-wire',
  'tests/consent-wire.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('consent-wire', mb_consent_wire, timeout: 10)

# ---------------------------------------------------------------------
# mb-consent-responder — server-side CONSENT_REQUEST responder test
# ---------------------------------------------------------------------
#
# Drives mb_consent_responder end-to-end. Parent runs mb_server_t +
# the responder pointing at the real moonbase-consent binary; forks a
# child per phase that calls mb_ipc_consent_request, and asserts the
# reply (GRANT / DENY / ERROR) plus that consents.toml landed the
# right decision under the HELLO bundle-id. Also pins that the
# responder ignores any client-side attempt to spoof bundle-id.

mb_consent_responder = executable(
  'mb-consent-responder',
  'tests/consent-responder.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('consent-responder', mb_consent_responder,
  args: [moonbase_consent],
  depends: [moonbase_consent],
  timeout: 30,
)

# ---------------------------------------------------------------------
# mb-consent-gate-ipc — slice 6A end-to-end gate test
# ---------------------------------------------------------------------
#
# Drives mb_consent_gate_allows' deny-on-missing + IPC path. Parent
# runs mb_server_t + the responder pointing at moonbase-consent; forks
# a child per phase that calls mb_consent_gate_allows (not the raw
# mb_ipc_consent_request wire), and asserts the decision matches the
# AUTO mode and that consents.toml lands the right entry. Phase 3
# never calls moonbase_init so the gate's !handshaken deny-branch is
# pinned — no IPC, no write.

mb_consent_gate_ipc = executable(
  'mb-consent-gate-ipc',
  'tests/consent-gate-ipc.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('consent-gate-ipc', mb_consent_gate_ipc,
  args: [moonbase_consent],
  depends: [moonbase_consent],
  timeout: 30,
)

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
# CopyCatOS/Resources, relative-symlink escape via realpath, absolute
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
# examples/hello — reference MoonBase app + assembled .appdev bundle
# ---------------------------------------------------------------------
#
# Produces build/hello.appdev/Contents/{Info.appc, CopyCatOS/hello} plus
# a Contents/MacOS → CopyCatOS compat symlink and a sibling hello.app
# symlink → hello.appdev. Layout matches bundle-spec.md §1 and passes
# mb_bundle_load's validator. Not installed system-wide — the bundle
# is a build artifact the user runs from the build tree via
# `moonbase-launch build/hello.appdev` (or `build/hello.app`).
# (.appdev is the developer directory format; the shipping single-file
# .app becomes a real ELF-stub artifact once the packager lands.)
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

hello_appdev = custom_target('hello-appdev',
  output: ['hello.appdev', 'hello.app'],
  input: [files('examples/hello/Info.appc'), hello_exe],
  command: [
    find_program('sh'), '-c',
    'set -eu; '
    + 'rm -rf "$0"; rm -f "$3"; '
    + 'mkdir -p "$0/Contents/CopyCatOS" "$0/Contents/Resources"; '
    + 'cp "$1" "$0/Contents/Info.appc"; '
    + 'cp "$2" "$0/Contents/CopyCatOS/hello"; '
    + 'chmod 0755 "$0/Contents/CopyCatOS/hello"; '
    + 'ln -s CopyCatOS "$0/Contents/MacOS"; '
    + 'ln -s "$(basename "$0")" "$3"',
    '@OUTPUT0@', '@INPUT0@', '@INPUT1@', '@OUTPUT1@',
  ],
  build_by_default: true,
)

# ---------------------------------------------------------------------
# examples/gl-hello — minimal MOONBASE_RENDER_GL reference app
# ---------------------------------------------------------------------
#
# Proves the GL ABI end-to-end. Creates a 480×320-point GL window,
# clears to Aqua blue, draws one triangle via a trivial ES3 shader,
# and commits every redraw. Paired with a declared render-mode of
# "gl" in Info.appc so moonbase-launch binds /dev/dri into the
# sandbox. Not installed — run from the build tree:
#   moonbase-launch build/gl-hello.appdev

gl_hello_exe = executable(
  'gl-hello',
  'examples/gl-hello/main.c',
  include_directories: include_directories('include'),
  dependencies: [glesv2_dep],
  link_with: libmoonbase,
  install: false,
)

gl_hello_appdevev = custom_target('gl-hello-appdev',
  output: ['gl-hello.appdev', 'gl-hello.app'],
  input: [files('examples/gl-hello/Info.appc'), gl_hello_exe],
  command: [
    find_program('sh'), '-c',
    'set -eu; '
    + 'rm -rf "$0"; rm -f "$3"; '
    + 'mkdir -p "$0/Contents/CopyCatOS" "$0/Contents/Resources"; '
    + 'cp "$1" "$0/Contents/Info.appc"; '
    + 'cp "$2" "$0/Contents/CopyCatOS/gl-hello"; '
    + 'chmod 0755 "$0/Contents/CopyCatOS/gl-hello"; '
    + 'ln -s CopyCatOS "$0/Contents/MacOS"; '
    + 'ln -s "$(basename "$0")" "$3"',
    '@OUTPUT0@', '@INPUT0@', '@INPUT1@', '@OUTPUT1@',
  ],
  build_by_default: true,
)

# ---------------------------------------------------------------------
# examples/textedit — TextEdit.appdev v0.1
# ---------------------------------------------------------------------
#
# Smallest useful Cairo app on MoonBase: one window, one in-memory byte
# buffer, TEXT_INPUT + KEY_DOWN drive editing. Same bundle-assembly
# shape as hello.appdev. No menus, no file I/O, no selection yet —
# TextEdit grows into those as their framework slices land.

textedit_exe = executable(
  'textedit',
  'examples/textedit/main.c',
  include_directories: include_directories('include'),
  dependencies: [cairo_dep],
  link_with: libmoonbase,
  install: false,
)

textedit_appdev = custom_target('textedit-appdev',
  output: ['textedit.appdev', 'textedit.app'],
  input: [files('examples/textedit/Info.appc'), textedit_exe],
  command: [
    find_program('sh'), '-c',
    'set -eu; '
    + 'rm -rf "$0"; rm -f "$3"; '
    + 'mkdir -p "$0/Contents/CopyCatOS" "$0/Contents/Resources"; '
    + 'cp "$1" "$0/Contents/Info.appc"; '
    + 'cp "$2" "$0/Contents/CopyCatOS/textedit"; '
    + 'chmod 0755 "$0/Contents/CopyCatOS/textedit"; '
    + 'ln -s CopyCatOS "$0/Contents/MacOS"; '
    + 'ln -s "$(basename "$0")" "$3"',
    '@OUTPUT0@', '@INPUT0@', '@INPUT1@', '@OUTPUT1@',
  ],
  build_by_default: true,
)

# ---------------------------------------------------------------------
# examples/keychain-access — Keychain Access.appdev v0.1
# ---------------------------------------------------------------------
#
# Reference app 3 of 4 required before the libmoonbase.so.1 public-SDK
# freeze. Drives every public symbol in moonbase_keychain.h through a
# real bundle: list / store / fetch / delete / generate_password /
# secret_free. v0.1 uses auto-generated labels + 20-char passwords so
# the app doesn't need a text-entry form yet (that lands when the
# framework grows modal dialogs).
#
# system:"keychain" in Info.appc is enforced at the libmoonbase API
# boundary — every keychain call returns MB_EPERM without it. The
# runtime D-Bus socket is still reachable through the launcher's
# XDG_RUNTIME_DIR bind; a sandboxed xdg-dbus-proxy filter is the
# follow-up.

keychain_access_exe = executable(
  'keychain-access',
  'examples/keychain-access/main.c',
  include_directories: include_directories('include'),
  dependencies: [cairo_dep],
  link_with: libmoonbase,
  install: false,
)

keychain_access_appdev = custom_target('keychain-access-appdev',
  output: ['keychain-access.appdev', 'keychain-access.app'],
  input: [files('examples/keychain-access/Info.appc'), keychain_access_exe],
  command: [
    find_program('sh'), '-c',
    'set -eu; '
    + 'rm -rf "$0"; rm -f "$3"; '
    + 'mkdir -p "$0/Contents/CopyCatOS" "$0/Contents/Resources"; '
    + 'cp "$1" "$0/Contents/Info.appc"; '
    + 'cp "$2" "$0/Contents/CopyCatOS/keychain-access"; '
    + 'chmod 0755 "$0/Contents/CopyCatOS/keychain-access"; '
    + 'ln -s CopyCatOS "$0/Contents/MacOS"; '
    + 'ln -s "$(basename "$0")" "$3"',
    '@OUTPUT0@', '@INPUT0@', '@INPUT1@', '@OUTPUT1@',
  ],
  build_by_default: true,
)

# ---------------------------------------------------------------------
# examples/preview — Preview.appdev v0.1
# ---------------------------------------------------------------------
#
# Reference app 4 of 4 before the libmoonbase.so.1 public-SDK freeze.
# Pressure-tests the GL render path, MB_EV_DRAG_* delivery, and the
# moonbase_prefs_* store in one bundle: open an image (argv or
# drag-drop), upload it as a GL texture, draw with letterboxing,
# remember window size and last-opened path between runs. PNG + JPEG
# only for v0.1 — every other format waits on its own slice.

preview_exe = executable(
  'preview',
  'examples/preview/main.c',
  include_directories: include_directories('include'),
  dependencies: [glesv2_dep, libpng_dep, libjpeg_dep],
  link_with: libmoonbase,
  install: false,
)

preview_appdev = custom_target('preview-appdev',
  output: ['preview.appdev', 'preview.app'],
  input: [files('examples/preview/Info.appc'), preview_exe],
  command: [
    find_program('sh'), '-c',
    'set -eu; '
    + 'rm -rf "$0"; rm -f "$3"; '
    + 'mkdir -p "$0/Contents/CopyCatOS" "$0/Contents/Resources"; '
    + 'cp "$1" "$0/Contents/Info.appc"; '
    + 'cp "$2" "$0/Contents/CopyCatOS/preview"; '
    + 'chmod 0755 "$0/Contents/CopyCatOS/preview"; '
    + 'ln -s CopyCatOS "$0/Contents/MacOS"; '
    + 'ln -s "$(basename "$0")" "$3"',
    '@OUTPUT0@', '@INPUT0@', '@INPUT1@', '@OUTPUT1@',
  ],
  build_by_default: true,
)

# ---------------------------------------------------------------------
# examples/activity-monitor — Activity Monitor.appdev v0.1
# ---------------------------------------------------------------------
#
# Fourth and final reference app before the libmoonbase.so.1 public-SDK
# freeze. Exercises long-lived periodic sampling (wait_event with a
# 1s timeout), Cairo-drawn list rows (no widget library required), and
# a declared system:process-list entitlement in Info.appc. Pure libc —
# reads /proc directly — so no extra dependencies beyond moonbase + cairo.

activity_monitor_exe = executable(
  'activity-monitor',
  'examples/activity-monitor/main.c',
  include_directories: include_directories('include'),
  dependencies: [cairo_dep],
  link_with: libmoonbase,
  install: false,
)

activity_monitor_appdev = custom_target('activity-monitor-appdev',
  output: ['activity-monitor.appdev', 'activity-monitor.app'],
  input: [files('examples/activity-monitor/Info.appc'), activity_monitor_exe],
  command: [
    find_program('sh'), '-c',
    'set -eu; '
    + 'rm -rf "$0"; rm -f "$3"; '
    + 'mkdir -p "$0/Contents/CopyCatOS" "$0/Contents/Resources"; '
    + 'cp "$1" "$0/Contents/Info.appc"; '
    + 'cp "$2" "$0/Contents/CopyCatOS/activity-monitor"; '
    + 'chmod 0755 "$0/Contents/CopyCatOS/activity-monitor"; '
    + 'ln -s CopyCatOS "$0/Contents/MacOS"; '
    + 'ln -s "$(basename "$0")" "$3"',
    '@OUTPUT0@', '@INPUT0@', '@INPUT1@', '@OUTPUT1@',
  ],
  build_by_default: true,
)
