# CopyCatOS

A complete desktop environment that recreates Mac OS X Snow Leopard on Linux. Built from scratch in C — custom window manager, compositor, dock, menu bar, desktop, and search overlay. Every pixel measured against a real Snow Leopard machine.

## Components

- **moonrock** — X11 reparenting window manager with MoonRock Compositor (OpenGL). Handles window framing, decoration painting, shadows, compositing, keyboard shortcuts, and strut reservation.
- **dock** — Glass shelf dock with parabolic magnification, two-phase bounce animation, icon reflections, running indicators, drag-and-drop with poof removal, folder stacks with grid popup, and config persistence.
- **menubar** — 22px translucent global menu bar with Apple logo, per-app menu sets, system tray (clock, battery, volume), and Spotlight icon. Wallpaper bleeds through the gradient.
- **desktop** — Wallpaper surface with JPEG loading, desktop icon grid with inotify file watching, drag-and-drop snap-to-grid, and Cairo-rendered context menus.
- **searchsystem** — Ctrl+Space search overlay. Scans .desktop files, real-time substring search, icon caching, keyboard navigation.
- **breeze-aqua** — Qt6 QStyle plugin rendering Aqua scrollbars, buttons, checkboxes, and widgets inside Qt applications using real Snow Leopard PNG assets.

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
DISPLAY=:1 startx /usr/local/bin/moonrock-session.sh -- :1
```

## License

Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
See [LICENSE](LICENSE) for details.
