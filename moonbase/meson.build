# CopyCatOS — by Kyle Blizzard at Blizzard.show

# moonbase — CopyCatOS application framework
#
# Native C host that loads apps written in other languages and embeds them
# inside a CopyCatOS window. Scope for v0.1 scaffold:
#
#   - Launch the host process, discover .appc bundles under the user's
#     Applications directory, open the bundle specified on the command line
#     (if any). That is all main.c does today.
#
# Language hosts get added in follow-up milestones:
#   - Swift     (via libSwiftCore runtime shim)
#   - Python    (via embedded CPython)
#   - Web (HTML/JS/CSS, via an embedded browser — WebKitGTK or CEF)
#
# Long-term vision: a local LLM runs on-device and generates .appc bundles
# for the user in response to natural-language prompts. moonbase is the
# execution environment those generated apps live in.
#
# Build:
#   meson setup build && meson compile -C build
#
# Install:
#   meson install -C build
#   (installs /usr/local/bin/moonbase)

project('moonbase', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2'],
)

# _GNU_SOURCE for realpath(3) and similar POSIX extensions used in
# bundle resolution.
add_project_arguments('-D_GNU_SOURCE', language: 'c')

# At scaffold stage we only need libc. As language hosts get added:
#   - Swift host: will need libSwiftCore, libobjc
#   - Python host: will need python3-embed pkg-config module
#   - Web host: will need webkit2gtk-4.1 or cef

sources = files(
  'src/main.c',
  'src/bundle.c',
)

executable('moonbase', sources,
  install: true,
  install_dir: get_option('bindir'),
)
