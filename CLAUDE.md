# CopyCatOS — Pixel-Perfect Snow Leopard Desktop Environment

## What This Is

A custom Linux desktop environment that exactly recreates Mac OS X 10.6 Snow Leopard.
Not a theme. Not an approximation. A closed platform where every pixel matches the real thing.

---

## Architecture (Locked)

- **Base OS:** Nobara Steam-Handheld (Fedora-based) on Lenovo Legion Go
- **Display server:** XLibre (actively maintained X11 fork)
- **Window manager:** `cc-wm` — custom C + Xlib + Cairo + Pango reparenting compositing WM
- **Shell components:** All standalone C + Cairo + Pango + Xlib programs
  - `cc-dock` — glass shelf dock with magnification, bounce, reflections
  - `cc-menubar` — 22px translucent global menu bar
  - `cc-desktop` — wallpaper surface + desktop icon grid
  - `cc-spotlight` — Ctrl+Space search overlay
- **Qt widget styling:** `breeze-aqua` / AquaStyle — C++ Qt6 QStyle plugin (~8,900 lines)
- **App distribution:** AppImage-only (each bundles AquaStyle + fonts + assets)
- **Icon theme:** `icons/AquaKDE-icons/` — 2,157 real Snow Leopard icons, freedesktop formatted
- **Session:** `cc-wm/session/cc-session.sh` starts: cc-wm -> cc-desktop -> cc-menubar -> cc-dock -> cc-spotlight

---

## Build Commands

```bash
# Window manager
cd cc-wm && meson setup build && meson compile -C build

# Shell components (same pattern for each)
cd cc-dock && meson setup build && meson compile -C build
cd cc-menubar && meson setup build && meson compile -C build
cd cc-desktop && meson setup build && meson compile -C build
cd cc-spotlight && meson setup build && meson compile -C build

# Qt6 style plugin (breeze-aqua)
cd breeze-aqua && mkdir -p build && cd build && cmake .. && make

# Install assets to runtime location
bash scripts/install-assets.sh
```

---

## Key Rules

1. **Pixel-perfect fidelity.** Use real Snow Leopard assets from `snowleopardaura/`. No approximations. No "close enough."
2. **C for shell, C++ for Qt plugin only.** All shell components (WM, dock, menubar, desktop, spotlight) are pure C + Cairo + Pango + Xlib. Only `breeze-aqua` uses C++ (it's a Qt6 plugin).
3. **No KDE/Plasma dependencies in the shell.** This is a standalone DE. No kwin, no plasmashell, no KF6 frameworks in shell components.
4. **Assets live in `~/.local/share/aqua-widgets/`.** Run `scripts/install-assets.sh` to deploy from `snowleopardaura/`.
5. **Lucida Grande is the only UI font.** Installed to `~/.local/share/fonts/`.
6. **All color values come from real screenshots.** See `snowleopardaura/aquadesignthoughts.txt`.
7. **Every animation serves spatial communication.** No decorative animations. Where did it go? Where did it come from?

---

## Asset Locations

| What | Where |
|------|-------|
| Source assets (511MB, don't modify) | `snowleopardaura/MacAssets/` |
| HITheme-rendered controls (440 PNGs) | `snowleopardaura/MacAssets/SnowReverseOutput/` |
| Dock assets (shelf, indicators) | `snowleopardaura/MacAssets/Dock/` |
| Menu bar assets (Apple logo, bg) | `snowleopardaura/MacAssets/HIToolbox/` + `SnowReverseOutput/menubar/` |
| System icons (.icns) | `snowleopardaura/MacAssets/Icons/` |
| CoreUI materials (gradients, textures) | `snowleopardaura/MacAssets/CoreUI/UI.bundle/Contents/Resources/` |
| Menu extras (Battery, Volume, etc.) | `snowleopardaura/MacAssets/MenuExtras/` |
| Wallpapers (99 originals) | `snowleopardaura/MacAssets/Wallpapers/` |
| Reference screenshots | `snowleopardaura/example photos/` |
| Design bible (568 lines) | `snowleopardaura/aquadesignthoughts.txt` |
| Asset guide with recipes | `snowleopardaura/MacAssets/SNOW_LEOPARD_GUIDE.md` |
| Apple HIG PDF (35MB) | `snowleopardaura/MacHumanInterfaceGuidelines.pdf` |
| Production icon theme (2,157 PNGs) | `icons/AquaKDE-icons/` |
| Raw icon source (357 PNGs, 512x512) | `snowleopardaura/snow leopard icon pngs/` |
| Runtime assets (installed) | `~/.local/share/aqua-widgets/` |
| Font | `fonts/LucidaGrande.ttc` |

---

## Critical Color Values (from real Snow Leopard screenshots)

```
Active title bar gradient:
  y=0:  RGB(76, 125, 176)   blue-gray top
  y=9:  RGB(73, 118, 167)   slightly darker
  y=10: RGB(62, 101, 143)   dark blue divider
  y=11: RGB(226, 226, 226)  jump to light gray
  y=19: RGB(208, 208, 208)  mid gray
  y=21: RGB(202, 202, 202)  bottom

Inactive title bar: RGB(236,236,236) -> RGB(218,218,218)
Title bar bottom border: active RGB(140,140,140), inactive RGB(185,185,185)

Traffic lights: Close #FF5F57, Minimize #FFBD2E, Zoom #28C940
Inactive buttons: #B0B0B0

Selection blue: #3875D7
Focus ring: #3B99FC
Window background: #ECECEC
Content area: #FFFFFF
Sidebar: #D8DEE8
Menu bar gradient: #F2F2F2 -> #E8E8E8 -> #D7D7D7 -> #D2D2D2
Menu bar bottom border: #A8A8A8

Window shadow: RGBA(0,0,0,0.4), 20px blur, 3px Y-offset
Window border: active RGB(160,160,160), inactive RGB(190,190,190)

Text primary: #1A1A1A
Text secondary: #737373
Text disabled: #A6A6A6
Text on selection: #FFFFFF
```

---

## File Layout

```
CopyCatOS/
├── CLAUDE.md                    # This file
├── cc-wm/                     # Window manager (C, ~1,400 lines, EXISTING)
│   ├── meson.build
│   ├── src/                     # 9 source files + new compositor/struts/resize
│   └── session/                 # cc-session.sh, aura.desktop
├── cc-dock/src/               # Dock (C, BUILT)
├── cc-menubar/src/            # Menu bar (C, BUILT)
├── cc-desktop/src/            # Desktop + icons + XDND + labels (C, BUILT)
├── cc-spotlight/src/          # Search overlay (C, BUILT)
├── cc-finder/src/             # Spatial file manager (C, PLANNED)
├── cc-inputd/src/             # Legion Go controller daemon (C, BUILT)
├── breeze-aqua/                 # Qt6 QStyle plugin (C++, 8,900 lines, EXISTING)
├── snowleopardaura/             # Asset library (511MB, DO NOT MODIFY)
├── icons/AquaKDE-icons/         # Production icon theme (2,157 PNGs, EXISTING)
├── fonts/                       # LucidaGrande.ttc
├── wallpaper/                   # 4K wallpaper JPGs
├── scripts/                     # install-assets.sh, build-all.sh
├── tasks/                       # todo.md, lessons.md
└── memory/                      # Claude project memory
```

---

## Build Sequence

See `tasks/todo.md` for the full checkable plan. Summary:

0. **Cleanup + asset prep** — delete old code, install assets
1. **cc-wm compositing** — XComposite shadows (depth stack)
2. **cc-wm struts** — reserve space for dock/menubar
3. **cc-wm resize** — edge/corner resize handles
4. **cc-desktop** — wallpaper + desktop icon grid + XDND + color labels
5. **cc-menubar** — 22px global bar with Apple logo, clock, app menus
6. **cc-dock** — glass shelf, magnification, bounce, reflections, indicators
7. **cc-spotlight** — Ctrl+Space search overlay
8. **Session integration** — all components start together
9. **AppImage packaging** — interim Qt app styling shim (bundle AquaStyle)
10. **Polish** — genie minimize, smart zoom, hover states, translucency
11. **cc-inputd** — Legion Go controller daemon + System Preferences panes
12. **cc-finder** — spatial file manager (icon/list/column views, Trash, bundle awareness)
13. **.capp bundles** — native app format (directory-based, replaces AppImage long-term)
14. **.cdmg disk images** — squashfs drag-to-install distribution format
