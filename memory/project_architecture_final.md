---
name: AuraOS architecture — final decision
description: Full custom DE in C on XLibre. Custom WM (aura-wm) + custom shell (dock, menubar, desktop, spotlight) + AquaStyle for Qt widgets. No KDE.
type: project
---

Architecture locked as of 2026-04-13:

- **Window manager:** aura-wm (C + Xlib + Cairo + Pango). Reparenting compositing WM with EWMH.
- **Shell:** Four standalone C programs — aura-dock, aura-menubar, aura-desktop, aura-spotlight.
- **Qt styling:** breeze-aqua (C++ Qt6 QStyle plugin) renders Aqua widgets inside Qt apps.
- **Display:** XLibre (X11). No Wayland for now.
- **Base OS:** Nobara Steam-Handheld (Fedora) on Lenovo Legion Go.
- **Apps:** AppImage-only. Each bundles AquaStyle + fonts + assets.
- **Icons:** AquaKDE-icons (2,157 real Snow Leopard PNGs, freedesktop formatted).
- **Assets:** snowleopardaura/ (511MB from real Snow Leopard machine).

**Why custom WM over kwin:** Total control over window chrome, shadows, animations. No dependency on KDE Frameworks for the shell. KDecoration2 was replaced by direct Cairo rendering in aura-wm.

**Why:** This is the "design system -> custom shell -> custom WM -> pixels" approach. Every pixel is ours.

**How to apply:** Never suggest adding KDE/Plasma dependencies to shell components. Qt is only for app internals via breeze-aqua.
