# CopyCatOS — by Kyle Blizzard at Blizzard.show

# moonbase — CopyCatOS application framework library
#
# Builds the real ELF shared library inside MoonBase.framework/ as the
# bare file 'MoonBase' (no 'lib' prefix, no '.so.1' suffix). The ELF
# carries SONAME=MoonBase, so every app links with '-l:MoonBase' and
# ld.so at runtime looks up a file literally named MoonBase under the
# framework path registered with /etc/ld.so.conf.d/moonbase.conf.
#
# Targets:
#   ${libdir}/MoonBase.framework/Versions/A/MoonBase   — the ELF
#   ${libdir}/MoonBase.framework/Versions/A/Headers/   — public API
#   ${libdir}/MoonBase.framework/Versions/Current → A
#   ${libdir}/MoonBase.framework/{MoonBase,Headers} → Versions/Current/…
#   moonbase-launch      — bwrap-based bundle launcher
#   mb-*                 — unit + integration tests
#
# Generated:
#   moonbase.pc          — pkg-config file (Libs: -l:MoonBase)
#   moonbase.conf        — ld.so.conf.d entry for framework path
#
# Build:
#   meson setup build
#   meson compile -C build
#
# Install:
#   sudo meson install -C build
#   (install script runs ldconfig when DESTDIR is unset)

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
# Phase A froze this set. Every app linking MoonBase includes
# through <moonbase.h> + one or more companions. All companion
# headers ship even in Phase B so downstream compile-gates against
# their MOONBASE_<DOMAIN>_API_VERSION macros resolve today.

# Shared basename list drives both the install_headers set and the
# MoonBase.framework/Versions/A/Headers symlink loop further down — keeping
# one list means the framework bundle can never drift from the flat
# ${includedir} install.
public_header_names = [
  'moonbase.h',
  'moonbase_keychain.h',
  'moonbase_a11y.h',
  'moonbase_notifications.h',
  'moonbase_power.h',
  'moonbase_controller.h',
  'moonbase_clipboard.h',
  'moonbase_display.h',
  'moonbase_services.h',
  'moonbase_share.h',
  'moonbase_print.h',
  'moonbase_quicklook.h',
  'moonbase_spotlight.h',
]

public_headers = []
foreach hdr : public_header_names
  public_headers += files('include' / hdr)
endforeach

ipc_headers = files(
  'include/moonbase/ipc/kinds.h',
)

install_headers(public_headers)
install_headers(ipc_headers, subdir: 'moonbase/ipc')

# ---------------------------------------------------------------------
# MoonBase — framework binary
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
  'src/bundle/appimg.c',
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

# pangocairo is the text shaper behind menubar_render. moonbase-launcher
# links the shared menubar_render translation unit for foreign-distro
# chrome — the framework library itself never depends on Pango.
pangocairo_dep = dependency('pangocairo')

# The shared library builds with:
#   - name 'MoonBase'        : output basename before prefix/suffix
#   - name_prefix ''         : drop the Linux 'lib' prefix
#   - no version/soversion   : meson's default suffix '.so' is accepted,
#                              but the custom_target below copies it to
#                              a bare 'MoonBase' (no extension) for the
#                              framework install path
#   - link_args -Wl,-soname  : burns DT_SONAME=MoonBase into the ELF so
#                              '-l:MoonBase' consumers end up with
#                              DT_NEEDED=MoonBase at link time, which
#                              ld.so resolves via ld.so.conf.d/moonbase.conf
#   - install: false         : handled by moonbase_framework_binary below
libmoonbase = shared_library(
  'MoonBase',
  moonbase_lib_sources,
  name_prefix: '',
  include_directories: include_directories('include', 'src'),
  dependencies: [threads_dep, cairo_dep, libsecret_dep, egl_dep, glesv2_dep],
  link_args: ['-Wl,-soname,MoonBase'],
  install: false,
)

# ---------------------------------------------------------------------
# mb_host — compositor-host shared static library
# ---------------------------------------------------------------------
#
# Compositor-agnostic helpers shared by moonrock (the full Snow Leopard
# compositor on a CopyCatOS session) and moonrock-lite (the foreign-distro
# sidecar used when a CopyCatOS user double-clicks a `.app` on Ubuntu /
# Fedora / KDE). Both compositors speak the same MoonBase IPC, paint the
# same Aqua chrome, and run the same hit-test math, so the parsers,
# encoders, Cairo painter, and util helpers all live here behind one
# header set.
#
# Built once inside moonbase under the project's strict
# warning_level=3 + werror flags so the host TUs compile clean against
# the same bar as libmoonbase. moonrock has a slightly looser flag mix
# (warning_level=2) and therefore builds the same source files into its
# own static library with explicit strict c_args to keep this file's
# guarantee from being silently weakened on the moonrock side.
#
# install: false — slice 19.H.1 lives in-tree; outside builds reach this
# code via the source files, not via a system static library.

mb_host_sources = files(
  'src/host/host_chrome.c',
  'src/host/host_hittest.c',
  'src/host/host_protocol.c',
  'src/host/host_util.c',
)

mb_host_lib = static_library(
  'mb_host',
  mb_host_sources,
  include_directories: include_directories('include', 'src'),
  dependencies: [cairo_dep, pangocairo_dep],
  install: false,
)

mb_host_dep = declare_dependency(
  link_with: mb_host_lib,
  include_directories: include_directories('include', 'src/host'),
  dependencies: [cairo_dep, pangocairo_dep],
)

# pkg-config so third-party code can `pkg-config --cflags --libs moonbase`.
# The Libs: line points at MoonBase.framework/Versions/Current and uses
# GNU ld's '-l:<exact-filename>' form so the linker picks up the bare
# 'MoonBase' file (not 'libMoonBase.so'). subdirs['.'] adds -I${includedir}
# so top-level headers resolve as <moonbase.h>.
pkg = import('pkgconfig')
pkg.generate(
  name: 'moonbase',
  description: 'CopyCatOS application framework (MoonBase.framework)',
  version: meson.project_version(),
  subdirs: ['.'],
  libraries: [
    '-L${libdir}/MoonBase.framework/Versions/Current',
    '-l:MoonBase',
  ],
)

# ---------------------------------------------------------------------
# MoonBase.framework — Snow Leopard-style bundle install-layout
# ---------------------------------------------------------------------
#
# The framework is the *only* install path for the shared library — there
# is no legacy ${libdir}/libmoonbase.so* anywhere on disk. The real ELF
# lives inside the bundle as:
#
#     ${libdir}/MoonBase.framework/Versions/A/MoonBase
#
# Headers are installed twice on purpose: a flat ${includedir}/moonbase.h
# tree (for distros that prefer pkg-config's default search paths) plus
# per-header symlinks inside MoonBase.framework/Versions/A/Headers/ that
# cross back into ${includedir}. Snow Leopard-style consumers use the
# framework's Headers/ path; a simple `-I${includedir}` also works.
#
# Layout under ${libdir}/MoonBase.framework/:
#     MoonBase             → Versions/Current/MoonBase
#     Headers              → Versions/Current/Headers
#     Versions/Current     → A
#     Versions/A/MoonBase  : the ELF shared library (SONAME=MoonBase)
#     Versions/A/Headers/<hdr>.h  → ${includedir}/<hdr>.h (per public header)
#     Versions/A/Headers/moonbase/ipc/kinds.h → ${includedir}/moonbase/ipc/kinds.h
#
# In-framework links stay relative so a whole-framework relocation stays
# valid; crossings out to ${includedir} are absolute because libdir and
# includedir can have different shapes (lib vs lib64, /usr vs /usr/local)
# and there is no reliable relative path between them at configure time.

mb_framework_dir       = get_option('libdir') / 'MoonBase.framework'
mb_framework_versions  = mb_framework_dir / 'Versions'
mb_framework_version_a = mb_framework_versions / 'A'
mb_framework_headers_a = mb_framework_version_a / 'Headers'

mb_absolute_includedir = get_option('prefix') / get_option('includedir')

# Install order matters: meson verifies each symlink's target exists at
# install time. Populate Versions/A/ (binary + per-header links) first,
# then Versions/Current, then the top-level convenience symlinks whose
# targets dereference through Current back into A/.

# Versions/A/MoonBase — the real ELF, copied out of the build tree under
# its bare framework name. Meson's shared_library() emits MoonBase.so
# (no lib-prefix thanks to name_prefix: '', '.so' suffix because meson
# 1.11 rejects an empty suffix); this custom_target strips the .so so
# the on-disk filename matches DT_SONAME=MoonBase exactly.
moonbase_framework_binary = custom_target(
  'MoonBase.bin',
  input: libmoonbase,
  output: 'MoonBase',
  command: [find_program('cp'), '@INPUT@', '@OUTPUT@'],
  install: true,
  install_dir: mb_framework_version_a,
  install_mode: 'rwxr-xr-x',
  build_by_default: true,
)

# Versions/A/libMoonBase.so → MoonBase
#
# Not a compatibility symlink. glibc's ldconfig scans directories listed
# in ld.so.conf.d/*.conf and filters entries by the 'lib*.so*' filename
# pattern *before* opening each candidate to read DT_SONAME. A file
# literally named 'MoonBase' is invisible to ldconfig without a
# matching-pattern name somewhere pointing at it — meaning ld.so.cache
# never learns MoonBase exists and every consumer fails to resolve
# DT_NEEDED=MoonBase at exec time.
#
# With this symlink, ldconfig:
#   1. matches libMoonBase.so against its lib*.so* pattern
#   2. follows the link to MoonBase and reads DT_SONAME=MoonBase
#   3. caches entry: 'MoonBase' → Versions/A/libMoonBase.so
#
# At app launch ld.so hits the cache under the SONAME key 'MoonBase'
# and loads the file (via the symlink, transparently). Nothing in the
# build system ever links to libMoonBase by name — consumers use
# -l:MoonBase, which produces DT_NEEDED=MoonBase, resolved through the
# SONAME key only. The filename is a pure ldconfig-visibility shim.
install_symlink(
  'libMoonBase.so',
  pointing_to: 'MoonBase',
  install_dir: mb_framework_version_a,
)

# Per-header symlinks inside Versions/A/Headers/
foreach hdr : public_header_names
  install_symlink(
    hdr,
    pointing_to: mb_absolute_includedir / hdr,
    install_dir: mb_framework_headers_a,
  )
endforeach

# Nested IPC header: MoonBase.framework/Versions/A/Headers/moonbase/ipc/kinds.h
install_symlink(
  'kinds.h',
  pointing_to: mb_absolute_includedir / 'moonbase' / 'ipc' / 'kinds.h',
  install_dir: mb_framework_headers_a / 'moonbase' / 'ipc',
)

# Versions/Current → A
install_symlink(
  'Current',
  pointing_to: 'A',
  install_dir: mb_framework_versions,
)

# Top-level MoonBase → Versions/Current/MoonBase
install_symlink(
  'MoonBase',
  pointing_to: 'Versions/Current/MoonBase',
  install_dir: mb_framework_dir,
)

# Top-level Headers → Versions/Current/Headers
install_symlink(
  'Headers',
  pointing_to: 'Versions/Current/Headers',
  install_dir: mb_framework_dir,
)

# ---------------------------------------------------------------------
# ld.so registration — /etc/ld.so.conf.d/moonbase.conf
# ---------------------------------------------------------------------
#
# ld.so resolves DT_NEEDED=MoonBase at app launch time. Without this
# file the loader would only search the default system library paths
# (/lib, /lib64, /usr/lib, /usr/lib64) and miss the framework. The
# generated conf contains a single absolute directory pointing at
# MoonBase.framework/Versions/Current; ldconfig follows the symlink
# chain into A/ and caches MoonBase from there.

moonbase_ld_conf_data = configuration_data()
moonbase_ld_conf_data.set(
  'FRAMEWORK_VERSIONS_CURRENT',
  get_option('prefix') / mb_framework_versions / 'Current',
)
moonbase_ld_conf = configure_file(
  input: files('packaging/moonbase.conf.in'),
  output: 'moonbase.conf',
  configuration: moonbase_ld_conf_data,
)
install_data(
  moonbase_ld_conf,
  install_dir: '/etc/ld.so.conf.d',
)

# Refresh ld.so.cache after install so DT_NEEDED=MoonBase resolves on the
# very next exec. Skipped when DESTDIR is set — staged installs (rpm,
# deb, pacman, nobara-user's --destdir) run ldconfig from their own
# package post-install hooks against the live rootfs, not the staging
# tree. `|| true` keeps `sudo-less` installs from failing loudly; the
# user just needs to `sudo ldconfig` themselves.
meson.add_install_script(
  'sh', '-c',
  'if [ -z "$DESTDIR" ] && command -v ldconfig >/dev/null 2>&1; then '
  + 'ldconfig || true; '
  + 'fi',
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
  install_rpath: get_option('prefix') / mb_framework_versions / 'Current',
)

# ---------------------------------------------------------------------
# moonbase-launcher — portable .app entry point
# ---------------------------------------------------------------------
#
# The user-facing launcher that gets registered as the .desktop Exec=
# target on every host. Decides at startup whether we're inside a full
# CopyCatOS session (XDG_SESSION_DESKTOP=CopyCatOS — fast pass-through to
# moonbase-launch) or on a foreign distro (KDE / GNOME / XFCE — forks
# moonrock-lite for chrome painting and execvp's into moonbase-launch).
#
# Pre-19.H.1.a this binary also painted Aqua chrome inline via Cairo
# and the menubar Legacy-Mode helpers. That moved out to the standalone
# moonrock-lite binary (../../moonrock-lite/) so the launcher process
# no longer pulls cairo, X11, xrandr, gio, libpng, or the four menubar
# TUs. It's now strictly a session detector + XDG registrar + execvp
# delegator — pure libc plus libmoonbase for the bundle parser.
moonbase_launcher = executable(
  'moonbase-launcher',
  'runtime/moonbase-launcher.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: true,
  install_rpath: get_option('prefix') / mb_framework_versions / 'Current',
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
  install_rpath: get_option('prefix') / mb_framework_versions / 'Current',
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
  install_rpath: get_option('prefix') / mb_framework_versions / 'Current',
)

# ---------------------------------------------------------------------
# moonbase-app-stub — static ELF embedded at the head of every .app
# ---------------------------------------------------------------------
#
# The stub is a freestanding nostdlib -static binary (see stub/stub.c)
# whose only job is to exec `moonbase-launch` against /proc/self/exe
# when a user runs a shipping single-file .app. It has no runtime
# dependencies — no libc, no DT_NEEDED, no interpreter — so a .app
# built today keeps working on any CopyCatOS host that installs the
# moonbase-launch helper, even across glibc version skew.
#
# Build args:
#   -nostdlib / -static       : no libc, no startup files, fully static
#   -fno-stack-protector      : no __stack_chk_fail symbol to resolve
#   -fno-pie / -no-pie        : fixed load address keeps the ELF tiny
#                               and the entry point deterministic
#   -fno-builtin              : stop GCC emitting implicit libc calls
#                               (memcpy/memset) we'd then have to provide
#   -ffreestanding            : no libc assumptions in codegen
#   -Os -fno-asynchronous-unwind-tables -fno-unwind-tables :
#                               strip CFI + exception tables we don't
#                               use — saves ~1 KiB off the ELF
#   -Wl,--build-id=none       : no .note.gnu.build-id section
#   -Wl,-z,noseparate-code    : pack .text + .rodata in the same segment
#   -Wl,-nostdlib / -static   : match the compile side on the link step
#
# Installed to ${datadir}/moonbase/moonbase-app-stub; moonbase-pack
# reads it via the MOONBASE_STUB_PATH compile-time default (overridable
# at runtime through --stub or $MOONBASE_APP_STUB).
#
# Override-set on the subproject so the project-wide `-D_GNU_SOURCE`
# doesn't bleed into a nostdlib build and so we can opt out of any
# project cflags that assume libc presence.

moonbase_app_stub = executable(
  'moonbase-app-stub',
  'stub/stub.c',
  c_args: [
    '-nostdlib',
    '-ffreestanding',
    '-fno-stack-protector',
    '-fno-pie',
    '-fno-builtin',
    '-fno-asynchronous-unwind-tables',
    '-fno-unwind-tables',
    '-Os',
    '-U_FORTIFY_SOURCE',
  ],
  link_args: [
    '-nostdlib',
    '-static',
    '-no-pie',
    '-Wl,--build-id=none',
  ],
  override_options: ['b_lundef=true', 'b_pie=false'],
  install: true,
  install_dir: get_option('datadir') / 'moonbase',
  install_mode: 'rwxr-xr-x',
)

# ---------------------------------------------------------------------
# moonbase-pack — .appdev → .app packager
# ---------------------------------------------------------------------
#
# Links libmoonbase (mb_bundle_load validates §8 before packing; the
# writer side of appimg.c emits the trailer) and shells out to
# mksquashfs at runtime. Install to bindir — it's the user-facing
# tool developers call from `meson install` recipes or by hand.

mksquashfs_prog = find_program('mksquashfs', required: false)
if not mksquashfs_prog.found()
  # Surface the missing dependency at configure time on a dev host so
  # the error story is "install squashfs-tools" rather than a cryptic
  # mksquashfs failure deep inside moonbase-pack at runtime. Only a
  # warning — cross-building on macOS is expected to hit this path.
  warning('mksquashfs not found; moonbase-pack will fail at runtime. '
        + 'Install squashfs-tools (dnf install squashfs-tools).')
endif

moonbase_pack = executable(
  'moonbase-pack',
  'tools/moonbase-pack.c',
  include_directories: include_directories('include', 'src'),
  c_args: [
    '-DMOONBASE_STUB_PATH="@0@"'.format(
      get_option('prefix') / get_option('datadir') / 'moonbase' / 'moonbase-app-stub',
    ),
  ],
  link_with: libmoonbase,
  install: true,
  install_rpath: get_option('prefix') / mb_framework_versions / 'Current',
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
# mb-window-commit-burst — regression for MB_EPROTO on 2nd commit
# ---------------------------------------------------------------------
#
# Fires two WINDOW_COMMITs back-to-back from the client. The old server
# tagged SCM_RIGHTS batches with pre-append rx_received, which — when
# frame1's body-skb coalesced with frame2's header+scm-skb on the same
# recvmsg — pointed into frame1's tail instead of frame2's start, and
# drain_rx either stole frame2's fds or returned MB_EPROTO. Fix tags
# at post-append rx_received (== end of scm-skb per AF_UNIX kernel
# invariant) and matches frames by byte-span range.

mb_window_commit_burst = executable(
  'mb-window-commit-burst',
  'tests/window-commit-burst.c',
  include_directories: include_directories('include', 'src'),
  dependencies: [cairo_dep],
  link_with: libmoonbase,
  install: false,
)

test('window-commit-burst', mb_window_commit_burst, timeout: 10)

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
# mb-appimg — single-file .app trailer format unit test
# ---------------------------------------------------------------------
#
# Builds synthetic trailers in tmpfiles and drives mb_appimg_read_trailer
# through every branch: happy-path round-trip, short file, wrong
# magic_end / magic_start, unsupported version, oversize trailer,
# squashfs-span mismatch, empty / oversize / NUL-embedded bundle-id,
# plus mb_appimg_is_single_file edge cases (ENOENT, directory path).
# Pins the shared trailer format before the packer starts writing it.

mb_appimg = executable(
  'mb-appimg',
  'tests/appimg.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('appimg', mb_appimg, timeout: 10)

# ---------------------------------------------------------------------
# mb-bundle-load — Phase D slice 2 on-disk loader test
# ---------------------------------------------------------------------
#
# Drives mb_bundle_load against real tmpdir .app trees. Covers the
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
# Builds a real fixture .app in a tmpdir, forks moonbase-launch with
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
# mb-launch-appimg — Slice 17-B single-file .app end-to-end test
# ---------------------------------------------------------------------
#
# Builds a .appdev tree, packs Contents/ into a squashfs image, prepends
# a dummy 64-byte ELF stub and appends the trailer — producing a real
# single-file .app — then launches it through moonbase-launch. Asserts
# the embedded script ran inside the bwrap sandbox on a squashfuse mount,
# that MOONBASE_BUNDLE_ID was plumbed through, that the app's exit code
# propagates, and — critically — that after the launcher exits the
# $XDG_RUNTIME_DIR/moonbase/mounts/ directory is empty, pinning the
# atexit unmount + rmdir lifecycle. Skips (exit 77) if bwrap, mksquashfs,
# squashfuse, or fusermount is missing on the host.

mb_launch_appimg = executable(
  'mb-launch-appimg',
  'tests/launch-appimg.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('launch-appimg', mb_launch_appimg,
  args: [moonbase_launch, meson.project_source_root() / 'sandbox'],
  depends: [moonbase_launch],
  timeout: 30,
)

# ---------------------------------------------------------------------
# mb-pack-smoke — Slice 17-A moonbase-pack integration test
# ---------------------------------------------------------------------
#
# Builds a minimal `.appdev` fixture, runs the real moonbase-pack
# binary with a dummy 4-byte placeholder stub, and verifies the output
# `.app`: stub bytes survived, squashfs image at the advertised offset
# starts with the `hsqs` magic, trailer parses back to the same
# bundle-id, final file is chmod 0755. Skips (exit 77) if mksquashfs
# isn't on the host.

mb_pack_smoke = executable(
  'mb-pack-smoke',
  'tests/pack-smoke.c',
  include_directories: include_directories('include', 'src'),
  link_with: libmoonbase,
  install: false,
)

test('pack-smoke', mb_pack_smoke,
  args: [moonbase_pack],
  depends: [moonbase_pack],
  timeout: 30,
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
# examples/* — reference MoonBase apps, each built as .appdev + .app
# ---------------------------------------------------------------------
#
# Per example we produce two build artifacts:
#
#   <name>.appdev   — classic directory form (bundle-spec.md §1).
#                     Contents/{Info.appc, CopyCatOS/<exe>, Resources/}.
#                     Runs directly via `moonbase-launch <name>.appdev`
#                     — the daily dev loop.
#
#   <name>.app      — single-file shipping bundle (ELF stub + squashfs
#                     + trailer). Produced by moonbase-pack on hosts
#                     where mksquashfs is available (Legion: yes; macOS
#                     dev: no — falls back to a symlink so iteration
#                     stays fast on machines that can't pack).
#
# Nothing is installed system-wide — these are build-tree artifacts
# used for manual smoke tests and the integration tests under
# tests/ that drive moonbase-launch against the real bundles.

example_specs = [
  {
    'name': 'hello',
    'info': files('examples/hello/Info.appc'),
    'src':  files('examples/hello/main.c'),
    'deps': [cairo_dep],
  },
  {
    'name': 'gl-hello',
    'info': files('examples/gl-hello/Info.appc'),
    'src':  files('examples/gl-hello/main.c'),
    'deps': [glesv2_dep],
  },
  {
    'name': 'textedit',
    'info': files('examples/textedit/Info.appc'),
    'src':  files('examples/textedit/main.c'),
    'deps': [cairo_dep],
  },
  {
    'name': 'keychain-access',
    'info': files('examples/keychain-access/Info.appc'),
    'src':  files('examples/keychain-access/main.c'),
    'deps': [cairo_dep],
  },
  {
    'name': 'preview',
    'info': files('examples/preview/Info.appc'),
    'src':  files('examples/preview/main.c'),
    'deps': [glesv2_dep, libpng_dep, libjpeg_dep],
  },
  {
    'name': 'activity-monitor',
    'info': files('examples/activity-monitor/Info.appc'),
    'src':  files('examples/activity-monitor/main.c'),
    'deps': [cairo_dep],
  },
]

foreach spec : example_specs
  name = spec['name']

  example_exe = executable(
    name,
    spec['src'],
    include_directories: include_directories('include'),
    dependencies: spec['deps'],
    link_with: libmoonbase,
    install: false,
  )

  # Step 1: assemble the .appdev directory. Shape matches bundle-spec.md
  # §1: Contents/Info.appc + Contents/CopyCatOS/<exe> + an empty
  # Resources/ placeholder.
  #
  # Optional AppIcon.png under examples/<name>/Resources/ is copied when
  # present. Passed as a plain path string (not an `input`) so a missing
  # file is a no-op instead of a meson configure error — dev placeholders
  # don't have to exist. The trade-off is that changing the PNG doesn't
  # trigger a rebuild; re-run `meson compile --clean` or touch Info.appc
  # when swapping a placeholder icon.
  example_icon = meson.current_source_dir() / 'examples' / name / 'Resources' / 'AppIcon.png'

  # Drop freshly-built MoonBase next to the example exe so the bundle's
  # $ORIGIN/ rpath resolves to *this* build's framework, not whatever
  # stale `MoonBase` happens to live in /usr/local/lib64/MoonBase.framework/.
  # Without this, every iterative MoonBase change would be invisible to a
  # bundle launched out of the build tree until the dev re-installed
  # system-wide. moonbase_framework_binary already produces a bare
  # `MoonBase` filename (no .so suffix) — exactly what DT_NEEDED=MoonBase
  # resolves against — so a plain copy is all we need.
  appdev_target = custom_target(name + '-appdev',
    output: name + '.appdev',
    input: [spec['info'], example_exe, moonbase_framework_binary],
    command: [
      find_program('sh'), '-c',
      'set -eu; '
      + 'rm -rf "$0"; '
      + 'mkdir -p "$0/Contents/CopyCatOS" "$0/Contents/Resources"; '
      + 'cp "$1" "$0/Contents/Info.appc"; '
      + 'cp "$2" "$0/Contents/CopyCatOS/$3"; '
      + 'chmod 0755 "$0/Contents/CopyCatOS/$3"; '
      + 'cp "$5" "$0/Contents/CopyCatOS/MoonBase"; '
      + 'chmod 0755 "$0/Contents/CopyCatOS/MoonBase"; '
      + 'if [ -f "$4" ]; then cp "$4" "$0/Contents/Resources/AppIcon.png"; fi',
      '@OUTPUT0@', '@INPUT0@', '@INPUT1@', name, example_icon, '@INPUT2@',
    ],
    build_by_default: true,
  )

  # Step 2: pack the .appdev into a single-file .app. Two paths —
  # real pack via moonbase-pack when mksquashfs is available, or a
  # simple symlink otherwise. Either way the top-level `<name>.app`
  # artifact exists so downstream tests and dev scripts don't have to
  # branch on host capability.
  if mksquashfs_prog.found()
    custom_target(name + '-app',
      output: name + '.app',
      input: [appdev_target, moonbase_pack, moonbase_app_stub],
      command: [
        find_program('sh'), '-c',
        # moonbase-pack writes atomically via mkstemps+rename. `rm -f`
        # the dst first so a previous build's stale symlink is gone
        # before the packer stats the target path. The `./` prefix on
        # $1 stops the shell from doing PATH lookup on a plain name —
        # meson hands us bare basenames here (cwd is the build dir).
        'set -eu; rm -f "$3"; exec "./$1" --stub "$2" "$0" "$3"',
        '@INPUT0@', '@INPUT1@', '@INPUT2@', '@OUTPUT0@',
      ],
      build_by_default: true,
    )
  else
    custom_target(name + '-app',
      output: name + '.app',
      input: appdev_target,
      command: [
        find_program('sh'), '-c',
        'set -eu; rm -f "$1"; ln -s "$(basename "$0")" "$1"',
        '@INPUT0@', '@OUTPUT0@',
      ],
      build_by_default: true,
    )
  endif
endforeach
