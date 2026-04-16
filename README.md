# CopyCatOS

A custom Linux desktop environment that recreates Mac OS X Snow Leopard.

Not a theme. Not a skin. A complete desktop environment built from scratch in C — custom window manager, custom compositor, custom dock, custom menu bar, custom desktop, custom search overlay. Every pixel measured against a real Snow Leopard machine.

## What's Here

- **cc-wm** — X11 reparenting window manager with MoonRock Compositor (OpenGL). Handles window framing, decoration painting, shadows, compositing, keyboard shortcuts, strut reservation.
- **cc-dock** — The dock. Glass shelf, parabolic magnification, two-phase bounce animation, icon reflections, running indicators, drag-and-drop with poof removal, folder stacks with grid popup, config persistence, enhanced context menus with submenus.
- **cc-menubar** — 22px translucent global menu bar. Apple logo, bold app name, per-app menu sets, system tray (clock, battery, volume), Spotlight search icon. Wallpaper bleeds through the gradient.
- **cc-desktop** — Wallpaper surface with JPEG loading, desktop icon grid with inotify file watching, drag-and-drop snap-to-grid, Cairo-rendered context menus.
- **cc-spotlight** — Ctrl+Space search overlay. Scans .desktop files, real-time substring search, icon caching, keyboard navigation.
- **breeze-aqua** — Qt6 QStyle plugin that renders Aqua scrollbars, buttons, checkboxes, and other widgets inside Qt applications using real Snow Leopard PNG assets.

## Architecture

- **Language:** C (shell components), C++ (Qt style plugin only)
- **Display server:** X11 via XLibre
- **Rendering:** Cairo + Pango (2D), OpenGL via GLX (compositor)
- **Target:** Nobara Linux (Fedora-based) on Lenovo Legion Go
- **No KDE/Plasma dependencies** in the shell. Qt is used only inside applications via breeze-aqua.

## Building

```bash
# Install dependencies (Fedora/Nobara)
sudo dnf install gcc meson cmake qt6-qtbase-devel \
  cairo-devel pango-devel libpng-devel libjpeg-turbo-devel \
  libX11-devel libXcomposite-devel libXdamage-devel libXfixes-devel \
  libXext-devel libXrender-devel libXrandr-devel libXcursor-devel \
  mesa-libGL-devel extra-cmake-modules

# Build all components
bash scripts/build-all.sh

# Install assets
bash scripts/install-assets.sh
```

## Running

Select "CopyCatOS" at the display manager login screen, or:

```bash
DISPLAY=:1 startx /usr/local/bin/cc-session.sh -- :1
```

## Why

I grew up with Snow Leopard. It was the last version of macOS where every pixel served a purpose and nothing was decorative for its own sake. The gloss on a button said "I'm raised and pressable." The shadow under a window said "I'm floating." The blue in a scrollbar said "I'm interactive."

I wanted to see if I could build that experience on Linux. Not approximate it — recreate it. Using the real assets, the real measurements, the real Human Interface Guidelines. Comparing against a real Snow Leopard machine pixel by pixel.

This is what I built.

## License

Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
See [LICENSE](LICENSE) for details.
