# CopyCatOS — by Kyle Blizzard at Blizzard.show

#
# meson.build — Build configuration for inputd
#
# inputd is the CopyCatOS input daemon. It reads Legion Go gamepad
# events and translates them into mouse/keyboard/shell actions.
#
# Build:
#   meson setup build
#   meson compile -C build
#
# Install:
#   meson install -C build
#   (installs binary to /usr/local/sbin/, systemd service, and udev rules)
#

project(
    'inputd',
    'c',
    version : '0.1.0',
    default_options : [
        'c_std=c11',
        'warning_level=2',
    ],
)

# _GNU_SOURCE enables Linux-specific APIs we need:
#   - timerfd_create() for the mouse emulator's 120Hz tick
#   - epoll_create1() for the main event loop
#   - pipe2() with O_NONBLOCK for the self-pipe trick
#   - strdup(), strsep(), and other POSIX extensions
add_project_arguments('-D_GNU_SOURCE', language : 'c')

# --------------------------------------------------------------------------
# Dependencies
# --------------------------------------------------------------------------
# libudev: Device enumeration and hotplug monitoring (/dev/input/event*)
# libm:    Math library for pow() in the mouse emulator's response curve
# --------------------------------------------------------------------------
dep_udev = dependency('libudev', required : true)
dep_math = meson.get_compiler('c').find_library('m', required : true)

# --------------------------------------------------------------------------
# Source files
# --------------------------------------------------------------------------
# All source files live in src/. Headers are found automatically because
# they're in the same directory as the .c files.
# --------------------------------------------------------------------------
sources = files(
    'src/main.c',
    'src/inputd.c',
    'src/device.c',
    'src/uinput.c',
    'src/mapper.c',
    'src/mouse.c',
    'src/power.c',
    'src/config.c',
    'src/ipc.c',
    'src/hid.c',
)

# --------------------------------------------------------------------------
# Executable
# --------------------------------------------------------------------------
# Installed to sbindir (/usr/local/sbin by default) because inputd
# runs as a system daemon with root privileges.
# --------------------------------------------------------------------------
executable(
    'inputd',
    sources,
    dependencies : [dep_udev, dep_math],
    install : true,
    install_dir : get_option('sbindir'),
)

# --------------------------------------------------------------------------
# Installed data files
# --------------------------------------------------------------------------
# These are optional — they make it easy to enable inputd as a systemd
# service and set up udev permissions. Install with `meson install -C build`.
# --------------------------------------------------------------------------

# Systemd service file — enables `systemctl enable/start inputd`
install_data(
    'data/inputd.service',
    install_dir : '/usr/lib/systemd/system',
)

# Udev rules — gives the "input" group access to /dev/uinput so the
# daemon can create virtual devices without being fully root
install_data(
    'data/99-inputd.rules',
    install_dir : '/usr/lib/udev/rules.d',
)
