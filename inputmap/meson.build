# CopyCatOS — by Kyle Blizzard at Blizzard.show

# inputmap — CopyCatOS Button Mapper
#
# Build:
#   meson setup build && meson compile -C build
#
# Install:
#   sudo meson install -C build

project('inputmap', 'c',
    version : '0.1.0',
    default_options : ['c_std=gnu17', 'warning_level=2']
)

# Dependencies — scanner needs libudev and linux headers,
# UI will need cairo/pango/x11 later
udev_dep  = dependency('libudev')
cairo_dep = dependency('cairo')
pango_dep = dependency('pangocairo')
x11_dep   = dependency('x11')

sources = files(
    'src/main.c',
    'src/scanner.c',
    'src/config_editor.c',
)

executable('inputmap', sources,
    dependencies : [udev_dep, cairo_dep, pango_dep, x11_dep],
    install : true,
    install_dir : get_option('prefix') / 'bin'
)
