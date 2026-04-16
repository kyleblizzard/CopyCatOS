# CopyCatOS Build Plan

## Step 0: Cleanup + Asset Prep
- [x] Backup project to zipBackups
- [x] Write unified CLAUDE.md
- [x] Write this todo.md
- [x] Delete old Python prototypes
- [x] Delete KDE theme dirs (aurorae, plasma, sddm, color-schemes)
- [x] Delete KDE plugins (kdecoration, aura-decoration)
- [x] Delete old installers (install.sh, install-snowleopard.sh)
- [x] Delete old planning docs (nextStepsForAura.txt, phase2-architecture.md)
- [x] Delete empty placeholders (AuraBar, AuraBarMacOS, AuraFarm, AuraFarmer)
- [x] Delete snowleopard-guard.sh, __pycache__
- [x] Clean up memory files
- [x] Create scaffold dirs (cc-dock, cc-menubar, cc-desktop, cc-spotlight, scripts, tasks)
- [x] Write scripts/install-assets.sh
- [x] Write scripts/build-all.sh
- [x] Update cc-session.sh

## Step 1: cc-wm Compositing (Shadows)
- [x] Create compositor.c / compositor.h (legacy — replaced by MoonRock)
- [x] Find/create 32-bit ARGB visual
- [x] Redirect windows to off-screen pixmaps via XComposite
- [x] Composite with alpha-blended shadows via XRender
- [x] Shadow: 20px blur, 3px Y-offset, RGBA(0,0,0,0.4), Gaussian falloff
- [x] XDamage tracking for efficient repainting
- [x] Frame windows use ARGB visual for transparent shadow borders
- [x] Frame geometry grows by shadow extent (20px L/R/T, 23px B)
- [x] Update _NET_FRAME_EXTENTS for shadow padding
- [x] XFixes input shape so clicks pass through shadows
- [x] Skip shadows for DOCK and DESKTOP window types
- [x] Wire compositor into main.c, events.c, decor.c, frame.c
- [x] MoonRock Compositor (moonrock.c/moonrock.h) — OpenGL replacement for compositor.c + picom
- [x] Wire MoonRock into WM: main.c, events.c, decor.c, frame.c, ewmh.c
- [x] Remove compositor.c from meson.build (kept as compositor_legacy backup)
- [x] Comment out picom from cc-session.sh
- [x] Inline ARGB visual fallback in moonrock.c (removed compositor.c dependency)
- [ ] Gate: windows have visible drop shadows, stronger below, no artifacts on move

## Step 2: cc-wm Strut Handling
- [x] Create struts.c / struts.h
- [x] Parse _NET_WM_STRUT_PARTIAL (12 longs) on dock windows
- [x] Parse _NET_WM_STRUT (4 longs) as fallback
- [x] Compute work area: root_geometry - max(all struts per edge)
- [x] Set _NET_WORKAREA on root window
- [x] Recalculate on dock/menubar map/unmap
- [x] Clamp new window placement to work area
- [x] Add _NET_WORKAREA and _NET_DESKTOP_GEOMETRY to supported atoms
- [x] Wire struts into main.c, events.c, frame.c
- [ ] Gate: xprop -root _NET_WORKAREA shows correct values with struts

## Step 3: cc-wm Resize Handles
- [x] Create resize.c / resize.h
- [x] 5px edge grab zones, 10x10px corner zones
- [x] Detect resize direction from click position
- [x] Set cursor shape per zone (Xcursor)
- [x] XMoveResizeWindow on frame + client during drag
- [x] Minimum window size: 200x100
- [x] Paint resize handle (Cairo-drawn diagonal ridges) in bottom-right
- [x] Wire resize into events.c (button press, motion, release)
- [ ] Gate: resize from all edges/corners, correct cursors, min size enforced

## Step 4: cc-desktop (Wallpaper + Desktop Icons)
- [x] Create meson.build
- [x] desktop.c: full-screen _NET_WM_WINDOW_TYPE_DESKTOP window
- [x] wallpaper.c: load and scale Aurora.jpg, set as background pixmap
- [x] icons.c: scan ~/Desktop with opendir, grid layout from top-right
- [x] Grid: 90x90 cells, 64x64 icons, Lucida Grande 11pt labels
- [x] White text with 1px black drop shadow
- [x] Selection: rounded rect #3875D7 alpha 160
- [x] inotify watch on ~/Desktop, 200ms debounce
- [x] Double-click: fork + exec xdg-open
- [x] dnd.c: drag icons, snap to grid (integrated into icons.c)
- [x] contextmenu.c: right-click menu (New Folder, Sort By, Clean Up, Open Terminal)
- [x] Cairo-rendered popup menus (override_redirect)
- [ ] Gate: wallpaper fills screen, icons appear, double-click opens, context menu works

## Step 5: cc-menubar (Global Menu Bar)
- [x] Create meson.build
- [x] menubar.c: full-width x 22px dock-type window at y=0
- [x] Set _NET_WM_STRUT_PARTIAL for top 22px
- [x] render.c: gradient #F2F2F2 -> #D2D2D2, 1px bottom border #ABABAB
- [x] Tile menubar_bg.png from SnowReverseOutput if available
- [ ] ARGB visual for wallpaper bleed-through translucency
- [x] apple.c: load HiResAppleMenu.png, 14x16px, left side
- [x] Apple menu dropdown: About, System Preferences, Sleep, Restart, Shut Down, Log Out
- [x] appmenu.c: track _NET_ACTIVE_WINDOW every 250ms
- [x] Map WM_CLASS to display name, show bold app name + menus
- [x] Hardcoded menu sets per app initially
- [x] systray.c: clock (update every second), battery (/sys/class/power_supply), volume (pactl)
- [x] Menu dropdowns: override_redirect popups
- [x] Menu item rendering: Lucida Grande 13pt, selection gradient, separators
- [ ] Gate: bar at top, Apple logo, clock, battery, app name updates on window switch

## Step 6: cc-dock (Dock)
- [x] Create meson.build
- [x] dock.c: centered bottom window, ARGB visual, dock-type
- [x] Set _NET_WM_STRUT_PARTIAL for bottom 42px
- [x] shelf.c: render scurve-xl.png with trapezoid clip (1.4% inset per side)
- [x] 65% shelf opacity, frontline.png top highlight
- [x] Separators: 1px white + 1px black between app/doc sections
- [x] magnify.c: cosine falloff, BASE=54 -> MAX=82, 3-neighbor range
- [x] Icon Y position: bottom-aligned on shelf surface
- [x] bounce.c: two-phase sine (0.72s cycle, 26px amplitude, 10s timeout, 60fps)
- [x] reflect.c: vertically flipped icon copy, 35% opacity, 40% height fade
- [x] indicator.c: load indicator_medium.png, center below running apps
- [x] launch.c: gio launch, process detection via ps, _NET_ACTIVE_WINDOW activation
- [x] Window detection via _NET_CLIENT_LIST + WM_CLASS matching
- [x] Running check every 3 seconds
- [x] menu.c: right-click context (Show In Finder, Quit)
- [x] Icon resolution: AquaKDE-icons theme, hicolor, pixmaps
- [ ] Gate: shelf renders, icons magnify, click launches with bounce, indicators show

## Step 7: cc-spotlight (Search Overlay)
- [x] Create meson.build
- [ ] Modify cc-wm input.c: grab Ctrl+Space, send _CC_SPOTLIGHT_TOGGLE ClientMessage
- [x] spotlight.c: 680px wide overlay, 22% from top, ARGB, override_redirect
- [x] Grabs Ctrl+Space on root directly (with NumLock/CapsLock variants)
- [x] Grab keyboard focus when visible
- [x] search.c: scan .desktop files from /usr/share/applications + flatpak + local
- [x] Minimal INI parser for desktop entries
- [x] Substring search with name-starts-with priority
- [x] Max 8 visible results
- [x] render.c: rounded rect 12px radius, RGBA(232,232,232,225/255)
- [x] 24px drop shadow, search field 48px, result rows 44px
- [x] Selected: RGBA(56,117,246,220/255) with white text
- [x] icons.c: 32px icons from theme, cached in djb2 hash map
- [x] Keyboard: Up/Down navigate, Enter launches, Escape dismisses
- [x] Launch: strip field codes, fork + setsid + exec
- [ ] Gate: Ctrl+Space shows overlay, typing filters apps, Enter launches, Escape hides

## Step 8: Session Integration
- [x] Update cc-session.sh: start all components in correct order
- [x] cc-wm first, sleep 0.5s
- [x] cc-desktop, sleep 0.2s
- [x] cc-menubar, cc-dock, cc-spotlight (parallel)
- [x] Clean shutdown on WM exit
- [x] Create aura.desktop for /usr/share/xsessions/
- [x] Set QT_STYLE_OVERRIDE=AquaStyle in session env
- [x] Set icon theme to AquaKDE-icons
- [ ] Gate: log in via DM, select CopyCatOS, all components start

## Step 9: AppImage Packaging (interim — Qt app styling shim)
- [x] Write scripts/build-appimage.sh template
- [x] Bundle breeze-aqua .so at plugins/styles/breezeaqua6.so
- [x] Bundle LucidaGrande.ttc at fonts/
- [x] Bundle aqua widget PNGs at share/aqua-scrollbar/ + share/aqua-controls/
- [x] AppRun sets QT_PLUGIN_PATH, QT_STYLE_OVERRIDE, FONTCONFIG_PATH
- [ ] Add fallback path in breezeaquaassets.h: check $APPDIR first
- [ ] Gate: build test AppImage (Kate), Qt widgets render Aqua

## Step 10: Polish
- [x] Traffic light hover states: show glyphs (x, -, +) on hover
- [x] Traffic light pressed state: darken button on click, action fires on release
- [ ] Inactive window buttons: colorize on hover
- [x] Smart zoom button: toggle between saved size and maximized
- [x] App grouping: WM_CLASS-based, Super+H hides all app windows
- [x] Menu bar translucency (ARGB visual already active, real menubar_bg.png tiled)
- [x] Menu bar fallback gradient matches SL color values (#F2F2F2 -> #D2D2D2 + #A8A8A8 border)
- [x] Unsaved changes dot in close button (detected from title prefix * or •)
- [ ] Genie minimize animation (compositor captures pixmap, distorts into dock)

## Step 11: Input Daemon (cc-inputd) — Legion Go Controller Support
- [x] cc-inputd skeleton: main.c, meson.build, systemd service/udev rules
- [x] Device discovery: libudev, Legion Go identification, EVIOCGRAB
- [x] Virtual devices: uinput mouse, keyboard, gamepad
- [x] Mapper engine: button→action mapping with desktop/game/login profiles
- [x] Right stick mouse: deadzone, expo curve, 120Hz timerfd
- [x] Power button: state machine (short=suspend, hold=restart)
- [x] Config parser: input.conf INI format, SIGHUP reload
- [x] IPC: Unix socket server for session bridge
- [x] cc-input-session: X11 window tracking, action dispatch, auto game mode
- [x] System Preferences: Controller pane (sensitivity/deadzone sliders)
- [x] System Preferences: Energy Saver pane (power button thresholds)
- [x] Build system: build-all.sh updated, cc-session.sh updated
- [ ] Gate: deploy to Legion Go, verify controller works at login + desktop
- [ ] Gate: game mode passthrough works with Steam
- [ ] Gate: power button suspend/restart works correctly

## Step 12: cc-finder (Spatial File Manager)
- [ ] Create meson.build
- [ ] finder.c: main window, ARGB visual, toolbar (Back/Forward/View buttons)
- [ ] sidebar.c: source list (Devices, Places, Favorites) — Snow Leopard gradient style
- [ ] iconview.c: spatial icon grid with label editing, color label rects
- [ ] listview.c: column headers (Name, Date Modified, Size, Kind), alternating row stripes
- [ ] columnview.c: Miller columns (Snow Leopard's default view)
- [ ] pathbar.c: breadcrumb path bar in toolbar area
- [ ] preview.c: Quick Look-style preview on spacebar
- [ ] spatial.c: per-folder window position/size/view-mode memory (xattrs or sidecar DB)
- [ ] navigation.c: back/forward history stack, double-click descent, up-arrow to parent
- [ ] clipboard.c: cut/copy/paste files (X selections + filesystem ops)
- [ ] trash.c: move to ~/.local/share/Trash/files/, write .trashinfo, restore support
- [ ] Bundle awareness: render .capp directories as single icons using Contents/Resources/ icon
- [ ] Bundle launch: double-click .capp → exec Contents/MacOS/<binary>
- [ ] Drag-and-drop: XDND v5 (reuse dnd.c patterns from cc-desktop)
- [ ] Context menus: Open, Get Info, Label, Move to Trash, New Folder, etc.
- [ ] Gate: browse filesystem spatially, open/copy/move/trash files, bundles render as apps

## Step 13: .capp App Bundles (native app format)
- [ ] Define bundle spec: MyApp.capp/Contents/{MacOS/,Resources/,Frameworks/,Info.capp}
- [ ] Info.capp format: JSON or key-value (name, version, icon, MIME types, identifier)
- [ ] Bundle icon resolution: Contents/Resources/app.icns → .png fallback chain
- [ ] cc-finder integration: .capp dirs render as single app icons, double-click launches
- [ ] Migrate breeze-aqua bundling from AppImage to Contents/Frameworks/ in .capp
- [ ] scripts/make-bundle.sh: wrap a Qt app into .capp (binary + breeze-aqua + fonts + assets)
- [ ] .desktop file generation: auto-create from Info.capp for freedesktop compatibility
- [ ] Register .capp MIME type so xdg-open routes to cc-finder's launcher
- [ ] Gate: build Kate.capp bundle, double-click in cc-finder launches with Aqua style

## Step 14: .cdmg Disk Images (drag-to-install distribution)
- [ ] Define format: squashfs image with .cdmg extension, loopback-mounted read-only
- [ ] Mount tool: cc-dmg-mount (C, losetup + squashfuse, auto-mount on double-click)
- [ ] cc-finder integration: mounted .cdmg shows as volume in sidebar under Devices
- [ ] Background image: .cdmg can embed a background PNG for the mount window
- [ ] Install UX: drag .capp bundle from mounted .cdmg to /Applications folder
- [ ] Eject: unmount on sidebar eject click or right-click → Eject
- [ ] scripts/make-dmg.sh: create .cdmg from a .capp bundle + background image
- [ ] Gate: double-click .cdmg, see app + background in cc-finder, drag to install, eject
